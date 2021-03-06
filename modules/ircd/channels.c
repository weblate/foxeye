/*
 * Copyright (C) 2010-2020  Andrej N. Gritsenko <andrej@rep.kiev.ua>
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
 *     You should have received a copy of the GNU General Public License along
 *     with this program; if not, write to the Free Software Foundation, Inc.,
 *     51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * This file is a part of FoxEye IRCd module: channels management (RFC2811).
 */

#include <foxeye.h>
#if IRCD_USES_ICONV == 0 || (defined(HAVE_ICONV) && (IRCD_NEEDS_TRANSLIT == 0 || defined(HAVE_CYRILLIC_TRANSLIT)))
#include <modules.h>
#include <init.h>
#include <conversion.h>

#include <wchar.h>

#include "ircd.h"
#include "numerics.h"

extern long int _ircd_hold_period;	/* see ircd.c */
extern long int _ircd_max_bans;
extern long int _ircd_max_channels;
extern bool _ircd_no_spare_invites;
extern bool _ircd_strict_modecmd;
extern bool _ircd_ignore_mkey_arg;

extern unsigned int _ircd_nicklen;

static struct bindtable_t *BTIrcdWhochar;
static struct bindtable_t *BTIrcdChannel;
static struct bindtable_t *BTIrcdModechange;
static struct bindtable_t *BTIrcdUmodechange;
static struct bindtable_t *BTIrcdCheckModechange;
static struct bindtable_t *BTIrcdIsupport;
//static struct bindtable_t *BTIrcdSetMember;
//static struct bindtable_t *BTIrcdLostMember;
static struct bindtable_t *BTIrcdEOB;

ALLOCATABLE_TYPE (CHANNEL, IrcdChan_, users)
ALLOCATABLE_TYPE (MEMBER, IrcdMemb_, prevnick)
ALLOCATABLE_TYPE (MASK, IrcdMask_, next)
ALLOCATABLE_TYPE (CMASKL, IrcdMaskL_, next)

/* list of translations MODE char into WHO char - has to be equal lenghts! */
static char Ircd_modechar_list[]  = "ohvaqO!"; /* list of known '+? nick' modes */
static char Ircd_whochar_list[16] = "       "; /* appropriate WHO chars */

static modeflag Ircd_modechar_mask = 0; /* filled with matching to above */

/* these three should have equal size */
static char _ircd_umodes[32]; /* 1 char per modeflag bit */
static char _ircd_cmodes[32]; /* 1 char per modeflag bit */
static char _ircd_wmodes[32]; /* 1 char per modeflag bit */

static char _ircd_isupport_modestring[128]; /* should be enough for a-Z with commas */

/* prototypes for too complex functions not easy castable */
#define static
typedef BINDING_TYPE_ircd_modechange ((*_mch_func_t));
typedef BINDING_TYPE_ircd_whochar ((*_wch_func_t));
#undef static


/* ---------------------------------------------------------------------------
 * Common functions.
 */

#define _ircd_find_channel_lc(ircd,lcname) Find_Key ((ircd)->channels, lcname)

static inline CHANNEL *_ircd_find_channel (IRCD *ircd, const char *name)
{
  char lcname[MB_LEN_MAX*CHANNAMELEN+1];

  unistrlower (lcname, name, sizeof(lcname));
  return _ircd_find_channel_lc (ircd, lcname);
}

#define REPLACE_NEXT_CHANNEL_CHAR {\
  if (*text_replace_char) \
    *ds++ = *text_replace_char; \
  ss++; \
  sz--; \
  continue; }

/* checks if name is valid and replace invalid chars */
static void _ircd_validate_channel_name (char *chname)
{
  /* check name for consistency:
     non-strict: no " \007\r\n,"
     strict: validate to CHARSET_8BIT compatibility */
  size_t sz, sp;
  ssize_t sc;
  wchar_t wc;
  mbstate_t ps;
  char *ds, *ss;
#if IRCD_STRICT_NAMES
  char *os;
  struct conversion_t *conv;
  char namebuf[CHANNAMELEN+1];
#endif

  dprint(5, "ircd:channels.c:_ircd_validate_channel_name: %s", chname);
  sz = safe_strlen (chname);
  if (sz == 0)				/* oops! */
    return;
  ds = ss = chname;
#if IRCD_STRICT_NAMES
  conv = Get_Conversion (CHARSET_8BIT);
  os = namebuf;				/* convert to CHARSET_8BIT */
  sc = sz;
  sp = Undo_Conversion (conv, &os, sizeof(namebuf), chname, &sz);
  sz = Do_Conversion (conv, &ss, sc, os, &sp); /* convert it back */
  ss[sz] = '\0';			/* terminate the string in any case */
  Free_Conversion (conv); /* ok, we got chname compatible with CHARSET_8BIT */
#endif
  memset(&ps, 0, sizeof(ps)); /* reset the state */
  for (sp = 0; *ss; sp++)
  {
    if (sp >= CHANNAMELEN)		/* name is too long */
      break;
    if (strchr ("\007\r\n,", *ss))	/* non-allowed chars */
      REPLACE_NEXT_CHANNEL_CHAR
    sc = mbrtowc (&wc, ss, sz, &ps);
    if (sc <= 0)			/* invalid sequence */
      REPLACE_NEXT_CHANNEL_CHAR
    if (!iswgraph (wc))			/* unprintable character */
    {
      if (*text_replace_char)
	*ds++ = *text_replace_char;
    }
    else				/* copy character */
    {
      if (ds != ss)
	memcpy (ds, ss, sc);
      ds += sc;
    }
    ss += sc;				/* go to next char */
    sz -= sc;
  }
  *ds = '\0';				/* terminate the string */
}

/* creates new empty CHANNEL */
static inline CHANNEL *_ircd_new_channel (IRCD *ircd, const char *name,
					  const char *lcname)
{
  CHANNEL *ch = alloc_CHANNEL();

  strfcpy (ch->name, name, sizeof(ch->name));
  _ircd_validate_channel_name (ch->name);
  strfcpy (ch->lcname, lcname, sizeof(ch->lcname));
  ch->users = ch->creator = ch->invited = NULL;
  ch->count = 0;
  ch->masks = NULL;
  ch->hold_upto = ch->noop_since = 0;
  ch->limit = 0;
  ch->fc[0] = *name;
  ch->fc[1] = '\0';
  ch->topic[0] = '\0';
  ch->key[0] = '\0';
  ch->mode = 0;
#if IRCD_MULTICONNECT
  ch->on_ack = 0;
#endif
#if TOPICWHOTIME
  ch->topic_by[0] = '\0';
  ch->topic_since = 0;
#endif
  if (Insert_Key (&ircd->channels, ch->lcname, ch, 1))
    ERROR("ircd:_ircd_new_channel: tree error on adding %s", ch->lcname);
    //TODO: isn't it fatal?
  else
    dprint(2, "ircd:channels.c:_ircd_new_channel: add chan %s", ch->lcname);
  return ch;
}

static inline MEMBER *_ircd_is_on_channel (CLIENT *cl, CHANNEL *ch)
{
  register MEMBER *m;

  for (m = ch->users; m; m = m->prevnick)
    if (m->who == cl)
      break;
  return m;
}

/* args: Ircd, buf, umode, sizeof(buf); returns buf */
static inline size_t _ircd_make_Xmode (const char *modeslist, char *buf,
				       modeflag umode, size_t bufsize)
{
  register size_t i, s;
  register modeflag um;

  bufsize--;				/* make a reserve for '\0' */
  for (i = 0, um = 1, s = 0; i < sizeof(_ircd_umodes); i++, um <<= 1)
    if ((umode & um) && modeslist[i])
    {
      buf[s++] = modeslist[i];
      if (s >= bufsize)			/* buffer is filled out */
	break;
    }
  buf[s] = '\0';
  return s;
}

#define _ircd_make_wmode(a,b,c) _ircd_make_Xmode (_ircd_wmodes, a,b,c)
#define _ircd_mode2cmode(a,b,c) _ircd_make_Xmode (_ircd_cmodes, a,b,c)

static inline char *_ircd_make_cmode (char *buf, size_t bs, CHANNEL *ch, int show)
{
  register size_t i;

  i = _ircd_make_Xmode (_ircd_cmodes, buf, ch->mode, bs); /* add bool modes */
  if (i < bs - 3 && ch->limit)		/* reserve for "l X" */
    buf[i++] = 'l';
  if (i < bs - 3 && ch->key[0])		/* reserve for "k X" */
    buf[i++] = 'k';
  if (!show) {
    buf[i] = '\0';
    return buf;
  }
  if (ch->limit)			/* it's checked above */
    i += snprintf (&buf[i], bs - i, " %hu", ch->limit); /* add limit */
  if (i < bs - 2 && ch->key[0])		/* reserve for " X" */
    snprintf (&buf[i], bs - i, " %s", ch->key); /* add key */
  return buf;
}

static void _ircd_del_from_invited (MEMBER *memb)
{
  register MEMBER **m;

  dprint(5, "ircd:channels.c:_ircd_del_from_invited: %s on %s", memb->who->nick,
	 memb->chan->name);
  for (m = &memb->who->via->i.nvited; *m && *m != memb; m = &(*m)->prevchan);
  if (*m)				/* remove channel from user */
    *m = memb->prevchan;
  else
    ERROR ("ircd:ircd_del_from_invited: not found channel %s on %s",
	   memb->chan->name, memb->who->nick);
  for (m = &memb->chan->invited; *m && *m != memb; m = &(*m)->prevnick);
  if (*m)				/* remove user from channel */
    *m = memb->prevnick;
  else
    ERROR ("ircd:ircd_del_from_invited: not found %s on channel %s",
	   memb->who->nick, memb->chan->name);
  free_MEMBER (memb);
}

static MASK** _get_chanmasklist(CHANNEL *ch, char mch)
{
  CMASKL *l, **ptr = &ch->masks;

  while ((l = *ptr))
    if (l->mch == mch)
      return &l->list;
    else
      ptr = &l->next;
  *ptr = l = alloc_CMASKL();
  l->mch = mch;
  l->list = NULL;
  l->next = NULL;
  return &l->list;
}


/* ---------------------------------------------------------------------------
 * Data-independent bindtables.
 */

/* "ircd-whochar" bindings */
BINDING_TYPE_ircd_whochar(iwc_ircd);
static char iwc_ircd (char tc)
{
  switch (tc)
  {
    case 'o':	/* channel operator */
      return '@';
    case 'v':	/* has a voice */
      return '+';
    default:
      return 0;
  }
}

/* Bindings for adding/removing users on different types of channel */
BINDING_TYPE_ircd_channel(ich_normal); /* & and # channels */
static modeflag ich_normal(INTERFACE *u, modeflag umode, modeflag chmode,
			   int count, const char *chname, NODE *cl,
			   const char **tocreate)
{
  if (!tocreate)			/* user parts the channel */
  {
    if (count == 1)
      return 0;				/* destroying the channel */
    return chmode;
  }
  if (!count)				/* doesn't exist yet */
  {
    if (chmode)
      return 0;				/* it's on hold now */
    *tocreate = chname;
    if (*chname == '&')			/* it's local channel */
      return (A_ISON | A_OP | A_INVISIBLE);
    return (A_ISON | A_OP);		/* default mode on creating */
  }
  return A_ISON;			/* default mode for joining */
}

BINDING_TYPE_ircd_channel(ich_add); /* + channels */
static modeflag ich_add(INTERFACE *u, modeflag umode, modeflag chmode,
			int count, const char *chname, NODE *cl,
			const char **tocreate)
{
  if (!tocreate)			/* user parts the channel */
  {
    if (count == 1)
      return 0;				/* destroying the channel */
    return chmode;
  }
  if (!count)				/* doesn't exist yet */
  {
    if (chmode)
      return 0;				/* it's on hold now */
    *tocreate = chname;
  }
  return (A_ISON | A_TOPICLOCK);	/* no-mode channel type */
}

/* checks if !?????name exists and return full name if it does */
static inline const char *_ich_safename_exists (NODE *n, const char *name)
{
  char lcname[MB_LEN_MAX*CHANNAMELEN];
  LEAF *l;
  const char *k;
  register size_t i;

  unistrlower (lcname, name, sizeof(lcname)); /* lower case part here */
  i = unistrcut(lcname, sizeof(lcname), CHANNAMELEN - CHIDLEN - 1);
  lcname[i] = '\0';
  /* scan whole list for !* channels (it may be slow, I know...) */
  l = Find_Leaf (n, "!", 0);		/* seek to first "!xxx" channel */
  if (!l)				/* there is no '!' channels yet */
    return NULL;
  k = Leaf_Key (l);			/* get key for first found leaf */
  do {
    if (k[0] != '!')			/* no more ! channels */
      return NULL;
    if (!strcmp (lcname, &k[CHIDLEN+1])) /* skip id part and compare it */
      return ((CHANNEL *)l->s.data)->name;
    l = Next_Leaf (n, l, &k);		/* get next channel and key */
  } while (l);
  return NULL;
}

#define CHIDCNUM 36		/* number of chars in alphabet below */
static char _ircd_chid_char[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";

/* makes !?????name in static buffer and returns it */
static inline char *_ich_make_safename (const char *name)
{
  static char nn[MB_LEN_MAX*CHANNAMELEN+1];
  size_t i;
  time_t t = Time;

  nn[0] = '!';				/* start with '!' */
  for (i = 1; i <= CHIDLEN; i++)	/* generate id part */
  {
    nn[i] = _ircd_chid_char[t % CHIDCNUM];
    t /= CHIDCNUM;
  }
  strfcpy (&nn[i], name, sizeof(nn) - 1 - CHIDLEN); /* add name part */
  i = unistrcut(nn, sizeof(nn), CHANNAMELEN);
  nn[i] = '\0';
  return nn;
}

BINDING_TYPE_ircd_channel(ich_excl); /* ! channels */
static modeflag ich_excl(INTERFACE *u, modeflag umode, modeflag chmode,
			 int count, const char *chname, NODE *cl,
			 const char **tocreate)
{
  if (!tocreate)			/* user parts the channel */
  {
    if (count == 1)
      return 0;				/* destroying the channel */
    return chmode;
  }
  if (chmode == 0)			/* requested to create */
  {
    if (chname[1] != '!')
    {
      *tocreate = _ich_safename_exists (cl, &chname[1]);
      if (*tocreate)		/* it could be attempt to join by short name */
	return A_ISON;			/* allowed to join found channel */
      return 0;				/* cannot create such way */
    }
    if (chname[2] == '!' || /* creating !<id>! conflicts with joining !<id>! */
	_ich_safename_exists (cl, &chname[2])) /* cannot duplicate short name */
      return 0;
    *tocreate = _ich_make_safename (&chname[2]);
    return (A_ISON | A_ADMIN);		/* default mode on creating */
  }
  /* chmode != 0 is OK as there is no 'unavailable' state for safe channels */
  return A_ISON;			/* default mode for joining */
}


/* "ircd-modechange" */

BINDING_TYPE_ircd_modechange(imch_o);
static modeflag imch_o(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target)
    return 0;
  if (!rchmode)
    return A_OP;
  if (tmode & A_ADMIN) {
    if (rchmode & A_ADMIN)
      return A_ADMIN;			/* +-o on admin will do +-O */
    return A_PINGED; /* else: mark of ERR_UNIQOPPRIVSNEEDED */
  } else if (rchmode & (A_ADMIN | A_OP))
    return A_OP;
  return 0;
}

BINDING_TYPE_ircd_modechange(imch_v);
static modeflag imch_v(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (target && ((rchmode & (A_OP | A_ADMIN)) || !rchmode))
    return A_VOICE;
  return 0;
}

BINDING_TYPE_ircd_modechange(imch_a);
static modeflag imch_a(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && !rchmode) /* it's testing */
    return A_ANONYMOUS;
  if (target || !(rchmode & (A_OP | A_ADMIN)))
    return 0;
  if (chtype == '&')			/* & channel */
    return A_ANONYMOUS;
  if (chtype != '!')			/* wrong mode */
    return 0;
  if ((rchmode & A_ADMIN) && add)	/* creator on ! channel */
    return A_ANONYMOUS;
  return A_PINGED; /* mark of ERR_UNIQOPPRIVSNEEDED */
}

BINDING_TYPE_ircd_modechange(imch_i);
static modeflag imch_i(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && ((rchmode & (A_OP | A_ADMIN)) || !rchmode))
    return A_INVITEONLY;
  return 0;
}

BINDING_TYPE_ircd_modechange(imch_m);
static modeflag imch_m(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && ((rchmode & (A_OP | A_ADMIN)) || !rchmode))
    return A_MODERATED;
  return 0;
}

BINDING_TYPE_ircd_modechange(imch_n);
static modeflag imch_n(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && ((rchmode & (A_OP | A_ADMIN)) || !rchmode))
    return A_NOOUTSIDE;
  return 0;
}

BINDING_TYPE_ircd_modechange(imch_q);
static modeflag imch_q(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && !rchmode) /* it's testing */
    return A_QUIET;
  return 0;				/* immutable */
}

BINDING_TYPE_ircd_modechange(imch_p);
static modeflag imch_p(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && !rchmode) /* it's testing */
    return A_PRIVATE;
  if (!target && (rchmode & (A_OP | A_ADMIN)) && !(tmode & A_SECRET))
    return A_PRIVATE;
  return 0;
}

BINDING_TYPE_ircd_modechange(imch_s);
static modeflag imch_s(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && !rchmode) /* it's testing */
    return A_SECRET;
  if (!target && (rchmode & (A_OP | A_ADMIN)) && !(tmode & A_PRIVATE))
    return A_SECRET;
  return 0;
}

BINDING_TYPE_ircd_modechange(imch_r);
static modeflag imch_r(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && !rchmode) /* it's testing */
    return A_REOP;
  if (target || chtype != '!')
    return (0);
  if (rchmode & A_ADMIN)		/* creator on ! channel */
    return A_REOP;
  return A_PINGED; /* mark of ERR_UNIQOPPRIVSNEEDED */
}

BINDING_TYPE_ircd_modechange(imch_t);
static modeflag imch_t(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && ((rchmode & (A_OP | A_ADMIN)) || !rchmode))
    return A_TOPICLOCK;
  return 0;
}

/* this is used to make sending numerics faster in _imch_do_* */
static CLIENT *_imch_client;
/* dummy client structure for cancelling overwritten masks */
static CHANNEL _imch_cancel = { .masks = NULL };

static int _imch_do_keyset (INTERFACE *srv, const char *rq, const char *ch,
			    int add, const char **param,
			    MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd)
{
  if (add < 0)
    return 0; /* invalid query */
  else if (add) {
    size_t len = unistrcut (*param, sizeof(chd->key), KEYLEN);

    if (len > safe_strlen(*param))
      goto _error; /* ERR_KEYSET */
    strfcpy (chd->key, *param, len + 1);
  } else if (!_ircd_ignore_mkey_arg && safe_strcmp(chd->key, *param)) {
_error:
    ircd_do_cnumeric (_imch_client, ERR_KEYSET, chd, 0, NULL);
    return 0;
  } else
    chd->key[0] = '\0';
  return 1;
}

BINDING_TYPE_ircd_modechange(imch_k);
static modeflag imch_k(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && ((rchmode & (A_OP | A_ADMIN)) || !rchmode))
  {
    *ma = &_imch_do_keyset;
    return (A_KEYSET | 1);
  }
  return (1);
}

static int _imch_do_limit (INTERFACE *srv, const char *rq, const char *ch,
			   int add, const char **param,
			   MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd)
{
  register int i;

  if (add < 0)
    return 0; /* invalid query */
  else if (add) {
    i = atoi (*param);
    if (i < 1) {
      i = 1;				/* 1 means nobody else can join */
      *param = "1";
    }
    chd->limit = i;
  } else if (chd->limit == 0) /* already reset */
    return (-1);
  else
    chd->limit = 0;
  return 1;
}

BINDING_TYPE_ircd_modechange(imch_l);
static modeflag imch_l(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && ((rchmode & (A_OP | A_ADMIN)) || !rchmode))
  {
    *ma = &_imch_do_limit;
    return (A_LIMIT | (add ? 1 : 0));
  }
  return (add ? 1 : 0);
}

static int _imch_add_mask (CHANNEL *ch, const char **ptr,
			   int num, const char *txt, char mchar)
{
  register MASK *mm;
  const char *mask = *ptr;
  const char *ex, *at;
  register size_t sz;
  MASK *nm;
  long int cnt;
  MASK **list = _get_chanmasklist(ch, mchar);
  MASK **cancel;

  dprint(5, "ircd:channels.c:_imch_add_mask: '%c' %s", mchar, txt);
  nm = alloc_MASK();
  if ((ex = strchr(mask, '!')) == NULL &&
      (at = strchr(mask, '@')) == NULL) { /* it's just nick */
    /* first valid mask is [^!@]{1,NICKLEN}, adding "!*@*" after */
    sz = unistrcut(mask, (sizeof(nm->what) - 4), _ircd_nicklen);
    unistrlower (nm->what, mask, sz + 1);
    strfcat(nm->what, "!*@*", sizeof(nm->what));
    *ptr = nm->what;
  } else if (ex == NULL && at[1] != '\0') {
    /* second valid mask is [^!]{1,IDENTLEN}@.+, adding "*!" before */
    sz = at - mask;
    if (sz > (sizeof(nm->what) - 4))
      sz = (sizeof(nm->what) - 4);
    sz = unistrcut(mask, sz + 1, IDENTLEN);
    nm->what[0] = '*';
    nm->what[1] = '!';
    sz = unistrlower(&nm->what[2], mask, sz + 1);
    sz += 2;
    unistrlower(&nm->what[sz], at, sizeof(nm->what) - sz);
    *ptr = nm->what;
  } else if (ex != NULL && ex != mask &&
	     (at = strchr(ex, '@')) != NULL && at[1] != '\0') {
    /* third valid mask is .+!.{1,IDENTLEN}@.+ */
    //FIXME: normalize it to [NICKLEN]![IDENTLEN]@[HOSTLEN]
    unistrlower (nm->what, mask, sizeof(nm->what));
  } else {
    /* any other mask is error */
    snprintf(nm->what, sizeof(nm->what), "%c :Invalid mask", mchar);
    ircd_do_cnumeric(_imch_client, ERR_BANLISTFULL, ch, 0, nm->what);
    free_MASK(nm);
    return 0;
  }
  /* note: it might exceed field size? */
  mask = nm->what;
  cnt = 0;
  while (*list)
    if (strcmp(mask, (*list)->what) == 0) { /* duplicate mask */
      free_MASK(nm);
      if (!CLIENT_IS_SERVER(_imch_client))
	ircd_do_cnumeric(_imch_client, num, txt, ch, 0, (*list)->what);
      return (-1);
    } else if (simple_match (mask, (*list)->what) > 0) { /* it eats that one */
      mm = *list;
      *list = mm->next;
      cancel = _get_chanmasklist(&_imch_cancel, mchar);
      mm->next = *cancel;	/* move it to cancellation list */
      *cancel = mm;
    } else if (simple_match ((*list)->what, mask) > 0) { /* that one eats it */
      free_MASK (nm);
      if (!CLIENT_IS_SERVER(_imch_client))
	ircd_do_cnumeric(_imch_client, num, txt, ch, 0, (*list)->what);
      return 0;
    } else {
      list = &(*list)->next;
      cnt++;
    }
  /* we are ready to add it, let test if it's not too much */
  if (cnt >= _ircd_max_bans) {
    if (!CLIENT_IS_SERVER(_imch_client)) {
      ircd_do_cnumeric(_imch_client, ERR_BANLISTFULL, ch, 0, nm->what);
      free_MASK (nm);
      return 0;
    } else
      WARNING("ircd:_imch_add_mask: too many bans on %s: %ld >= %ld",
	      ch->name, cnt, _ircd_max_bans);
  }
  *list = nm;
  nm->next = NULL;
  return 1; /* done */
}

static int _imch_del_mask (CHANNEL *ch, const char **mask, char mchar)
{
  register MASK *mm;
  register CMASKL **cl = &ch->masks;
  MASK **list;
  char what[HOSTMASKLEN+1];

  dprint(5, "ircd:channels.c:_imch_del_mask: %s", *mask);
  while (*cl)
    if ((*cl)->mch == mchar)
      break;
  if (!*cl)
    return 0; /* no such list */
  list = &(*cl)->list;
  unistrlower (what, *mask, sizeof(what));
  while ((mm = *list))
    if (!strcmp (mm->what, what))
    {
      *list = mm->next;
      free_MASK (mm);
      return 1; /* done */
    }
    else
      list = &mm->next;
  return 0; /* not found */
}

static int _imch_do_banset (INTERFACE *srv, const char *rq, const char *ch,
			    int add, const char **param,
			    MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd)
{
  if (add < 0)				/* query ban list */
  {
    MASK *m;

    for (m = BANS(chd); m; m = m->next)
      ircd_do_cnumeric (_imch_client, RPL_BANLIST, chd, 0, m->what);
    ircd_do_cnumeric (_imch_client, RPL_ENDOFBANLIST, chd, 0, NULL);
    return 1;
  }
  else if (add)
    return _imch_add_mask (chd, param, RPL_BANLIST, 'b');
  else
    return _imch_del_mask (chd, param, 'b');
}

BINDING_TYPE_ircd_modechange(imch_b);
static modeflag imch_b(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && ((rchmode & (A_OP | A_ADMIN)) || !rchmode || add < 0))
  {
    *ma = &_imch_do_banset;
    return (A_DENIED | 1);
  }
  return 1;
}

static int _imch_do_exemptset (INTERFACE *srv, const char *rq, const char *ch,
			       int add, const char **param,
			       MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd)
{
  if (add < 0)				/* query exempts list */
  {
    MASK *m;

    for (m = EXEMPTS(chd); m; m = m->next)
      ircd_do_cnumeric (_imch_client, RPL_EXCEPTLIST, chd, 0, m->what);
    ircd_do_cnumeric (_imch_client, RPL_ENDOFEXCEPTLIST, chd, 0, NULL);
    return 1;
  }
  else if (add)
    return _imch_add_mask (chd, param, RPL_EXCEPTLIST, 'e');
  else
    return _imch_del_mask (chd, param, 'e');
}

BINDING_TYPE_ircd_modechange(imch_e);
static modeflag imch_e(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && ((rchmode & (A_OP | A_ADMIN)) || !rchmode || add < 0))
  {
    *ma = &_imch_do_exemptset;
    return (A_EXEMPT | 1);
  }
  return 1;
}

static int _imch_do_inviteset (INTERFACE *srv, const char *rq, const char *ch,
			       int add, const char **param,
			       MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd)
{
  if (add < 0)				/* query invite list */
  {
    MASK *m;

    for (m = INVITES(chd); m; m = m->next)
      ircd_do_cnumeric (_imch_client, RPL_INVITELIST, chd, 0, m->what);
    ircd_do_cnumeric (_imch_client, RPL_ENDOFINVITELIST, chd, 0, NULL);
    return 1;
  }
  else if (add)
    return _imch_add_mask (chd, param, RPL_INVITELIST, 'I');
  else
    return _imch_del_mask (chd, param, 'I');
}

BINDING_TYPE_ircd_modechange(imch_I);
static modeflag imch_I(modeflag rchmode, modeflag rmode, const char *target,
		       modeflag tmode, modeflag tumode, int add, char chtype,
		       int (**ma)(INTERFACE *, const char *, const char *, int,
				  const char **,
				  MASK **(*gcl)(CHANNEL *, char), CHANNEL *chd))
{
  if (!target && (!rchmode || ((rchmode & (A_OP | A_ADMIN)) &&
			       (!_ircd_no_spare_invites || (tmode & A_INVITEONLY)))))
  {
    *ma = &_imch_do_inviteset;
    return (A_INVITED | 1);
  }
  return 1;
}


/* "ircd-umodechange" */
BINDING_TYPE_ircd_umodechange(iumch_a);
static modeflag iumch_a(modeflag rumode, int add,
			void (**ma)(INTERFACE *, const char *, char *,
				    const char *, size_t, int, const char *))
{
  if (!rumode || /* it's a test */
      (rumode & A_SERVER)) /* or servermode */
    return A_AWAY;
  return 0;
}

BINDING_TYPE_ircd_umodechange(iumch_i);
static modeflag iumch_i(modeflag rumode, int add,
			void (**ma)(INTERFACE *, const char *, char *,
				    const char *, size_t, int, const char *))
{
  return A_INVISIBLE;
}

BINDING_TYPE_ircd_umodechange(iumch_w);
static modeflag iumch_w(modeflag rumode, int add,
			void (**ma)(INTERFACE *, const char *, char *,
				    const char *, size_t, int, const char *))
{
  return A_WALLOP;
}

BINDING_TYPE_ircd_umodechange(iumch_r);
static modeflag iumch_r(modeflag rumode, int add,
			void (**ma)(INTERFACE *, const char *, char *,
				    const char *, size_t, int, const char *))
{
  if (add || !rumode) /* cannot be removed */
    return A_RESTRICTED;
  return 0;
}

BINDING_TYPE_ircd_umodechange(iumch_o);
static modeflag iumch_o(modeflag rumode, int add,
			void (**ma)(INTERFACE *, const char *, char *,
				    const char *, size_t, int, const char *))
{
  if (!add || /* only can be deopped */
      !rumode || /* or it's a test */
      (rumode & A_SERVER)) /* or servermode */
    return A_OP;
  return 0;
}

BINDING_TYPE_ircd_umodechange(iumch_O);
static modeflag iumch_O(modeflag rumode, int add,
			void (**ma)(INTERFACE *, const char *, char *,
				    const char *, size_t, int, const char *))
{
  if (!add || /* only can be deopped */
      !rumode || /* or it's a test */
      (rumode & A_SERVER)) /* or servermode */
    return A_HALFOP;
  return 0;
}

BINDING_TYPE_ircd_umodechange(iumch_s);
static modeflag iumch_s(modeflag rumode, int add,
			void (**ma)(INTERFACE *, const char *, char *,
				    const char *, size_t, int, const char *))
{
  return 1; /* not supported, obsolete flag */
}

/* special SSL support */
BINDING_TYPE_ircd_umodechange(iumch_z);
static modeflag iumch_z(modeflag rumode, int add,
			void (**ma)(INTERFACE *, const char *, char *,
				    const char *, size_t, int, const char *))
{
  if (!rumode || /* it's a test */
      (rumode & A_SERVER)) /* or servermode */
    return A_SSL;
  return 0; /* cannot be changed */
}


/* restricted users cannot change any modes */
BINDING_TYPE_ircd_check_modechange(ichmch_r);
static int ichmch_r(INTERFACE *u, modeflag umode, const char *chn, modeflag cmode, int add,
		    modeflag chg, const char *tgt, modeflag tumode, modeflag tcmode,
		    MASK *list)
{
  if ((umode & A_RESTRICTED) && chg != 0)
    return 0;
  return 1;
}


/* ---------------------------------------------------------------------------
 * Data manipulation bindtables.
 */

#undef __TRANSIT__
#define __TRANSIT__ __CHECK_TRANSIT__(token)
static inline void _ircd_mode_broadcast (IRCD *ircd, int id, CLIENT *sender,
					 CHANNEL *ch, char *imp,
					 struct peer_priv *pp, unsigned short token,
					 char *modepass, const char **passed,
					 int x)
{
  size_t sz, ptr = 0;
  int i;
  char buff[MESSAGEMAX];

  if (*imp != '+' && *imp != '-')	/* no empty signs */
    imp++;
  *imp = '\0';				/* terminate line */
  sz = sizeof(buff);
  buff[0] = 0;				/* in case if no arguments */
//  if (ch->mode & A_ANONYMOUS) {
//    for (i = 0; i < x && ptr < (sz - 1); i++)
//      ptr += strfcpy(&buff[ptr], " anonymous", sz - ptr);
//    if (CLIENT_IS_SERVER (sender))
//      ircd_sendto_chan_local (ch, ":%s MODE %s %s%s", sender->nick, ch->name,
//			      modepass, buff);
//    else if (CLIENT_IS_SERVICE (sender)) /* it's forbidden for local services */
//      ircd_sendto_chan_local (ch, ":%s@%s MODE %s %s%s", sender->nick,
//			      sender->cs->nick, ch->name, modepass, buff);
//    else
//      ircd_sendto_chan_local (ch, ":anonymous!anonymous@anonymous. MODE %s %s%s",
//			      ch->name, modepass, buff);
//    ptr = 0;
//  }
  for (i = 0; i < x && ptr < (sz-1); i++) /* compose arguments */
    ptr += snprintf (&buff[ptr], sz - ptr, " %s", passed[i]);
    //TODO: errors check?
  /* notify local users who are on channel */
#ifdef USE_SERVICES
  ircd_sendto_services_mark_prefix (ircd, SERVICE_WANT_MODE);
#endif
  if (ch->mode & A_QUIET) ;
  else if (!sender)
    ircd_sendto_chan_local (ch, "MODE %s %s%s", ch->name, modepass, buff);
  else if (CLIENT_IS_SERVER (sender))
    ircd_sendto_chan_local (ch, ":%s MODE %s %s%s", sender->nick, ch->name,
			    modepass, buff);
  else if (CLIENT_IS_SERVICE (sender))	/* it's forbidden for local services */
    ircd_sendto_chan_local (ch, ":%s@%s MODE %s %s%s", sender->nick,
			    sender->cs->nick, ch->name, modepass, buff);
  else if (ch->mode & A_ANONYMOUS) {
    if (!CLIENT_IS_REMOTE(sender))
      New_Request(sender->via->p.iface, 0, ":%s!%s@%s MODE %s %s%s",
		  sender->nick, sender->user, sender->vhost, ch->name, modepass,
		  buff);
    ircd_sendto_chan_butone(ch, sender, ":anonymous!anonymous@anonymous. MODE %s %s%s",
			    ch->name, modepass, buff);
  } else
    ircd_sendto_chan_local (ch, ":%s!%s@%s MODE %s %s%s", sender->nick,
			    sender->user, sender->vhost, ch->name, modepass,
			    buff);
  /* every server should know channel state too */
  if (ch->mode & A_INVISIBLE ||	/* don't broadcast local channel mode */
      ch->name[0] == '+')	/* nor mode +t for modeless channel */
    return;
#if IRCD_MULTICONNECT
  if (id < 0 && sender && !CLIENT_IS_LOCAL(sender))
    id = ircd_new_id(sender->cs); /* allocate id for IMODE */
  else if (id < 0)
    id = ircd_new_id(NULL);	/* make new id if it's local client */
#endif
#ifdef USE_SERVICES
  ircd_sendto_services_mark_nick (ircd, SERVICE_WANT_MODE);
#endif
  imp = strchr (ch->name, ':');	/* use it as mask ptr storage */
  if (imp)
  {
    imp++;
    if (sender) {
      ircd_sendto_servers_mask_old(ircd, pp, imp, ":%s MODE %s %s%s",
				   sender->nick, ch->name, modepass, buff);
      ircd_sendto_servers_mask_new(ircd, pp, imp, ":%s IMODE %d %s %s%s",
				   sender->nick, id, ch->name, modepass, buff);
    } else {
      ircd_sendto_servers_mask_old(ircd, pp, imp, "MODE %s %s%s",
				   ch->name, modepass, buff);
      ircd_sendto_servers_mask_new(ircd, pp, imp, "IMODE %d %s %s%s",
				   id, ch->name, modepass, buff);
    }
  }
  else if (sender)
  {
    ircd_sendto_servers_old(ircd, pp, ":%s MODE %s %s%s",
			    sender->nick, ch->name, modepass, buff);
    ircd_sendto_servers_new(ircd, pp, ":%s IMODE %d %s %s%s",
			    sender->nick, id, ch->name, modepass, buff);
  }
  else
  {
    ircd_sendto_servers_old(ircd, pp, "MODE %s %s%s", ch->name, modepass, buff);
    ircd_sendto_servers_new(ircd, pp, "IMODE %d %s %s%s",
			    id, ch->name, modepass, buff);
  }
}
#undef __TRANSIT__
#define __TRANSIT__

/* few MODE specific sub-functions */
static inline int _ircd_mode_query_reply (CLIENT *cl, CHANNEL *ch)
{
  char cmode[KEYLEN+64]; /* A-Za-z num key */

  if (ch->name[0] == '+')		/* nomode channel */
    return ircd_do_cnumeric (cl, ERR_NOCHANMODES, ch, 0, NULL);
  _ircd_make_cmode (cmode, sizeof(cmode), ch, _ircd_is_on_channel(cl, ch)?1:0);
  return ircd_do_cnumeric (cl, RPL_CHANNELMODEIS, ch, 0, cmode);
}

static inline int _ircd_mode_mask_query_reply (INTERFACE *srv, CLIENT *cl,
					CHANNEL *ch, const char *par, modeflag mm)
{
  modeflag mf;
  register _mch_func_t f;
  int (*ma)(INTERFACE *, const char *, const char *, int, const char **,
	    MASK **(*)(CHANNEL *, char), CHANNEL *);
  register struct binding_t *b;

  if (ch->name[0] == '+')		/* nomode channel */
    return ircd_do_cnumeric (cl, ERR_NOCHANMODES, ch, 0, NULL);
  b = Check_Bindtable (BTIrcdModechange, par, U_ALL, U_ANYCH, NULL);
  if (b == NULL && par[0] == 'O' && ch->creator != NULL)
    return ircd_do_cnumeric (cl, RPL_UNIQOPIS, ch, 0, ch->creator->who->nick);
  ma = NULL;
  mf = 0;
  while (b)
  {
    if (!b->name && b->key[1] == '*')	/* check for parameterized */
    {
      f = (_mch_func_t)b->func;		/* run binding */
      mf |= f (mm, cl->umode, NULL, ch->mode, 0, -1, ch->name[0], &ma);
    }
    b = Check_Bindtable (BTIrcdModechange, par, U_ALL, U_ANYCH, b);
  }
  if ((mf & ~(1|A_PINGED)) == 0)
    return ircd_do_cnumeric (cl, ERR_CHANOPRIVSNEEDED, ch, 0, NULL);
  if (ma == NULL)
    return ircd_do_cnumeric (cl, ERR_UNKNOWNMODE, ch, 0, par);
  ma (srv, cl->nick, ch->name, -1, NULL, &_get_chanmasklist, ch); /* do report */
  return 1;
}

bool ircd_check_modechange(INTERFACE *u, modeflag umode, const char *chname,
			   modeflag cmode, int add, modeflag chg,
			   const char *tgt, modeflag tumode, modeflag tcmode,
			   CHANNEL *ch)
{
  struct binding_t *b = NULL;
#define static register
  BINDING_TYPE_ircd_check_modechange ((*ff));
#undef static
  char buf[4];

  _ircd_make_wmode(buf, chg, sizeof(buf));
  dprint(5, "ircd:channels.c:ircd_check_modechange: %c%s(%#x) %s on %s",
	 add ? '+' : '-', buf, (int)chg, tgt, chname);
  while ((b = Check_Bindtable(BTIrcdCheckModechange, chname, U_ALL, U_ANYCH, b)))
    if (b->name == NULL)
    {
      ff = b->func;
      if (ff(u, umode, chname, cmode, add, chg, tgt, tumode, tcmode,
	     ch ? get_chanmask(ch, buf[0]) : NULL) == 0) // FIXME: can buf[1] be?
	return FALSE;
    }
  return TRUE;
}

#define CONTINUE_ON_MODE_ERROR(A,B) if (ircd_do_cnumeric (cl, A, ch, 0, B)) continue;

BINDING_TYPE_ircd_client_cmd(ircd_mode_cb); /* huge one as hell */
static int ircd_mode_cb(INTERFACE *srv, struct peer_t *peer, const char *lcnick,
			const char *user, const char *host, const char *vhost,
			modeflag eum, int argc, const char **argv)
{ /* args: <target> [modes...] */
  CLIENT *cl = ((struct peer_priv *)peer->iface->data)->link->cl;
  CHANNEL *ch;
  int i, n;
  char modepass[64]; /* it should accept all modes at once */

  if (argc < 1)
    return ircd_do_unumeric (cl, ERR_NEEDMOREPARAMS, cl, 0, "MODE");
  modepass[0] = argv[0][0];
  modepass[1] = '\0';
  /* check if target is a channel name */
  if (Check_Bindtable(BTIrcdChannel, modepass, U_ALL, U_ANYCH, NULL))
  {
    MEMBER *memb;
    char *imp;
    const char *passed[MAXMODES];
    const char *c;
    int x, add;
    CMASKL *ml;

    ch = _ircd_find_channel ((IRCD *)srv->data, argv[0]);
    if (ch == NULL)
      return ircd_do_unumeric (cl, ERR_NOSUCHCHANNEL, cl, 0, argv[0]);
    if (argc == 1)			/* channel mode query */
      return _ircd_mode_query_reply (cl, ch);
    memb = _ircd_is_on_channel (cl, ch); /* may query some modes if NULL */
    _imch_client = cl;
    //TODO: accept RFC1459 query syntax too (mode #chan +b) ifndef IRCD_STRICT_MODECMD
    if (argc == 2 && *argv[1] != '+' && *argv[1] != '-') /* mask mode query */
      return _ircd_mode_mask_query_reply (srv, cl, ch, argv[1],
					  memb ? memb->mode : 0);
    if (!memb)
      return ircd_do_cnumeric (cl, ERR_USERNOTINCHANNEL, ch, 0, peer->dname);
    if (ch->name[0] == '+')		/* nomode channel */
      return ircd_do_cnumeric (cl, ERR_NOCHANMODES, ch, 0, NULL);
    n = x = 0;
    imp = modepass;
    *imp = 0;
    for (i = 1; i < argc; i++)		/* parse modes */
    {
      if (!_ircd_strict_modecmd && i == 1) {
	*imp = '+';			/* implicit '+' before first arg */
	add = 1;
      } else
	add = -1;			/* next args should have + or - */
      for (c = argv[i]; *c; c++)
      {
	if (*c == '+')			/* adding mode */
	{
	  if (imp > modepass && *imp != '-' && *imp != '+')
	    imp++;
	  *imp = *c;
	  add = 1;
	}
	else if (*c == '-')		/* removing mode */
	{
	  if (imp > modepass && *imp != '-' && *imp != '+')
	    imp++;
	  *imp = *c;
	  add = 0;
	}
	else if (add < 0)		/* invalid */
	{
	  char charstr[2];

	  charstr[0] = *c;
	  charstr[1] = '\0';
	  ircd_do_cnumeric (cl, ERR_UNKNOWNMODE, ch, 0, charstr);
	}
	else				/* valid */
	{
	  struct binding_t *b;
	  modeflag mf = 0;
	  char charstr[2];
	  register _mch_func_t f;
	  register int ec;
	  int (*ma)(INTERFACE *, const char *, const char *, int, const char **,
		    MASK **(*)(CHANNEL *, char), CHANNEL *);
	  const char *par;
	  MEMBER *tar;

	  charstr[0] = *c;
	  charstr[1] = '\0';
	  b = Check_Bindtable (BTIrcdModechange, charstr, U_ALL, U_ANYCH, NULL);
	  if (!b)
	    CONTINUE_ON_MODE_ERROR (ERR_UNKNOWNMODE, charstr);
	  tar = NULL;
	  if ((par = strchr (Ircd_modechar_list, *c)) /* check for compliance */
	      && Ircd_whochar_list[par-Ircd_modechar_list] != ' ')
	  {
	    if (i + 1 >= argc)		/* if param available at all */
	      CONTINUE_ON_MODE_ERROR (ERR_NEEDMOREPARAMS, "MODE");
	    par = argv[++i];		/* target is next param */
	    tar = _ircd_is_on_channel (ircd_find_client (par, NULL), ch);
	    if (!tar)
	      CONTINUE_ON_MODE_ERROR (ERR_USERNOTINCHANNEL, par);
	  } else
	    par = NULL;
	  ma = NULL;
	  while (b)			/* cycle thru all */
	  {
	    if (!b->name)
	    {
	      f = (_mch_func_t)b->func;
	      if (tar)			/* modechange for target */
		mf |= f (memb->mode, cl->umode, tar->who->nick, tar->mode,
			 tar->who->umode, add, ch->name[0], &ma);
	      else
		mf |= f (memb->mode, cl->umode, NULL, ch->mode, 0, add,
			 ch->name[0], &ma);
	    }
	    b = Check_Bindtable (BTIrcdModechange, charstr, U_ALL, U_ANYCH, b);
	  }
	  if (mf & 1)			/* require a parameter */
	  {
	    if (i + 1 >= argc)		/* but parameter unavailable */
	      CONTINUE_ON_MODE_ERROR (ERR_NEEDMOREPARAMS, "MODE");
	    par = argv[++i];		/* parameter is next one */
	    mf--;			/* reset the flag */
	  }
	  if (mf & A_PINGED)		/* check if A_ADMIN required but not */
	    CONTINUE_ON_MODE_ERROR (ERR_UNIQOPPRIVSNEEDED, NULL);
	  if (mf && !ircd_check_modechange(peer->iface, memb->mode, ch->name, ch->mode,
					   add, mf, par, tar ? tar->who->umode : 0,
					   tar ? tar->mode : 0, ch))
	      mf = 0;			/* change denied, stop */
	  if (!mf) {
	    if (!(memb->mode & (A_OP | A_ADMIN))) { /* check permissions */
	      CONTINUE_ON_MODE_ERROR (ERR_CHANOPRIVSNEEDED, NULL);
	    } else
	      CONTINUE_ON_MODE_ERROR (ERR_UNKNOWNMODE, charstr);
	  } else if ((par != NULL && x == MAXMODES) || n > 30) /* see modepass */
	    continue; //TODO: silently ignore or what?
	  if (add)
	  {
	    if (tar) {			/* it has a target */
	      if (tar->mode & mf)
		continue;
	      tar->mode |= mf;
	      if (mf & A_OP)		/* operator added so reset this */
		ch->noop_since = 0;
	    } else if (ma) {		/* it has an handler */
	      ec = ma (srv, peer->dname, ch->name, add, &par, &_get_chanmasklist, ch);
	      if (ec <= 0) {
		if (ec < 0)
		  dprint(4, "ircd:channels.c: mode +%c %s already present on %s",
			 *c, par, ch->name);
		else
		  WARNING ("ircd: error on setting MODE %s +%c %s", ch->name,
			   *c, par);
		continue;
	      } /* else it's handled, no flags just propagate */
	    } else if (ch->mode & mf) { /* dummy modechange */
	      dprint(4, "ircd:channels.c: mode +%c already present on %s", *c,
		     ch->name);
	      continue;
	    } else			/* just modechange */
	      ch->mode |= mf;
	    n++;			/* one more accepted */
	    *++imp = *c;		/* it has '+' or last */
	    if (par)
	      passed[x++] = par;	/* one more param accepted */
	    continue;
	  }
	  if (tar) {			/* it has a target */
	    if (!(tar->mode & mf))
	      continue;
	    tar->mode &= ~mf;
	    if (mf == A_OP && ch->name[0] == '!')
	    {
	      /* set noop_since if it was last op in channel */
	      register MEMBER *op;

	      for (op = ch->users; op; op = op->prevnick)
		if (op->mode & (A_OP | A_ADMIN))
		  break;
	      if (op)
	      {
		ch->noop_since = Time;
		Add_Timer (srv, S_WAKEUP, REOP_DELAY); // FIXME: add a random?
	      }
	    }
	  } else if (ma) {		/* it has a parameter */
	    ec = ma (srv, peer->dname, ch->name, add, &par, &_get_chanmasklist, ch);
	    if (ec <= 0) {
	      if (ec < 0)
		dprint(4, "ircd:channels.c: there isn't +%c %s on %s to remove",
		       *c, par, ch->name);
	      else
		WARNING ("ircd: error on setting MODE %s -%c %s", ch->name, *c,
			 par);
	      continue;
	    } /* else it's handled, no flags just propagate */
	  } else if (!(ch->mode & mf)) { /* dummy modechange */
	    dprint(4, "ircd:channels.c: there isn't +%c on %s to remove", *c,
		   ch->name);
	    continue;
	  } else			/* just modechange */
	    ch->mode &= ~mf;
	  n++;				/* one more accepted */
	  *++imp = *c;			/* it has '-' or last */
	  if (par)
	    passed[x++] = par;		/* one more param accepted */
	}
      }
    }
    if (n)				/* there were some changes */
      _ircd_mode_broadcast((IRCD *)srv->data, -1, cl, ch, imp, NULL, 0,
			   modepass, passed, x);
    /* broadcast cancelled modes now */
    x = 0;
    modepass[0] = '-';
    imp = modepass;
    for (ml = _imch_cancel.masks; ml; ml = ml->next) {
      while (ml->list != NULL) {
	register MASK *mm;

	if (x == MAXMODES) {
	  _ircd_mode_broadcast((IRCD *)srv->data, -1, cl, ch, imp, NULL, 0,
			       modepass, passed, x);
	  imp = modepass;
	  x = 0;
	}
	*++imp = ml->mch;
	mm = ml->list;
	ml->list = mm->next;
	passed[x++] = mm->what;	/* it will be not changed yet */
	free_MASK(mm);
      }
    }
    if (x > 0)
      _ircd_mode_broadcast((IRCD *)srv->data, -1, cl, ch, imp, NULL, 0,
			   modepass, passed, x);
  }
  else if (ircd_find_client(argv[0], NULL) != cl)
    return ircd_do_unumeric (cl, ERR_USERSDONTMATCH, cl, 0, argv[0]);
  else					/* umode request */
  {
    modeflag toadd = 0, todel = 0;

    if (argc == 1)			/* umode query */
    {
      char umode[16];
      ircd_make_umode (umode, cl->umode, sizeof(umode));
      return ircd_do_unumeric (cl, RPL_UMODEIS, cl, 0, umode);
    }
    n = 0;
    for (i = 1; i < argc; i++)		/* parse modes */
    {
      const char *c;
      int add = -1;

      for (c = argv[i]; *c; c++)
      {
	if (*c == '+')			/* adding */
	  add = 1;
	else if (*c == '-')		/* removing */
	  add = 0;
	else if (add < 0)		/* invalid */
	{
	  char charstr[2];

	  charstr[0] = *c;
	  charstr[1] = '\0';
	  ircd_do_unumeric (cl, ERR_UMODEUNKNOWNFLAG, cl, 0, charstr);
	}
	else if (n == MAXMODES)
	  ; //TODO: silently ignore or what?
	else				/* valid */
	{
	  struct binding_t *b;
	  modeflag mf = 0;
	  char charstr[2];
#define static register
	  BINDING_TYPE_ircd_umodechange ((*f));
#undef static
	  void (*ma)(INTERFACE *, const char *, char *, const char *, size_t, int, const char *);

	  charstr[0] = *c;
	  charstr[1] = '\0';
	  b = Check_Bindtable (BTIrcdUmodechange, charstr, U_ALL, U_ANYCH, NULL);
	  if (!b)
	  {
	    ircd_do_unumeric (cl, ERR_UMODEUNKNOWNFLAG, cl, 0, charstr);
	    continue;			/* ignoring it */
	  }
	  ma = NULL;
	  while (b)			/* cycle thru all */
	  {
	    if (!b->name && (f = (modeflag (*)())b->func))
	      mf |= f (eum, add, &ma);
	    b = Check_Bindtable (BTIrcdUmodechange, charstr, U_ALL, U_ANYCH, b);
	  }
	  mf &= ~(A_ISON | A_PINGED);
#if 0
	  while (mf && (b = Check_Bindtable (BTIrcdCheckModechange, peer->dname,
					     U_ALL, U_ANYCH, b)))
	    if (!b->name && !b->func (srv, cl->umode, 0, add, mf, NULL))
	      mf = 0;			/* change denied, stop */
#endif
	  if (!mf) {
	    continue;			/* change denied */
	  }
	  if (add)
	  {
	    mf &= ~cl->umode;		/* reset modes that we have already */
	    if (!mf)
	      continue;
	    if (ma)			/* update vhost */
	      ma (srv, peer->dname, cl->vhost, cl->host, sizeof(cl->vhost), add,
		  cl->cs->nick);
	    n++;			/* one more accepted */
	    New_Request (cl->via->p.iface, 0, ":%s MODE %s +%c", peer->dname,
			 peer->dname, *c);
	    toadd |= mf;
	    todel &= ~mf;
	    continue;
	  }
	  mf &= cl->umode;		/* reset modes that we don't have */
	  if (!mf)
	    continue;
	  if (ma)			/* update vhost */
	    ma (srv, peer->dname, cl->vhost, cl->host, sizeof(cl->vhost), add,
		cl->cs->nick);
	  n++;				/* one more accepted */
	  New_Request (cl->via->p.iface, 0, ":%s MODE %s -%c", peer->dname,
		       peer->dname, *c);
	  todel |= mf;
	  toadd &= ~mf;
	}
      }
    }
#ifdef USE_SERVICES
    /* notify local services */
    if (toadd | todel)
    {
      char *c = modepass;

      if (toadd)
      {
	*c++ = '+';
	ircd_make_umode (c, toadd, MAXMODES+1);
	c += strlen (c);
      }
      if (todel)
      {
	*c++ = '-';
	ircd_make_umode (c, todel, MAXMODES+1);
      }
      if ((toadd & (A_OP | A_HALFOP)) || (todel & (A_OP | A_HALFOP)))
        ircd_sendto_services_all ((IRCD *)srv->data,
				  SERVICE_WANT_UMODE | SERVICE_WANT_OPER,
				  ":%s MODE %s %s", peer->dname, peer->dname,
				  modepass);
      else
        ircd_sendto_services_all ((IRCD *)srv->data, SERVICE_WANT_UMODE,
				  ":%s MODE %s %s", peer->dname, peer->dname,
				  modepass);
    }
#endif
    cl->umode |= toadd;
    cl->umode &= ~todel;
    toadd &= ~A_HALFOP;			/* localops should not be broadcasted */
    todel &= ~A_HALFOP;
    if (toadd | todel)			/* we have something changed */
    {
      char *c = modepass;

      if (toadd)
      {
	*c++ = '+';
	ircd_make_umode (c, toadd, MAXMODES+1);
	c += strlen (c);
      }
      if (todel)
      {
	*c++ = '-';
	ircd_make_umode (c, todel, MAXMODES+1);
      }
      ircd_sendto_servers_new (((IRCD *)srv->data), NULL, ":%s IMODE %d %s %s",
			       peer->dname, ircd_new_id(NULL), peer->dname,
			       modepass);
      ircd_sendto_servers_old (((IRCD *)srv->data), NULL, ":%s MODE %s %s",
			       peer->dname, peer->dname, modepass);
    }
  }
  return 1;
}
#undef CONTINUE_ON_MODE_ERROR

/* modified version of ircd_new_to_channel() */
static inline MEMBER *_ircd_do_join(IRCD *ircd, CLIENT *cl, CHANNEL *ch, modeflag mf)
{
  MEMBER *r;
  register MEMBER *mm;

  dprint(5, "ircd:channels.c:_ircd_do_join: %s to %s", cl->nick, ch->name);
  for (mm = ch->invited; mm; mm = mm->prevnick)
    if (mm->who == cl)
      break;
  if (mm)
    _ircd_del_from_invited (mm);
  r = ircd_add_to_channel (ircd, NULL, ch, cl, mf); /* this way cannot get errors */
  if (r == NULL)			/* ignored due to acks or already joined */
    return (NULL);
  if (ch->topic[0])
  {
    ircd_do_cnumeric (cl, RPL_TOPIC, ch, 0, ch->topic);
#ifdef TOPICWHOTIME
    if (ch->topic_since > 0)
    {
      char topicwhotime[HOSTMASKLEN+12]; /* nick time */

      if (ch->mode & A_ANONYMOUS)
	snprintf (topicwhotime, sizeof(topicwhotime),
		  "anonymous!anonymous@anonymous. %ld", ch->topic_since);
      else
	snprintf (topicwhotime, sizeof(topicwhotime), "%s %ld", ch->topic_by,
		  ch->topic_since);
      ircd_do_cnumeric (cl, RPL_TOPICWHOTIME, ch, 0, topicwhotime);
    }
#endif
  }
  ircd_names_reply (ircd_find_client(NULL, NULL), cl, ch, 0); /* RPL_NAMREPLY */
  ircd_do_unumeric (cl, RPL_ENDOFNAMES, cl, 0, ch->name);
  return (r);
}

static inline void _ircd_join_0_local (IRCD *ircd, CLIENT *cl, char *key)
{
  register CHANNEL *ch;

  if (cl->c.hannels == NULL)	/* nothing to do! */
    return;
  if (key == NULL)
    key = cl->nick;
  while (cl->c.hannels)
  {
    if ((ch = cl->c.hannels->chan)->mode & A_QUIET)
      New_Request (cl->via->p.iface, 0, ":%s!%s@%s PART %s :%s", cl->nick,
		   cl->user, cl->vhost, ch->name, key);
    else if (ch->mode & A_ANONYMOUS)
    {
      New_Request (cl->via->p.iface, 0, ":%s!%s@%s PART %s :%s", cl->nick,
		   cl->user, cl->vhost, ch->name, key);
      ircd_sendto_chan_butone (ch, cl, ":anonymous!anonymous@anonymous. PART %s :anonymous",
			       ch->name);
    }
    else
      ircd_sendto_chan_local (ch, ":%s!%s@%s PART %s :%s", cl->nick,
			      cl->user, cl->vhost, ch->name, key);
    ircd_del_from_channel (ircd, cl->c.hannels, 0);
  }
  ircd_sendto_servers_all_ack(ircd, cl, CHANNEL0, (struct peer_priv *)NULL,
			      ":%s JOIN 0 :%s", cl->nick, key);
}

BINDING_TYPE_ircd_client_cmd(ircd_join_cb);
static int ircd_join_cb(INTERFACE *srv, struct peer_t *peer, const char *lcnick,
			const char *user, const char *host, const char *vhost,
			modeflag eum, int argc, const char **argv)
{ /* args: <channel> [,<channel> ...] [<key> [,<key> ... ]] | 0 */
  CLIENT *cl = ((struct peer_priv *)peer->iface->data)->link->cl;
  CHANNEL *ch;
  register MEMBER *mm;
  char *chn, *key, *nextch, *nextkey, *cmask;
  const char *nchn;
  CLIENT *me = ircd_find_client(NULL, NULL);
  struct binding_t *b;
#define static register
  BINDING_TYPE_ircd_channel ((*f));
#undef static
  MASK *cm;
  modeflag mf;
  int x, i, ptr;
  char cnfc[2];
  char lcchname[MB_LEN_MAX*CHANNAMELEN+1];
  char lcb[MB_LEN_MAX*NICKLEN+IDENTLEN+HOSTLEN+3];
  char lcbv[MB_LEN_MAX*NICKLEN+IDENTLEN+HOSTLEN+3];
  char bufforservers[MB_LEN_MAX*IRCMSGLEN];

#ifdef USE_SERVICES
  /* forbidden for services */
  if (CLIENT_IS_SERVICE(cl))
    return 0;
#endif
  if (argc == 0)
    return ircd_do_unumeric (cl, ERR_NEEDMOREPARAMS, cl, 0, "JOIN");
  chn = (char *)argv[0];
  nextkey = NULL;
  if (argc == 1)
    key = NULL;
  else
    key = (char *)argv[1];
  for (x = 0, mm = cl->c.hannels; mm; mm = mm->prevchan) /* count channels */
    x++;
  cnfc[1] = '\0';
  ptr = 0;
  while (chn)
  {
    /* split with next chan */
    nextch = strchr (chn, ',');
    if (nextch)
      *nextch++ = 0;
    if (key)
      nextkey = strchr (key, ',');
    if (nextkey)
      *nextkey++ = 0;
    if (!strcmp (chn, "0"))		/* user requested to part all */
    {
      _ircd_join_0_local ((IRCD *)srv->data, cl, key);
      chn = nextch;
      key = nextkey;
      x = 0;				/* reset counters */
      ptr = 0;
      continue;
    }
    //TODO: just drop ^G or cancel (sub)join?
    cmask = strchr (chn, ':');
    if (cmask)
      cmask++;
    /* now we have chn, cmask, and key prepared so check them */
    i = -1;
    mf = 0;
    cnfc[0] = chn[0];
    lcb[0] = '\0';
    lcbv[0] = '\0';
    unistrlower(lcchname, chn, sizeof(lcchname));
    _ircd_validate_channel_name(lcchname);
    ch = _ircd_find_channel ((IRCD *)srv->data, lcchname);
    if (ch && Time >= ch->hold_upto &&	/* it's available to hold off now */
#if IRCD_MULTICONNECT
	ch->on_ack == 0 &&
#endif
	ch->count == 0)
    {
      ircd_drop_channel ((IRCD *)srv->data, ch);
      ch = NULL;
    }
    nchn = chn;
    if (cmask && simple_match (cmask, me->lcnick) <= 0)
      ircd_do_unumeric (cl, ERR_BADCHANMASK, cl, 0, cmask);
    else if ((b = Check_Bindtable (BTIrcdChannel, cnfc, U_ALL, U_ANYCH, NULL))
	     && !b->name)
      mf = (f = (modeflag (*)())b->func) (peer->iface, cl->umode,
					  ch ? ch->mode : 0, ch ? ch->count : 0,
					  chn, ((IRCD *)srv->data)->channels,
					  &nchn);
    if (ch == NULL && mf != 0 && nchn != chn) { /* binding changed the name */
      unistrlower(lcchname, nchn, sizeof(lcchname));
      _ircd_validate_channel_name(lcchname);
      ch = _ircd_find_channel ((IRCD *)srv->data, lcchname);
    }
    if (ch && !mf)			/* channel is still on hold */
      ircd_do_unumeric (cl, ERR_UNAVAILRESOURCE, cl, 0, chn);
    else if (!mf)			/* cannot create a channel */
      ircd_do_unumeric (cl, ERR_NOSUCHCHANNEL, cl, 0, chn);
    else if (!ch)			/* OK, user is allowed to create */
      i = 1;
#ifdef IRCD_ALLOW_TWILIGHT_JOIN
    else if (cl->umode & A_OP)		/* IRCop can override some modes */
      i = 1;
#endif
    else if (ch->limit && ch->count >= ch->limit) /* out of channel limit */
      ircd_do_cnumeric (cl, ERR_CHANNELISFULL, ch, 0, NULL);
    else if (ch->key[0] && safe_strcmp (ch->key, key)) /* check key */
      ircd_do_cnumeric (cl, ERR_BADCHANNELKEY, ch, 0, NULL);
    else if (ch->mode & A_INVITEONLY)	/* check invitations */
    {
      i = 0;
      for (mm = ch->invited; mm; mm = mm->prevnick)
	if (mm->who == cl)
	  break;			/* found */
      if (!mm)
      {
	snprintf (lcb, sizeof(lcb), "%s!%s@%s", cl->lcnick, cl->user, cl->host);
	if (cl->umode & A_MASKED)
	  snprintf (lcbv, sizeof(lcbv), "%s!%s@%s", cl->lcnick, cl->user, cl->vhost);
	for (cm = INVITES(ch); cm; cm = cm->next)
	  if ((lcbv[0] && simple_match (cm->what, lcbv) > 0) ||
	      simple_match (cm->what, lcb) > 0) //TODO: check expiration
	    break;
	if (!cm)			/* not found */
	  i = -ircd_do_cnumeric (cl, ERR_INVITEONLYCHAN, ch, 0, NULL);
      }
    } else
      i = 0;
    if (mf && i == 0)			/* check bans/exceptions */
    {
      for (mm = ch->invited; mm; mm = mm->prevnick)
	if (mm->who == cl)
	  break;
      if (!mm)				/* found invite will override ban */
      {
	if (!*lcb)			/* it might be done by check above */
	  snprintf (lcb, sizeof(lcb), "%s!%s@%s", cl->lcnick, cl->user, cl->host);
	if (!*lcbv && (cl->umode & A_MASKED))
	  snprintf (lcbv, sizeof(lcbv), "%s!%s@%s", cl->lcnick, cl->user, cl->vhost);
	for (cm = BANS(ch); cm; cm = cm->next)
	  if ((lcbv[0] && simple_match (cm->what, lcbv) > 0) ||
	      simple_match (cm->what, lcb) > 0)
	    break;			/* found ban */
	if (cm)
	{
	  for (cm = EXEMPTS(ch); cm; cm = cm->next)
	    if ((lcbv[0] && simple_match (cm->what, lcbv) > 0) ||
		simple_match (cm->what, lcb) > 0)
	      break;			/* found exception */
	  if (!cm)
	    i = -ircd_do_cnumeric (cl, ERR_BANNEDFROMCHAN, ch, 0, NULL);
	}
      }
    }
    if (mf && i == 0)			/* OK, user is allowed to join yet */
    {
      if (ircd_check_modechange(peer->iface, cl->umode, nchn, ch ? ch->mode : mf,
				1, 0, cl->nick, cl->umode, 0, ch))
	i = 1;				/* so he/she allowed at last */
    }
    if (x >= _ircd_max_channels)		/* joined too many channels already */
      ircd_do_unumeric (cl, ERR_TOOMANYCHANNELS, cl, 0, NULL);
    else if (i > 0) {			/* so user can join, do it then */
      if (ch == NULL)			/* it's still not found */
	ch = _ircd_new_channel ((IRCD *)srv->data, nchn, lcchname);
      mm = _ircd_do_join ((IRCD *)srv->data, cl, ch, mf);
      if (mm == NULL) {
	dprint(3, "refused to add %s into %s", cl->nick, ch->name);
      } else if (!(ch->mode & A_INVISIBLE)) { /* it is not a local channel */
	char smode[sizeof(Ircd_modechar_list)+1];

	_ircd_make_wmode(smode, mm->mode, sizeof(smode));
	if (cmask == NULL) {		/* global wide channel */
	  if (ptr && ptr + strlen(ch->name) >= sizeof(bufforservers) - 2) {
	    ircd_sendto_servers_all((IRCD *)srv->data, NULL, ":%s JOIN %s",
				    cl->nick, bufforservers);
	    ptr = 0;
	  }
	  ptr += snprintf(&bufforservers[ptr], sizeof(bufforservers) - ptr,
			  "%s%s%s%s", ptr ? "," : "", ch->name,
			  *smode ? "\007" : "", smode);
	} else				/* there is a channel mask */
	  ircd_sendto_servers_mask((IRCD *)srv->data, NULL, cmask,
				   ":%s JOIN %s%c%s", cl->nick, ch->name,
				   *smode ? '\007' : '\0', smode);
	x++;
      } else
	x++;
    }
    chn = nextch;
    key = nextkey;
  }
  if (ptr)
    ircd_sendto_servers_all((IRCD *)srv->data, NULL, ":%s JOIN %s", cl->nick,
			    bufforservers);
  return 1;
}

#define _ircd_find_client_lc(I,X) Find_Key ((I)->clients, X)

#define MAXTRACKED_MODE_ERRORS 4

/* this one should be used in brackets if around if() */
#define CONTINUE_ON_MODE_ERROR(A,B,...) \
    ERROR ("ircd:" A B, __VA_ARGS__); \
    if (errors < MAXTRACKED_MODE_ERRORS) \
      lasterror[errors] = A; \
    errors++; \
    continue

#undef __TRANSIT__
#define __TRANSIT__ __CHECK_TRANSIT__(token)
/* huge function, used by both ircd_mode_sb and ircd_imode */
static int _ircd_do_smode(INTERFACE *srv, struct peer_priv *pp,
			  unsigned short token, int id, const char *sender,
			  const char *lcsender, int argc, const char **argv)
{ /* input: <channel> modes... */
  CLIENT *src, *tgt;
  CHANNEL *ch;
  int i, n;
  char modepass[64]; /* it should accept all modes at once */

  if (!(src = _ircd_find_client_lc ((IRCD *)srv->data, lcsender)))
  {
    ERROR ("ircd:MODE command by unknown \"%s\" via %s", sender, pp->p.dname);
    return ircd_recover_done (pp, "bogus MODE sender");
  }
  ch = _ircd_find_channel ((IRCD *)srv->data, argv[0]);
  if (ch)				/* channel mode change */
  {
    register MEMBER *who;
    char *imp;
    const char *lasterror[MAXTRACKED_MODE_ERRORS];
    modeflag whof;
    const char *passed[MAXMODES];
    int x, errors;
    CMASKL *ml;

    _imch_client = src;
    if (ch->name[0] == '+')		/* nomode channel */
    {
      ERROR ("ircd:MODE for no-mode %s via %s", ch->name, pp->p.dname);
      return ircd_recover_done (pp, "MODE for modeless channel");
    } else if (ch->mode & A_INVISIBLE) { /* local channel */
      ERROR ("ircd:MODE for local %s via %s", ch->name, pp->p.dname);
      return ircd_recover_done (pp, "MODE for local channel");
    }
    if (CLIENT_IS_SERVER(src))
      whof = A_OP | A_ADMIN;		/* for servermode */
    else if ((who = _ircd_is_on_channel (src, ch)))
      whof = who->mode;
    else
    {
      /* log alient MODE change */
      Add_Request(I_LOG, "*", F_WARN, "ircd:MODE by %s not on %s via %s",
		  src->nick, ch->name, pp->p.dname);
#if !ALLOW_NOOP_CHANMGMT
      if (!ircd_recover_done (pp, "bogus MODE sender"))
	return 1;
#endif
      whof = A_OP | A_ADMIN;		/* but allow it anyway */
    }
    n = x = errors = 0;
    imp = modepass;
    *imp = 0;
    for (i = 1; i < argc; i++)		/* parse modes */
    {
      const char *c;
      int add = -1;

      for (c = argv[i]; *c; c++)
      {
	if (*c == '+')			/* adding mode */
	{
	  if (imp > modepass && *imp != '-' && *imp != '+')
	    imp++;
	  *imp = *c;
	  add = 1;
	}
	else if (*c == '-')		/* removing mode */
	{
	  if (imp > modepass && *imp != '-' && *imp != '+')
	    imp++;
	  *imp = *c;
	  add = 0;
	}
	else if (add < 0) {		/* invalid */
	  CONTINUE_ON_MODE_ERROR ("unknown MODE char", " %c via %s", *c,
				  pp->p.dname);
	} else {			/* valid */
	  struct binding_t *b;
	  modeflag mf = 0;
	  char charstr[2];
	  register _mch_func_t f;
	  register int ec;
	  int (*ma)(INTERFACE *, const char *, const char *, int, const char **,
		    MASK **(*)(CHANNEL *, char), CHANNEL *);
	  const char *par;
	  MEMBER *tar;

	  charstr[0] = *c;
	  charstr[1] = '\0';
	  b = Check_Bindtable (BTIrcdModechange, charstr, U_ALL, U_ANYCH, NULL);
	  if (!b) {
	    /* cannot revert it since don't know if it should take parameter
	       and also it may mess order but what we can do anyway? squit? */
	    CONTINUE_ON_MODE_ERROR ("unknown MODE char", " %c via %s", *c,
				    pp->p.dname);
	  }
	  tar = NULL;
	  if ((par = strchr (Ircd_modechar_list, *c)) /* check for compliance */
	      && Ircd_whochar_list[par-Ircd_modechar_list] != ' ')
	  {
	    if (i + 1 >= argc) {	/* if param available at all */
	      CONTINUE_ON_MODE_ERROR ("incomplete MODE", " %c%c via %s",
				      add ? '+' : '-', *c, pp->p.dname);
	    }
	    par = argv[++i];		/* target is next param */
	    tar = _ircd_is_on_channel (ircd_find_client (par, pp), ch);
	    /* ircd_find_client() traces history for nick changes so
	       there should be no race collisions for MODE on that nick */
	    if (!tar) {
	      CONTINUE_ON_MODE_ERROR ("bogus MODE target", " %s not on %s via %s",
				      par, ch->name, pp->p.dname);
	    }
	  } else
	    par = NULL;
	  ma = NULL;
	  while (b)			/* cycle thru all */
	  {
	    if (!b->name)
	    {
	      f = (_mch_func_t)b->func;
	      if (tar)			/* modechange for target */
		mf |= f (whof, src->umode, tar->who->nick, tar->mode,
			 tar->who->umode, add, ch->name[0], &ma);
	      else
		mf |= f (whof, src->umode, NULL, ch->mode, 0, add, ch->name[0],
			 &ma);
	    }
	    b = Check_Bindtable (BTIrcdModechange, charstr, U_ALL, U_ANYCH, b);
	  }
	  if (mf & 1)			/* require a parameter */
	  {
	    if (i + 1 >= argc) {	/* but parameter unavailable */
	      CONTINUE_ON_MODE_ERROR ("incomplete MODE", " %c%c via %s",
				      add ? '+' : '-', *c, pp->p.dname);
	    }
	    par = argv[++i];		/* parameter is next one */
	    mf--;			/* reset the flag */
	  }
	  mf &= ~A_PINGED;		/* reset extra flag */
#if !ALLOW_NOOP_CHANMGMT
	  if (mf && !ircd_check_modechange(NULL, whof, ch->name, ch->mode, add,
					   mf, par, tar ? tar->who->umode : 0,
					   tar ? tar->mode : 0, ch))
	      mf = 0;			/* change denied, stop */
	  if (!mf)			/* check permissions */
	  {
	    New_Request (pp->p.iface, 0, "MODE %s %c%c %s", ch->name,
			 add ? '-' : '+', *c, NONULL(par)); /* revert it */
	    CONTINUE_ON_MODE_ERROR ("impossible MODE", " %c%c via %s",
				    add ? '+' : '-', *c, pp->p.dname);
	  }
#endif
	  if ((par != NULL && x == MAXMODES) || n > 30) /* see modepass */
	  {
#if IRCD_MULTICONNECT
	    if (id >= 0) {
	      ERROR("ircd:too many modes via %s, squit it to avoid desync",
		    pp->p.dname);
	      errors = -1;
	      i = argc;			/* terminate parsing right now */
	      break;
	    }
#endif
	    _ircd_mode_broadcast ((IRCD *)srv->data, -1, src, ch, imp, pp,
				  token, modepass, passed, x);
	    n = x = 0;
	    imp = modepass;
	    if (add)
	      *imp = '+';
	    else
	      *imp = '-';
	  }
	  if (add)
	  {
	    if (tar) {			/* it has a target */
	      tar->mode |= mf;
	      if (mf & A_OP)		/* operator added so reset this */
		ch->noop_since = 0;
	    } else if (ma) {		/* it has a parameter */
	      ec = ma (srv, pp->p.dname, ch->name, add, &par, &_get_chanmasklist, ch);
	      if (ec <= 0) {
		if (ec == 0) {
		  WARNING ("ircd: error on setting MODE %s +%c %s", ch->name,
			   *c, par);
		  New_Request (pp->p.iface, 0, "MODE %s -%c %s", ch->name, *c,
			       par);	/* revert it */
		} else
		  dprint(4, "ircd:channels.c: mode +%c %s already present on %s",
			 *c, par, ch->name);
		continue;
	      } /* else it's handled, no flags just propagate */
	    } else if (ch->mode & mf) { /* dummy modechange */
	      dprint(4, "ircd:channels.c: dummy modechange via %s on %s: +%c",
		     pp->p.dname, ch->name, *c);
	      continue;
	    } else			/* just modechange */
	      ch->mode |= mf;
	    n++;			/* one more accepted */
	    *++imp = *c;		/* it has '+' or last */
	    if (par)
	      passed[x++] = par;	/* one more param accepted */
	    continue;
	  }
	  if (tar) {			/* it has a target */
	    tar->mode &= ~mf;
	    if (mf == A_OP && ch->name[0] == '!')
	    {
	      /* set noop_since if it was last op in channel */
	      register MEMBER *op;

	      for (op = ch->users; op; op = op->prevnick)
		if (op->mode & (A_OP | A_ADMIN))
		  break;
	      if (op)
	      {
		ch->noop_since = Time;
		Add_Timer (srv, S_WAKEUP, REOP_DELAY); // FIXME: add a random?
	      }
	    }
	  } else if (ma) {		/* it has a parameter */
	    ec = ma (srv, pp->p.dname, ch->name, add, &par, &_get_chanmasklist, ch);
	    if (ec <= 0) {
#if IRCD_MULTICONNECT
	      if (id < 0) {		/* we sent that mode before */
#endif
		dprint(4, "ircd:channels.c: there isn't +%c %s on %s to remove, ignoring it",
		       *c, par, ch->name);
		continue;
#if IRCD_MULTICONNECT
	      }
	      dprint(4, "ircd:channels.c: there isn't +%c %s on %s to remove but sending it anyway",
		     *c, par, ch->name); /* see below - we skipped sending */
#endif
	    } /* else it's handled, no flags just propagate */
	  } else if (!(ch->mode & mf)) { /* dummy modechange */
	    dprint(4, "ircd:channels.c: dummy modechange via %s on %s: -%c",
		   pp->p.dname, ch->name, *c);
	    continue;
	  } else			/* just modechange */
	    ch->mode &= ~mf;
	  n++;				/* one more accepted */
	  *++imp = *c;			/* it has '-' or last */
	  if (par)
	    passed[x++] = par;		/* one more param accepted */
	}
      }
    }
    if (n)				/* there were some changes */
      _ircd_mode_broadcast ((IRCD *)srv->data, id, src, ch, imp, pp, token,
			    modepass, passed, x);
    /* broadcast cancelled modes now */
    x = 0;
    modepass[0] = '-';
    imp = modepass;
    for (ml = _imch_cancel.masks; ml; ml = ml->next) {
      while (ml->list != NULL) {
	register MASK *mm;

	if (x == MAXMODES) {
	  _ircd_mode_broadcast((IRCD *)srv->data, -1, NULL, ch, imp, NULL, 0,
			       modepass, passed, x);
	  imp = modepass;
	  x = 0;
	}
	mm = ml->list;
	ml->list = mm->next;
#if IRCD_MULTICONNECT
	if (id < 0) {		/* it came from RFC2812 server */
#endif
	  *++imp = ml->mch;
	  passed[x++] = mm->what;	/* it will be not changed yet */
#if IRCD_MULTICONNECT
	} else			/* and we trust new server send it itself */
	  dprint(4, "ircd:channels.c: not sending MODE %s -%c %s thinking %s will do it",
		 ch->name, ml->mch, mm->what, pp->p.dname);
#endif
	free_MASK(mm);
      }
    }
    if (x > 0)
      _ircd_mode_broadcast((IRCD *)srv->data, -1, NULL, ch, imp, NULL, 0,
			   modepass, passed, x);
#if IRCD_MULTICONNECT
    if (errors < 0)
      ircd_do_squit(pp->link, pp, "MODE protocol error");
    else
#endif
      for (i = 0; i < errors; i++)
	if (!ircd_recover_done (pp, (i >= MAXTRACKED_MODE_ERRORS) ?
						"MODE error" : lasterror[i]))
	  break; /* return */
  }
  else if ((tgt = ircd_find_client(argv[0], pp)) != NULL &&
	   !CLIENT_IS_SERVER(tgt) &&	/* user mode */
	   tgt == src)			/* RFC2812: user can change only own mode */
  {
    modeflag toadd = 0, todel = 0;
    const char *lasterror[MAXTRACKED_MODE_ERRORS];
    int errors;

    n = errors = 0;
    for (i = 1; i < argc; i++)		/* parse modes */
    {
      const char *c;
      int add = -1;

      for (c = argv[i]; *c; c++)
      {
	if (*c == '+')			/* adding */
	  add = 1;
	else if (*c == '-')		/* removing */
	  add = 0;
	else if (add < 0)		/* invalid */
	{
	  ERROR ("ircd:unknown MODE char %c for %s via %s", *c, tgt->nick,
		 pp->p.dname);
	  if (!ircd_recover_done (pp->p.iface->data, "unknown MODE char"))
	    return 1;
	}
	else				/* valid */
	{
	  struct binding_t *b;
	  modeflag mf = 0;
	  char charstr[2];
#define static register
	  BINDING_TYPE_ircd_umodechange ((*f));
#undef static
	  void (*ma)(INTERFACE *, const char *, char *, const char *, size_t, int, const char *);

	  charstr[0] = *c;
	  charstr[1] = '\0';
	  b = Check_Bindtable (BTIrcdUmodechange, charstr, U_ALL, U_ANYCH, NULL);
	  if (!b)
	  {
	    New_Request (pp->p.iface, 0, "MODE %s %c%c", tgt->nick,
			 add ? '-' : '+', *c); /* revert it */
	    CONTINUE_ON_MODE_ERROR ("unknown MODE char", " %c for %s via %s",
				    *c, tgt->nick, pp->p.dname);
	  }
	  ma = NULL;
	  while (b)			/* cycle thru all */
	  {
	    if (!b->name && (f = (modeflag (*)())b->func))
	      mf |= f ((src->umode | A_SERVER), add, &ma);
	    b = Check_Bindtable (BTIrcdUmodechange, charstr, U_ALL, U_ANYCH, b);
	  }
	  if (!mf)
	  {
	    New_Request (pp->p.iface, 0, "MODE %s %c%c", tgt->nick,
			 add ? '-' : '+', *c); /* revert it */
	    CONTINUE_ON_MODE_ERROR ("impossible MODE", " %s %c%c via %s",
				    tgt->nick, add ? '+' : '-', *c,
				    pp->p.dname);
	  }
	  mf &= ~(A_ISON | A_PINGED);
	  if (!mf) {
	    Add_Request(I_LOG, "*", F_WARN, "ircd: umode change %c%c on %s ignored",
			add ? '+' : '-', *c, tgt->nick);
	    continue;
	  }
	  if (mf & A_HALFOP) {		/* remote cannot switch localop flag */
	    CONTINUE_ON_MODE_ERROR ("impossible MODE", " %s %c%c via %s",
				    tgt->nick, add ? '+' : '-', *c,
				    pp->p.dname);
	  }
	  if (add)
	  {
	    mf &= ~tgt->umode;		/* reset modes that we have already */
	    if (!mf)
	      continue;
	    if (ma)			/* update vhost */
	      ma (srv, tgt->nick, tgt->vhost, tgt->host, sizeof(tgt->vhost),
		  add, tgt->cs->nick);
	    n++;			/* one more accepted */
	    if (!CLIENT_IS_REMOTE(tgt))
	      New_Request (tgt->via->p.iface, 0, ":%s MODE %s +%c", sender,
			   tgt->nick, *c);
	    toadd |= mf;
	    todel &= ~mf;
	    tgt->umode |= mf;
	    continue;
	  }
	  mf &= tgt->umode;		/* reset modes that we don't have */
	  if (!mf)
	    continue;
	  if (ma)			/* update vhost */
	    ma (srv, tgt->nick, tgt->vhost, tgt->host, sizeof(tgt->vhost),
		add, tgt->cs->nick);
	  n++;				/* one more accepted */
	  if (!CLIENT_IS_REMOTE(tgt))
	    New_Request (tgt->via->p.iface, 0, ":%s MODE %s -%c", sender,
			 tgt->nick, *c);
	  todel |= mf;
	  toadd &= ~mf;
	  tgt->umode &= ~mf;
	}
      }
    }
    if (toadd | todel)			/* we have something changed */
    {
      char *c = modepass;

      if (toadd)
      {
	*c++ = '+';
	ircd_make_umode (c, toadd, MAXMODES+1);
	c += strlen (c);
      }
      if (todel)
      {
	*c++ = '-';
	ircd_make_umode (c, todel, MAXMODES+1);
      }
#ifdef USE_SERVICES
      /* notify local services too */
      if ((toadd & A_OP) || (todel & A_OP))
        ircd_sendto_services_mark_all ((IRCD *)srv->data,
				       SERVICE_WANT_UMODE | SERVICE_WANT_OPER);
      else
        ircd_sendto_services_mark_all ((IRCD *)srv->data, SERVICE_WANT_UMODE);
#endif
#if IRCD_MULTICONNECT
      if (id < 0 && !CLIENT_IS_LOCAL(src))
	id = ircd_new_id(src->cs); /* allocate id for IMODE */
      else if (id < 0)
	id = ircd_new_id(NULL); /* make new id if it's local client */
#endif
      ircd_sendto_servers_old(((IRCD *)srv->data), pp, ":%s MODE %s %s",
			      sender, tgt->nick, modepass);
      ircd_sendto_servers_new(((IRCD *)srv->data), pp, ":%s IMODE %d %s %s",
			      sender, id, tgt->nick, modepass);
    }
    for (i = 0; i < errors; i++)
      if (!ircd_recover_done (pp, (i >= MAXTRACKED_MODE_ERRORS) ?
						"MODE error" : lasterror[i]))
	break; /* return */
  }
  else					/* unrecognized target */
  {
    ERROR ("ircd:MODE command for unknown \"%s\" via %s", argv[0], pp->p.dname);
    ircd_recover_done (pp->p.iface->data, "bogus MODE target");
  }
  return 1;
}

BINDING_TYPE_ircd_server_cmd(ircd_mode_sb);
static int ircd_mode_sb(INTERFACE *srv, struct peer_t *peer, unsigned short token,
			const char *sender, const char *lcsender,
			int argc, const char **argv)
{ /* args: <target> modes... */
  struct peer_priv *pp;

  if (peer == NULL) /* invalid internal call */
    return 0;
  pp = peer->iface->data; /* it's really peer */
  if (argc < 2)
  {
    ERROR ("ircd:incomplete MODE command by %s via %s", sender, peer->dname);
    return ircd_recover_done (pp, "incomplete MODE command");
  }
#if IRCD_MULTICONNECT
  if (pp->link->cl->umode & A_MULTI)
  {
    ERROR ("ircd:illegal MODE command via %s", peer->dname);
    return ircd_recover_done (pp, "illegal MODE command");
  }
#endif
  return _ircd_do_smode(srv, pp, token, -1, sender, lcsender, argc, argv);
}

#if IRCD_MULTICONNECT
BINDING_TYPE_ircd_server_cmd(ircd_imode);
static int ircd_imode(INTERFACE *srv, struct peer_t *peer, unsigned short token,
		      const char *sender, const char *lcsender,
		      int argc, const char **argv)
{ /* args: <id> <target> modes... */
  struct peer_priv *pp;
  int id;

  if (peer == NULL) /* invalid internal call */
    return 0;
  pp = peer->iface->data; /* it's really peer */
  if (!(pp->link->cl->umode & A_MULTI))
    return (0);			/* it is ambiguous from RFC2813 servers */
  if (argc < 3) {
    ERROR ("ircd:incomplete IMODE command by %s via %s", sender, peer->dname);
    return ircd_recover_done (pp, "incomplete MODE command");
  }
  id = atoi(argv[0]);
  if (!ircd_test_id(((IRCD *)srv->data)->token[token], id))
    //TODO: log duplicate?
    return (1);
  return _ircd_do_smode(srv, pp, token, id, sender, lcsender, argc - 1,
			&argv[1]);
}
#endif /* IRCD_MULTICONNECT */
#undef __TRANSIT__
#define __TRANSIT__

#undef CONTINUE_ON_MODE_ERROR


/* ---------------------------------------------------------------------------
 * Logging facility.
 */

#define _IRCD_LOGGER_STEP 8

typedef struct {
  CHANNEL *m;
  flag_t fl;
} __ircd_logger;

static __ircd_logger *_ircd_internal_logger_list = NULL;
static int _ircd_internal_logger_list_n = 0;
static int _ircd_internal_logger_list_a = 0;

static INTERFACE *_ircd_internal_logger = NULL;
static IRCD *_ircd_internal_logger_ircd;

/* system channels */
static CLIENT ME = { .umode = A_ISON, .via = NULL, .c.hannels = NULL };

static int _ircd_internal_logger_req (INTERFACE *i, REQUEST *req)
{
  if (req)
  {
    int i;
    register __ircd_logger *log;
//    const char *sender;

//    if (req-> from && req->from->name)
//      sender = req->from->name;
//    else
//      sender = "server";
    for (i = 0; i < _ircd_internal_logger_list_n; i++)
      if ((log = &_ircd_internal_logger_list[i])->fl & req->flag)
//	ircd_sendto_chan_local (log->m, ":%s NOTICE %s :%s", sender,
//				log->m->name, req->string);
	ircd_sendto_chan_local (log->m, ":server NOTICE %s :%s", log->m->name,
				req->string);
  }
  return REQ_OK;
}

static iftype_t _ircd_internal_logger_sig (INTERFACE *i, ifsig_t sig)
{
  if (sig == S_TERMINATE && i)
  {
    while (ME.c.hannels)
      ircd_del_from_channel (_ircd_internal_logger_ircd, ME.c.hannels, 0);
    FREE (&_ircd_internal_logger_list);
    _ircd_internal_logger_list_a = _ircd_internal_logger_list_n = 0;
    i->ift = I_DIED;
  }
  return 0;
}

static void _ircd_log_channel (IRCD *ircd, const char *name, const char *topic,
			       flag_t fl)
{
  MEMBER *memb;
  register __ircd_logger *log;

  /* create channel and add ME to it */
  dprint(5, "ircd:channels.c:_ircd_log_channel: adding system channel %s", name);
  memb = ircd_new_to_channel (ircd, NULL, name, &ME,
			      A_INVISIBLE | A_MODERATED | A_TOPICLOCK |
			      A_QUIET | A_ANONYMOUS | A_NOOUTSIDE);
  if (!memb)
  {
    ERROR ("ircd:duplicate _ircd_log_channel for %s", name);
    return;
  }
  //add it into logger array
  if (!_ircd_internal_logger)
  {
    _ircd_internal_logger_ircd = ircd;
    _ircd_internal_logger = Add_Iface (I_LOG, "*", &_ircd_internal_logger_sig,
				       &_ircd_internal_logger_req, NULL);
  }
  if (_ircd_internal_logger_list_a == _ircd_internal_logger_list_n)
  {
    _ircd_internal_logger_list_a += _IRCD_LOGGER_STEP;
    safe_realloc ((void **)&_ircd_internal_logger_list,
		  _ircd_internal_logger_list_a * sizeof(__ircd_logger));
  }
  log = &_ircd_internal_logger_list[_ircd_internal_logger_list_n++];
  log->m = memb->chan;
  strfcpy(log->m->topic, topic, sizeof(log->m->topic));
  log->fl = fl;
}


/* ---------------------------------------------------------------------------
 * Common external functions.
 */

/* returns whochar for first appropriate mode within Ircd_modechar_list */
char *ircd_mode2whochar (modeflag mf, char *buf, size_t sb)
{
  size_t p = 0;

  if (mf & Ircd_modechar_mask)
  {
    char wm[16];
    register char ch;
    int i;

    _ircd_make_wmode (wm, mf, sizeof(wm)); /* make list of modes of mf */
    for (i = 0; (ch = Ircd_modechar_list[i]); i++) /* scan Ircd_modechar_list */
      if (Ircd_whochar_list[i] != ' ')	/* if it's supported */
	if (strchr (wm, ch))		/* and present in mf */
	  if (p + 1 < sb)
	    buf[p++] = Ircd_whochar_list[i];
  }
  buf[p] = '\0';
  return buf;
}

/* adds to existing channel and does local broadcast */
MEMBER *ircd_add_to_channel (IRCD *ircd, struct peer_priv *bysrv, CHANNEL *ch,
			     CLIENT *cl, modeflag mf)
{
  MEMBER *memb;
  size_t sz;
  int i, n;
  modeflag modeadd;
  char smode[sizeof(Ircd_modechar_list)+1];
  char madd[MESSAGEMAX];

  if (!ch || !cl)
  {
    dprint(3, "ircd:ircd_add_to_channel: %p to %p: NULL!", cl, ch);
    return NULL;
  }
  dprint(5, "ircd:channels.c:ircd_add_to_channel: %s to %s", cl->nick, ch->name);
#if IRCD_MULTICONNECT
  if (bysrv && ircd_check_ack(bysrv, cl, ch))
    return NULL;			/* duplicate, ignoring */
  if (bysrv && ircd_check_ack(bysrv, cl, CHANNEL0))
    return NULL;			/* we sent JOIN 0, ignoring */
#endif
  if (_ircd_is_on_channel (cl, ch))
  {
    dprint(4, "ircd:ircd_add_to_channel: %s already is on %s!", cl->nick, ch->name);
    return NULL;
  }
  if ((mf & A_ADMIN) && ch->creator)	/* another creator? abort it */
  {
    ERROR ("ircd: attempt to set %s as creator of %s while there was another one %s",
	   cl->nick, ch->name, ch->creator->who->nick);
    if (bysrv)
      if (!ircd_recover_done (bysrv, "duplicate channel creator"))
	return NULL; /* it could squit it of course */
    mf &= ~A_ADMIN;
  }
  memb = alloc_MEMBER();		/* set new MEMBER */
  memb->who = cl;
  memb->chan = ch;
  memb->mode = A_ISON | (mf & Ircd_modechar_mask);
  memb->prevchan = cl->c.hannels;
  memb->prevnick = ch->users;
  cl->c.hannels = memb;
  ch->users = memb;
  ch->count++;
#ifdef USE_SERVICES
  if (ch->count == 1)			/* newly created channel */
    ircd_sendto_services_all (ircd, SERVICE_WANT_CHANNEL | SERVICE_WANT_VCHANNEL,
			      "CHANNEL %s %hd", ch->name, ch->count);
  else
    ircd_sendto_services_all (ircd, SERVICE_WANT_VCHANNEL, "CHANNEL %s %hd",
			      ch->name, ch->count);
#endif
  if (mf & A_ADMIN)			/* support for ! channels */
    ch->creator = memb;
  modeadd = (mf & ~(A_ISON | Ircd_modechar_mask | ch->mode));
  ch->mode |= A_ISON | (mf & ~Ircd_modechar_mask);
  if (memb->mode & A_OP)		/* operator added so reset this */
    ch->noop_since = 0;
  ch->hold_upto = 0;			/* it's now active so reset this */
  if (!(ch->mode & A_QUIET))		/* notify users */
  {
    if(ch->mode & A_ANONYMOUS)
    {
      if (!CLIENT_IS_ME(cl) && !CLIENT_IS_REMOTE(cl))
	New_Request (cl->via->p.iface, 0, ":%s!%s@%s JOIN %s", cl->nick,
		     cl->user, cl->vhost, ch->name);
      ircd_sendto_chan_butone (ch, cl, ":anonymous!anonymous@anonymous. JOIN %s",
			       ch->name); /* broadcast for users */
    }
    else
    {
      _ircd_make_wmode (smode, memb->mode, sizeof(smode)); /* mode chars */
      for (i = 0, n = strlen (smode), sz = 0; i < n && sz < sizeof(madd)-3; i++)
      {
	madd[sz++] = ' ';		/* add " nick" there */
	sz += strfcpy (&madd[sz], cl->nick, sizeof(madd) - sz);
      }
      ircd_sendto_chan_local (ch, ":%s!%s@%s JOIN %s", cl->nick, cl->user,
			      cl->vhost, ch->name); /* broadcast for users */
      if (*smode) {			/* we have a mode provided */
	/* don't send client mode to client as NAMES will do it */
	if (bysrv)
	  ircd_sendto_chan_butone(ch, cl, ":%s MODE %s +%s%s",
				  bysrv->link->cl->nick, ch->name, smode,
				  madd);
	else
	  ircd_sendto_chan_butone(ch, cl, ":%s!%s@%s MODE %s +%s%s", cl->nick,
				  cl->user, cl->vhost, ch->name, smode, madd);
      }
      madd[0] = 0;
      if (modeadd && ch->count > 1)	/* mode of channel was updated */
	_ircd_mode2cmode (madd, modeadd, sizeof(madd)); /* make channel mode */
      if (madd[0]) {
#ifdef USE_SERVICES
	ircd_sendto_services_nick(ircd, SERVICE_WANT_MODE, ":%s MODE %s +%s",
				  cl->nick, ch->name, madd);
	ircd_sendto_services_mark_prefix(ircd, SERVICE_WANT_MODE);
#endif
	if (bysrv)
	  ircd_sendto_chan_butone(ch, cl, ":%s MODE %s +%s",
				  bysrv->link->cl->nick, ch->name, madd);
	else
	  ircd_sendto_chan_butone(ch, cl, ":%s!%s@%s MODE %s +%s", cl->nick,
				  cl->user, cl->vhost, ch->name, madd);
      }
    }
  }
  else if (CLIENT_IS_LOCAL(cl))		/* notify only sender */
    New_Request (cl->via->p.iface, 0, ":%s!%s@%s JOIN %s", cl->nick, cl->user,
		 cl->vhost, ch->name);
  return memb;
}

/* simply adds a client to channel, does not checks anything */
MEMBER *ircd_new_to_channel (IRCD *ircd, struct peer_priv *bysrv, const char *name,
			     CLIENT *cl, modeflag mf)
{
  char lcname[MB_LEN_MAX*CHANNAMELEN+1];
  CHANNEL *ch;
  register MEMBER *memb;

  dprint(5, "ircd:channels.c:ircd_new_to_channel: %s to %s", cl->nick, name);
  unistrlower (lcname, name, sizeof(lcname));
  _ircd_validate_channel_name (lcname);
  ch = _ircd_find_channel_lc (ircd, lcname);
  if (!ch)
    ch = _ircd_new_channel (ircd, name, lcname);
#if IRCD_MULTICONNECT
  if (ch->count == 0 && ch->hold_upto != 0) /* channel was hold by acks */
  {
//    strfcpy (ch->name, name, sizeof(ch->name)); /* update name now */
//    _ircd_validate_channel_name (ch->name);
    ch->mode = 0;			/* see ircd_del_from_channel() */
    Add_Request (I_LOG, "*", F_WARN,	/* send warning */
		 "ircd: got an user %s to holded channel %s (%s)", cl->nick,
		 ch->name, name);
  }
#endif
  memb = ircd_add_to_channel (ircd, bysrv, ch, cl, mf);
  if (!ch->mode) /* fresh empty channel should be removed */
    ircd_drop_channel (ircd, ch);
  return memb;
}

#define CLEAR_MASKS(a) for (; (x = a); free_MASK (x)) a = x->next

/* removes MEMBER, runs bindings, should be called after broadcast */
void ircd_del_from_channel (IRCD *ircd, MEMBER *memb, int tohold)
{
  register MEMBER **m;
#define static register
  BINDING_TYPE_ircd_channel ((*f));
#undef static

  dprint(5, "ircd:channels.c:ircd_del_from_channel");
  if (memb == memb->chan->creator)	/* support for ! channels */
    memb->chan->creator = NULL;
  for (m = &memb->who->c.hannels; *m && *m != memb; m = &(*m)->prevchan);
  if (*m)				/* remove channel from user */
    *m = memb->prevchan;
  else
    ERROR ("ircd:ircd_del_from_channel: not found channel %s on %s",
	   memb->chan->name, memb->who->nick);
  for (m = &memb->chan->users; *m && *m != memb; m = &(*m)->prevnick);
  if (*m)				/* remove user from channel */
  {
    modeflag mf;
    register struct binding_t *b;

    *m = memb->prevnick;
    if (memb->mode & (A_OP | A_ADMIN)) { /* check if it was last OP left */
      register MEMBER *op;

      for (op = memb->chan->users; op; op = op->prevnick)
	if (op->mode & (A_OP | A_ADMIN))
	  break;
      if (op == NULL)
      {
	memb->chan->noop_since = Time;
	Add_Timer (ircd->iface, S_WAKEUP, REOP_DELAY); // FIXME: add a random?
      }
    }
    if (tohold) {			/* it's split, mark it now! */
      if (memb->chan->name[0] == '!')	/* special support for safe channels */
	memb->chan->hold_upto = Time + _ircd_hold_period;
      else
	memb->chan->hold_upto = memb->chan->noop_since + _ircd_hold_period;
    }
    mf = 0;				/* in case of unknown type */
    b = Check_Bindtable (BTIrcdChannel, memb->chan->fc, U_ALL, U_ANYCH, NULL);
    if (b && !b->name)			/* run internal binding */
    {
      register INTERFACE *u;

      f = (modeflag (*)())b->func;
      if (CLIENT_IS_ME (memb->who) || CLIENT_IS_REMOTE (memb->who))
	u = NULL;
      else
	u = memb->who->via->p.iface;
      mf = f (u, memb->who->umode, memb->chan->mode, memb->chan->count,
	      memb->chan->name, ircd->channels, NULL);
    }
    //TODO: check if channel mode was changed and there are local users left
    if ((--memb->chan->count) == 0 && mf == 0 && /* want to delete channel */
	Time >= memb->chan->hold_upto)
    {
      MASK *x;
      CMASKL *l;

#ifdef USE_SERVICES
      ircd_sendto_services_all (ircd, SERVICE_WANT_CHANNEL | SERVICE_WANT_VCHANNEL,
				"CHANNEL %s 0", memb->chan->name);
#endif
      while ((l = memb->chan->masks))
      {
	memb->chan->masks = l->next;
	CLEAR_MASKS(l->list);
	free_CMASKL(l);
      }
#if IRCD_MULTICONNECT
//      memb->chan->name[0] = '\0';	/* mark it for later */
      if (memb->chan->on_ack)
	memb->chan->hold_upto = Time;	/* hold it while acks on it */
      else
#endif
	ircd_drop_channel (ircd, memb->chan);
    }
    else
    {
      memb->chan->mode = mf;
#ifdef USE_SERVICES
      if (memb->chan->count == 0)	/* emptied channel */
	ircd_sendto_services_all (ircd, SERVICE_WANT_CHANNEL | SERVICE_WANT_VCHANNEL,
				  "CHANNEL %s 0", memb->chan->name);
      else
	ircd_sendto_services_all (ircd, SERVICE_WANT_VCHANNEL, "CHANNEL %s %hd",
				  memb->chan->name, memb->chan->count);
#endif
    }
  }
  else
    ERROR ("ircd:ircd_del_from_channel: not found %s on channel %s",
	   memb->who->nick, memb->chan->name);
  free_MEMBER (memb);
}

MEMBER *ircd_find_member (IRCD *ircd, const char *chan, CLIENT *client)
{
  register CHANNEL *ch = _ircd_find_channel (ircd, chan);
  register CLIENT *cl;
  register MEMBER *m;

  if (!ch || !ch->users) /* either not exist or on hold */
    return NOSUCHCHANNEL;
  if (!(cl = client))
    return ch->users;
  for (m = ch->users; m; m = m->prevnick)
    if (m->who == cl)
      return m;
  return NULL;
}

void ircd_add_invited (CLIENT *cl, CHANNEL *ch)
{
  register MEMBER *memb;

  if (CLIENT_IS_ME(cl) || CLIENT_IS_REMOTE (cl))
    return;
  for (memb = ch->invited; memb; memb = memb->prevnick)
    if (memb->who == cl)
      return;
  memb = alloc_MEMBER();
  memb->who = cl;
  memb->chan = ch;
  memb->prevchan = cl->via->i.nvited;
  memb->prevnick = ch->invited;
  cl->via->i.nvited = memb;
  ch->invited = memb;
}

/* removes user from channel and marks I_PENDING for further notify */
void ircd_quit_all_channels (IRCD *ircd, CLIENT *cl, int tohold, int isquit)
{
  register MEMBER *td;
  MEMBER *ch;

  /* do it with anonymous channels at first and send PART to local users */
  if (isquit)
    for (ch = cl->c.hannels; ch; ch = ch->prevchan)
      if ((ch->chan->mode & (A_ANONYMOUS | A_QUIET)) == A_ANONYMOUS)
      {
	for (td = ch->chan->users; td; td = td->prevnick) /* ignore cl and me */
	  if (td != ch && CLIENT_IS_LOCAL(td->who))
	    td->who->via->p.iface->ift |= I_PENDING; /* it needs notify */
	Add_Request (I_PENDING, "*", 0, /* PART instead of QUIT, RFC2811 */
		     ":anonymous!anonymous@anonymous. PART %s :anonymous",
		     ch->chan->name);
      }
  /* and now work with non-anonymous and non-quiet channels, mark them */
  for (ch = cl->c.hannels; ch; ch = ch->prevchan)
    if (!(ch->chan->mode & (A_ANONYMOUS | A_QUIET)))
      for (td = ch->chan->users; td; td = td->prevnick)
	if (td != ch && CLIENT_IS_LOCAL(td->who))
	  td->who->via->p.iface->ift |= I_PENDING; /* it needs notify */
  /* remove from list of invited too */
  if (CLIENT_IS_LOCAL(cl))
    while (cl->via->i.nvited)
      _ircd_del_from_invited (cl->via->i.nvited);
  if (!isquit)
    return;
  /* and at last remove user from channels */
  while (cl->c.hannels)
    ircd_del_from_channel (ircd, cl->c.hannels, tohold); /* remove from list */
}

/* removes channel structure which was on hold, it should have no acks ATM */
void ircd_drop_channel (IRCD *ircd, CHANNEL *ch)
{
  register MASK *x;
  CMASKL *l;

  dprint (5, "ircd:ircd_drop_channel %s", ch->lcname); /* ch->name undefined */
  if (ch->count || ch->users)
    ERROR ("ircd:ircd_drop_channel: count=%d, users=%p", ch->count, ch->users);
  while ((l = ch->masks))
  {
    ch->masks = l->next;
    CLEAR_MASKS(l->list);
    free_CMASKL(l);
  }
  while (ch->invited)
    _ircd_del_from_invited (ch->invited);
  if (ircd && Delete_Key (ircd->channels, ch->lcname, ch))
    ERROR("ircd:ircd_drop_channel: tree error on removing %s", ch->lcname);
    //TODO: isn't it fatal?
  else
    dprint(2, "ircd:channels.c:ircd_drop_channel: del chan %s", ch->lcname);
  free_CHANNEL (ch);
}

#define IRCD_SET_MODECHAR(f,s,c) \
if (f) do { \
  register modeflag mf = 1; \
  register size_t shift = 0; \
  while (mf && shift < sizeof(s)) \
    if (f & mf) { \
      s[shift] = c; \
      break; } \
    else { \
      shift++; \
      mf <<= 1; } } while(0)

/* internal sub-functions */
static inline char *_ircd_ch_flush_umodes (INTERFACE *i, char *c, char *e)
{
  modeflag mode = 0;
  struct binding_t *b = NULL;
#define static register
  BINDING_TYPE_ircd_umodechange ((*ff));
#undef static
  void (*ma)(INTERFACE *, const char *, char *, const char *, size_t, int, const char *);

  while (!mode && (b = Check_Bindtable (BTIrcdUmodechange, c, U_ALL, U_ANYCH, b)))
    if (!b->name)
    {
      ff = (modeflag (*)())b->func;
      mode = (ff (0, 0, &ma) & ~(A_ISON | A_PINGED));
      IRCD_SET_MODECHAR (mode, _ircd_umodes, *c);
    }
  if (mode && c < e)
    c++;
  return c;
}

static inline char *_ircd_ch_flush_cmodes (INTERFACE *i, char *c, char *e)
{
  Function dummy;
  modeflag mode1 = 0, mode2 = 0;
  struct binding_t *b = NULL;
  register _mch_func_t ff;

  while ((b = Check_Bindtable (BTIrcdModechange, c, U_ALL, U_ANYCH, b)))
    if (!b->name)
    {
      ff = (_mch_func_t)b->func;
      /* channel modes: make _ircd_cmodes */
      mode1 |= ff (0, 0, NULL, 0, 0, 1, '\0', &dummy);
      /* user in channel modes: make Ircd_modechar_mask */
      mode2 |= ff (0, 0, "", 0, 0, 0, '\0', &dummy);
    }
  if (mode1 & ~(A_ISON | A_PINGED))
  {
    strfcat(_ircd_isupport_modestring, c, sizeof(_ircd_isupport_modestring));
    if (mode1 & A_ISON)
      strfcat(_ircd_isupport_modestring, ",", sizeof(_ircd_isupport_modestring));
  }
  mode1 &= ~(A_ISON | A_PINGED);
  IRCD_SET_MODECHAR (mode1, _ircd_cmodes, *c);
  mode2 &= ~(A_ISON | A_PINGED);
  Ircd_modechar_mask |= mode2;
  IRCD_SET_MODECHAR (mode2, _ircd_wmodes, *c);
  if ((mode1 || mode2) && c < e)
    c++;
  return c;
}

/* updates strings for fast access */
void ircd_channels_flush (IRCD *ircd, char *modestring, size_t s)
{
  int i;
  struct binding_t *b = NULL;
  char ch;
  char *c, *e;
  _wch_func_t f;

  for (i = 0; Ircd_modechar_list[i]; i++)
    Ircd_whochar_list[i] = ' ';		/* clear list of channel whochars */
  if (ircd->iface)
    c = ircd->iface->name;
  else
    c = "*";
  while ((b = Check_Bindtable (BTIrcdWhochar, c, U_ALL, U_ANYCH, b)))
    if (!b->name) /* do internal only */
      for (i = 0; Ircd_modechar_list[i]; i++)
	if ((ch = (f = (_wch_func_t)b->func) (Ircd_modechar_list[i])))
	  Ircd_whochar_list[i] = ch;	/* update list of channel whochars */
  /* make modes for 004 - including 'O' hidden mode for ! channels */
  memset (_ircd_umodes, 0, sizeof(_ircd_umodes));
  c = modestring;
  e = &c[s-1];
  /* also make modes for 005 */
  _ircd_isupport_modestring[0] = '\0';
  for (ch = 'a'; ch <= 'z'; ch++)	/* do user modes */
  {
    *c = ch;
    c[1] = 0;
    c = _ircd_ch_flush_umodes (ircd->iface, c, e);
    *c = ch - 'a' + 'A'; /* uppercase one */
    c[1] = 0;
    c = _ircd_ch_flush_umodes (ircd->iface, c, e);
  }
  memset (_ircd_cmodes, 0, sizeof(_ircd_cmodes));
  memset (_ircd_wmodes, 0, sizeof(_ircd_wmodes));
  Ircd_modechar_mask = A_ADMIN;		/* 'O' is present */
  if (c < e)
    *c++ = ' '; /* space between user modes and channel modes */
  IRCD_SET_MODECHAR (A_ADMIN, _ircd_wmodes, 'O');
  if (c <= e) for (ch = 'a'; ch <= 'z'; ch++) /* do channel modes */
  {
    *c = ch;
    c[1] = 0;
    c = _ircd_ch_flush_cmodes (ircd->iface, c, e);
    *c = ch - 'a' + 'A'; /* uppercase one */
    c[1] = 0;
    if (ch == 'o') /* 'O' matches in any case */
    {
      if (c < e)
	c++;
    }
    else
      c = _ircd_ch_flush_cmodes (ircd->iface, c, e);
  }
  *c = '\0'; /* terminate the string */
}

#if IRCD_MULTICONNECT
#define UPDATE_BUFF if (mf & A_MULTI) \
    t = snprintf (buff, sizeof(buff), "IMODE %d %s +", ircd_new_id(NULL), ch->name);
#else
#define UPDATE_BUFF
#endif

void ircd_burst_channels (struct peer_t *peer, IRCD *ircd, modeflag mf)
{
  LEAF *leaf;
  CHANNEL *ch;
  MEMBER *m;
  MASK *mask;
  CMASKL *ml;
  size_t s, l, t;
  struct binding_t *b = NULL;
#define static register
  BINDING_TYPE_ircd_eob ((*ff));
#undef static
  char buff[MB_LEN_MAX*IRCMSGLEN+1];
  char mb[8];

  leaf = Next_Leaf (ircd->channels, NULL, NULL);
  while (leaf)
  {
    ch = leaf->s.data;
    leaf = Next_Leaf (ircd->channels, leaf, NULL);
    if (ch->hold_upto && ch->count == 0) /* it's unavailable */
      continue;
    if (ch->mode & A_INVISIBLE)		/* local channel */
      continue;
    dprint(5, "ircd:channels.c:ircd_burst_channels: send channel %s", ch->name);
    m = ch->users;
    while (m)				/* do NJOIN */
    {
      l = snprintf (buff, sizeof(buff), "NJOIN %s :", ch->name); /* start size */
      while (m)
      {
	t = strlen (m->who->nick) + 3;	/* projected max size */
	s = unistrcut (buff, sizeof(buff) - t - 1, IRCMSGLEN - 2 - t); /* a room */
	if (s < l)			/* insufficient space */
	  break;
	if (buff[l-1] != ':')
	  buff[l++] = ',';
	if (m->mode & A_ADMIN)		/* creator */
	  s = snprintf (&buff[l], sizeof(buff) - l, "@@%s", m->who->nick);
	else
	  s = snprintf (&buff[l], sizeof(buff) - l, "%s%s",
			ircd_mode2whochar(m->mode, mb, sizeof(mb)), m->who->nick);
	l += s;
	m = m->prevnick;
      }
      New_Request (peer->iface, 0, "%s", buff);
    }
    if (ch->name[0] == '+')		/* modeless channel */
      continue;
    if (ch->mode != A_ISON || ch->limit || ch->key[0]) /* do not-mask modes */
    {
      _ircd_make_cmode (buff, sizeof(buff), ch, 1);
#if IRCD_MULTICONNECT
      if (mf & A_MULTI)
	New_Request (peer->iface, 0, "IMODE %d %s +%s", ircd_new_id(NULL), ch->name, buff);
      else
#endif
      New_Request (peer->iface, 0, "MODE %s +%s", ch->name, buff);
    }
    l = s = 0;				/* do masks */
#if IRCD_MULTICONNECT
    if (mf & A_MULTI)
      t = snprintf (buff, sizeof(buff), "IMODE %d %s +", ircd_new_id(NULL), ch->name);
    else
#endif
    t = snprintf (buff, sizeof(buff), "MODE %s +", ch->name); /* CCC\0 */
    buff[t + MAXMODES] = '\0';
    for (ml = ch->masks; ml; ml = ml->next)
    {
      for (mask = ml->list; mask; mask = mask->next)
      {
        size_t k = strlen (mask->what);
        if (t + k + l + s > IRCMSGLEN - 4 || /* reserving mode char and space */
	    l >= MAXMODES)
        {
	  New_Request (peer->iface, 0, "%.*s%s", (int)(t + l), buff,
		       &buff[t + MAXMODES + 1]);
	  UPDATE_BUFF
	  l = s = 0;
        }
        buff[t + l] = ml->mch;
        l++;
        buff[t + MAXMODES + 1 + s] = ' '; /* " a!b@c" */
        strfcpy (&buff[t + MAXMODES + 2 + s], mask->what, k + 1);
        s += (k + 1);
      }
    }
    if (l)
      New_Request (peer->iface, 0, "%.*s%s", (int)(t + l), buff, &buff[t + MAXMODES + 1]);
    while ((b = Check_Bindtable (BTIrcdEOB, ch->name, U_ALL, U_ANYCH, b)))
      if (!b->name && (ff = (void(*)())b->func)) /* do internal only */
	ff (ircd->iface, peer, ch->name, ch->mode, (leaf == NULL));
  }
}
#undef UPDATE_BUFF

void ircd_channels_report (INTERFACE *to)
{
  //TODO.......
}

static inline void _ircd_do_reop(IRCD *ircd, CLIENT *me, CHANNEL *ch)
{
  MEMBER *who = ch->users;
  const char *cmask;

  ch->noop_since = 0;			/* operator is adding so reset this */
  //TODO: find local client with the least idle
  //TODO: if not found then op some random user
  who->mode |= A_OP;
  ircd_sendto_chan_local(ch, ":%s MODE %s +o %s", me->lcnick, ch->name,
			 who->who->nick);
  /* every server should know channel state too */
  cmask = strchr (ch->name, ':');
  if (cmask) {
    cmask++;
    ircd_sendto_servers_mask_new(ircd, NULL, cmask, ":%s IMODE %d %s +o %s",
				 me->lcnick, ircd_new_id(NULL), ch->name,
				 who->who->nick);
    ircd_sendto_servers_mask_old(ircd, NULL, cmask, ":%s MODE %s +o %s",
				 me->lcnick, ch->name, who->who->nick);
    return;
  }
  ircd_sendto_servers_new(ircd, NULL, ":%s IMODE %d %s +o %s", me->lcnick,
			  ircd_new_id(NULL), ch->name, who->who->nick);
  ircd_sendto_servers_old(ircd, NULL, ":%s MODE %s +o %s", me->lcnick, ch->name,
			  who->who->nick);
}

void ircd_channels_chreop(IRCD *ircd, CLIENT *me)
{
  LEAF *l = NULL;

  while ((l = Next_Leaf(ircd->channels, l, NULL)) != NULL) {
    register CHANNEL *ch = l->s.data;

    if ((ch->mode & A_REOP) && ch->users != NULL && ch->noop_since != 0 &&
	Time > ch->noop_since + REOP_DELAY)
	//TODO: add random time (0...REOP_DELAY) to that delay!
      _ircd_do_reop(ircd, me, ch);
  }
}

/* args: buf, umode, sizeof(buf); returns buf */
char *ircd_make_umode (char *buf, modeflag umode, size_t bufsize)
{
  _ircd_make_Xmode (_ircd_umodes, buf, umode, bufsize);
  return buf;
}

modeflag ircd_char2umode(INTERFACE *srv, const char *sname, char c, CLIENT *tgt)
{
  struct binding_t *b;
  modeflag mf = 0;
  char charstr[2];
#define static register
  BINDING_TYPE_ircd_umodechange ((*f));
#undef static
  void (*ma)(INTERFACE *, const char *, char *, const char *, size_t, int, const char *);

  charstr[0] = c;
  charstr[1] = '\0';
  ma = NULL;
  b = Check_Bindtable (BTIrcdUmodechange, charstr, U_ALL, U_ANYCH, NULL);
  while (b)				/* cycle thru all */
  {
    if (!b->name)
      mf |= (f = (modeflag (*)())b->func) (A_SERVER, 1, &ma);
    b = Check_Bindtable (BTIrcdUmodechange, charstr, U_ALL, U_ANYCH, b);
  }
  if (ma)
    ma (srv, tgt->nick, tgt->vhost, tgt->host, sizeof(tgt->vhost), 1, sname);
  return (mf & ~(A_ISON | A_PINGED));
}

modeflag ircd_char2mode(INTERFACE *srv, const char *sname, const char *tar,
			const char *chname, char c)
{
  const char *p;
  struct binding_t *b;
#define static register
  BINDING_TYPE_ircd_channel ((*ff));
#undef static
  register _mch_func_t f;
  int (*ma)(INTERFACE *, const char *, const char *, int, const char **,
	    MASK **(*)(CHANNEL *, char), CHANNEL *);
  modeflag mf;
  char chfc[2];

  chfc[1] = '\0';
  if (c == '\0') {
    chfc[0] = chname[0];
    b = Check_Bindtable(BTIrcdChannel, chfc, U_ALL, U_ANYCH, NULL);
    if (b == NULL || b->name != NULL)
      return 0;
    return ((ff = (modeflag (*)())b->func)(NULL, 0, 0, 0, chname,
				   ((IRCD *)srv->data)->channels, &p));
  }
  chfc[0] = c;
  if (!(p = strchr (Ircd_modechar_list, c)) /* check for compliance */
      || Ircd_whochar_list[p-Ircd_modechar_list] == ' ')
    return ((c == 'O') ? A_ADMIN : 0); /* 'O' is creator by default */
  mf = 0;
  b = Check_Bindtable (BTIrcdModechange, chfc, U_ALL, U_ANYCH, NULL);
  while (b)				/* cycle thru all */
  {
    if (!b->name)
    {
      f = (_mch_func_t)b->func;
      mf |= f((A_OP|A_ADMIN), A_SERVER, tar, (modeflag)0, (modeflag)0, 1, chname[0], &ma);
    }
    b = Check_Bindtable (BTIrcdModechange, chfc, U_ALL, U_ANYCH, b);
  }
  /* we don't call ma() as it is a servermode on joining channel, no mask */
  return (mf & ~(A_ISON | A_PINGED));
}

modeflag ircd_whochar2mode(char ch)
{
  register char *ptr;
  register size_t i;

  ptr = strchr(Ircd_whochar_list, ch);
  if (ptr == NULL)
    return (0);
  ch = Ircd_modechar_list[ptr-Ircd_whochar_list];
  for (i = 0; i < sizeof(_ircd_wmodes); i++)
    if (_ircd_wmodes[i] == ch)
      return (1 << i);
  return (0);
}

#define STR(x) NUMSTR(x)
#define NUMSTR(x) #x

void send_isupport(IRCD *ircd, CLIENT *cl)
{
  char isupport[2*MESSAGEMAX];
  char buff[MESSAGEMAX];
  size_t n, s, ptr, len;
  struct binding_t *b = NULL;

  /* prepare own strings first */
  strfcpy(isupport, "PREFIX=(", sizeof(isupport));
  len = strlen(isupport);
  for (n = 0, ptr = 0; Ircd_modechar_list[n]; n++)
    if (Ircd_whochar_list[n] != ' ')
    {
      isupport[len+ptr] = Ircd_modechar_list[n];
      buff[ptr++] = Ircd_whochar_list[n];
    }
  isupport[len+ptr] = ')';
  buff[ptr++] = '\0';
  strfcpy(&isupport[len+ptr], buff, sizeof(isupport)-len-ptr);
  strfcat(isupport, " CHANTYPES=", sizeof(isupport));
  len = strlen(isupport);
  buff[1] = '\0';
  for (buff[0] = 0x21/* '!' */; buff[0] < 'A'; buff[0]++)
    if (Check_Bindtable(BTIrcdChannel, buff, U_ALL, U_ANYCH, NULL))
      isupport[len++] = buff[0];
  isupport[len] = '\0';
  snprintf(buff, sizeof(buff), " CHANMODES=%s MODES=" STR(MAXMODES)
			       " MAXCHANNELS=%ld NICKLEN=%u NICKTEST=%s"
			       " MAXLIST=beI:%ld NETWORK=%s EXCEPTS=e INVEX=I"
			       " CASEMAPPING=utf-8 TOPICLEN=" STR(TOPICLEN)
			       " CHANNELLEN=" STR(CHANNAMELEN)
			       " IDCHAN=!:" STR(CHIDLEN) " RFC2812 SAFELIST",
	   _ircd_isupport_modestring, _ircd_max_channels, _ircd_nicklen,
#if IRCD_STRICT_NAMES
	   CHARSET_8BIT,
#else
	   Charset,
#endif
	   _ircd_max_bans, ircd->iface->name);
  strfcat(isupport, buff, sizeof(isupport));
  len = ptr = 0;
  while (1)
  {
    while (isupport[ptr])
    {
      s = ptr;
      for (n = 0; n < 12 && isupport[ptr]; n++)
      {
	len = NextWord(&isupport[ptr]) - &isupport[s];
	if (len > 400)
	  break;
	ptr = len + s;
      }
      len = ptr;
      ptr = s;
      if (n < 12 && !isupport[len])	/* buffer exhausted */
	break;
      s = len;
      while (s > ptr && isupport[s-1] == ' ')
	s--;
      isupport[s] = '\0';
      ircd_do_unumeric(cl, RPL_ISUPPORT, cl, 0, &isupport[ptr]);
      ptr = len;
    }
    if (len > ptr)
      memmove(isupport, &isupport[ptr], len - ptr);
    len -= ptr;
    isupport[len] = '\0';
    ptr = 0;
    while ((b = Check_Bindtable(BTIrcdIsupport, ircd->iface->name, U_ALL, U_ANYCH, b)))
      if (!b->name)
	break;
    if (b == NULL)			/* finished all */
      break;
    if (len > 0)
      isupport[len++] = ' '; /* separator */
    isupport[len] = '\0'; /* at least 2*MESSAGEMAX-400 left so don't check */
    b->func(&isupport[len], sizeof(isupport) - len);
    //TODO: check for duplicates probably?
  }
  /* send leftovers */
  if (isupport[ptr])
    ircd_do_unumeric(cl, RPL_ISUPPORT, cl, 0, &isupport[ptr]);
}

static void _ircd_catch_undeleted_ch (void *ch)
{
  ERROR ("ircd:_ircd_catch_undeleted_ch: channel %s with %d users",
	 ((CHANNEL *)ch)->name, ((CHANNEL *)ch)->count);
  ircd_drop_channel (NULL, ch);
}

		/* ircd-set-channel-topic <network> <channel> [<time> <who>] <topic> */
static int _ircd_set_channel_topic(const char *topic)
{
  char who[MB_LEN_MAX*NICKLEN+1];
  char *c;
  long time;
  size_t len;
  INTERFACE *iface;
  IRCD *ircd;
  CHANNEL *ch;

  c = NextWord_Unquoted(who, (char *)topic, sizeof(who));
  iface = Find_Iface(I_SERVICE, who);
  if (iface == NULL)
  {
    ERROR ("ircd-set-channel-topic: network %s isn't known", who);
    return (0);
  }
  ircd = iface->data;
  Unset_Iface();
  c = NextWord_Unquoted(who, c, sizeof(who));
  ch = _ircd_find_channel(ircd, who);
  if (ch == NULL)
  {
    ERROR ("ircd-set-channel-topic: channel %s isn't formed", who);
    return (0);
  }
  time = strtoul(c, &c, 10);
  if (time > 0)			/* <time> <who> present */
  {
    c = NextWord_Unquoted(who, NextWord(c), sizeof(who));
#ifdef TOPICWHOTIME
    len = unistrcut(who, sizeof(ch->topic_by), NICKLEN+IDENTLEN+HOSTLEN+2);
    strfcpy(ch->topic_by, who, len + 1);
    ch->topic_since = time;
#endif
  }
  len = unistrcut(c, sizeof(ch->topic), TOPICLEN);
  strfcpy(ch->topic, c, len + 1);
  return (1);
}

/* common end and start of channel protocol */
void ircd_channel_proto_end (NODE **tree)
{
  Delete_Binding ("ircd-whochar", (Function)&iwc_ircd, NULL);
  Delete_Binding ("ircd-channel", (Function)&ich_normal, NULL);
  Delete_Binding ("ircd-channel", (Function)&ich_add, NULL);
  Delete_Binding ("ircd-channel", (Function)&ich_excl, NULL);
  Delete_Binding ("ircd-client-cmd", &ircd_mode_cb, NULL);
  Delete_Binding ("ircd-server-cmd", (Function)&ircd_mode_sb, NULL);
#if IRCD_MULTICONNECT
  Delete_Binding ("ircd-server-cmd", (Function)&ircd_imode, NULL);
#endif
  Delete_Binding ("ircd-client-cmd", &ircd_join_cb, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_o, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_v, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_a, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_i, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_m, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_n, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_q, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_p, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_s, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_r, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_t, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_k, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_l, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_b, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_e, NULL);
  Delete_Binding ("ircd-modechange", (Function)&imch_I, NULL);
  Delete_Binding ("ircd-umodechange", (Function)&iumch_a, NULL);
  Delete_Binding ("ircd-umodechange", (Function)&iumch_i, NULL);
  Delete_Binding ("ircd-umodechange", (Function)&iumch_w, NULL);
  Delete_Binding ("ircd-umodechange", (Function)&iumch_r, NULL);
  Delete_Binding ("ircd-umodechange", (Function)&iumch_o, NULL);
  Delete_Binding ("ircd-umodechange", (Function)&iumch_O, NULL);
  Delete_Binding ("ircd-umodechange", (Function)&iumch_s, NULL);
  Delete_Binding ("ircd-umodechange", (Function)&iumch_z, NULL);
  Delete_Binding ("ircd-check-modechange", &ichmch_r, NULL);
  UnregisterFunction ("ircd-set-channel-topic");
  _ircd_internal_logger_sig (_ircd_internal_logger, S_TERMINATE); /* stop &* */
  Destroy_Tree (tree, &_ircd_catch_undeleted_ch);
  _forget_(CHANNEL);
  _forget_(MEMBER);
  _forget_(MASK);
  _forget_(CMASKL);
}

void ircd_channel_proto_start (IRCD *ircd)
{
  BTIrcdWhochar = Add_Bindtable ("ircd-whochar", B_MASK);
  BTIrcdChannel = Add_Bindtable ("ircd-channel", B_UNIQ);
  BTIrcdModechange = Add_Bindtable ("ircd-modechange", B_MATCHCASE);
  BTIrcdUmodechange = Add_Bindtable ("ircd-umodechange", B_MATCHCASE);
  BTIrcdCheckModechange = Add_Bindtable ("ircd-check-modechange", B_MASK);
  BTIrcdIsupport = Add_Bindtable ("ircd-isupport", B_MASK);
  BTIrcdEOB = Add_Bindtable ("ircd-eob", B_MASK);
  Add_Binding ("ircd-whochar", "*", 0, 0, (Function)&iwc_ircd, NULL);
  /* default 4 channel types, see RFC2811 */
  Add_Binding ("ircd-channel", "&", 0, 0, (Function)&ich_normal, NULL);
  Add_Binding ("ircd-channel", "#", 0, 0, (Function)&ich_normal, NULL);
  Add_Binding ("ircd-channel", "+", 0, 0, (Function)&ich_add, NULL);
  Add_Binding ("ircd-channel", "!", 0, 0, (Function)&ich_excl, NULL);
  /* those two do manipulate bindtables which are defined here */
  Add_Binding ("ircd-client-cmd", "mode", 0, 0, &ircd_mode_cb, NULL);
  Add_Binding ("ircd-server-cmd", "mode", 0, 0, (Function)&ircd_mode_sb, NULL);
#if IRCD_MULTICONNECT
  Add_Binding ("ircd-server-cmd", "imode", 0, 0, (Function)&ircd_imode, NULL);
#endif
  /* this one needs to check channel parameters before adding user */
  Add_Binding ("ircd-client-cmd", "join", 0, 0, &ircd_join_cb, NULL);
  /* default channel modes, see RFC2812 */
  Add_Binding ("ircd-modechange", "o", 0, 0, (Function)&imch_o, NULL);
  Add_Binding ("ircd-modechange", "v", 0, 0, (Function)&imch_v, NULL);
  Add_Binding ("ircd-modechange", "a", 0, 0, (Function)&imch_a, NULL);
  Add_Binding ("ircd-modechange", "i", 0, 0, (Function)&imch_i, NULL);
  Add_Binding ("ircd-modechange", "m", 0, 0, (Function)&imch_m, NULL);
  Add_Binding ("ircd-modechange", "n", 0, 0, (Function)&imch_n, NULL);
  Add_Binding ("ircd-modechange", "q", 0, 0, (Function)&imch_q, NULL);
  Add_Binding ("ircd-modechange", "p", 0, 0, (Function)&imch_p, NULL);
  Add_Binding ("ircd-modechange", "s", 0, 0, (Function)&imch_s, NULL);
  Add_Binding ("ircd-modechange", "r", 0, 0, (Function)&imch_r, NULL);
  Add_Binding ("ircd-modechange", "t", 0, 0, (Function)&imch_t, NULL);
  Add_Binding ("ircd-modechange", "k*", 0, 0, (Function)&imch_k, NULL);
  Add_Binding ("ircd-modechange", "l*", 0, 0, (Function)&imch_l, NULL);
  Add_Binding ("ircd-modechange", "b*", 0, 0, (Function)&imch_b, NULL);
  Add_Binding ("ircd-modechange", "e*", 0, 0, (Function)&imch_e, NULL);
  Add_Binding ("ircd-modechange", "I*", 0, 0, (Function)&imch_I, NULL);
  /* default user modes, see RFC2812 */
  Add_Binding ("ircd-umodechange", "a", 0, 0, (Function)&iumch_a, NULL);
  Add_Binding ("ircd-umodechange", "i", 0, 0, (Function)&iumch_i, NULL);
  Add_Binding ("ircd-umodechange", "w", 0, 0, (Function)&iumch_w, NULL);
  Add_Binding ("ircd-umodechange", "r", 0, 0, (Function)&iumch_r, NULL);
  Add_Binding ("ircd-umodechange", "o", 0, 0, (Function)&iumch_o, NULL);
  Add_Binding ("ircd-umodechange", "O", 0, 0, (Function)&iumch_O, NULL);
  Add_Binding ("ircd-umodechange", "s", 0, 0, (Function)&iumch_s, NULL);
  Add_Binding ("ircd-umodechange", "z", 0, 0, (Function)&iumch_z, NULL);
  Add_Binding ("ircd-check-modechange", "*", 0, 0, &ichmch_r, NULL);
  /* grant access to topic for other modules */
  RegisterFunction ("ircd-set-channel-topic", &_ircd_set_channel_topic, NULL);
  /* create common local channels */
  _ircd_log_channel (ircd, "&KILLS",
		     "SERVER MESSAGES: operator and server kills", F_MODES);
  _ircd_log_channel (ircd, "&NOTICES",
		     "SERVER MESSAGES: warnings and notices", F_WARN);
  _ircd_log_channel (ircd, "&ERRORS",
		     "SERVER MESSAGES: server errors", F_ERROR);
  _ircd_log_channel (ircd, "&LOCAL",
		     "SERVER MESSAGES: notices about local connections", F_CONN);
//  _ircd_log_channel (ircd, "&CHANNEL",
//		     "SERVER MESSAGES: fake modes", F_JOIN);
//  _ircd_log_channel ("&HASH", "SERVER MESSAGES: hash tables growth", ?????);
//  _ircd_log_channel ("&NUMERICS", "SERVER MESSAGES: numerics received", ?????);
  _ircd_log_channel (ircd, "&SERVERS",
		     "SERVER MESSAGES: servers joining and leaving", F_SERV);
  _ircd_log_channel (ircd, "&WALLOPS",
		     "WALLOPS MESSAGES: supermouse-only", F_WALL);
  _ircd_internal_logger_list[_ircd_internal_logger_list_n-1].m->mode |= A_SECRET;
// "&SERVICES", "SERVER MESSAGES: services joining and leaving"
// "&AUTH", "SERVER MESSAGES: messages from the authentication slave"
// "&ISERV", "SERVER MESSAGES: messages from the configuration slave" -- A_SECRET
// "&OPER", "SERVER MESSAGES: opers-only notices" -- A_SECRET
  //TODO: BTIrcdSystemChannel -- create empty log channel
}
#endif
