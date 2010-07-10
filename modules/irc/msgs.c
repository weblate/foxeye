/*
 * Copyright (C) 2005-2008  Andrej N. Gritsenko <andrej@rep.kiev.ua>
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * The FoxEye "irc" module: do queue private msgs for my IRC client connection
 */

#include "foxeye.h"
#include "modules.h"
#include "irc.h"
#include "init.h"
#include "list.h"
#include "sheduler.h"

typedef struct pmsgout_stack {
  INTERFACE *client;
  struct pmsgout_stack *prev;
  struct pmsgout_stack *next;
  time_t last;
  short msg_flood;
  short ctcp_flood;
  int run:1;
} pmsgout_stack;


static bindtable_t *BT_PubMsgMask;
static bindtable_t *BT_PubNoticeMask;
static bindtable_t *BT_PubMsgCmd;
static bindtable_t *BT_PubNoticeCmd;
static bindtable_t *BT_PubCtcp;
static bindtable_t *BT_PubCtcr;
static bindtable_t *BT_PrivMsgMask;
static bindtable_t *BT_PrivNoticeMask;
static bindtable_t *BT_PrivMsgCmd;
static bindtable_t *BT_PrivNoticeCmd;
static bindtable_t *BT_PrivCtcp;
static bindtable_t *BT_PrivCtcr;
static bindtable_t *BT_Flood;

static char *format_irc_message;
static char *format_irc_notice;
static char *format_irc_ctcp;
static char *format_irc_ctcr;
static char *format_irc_action;
static char *format_irc_cmdmessage;
static char *format_irc_cmdctcp;

static short *FloodMsg;
static short *FloodCtcp;


struct pmsgin_actions
{
  bindtable_t **priv_bt;
  bindtable_t **pub_bt;
  size_t shift;
  char **format;
  char **cmdformat;
};

static bindtable_t *BT_NULL = NULL;

static struct pmsgin_actions PmsginTable[] = {
  { &BT_PrivMsgCmd,	&BT_PubMsgCmd,		0, &format_irc_message,	&format_irc_cmdmessage },
  { &BT_PrivNoticeCmd,	&BT_PubNoticeCmd,	0, &format_irc_notice,	NULL },
  { &BT_PrivCtcp,	&BT_PubCtcp,		1, &format_irc_ctcp,	&format_irc_cmdctcp },
  { &BT_PrivCtcr,	&BT_PubCtcr,		1, &format_irc_ctcr,	NULL },
  { &BT_NULL,		&BT_NULL,		8, &format_irc_action,	NULL }
};


/* --- me => server part ---------------------------------------------------- */

const char *_pmsgout_send_formats[] = {
  "PRIVMSG %s :%s",
  "NOTICE %s :%s",
  "PRIVMSG %s :\001%s\001",
  "NOTICE %s :\001%s\001",
  "PRIVMSG %s :\001ACTION %s\001"
};

/* send one message from stack */
static void _pmsgout_send (char *to, char *msg, flag_t flag, char *dog)
{
  register unsigned int i;
  unsigned char *c;

  if (dog && msg[0])			/* don't send empty messages */
  {
    *dog = 0;
    i = (flag & F_T_MASK);
    if (i > 4)
      i = 0;				/* default */
    Add_Request (I_SERVICE, &dog[1], 0, _pmsgout_send_formats[i], to, msg);
    /* parse all recipients */
    for (c = to; to; to = c)
    {
      int pub;
      char tocl[IFNAMEMAX+1];
      char logmsg[MESSAGEMAX+1];

      if ((*c >= 'A' && *c < '~') || *c >= 160)
	pub = 0;
      else
	pub = 1;
      c = strchr (to, ',');
      if (c)
	*c = 0;
      /* TODO: convert recipient to lowercase, it might be #ChaNNel */
      /* %N - mynick, %# - target, %* - message */
      if (*to == '!')		/* special support for "!XXXXXchannel" */
	snprintf (tocl, sizeof(tocl), "!%s@%s", &to[6], &dog[1]);
      else
	snprintf (tocl, sizeof(tocl), "%s@%s", to, &dog[1]);
      printl (logmsg, sizeof(logmsg), *PmsginTable[i].format, 0,
	      irc_mynick (&dog[1]), NULL, NULL, to, 0, 0, 0, msg);
      if (*logmsg)
	Add_Request (I_LOG, tocl, (pub ? F_PUBLIC : F_PRIV) | F_MINE | i,
		     "%s", logmsg);
      if (c)
	*c++ = ',';
    }
    *dog = '@';
  }
}

static int _pmsgout_run (INTERFACE *client, REQUEST *req)
{
  if (!req)
    return REQ_OK;
  if (!((pmsgout_stack *)client->data)->run)
  {
    return REQ_REJECTED;
  }
  _pmsgout_send (client->name, req->string, req->flag,
		 strrchr (client->name, '@'));	/* get server name from a@b */
  ((pmsgout_stack *)client->data)->run = 0;
  return REQ_OK;
}

ALLOCATABLE_TYPE (pmsgout_stack, _PMS, next) /* (alloc|free)_pmsgout_stack() */

static INTERFACE *_pmsgout_stack_insert (pmsgout_stack **stack, char *to)
{
  INTERFACE *client;
  pmsgout_stack *cur = alloc_pmsgout_stack();

  dprint (4, "_pmsgout_stack_insert: adding %s", to);
  client = Add_Iface (I_CLIENT, to, NULL, &_pmsgout_run, NULL);
  if (*stack)
  {
    cur->next = *stack;
    cur->prev = (*stack)->prev;
    (*stack)->prev = cur;
    cur->prev->next = cur;
  }
  else
    *stack = cur->prev = cur->next = cur;
  cur->client = client;
  client->data = cur;
  cur->msg_flood = cur->ctcp_flood = 0;
  cur->run = 0;
  return client;
}

static void _pmsgout_stack_remove (pmsgout_stack **stack, pmsgout_stack *cur)
{
  dprint (4, "_pmsgout_stack_remove: removing %s", cur->client->name);
  if (cur->prev == cur)
    *stack = NULL;
  else if (*stack == cur)
    *stack = cur->next;
  cur->next->prev = cur->prev;
  cur->prev->next = cur->next;
  NoCheckFlood (&cur->msg_flood);
  NoCheckFlood (&cur->ctcp_flood);
  free_pmsgout_stack (cur);
  cur->client->ift |= I_DIED;		/* it will free cur too so preserve it */
  cur->client->data = NULL;
}

/* ATTENTION: on returned INTERFACE must be called Unset_Iface() after use! */
static INTERFACE *_pmsgout_get_client (char *net, char *to)
{
  char lcto[IFNAMEMAX+1];

  strfcpy (lcto, to, sizeof(lcto));
  strfcat (lcto, net, sizeof(lcto));
  dprint (4, "_pmsgout_get_client: search %s", lcto);
  return Find_Iface (I_CLIENT, lcto); 
}

/* send message from head of stack and revolve stack */
void irc_privmsgout (INTERFACE *pmsgout, int pmsg_keep)
{
  register pmsgout_stack *stack = pmsgout->data, *cur;

  if (!stack)
    return;
  cur = stack->next;
  while (cur != stack && cur->client->qsize == 0)
  {
    if (Time > cur->last)
    {
      if (Find_Iface (I_QUERY, cur->client->name)) /* keep it while query exists */
	Unset_Iface();
      else
      {
	cur = cur->prev;
	_pmsgout_stack_remove ((pmsgout_stack **)&pmsgout->data, cur->next);
      }
    }
    cur = cur->next;
  }
  if (cur->client->qsize == 0)
    return;
  pmsgout->data = cur;
  cur->last = Time + pmsg_keep;
  cur->run = 1;
}

int irc_privmsgout_default (INTERFACE *pmsgout, REQUEST *req)
{
  INTERFACE *client;
  char *dog;

  if (!req)
    return REQ_OK;
  /* may we send it to list or some mask? just forward it to server then */
  dog = strrchr (req->to, '@');			/* get server name from a@b */
  if (strchr (req->to, ',') || strchr (req->to, '%') ||
      strchr (req->to, '@') != dog ||
      (req->to[0] < 0x41 && strchr (CHANNFIRSTCHAR, req->to[0])) ||
      (req->to[0] > 0x7d && req->to[0] < 0xa0))
  {
    _pmsgout_send (req->to, req->string, req->flag, dog);
    return REQ_OK;
  }
  /* it's to one so let's create client queue interface */
  client = _pmsgout_stack_insert ((pmsgout_stack **)&pmsgout->data, req->to);
  return Relay_Request (I_CLIENT, client->name, req);
}

/* remove client from stack or destroy all stack if NULL */
void irc_privmsgout_cancel (INTERFACE *pmsgout, char *to)
{
  INTERFACE *iface;

  dprint (4, "_privmsgout_cancel: cancel %s%s", to ? to : "*", pmsgout->name);
  if (pmsgout->data && !to)
  {
    while (pmsgout->data)
      _pmsgout_stack_remove ((pmsgout_stack **)&pmsgout->data,
			     ((pmsgout_stack *)pmsgout->data)->prev);
    return;
  }
  if (!pmsgout->data || !(iface = _pmsgout_get_client (pmsgout->name, to)))
    return;
  _pmsgout_stack_remove ((pmsgout_stack **)&pmsgout->data, iface->data);
  Unset_Iface();
}

int irc_privmsgout_count (INTERFACE *pmsgout)
{
  register int i;
  register pmsgout_stack *stack = pmsgout->data;
  if (!stack)
    return 0;
  for (i = 1; stack->next != pmsgout->data; stack = stack->next)
    i++;
  return i;
}


/* --- server => me part ---------------------------------------------------- */

/*
 * do PRIVMSG and NOTICE parsing
 * to is target of the message and is NULL if message is for me
 * message may be modified free (see irc.c)
 * msg_type has bits:
 *   xx1  notice
 *   00x  message
 *   01x  ctcp
 *   100  action
 */
int irc_privmsgin (INTERFACE *pmsgout, char *from, char *to,
		   char *msg, int notice, int allow_cmdpmsg, int pmsg_keep,
		   size_t (*lc) (char *, const char *, size_t))
{
  char lcnick[HOSTMASKLEN+1];
  char tocl[IFNAMEMAX+1];
  int msg_type;
  char *lname, *ft, *ae; /* ae - at exclamation */
  size_t msglen = 0;			/* avoiding compiler warning */
  INTERFACE *client;
  clrec_t *clr;
  userflag uf;
  binding_t *bind;
  bindtable_t *btmask;
  int i;

  /* convert sender nick to lower case before searching for client */
  if ((ae = strchr (from, '!')))
    *ae = 0;
  if (lc)
    lc (lcnick, from, MBNAMEMAX+1);
  else
    strfcpy (lcnick, from, MBNAMEMAX+1);
  /* let's check if it's identified user at first */
  if (Inspect_Client (&pmsgout->name[1], from, (const char **)&lname, NULL,
		      NULL, NULL) & A_REGISTERED)
  {
    if ((clr = Lock_Clientrecord (lname)))	/* might it be deleted yet? */
      uf = Get_Flags (clr, &pmsgout->name[1]);
    else
      uf = 0;
  }
  else
  {
    if (ae)
    {
      *ae = '!';
      msglen = strlen (lcnick);
      unistrlower (&lcnick[msglen], ae, sizeof(lcnick) - msglen);
    }
    clr = Find_Clientrecord (lcnick, &lname, &uf, &pmsgout->name[1]);
    if (ae)
      lcnick[msglen] = 0;		/* leave only lower case nick there */
  }
  if (clr)
  {
    lname = safe_strdup (lname);
    Unlock_Clientrecord (clr);
  }
  else
    lname = NULL;
  msg_type = notice ? 1 : 0;		/* check type of the message */
  msglen = safe_strlen (msg);
  if (*msg == '\001' && msg[msglen-1] == '\001')
  {
    msg_type += 2;
    if (!notice && !strncmp (&msg[1], "ACTION ", 7))	/* ignore CTCR ACTION */
      msg_type = 4;
    msglen--;
  }
  dprint (4, "irc_privmsgin: got message from %s to %s of type %d", from, to,
	  msg_type);
  if (ae)				/* find/create pmsgout for sender */
    *ae = 0;				/* pmsgout has to be nick@net */
  if ((client = _pmsgout_get_client (pmsgout->name, from)))
    Unset_Iface();
  else
  {
    strfcpy (tocl, from, sizeof(tocl));		/* use as temporary buffer */
    strfcat (tocl, pmsgout->name, sizeof(tocl));
    client = _pmsgout_stack_insert ((pmsgout_stack **)&pmsgout->data, tocl);
  }
  if (ae)
    *ae = '!';
  ((pmsgout_stack *)client->data)->last = Time + pmsg_keep;
  /* check for flood */
  bind = NULL;
  ft = (msg_type & 2) ? "ctcp" : "msg";			/* action as msg too */
  i = 0;
  while ((bind = Check_Bindtable (BT_Flood, ft, uf, -1, bind)))
  {
    if (bind->name)
      i = RunBinding (bind, client->name, lname ? lname : "*", ft, -1,
		      to ? to : "*");
    else
      i = bind->func (from, lname, ft, to);
    if (i)
      break;
  }
  if (i == 0)
  {
    if (msg_type & 2)
    {
      if (CheckFlood (&((pmsgout_stack *)pmsgout->data)->ctcp_flood, FloodCtcp) > 0)
      {
	FREE (&lname);
        return 1;
      }
    }
    else
    {
      if (CheckFlood (&((pmsgout_stack *)pmsgout->data)->msg_flood, FloodMsg) > 0)
      {
	FREE (&lname);
        return 1;
      }
    }
  }
  /* prepare mask bindtable */
  if (notice)
  {
    if (to)
      btmask = BT_PubNoticeMask;
    else
      btmask = BT_PrivNoticeMask;
  }
  else
  {
    if (to)
      btmask = BT_PubMsgMask;
    else
      btmask = BT_PrivMsgMask;
  }
  /* parse destination (*to) - i hope it is just one but really may be not */
  do
  {
    char tomask[MESSAGEMAX+1];
    char *chmask;
    INTERFACE *tmp;

    /*
     * now get where to put it! (for each of targets, of course)
     * a) private message/action: if there is a I_QUERY for target then go to (d)
     * b) check for mask bindings - run all
     * c) check for command - run (and log if 1) and go out
     * d) log unmatched to any
     */
    /* select the target */
    if (to)
    {
      ft = strchr (to, ',');
      if (ft)
	*ft++ = 0;
      if (*to == '!')		/* special support for "!XXXXXchannel" */
      {
	snprintf (tomask, sizeof(tomask), "!%s %s", &to[6], msg);
	tocl[0] = '!';
	/* TODO: use lc() for channel name too */
	unistrlower (&tocl[1], &to[6], sizeof(tocl) - 1);
      }
      else
      {
	snprintf (tomask, sizeof(tomask), "%s %s", to, msg);
	/* TODO: use lc() for channel name too */
	unistrlower (tocl, to, sizeof(tocl));
      }
      chmask = tomask;
      strfcat (tocl, pmsgout->name, sizeof(tocl));
    }
    else
      chmask = msg;
    /* check for query for target and pass casual privates */
    if (!allow_cmdpmsg && !to && (msg_type == 0 || msg_type == 4))
      tmp = Find_Iface (I_QUERY, client->name);
    else
      tmp = NULL;
    i = 0;
    if (tmp)
      Unset_Iface();
    else
    {
      userflag cf;

      if (lname && to)
	cf = Get_Clientflags (lname, tocl);
      else if (to)
	cf = 0;
      else
	cf = -1;
      /* do mask bindtable */
      for (bind = NULL; (bind = Check_Bindtable (btmask, chmask, uf, cf, bind)); )
      {
	if (bind->name)
	  RunBinding (bind, client->name, lname ? lname : "*",
		      to ? tocl : NULL, -1, msg);
	else if (to)
	  bind->func (client, from, lname, lcnick, tocl, msg);
	else
	  bind->func (client, from, lname, lcnick, msg);
      }
      if (msg_type >= 2)
	msg[msglen] = 0;
      /* check for command */
      if (msg_type == 0 && !allow_cmdpmsg)
	bind = NULL;
      else
	bind = Check_Bindtable (to ? *PmsginTable[msg_type].pub_bt :
				*PmsginTable[msg_type].priv_bt,
				&msg[PmsginTable[msg_type].shift], uf, cf, NULL);
      if (bind)
      {
	if (bind->name)
	  i = RunBinding (bind, client->name, lname ? lname : "*",
			  to ? tocl : NULL, -1,
			  NextWord(&msg[PmsginTable[msg_type].shift]));
	else if (to)
	  i = bind->func (client, from, lname, lcnick, tocl,
			  NextWord(&msg[PmsginTable[msg_type].shift]));
	else
	  i = bind->func (client, from, lname, lcnick,
			  NextWord(&msg[PmsginTable[msg_type].shift]));
      }
      if (msg_type >= 2)
	msg[msglen] = '\001';
    }
    /* log if no command or if command was succesful */
    if ((i == 0 && PmsginTable[msg_type].format != NULL) ||
	(i > 0 && PmsginTable[msg_type].cmdformat != NULL))
    {
      if (ae)
	*ae = 0;
      /* %N - nick, %@ - ident@host, %L - lname, %# - target, %* - message */
      if (msg_type >= 2)
	msg[msglen] = 0;
      if (i)
	printl (tomask, sizeof(tomask), *PmsginTable[msg_type].cmdformat, 0,
		from, ae ? &ae[1] : NULL, lname, to, 0, 0, 0,
		&msg[PmsginTable[msg_type].shift]);
      else
	printl (tomask, sizeof(tomask), *PmsginTable[msg_type].format, 0,
		from, ae ? &ae[1] : NULL, lname, to, 0, 0, 0,
		&msg[PmsginTable[msg_type].shift]);
      if (msg_type >= 2)
	msg[msglen] = '\001';
      if (*tomask)
	Add_Request (I_LOG, to ? tocl : lcnick,
		     (i ? F_CMDS : to ? F_PUBLIC : F_PRIV) | msg_type,
		     "%s", tomask);
      if (ae)
	*ae = '!';
    }
    if (to)
      to = ft;	/* next target */
  } while (to && *to);
  FREE (&lname);
  return 1;
}

/* nocheckflood for friends */
BINDING_TYPE_irc_flood (irc_ignflood);
static int irc_ignflood (unsigned char *from, char *lname, char *type, char *chan)
{
  return 1;
}


/* --- common part ---------------------------------------------------------- */

void irc_privmsgreg (void)
{
  BT_PubMsgMask = Add_Bindtable ("irc-pub-msg-mask", B_MASK);
  BT_PubNoticeMask = Add_Bindtable ("irc-pub-notice-mask", B_MASK);
  BT_PubMsgCmd = Add_Bindtable ("irc-pub-msg-cmd", B_UNIQ);
  BT_PubNoticeCmd = Add_Bindtable ("irc-pub-notice-cmd", B_UNIQ);
  BT_PubCtcp = Add_Bindtable ("irc-pub-msg-ctcp", B_MATCHCASE);
  BT_PubCtcr = Add_Bindtable ("irc-pub-notice-ctcp", B_MATCHCASE);
  BT_PrivMsgMask = Add_Bindtable ("irc-priv-msg-mask", B_MASK);
  BT_PrivNoticeMask =  Add_Bindtable ("irc-priv-notice-mask", B_MASK);
  BT_PrivMsgCmd = Add_Bindtable ("irc-priv-msg-cmd", B_UNIQ);
  BT_PrivNoticeCmd = Add_Bindtable ("irc-priv-notice-cmd", B_UNIQ);
  BT_PrivCtcp = Add_Bindtable ("irc-priv-msg-ctcp", B_MATCHCASE);
  BT_PrivCtcr = Add_Bindtable ("irc-priv-notice-ctcp", B_MATCHCASE);
  BT_Flood = Add_Bindtable ("irc-flood", B_MASK);
  Add_Binding ("irc-flood", "*", U_FRIEND, U_FRIEND, &irc_ignflood, NULL);
  FloodMsg = FloodType ("irc-msgs");	/* register flood, no defaults */
  FloodCtcp = FloodType ("irc-ctcps");
  format_irc_message = SetFormat ("irc_message", "<%N> %*");
  format_irc_notice = SetFormat ("irc_notice", "-%N- %*");
  format_irc_ctcp = SetFormat ("irc_ctcp", "%N requested CTCP %* from %?#%#?me?");
  format_irc_ctcr = SetFormat ("irc_ctcp_reply", "CTCP reply from %N: %*");
  format_irc_action = SetFormat ("irc_action", "* %N %*");
  format_irc_cmdmessage = SetFormat ("irc_message_command", "%?#<<?(?%N%?#>>?!%@)? !%L! %*");
  format_irc_cmdctcp = SetFormat ("irc_ctcp_command", "");
}

void irc_privmsgunreg (void)
{
  Delete_Binding ("irc-flood", &irc_ignflood, NULL);
}