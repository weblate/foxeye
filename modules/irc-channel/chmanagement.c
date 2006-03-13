/*
 * Copyright (C) 2005-2006  Andrej N. Gritsenko <andrej@rep.kiev.ua>
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
 * FoxEye's "irc-channel" module. Channel management: parsing and setting all
 *   modes on the channel, "ss-irc" bindings.
 *   
 */

#include "foxeye.h"

#include <ctype.h>
#include <pthread.h>

#include "tree.h"
#include "wtmp.h"
#include "init.h"
#include "sheduler.h"
#include "direct.h"
#include "irc-channel.h"

extern long int ircch_enforcer_time;		/* from irc-channel.c */
extern bool ircch_ignore_ident_prefix;
extern long int ircch_ban_keep;
extern long int ircch_exempt_keep;
extern long int ircch_invite_keep;
extern long int ircch_greet_time;
extern long int ircch_mode_timeout;

#define	MODECHARSMAX	32	/* long int capable */
#define	MODECHARSFIX	2	/* channel's specials, see net_t structure */

typedef struct
{
  uchar mc[MODECHARSMAX];	/* chars of modes */
  uchar mcf[MODECHARSFIX];	/* references to above for specials */
  uchar admin, anonymous, halfop;
} mch_t;

typedef struct
{
  size_t changes, pos, apos;
  char modechars[MODECHARSMAX];
  const char *cmd;
  char mchg[STRING];
  char args[STRING];
} modebuf_t;

static mch_t *ModeChars = NULL;			

/* TODO: extra channel modes:
    Rizon: MNOZ
    irchighway: AKMNOS
    ChatSpike: a,fruzACGKL,MNOQSTV */

static void _make_modechars (char *modechars, net_t *net)
{
  unsigned long int i, m;

  if (!ModeChars)
  {
    ModeChars = safe_calloc (1, sizeof(mch_t));
    for (i = 0; i < MODECHARSFIX; i++)
      ModeChars->mcf[i] = MODECHARSMAX;
    for (m = 1, i = 0; i < MODECHARSMAX && m; m += m, i++)
    {
      /* fill network specific values */
      if (m == A_RESTRICTED)
	ModeChars->mcf[0] = i;
      else if (m == A_REGISTERED)
	ModeChars->mcf[1] = i;
      else if (m == A_ADMIN)	/* first reserved is admin status 'a' */
	ModeChars->admin = i;
      else if (m == A_ANONYMOUS)/* second reserved is anonymous 'a' */
	ModeChars->anonymous = i;
      else if (m == A_HALFOP)	/* third reserved is halfop 'h' */
	ModeChars->halfop = i;
      /* and standard ones */
      else if (m == A_OP)
	ModeChars->mc[i] = 'o';
      else if (m == A_VOICE)
	ModeChars->mc[i] = 'v';
      else if (m == A_INVITEONLY)
	ModeChars->mc[i] = 'i';
      else if (m == A_MODERATED)
	ModeChars->mc[i] = 'm';
      else if (m == A_QUIET)
	ModeChars->mc[i] = 'q';
      else if (m == A_NOOUTSIDE)
	ModeChars->mc[i] = 'n';
      else if (m == A_PRIVATE)
	ModeChars->mc[i] = 'p';
      else if (m == A_SECRET)
	ModeChars->mc[i] = 's';
      else if (m == A_REOP)
	ModeChars->mc[i] = 'r';
      else if (m == A_TOPICLOCK)
	ModeChars->mc[i] = 't';
      else if (m == A_NOCOLOR)
	ModeChars->mc[i] = 'c';
      else if (m == A_ASCIINICK)
	ModeChars->mc[i] = 'z';
    }
  }
  memcpy (modechars, &ModeChars->mc, MODECHARSMAX);
  if (net->features & L_HASADMIN)
    modechars[ModeChars->admin] = 'a';		/* admin */
  else
  {
    modechars[ModeChars->admin] = 'O';		/* creator */
    modechars[ModeChars->anonymous] = 'a';	/* anonymous */
  }
  if (net->features & L_HASHALFOP)
    modechars[ModeChars->halfop] = 'h';		/* halfop */
  for (i = 0; i < MODECHARSFIX; i++)
    if (ModeChars->mcf[i] < MODECHARSMAX)
      modechars[ModeChars->mcf[i]] = net->modechars[i];
}

static link_t *_find_me_op (net_t *net, ch_t *ch)
{
  link_t *me;

  if (!ch)
    return NULL;
  for (me = net->me->channels; me; me = me->prevchan)
    if (me->chan == ch)
      break;
  if (me && !(me->mode & (A_ADMIN | A_OP | A_HALFOP)))
    return NULL;
  return me;
}

/* sf is global (G) plus server (S) flags, cf is channel (C) flags
   (S) are U_DENY,U_ACCESS,U_OP,U_HALFOP,U_AUTO,U_DEOP, all other are (G) */
static userflag _make_rf (net_t *net, userflag sf, userflag cf)
{
  userflag rf, tf;

  rf = cf & (U_DENY | U_ACCESS);		/* channel access group prior */
  if (!rf)
    rf = sf & (U_DENY | U_ACCESS);
  rf |= (cf | sf) & (U_FRIEND | U_OWNER | U_BOT);	/* just OR these */
  if (!(net->features & L_HASADMIN))
  {
    tf = U_HALFOP | U_OP | U_DEOP;
    rf |= (cf | sf) & U_MASTER;
  }
  else
    tf = U_MASTER | U_HALFOP | U_OP | U_DEOP;
  if (cf & (U_QUIET | U_VOICE | U_SPEAK))	/* channel voice group prior */
    rf |= cf & (U_QUIET | U_VOICE | U_SPEAK);
  else
    rf |= sf & (U_QUIET | U_VOICE);
  if (cf & tf)					/* channel op group prior */
    rf |= cf & (tf | U_AUTO);
  else
    rf |= sf & (tf | U_AUTO);
  return rf;
}

static char *_make_mask (char *mask, char *uh, size_t s)
{
  register char *c, *m;
  int n;

  if (s < 9) return NULL;	/* it cannot be true, at least "*!*@*.xx\0" */
  m = mask;
  n = 0;
  for (c = uh; *c && *c != '!'; c++);
  *m++ = '*';
  if (ircch_ignore_ident_prefix == TRUE && *c == '!' && strchr ("^~-=+", c[1]))
  {
    *m++ = '!';
    *m++ = '?';
    c += 2;
  }
  while (m < &mask[s-3] && *c)
  {
    *m++ = *c;
    if (*c++ == '@')
      break;
  }
  uh = c;
  while (*c) c++;
  while (c > uh)
  {
    if (*(--c) == '.' && (++n) == 3)		/* third dot reached, stop */
      break;
    if (*c >= '0' && *c <= '9' && n != 1)	/* in 2nd domain ignore it */
      break;
  }
  if (n == 0)					/* numeric host? */
  {
    while (m < &mask[s-3] && *c && n < 3)
    {
      *m++ = *c++;
      if (*c == '.')
	n++;
    }
    *m++ = '.';
    *m++ = '*';
  }
  else if (c != uh || n > 1)			/* some numerics found */
  {
    *m++ = '*';
    *m++ = '.';
    while (*c && *c != '.')
      c++;
    if (*c)
      c++;
    while (m < &mask[s-1] && *c)
      *m++ = *c++;
  }
  else						/* domain 1st or 2nd level */
  {
    while (m < &mask[s-1] && *c)
      *m++ = *c++;
  }
  *m = 0;
  return mask;
}

static char *_make_literal_mask (char *mask, char *uh, size_t s)
{
  register char *c, *m;

  if (s < 9) return NULL;	/* it cannot be true, at least "*!*@*.xx\0" */
  m = mask;
  for (c = uh; *c && *c != '!'; c++);
  *m++ = '*';
  if (ircch_ignore_ident_prefix == TRUE && *c == '!' && strchr ("^~-=+", c[1]))
  {
    *m++ = '!';
    *m++ = '?';
    c += 2;
  }
  while (m < &mask[s-1] && *c)
    *m++ = *c;
  *m = 0;
  return mask;
}

static void _flush_mode (net_t *net, ch_t *chan, modebuf_t *mbuf)
{
  char *c;

  if (mbuf->cmd == NULL || mbuf->changes == 0)
    return;
  c = strrchr (chan->chi->name, '@');
  if (c) *c = 0;
  mbuf->mchg[mbuf->pos] = 0;
  mbuf->args[mbuf->apos] = 0;
  New_Request (net->neti, 0, "%s %s %s %s", mbuf->cmd, chan->chi->name,
	       mbuf->mchg, mbuf->args);
  if (c) *c = '@';
  mbuf->cmd = NULL;
  mbuf->changes = mbuf->pos = mbuf->apos = 0;
}

static void _push_mode (net_t *net, link_t *target, modebuf_t *mbuf,
			modeflag mch, int add, char *mask)
{
  const char CMD_MODE[] = "MODE";
  size_t i, m;

  for (m = 0; m < MODECHARSMAX && !(mch & (1<<m)); m++);
  if (m == MODECHARSMAX || mbuf->modechars[m] == 0)
    return; /* oops, illegal mode! */
  if (mbuf->cmd != CMD_MODE || mbuf->changes == net->maxmodes)
  {
    _flush_mode (net, target->chan, mbuf);
    mbuf->cmd = CMD_MODE;
  }
  /* check for user-on-chan mode changes */
  if (mch & (A_ADMIN | A_OP | A_HALFOP | A_VOICE))
  {
    register char *c;

    mask = target->nick->host;
    i = safe_strlen (mask);
    c = memchr (mask, '!', i);
    if (c)
      i = c - mask;
  }
  else
    i = safe_strlen (mask);
  /* check if we need flush */
  if (mbuf->apos && mbuf->apos + i >= sizeof(mbuf->args) - 2)
    _flush_mode (net, target->chan, mbuf);
  if (mbuf->pos && ((add && *mbuf->mchg == '-') || (!add && *mbuf->mchg == '+')))
    _flush_mode (net, target->chan, mbuf);
  /* add change to mbuf */
  if (!mbuf->changes)
  {
    if (add)
      *mbuf->mchg = '+';
    else
      *mbuf->mchg = '-';
    mbuf->pos++;
  }
  mbuf->mchg[mbuf->pos++] = mbuf->modechars[m];
  mbuf->changes++;
  target->lmct = Time;
  if (!i)
    return;
  if (i >= sizeof(mbuf->args) - mbuf->apos) /* it's impossible but anyway */
    i = sizeof(mbuf->args) - mbuf->apos - 1;
  if (mbuf->apos)
    mbuf->args[mbuf->apos++] = ' ';
  memcpy (&mbuf->args[mbuf->apos], mask, i);
  mbuf->apos += i;
}

static void _push_kick (net_t *net, link_t *target, modebuf_t *mbuf,
		        char *reason)
{
  const char CMD_KICK[] = "KICK";
  char *c;
  size_t i, m;

  if (mbuf->cmd != CMD_KICK || mbuf->changes == net->maxtargets)
  {
    _flush_mode (net, target->chan, mbuf);
    mbuf->cmd = CMD_KICK;
  }
  m = safe_strlen (target->nick->host);
  c = memchr (target->nick->host, '!', m);
  if (c)
    m = c - target->nick->host;
  if (mbuf->pos && mbuf->pos + m >= sizeof(mbuf->args) - 2)
    _flush_mode (net, target->chan, mbuf);
  i = safe_strlen (reason);
  if (i >= sizeof(mbuf->args) - 1)
    i = sizeof(mbuf->args) - 2;
  if (mbuf->apos && (i != mbuf->apos - 1 ||
      safe_strncmp (&mbuf->args[1], reason, mbuf->apos - 1)))
    _flush_mode (net, target->chan, mbuf);
  if (!mbuf->apos && i != 0) /* write reason */
  {
    *mbuf->args = ':';
    memcpy (&mbuf->args[1], reason, i);
    mbuf->apos = i + 1;
  }
  if (mbuf->mchg)
    mbuf->mchg[mbuf->pos++] = ',';
  memcpy (&mbuf->mchg[mbuf->pos], target->nick->host, m);
  mbuf->pos += m;
  mbuf->changes++;
}

static void _kickban_user (net_t *net, link_t *target, modebuf_t *mbuf,
			   char *reason)
{
  char mask[HOSTMASKLEN];

  _make_literal_mask (mask, target->nick->host, sizeof(mask));
  _push_mode (net, target, mbuf, A_DENIED, 1, mask);
  _push_kick (net, target, mbuf, reason ? reason : "you are banned");
}

static void _recheck_modes (net_t *net, link_t *target, userflag rf,
			    userflag cf, modebuf_t *mbuf, int firstjoined)
{
  clrec_t *cl;
  char *greeting, *c;
  wtmp_t wtmp;

  /* check server/channel user's op/voice status to take */
  if ((target->mode & A_ADMIN) &&			/* is an admin */
      ((rf & U_DEOP) ||					/* must be deopped */
       (!(rf & U_MASTER) && (cf & U_DEOP))))		/* +bitch */
    _push_mode (net, target, mbuf, A_ADMIN, 0, NULL);
  else if ((target->mode & A_OP) &&			/* is an op */
	   ((rf & U_DEOP) ||				/* must be deopped */
	    (!(rf & U_OP) && (cf & U_DEOP))))		/* +bitch */
    _push_mode (net, target, mbuf, A_OP, 0, NULL);
  else if ((target->mode & A_HALFOP) &&			/* is a halfop */
	   ((rf & U_DEOP) ||				/* must be deopped */
	    (!(rf & U_HALFOP) && (cf & U_DEOP))))	/* +bitch */
    _push_mode (net, target, mbuf, A_HALFOP, 0, NULL);
  else if ((target->mode & A_VOICE) &&			/* is voiced */
	   (rf & U_QUIET))				/* must be quiet */
    _push_mode (net, target, mbuf, A_VOICE, 0, NULL);
  /* check server/channel user's op/voice status to give */
  if (!(target->mode & A_OP) && !(rf & U_DEOP) && (rf & U_OP) &&
      ((rf & U_AUTO) || (cf & U_OP)))			/* +autoop */
    _push_mode (net, target, mbuf, A_OP, 1, NULL);	/* autoop */
  else if (!(target->mode & A_HALFOP) && !(rf & U_DEOP) && (rf & U_HALFOP) &&
	   ((rf & U_AUTO) || (cf & U_OP)) &&		/* +autoop */
	   (net->features & L_HASHALFOP))
    _push_mode (net, target, mbuf, A_HALFOP, 1, NULL);	/* auto-halfop */
  else if (!(target->mode & A_VOICE) && !(rf & U_QUIET) &&
	   ((rf & U_SPEAK) ||
	    ((rf & U_VOICE) && (cf & U_VOICE))))	/* +autovoice */
    _push_mode (net, target, mbuf, A_VOICE, 1, NULL);	/* autovoice */
  /* greeting user if it is first joined after enough absence */
  if (firstjoined && (cf & U_SPEAK) && target->nick->lname &&
      target->chan->id != ID_REM && ircch_greet_time > 0)
  {
    if ((!FindEvent (&wtmp, target->nick->lname, W_ANY, target->chan->id) ||
	 Time - wtmp.time > ircch_greet_time) &&
	(cl = Lock_Clientrecord (target->nick->lname)))
    {
      greeting = safe_strdup (Get_Field (cl, target->chan->chi->name, NULL));
      Unlock_Clientrecord (cl);
      if (greeting)
      {
	if ((c = strchr (target->nick->host, '!')))
	  *c = 0;
	Add_Request (I_SERVICE, target->chan->chi->name, F_T_MESSAGE, "%s: %s",
		     target->nick->host, greeting);
	if (c)
	  *c = '!';
	FREE (&greeting);
      }
    }
  }
}

void ircch_recheck_modes (net_t *net, link_t *target, userflag sf, userflag cf,
			  char *info, int x)
{
  link_t *me;
  userflag rf;
  modebuf_t mbuf;

  if (!target || !(me = _find_me_op (net, target->chan)))
    return;		/* I have not any permissions to operate */
  if (Time - target->lmct < (unsigned long int)ircch_mode_timeout)
    return;		/* don't push too fast modechanges, it will abuse */
  _make_modechars (mbuf.modechars, net);
  mbuf.changes = mbuf.pos = mbuf.apos = 0;
  mbuf.cmd = NULL;
  /* make resulting flags */
  rf = _make_rf (net, sf, cf);
  /* first of all check if user has access: channel and U_ACCESS has priority */
  if (!(rf & U_ACCESS) && (rf & U_DENY))
    _kickban_user (net, target, &mbuf, info);
  else
    _recheck_modes (net, target, rf,
		    Get_Clientflags (target->chan->chi->name, NULL), &mbuf, x);
  _flush_mode (net, target->chan, &mbuf);
}

static void _ircch_expire_exempts (net_t *net, ch_t *ch, modebuf_t *mbuf)
{
  time_t t;
  list_t *list, *list2;
  clrec_t *cl;
  userflag uf, cf;

  /* check exceptions and remove all expired if no matched bans left */
  if (ircch_exempt_keep > 0)
  {
    t = Time - ircch_exempt_keep * 60;
    for (list = ch->exempts; list; list = list->next)
    {
      if (list->since > t)
	continue;
      for (list2 = ch->bans; list2; list2 = list2->next)
	if (match (list2->what, list->what) > 0)	/* there is a ban... */
	  break;
      if (list2)
	continue;					/* ...so keep it */
      if ((cl = Find_Clientrecord (list->what, NULL, &uf, net->name)))
      {
	cf = Get_Flags (cl, ch->chi->name);
	Unlock_Clientrecord (cl);
	if ((uf & (U_ACCESS | U_NOAUTH)) == (U_ACCESS | U_NOAUTH) ||
	    (cf & (U_ACCESS | U_NOAUTH)) == (U_ACCESS | U_NOAUTH))
	  continue;					/* it's sticky */
      }
      Add_Request (I_LOG, ch->chi->name, F_MODES, "Exception %s on %s expired.",
		   list->what, ch->chi->name);
      _push_mode (net, ch->nicks, mbuf, A_EXEMPT, 0, list->what);
    }
  }
}

/* do channel mode change parsing, origin may be NULL if it came from server */
int ircch_parse_modeline (net_t *net, ch_t *chan, link_t *origin, char *prefix,
			  userflag uf, bindtable_t *mbt, bindtable_t *kbt,
			  int parc, char **parv)
{
  link_t *me, *target;
  char *pstr, *schr;
  list_t **list;
  binding_t *bind;
  char mf, mc;
  userflag ocf, orf, trf, gcf;
  modeflag mch;
  int nextpar;
  clrec_t *cr;
  char buf[STRING];
  modebuf_t mbuf;

  nextpar = 0;
  me = _find_me_op (net, chan);
  _make_modechars (mbuf.modechars, net);
  mbuf.changes = mbuf.pos = mbuf.apos = 0;
  mbuf.cmd = NULL;
  if (origin)
    orf = _make_rf (net, uf,
		    (ocf = Get_Clientflags (origin->nick->lname, chan->chi->name)));
  else
    ocf = orf = 0;					/* server nethack? */
  gcf = Get_Clientflags (chan->chi->name, NULL);
  /* parse parameters */
  while (nextpar < parc)
  {
    pstr = parv[nextpar++];
    mf = 0;
    while (*pstr)
    {
      mc = *pstr++;
      list = NULL;
      mch = 0;
      target = origin;
      schr = NULL;
      switch (mc)
      {
	case '-':
	case '+':
	  mf = mc;
	  break;
	case 'b':
	  if (nextpar == parc)			/* oops, no parameter */
	    break;
	  schr = parv[nextpar++];
	  mch = A_DENIED;
	  list = &chan->bans;
	  break;
	case 'e':
	  if (nextpar == parc)			/* oops, no parameter */
	    break;
	  schr = parv[nextpar++];
	  mch = A_EXEMPT;
	  list = &chan->exempts;
	  break;
	case 'I':
	  if (nextpar == parc)			/* oops, no parameter */
	    break;
	  schr = parv[nextpar++];
	  mch = A_INVITED;
	  list = &chan->invites;
	  break;
	case 'k':
	  if (nextpar == parc)			/* oops, no parameter */
	    break;
	  schr = parv[nextpar++];
	  mch = A_KEYSET;
	  break;
	case 'l':
	  if (mf == '+')
	  {
	    if (nextpar == parc)		/* oops, no parameter */
	      break;
	    schr = parv[nextpar++];
	  }
	  mch = A_LIMIT;
	  break;
	default:
	  schr = memchr (mbuf.modechars, mc, sizeof(mbuf.modechars));
	  if (schr)
	    mch = 1 << (schr - mbuf.modechars);
	  else
	    mch = 0;
	  if (mch & (A_ADMIN | A_OP | A_HALFOP | A_VOICE))
	  {
	    if (nextpar == parc)		/* oops, no parameter */
	      mch = 0;
	    else
	      schr = parv[nextpar++];
	  }
	  else
	    schr = NULL;
	  if (schr)
	  {
	    if (net->lc)
	    {
	      net->lc (buf, schr, sizeof(buf));
	      target = ircch_find_link (net, buf, chan);
	    }
	    else
	      target = ircch_find_link (net, schr, chan);
	  }
      }
      if (mc == mf)
	continue;
      else if (!mf || !list || !mch)
      {
	/* bad modechange! */
	continue;
      }
      snprintf (buf, sizeof(buf), "%s %c%c%s%s", chan->chi->name, mf, mc,
		schr ? " " : "", NONULL(schr));
      for (bind = NULL; (bind = Check_Bindtable (mbt, buf, uf, ocf, bind)); )
      {
	if (bind->name)
	  RunBinding (bind, prefix,
		      (origin && origin->nick->lname) ? origin->nick->lname : "*",
		      chan->chi->name, -1, NextWord (buf));
	else
	  bind->func (prefix, origin ? origin->nick->lname : NULL, chan->chi,
		      NextWord (buf));
      }
      if (list)				/* add/remove mask [schr] in [list] */
      {
	if (!(orf & U_OWNER) && mf == '+' && (gcf & U_DENY))
	  _push_mode (net, target, &mbuf, mch, 0, schr);
	if (mf == '-')
	{
	  list_t *item;

	  if ((item = ircch_find_mask (*list, schr)))
	    ircch_remove_mask (list, item);
	  /* if ban was removed then remove all matched dynamic exceptions */
	  if (mc == 'b' && (Get_Clientflags (chan->chi->name, NULL) & U_NOAUTH))
	    _ircch_expire_exempts (net, chan, &mbuf);
	  continue;
	}
	else if (origin)
	{
	  ircch_add_mask (list, origin->nick->host,
			  safe_strlen (origin->nick->name), schr);
	  /* TODO: set matched exceptions while ban is set */
	  if ((gcf & U_AUTO) && chan->tid == -1 && /* start enforcer */
	      ((mc == 'b' && mf == '+') || (mc == 'e' && mf == '-')))
	    chan->tid = NewTimer (chan->chi->ift, chan->chi->name, S_LOCAL,
				  ircch_enforcer_time, 0, 0, 0);
	}
	else
	  ircch_add_mask (list, "", 0, schr);
      }
      else if (mch)
      {
	/* check for modelocks and reverse modechange */
	if (!(orf & U_OWNER))		/* owner may change any modes */
	{
	  if (mf == '-' && (mch & chan->mlock))
	  {
	    if (mch & A_LIMIT)
	    {
	      snprintf (buf, sizeof(buf), "%d", chan->limit);
	      _push_mode (net, target, &mbuf, mch, 1, buf);
	    }
	    else if (mch & A_KEYSET)
	      _push_mode (net, target, &mbuf, mch, 1, chan->key);
	    else
	      _push_mode (net, target, &mbuf, mch, 1, NULL);
	  }
	  else if (mf == '+' && (mch & chan->munlock))
	    _push_mode (net, target, &mbuf, mch, 0, NULL);
	}
	/* apply changes */
	if (mch & A_KEYSET)
	{
	  if (mf == '-')
	    schr = NULL;
	  for (bind = NULL; (bind = Check_Bindtable (mbt, chan->chi->name, uf,
						     ocf, bind)); )
	    if (!bind->name)
	      bind->func (chan->chi->name, prefix,
			  (origin && origin->nick->lname) ? origin->nick->lname : NULL,
			  orf, schr);
	  FREE (&chan->key);
	  chan->key = safe_strdup (schr);
	  cr = Lock_Clientrecord (chan->chi->name);
	  if (cr)			/* oops, key changed! memorize it! */
	  {
	    WARNING ("Key for channel %s was changed!", chan->chi->name);
	    Set_Field (cr, "passwd", chan->key);
	    Unlock_Clientrecord (cr);
	  }
	}
	else if (mch & A_LIMIT)
	{
	  if (mf == '-')
	    chan->limit = 0;
	  else
	    chan->limit = atoi (schr);
	}
	else if (schr)			/* modechange for [schr] on channel */
	{
	  if (mf == '-')
	    target->mode &= ~mch;
	  else
	    target->mode |= mch;
	  if (!(orf & U_MASTER))	/* masters may change modes */
	  {
	    trf = _make_rf (net, Get_Clientflags (target->nick->lname, NULL),
			    Get_Clientflags (target->nick->lname, chan->chi->name));
	    if (mf == '-' && (mch & (A_OP | A_HALFOP)) && (gcf & U_MASTER) &&
		!(gcf & U_DEOP) && !(trf & U_DEOP) &&
		(trf & (U_OP | U_HALFOP | U_FRIEND))) /* protect ops/friends */
	      _push_mode (net, target, &mbuf, mch, 1, NULL);
	    else
	      _recheck_modes (net, target, trf, gcf, &mbuf, 0);
	  }
	}
	else if (mf == '-')		/* modechange for whole channel */
	{
	  chan->mode &= ~mch;
	}
	else
	{
	  chan->mode |= mch;
	}
      }
    }
  }
  _flush_mode (net, chan, &mbuf);
  return nextpar;
}

void ircch_parse_configmodeline (net_t *net, ch_t *chan, char *mode)
{
  char mf, mc;
  modeflag mch;
  char *c;
  modebuf_t mbuf;

  chan->mlock = chan->munlock = 0;		/* reset it at first */
  _make_modechars (mbuf.modechars, net);
  mf = 0;
  while (*mode && *mode != ' ')	/* it may have second parameter - limit */
  {
    mc = *mode++;
    switch (mc)
    {
      case '-':
      case '+':
	mf = mc;
	mch = 0;
	break;
      case 'k':
	mch = A_KEYSET;
	break;
      case 'l':
	mch = A_LIMIT;
	break;
      default:
	c = memchr (mbuf.modechars, mc, sizeof(mbuf.modechars));
	if (c)
	  mch = 1 << (c - mbuf.modechars);
	else
	  mch = 0;
    }
    if (!mch || !mf)
      continue;
    if (mf == '-')		/* modechange for whole channel */
    {
      chan->munlock |= mch;
      chan->mlock &= ~mch;
    }
    else
    {
      chan->mlock |= mch;
      chan->munlock &= ~mch;
    }
  }
  if (chan->mlock & A_KEYSET)
    chan->limit = atoi (NextWord (mode));
  else
    chan->limit = 0;
}

void ircch_enforcer (net_t *net, ch_t *chan)
{
  link_t *link;
  clrec_t *clr;
  userflag rf, cf;
  list_t *ban, *ex;
  modebuf_t mbuf;

  mbuf.changes = mbuf.pos = mbuf.apos = 0;
  mbuf.cmd = NULL;
  cf = Get_Clientflags (chan->chi->name, NULL);
  for (link = chan->nicks; link; link = link->prevnick)
    if (!(link->mode & (A_ADMIN | A_OP | A_HALFOP)) || !(cf & U_FRIEND))
    {
      if ((clr = Lock_Clientrecord (link->nick->lname)))
      {
	rf = _make_rf (net, Get_Flags (clr, net->neti->name),
		       Get_Flags (clr, chan->chi->name));
	Unlock_Clientrecord (clr);
      }
      else
	rf = 0;
      if (rf & (U_FRIEND | U_MASTER | U_OP | U_HALFOP))
	continue;
      for (ban = chan->bans, ex = NULL; ban; ban = ban->next);
	if (match (ban->what, link->nick->host) > 0)
	{
	  for (ex = chan->exempts; ex; ex = ex->next)
	    if (match (ban->what, ex->what) > 0 &&
		match (ex->what, link->nick->host) > 0)
	      break;
	  if (!ex)
	    break;
	}
      if (ban && !ex)
	_push_kick (net, link, &mbuf, "you are banned");
    }
  _flush_mode (net, chan, &mbuf);
}

static void _ircch_expire_bans (net_t *net, ch_t *ch, modebuf_t *mbuf)
{
  time_t t;
  list_t *list;
  clrec_t *cl;
  userflag uf, cf;

  /* check bans and remove all expired (unsticky) */
  if (ircch_ban_keep > 0)
  {
    t = Time - ircch_ban_keep * 60;
    for (list = ch->bans; list; list = list->next)
    {
      if (list->since > t)
	continue;
      if ((cl = Find_Clientrecord (list->what, NULL, &uf, net->name)))
      {
	cf = Get_Flags (cl, ch->chi->name);
	Unlock_Clientrecord (cl);
	if ((uf & (U_DENY | U_NOAUTH)) == (U_DENY | U_NOAUTH) ||
	    (cf & (U_DENY | U_NOAUTH)) == (U_DENY | U_NOAUTH))
	  continue;					/* it's sticky */
      }
      Add_Request (I_LOG, ch->chi->name, F_MODES, "Ban %s on %s expired.",
		   list->what, ch->chi->name);
      _push_mode (net, ch->nicks, mbuf, A_DENIED, 0, list->what);
    }
  }
}

void ircch_expire (net_t *net, ch_t *ch)
{
  modebuf_t mbuf;

  if (!(Get_Clientflags (ch->chi->name, NULL) & U_NOAUTH))	/* +dynamic */
    return;
  mbuf.changes = mbuf.pos = mbuf.apos = 0;
  mbuf.cmd = NULL;
  _ircch_expire_bans (net, ch, &mbuf);
  _ircch_expire_exempts (net, ch, &mbuf);
  if (!(ch->mode & A_INVITEONLY))
  {
    /* TODO: check invites and remove all expired */
  }
  _flush_mode (net, ch, &mbuf);
}


/* --- "ss-irc" bindings ------------------------------------------------------
 *   int func(peer_t *who, INTERFACE *where, char *args);
 */

		/* .adduser [!]nick [lname] */
static int ssirc_adduser (peer_t *who, INTERFACE *where, char *args)
{
  net_t *net;
  char *c, *as;
  link_t *link;
  clrec_t *u;
  int i;
  userflag tf;
  char name[MBNAMEMAX+1];
  char mask[HOSTMASKLEN+1];

  /* find and check target */
  ircch_find_service (where, &net);
  if (!net || !args)
    return 0;
  if (args[0] == '!')
    i = 1;
  else
    i = 0;
  c = strchr (&args[i], ' ');
  if (c)
  {
    as = NextWord (c);
    *c = 0;
  }
  else
    as = &args[i];
  if (net->lc)
  {
    net->lc (name, as, sizeof(name));
    as = name;
    net->lc (mask, &args[i], sizeof(mask));
    link = ircch_find_link (net, mask, NULL);
  }
  else
    link = ircch_find_link (net, &args[i], NULL);
  if (link && link->nick->lname)
  {
    New_Request (who->iface, 0, _("adduser: %s is already known as %s."),
		 &args[i], link->nick->lname);
    if (c)
      *c = ' ';
    return 0;
  }
  if (link && !link->nick->host)
  {
    New_Request (who->iface, 0, _("Could not find host of %s."), &args[i]);
    if (c)
      *c = ' ';
    return 0;
  }
  if (c)
    *c = ' ';
  if (!link)
    return 0;
  if (i == 1)
    _make_literal_mask (mask, link->nick->host, sizeof(mask));
  else
    _make_mask (mask, link->nick->host, sizeof(mask));
  /* add mask (mask) to target (as) */
  u = Lock_Clientrecord (as);
  if (u)	/* existing client */
  {
    tf = Get_Flags (u, NULL);
    /* master may change only own or not-masters masks */
    if (!(tf & (U_MASTER | U_OWNER)) || (who->uf & U_OWNER) ||
	!safe_strcmp (as, who->iface->name))
      i = Add_Mask (u, mask);
    else
    {
      New_Request (who->iface, 0, _("Permission denied."));
      i = 1;	/* report it to log anyway */
    }
    Unlock_Clientrecord (u);
  }
  else		/* new clientrecord with no flags and no password */
  {
    i = Add_Clientrecord (as, mask, U_ANY);
  }
  return i;	/* nothing more, all rest will be done on next users's event */
}

		/* .deluser [!]nick */
static int ssirc_deluser (peer_t *who, INTERFACE *where, char *args)
{
  net_t *net;
  link_t *link;
  ch_t *ch;
  clrec_t *u;
  int i;
  userflag tf;
  char mask[HOSTMASKLEN+1];

  /* find and check target */
  ch = ircch_find_service (where, &net);
  if (!net || !args)
    return 0;
  if (args[0] == '!')
    i = 1;
  else
    i = 0;
  if (net->lc)
  {
    net->lc (mask, &args[i], sizeof(mask));
    link = ircch_find_link (net, mask, ch);
  }
  else
    link = ircch_find_link (net, &args[i], ch);
  if (!link)
  {
    New_Request (who->iface, 0, _("Could not find nick %s."), &args[i]);
    return 0;
  }
  if (!link->nick->lname)
  {
    New_Request (who->iface, 0, _("deluser: %s isn't registered."), &args[i]);
    return 0;
  }
  if (!link->nick->host)
  {
    New_Request (who->iface, 0, _("Could not find host of %s."), &args[i]);
    return 0;
  }
  if (i == 1)
    _make_literal_mask (mask, link->nick->host, sizeof(mask));
  else
    _make_mask (mask, link->nick->host, sizeof(mask));
  /* delete mask (mask) from target (as) */
  u = Lock_Clientrecord (link->nick->lname);
  if (u)	/* existing client */
  {
    tf = Get_Flags (u, NULL) | Get_Flags (u, ch->chi->name);
    /* master may change only own or not-masters masks */
    if (!(tf & (U_MASTER | U_OWNER)) || (who->uf & U_OWNER) ||
	!safe_strcmp (link->nick->lname, who->iface->name))
      i = Delete_Mask (u, mask);
    else
      New_Request (who->iface, 0, _("Permission denied."));
    Unlock_Clientrecord (u);
    if (i == -1) /* it was last host */
      Delete_Clientrecord (link->nick->lname);
  }
  else		/* oops! */
  {
    WARNING ("irc:deluser: Lname %s is lost", link->nick->lname);
  }
  return 1;	/* nothing more, all rest will be done on next users's event */
}

static int _ssirc_say (peer_t *who, INTERFACE *where, char *args, flag_t type)
{
  char target[IFNAMEMAX+1];
  char *c = NULL;

  if (!args)
    return 0;
  if (strchr ("#&!+", args[0]))
  {
    args = NextWord_Unquoted (target, args, sizeof(target));
    if (!strchr (target, '@') && !(c = strrchr (where->name, '@')))
    {
      strfcat (target, "@", sizeof(target));
      c = where->name;
    }
    if (c)
      strfcat (target, c, sizeof(target));
    Add_Request (I_SERVICE, target, type, "%s", args);
  }
  else if (!strchr (where->name, '@'))
    return 0;
  else
    New_Request (where, type, "%s", args);
  return 1;
}

		/* .say [channel[@net]] text... */
static int ssirc_say (peer_t *who, INTERFACE *where, char *args)
{
  return _ssirc_say (who, where, args, F_T_MESSAGE);
}

		/* .act [channel[@net]] text... */
static int ssirc_act (peer_t *who, INTERFACE *where, char *args)
{
  return _ssirc_say (who, where, args, F_T_ACTION);
}

		/* .notice target[@net] text... */
static int ssirc_notice (peer_t *who, INTERFACE *where, char *args)
{
  char target[IFNAMEMAX+1];
  char *c = NULL;

  if (!args)
    return 0;
  args = NextWord_Unquoted (target, args, sizeof(target));
  if (!strchr (target, '@') && !(c = strrchr (where->name, '@')))
  {
    strfcat (target, "@", sizeof(target));
    c = where->name;
  }
  if (c)
    strfcat (target, c, sizeof(target));
  Add_Request (I_SERVICE, target, F_T_NOTICE, "%s", args);
  return 1;
}

		/* .ctcp target[@net] text... */
static int ssirc_ctcp (peer_t *who, INTERFACE *where, char *args)
{
  char target[IFNAMEMAX+1];
  char cmd[CTCPCMDMAX+1];
  char *c = NULL;

  if (!args)
    return 0;
  args = NextWord_Unquoted (target, args, sizeof(target));
  if (!strchr (target, '@') && !(c = strrchr (where->name, '@')))
  {
    strfcat (target, "@", sizeof(target));
    c = where->name;
  }
  if (c)
    strfcat (target, c, sizeof(target));
  for (c = cmd; c < &cmd[sizeof(cmd)-1] && *args && *args != ' '; args++)
    *c++ = toupper (((uchar *)args)[0]);
  *c = 0;
  Add_Request (I_SERVICE, target, F_T_CTCP, "%s%s", cmd, args);
  return 1;
}

		/* .topic [channel[@net]] text... */
static int ssirc_topic (peer_t *who, INTERFACE *where, char *args)
{
  char target[IFNAMEMAX+1];
  char net[IFNAMEMAX+1];
  char *c = NULL;

  if (!args)
    args = "";
  if (strchr ("#&!+", args[0]))
  {
    args = NextWord_Unquoted (target, args, sizeof(target));
    if ((c = strrchr (target, '@')))
    {
      strfcpy (net, c, sizeof(net));
      *c = 0;
      c = net;
    }
    else if (!(c = strrchr (where->name, '@')))
      return 0;					/* cannot determine network */
  }
  else if (!(c = strrchr (where->name, '@')))
    return 0;					/* cannot determine network */
  else if (c - (char *)where->name >= sizeof(target))
    strfcpy (target, where->name, sizeof(target)); /* it's impossible! */
  else
    strfcpy (target, where->name, (c - (char *)where->name) + 1);
  Add_Request (I_SERVICE, c, 0, "TOPIC %s :%s", target, args);
  return 1;
}

/*
  {"reset",		"m|m",	(Function) cmd_reset,		NULL},
  {"resetbans",		"o|o",	(Function) cmd_resetbans,	NULL},
  {"resetexempts",	"o|o",	(Function) cmd_resetexempts,	NULL},
  {"resetinvites",	"o|o",	(Function) cmd_resetinvites,	NULL},
?  {"msg",		"S",	(Function) cmd_msg,		NULL},
  {"voice",		"h|h",	(Function) cmd_voice,		NULL},
  {"devoice",		"h|h",	(Function) cmd_devoice,		NULL},
  {"op",		"o|o",	(Function) cmd_op,		NULL},
  {"deop",		"o|o",	(Function) cmd_deop,		NULL},
  hop/dehop
  {"invite",		"h|h",	(Function) cmd_invite,		NULL},
  {"kick",		"o|o",	(Function) cmd_kick,		NULL},
  {"kickban",		"o|o",	(Function) cmd_kickban,		NULL},
*/

void ircch_set_ss (void)
{
#define NB(a,b,c) Add_Binding ("ss-irc", #a, b, c, &ssirc_##a)
  NB (adduser,	U_MASTER, U_MASTER);
  NB (deluser,	U_MASTER, U_MASTER);
  NB (say,	U_SPEAK, U_SPEAK);
  NB (act,	U_SPEAK, U_SPEAK);
  NB (ctcp,	U_SPEAK, U_SPEAK);
  NB (notice,	U_SPEAK, U_SPEAK);
  NB (topic,	U_SPEAK, U_SPEAK);
#undef NB
}

void ircch_unset_ss (void)
{
#define NB(a) Delete_Binding ("ss-irc", &ssirc_##a)
  NB (adduser);
  NB (deluser);
  NB (say);
  NB (act);
  NB (ctcp);
  NB (notice);
  NB (topic);
#undef NB
}
