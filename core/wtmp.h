/*
 * Copyright (C) 2001-2010  Andrej N. Gritsenko <andrej@rep.kiev.ua>
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
 * This file is part of FoxEye's source: wtmp file API.
 */

#ifndef _WTMP_H
#define _WTMP_H 1

#include "list.h"

/* events */
#define W_ANY -1	/* any event - for FindEvent() */
#define W_END 0		/* "session end" event */
#define W_START 1	/* "session start" event */
#define W_DOWN 2	/* "shutdown" event (also if bot parts channel) */
#define W_CHG 3		/* change fuid to uid */
#define W_DEL 4		/* delete uid */
#define W_USER 5	/* start of user defined events */

struct wtmp_t
{
  lid_t uid;
  lid_t fuid;		/* from (where) uid */
  short count;		/* event specific data */
  short event;
  uint32_t time;	/* to have struct of fixed size */
};

short Event (const char *)
		__attribute__((warn_unused_result));	/* user event name -> event */
int FindEvent (struct wtmp_t *, const char *, short, lid_t, time_t)
		__attribute__((warn_unused_result));	/* Lname, event, from, upto */
int FindEvents (struct wtmp_t *, int, const char *, short, lid_t, time_t)
		__attribute__((warn_unused_result));	/* size, ... */
void NewEvent (short, lid_t, lid_t, short);		/* event, from, lid, count */
void NewEvents (short, lid_t, size_t, lid_t[], short[]);
void RotateWtmp (void);					/* called monthly */

#endif
