/*
 * Copyright (C) 1999-2017  Andrej N. Gritsenko <andrej@rep.kiev.ua>
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
 * This file is part of FoxEye's source: scheduler API.
 */

#ifndef _SCHEDULER_H
#define _SCHEDULER_H 1

typedef int tid_t;

/* script-style schedules */
void NewShedule (iftype_t, const char *, ifsig_t, char *, char *, char *, char *, char *);
void KillShedule (iftype_t, const char *, ifsig_t, char *, char *, char *, char *, char *);
tid_t NewTimer (iftype_t, const char *, ifsig_t, unsigned int, unsigned int, unsigned int, unsigned int);
/* exact interface schedules */
void Add_Schedule (INTERFACE *, ifsig_t, char *, char *, char *, char *, char *);
void Stop_Schedule (INTERFACE *, ifsig_t, char *, char *, char *, char *, char *);
tid_t Add_Timer (INTERFACE *, ifsig_t, time_t);
void KillTimer (tid_t);
int CheckFlood (short *, short[2]);
void NoCheckFlood (short *);

/* should be called from dispatcher on interface freeing */
void _stop_timers (INTERFACE *iface);

#endif /* _SCHEDULER_H */
