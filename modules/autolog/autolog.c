/*
 * Copyright (C) 2006-2017  Andrej N. Gritsenko <andrej@@rep.kiev.ua>
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
 * The FoxEye autolog module - auto creating log files for client traffic.
 *   TODO: "U_SECRET" feature.
 */

#include "foxeye.h"
#include "modules.h"

#include <errno.h>
#include <fcntl.h>

#include <init.h>
#include <sheduler.h>

#define AUTOLOG_LEVELS (F_PUBLIC | F_PRIV | F_JOIN | F_MODES)
#define AUTOLOG_LEVELS2 (F_WARN | F_END)

static char autolog_ctl_prefix[32] = "-|- ";	/* prefix for notices */
static char autolog_path[128] = "~/.foxeye/logs/%@/%N";
static char autolog_serv_path[128] = "~/.foxeye/logs/%@.netlog";
static char autolog_open[64];			/* set on init */
static char autolog_close[64];
static char autolog_daychange[64];
static char autolog_timestamp[32] = "[%H:%M] ";	/* with ending space */
static char autolog_lname_prefix[8] = "=";
static bool autolog_by_lname = TRUE;
static long int autolog_autoclose = 600;	/* in seconds */


typedef struct
{
  char *path;
  int fd;
  tid_t timer;
  time_t timestamp;
  int reccount;
  int day;
  char *lname;
  size_t inbuf;
  char buf[HUGE_STRING];
} autologdata_t;

typedef struct autolog_t
{
  autologdata_t *d;
  struct autolog_t *prev;
  INTERFACE *iface;
} autolog_t;

typedef struct autolognet_t
{
  struct autolognet_t *prev;
  INTERFACE *net;
  autolog_t *log;
} autolognet_t;


/* ----------------------------------------------------------------------------
 *	"name@network" autolog interface - handles opened logs
 */

static iftype_t _autolog_nolog_s (INTERFACE *iface, ifsig_t sig)
{
  switch (sig)
  {
    case S_FLUSH:
    case S_TERMINATE:
    case S_SHUTDOWN:
      ((autolog_t *)iface->data)->iface = NULL;
      iface->data = NULL;
      return I_DIED;
    default: ;
  }
  return 0;
}

static int flush_autolog (autologdata_t *log)
{
  ssize_t x;
  struct flock lck;
  int es;

  if (log->inbuf == 0)
    return 0;
  if (log->fd < 0)
    return EBADF;
  dprint (5, "autolog: trying logfile %s: %zu bytes", log->path, log->inbuf);
  memset (&lck, 0, sizeof (struct flock));
  lck.l_type = F_WRLCK;
  lck.l_whence = SEEK_END;
  if (fcntl (log->fd, F_SETLK, &lck) < 0)
    return errno;		/* cannot lock the file */
  lseek (log->fd, 0, SEEK_END);
  x = write (log->fd, log->buf, log->inbuf);
  es = errno;
  lck.l_type = F_UNLCK;
  fcntl (log->fd, F_SETLK, &lck);
  if (x < 0)
    return es;			/* fatal error on write! */
  log->inbuf = 0;
  return 0;
}

#define MAXLOGQUEUE 50

static int __flush_autolog (autolog_t *log, int quiet)
{
  int x = flush_autolog (log->d);			/* try to write it */

  if (x == EACCES || x == EAGAIN)			/* file locked */
  {
    if (log->iface->qsize > MAXLOGQUEUE) /* check if it's locked too far ago */
    {
      if (!quiet)
	ERROR ("Logfile %s is locked but queue grew to %d, abort logging to it.",
	       log->d->path, log->iface->qsize);
      return -1;
    }
    return 0;
  }
  else if (x)						/* fatal error! */
  {
    if (!quiet)
    {
#if _GNU_SOURCE
      register const char *str = strerror_r (x, log->d->buf, sizeof(log->d->buf));
      ERROR ("Couldn't write to logfile %s (%s), abort logging to it.",
             log->d->path, str);
#else
      if (strerror_r(x, log->d->buf, sizeof(log->d->buf)) != 0)
        snprintf(log->d->buf, sizeof(log->d->buf), "(failed to decode err=%d)", x);
      ERROR ("Couldn't write to logfile %s (%s), abort logging to it.",
	     log->d->path, log->d->buf);
#endif
    }
    return -1;
  }
  log->d->timestamp = Time;				/* all is OK */
  return 1;
}

/* inline substitution to disable warnings */
#if __GNUC__ >= 4
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static inline size_t __strftime (char *l, size_t s, char *f, struct tm *t)
{
  return strftime (l, s, f, t);
}
#if __GNUC__ >= 4
#pragma GCC diagnostic error "-Wformat-nonliteral"
#endif

/*
 * puts text to logfile with optional timestamp ts and prefix
 * prefix if skipped if sp==0
 * if text=="" then do nothing
 */
static int autolog_add (autolog_t *log, char *ts, char *text, size_t sp,
			struct tm *tm, int quiet)
{
  size_t ptr, sz, sts;

  if (log->d->inbuf && __flush_autolog (log, quiet) < 0)
    return -1;		/* error happened */
  if (text && text[0] == 0)
    return 1;		/* nothing to put */
  DBG ("autolog:autolog_add: to=\"%s\" text=\"%s%s%s\"", log->d->path, ts, sp ? autolog_ctl_prefix : "", NONULL(text));
  sz = safe_strlen (text);
  ptr = log->d->inbuf;
  if (ptr + sp + sz + safe_strlen (ts) + 1 >= sizeof(log->d->buf))
    return 0;		/* try assuming that timestamp is ts long */
  if (*ts)						/* do timestamp */
  {
    sts = __strftime (&log->d->buf[ptr], sizeof(log->d->buf) - ptr - 1, ts, tm);
    if (sts >= sizeof(log->d->buf) - ptr)
      sts = sizeof(log->d->buf) - 1;
  }
  else
    sts = 0;
  if (ptr + sp + sz + sts + 1 >= sizeof(log->d->buf))
    return 0;		/* so we know full size so check it */
  if (sp)						/* do prefix */
    memcpy (&log->d->buf[ptr+sts], autolog_ctl_prefix, sp);
  if (sz)						/* do message itself */
    memcpy (&log->d->buf[ptr+sts+sp], text, sz);
  ptr += sts + sp + sz;
  log->d->buf[ptr++] = '\n';
  log->d->inbuf = ptr;
  if (__flush_autolog (log, quiet) < 0)
    return -1;
  DBG ("autolog:autolog_add: success");
  return 1;	/* it already in buffer */
}

static int _autolog_makepath (char *buf, size_t sb, char *net, const char *tgt,
			      size_t st, const char *prefix, struct tm *tm)
{
  char *c, *t, *tc;
  size_t sn, s, sp;
  char templ[PATH_MAX+1];

  sn = safe_strlen (net);
  sp = safe_strlen (prefix);
  tc = NULL;
  c = templ;
  if (!st)					/* tgt is service */
    t = autolog_serv_path;
  else
    t = autolog_path;
  do
  {
    if (&c[1] >= &templ[sizeof(templ)])		/* no space for a char */
      return -1;
    if (t[0] == '%' && t[1] == 0)		/* correcting wrong syntax */
      t[0] = 0;
    if (t[0] == '%')
    {
      if (t[1] == '@')
      {
	s = &templ[sizeof(templ)] - c;
	if (tc)
	{
	  *t = 0;
	  s = __strftime (c, s, tc, tm);
	  *t = '%';
	  c += s;
	  s = &templ[sizeof(templ)] - c;
	}
	if (sn >= s)
	  return -1;				/* no space for network name */
	memcpy (c, net, sn);
	c += sn;
      }
      else if (t[1] == 'N')
      {
	s = &templ[sizeof(templ)] - c;
	if (tc)
	{
	  *t = 0;
	  s = __strftime (c, s, tc, tm);
	  *t = '%';
	  c += s;
	  s = &templ[sizeof(templ)] - c;
	}
	if (st + sp >= s)
	  return -1;				/* no space for target name */
	if (sp)
	  memcpy (c, prefix, sp);
	c += sp;
	if (st)
	  memcpy (c, tgt, st);
	c += st;
      }
      else if (t[1] == '%')			/* "%%" --> "%" */
      {
	if (!tc)				/* strftime will do it */
	  *c++ = '%';
      }
      else if (!tc)				/* it's strftime syntax */
	tc = t;
      t++;					/* skip char next to '%' */
    }
    else if (!*t)				/* end of line */
    {
      if (tc)					/* finishing strftime part */
	c += __strftime (c, &templ[sizeof(templ)] - c, tc, tm);
    }
    else if (!tc)				/* no strftime syntax yet */
      *c++ = *t;				/* just copy next char */
  } while (*t++);
  *c = 0;
  t = (char *)expand_path (buf, templ, sb);
  if (t != buf)
    strfcpy (buf, templ, sb);
  return 0; /* all OK */
}

static inline int open_log_file(char *path, int do_dir)
{
  char *p, *p2;
  int rc;

  rc = open (path, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP);
  if (rc >= 0 || !do_dir || errno != ENOENT)
    return rc;
  p2 = NULL;
  while ((p = strrchr(path, '/'))) {
    if (p2)
      *p2 = '/';
    p2 = p;
    *p = '\0';
    rc = mkdir(path, S_IRWXU | S_IRGRP | S_IXGRP);
    if (rc == 0)
      break;
    if (errno != ENOENT) {
      *p = '/';
      return rc;
    }
  }
  if (p2)
    *p2 = '/';
  return open_log_file(path, do_dir); /* do it again */
}

/* accepts: S_TERMINATE, S_SHUTDOWN, S_FLUSH */
static iftype_t _autolog_name_signal (INTERFACE *iface, ifsig_t sig)
{
  autolog_t *log = (autolog_t *)iface->data;
  struct tm tm;
  struct timeval tv0, tv;

  if (!(iface->ift & I_DIED)) switch (sig)	/* is it terminated already? */
  {
    case S_TIMEOUT:
      log->d->timer = -1;
      if (Time - log->d->timestamp < autolog_autoclose)
	/* some spurious call? */
	return 0;
      /* timeout: close log if queue is empty */
      Mark_Iface (iface);
      break;
    case S_FLUSH:
      if (iface->qsize > 0) /* so there is queue... just reopen the log file */
      {
	close (log->d->fd);
	log->d->fd = open_log_file (log->d->path, 0);
	return 0;
      }			/* else terminate it on flush to get right timestamps */
    case S_TERMINATE:
      localtime_r (&log->d->timestamp, &tm);
      gettimeofday (&tv0, NULL);
      if (autolog_add (log, autolog_close, NULL, 0, &tm, 0) >= 0)
	while (__flush_autolog (log, 0) == 0)	/* retry if file is locked */
	  if (gettimeofday (&tv, NULL) ||
	      tv.tv_usec > tv0.tv_usec + 100000) /* wait up to 100 ms */
	  {
	    ERROR ("autolog: time out on closing %s.", log->d->path);
	    break;
	  }
      if (log->d->timer >= 0)
	KillTimer(log->d->timer);
      if (log->d->fd >= 0)
	close (log->d->fd);
      FREE (&log->d->path);
      FREE (&log->d->lname);
      log->iface = NULL;
      iface->data = NULL;
      iface->ift |= I_DIED;
      return I_DIED;
    case S_SHUTDOWN:
      localtime_r (&log->d->timestamp, &tm);
      autolog_add (log, autolog_close, NULL, 0, &tm, 1); /* ignore result */
      if (log->d->fd >= 0)
	close (log->d->fd);
      log->iface = NULL;
      iface->data = NULL;
      iface->ift |= I_DIED;
      return I_DIED;
    default: ;
  }
  return 0;
}

static int _autolog_name_request (INTERFACE *iface, REQUEST *req)
{
  ssize_t x;
  autolog_t *log;
  struct tm tm;
  struct timeval tv0, tv;

  if (req) DBG ("_autolog_name_request: message for %s", req->to);
  log = (autolog_t *)iface->data;
  if (!req || !(req->flag & (AUTOLOG_LEVELS | AUTOLOG_LEVELS2)))
  {
    if (Time - log->d->timestamp >= autolog_autoclose)	/* timeout: close log */
      _autolog_name_signal (iface, S_TERMINATE);
    return REQ_OK;
  }
  localtime_r (&Time, &tm);
  if (autolog_by_lname == TRUE)
  {
    unsigned char *n;
    const char *l;
#if PATH_MAX > IFNAMEMAX
    char path[PATH_MAX+1];
#else
    char path[IFNAMEMAX+1];
#endif

    /* make path and open file */
    n = strrchr (req->to, '@');
    if (n)
    {
      n++;
      strfcpy (path, req->to, (n - req->to));
      if (Inspect_Client (n, NULL, path, &l, NULL, NULL, NULL) &&
	  safe_strcmp (l, log->d->lname))	/* Lname was changed? */
      {
	gettimeofday (&tv0, NULL);
	if (autolog_add (log, autolog_close, NULL, 0, &tm, 0) >= 0)
	  while (__flush_autolog (log, 0) == 0)	/* retry if file is locked */
	    if (gettimeofday (&tv, NULL) ||
		tv.tv_usec > tv0.tv_usec + 100000) /* wait up to 100 ms */
	    {
	      ERROR ("autolog: time out on closing %s.", log->d->path);
	      break;
	    }
	if (log->d->fd >= 0)
	  close (log->d->fd);
	FREE (&log->d->path);
	FREE (&log->d->lname);
	if (_autolog_makepath (path, sizeof(path), n, l ? l : (char *)req->to,
			       l ? strlen(l) : (size_t)(n - req->to - 1),
			       l ? autolog_lname_prefix : NULL, &tm))
	{
	  ERROR ("autolog: could not make path for %s.", req->to);
	  log->d->fd = -1;
	}
	else if ((log->d->fd = open_log_file (path, 1)) < 0)
	  ERROR ("autolog: could not open log file %s: %s", path, strerror (errno));
	if (log->d->fd < 0)
	{
	  dprint (3, "autolog:_autolog_name_request: halted logger \"%s\"",
		  log->iface->name);
	  return REQ_OK;
	}
	log->d->path = safe_strdup (path);
	log->d->reccount = 0;
	log->d->inbuf = 0;
	log->d->day = tm.tm_mday;
	log->d->lname = safe_strdup (l);
	autolog_add (log, autolog_open, NULL, 0, &tm, 0); /* ignore result */
	dprint (3, "autolog:_autolog_name_request: changed path for %s.",
		iface->name);
      }
    }
  }
  if (log->d->day != tm.tm_mday)
  {
    if (*autolog_daychange &&
	autolog_add (log, autolog_daychange, NULL, 0, &tm, 0) <= 0)
    {
      _autolog_name_signal (iface, S_TERMINATE);
      WARNING ("autolog:_autolog_name_request: %s terminated.", iface->name);
      return REQ_REJECTED;				/* could not add it */
    }
    else
      log->d->day = tm.tm_mday;
    /* we would check for rotation (by strftime in _autolog_makepath)
       but we let conversation still go until timeout (autolog_autoclose)
       and it will be reopened next time with new rotated name */
  }
  x = autolog_add (log, autolog_timestamp, req->string,
		   (req->flag & F_PREFIXED) ? strlen (autolog_ctl_prefix) : 0,
		   &tm, 0);
  if (x <= 0)
  {
    if (x < 0)
      _autolog_name_signal (iface, S_TERMINATE);
    WARNING ("autolog:_autolog_name_request: %s terminated", iface->name);
    return REQ_REJECTED;
  }
  if (req->flag & F_END && iface->qsize == 0)		/* session ended */
  {
    _autolog_name_signal (iface, S_TERMINATE);
    dprint (3, "autolog:_autolog_name_request: %s terminated", iface->name);
    return REQ_OK;
  }
  if (log->d->reccount++ < 5)		/* add up to 5 sequent requests */
    Get_Request();
  else
    log->d->reccount = 0;
  if (log->d->timer >= 0)
    KillTimer(log->d->timer);
  log->d->timer = Add_Timer (iface, S_TIMEOUT, autolog_autoclose);
  return REQ_OK;
}


/* ----------------------------------------------------------------------------
 *	"@network" autolog interface - handles new logs
 */

static autolog_t *_get_autolog_t (autolognet_t *net)
{
  autolog_t *next;

  for (next = net->log; next; next = next->prev)
    if (next->iface == NULL)
      return next;
  next = safe_calloc (1, sizeof(autolog_t));
  next->prev = net->log;
  net->log = next;
  return next;
}

static autolog_t *_find_autolog_t (autolog_t *tail, char *name)
{
  for (; tail; tail = tail->prev)
    if (!strcmp (tail->iface->name, name))
      break;
  return tail;
}

static iftype_t _autolog_net_signal (INTERFACE *iface, ifsig_t sig)
{
  autolog_t *log;
  register iftype_t rc;

  switch (sig)
  {
    case S_TERMINATE:
    case S_SHUTDOWN:
      while ((log = ((autolognet_t *)iface->data)->log))
      {
	if (log->iface && (rc = log->iface->IFSignal (log->iface, sig)))
	  log->iface->ift |= rc;
	((autolognet_t *)iface->data)->log = log->prev;
	if (sig != S_SHUTDOWN)
	  FREE (&log);
      }
      ((autolognet_t *)iface->data)->net = NULL;
      iface->data = NULL;
      iface->ift |= I_DIED;
      return I_DIED;
    default: ;
  }
  return 0;
}

static int _autolog_net_request (INTERFACE *iface, REQUEST *req)
{ /* reacts only to I_LOG : F_PUBLIC F_PRIV F_JOIN F_MODES */
  if (req) DBG ("_autolog_net_request: message for %s", req->to);
  if (req && (req->flag & AUTOLOG_LEVELS) && Have_Wildcard (req->to) < 0)
  {
    autolog_t *log;
    int fd;
    struct tm tm;
    const char *tpath;
    char *p;
    size_t s;
#if PATH_MAX > NAMEMAX
    char path[PATH_MAX+1];
#else
    char path[NAMEMAX+1];
#endif

    if (Find_Iface (I_FILE | I_LOG, req->to))
    {
      dprint (4, "autolog: logger for %s found, not creating own.", req->to);
      Unset_Iface();
      return REQ_OK;
    }
    /* check if I_LOG for target already exists then bounce it */
    if ((log = _find_autolog_t (((autolognet_t *)iface->data)->log, req->to)))
    {
      WARNING ("autolog:_autolog_net_request: found strange logger \"%s\"",
	       log->iface->name);
      if (log->d)
	return _autolog_name_request (log->iface, req);
      return REQ_OK;
    }
    /* make path and open file */
    tpath = strrchr (req->to, '@');
    if (tpath)
      s = tpath - (char *)req->to;
    else				/* hmm, how it can be possible? */
      s = strlen (req->to);
    /* if enabled then get client Lname and do with it */
    if (autolog_by_lname == TRUE && strfcpy (path, req->to, s+1) &&
	Inspect_Client (&iface->name[1], NULL, path, &tpath, NULL, NULL, NULL) &&
	tpath)				/* it's client with Lname */
    {
      s = strlen (tpath);
      p = autolog_lname_prefix;
    }
    else
    {
      tpath = req->to;
      p = NULL;
    }
    localtime_r (&Time, &tm);
    if (_autolog_makepath (path, sizeof(path), &iface->name[1], tpath, s, p,
			   &tm))
    {
      ERROR ("autolog: could not make path for %s", tpath);
      fd = -1;
    }
    else if ((fd = open_log_file (path, 1)) < 0)
      ERROR ("autolog: could not open log file %s: %s", path, strerror (errno));
    if (fd < 0)
    {
      log = _get_autolog_t ((autolognet_t *)iface->data);
      FREE (&log->d);
      log->iface = Add_Iface (I_LOG, req->to, &_autolog_nolog_s, NULL, log);
      dprint (3, "autolog:_autolog_net_request: created new NOT logger \"%s\"",
	      log->iface->name);
      return REQ_OK;
    }
    log = _get_autolog_t ((autolognet_t *)iface->data);	/* make structure */
    if (!log->d)
      log->d = safe_malloc (sizeof(autologdata_t));
    log->d->path = safe_strdup (path);
    log->d->fd = fd;
    log->d->reccount = 0;
    log->d->inbuf = 0;
    log->d->day = tm.tm_mday;
    log->d->lname = NULL;
    if (tpath != (char *)req->to)
      log->d->lname = safe_strdup (tpath);
    log->iface = Add_Iface (I_LOG | I_FILE, req->to, &_autolog_name_signal,
			    &_autolog_name_request, log);
    autolog_add (log, autolog_open, NULL, 0, &tm, 0);	/* ignore result */
    dprint (3, "autolog:_autolog_net_request: created new log \"%s\"",
	    log->iface->name);
    _autolog_name_request (log->iface, req);	/* anyway nothing more to do */
  }
  return REQ_OK;
}


/* ----------------------------------------------------------------------------
 *	"*" autolog interface - handles new networks
 */

static autolognet_t *_get_autolognet_t (autolognet_t **tail)
{
  autolognet_t *next;

  for (next = *tail; next; next = next->prev)
    if (next->net == NULL)
      return next;
  next = safe_calloc (1, sizeof(autolognet_t));
  next->prev = *tail;
  *tail = next;
  return next;
}

static autolognet_t *_find_autolognet_t (autolognet_t *tail, char *name)
{
  for (; tail; tail = tail->prev)
    if (!strcmp (tail->net->name, name))
      break;
  return tail;
}

static INTERFACE *_autolog_mass = NULL;

static iftype_t _autolog_mass_signal (INTERFACE *iface, ifsig_t sig)
{
  autolognet_t *net;
  register iftype_t rc;

  switch (sig)
  {
    case S_TERMINATE:
    case S_SHUTDOWN:
      while ((net = (autolognet_t *)iface->data))
      {
	if (net->net && (rc = net->net->IFSignal (net->net, sig)))
	  net->net->ift |= rc;
	iface->data = net->prev;
	if (sig != S_SHUTDOWN)
	  FREE (&net);
      }
      _autolog_mass = NULL;
      iface->ift |= I_DIED;
      return I_DIED;
    default: ;
  }
  return 0;
}

static int _autolog_mass_request (INTERFACE *iface, REQUEST *req)
{ /* reacts only to I_LOG : F_PUBLIC F_PRIV F_JOIN F_MODES */
  char *c;
  autolognet_t *net;

  /* check for (and create if need) interface I_LOG "@network" */
  if (req && (req->flag & AUTOLOG_LEVELS) && Have_Wildcard (req->to) < 0 &&
      (c = strrchr (req->to, '@')) &&
      !(net = _find_autolognet_t (iface->data, c)))
  {
      net = _get_autolognet_t ((autolognet_t **)&iface->data);
      net->net = Add_Iface (I_LOG, c, &_autolog_net_signal,
			    &_autolog_net_request, net);
      dprint (3, "autolog:_autolog_mass_request: created new network \"%s\"",
	      net->net->name);
      return _autolog_net_request (net->net, req);
  }
  return REQ_OK;
}


/* ----------------------------------------------------------------------------
 *	common module interface
 */

static void autolog_register (void)
{
  /* register module itself */
  Add_Request (I_INIT, "*", F_REPORT, "module autolog");
  /* register all variables */
  RegisterString ("autolog-ctl-prefix", autolog_ctl_prefix,
		  sizeof(autolog_ctl_prefix), 0);
  RegisterString ("autolog-path", autolog_path, sizeof(autolog_path), 0);
  RegisterString ("autolog-serv-path", autolog_serv_path,
		  sizeof(autolog_serv_path), 0);
  RegisterString ("autolog-open", autolog_open, sizeof(autolog_open), 0);
  RegisterString ("autolog-close", autolog_close, sizeof(autolog_close), 0);
  RegisterString ("autolog-daychange", autolog_daychange,
		  sizeof(autolog_daychange), 0);
  RegisterString ("autolog-timestamp", autolog_timestamp,
		  sizeof(autolog_timestamp), 0);
  RegisterString ("autolog-lname-prefix", autolog_lname_prefix,
		  sizeof(autolog_lname_prefix), 0);
  RegisterBoolean ("autolog-by-lname", &autolog_by_lname);
  RegisterInteger ("autolog-autoclose", &autolog_autoclose);
}

/*
 * this function must receive signals:
 *  S_TERMINATE - unload module,
 *  S_REG - (re)register variables,
 *  S_REPORT - out state info to log.
 */
static iftype_t module_autolog_signal (INTERFACE *iface, ifsig_t sig)
{
  switch (sig)
  {
    case S_TERMINATE:
      Delete_Help ("autolog");
      if (_autolog_mass)
	_autolog_mass_signal (_autolog_mass, sig);
      UnregisterVariable ("autolog-ctl-prefix");
      UnregisterVariable ("autolog-path");
      UnregisterVariable ("autolog-serv-path");
      UnregisterVariable ("autolog-open");
      UnregisterVariable ("autolog-close");
      UnregisterVariable ("autolog-daychange");
      UnregisterVariable ("autolog-timestamp");
      UnregisterVariable ("autolog-lname-prefix");
      UnregisterVariable ("autolog-by-lname");
      UnregisterVariable ("autolog-autoclose");
      return I_DIED;
    case S_REG:
      /* reregister all */
      autolog_register();
      break;
    case S_REPORT:
      if (_autolog_mass)
      {
	int i = 0;
	autolog_t *log;
	autolognet_t *net;
	INTERFACE *tmp = Set_Iface (iface);

	for (net = (autolognet_t *)_autolog_mass->data; net; net = net->prev)
	  for (log = net->log; log; log = log->prev)
	    if (log->iface && log->d && log->d->fd >= 0)
	      New_Request (tmp, F_REPORT,
			   _("Auto log #%d: file \"%s\" for client %s."),
			   ++i, log->d->path, log->iface->name);
	if (i == 0)
	  New_Request (tmp, F_REPORT, _("Module autolog: no opened logs."));
	Unset_Iface();
      }
      break;
    default: ;
  }
  return 0;
}

/*
 * this function called when you load a module.
 * Input: parameters string args.
 * Returns: address of signals receiver function, NULL if not loaded.
 */
SigFunction ModuleInit (char *args)
{
  CheckVersion;
  strfcpy (autolog_open, _("IRC log started %c"), sizeof(autolog_open));
  strfcpy (autolog_close, _("IRC log ended %c"), sizeof(autolog_close));
  strfcpy (autolog_daychange, _("Day changed: %a %x"), sizeof(autolog_daychange));
  Add_Help ("autolog");
  autolog_register();
  _autolog_mass = Add_Iface (I_LOG, "*", &_autolog_mass_signal,
			     &_autolog_mass_request, NULL);
  //TODO: "time-shift" binding
  return &module_autolog_signal;
}
