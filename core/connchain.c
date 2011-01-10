/*
 * Copyright (C) 2008-2011  Andrej N. Gritsenko <andrej@rep.kiev.ua>
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
 * This file is part of FoxEye's source: connection chain API and "x" filter.
 *      Bindtables: connchain-grow
 */

#include "foxeye.h"
#include "socket.h"
#include "direct.h"
#include "init.h"

/* ---------------------------------------------------------------------------
 * Common data typedef and last link handlers for connchain.
 */

struct connchain_i			/* internal use only */
{
  ssize_t (*recv) (connchain_i **, idx_t, char *, size_t, connchain_buffer **);
  ssize_t (*send) (connchain_i **, idx_t, const char *, size_t *, connchain_buffer **);
  connchain_buffer *buf;		/* instance specific buffer */
  connchain_i *next;			/* for collector and chain */
  char tc;
};

bindtable_t *BT_CChain;

ALLOCATABLE_TYPE (connchain_i, _CC_, next) /* alloc_connchain_i(), free_... */

/* send raw data into socket, *chain and *b are undefined here */
static ssize_t _connchain_send (connchain_i **chain, idx_t idx,
				const char *data, size_t *sz, connchain_buffer **b)
{
  ssize_t i, ptr = 0;

  if (data == NULL)		/* if it's NULL then we have to terminate */
    i = E_NOSOCKET;
  else if (*sz == 0)		/* if it's a test then answer we are ready */
    return CONNCHAIN_READY;
  else
    i = WriteSocket (idx, data, &ptr, sz, M_RAW);
  if (i < 0)
  {
    DBG ("connchain: send: socket error %d", (int)i);
    CloseSocket (idx);
  }
  else if (i)
    dprint (5, "put to peer %d:[%-*.*s]", (int)idx, (int)i, (int)i, data);
  return i;
}

/* receive raw data from socket, *chain and *b are undefined here */
static ssize_t _connchain_recv (connchain_i **chain, idx_t idx,
				char *data, size_t sz, connchain_buffer **b)
{
  ssize_t i = E_NOSOCKET;

  if (data != NULL)		/* if it's NULL then we have to stop */
    i = ReadSocket (data, idx, sz, M_RAW);
  if (i < 0)
  {
    DBG ("connchain: recv: socket error %d", (int)i);
    CloseSocket (idx);
  }
  else if (i)
    dprint (5, "got from peer %d:[%-*.*s]", (int)idx, (int)i, (int)i, data);
  return i;
}

/* allocate starting link structure */
static void _connchain_create (peer_t *peer)
{
  peer->connchain = alloc_connchain_i();
  peer->connchain->recv = &_connchain_recv;
  peer->connchain->send = &_connchain_send;
  /* ignoring ->buf since we don't need it for _this_ link */
  peer->connchain->next = NULL;
  peer->connchain->tc = 0;
  dprint (2, "connchain.c: created initial link %p", peer->connchain);
}

/* ---------------------------------------------------------------------------
 * Management of connchains.
 */

int Connchain_Grow (peer_t *peer, char c)
{
  connchain_i *chain, *chk;
  char tc[2];
  binding_t *b;

  if (peer->socket < 0)
    return -1;				/* nothing to do */
  if (!peer->connchain)
    _connchain_create (peer);		/* it's first call */
  if (c == 0)
    return 1;				/* idle call */
  for (chk = peer->connchain; chk; chk = chk->next)
  {
    if (chk->tc == c)
    {
      dprint (3, "connchain.c: link %c seems duped.", c);
      return -1;			/* ignore duplicate */
    }
  }
  tc[0] = c;
  tc[1] = 0;
  b = NULL;				/* start from scratch */
  chain = alloc_connchain_i();		/* allocate link struct */
  while ((b = Check_Bindtable (BT_CChain, tc, U_ALL, U_ANYCH, b)))
    if (!b->name &&			/* internal only? trying to init */
	b->func (peer, &chain->recv, &chain->send, &chain->buf))
      break;				/* found a working binding! */
  if (b == NULL)			/* no handler found there! */
  {
    dprint (3, "connchain.c: link %c not found.", c);
    free_connchain_i (chain);		/* unalloc struct then */
    return 0;
  }
  chain->next = peer->connchain;	/* it's done OK, insert into chain */
  chain->tc = c;
  peer->connchain = chain;
  dprint (2, "connchain.c: created link %c: %p", c, chain);
  return 1;
}

int Connchain_Check (peer_t *peer, char c)
{
  connchain_i *chk;
  char tc[2];
  binding_t *b;
  ssize_t (*recv) (connchain_i **, idx_t, char *, size_t, connchain_buffer **);
  ssize_t (*send) (connchain_i **, idx_t, const char *, size_t *, connchain_buffer **);

  if (c == 0)
    return 1;				/* idle call */
  for (chk = peer->connchain; chk; chk = chk->next)
    if (chk->tc == c)
      return -1;			/* ignore duplicate */
  tc[0] = c;
  tc[1] = 0;
  b = NULL;				/* start from scratch */
  while ((b = Check_Bindtable (BT_CChain, tc, U_ALL, U_ANYCH, b)))
    if (!b->name &&			/* internal only? trying to init */
	b->func (peer, &recv, &send, NULL))
      break;				/* found a working binding! */
  if (b == NULL)			/* no handler found there! */
    return 0;
  return 1;
}

/*
 * state	peer->socket	peer->connchain	peer->c.->recv	peer->c.->send
 *-----------------------------------------------------------------------------
 * initial	n		NULL		-		-
 * first	n		somechain	NULL		NULL
 * working	n		somechain	somerecv	somesend
 * dying	-1		somechain	somerecv	somesend/NULL
 * died		-1		_connchain_null	-		-
 */

ssize_t Connchain_Put (connchain_i **chain, idx_t idx, const char *buf, size_t *sz)
{
  ssize_t i;

  if (chain == NULL || *chain == NULL)		/* it's error or dead */
    return E_NOSOCKET;			
  /* ok, it seems we got something to run */
  i = (*chain)->send (&(*chain)->next, idx, buf, sz, &(*chain)->buf);
  if (i >= 0)					/* everything seems OK */
    return i;
  if ((i = Connchain_Put (&(*chain)->next, idx, NULL, 0)) < 0)
    return i;					/* next link finished */
  return 0;
}

ssize_t Connchain_Get (connchain_i **chain, idx_t idx, char *buf, size_t sz)
{
  ssize_t i;

  if (chain == NULL || *chain == NULL)		/* it's error or dead */
    return E_NOSOCKET;	
  /* ok, it seems we got something to run */
  i = (*chain)->recv (&(*chain)->next, idx, buf, sz, &(*chain)->buf);
  if (i >= 0)					/* everything seems OK */
    return i;
  if(Connchain_Get (&(*chain)->next, idx, NULL, 0))i=i; /* it's dead, kill next */
  dprint (2, "connchain.c: destroying link %p", *chain);
  free_connchain_i (*chain);
  *chain = NULL;
  return i;
}

/* ---------------------------------------------------------------------------
 * connchain filter 'x': M_RAW -> M_TEXT.
 */

typedef struct				/* one side buffer for filter 'x' */
{
  ssize_t inbuf;			/* how much bytes are in buf */
  size_t bufpos;			/* where is first char */
  char buf[2*MB_LEN_MAX*MESSAGEMAX];	/* message buffer */
} connchain_b;

struct connchain_buffer			/* local buffer type for filter 'x' */
{
  connchain_b in;			/* incoming message buffer */
  connchain_b out;			/* outgoing message buffer */
  peer_t *peer;				/* for notification */
};

/* get full line, add CR+LF, put into buffer and send it */
static ssize_t _ccfilter_x_send (connchain_i **ch, idx_t id, const char *str,
				 size_t *sz, connchain_buffer **b)
{
  connchain_b *bb = &(*b)->out;
  ssize_t i;

  if (bb->inbuf < 0)			/* it's already ended */
    return E_NOSOCKET;
  if (bb->inbuf > 0)			/* there is something to send */
  {
    i = Connchain_Put (ch, id, &bb->buf[bb->bufpos], &bb->inbuf);
    if (i < 0)				/* some error, end us */
    {
      bb->inbuf = (ssize_t)-1;		/* forget the buffer */
      return i;				/* return error */
    }
    if (bb->inbuf != 0)			/* trying to send line by line */
    {
      bb->bufpos += i;			/* there is something left */
      return 0;
    }
  }
  if (str == NULL)			/* got termination */
  {
    bb->inbuf = (ssize_t)-1;		/* forget buffer */
    return E_NOSOCKET;			/* return error */
  }
  i = *sz;
  if (i == 0)				/* it was a test and we are ready */
    return Connchain_Put (ch, id, str, sz); /* bounce test to next link */
  if (i > (ssize_t)sizeof(bb->buf) - 2)	/* line + CR/LF */
    i = sizeof(bb->buf) - 2;
  memcpy (bb->buf, str, i);
  *sz -= i;
  bb->buf[i] = '\r';			/* CR */
  bb->buf[i+1] = '\n';			/* LF */
  bb->inbuf = i + 2;
  bb->bufpos = 0;
  return i;
}

/* returns lenght of found line up to LF or -1 if not found */
static ssize_t _ccfx_find_line (connchain_b *bb)
{
  register char *c;

  if (bb->bufpos + bb->inbuf <= sizeof(bb->buf)) /* all is in one piece */
  {
    if ((c = memchr (&bb->buf[bb->bufpos], '\n', bb->inbuf)))
      return (c - &bb->buf[bb->bufpos]);	/* found it */
    else if (bb->inbuf >= (ssize_t)sizeof(bb->buf) - 1)	/* buffer is full */
      return (sizeof(bb->buf) - 1);
    return -1;					/* try it later */
  }
  if ((c = memchr (&bb->buf[bb->bufpos], '\n', sizeof(bb->buf) - bb->bufpos)))
    return (c - &bb->buf[bb->bufpos]);		/* found in end of buf */
  if ((c = memchr (bb->buf, '\n', bb->bufpos + bb->inbuf - sizeof(bb->buf))))
    return (c - bb->buf);			/* found at start of buf */
  else if (bb->inbuf >= (ssize_t)sizeof(bb->buf) - 1) /* all buffer is filled */
    return (sizeof(bb->buf) - 1);
  return -1;					/* try it later */
}

/* copies found line and reset pointers */
/* it may be weird but if we get full buffer but no LF in it then we
   return it all as the line but last char will be lost */
static ssize_t _ccfx_get_line (connchain_b *bb, ssize_t i, char *str, size_t sz)
{
  register ssize_t x, d;

  if (bb->bufpos + i <= sizeof(bb->buf))	/* it's in one piece */
  {
    d = i + 1;					/* size with LF */
    if (i && bb->buf[bb->bufpos + i - 1] == '\r') /* check for CR */
      x = i;					/* \0 instead of CR */
    else					/* no CR found */
      x = d;					/* \0 instead of LF */
    if (x > (ssize_t)sz)			/* we don't care! */
      x = sz;					/* put as many as we can */
    strfcpy (str, &bb->buf[bb->bufpos], x);	/* with terminating 0 */
    if ((bb->inbuf -= d) == 0)			/* emptied, ignore ->bufpos */
      return x;
    d += bb->bufpos;				/* calculate new bufpos */
    if (d >= (ssize_t)sizeof(bb->buf))
      d -= sizeof(bb->buf);
    bb->bufpos = d;
    return x;
  }
  d = sizeof(bb->buf) - bb->bufpos;		/* piece at end */
  i -= d;					/* left at start */
  bb->inbuf -= (i + 1);				/* prepare now */
  if (d >= (ssize_t)sz)				/* we don't care! */
    d = sz - 1;
  memcpy (str, &bb->buf[bb->bufpos], d);
  sz -= d;					/* how many left in buf */
  if (i && bb->buf[i - 1] == '\r')		/* check for CR */
    x = i;					/* \0 instead of CR */
  else						/* no CR found */
    x = i + 1;					/* \0 instead of LF */
  if (x > (ssize_t)sz)				/* we don't care! */
    x = sz;					/* put as many as we can */
  strfcpy (str, bb->buf, x);			/* with terminating 0 */
  bb->bufpos = i + 1;
  return (x + d);
}

/* trying to get line and return it when CR+LF occured, skipping CR+LF */
static ssize_t _ccfilter_x_recv (connchain_i **ch, idx_t id, char *str,
				 size_t sz, connchain_buffer **b)
{
  connchain_b *bb = &(*b)->in;
  ssize_t i;

  if (str == NULL)			/* termination requested */
  {
    FREE (b);				/* free the buffer struct */
    return E_NOSOCKET;
  }
  if (bb->inbuf && (i = _ccfx_find_line (bb)) >= 0)
    return _ccfx_get_line (bb, i, str, sz); /* pull all lines from buffer */
  if (bb->inbuf == 0)
    bb->bufpos = 0;			/* restart the pointer */
  if ((i = bb->bufpos + bb->inbuf) < (ssize_t)sizeof(bb->buf))
  {
    i = Connchain_Get (ch, id, &bb->buf[i], sizeof(bb->buf) - i);
    if (i < 0)				/* got error */
    {
      if (bb->inbuf)			/* there is something left in buffer */
      {
	if (bb->inbuf >= (ssize_t)sz)
	  i = strfcpy (str, &bb->buf[bb->bufpos], sz);
	else
	  i = strfcpy (str, &bb->buf[bb->bufpos], bb->inbuf + 1);
	bb->inbuf -= i;
	bb->bufpos += i;
	return i + 1;
      }
      FREE (b);				/* terminating */
      return i;
    }
    if (i == 0)				/* no data received yet */
      return 0;
    bb->inbuf += i;			/* update counter */
    return _ccfilter_x_recv (ch, id, str, sz, b);
    /* if we filled rest of buffer then go to next line by return */
  }
  i -= sizeof(bb->buf);			/* buffer is cycled, go to start */
  i = Connchain_Get (ch, id, &bb->buf[i], bb->bufpos - i);
  if (i < 0)				/* got error */
  {
    if (bb->inbuf)			/* there is something left in buffer */
    {
      if ((sizeof(bb->buf) - bb->bufpos) >= sz)
	i = strfcpy (str, &bb->buf[bb->bufpos], sz);
      else
	i = strfcpy (str, &bb->buf[bb->bufpos],
		     sizeof(bb->buf) + 1 - bb->bufpos);
      bb->inbuf -= i;			/* copied from end of buffer */
      if (bb->inbuf)
      {
	register ssize_t sg;

	sz -= i;
	if (bb->inbuf >= (ssize_t)sz)
	  sg = strfcpy (str, bb->buf, sz);
	else
	  sg = strfcpy (str, bb->buf, bb->inbuf + 1);
	bb->inbuf -= sg;
	i += sg;
	bb->bufpos = sg;		/* copied from start of buffer */
      }
      return i + 1;
    }
    FREE (b);				/* terminating */
    return i;
  }
  bb->inbuf += i;			/* update counter */
  if (i != 0 && (i = _ccfx_find_line (bb)) >= 0)
    return _ccfx_get_line (bb, i, str, sz); /* pull line from buffer */
  return 0;				/* still get nothing to give */
}

BINDING_TYPE_connchain_grow(_ccfilter_x_init);
static int _ccfilter_x_init (peer_t *peer,
	ssize_t (**recv) (connchain_i **, idx_t, char *, size_t, connchain_buffer **),
	ssize_t (**send) (connchain_i **, idx_t, const char *, size_t *, connchain_buffer **),
	connchain_buffer **b)
{
  *recv = &_ccfilter_x_recv;
  *send = &_ccfilter_x_send;
  if (b == NULL)
    return 1;
  *b = safe_malloc (sizeof(connchain_buffer));
  (*b)->in.inbuf = (*b)->in.bufpos = (*b)->out.inbuf = (*b)->out.bufpos = 0;
  (*b)->peer = peer;
  return 1;
}

/* simple report */
void Status_Connchains (INTERFACE *iface)
{
  New_Request (iface, F_REPORT, "Connchains: %u in use (max was %u), %zu bytes.",
	       _CC_num, _CC_max, _CC_asize);
}

/* init all this stuff */
void _fe_init_connchains (void)
{
  BT_CChain = Add_Bindtable ("connchain-grow", B_MATCHCASE);
  Add_Binding ("connchain-grow", "x", 0, 0, &_ccfilter_x_init, NULL);
}
