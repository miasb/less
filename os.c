/*
 * Copyright (C) 1984-2022  Mark Nudelman
 *
 * You may distribute under the terms of either the GNU General Public
 * License or the Less License, as specified in the README file.
 *
 * For more information, see the README file.
 */


/*
 * Operating system dependent routines.
 *
 * Most of the stuff in here is based on Unix, but an attempt
 * has been made to make things work on other operating systems.
 * This will sometimes result in a loss of functionality, unless
 * someone rewrites code specifically for the new operating system.
 *
 * The makefile provides defines to decide whether various
 * Unix features are present.
 */

#include "less.h"
#include <signal.h>
#include <setjmp.h>
#if MSDOS_COMPILER==WIN32C
#include <windows.h>
#endif
#if HAVE_TIME_H
#include <time.h>
#endif
#if HAVE_ERRNO_H
#include <errno.h>
#endif
#if HAVE_VALUES_H
#include <values.h>
#endif

#if HAVE_POLL && !MSDOS_COMPILER && !defined(__APPLE__)
#define USE_POLL 1
#else
#define USE_POLL 0
#endif
#if USE_POLL
#include <poll.h>
#endif

/*
 * BSD setjmp() saves (and longjmp() restores) the signal mask.
 * This costs a system call or two per setjmp(), so if possible we clear the
 * signal mask with sigsetmask(), and use _setjmp()/_longjmp() instead.
 * On other systems, setjmp() doesn't affect the signal mask and so
 * _setjmp() does not exist; we just use setjmp().
 */
#if HAVE__SETJMP && HAVE_SIGSETMASK
#define SET_JUMP        _setjmp
#define LONG_JUMP       _longjmp
#else
#define SET_JUMP        setjmp
#define LONG_JUMP       longjmp
#endif

public int reading;
public int consecutive_nulls = 0;

static jmp_buf read_label;

extern int sigs;
extern int ignore_eoi;
extern int exit_F_on_close;
#if !MSDOS_COMPILER
extern int tty;
#endif
#if LESSTEST
extern char *ttyin_name;
#endif /*LESSTEST*/

#if USE_POLL
/*
 * Return true if one of the events has occurred on the specified file.
 */
	static int
poll_events(fd, events)
	int fd;
	int events;
{
	struct pollfd poller = { fd, events, 0 };
	int n = poll(&poller, 1, 0);
	if (n <= 0)
		return 0;
	return (poller.revents & events);
}
#endif

/*
 * Like read() system call, but is deliberately interruptible.
 * A call to intread() from a signal handler will interrupt
 * any pending iread().
 */
	public int
iread(fd, buf, len)
	int fd;
	unsigned char *buf;
	unsigned int len;
{
	int n;

start:
#if MSDOS_COMPILER==WIN32C
	if (ABORT_SIGS())
		return (READ_INTR);
#else
#if MSDOS_COMPILER && MSDOS_COMPILER != DJGPPC
	if (kbhit())
	{
		int c;
		
		c = getch();
		if (c == '\003')
			return (READ_INTR);
		ungetch(c);
	}
#endif
#endif
	if (!reading && SET_JUMP(read_label))
	{
		/*
		 * We jumped here from intread.
		 */
		reading = 0;
#if HAVE_SIGPROCMASK
		{
		  sigset_t mask;
		  sigemptyset(&mask);
		  sigprocmask(SIG_SETMASK, &mask, NULL);
		}
#else
#if HAVE_SIGSETMASK
		sigsetmask(0);
#else
#ifdef _OSK
		sigmask(~0);
#endif
#endif
#endif
		return (READ_INTR);
	}

	flush();
	reading = 1;
#if MSDOS_COMPILER==DJGPPC
	if (isatty(fd))
	{
		/*
		 * Don't try reading from a TTY until a character is
		 * available, because that makes some background programs
		 * believe DOS is busy in a way that prevents those
		 * programs from working while "less" waits.
		 */
		fd_set readfds;

		FD_ZERO(&readfds);
		FD_SET(fd, &readfds);
		if (select(fd+1, &readfds, 0, 0, 0) == -1)
		{
			reading = 0;
			return (-1);
		}
	}
#endif
#if USE_POLL
	if (fd != tty)
	{
		int close_events = (ignore_eoi && !exit_F_on_close) ? POLLERR : POLLERR|POLLHUP;
#if LESSTEST
		if (ttyin_name == NULL) /* only do this for a real tty */
#endif /*LESSTEST*/
		{
			if (poll_events(tty, POLLIN) && getchr() == CONTROL('X'))
			{
				sigs |= S_INTERRUPT;
				reading = 0;
				return (READ_INTR);
			}
		}
		int fd_events = poll_events(fd, POLLIN|POLLHUP|POLLERR);
		if ((fd_events & close_events) && !(fd_events & POLLIN))
		{
			sigs |= S_INTERRUPT;
			reading = 0;
			return (READ_INTR);
		}
		if ((fd_events & POLLIN) == 0)
		{
			/* 
			 * No input data: return here rather than
			 * possibly getting stuck in a blocking read() below. 
			 * This allows ^X to abort reading an empty pipe.
			 */
			return ((fd_events & POLLHUP) ? 0 : READ_AGAIN);
		}
	}
#else
#if MSDOS_COMPILER==WIN32C
	if (win32_kbhit() && WIN32getch() == CONTROL('X'))
	{
		sigs |= S_INTERRUPT;
		reading = 0;
		return (READ_INTR);
	}
#endif
#endif
	n = read(fd, buf, len);
	reading = 0;
#if 1
	/*
	 * This is a kludge to workaround a problem on some systems
	 * where terminating a remote tty connection causes read() to
	 * start returning 0 forever, instead of -1.
	 */
	{
		if (!ignore_eoi)
		{
			if (n == 0)
				consecutive_nulls++;
			else
				consecutive_nulls = 0;
			if (consecutive_nulls > 20)
				quit(QUIT_ERROR);
		}
	}
#endif
	if (n < 0)
	{
#if HAVE_ERRNO
		/*
		 * Certain values of errno indicate we should just retry the read.
		 */
#if MUST_DEFINE_ERRNO
		extern int errno;
#endif
#ifdef EINTR
		if (errno == EINTR)
			goto start;
#endif
#ifdef EAGAIN
		if (errno == EAGAIN)
			goto start;
#endif
#endif
		return (-1);
	}
	return (n);
}

/*
 * Interrupt a pending iread().
 */
	public void
intread(VOID_PARAM)
{
	LONG_JUMP(read_label, 1);
}

/*
 * Return the current time.
 */
#if HAVE_TIME
	public time_type
get_time(VOID_PARAM)
{
	time_type t;

	time(&t);
	return (t);
}
#endif


#if !HAVE_STRERROR
/*
 * Local version of strerror, if not available from the system.
 */
	static char *
strerror(err)
	int err;
{
	static char buf[INT_STRLEN_BOUND(int)+12];
#if HAVE_SYS_ERRLIST
	extern char *sys_errlist[];
	extern int sys_nerr;
  
	if (err < sys_nerr)
		return sys_errlist[err];
#endif
	sprintf(buf, "Error %d", err);
	return buf;
}
#endif

/*
 * errno_message: Return an error message based on the value of "errno".
 */
	public char *
errno_message(filename)
	char *filename;
{
	char *p;
	char *m;
	int len;
#if HAVE_ERRNO
#if MUST_DEFINE_ERRNO
	extern int errno;
#endif
	p = strerror(errno);
#else
	p = "cannot open";
#endif
	len = (int) (strlen(filename) + strlen(p) + 3);
	m = (char *) ecalloc(len, sizeof(char));
	SNPRINTF2(m, len, "%s: %s", filename, p);
	return (m);
}

/* #define HAVE_FLOAT 0 */

	static POSITION
muldiv(val, num, den)
	POSITION val, num, den;
{
#if HAVE_FLOAT
	double v = (((double) val) * num) / den;
	return ((POSITION) (v + 0.5));
#else
	POSITION v = ((POSITION) val) * num;

	if (v / num == val)
		/* No overflow */
		return (POSITION) (v / den);
	else
		/* Above calculation overflows; 
		 * use a method that is less precise but won't overflow. */
		return (POSITION) (val / (den / num));
#endif
}

/*
 * Return the ratio of two POSITIONS, as a percentage.
 * {{ Assumes a POSITION is a long int. }}
 */
	public int
percentage(num, den)
	POSITION num;
	POSITION den;
{
	return (int) muldiv(num,  (POSITION) 100, den);
}

/*
 * Return the specified percentage of a POSITION.
 */
	public POSITION
percent_pos(pos, percent, fraction)
	POSITION pos;
	int percent;
	long fraction;
{
	/* Change percent (parts per 100) to perden (parts per NUM_FRAC_DENOM). */
	POSITION perden = (percent * (NUM_FRAC_DENOM / 100)) + (fraction / 100);

	if (perden == 0)
		return (0);
	return (POSITION) muldiv(pos, perden, (POSITION) NUM_FRAC_DENOM);
}

#if !HAVE_STRCHR
/*
 * strchr is used by regexp.c.
 */
	char *
strchr(s, c)
	char *s;
	int c;
{
	for ( ;  *s != '\0';  s++)
		if (*s == c)
			return (s);
	if (c == '\0')
		return (s);
	return (NULL);
}
#endif

#if !HAVE_MEMCPY
	VOID_POINTER
memcpy(dst, src, len)
	VOID_POINTER dst;
	VOID_POINTER src;
	int len;
{
	char *dstp = (char *) dst;
	char *srcp = (char *) src;
	int i;

	for (i = 0;  i < len;  i++)
		dstp[i] = srcp[i];
	return (dst);
}
#endif

#ifdef _OSK_MWC32

/*
 * This implements an ANSI-style intercept setup for Microware C 3.2
 */
	public int 
os9_signal(type, handler)
	int type;
	RETSIGTYPE (*handler)();
{
	intercept(handler);
}

#include <sgstat.h>

	int 
isatty(f)
	int f;
{
	struct sgbuf sgbuf;

	if (_gs_opt(f, &sgbuf) < 0)
		return -1;
	return (sgbuf.sg_class == 0);
}
	
#endif

	public void
sleep_ms(ms)
	int ms;
{
#if MSDOS_COMPILER==WIN32C
	Sleep(ms);
#else
#if HAVE_NANOSLEEP
	int sec = ms / 1000;
	struct timespec t = { sec, (ms - sec*1000) * 1000000 };
	nanosleep(&t, NULL);
#else
#if HAVE_USLEEP
	usleep(ms);
#else
	sleep((ms+999) / 1000);
#endif
#endif
#endif
}
