/* Initialize and serve a login terminal
 *
 * Copyright (c) 2016-2021  Joachim Wiberg <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"

#include <err.h>
#include <errno.h>
#include <limits.h>		/* LOGIN_NAME_MAX */
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/ttydefaults.h>	/* Not included by default in musl libc */
#include <termios.h>
#include <unistd.h>		/* sysconf() */

#include "finit.h"
#include "helpers.h"
#include "utmp-api.h"

#ifndef _PATH_LOGIN
#define _PATH_LOGIN  "/bin/login"
#endif

#ifndef LOGIN_NAME_MIN
#define LOGIN_NAME_MIN 64
#endif

static long logname_len = 32;	/* useradd(1) limit at 32 chars */

/*
 * Read one character from stdin.
 */
static int readch(void)
{
	char ch1;
	int st;

	st = read(STDIN_FILENO, &ch1, 1);
	if (st <= 0)
		return -1;

	return ch1 & 0xFF;
}

/*
 * Parse and display a line from /etc/issue
 */
static void do_parse(char *line, struct utsname *uts, char *tty)
{
	char *s, *s0;

	s0 = line;
	for (s = line; *s != 0; s++) {
		if (*s == '\\') {
			if ((s - s0) > 0)
				dprint(STDOUT_FILENO, s0, s - s0);
			s0 = s + 2;
			switch (*++s) {
			case 'l':
				dprint(STDOUT_FILENO, tty, 0);
				break;
			case 'm':
				dprint(STDOUT_FILENO, uts->machine, 0);
				break;
			case 'n':
				dprint(STDOUT_FILENO, uts->nodename, 0);
				break;
#ifdef _GNU_SOURCE
			case 'o':
				dprint(STDOUT_FILENO, uts->domainname, 0);
				break;
#endif
			case 'r':
				dprint(STDOUT_FILENO, uts->release, 0);
				break;
			case 's':
				dprint(STDOUT_FILENO, uts->sysname, 0);
				break;
			case 'v':
				dprint(STDOUT_FILENO, uts->version, 0);
				break;
			case 0:
				goto leave;
			default:
				s0 = s - 1;
			}
		}
	}

leave:
	if ((s - s0) > 0)
		dprint(STDOUT_FILENO, s0, s - s0);
}

/*
 * Parse and display /etc/issue
 */
static void do_issue(char *tty)
{
	FILE *fp;
	char buf[BUFSIZ] = "Welcome to \\s \\v \\n \\l\n\n";
	struct utsname uts;

	/*
	 * Get data about this machine.
	 */
	uname(&uts);

	fp = fopen("/etc/issue", "r");
	if (fp) {
		while (fgets(buf, sizeof(buf), fp))
			do_parse(buf, &uts, tty);

		fclose(fp);
	} else {
		do_parse(buf, &uts, tty);
	}

	do_parse("\\n login: ", &uts, tty);
}

/*
 * Handle the process of a GETTY.
 */
static int get_logname(char *tty, char *name, size_t len)
{
	char *np;
	int ch;

	/*
	 * Display prompt.
	 */
	ch = ' ';
	*name = '\0';
	while (ch != '\n') {
		do_issue(tty);

		np = name;
		while ((ch = readch()) != '\n') {
			if (ch < 0)
				return 1;

			if (np < name + len)
				*np++ = ch;
		}

		*np = '\0';
		if (*name == '\0')
			ch = ' ';	/* blank line typed! */
	}

	name[len - 1] = 0;
	return 0;
}

/*
 * Execute the login(1) command with the current
 * username as its argument. It will reply to the
 * calling user by typing "Password: "...
 */
static int do_login(char *name)
{
	struct stat st;

	execl(_PATH_LOGIN, _PATH_LOGIN, name, NULL);

	/*
	 * Failed to exec login, should not happen on normal systems.
	 * Try a starting a rescue shell instead.
	 */
	if (fstat(0, &st) == 0 && S_ISCHR(st.st_mode)) {
		warnx("Failed exec %s, attempting fallback to %s ...", _PATH_LOGIN, _PATH_SULOGIN);
		execl(_PATH_SULOGIN, _PATH_SULOGIN, NULL);

		warnx("Failed exec %s, attempting fallback to %s ...", _PATH_SULOGIN, _PATH_BSHELL);
		execl(_PATH_BSHELL, _PATH_BSHELL, NULL);
	}

	return 1;	/* We shouldn't get here ... */
}

static int getty(char *tty, speed_t speed, char *term, char *user)
{
	const char cln[] = "\r\e[K\n";
	char name[logname_len + 1]; /* +1 for NUL termination */
	pid_t sid;

	/*
	 * Clean up tty name.
	 */
	if (!strncmp(tty, _PATH_DEV, strlen(_PATH_DEV)))
		tty += 5;

	/* The getty process is responsible for the UTMP login record */
	utmp_set_login(tty, NULL);

	/* Replace "Please press enter ..." with login: */
	dprint(STDERR_FILENO, cln, strlen(cln));

restart:
	stty(STDIN_FILENO, speed);
	if (!user) {
		if (get_logname(tty, name, sizeof(name)))
			goto restart;
	} else
		strlcpy(name, user, sizeof(name));

	/* check current session associated with our tty */
	sid = tcgetsid(0);
	if (sid < 0 || getpid() != sid) {
		if (ioctl(STDIN_FILENO, TIOCSCTTY, 1) == -1)
			err(1, "failed stealing controlling TTY");
	}

	if (term && term[0])
		setenv("TERM", term, 1);

	return do_login(name);
}

static int usage(int rc)
{
	warnx("usage: getty [-h?] tty [speed [term]]");
	return rc;
}

int main(int argc, char *argv[])
{
	char *tty, *speed = "0", *term = NULL;
	int c;

	while ((c = getopt(argc, argv, "h?")) != EOF) {
		switch(c) {
		case 'h':
		case '?':
			return usage(0);
		default:
			return usage(1);
		}
	}

	if (optind >= argc)
		return usage(1);

	tty = argv[optind++];
	if (optind < argc)
		speed = argv[optind++];
	if (optind < argc)
		term = argv[optind++];

	logname_len = sysconf(_SC_LOGIN_NAME_MAX);
	if (logname_len == -1i)
	       logname_len = LOGIN_NAME_MAX;
	if (logname_len < LOGIN_NAME_MIN)
		logname_len = LOGIN_NAME_MIN;

	return getty(tty, atoi(speed), term, NULL);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
