/*
 * Copyright (C) 1999-2002  Andrej N. Gritsenko <andrej@rep.kiev.ua>
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
 * This file contains sheduler interface.
 */

#include "foxeye.h"
#include "sheduler.h"
#include "init.h"

#include <pthread.h>

#define MAXTABLESIZE 20000	/* can i do it for one second? */

typedef struct
{
  short *ptr;			/* if 0 then this cell is empty */
  short timer;
} shedfloodentry_t;

static shedfloodentry_t *Floodtable = NULL;
static unsigned int _SFalloc = 0;
static unsigned int _SFnum = 0;

/* create new cell in Floodtable */
int CheckFlood (short *ptr, short floodtype[2])
{
  if (_SFnum >= MAXTABLESIZE)
    bot_shutdown ("Internal error in CheckFlood()", 8);
  if (_SFnum >= _SFalloc)
  {
    _SFalloc += 32;
    safe_realloc ((void **)&Floodtable, (_SFalloc) * sizeof(shedfloodentry_t));
  }
  (*ptr)++;
  Floodtable[_SFnum].ptr = ptr;
  Floodtable[_SFnum].timer = floodtype[1];
  _SFnum++;
  return ((*ptr) - floodtype[0]);
}

/* delete cell from Floodtable */
void NoCheckFlood (short *ptr)
{
  register int i;

  for (i = 0; i < _SFnum; i++)
    if (Floodtable[i].ptr == ptr)
    {
      _SFnum--;
      if (i != _SFnum)
	memcpy (&Floodtable[i], &Floodtable[_SFnum], sizeof(shedfloodentry_t));
    }
}

/* format: "time[,time,...]" where time is "*[/i]" or "a[-b[/i]]" */
static void _get_mask (char *mask, uint32_t *bitmap)
{
  register long per, first, last;

  bitmap[0] = bitmap[1] = 0;
  while (mask && *mask)
  {
    if (*mask == '*')
    {
      first = 0;
      last = 63;
      mask++;
    }
    else
    {
      first = strtol (mask, &mask, 10);
      if (*mask == '-')
	last = strtol (&mask[1], &mask, 10);
      else
	last = 0;
    }
    if (*mask == '/')
      per = strtol (&mask[1], &mask, 10);
    else
      per = 1;
    while (first >= 0 && first < 64)
    {
      if (first < 32)
	bitmap[0] |= 1<<first;
      else
	bitmap[1] |= 1<<(first-32);
      first += per;
      if (first > last)
	break;
    }
    mask = strchr (mask, ',');
    if (mask)
      mask++;
  }
}

static pthread_mutex_t LockShed = PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
  uint32_t min[2];
  uint32_t hour;
  uint32_t day;
  uint16_t month;
  uint16_t weekday;
  iface_t iface;		/* if 0 then this cell is empty */
  char *to;
  ifsig_t signal;
} shedentry_t;

static shedentry_t *Crontable = NULL;
static unsigned int _SCalloc = 0;
static unsigned int _SCnum = 0;

/* create new cell in Crontable */
void NewShedule (iface_t ift, char *name, ifsig_t sig,
		char *min, char *hr, char *ds, char *mn, char *wk)
{
  register shedentry_t *ct;
  uint32_t mask[2];

  pthread_mutex_lock (&LockShed);
  if (_SCnum >= MAXTABLESIZE)
    bot_shutdown ("Internal error in NewShedule()", 8);
  if (_SCnum >= _SCalloc)
  {
    _SCalloc += 32;
    safe_realloc ((void **)&Crontable, (_SCalloc) * sizeof(shedentry_t));
  }
  ct = &Crontable[_SCnum];
  ct->iface = ift;
  ct->to = name;
  ct->signal = sig;
  _get_mask (min, ct->min);
  _get_mask (hr, mask);
  ct->hour = mask[0];
  _get_mask (ds, mask);
  ct->day = mask[0];
  _get_mask (mn, mask);
  ct->month = (uint16_t)mask[0];
  _get_mask (wk, mask);
  ct->weekday = (uint16_t)mask[0];
  _SCnum++;
  pthread_mutex_unlock (&LockShed);
}

/* delete cell from Crontable */
void KillShedule (iface_t ift, char *name, ifsig_t sig,
		 char *min, char *hr, char *ds, char *mn, char *wk)
{
  register int i;
  register shedentry_t *ct;
  uint32_t mask[2];

  pthread_mutex_lock (&LockShed);
  for (i = 0; i < _SCnum; i++)
  {
    ct = &Crontable[i];
    if (ct->iface == ift && ct->to == name)
    {
      _get_mask (min, mask);
      ct->min[0] &= ~(mask[0]);
      ct->min[1] &= ~(mask[1]);
      _get_mask (hr, mask);
      ct->hour &= ~(mask[0]);
      _get_mask (ds, mask);
      ct->day &= ~(mask[0]);
      _get_mask (mn, mask);
      ct->month &= ~((uint16_t)mask[0]);
      _get_mask (wk, mask);
      ct->weekday &= ~((uint16_t)mask[0]);
      if (!ct->min[0] && !ct->min[1] && !ct->hour && !ct->day && !ct->month &&
	  !ct->weekday)
      {
	_SCnum--;
	if (i != _SCnum)
	  memcpy (ct, &Crontable[_SCnum], sizeof(shedentry_t));
      }
    }
  }
  pthread_mutex_unlock (&LockShed);
}

typedef struct
{
  time_t timer;
  iface_t iface;		/* if 0 then this cell is empty */
  char *to;
  ifsig_t signal;
} shedtimerentry_t;

static shedtimerentry_t *Timerstable = NULL;
static unsigned int _STalloc = 0;
static unsigned int _STnum = 0;

/* create new cell in Timerstable */
void NewTimer (iface_t ift, char *name, ifsig_t sig, unsigned int sec,
	      unsigned int min, unsigned int hr, unsigned int ds)
{
  register shedtimerentry_t *ct;

  pthread_mutex_lock (&LockShed);
  if (_STnum >= MAXTABLESIZE)
    bot_shutdown ("Internal error in NewTimer()", 8);
  if (_STnum >= _STalloc)
  {
    _STalloc += 32;
    safe_realloc ((void **)&Timerstable, (_STalloc) * sizeof(shedtimerentry_t));
  }
  ct = &Timerstable[_STnum];
  ct->iface = ift;
  ct->to = name;
  ct->signal = sig;
  ct->timer = (sec + 60*min + 3600*hr + 86400*ds);
  _STnum++;
  pthread_mutex_unlock (&LockShed);
}

static REQUEST *Sheduler (INTERFACE *iface, REQUEST *req)
{
  register time_t curtime = time (NULL);
  struct tm tm;
  struct tm tm0;
  register unsigned int i, j = 0;

  if (curtime != Time)
  {
    /* decrement floodtimers */
    for (i = 0; i < _SFnum; i++)
    {
      if (Floodtable[i].ptr && (--Floodtable[i].timer) == 0)
      {
	(*Floodtable[i].ptr)--;
	_SFnum--;
	j++;
	if (i != _SFnum)
	  memcpy (&Floodtable[i], &Floodtable[_SFnum], sizeof(shedfloodentry_t));
      }
    }
    if (j)
      dprint (3, "Sheduler: removed %u flood timer(s), remained %u/%u",
	      j, _SFnum, MAXTABLESIZE);
    pthread_mutex_lock (&LockShed);
    /* decrement timers */
    for (i = 0; i < _STnum; i++)
    {
      register shedtimerentry_t *ct = &Timerstable[i];

      if (ct->iface && (--ct->timer) == 0)
      {
	ifsig_t sig = ct->signal;
	iface_t iface = ct->iface;
	char *to = ct->to;

	pthread_mutex_unlock (&LockShed);
	Add_Request (iface, to, F_SIGNAL, (char *)sig);
	pthread_mutex_lock (&LockShed);
	_STnum--;
	if (i != _STnum)
	  memcpy (ct, &Timerstable[_STnum], sizeof(shedtimerentry_t));
      }
    }
    /* update time variables */
    localtime_r (&Time, &tm0);
    Time = curtime;
    localtime_r (&Time, &tm);
    if (tm.tm_min != tm0.tm_min)
    {
      shedentry_t sh;
      ifsig_t ifsig = S_TIMEOUT;

      /* update datestamp */
      if (!strftime (DateString, sizeof(DateString), "%e %b %H:%M", &tm))
	dprint (1, "Cannot form datestamp!");
      /* flush all files */
      Add_Request (I_FILE, "*", F_SIGNAL, (char *)&ifsig);
      /* run Crontable (?TODO: check for missed minutes?) */
      memset (&sh, 0, sizeof(sh));
      if (tm.tm_min > 31)
	sh.min[1] = 1<<(tm.tm_min-32);
      else
	sh.min[0] = 1<<tm.tm_min;
      sh.hour = 1<<tm.tm_hour;
      sh.day = 1<<(tm.tm_mday-1);
      sh.month = 1<<tm.tm_mon;
      sh.weekday = 1<<tm.tm_wday;
      for (i = 0; i < _SCnum; i++)
      {
	register shedentry_t *ct = &Crontable[i];

	if (ct->iface && ((ct->min[0] & sh.min[0]) || (ct->min[1] & sh.min[1])) &&
	    (ct->hour & sh.hour) && (ct->day & sh.day) &&
	    (ct->month & sh.month) && (ct->weekday & sh.weekday))
	{
	  ifsig_t sig = ct->signal;
	  iface_t iface = ct->iface;
	  char *to = ct->to;

	  pthread_mutex_unlock (&LockShed);
	  Add_Request (iface, to, F_SIGNAL, (char *)sig);
	  pthread_mutex_lock (&LockShed);
	}
      }
    }
    pthread_mutex_unlock (&LockShed);
  }
  return NULL;
}

void Status_Sheduler (INTERFACE *iface)
{
  register unsigned int a, b;

  pthread_mutex_lock (&LockShed);
  b = _SCnum;
  a = _STnum;
  pthread_mutex_unlock (&LockShed);
  New_Request (iface, 0, _("Timers: flood %u, once %u, periodic %u."),
	       _SFnum, a, b);
}

static INTERFACE *ShedIface = NULL;

char *IFInit_Sheduler (void)
{
  /* create/reset floodtimers table */
  if (!Floodtable)
  {
    Floodtable = safe_calloc (32, sizeof(shedfloodentry_t));
    _SFalloc = 32;
  }
  else
    _SFnum = 0;
  /* create/reset timers table */
  if (!Timerstable)
  {
    Timerstable = safe_calloc (32, sizeof(shedtimerentry_t));
    _STalloc = 32;
  }
  else
    _STnum = 0;
  /* create/reset Crontable */
  if (!Crontable)
  {
    Crontable = safe_calloc (32, sizeof(shedentry_t));
    _SCalloc = 32;
  }
  else
    _SCnum = 0;
  /* create own interface - I_TEMP forever ;) */
  if (!ShedIface)
    ShedIface = Add_Iface (NULL, I_TEMP, NULL, &Sheduler, NULL);
  Sheduler (NULL, NULL);
  return NULL;
}