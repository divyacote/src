/*	$OpenBSD: chpass.c,v 1.17 2001/08/15 20:47:53 millert Exp $	*/
/*	$NetBSD: chpass.c,v 1.8 1996/05/15 21:50:43 jtc Exp $	*/

/*-
 * Copyright (c) 1988, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef lint
static char copyright[] =
"@(#) Copyright (c) 1988, 1993, 1994\n\
	The Regents of the University of California.  All rights reserved.\n";
#endif /* not lint */

#ifndef lint
#if 0
static char sccsid[] = "@(#)chpass.c	8.4 (Berkeley) 4/2/94";
#else 
static char rcsid[] = "$OpenBSD: chpass.c,v 1.17 2001/08/15 20:47:53 millert Exp $";
#endif
#endif /* not lint */

#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util.h>

#include "chpass.h"
#include "pathnames.h"

char tempname[] = __CONCAT(_PATH_VARTMP,"pw.XXXXXXXX");
enum { NEWSH, LOADENTRY, EDITENTRY } op;
uid_t uid;

extern char *__progname;

#ifdef	YP
int use_yp;
int force_yp = 0;
extern struct passwd *ypgetpwnam(), *ypgetpwuid();
int _yp_check __P((char **));
int pw_yp __P((struct passwd *, uid_t));
#endif

void	baduser __P((void));
void	tempcleanup __P((void));
void	kbintr __P((int));
void	usage __P((void));

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct passwd *pw, lpw;
	int ch, pfd, tfd, dfd;
	char *arg;
	char *s = NULL;
	sigset_t fullset;

#ifdef	YP
	use_yp = _yp_check(NULL);
#endif

	op = EDITENTRY;
	while ((ch = getopt(argc, argv, "a:s:ly")) != -1)
		switch(ch) {
		case 'a':
			op = LOADENTRY;
			arg = optarg;
			break;
		case 's':
			op = NEWSH;
			arg = optarg;
			break;
#ifdef	YP
		case 'l':
			use_yp = 0;
			break;
		case 'y':
			if (!use_yp) {
				warnx("YP not in use.");
				usage();
			}
			force_yp = 1;
			break;
#endif
		case '?':
		default:
			usage();
		}
	argc -= optind;
	argv += optind;

#ifdef	YP
	if (op == LOADENTRY && use_yp)
		errx(1, "cannot load entry using NIS.\n\tUse the -l flag to load local.");
#endif
	uid = getuid();

	if (op == EDITENTRY || op == NEWSH)
		switch(argc) {
		case 0:
			pw = getpwuid(uid);
#ifdef	YP
			if (pw && !force_yp)
				use_yp = 0;
			else if (use_yp)
				pw = ypgetpwuid(uid);
#endif	/* YP */
			if (!pw)
				errx(1, "unknown user: uid %u\n", uid);
			break;
		case 1:
			pw = getpwnam(*argv);
#ifdef	YP
			if (pw && !force_yp)
				use_yp = 0;
			else if (use_yp)
				pw = ypgetpwnam(*argv);
#endif	/* YP */
			if (!pw)
				errx(1, "unknown user: %s", *argv);
			if (uid && uid != pw->pw_uid)
				baduser();
			break;
		default:
			usage();
		}

	if (op == NEWSH) {
		/* protect p_shell -- it thinks NULL is /bin/sh */
		if (!arg[0])
			usage();
		if (p_shell(arg, pw, NULL))
			pw_error(NULL, 0, 1);
	}

	if (op == LOADENTRY) {
		if (uid)
			baduser();
		pw = &lpw;
		if (!pw_scan(arg, pw, NULL))
			exit(1);
	}

	/* Edit the user passwd information if requested. */
	if (op == EDITENTRY) {
		dfd = mkstemp(tempname);
		if (dfd == -1 || fcntl(dfd, F_SETFD, 1) == -1)
			pw_error(tempname, 1, 1);
		atexit(tempcleanup);
		display(tempname, dfd, pw);
		edit(tempname, pw);
	}

	/* Drop user's real uid and block all signals to avoid a DoS. */
	setuid(0);
	sigfillset(&fullset);
	sigdelset(&fullset, SIGINT);
	sigprocmask(SIG_BLOCK, &fullset, NULL);

	/* Get the passwd lock file and open the passwd file for reading. */
	pw_init();
	for (;;) {
		int i, c, d;

		(void)fputs("Please wait", stderr);
		for (i = 0; i < (s ? 64 : 8) && (tfd = pw_lock(0)) == -1; i++) {
			(void)signal(SIGINT, kbintr);
			fputc('.', stderr);
			usleep(250000);
			(void)signal(SIGINT, SIG_IGN);
		}
		fputc('\n', stderr);
		if (tfd != -1)
			break;

		/* Unable to lock passwd file, let the user decide. */
		if (errno == EEXIST) {
			if (s == NULL)
				s = "The passwd file is busy,";
			else
				s = "The passwd file is still busy,";
		} else
			s = "Unable to open passwd temp file,";
		(void)fprintf(stderr,
		    "%s do you want to wait until it is available? [y/n] ", s);
		(void)signal(SIGINT, kbintr);
		c = getchar();
		(void)signal(SIGINT, SIG_IGN);
		if (c != '\n')
			while ((d = getchar()) != '\n' && d != EOF)
				;
		if (tolower(c) != 'y')
			pw_error(NULL, 0, 1);
	}
	pfd = open(_PATH_MASTERPASSWD, O_RDONLY, 0);
	if (pfd == -1 || fcntl(pfd, F_SETFD, 1) == -1)
		pw_error(_PATH_MASTERPASSWD, 1, 1);

#ifdef	YP
	if (use_yp) {
		if (pw_yp(pw, uid))
			pw_error(NULL, 0, 1);
		else {
			pw_abort();
			exit(0);
		}
	} else
#endif	/* YP */
	{
		/* Copy the passwd file to the lock file, updating pw. */
		pw_copy(pfd, tfd, pw);

		/* Now finish the passwd file update. */
		if (pw_mkdb(pw->pw_name) == -1)
			pw_error(NULL, 0, 1);
	}

	exit(0);
}

void
baduser()
{

	errx(1, "%s", strerror(EACCES));
}

void
tempcleanup()
{

	unlink(tempname);
}

void
kbintr(signo)
	int signo;
{
	struct iovec iv[5];

	iv[0].iov_base = "\n";
	iv[0].iov_len = 1;
	iv[1].iov_base = __progname;
	iv[1].iov_len = strlen(__progname);
	iv[2].iov_base = ": ";
	iv[2].iov_len = 2;
	iv[3].iov_base = _PATH_MASTERPASSWD;
	iv[3].iov_len = sizeof(_PATH_MASTERPASSWD) - 1;
	iv[4].iov_base = " unchanged\n";
	iv[4].iov_len = 11;
	writev(STDERR_FILENO, iv, 5);

	if (op == EDITENTRY)
		unlink(tempname);

	_exit(1);
}

void
usage()
{

#ifdef	YP
	(void)fprintf(stderr,
	    "usage: %s [-l%s] [-a list] [-s newshell] [user]\n",
	    __progname, use_yp ? "y" : "");
#else
	(void)fprintf(stderr, "usage: %s [-a list] [-s newshell] [user]\n",
	    __progname);
#endif
	exit(1);
}
