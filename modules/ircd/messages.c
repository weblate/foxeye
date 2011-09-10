/*
 * Copyright (C) 2010-2011  Andrej N. Gritsenko <andrej@rep.kiev.ua>
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
 * This file is a part of FoxEye IRCd module: messages handling.
 */

#include <foxeye.h>
#if IRCD_USES_ICONV == 0 || (defined(HAVE_ICONV) && (IRCD_NEEDS_TRANSLIT == 0 || defined(HAVE_CYRILLIC_TRANSLIT)))
#include <modules.h>

#include "ircd.h"
#include "numerics.h"

static struct bindtable_t *BTIrcdCheckMessage;

/* ---------------------------------------------------------------------------
 * Common internal functions.
 */
#define _ircd_find_client_lc(I,X) Find_Key ((I)->clients, X)

#define _ircd_find_channel_lc(I,X) Find_Key ((I)->channels, X)

static inline MEMBER *_ircd_is_on_channel (CLIENT *cl, CHANNEL *ch)
{
  register MEMBER *m;

  for (m = ch->users; m; m = m->prevnick)
    if (m->who == cl)
      break;
  return m;
}

static inline void _ircd_bmsgl_chan (CHANNEL *ch, CLIENT *cl, const char *user,
				     const char *host, const char *mode,
				     const char *msg)
{
  if (ch->mode & A_ANONYMOUS)
    ircd_sendto_chan_butone(ch, cl, ":anonymous!anonymous@anonymous. %s %s :%s",
			    mode, ch->name, msg);
  else
    ircd_sendto_chan_butone(ch, cl, ":%s!%s@%s %s %s :%s",
			    cl->nick, user, host, mode, ch->name, msg);
}

static void _ircd_bmsgl_mask (IRCD *ircd, const char *t, char *mask,
			      modeflag m, const char *nick,
			      const char *user, const char *host,
			      const char *mode, const char *msg)
{
  CLIENT *tgt;
  LEAF *l = NULL;

  if (*t == '#') /* host mask */
  {
    while ((l = Next_Leaf (ircd->clients, l, NULL)))
    {
      tgt = l->s.data;
      if ((tgt->umode & (A_SERVER | A_SERVICE)) == m && !tgt->hold_upto &&
	  CLIENT_IS_LOCAL(tgt) && simple_match (mask, tgt->host) > 0)
	tgt->via->p.iface->ift |= I_PENDING;
    }
    if (!user)				/* service */
      Add_Request (I_PENDING, "*", 0, ":%s@%s %s %s :%s", nick, host, mode, t,
		   msg);
    else				/* user */
      Add_Request (I_PENDING, "*", 0, ":%s!%s@%s %s %s :%s", nick, user, host,
		   mode, t, msg);
    return;
  }
  tgt = ircd_find_client(NULL, NULL);
  if (simple_match (mask, tgt->lcnick) > 0) /* matches my name */
  {
    while ((l = Next_Leaf (ircd->clients, l, NULL)))
    {
      tgt = l->s.data;
      if (!(tgt->umode & (A_SERVER | A_SERVICE)) && !tgt->hold_upto &&
	  CLIENT_IS_LOCAL(tgt))
	tgt->via->p.iface->ift |= I_PENDING;
    }
    if (!user)				/* service */
      Add_Request (I_PENDING, "*", 0, ":%s@%s %s %s :%s", nick, host, mode, t,
		   msg);
    else				/* user */
      Add_Request (I_PENDING, "*", 0, ":%s!%s@%s %s %s :%s", nick, user, host,
		   mode, t, msg);
  }
}

static int _ircd_check_server_clients_hosts (LINK *tgt, unsigned short token,
					     const char *mask)
{
  if (tgt->cl->x.token == token)
    return 0;
  for (tgt = tgt->cl->c.lients; tgt; tgt = tgt->prev)
    if (CLIENT_IS_SERVER(tgt->cl)) {
      if (_ircd_check_server_clients_hosts (tgt, token, mask))
	return 1;
    } else if (!(tgt->cl->umode & A_SERVICE) && !tgt->cl->hold_upto)
      if (simple_match (mask, tgt->cl->host) > 0)
	return 1;
  return 0;
}

static int _ircd_can_send_to_chan (CLIENT *cl, CHANNEL *ch, const char *msg)
{
  modeflag mf = 0;
  register MEMBER *m;
  MASK *cm;
  struct binding_t *b = NULL;
  int i, x = -1;
  char buff[MB_LEN_MAX*NICKLEN+USERLEN+HOSTLEN+3];

  m = _ircd_is_on_channel (cl, ch);
  if (m)
    mf = m->mode;
  snprintf (buff, sizeof(buff), "%s!%s@%s", cl->lcnick, cl->user, cl->host);
  /* note: check all bans can be slow, I know, but what else to do? */
  for (cm = ch->bans; cm; cm = cm->next)
    if (simple_match (cm->what, buff) > 0)
      break;
  if (cm)
  {
    for (cm = ch->exempts; cm; cm = cm->next)
      if (simple_match (cm->what, buff) > 0)
	break;
    if (!cm)
      mf |= A_DENIED;
  }
  while ((b = Check_Bindtable (BTIrcdCheckMessage, ch->name, U_ALL, U_ANYCH, b)))
    if (b->name)
      continue;
    else if ((i = b->func (mf, ch->mode, msg)) > 0)
      return 1;
    else
      x &= i;
  return x;
}

/* since this is too hard to do matching, we don't send messages by alternate
   way here so this is rare ocasion when they might be lost in netsplit but
   even so there is still a chance they will come by another way due to mask */
#if IRCD_MULTICONNECT
static void _ircd_broadcast_msglist_new (IRCD *ircd, struct peer_priv *via,
			unsigned short token, int id, const char *nick,
			const char *targets, const char **tlist, size_t s,
			const char *mode, const char *msg)
{
  size_t i;
  CHANNEL *tch;
  const char *c;

  for (i = 0; i < s; i++)
  {
    if ((tch = _ircd_find_channel_lc (ircd, tlist[i]))) /* to channel */
    {
      MEMBER *mm;

      if ((c = strchr (tch->lcname, ':')))
      {
	c++;
	for (mm = tch->users; mm; mm = mm->prevnick)
	  if (!CLIENT_IS_LOCAL(mm->who) && mm->who->cs->via != via &&
	      mm->who->cs->x.token != token && (mm->who->cs->umode & A_MULTI) &&
	      !(mm->who->cs->via->p.iface->ift & I_PENDING) &&
	      simple_match (c, mm->who->cs->lcnick) > 0)
	    mm->who->cs->via->p.iface->ift |= I_PENDING;
      }
      else
      {
	for (mm = tch->users; mm; mm = mm->prevnick)
	  if (!CLIENT_IS_LOCAL(mm->who) && mm->who->cs->via != via &&
	      mm->who->cs->x.token != token && (mm->who->cs->umode & A_MULTI))
	    mm->who->cs->via->p.iface->ift |= I_PENDING;
      }
    }
    else if (*tlist[i] == '#') /* to hostmask */
    {
      LINK *srv;

      for (srv = ircd->servers; srv; srv = srv->prev)
	if ((srv->cl->umode & A_MULTI) && srv->cl->via != via &&
	    _ircd_check_server_clients_hosts (srv, token, tlist[i]+1))
	  srv->cl->via->p.iface->ift |= I_PENDING;
    }
    else if (*tlist[i] == '$') /* to servermask */
    {
      unsigned short t;

      c = tlist[i] + 1;
      for (t = 1; t < ircd->s; t++)
	if (t != token && ircd->token[t] && ircd->token[t]->via != via &&
	    (ircd->token[t]->via->link->cl->umode & A_MULTI) &&
	    simple_match (c, ircd->token[t]->lcnick) > 0)
	  ircd->token[t]->via->p.iface->ift |= I_PENDING;
    }
    else /* to client */
    {
      CLIENT *tcl = _ircd_find_client_lc (ircd, tlist[i]);

      if (tcl->cs->via->link->cl->umode & A_MULTI)
	tcl->cs->via->p.iface->ift |= I_PENDING;
    }
  }
  Add_Request (I_PENDING, "*", 0, ":%s I%s %d %s :%s", nick, mode, id,
	       targets, msg);
}
#else
# define _ircd_broadcast_msglist_new(a,b,c,d,e,f,g,h,i,j)
#endif

static void _ircd_broadcast_msglist_old (IRCD *ircd, struct peer_priv *via,
			unsigned short token, const char *nick, int all,
			const char *targets, const char **tlist, size_t s,
			const char *mode, const char *msg)
{
  size_t i;
  CHANNEL *tch;
  const char *c;

  for (i = 0; i < s; i++)
  {
    if ((tch = _ircd_find_channel_lc (ircd, tlist[i]))) /* to channel */
    {
      MEMBER *mm;

      if ((c = strchr (tch->lcname, ':')))
      {
	c++;
	for (mm = tch->users; mm; mm = mm->prevnick)
	  if (!CLIENT_IS_LOCAL(mm->who) && mm->who->cs->via != via &&
	      mm->who->cs->x.token != token &&
#if IRCD_MULTICONNECT
	      (all || !(mm->who->cs->umode & A_MULTI)) &&
#endif
	      !(mm->who->cs->via->p.iface->ift & I_PENDING) &&
	      simple_match (c, mm->who->cs->lcnick) > 0)
	    mm->who->cs->via->p.iface->ift |= I_PENDING;
      }
      else
      {
	for (mm = tch->users; mm; mm = mm->prevnick)
	  if (!CLIENT_IS_LOCAL(mm->who) && mm->who->cs->via != via &&
#if IRCD_MULTICONNECT
	      (all || !(mm->who->cs->umode & A_MULTI)) &&
#endif
	      mm->who->cs->x.token != token)
	    mm->who->cs->via->p.iface->ift |= I_PENDING;
      }
    }
    else if (*tlist[i] == '#') /* to hostmask */
    {
      LINK *srv;

      c = tlist[i] + 1;
      for (srv = ircd->servers; srv; srv = srv->prev)
	if (srv->cl->via != via &&
#if IRCD_MULTICONNECT
	    (all || !(srv->cl->umode & A_MULTI)) &&
#endif
	    _ircd_check_server_clients_hosts (srv, token, c))
	  srv->cl->via->p.iface->ift |= I_PENDING;
    }
    else if (*tlist[i] == '$') /* to servermask */
    {
      unsigned short t;

      c = tlist[i] + 1;
      for (t = 1; t < ircd->s; t++)
	if (t != token && ircd->token[t] && ircd->token[t]->via != via &&
#if IRCD_MULTICONNECT
	    (all || !(ircd->token[t]->via->link->cl->umode & A_MULTI)) &&
#endif
	    simple_match (c, ircd->token[t]->lcnick) > 0)
	  ircd->token[t]->via->p.iface->ift |= I_PENDING;
    }
    else /* to client */
    {
      CLIENT *tcl = _ircd_find_client_lc (ircd, tlist[i]);

#if IRCD_MULTICONNECT
      if (all || (tcl->cs->via->link->cl->umode & A_MULTI))
#endif
	tcl->cs->via->p.iface->ift |= I_PENDING;
    }
  }
  Add_Request (I_PENDING, "*", 0, ":%s %s %s :%s", nick, mode, targets, msg);
}

static inline CLIENT *_ircd_find_msg_target (IRCD *i, const char *target,
					     struct peer_priv *pp)
{
  CLIENT *tgt = ircd_find_client (target, pp);

  if (tgt && (tgt->hold_upto || (tgt->umode & (A_SERVER | A_SERVICE))))
    return NULL;
  //TODO: proc not plain too: user[%host]@servername user%host nickname!user@host
  return tgt;
}

#define ADD_TO_LIST(S) \
  if (s < MAXTARGETS) \
    tlist[s++] = S; \
  if (s2 && s2 < sizeof(targets) - 2) /* reserved for ,x */ \
    targets[s2++] = ','; \
  s2 += strfcpy (targets, c, sizeof(targets) - s2)


/* ---------------------------------------------------------------------------
 * Client protocol bindings.
 */

/* note: if channel mode is A_INVISIBLE then it should not be broadcasted
   and any type of messages for it from servers should generate an error!!! */

BINDING_TYPE_ircd_client_cmd(ircd_privmsg_cb);
static int ircd_privmsg_cb(INTERFACE *srv, struct peer_t *peer, char *lcnick,
			   char *user, char *host, int argc, const char **argv)
{ /* args: <msgtarget> <text to be sent> */
  CLIENT *cl = ((struct peer_priv *)peer->iface->data)->link->cl, *tcl;
  MEMBER *tch;
  char *c, *cnext;
  register char *cmask;
  int n;
  const char *tlist[MAXTARGETS];
  size_t s = 0, s2 = 0;
  char targets[MESSAGEMAX];

  if (argc == 0 || !*argv[0])
    return ircd_do_unumeric (cl, ERR_NORECIPIENT, cl, 0, NULL);
  if (argc == 1 || !*argv[1])
    return ircd_do_unumeric (cl, ERR_NOTEXTTOSEND, cl, 0, NULL);
#ifdef IDLE_FROM_MSG
  ((struct peer_priv *)peer->iface->data)->noidle = Time;
#endif
  for (c = (char *)argv[0], n = 0; c; c = cnext)
  {
    if ((cnext = strchr (c, ',')))
      *cnext++ = 0;
    if (++n > MAXTARGETS)
      ircd_do_unumeric (cl, ERR_TOOMANYTARGETS, cl, 0, "No Message Delivered.");
    else if ((tch = ircd_find_member ((IRCD *)srv->data, c, NULL))
	     != NOSUCHCHANNEL)
    {
      if ((tch->chan->mode & (A_PRIVATE | A_SECRET)) &&
	  _ircd_is_on_channel(cl, tch->chan) == NULL) /* don't discover it so */
	ircd_do_unumeric (cl, ERR_NOSUCHNICK, cl, 0, c);
      else if (_ircd_can_send_to_chan (cl, tch->chan, argv[1]))
      {
	_ircd_bmsgl_chan (tch->chan, cl, user, host, "PRIVMSG", argv[1]);
	if (!(tch->mode & A_INVISIBLE))
	  ADD_TO_LIST(tch->chan->lcname);
      }
      else
	ircd_do_unumeric (cl, ERR_CANNOTSENDTOCHAN, cl, 0, c);
    }
    else if ((cl->umode & (A_OP | A_HALFOP)) && //TODO: can local ops send it too?
	     (*argv[0] == '#' || *argv[0] == '$'))
    {
      if (!(cmask = strrchr (c+1, '.')))
	ircd_do_unumeric (cl, ERR_NOTOPLEVEL, cl, 0, argv[1]);
      else if (strpbrk (cmask, "*?"))
	ircd_do_unumeric (cl, ERR_WILDTOPLEVEL, cl, 0, argv[1]);
      else
      {
#ifdef USE_SERVICES
	if (CLIENT_IS_SERVICE (cl))
	  _ircd_bmsgl_mask ((IRCD *)srv->data, c, &c[1], 0, peer->dname, NULL,
			    ircd_find_client(NULL, NULL), "PRIVMSG", argv[1]);
	else
#endif
	  _ircd_bmsgl_mask ((IRCD *)srv->data, c, &c[1], 0, peer->dname, user,
			    host, "PRIVMSG", argv[1]);
	ADD_TO_LIST(c); //TODO: lowercase it?
      }
    }
    else if ((tcl = _ircd_find_msg_target ((IRCD *)srv->data, c, NULL)))
    {
      if (CLIENT_IS_LOCAL(tcl))
      {
	New_Request (tcl->via->p.iface, 0, "PRIVMSG %s :%s", c, argv[1]);
	if (tcl->umode & A_AWAY)
	  ircd_do_unumeric (cl, RPL_AWAY, tcl, 0, tcl->away);
      }
      else
	ADD_TO_LIST(tcl->lcnick);
    }
    else
      ircd_do_unumeric (cl, ERR_NOSUCHNICK, cl, 0, c);
  }
  if (s)
  {
    _ircd_broadcast_msglist_new ((IRCD *)srv->data, NULL, 0, ircd_new_id(),
				 peer->dname, targets, tlist, s, "PRIVMSG",
				 argv[1]);
    _ircd_broadcast_msglist_old ((IRCD *)srv->data, NULL, 0, peer->dname, 0,
				 targets, tlist, s, "PRIVMSG", argv[1]);
  }
  return 1;
}

BINDING_TYPE_ircd_client_cmd(ircd_notice_cb);
static int ircd_notice_cb(INTERFACE *srv, struct peer_t *peer, char *lcnick,
			  char *user, char *host, int argc, const char **argv)
{ /* args: <msgtarget> <text to be sent> */
  CLIENT *cl = ((struct peer_priv *)peer->iface->data)->link->cl, *tcl;
  MEMBER *tch;
  char *c, *cnext;
  register char *cmask;
  int n;
  const char *tlist[MAXTARGETS];
  size_t s = 0, s2 = 0;
  char targets[MESSAGEMAX];

  if (argc == 0 || !*argv[0])
    return ircd_do_unumeric (cl, ERR_NORECIPIENT, cl, 0, NULL);
  if (argc == 1 || !*argv[1])
    return ircd_do_unumeric (cl, ERR_NOTEXTTOSEND, cl, 0, NULL);
  for (c = (char *)argv[0], n = 0; c; c = cnext)
  {
    if ((cnext = strchr (c, ',')))
      *cnext++ = 0;
    if (++n > MAXTARGETS)
      continue;
    else if ((tch = ircd_find_member ((IRCD *)srv->data, c, NULL))
	     != NOSUCHCHANNEL)
    {
      if (_ircd_can_send_to_chan (cl, tch->chan, argv[1]))
      {
	_ircd_bmsgl_chan (tch->chan, cl, user, host, "NOTICE", argv[1]);
	if (!(tch->mode & A_INVISIBLE))
	  ADD_TO_LIST(tch->chan->lcname);
      }
    }
    else if ((cl->umode & (A_OP | A_HALFOP)) && //TODO: can local ops send it too?
	     (*argv[0] == '#' || *argv[0] == '$'))
    {
      if (!(cmask = strrchr (c+1, '.')))
	ircd_do_unumeric (cl, ERR_NOTOPLEVEL, cl, 0, argv[1]);
      else if (strpbrk (cmask, "*?"))
	ircd_do_unumeric (cl, ERR_WILDTOPLEVEL, cl, 0, argv[1]);
      else
      {
#ifdef USE_SERVICES
	if (CLIENT_IS_SERVICE (cl))
	  _ircd_bmsgl_mask ((IRCD *)srv->data, c, &c[1], 0, peer->dname, NULL,
			    ircd_find_client(NULL, NULL), "NOTICE", argv[1]);
	else
#endif
	  _ircd_bmsgl_mask ((IRCD *)srv->data, c, &c[1], 0, peer->dname, user,
			    host, "NOTICE", argv[1]);
	ADD_TO_LIST(c); //TODO: lowercase it?
      }
    }
    else if ((tcl = _ircd_find_msg_target ((IRCD *)srv->data, c, NULL)))
    {
      if (CLIENT_IS_LOCAL(tcl))
	New_Request (tcl->via->p.iface, 0, "NOTICE %s :%s", c, argv[1]);
      else
	ADD_TO_LIST(tcl->lcnick);
    }
  }
  if (s)
  {
    _ircd_broadcast_msglist_new ((IRCD *)srv->data, NULL, 0, ircd_new_id(),
				 peer->dname, targets, tlist, s, "NOTICE",
				 argv[1]);
    _ircd_broadcast_msglist_old ((IRCD *)srv->data, NULL, 0, peer->dname, 0,
				 targets, tlist, s, "NOTICE", argv[1]);
  }
  return 1;
}

BINDING_TYPE_ircd_client_cmd(ircd_squery_cb);
static int ircd_squery_cb(INTERFACE *srv, struct peer_t *peer, char *lcnick,
			  char *user, char *host, int argc, const char **argv)
{ /* args: <servicename> <text to be sent> */
  CLIENT *cl = ((struct peer_priv *)peer->iface->data)->link->cl, *tcl;

  if (argc == 0 || !*argv[0])
    return ircd_do_unumeric (cl, ERR_NORECIPIENT, cl, 0, NULL);
  if (argc == 1 || !*argv[1])
    return ircd_do_unumeric (cl, ERR_NOTEXTTOSEND, cl, 0, NULL);
  if (!(tcl = _ircd_find_msg_target ((IRCD *)srv->data, argv[0], NULL)) ||
      !CLIENT_IS_SERVICE (tcl))
    return ircd_do_unumeric (cl, ERR_NOSUCHSERVICE, cl, 0, argv[0]);
#ifdef USE_SERVICES
  if (CLIENT_IS_LOCAL (tcl))
  {
    New_Request (tcl->via->p.iface, 0, "SQUERY %s :%s", c, argv[1]);
    return 1;
  }
#endif
  _ircd_broadcast_msglist_new((IRCD *)srv->data, NULL, 0, ircd_new_id(),
			      peer->dname, argv[0], argv, 1, "SQUERY", argv[1]);
  _ircd_broadcast_msglist_old((IRCD *)srv->data, NULL, 0, peer->dname, 0,
			      argv[0], argv, 1, "SQUERY", argv[1]);
  return 1;
}


/* ---------------------------------------------------------------------------
 * Server-to-server protocol bindings
 */

/* not defining __TRANSIT__ here since token is handled by _ircd_broadcast_* */
BINDING_TYPE_ircd_server_cmd(ircd_privmsg_sb);
static int ircd_privmsg_sb(INTERFACE *srv, struct peer_t *peer, unsigned short token,
			   const char *sender, const char *lcsender, char *cmd,
			   int argc, const char **argv)
{ /* args: <msgtarget> <text to be sent> */
  CLIENT *cl, *tcl;
  MEMBER *tch;
  char *c, *cnext;
  struct peer_priv *pp = peer->iface->data; /* it's really peer */
  const char *tlist[MAXTARGETS];
  size_t s = 0, s2 = 0;
  char targets[MESSAGEMAX];

  /* check number of parameters */
  if (argc != 2) {
    ERROR("ircd:got invalid PRIVMSG via %s with %d parameters", peer->dname,
	  argc);
    return ircd_recover_done(pp, "Invalid number of parameters");
  }
  cl = _ircd_find_client_lc((IRCD *)srv->data, lcsender);
  for (c = (char *)argv[0]; c; c = cnext)
  {
    if ((cnext = strchr(c, ',')))
      *cnext++ = 0;
    if (s == MAXTARGETS) {
      _ircd_broadcast_msglist_old((IRCD *)srv->data, pp, token, sender, 1,
				  targets, tlist, s, "PRIVMSG", argv[1]);
      s = s2 = 0;
    }
    tch = ircd_find_member((IRCD *)srv->data, c, NULL);
    if (tch != NOSUCHCHANNEL) {
      if (!_ircd_can_send_to_chan(cl, tch->chan, argv[1]))
	Add_Request(I_LOG, "*", F_WARN,
		    "ircd:not permitted channel message from %s via %s",
		    sender, peer->dname);
      if (!(tch->mode & A_INVISIBLE)) {
	ADD_TO_LIST(tch->chan->lcname);
	_ircd_bmsgl_chan(tch->chan, cl, cl->user, cl->host, "PRIVMSG", argv[1]);
      } else {
	ERROR("ircd:PRIVMSG via %s for channel %s ignored", peer->dname, c);
	ircd_recover_done(pp, "Invalid message target");
      }
    } else if ((cl->umode & A_OP) && (*argv[0] == '#' || *argv[0] == '$')) {
      if (CLIENT_IS_SERVICE(cl))
	_ircd_bmsgl_mask((IRCD *)srv->data, c, &c[1], 0, sender, NULL,
			 cl->cs->lcnick, "PRIVMSG", argv[1]);
      else
	_ircd_bmsgl_mask((IRCD *)srv->data, c, &c[1], 0, sender, cl->user,
			 cl->host, "PRIVMSG", argv[1]);
      ADD_TO_LIST(c); //TODO: lowercase it?
    } else if ((tcl = _ircd_find_msg_target((IRCD *)srv->data, c, pp))) {
      if (CLIENT_IS_LOCAL(tcl))
      {
	if (CLIENT_IS_SERVICE(cl))
	  New_Request(tcl->via->p.iface, 0, ":%s@%s PRIVMSG %s :%s", sender,
		      cl->cs->lcnick, c, argv[1]);
	else
	  New_Request(tcl->via->p.iface, 0, ":%s!%s@%s PRIVMSG %s :%s", sender,
		      cl->user, cl->host, c, argv[1]);
	if (tcl->umode & A_AWAY)
	  ircd_do_unumeric(cl, RPL_AWAY, tcl, 0, tcl->away);
      }
      else
	ADD_TO_LIST(tcl->lcnick);
    } else {
      ERROR("ircd:invalid PRIVMSG target %s via %s", c, peer->dname);
      ircd_recover_done(pp, "Invalid recipient");
    }
  }
  if (s)
    _ircd_broadcast_msglist_old((IRCD *)srv->data, pp, token, sender, 1,
				targets, tlist, s, "PRIVMSG", argv[1]);
  return 1;
}

BINDING_TYPE_ircd_server_cmd(ircd_notice_sb);
static int ircd_notice_sb(INTERFACE *srv, struct peer_t *peer, unsigned short token,
			  const char *sender, const char *lcsender, char *cmd,
			  int argc, const char **argv)
{ /* args: <msgtarget> <text to be sent> */
  CLIENT *cl, *tcl;
  MEMBER *tch;
  char *c, *cnext;
  struct peer_priv *pp = peer->iface->data; /* it's really peer */
  const char *tlist[MAXTARGETS];
  size_t s = 0, s2 = 0;
  char targets[MESSAGEMAX];

  /* check number of parameters */
  if (argc != 2) {
    Add_Request(I_LOG, "*", F_WARN,
		"ircd:got invalid NOTICE via %s with %d parameters",
		peer->dname, argc);
    return (1);
  }
  cl = _ircd_find_client_lc((IRCD *)srv->data, lcsender);
  for (c = (char *)argv[0]; c; c = cnext)
  {
    if ((cnext = strchr(c, ',')))
      *cnext++ = 0;
    if (s == MAXTARGETS) {
      _ircd_broadcast_msglist_old((IRCD *)srv->data, pp, token, sender, 1,
				  targets, tlist, s, "NOTICE", argv[1]);
      s = s2 = 0;
    }
    tch = ircd_find_member((IRCD *)srv->data, c, NULL);
    if (tch != NOSUCHCHANNEL) {
      if (!_ircd_can_send_to_chan(cl, tch->chan, argv[1]))
	Add_Request(I_LOG, "*", F_WARN,
		    "ircd:not permitted channel message from %s via %s",
		    sender, peer->dname);
      if (!(tch->mode & A_INVISIBLE)) {
	ADD_TO_LIST(tch->chan->lcname);
	_ircd_bmsgl_chan(tch->chan, cl, cl->user, cl->host, "NOTICE", argv[1]);
      } else
	Add_Request(I_LOG, "*", F_WARN, "ircd:invalid NOTICE target %s via %s",
		    c, peer->dname);
    } else if ((cl->umode & A_OP) && (*argv[0] == '#' || *argv[0] == '$')) {
      if (CLIENT_IS_SERVICE(cl))
	_ircd_bmsgl_mask((IRCD *)srv->data, c, &c[1], 0, sender, NULL,
			 cl->cs->lcnick, "NOTICE", argv[1]);
      else
	_ircd_bmsgl_mask((IRCD *)srv->data, c, &c[1], 0, sender, cl->user,
			 cl->host, "NOTICE", argv[1]);
      ADD_TO_LIST(c); //TODO: lowercase it?
    } else if ((tcl = _ircd_find_msg_target((IRCD *)srv->data, c, pp))) {
      if (CLIENT_IS_LOCAL(tcl)) {
	if (CLIENT_IS_SERVICE(cl))
	  New_Request(tcl->via->p.iface, 0, ":%s@%s NOTICE %s :%s", sender,
		      cl->cs->lcnick, c, argv[1]);
	else
	  New_Request(tcl->via->p.iface, 0, ":%s!%s@%s NOTICE %s :%s", sender,
		      cl->user, cl->host, c, argv[1]);
      } else
	ADD_TO_LIST(tcl->lcnick);
    } else
      Add_Request(I_LOG, "*", F_WARN, "ircd:invalid NOTICE target %s via %s", c,
		  peer->dname);
  }
  if (s)
    _ircd_broadcast_msglist_old((IRCD *)srv->data, pp, token, sender, 1,
				targets, tlist, s, "NOTICE", argv[1]);
  return 1;
}

BINDING_TYPE_ircd_server_cmd(ircd_squery_sb);
static int ircd_squery_sb(INTERFACE *srv, struct peer_t *peer, unsigned short token,
			  const char *sender, const char *lcsender, char *cmd,
			  int argc, const char **argv)
{ /* args: <servicename> <text to be sent> */
  CLIENT *tcl;
  struct peer_priv *pp = peer->iface->data; /* it's really peer */

  /* check number of parameters */
  if (argc != 2) {
    ERROR("ircd:got invalid SQUERY via %s with %d parameters", peer->dname,
	  argc);
    return ircd_recover_done(pp, "Invalid number of parameters");
  }
  //cl = _ircd_find_client_lc((IRCD *)srv->data, lcsender);
  if (!(tcl = _ircd_find_msg_target((IRCD *)srv->data, argv[0], pp)) ||
      !CLIENT_IS_SERVICE(tcl)) {
    ERROR("ircd:invalid SQUERY target %s via %s", argv[0], peer->dname);
    return ircd_recover_done(pp, "Invalid recipient");
  }
#ifdef USE_SERVICES
  if (CLIENT_IS_LOCAL(tcl))
    New_Request(tcl->via->p.iface, 0, ":%s SQUERY %s :%s", sender, c, argv[1]);
  else
#endif
    _ircd_broadcast_msglist_old((IRCD *)srv->data, pp, token, sender, 1,
				argv[0], argv, 1, "SQUERY", argv[1]);
  return (1);
}

#if IRCD_MULTICONNECT
BINDING_TYPE_ircd_server_cmd(ircd_iprivmsg);
static int ircd_iprivmsg(INTERFACE *srv, struct peer_t *peer, unsigned short token,
			 const char *sender, const char *lcsender, char *cmd,
			 int argc, const char **argv)
{ /* args: <id> <msgtarget> <text to be sent> */
  CLIENT *cl, *tcl;
  MEMBER *tch;
  char *c, *cnext;
  struct peer_priv *pp = peer->iface->data; /* it's really peer */
  const char *tlist[MAXTARGETS];
  size_t s = 0, s2 = 0;
  int id;
  char targets[MESSAGEMAX];

  /* check number of parameters */
  if (argc != 3) {
    ERROR("ircd:got invalid IPRIVMSG via %s with %d parameters", peer->dname,
	  argc);
    return ircd_recover_done(pp, "Invalid number of parameters");
  }
  id = atoi(argv[0]);
  if (!ircd_test_id(((IRCD *)srv->data)->token[token], id))
    //TODO: log duplicate?
    return (1);
  cl = _ircd_find_client_lc((IRCD *)srv->data, lcsender);
  for (c = (char *)argv[1]; c; c = cnext)
  {
    if ((cnext = strchr(c, ',')))
      *cnext++ = 0;
    if (s == MAXTARGETS) {
      ERROR("ircd:too many targets of IPRIVMSG via %s, may lose message for %s",
	    peer->dname, c);
      ircd_recover_done(pp, "Too many targets");
    }
    tch = ircd_find_member((IRCD *)srv->data, c, NULL);
    if (tch != NOSUCHCHANNEL) {
      if (!_ircd_can_send_to_chan(cl, tch->chan, argv[2]))
	Add_Request(I_LOG, "*", F_WARN,
		    "ircd:not permitted channel message from %s via %s",
		    sender, peer->dname);
      if (!(tch->mode & A_INVISIBLE)) {
	_ircd_bmsgl_chan(tch->chan, cl, cl->user, cl->host, "PRIVMSG", argv[2]);
	ADD_TO_LIST(tch->chan->lcname);
      } else {
	ERROR("ircd:IPRIVMSG via %s for channel %s ignored", peer->dname, c);
	ircd_recover_done(pp, "Invalid message target");
      }
    } else if ((cl->umode & A_OP) && (*argv[1] == '#' || *argv[1] == '$')) {
      if (CLIENT_IS_SERVICE(cl))
	_ircd_bmsgl_mask((IRCD *)srv->data, c, &c[1], 0, sender, NULL,
			 cl->cs->lcnick, "PRIVMSG", argv[2]);
      else
	_ircd_bmsgl_mask((IRCD *)srv->data, c, &c[1], 0, sender, cl->user,
			 cl->host, "PRIVMSG", argv[2]);
      ADD_TO_LIST(c); //TODO: lowercase it?
    } else if ((tcl = _ircd_find_msg_target((IRCD *)srv->data, c, pp))) {
      if (CLIENT_IS_LOCAL(tcl))
      {
	if (CLIENT_IS_SERVICE(cl))
	  New_Request(tcl->via->p.iface, 0, ":%s@%s PRIVMSG %s :%s", sender,
		      cl->cs->lcnick, c, argv[2]);
	else
	  New_Request(tcl->via->p.iface, 0, ":%s!%s@%s PRIVMSG %s :%s", sender,
		      cl->user, cl->host, c, argv[2]);
	if (tcl->umode & A_AWAY)
	  ircd_do_unumeric(cl, RPL_AWAY, tcl, 0, tcl->away);
      }
      else
	ADD_TO_LIST(tcl->lcnick);
    } else {
      ERROR("ircd:invalid IPRIVMSG target %s via %s", c, peer->dname);
      ircd_recover_done(pp, "Invalid recipient");
    }
  }
  if (s) {
    _ircd_broadcast_msglist_new((IRCD *)srv->data, pp, token, id, sender,
				targets, tlist, s, "PRIVMSG", argv[2]);
    _ircd_broadcast_msglist_old((IRCD *)srv->data, pp, token, sender, 0,
				targets, tlist, s, "PRIVMSG", argv[2]);
  }
  return 1;
}

BINDING_TYPE_ircd_server_cmd(ircd_inotice);
static int ircd_inotice(INTERFACE *srv, struct peer_t *peer, unsigned short token,
			const char *sender, const char *lcsender, char *cmd,
			int argc, const char **argv)
{ /* args: <id> <msgtarget> <text to be sent> */
  CLIENT *cl, *tcl;
  MEMBER *tch;
  char *c, *cnext;
  struct peer_priv *pp = peer->iface->data; /* it's really peer */
  const char *tlist[MAXTARGETS];
  size_t s = 0, s2 = 0;
  int id;
  char targets[MESSAGEMAX];

  /* check number of parameters */
  if (argc != 3) {
    Add_Request(I_LOG, "*", F_WARN,
		"ircd:got invalid INOTICE via %s with %d parameters",
		peer->dname, argc);
    return (1);
  }
  id = atoi(argv[0]);
  if (!ircd_test_id(((IRCD *)srv->data)->token[token], id))
    //TODO: log duplicate?
    return (1);
  cl = _ircd_find_client_lc((IRCD *)srv->data, lcsender);
  for (c = (char *)argv[1]; c; c = cnext)
  {
    if ((cnext = strchr(c, ',')))
      *cnext++ = 0;
    if (s == MAXTARGETS)
      Add_Request(I_LOG, "*", F_WARN, "ircd:too many targets of INOTICE via "
		  "%s, may lose message for %s", peer->dname, c);
    tch = ircd_find_member((IRCD *)srv->data, c, NULL);
    if (tch != NOSUCHCHANNEL) {
      if (!_ircd_can_send_to_chan(cl, tch->chan, argv[2]))
	Add_Request(I_LOG, "*", F_WARN,
		    "ircd:not permitted channel message from %s via %s",
		    sender, peer->dname);
      if (!(tch->mode & A_INVISIBLE)) {
	_ircd_bmsgl_chan(tch->chan, cl, cl->user, cl->host, "NOTICE", argv[2]);
	ADD_TO_LIST(tch->chan->lcname);
      } else
	Add_Request(I_LOG, "*", F_WARN, "ircd:invalid INOTICE target %s via %s",
		    c, peer->dname);
    } else if ((cl->umode & A_OP) && (*argv[1] == '#' || *argv[1] == '$')) {
      if (CLIENT_IS_SERVICE(cl))
	_ircd_bmsgl_mask((IRCD *)srv->data, c, &c[1], 0, sender, NULL,
			 cl->cs->lcnick, "NOTICE", argv[2]);
      else
	_ircd_bmsgl_mask((IRCD *)srv->data, c, &c[1], 0, sender, cl->user,
			 cl->host, "NOTICE", argv[2]);
      ADD_TO_LIST(c); //TODO: lowercase it?
    } else if ((tcl = _ircd_find_msg_target((IRCD *)srv->data, c, pp))) {
      if (CLIENT_IS_LOCAL(tcl)) {
	if (CLIENT_IS_SERVICE(cl))
	  New_Request(tcl->via->p.iface, 0, ":%s@%s NOTICE %s :%s", sender,
		      cl->cs->lcnick, c, argv[2]);
	else
	  New_Request(tcl->via->p.iface, 0, ":%s!%s@%s NOTICE %s :%s", sender,
		      cl->user, cl->host, c, argv[2]);
      } else
	ADD_TO_LIST(tcl->lcnick);
    } else
      Add_Request(I_LOG, "*", F_WARN, "ircd:invalid INOTICE target %s via %s", c,
		  peer->dname);
  }
  if (s) {
    _ircd_broadcast_msglist_new((IRCD *)srv->data, pp, token, id, sender,
				targets, tlist, s, "NOTICE", argv[2]);
    _ircd_broadcast_msglist_old((IRCD *)srv->data, pp, token, sender, 0,
				targets, tlist, s, "NOTICE", argv[2]);
  }
  return 1;
}

BINDING_TYPE_ircd_server_cmd(ircd_isquery);
static int ircd_isquery(INTERFACE *srv, struct peer_t *peer, unsigned short token,
			const char *sender, const char *lcsender, char *cmd,
			int argc, const char **argv)
{ /* args: <id> <servicename> <text to be sent> */
  CLIENT *tcl;
  struct peer_priv *pp = peer->iface->data; /* it's really peer */
  int id;

  /* check number of parameters */
  if (argc != 3) {
    ERROR("ircd:got invalid ISQUERY via %s with %d parameters", peer->dname,
	  argc);
    return ircd_recover_done(pp, "Invalid number of parameters");
  }
  id = atoi(argv[0]);
  if (!ircd_test_id(((IRCD *)srv->data)->token[token], id))
    //TODO: log duplicate?
    return (1);
  //cl = _ircd_find_client_lc((IRCD *)srv->data, lcsender);
  if (!(tcl = _ircd_find_msg_target((IRCD *)srv->data, argv[1], pp)) ||
      !CLIENT_IS_SERVICE(tcl)) {
    ERROR("ircd:invalid ISQUERY target %s via %s", argv[1], peer->dname);
    return ircd_recover_done(pp, "Invalid recipient");
  }
#ifdef USE_SERVICES
  if (CLIENT_IS_LOCAL(tcl))
    New_Request(tcl->via->p.iface, 0, ":%s SQUERY %s :%s", sender, c, argv[2]);
  else
#endif
  {
    _ircd_broadcast_msglist_new((IRCD *)srv->data, pp, token, id, sender,
				argv[1], &argv[1], 1, "SQUERY", argv[2]);
    _ircd_broadcast_msglist_old((IRCD *)srv->data, pp, token, sender, 0,
				argv[1], &argv[1], 1, "SQUERY", argv[2]);
  }
  return (1);
}
#endif


/* ---------------------------------------------------------------------------
 * Common external functions.
 */


/* common end and start of bindings */
void ircd_message_proto_end (void)
{
  Delete_Binding ("ircd-client-cmd", &ircd_privmsg_cb, NULL);
  Delete_Binding ("ircd-client-cmd", &ircd_notice_cb, NULL);
  Delete_Binding ("ircd-client-cmd", &ircd_squery_cb, NULL);
  Delete_Binding ("ircd-server-cmd", (Function)&ircd_privmsg_sb, NULL);
  Delete_Binding ("ircd-server-cmd", (Function)&ircd_notice_sb, NULL);
  Delete_Binding ("ircd-server-cmd", (Function)&ircd_squery_sb, NULL);
#if IRCD_MULTICONNECT
  Delete_Binding ("ircd-server-cmd", (Function)&ircd_iprivmsg, NULL);
  Delete_Binding ("ircd-server-cmd", (Function)&ircd_inotice, NULL);
  Delete_Binding ("ircd-server-cmd", (Function)&ircd_isquery, NULL);
#endif
}

void ircd_message_proto_start (void)
{
  BTIrcdCheckMessage = Add_Bindtable ("ircd-check-message", B_MASK);
  Add_Binding ("ircd-client-cmd", "privmsg", 0, 0, &ircd_privmsg_cb, NULL);
  Add_Binding ("ircd-client-cmd", "notice", 0, 0, &ircd_notice_cb, NULL);
  Add_Binding ("ircd-client-cmd", "squery", 0, 0, &ircd_squery_cb, NULL);
  Add_Binding ("ircd-server-cmd", "privmsg", 0, 0, (Function)&ircd_privmsg_sb, NULL);
  Add_Binding ("ircd-server-cmd", "notice", 0, 0, (Function)&ircd_notice_sb, NULL);
  Add_Binding ("ircd-server-cmd", "squery", 0, 0, (Function)&ircd_squery_sb, NULL);
#if IRCD_MULTICONNECT
  Add_Binding ("ircd-server-cmd", "iprivmsg", 0, 0, (Function)&ircd_iprivmsg, NULL);
  Add_Binding ("ircd-server-cmd", "inotice", 0, 0, (Function)&ircd_inotice, NULL);
  Add_Binding ("ircd-server-cmd", "isquery", 0, 0, (Function)&ircd_isquery, NULL);
#endif
}
#endif
