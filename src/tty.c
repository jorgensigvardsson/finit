/* Finit TTY handling
 *
 * Copyright (c) 2013       Mattias Walström <lazzer@gmail.com>
 * Copyright (c) 2013-2017  Joachim Nilsson <troglobit@gmail.com>
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

#include <ctype.h>		/* isdigit() */
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <lite/lite.h>

#include "config.h"		/* Generated by configure script */
#include "finit.h"
#include "conf.h"
#include "helpers.h"
#include "tty.h"
#include "util.h"
#include "utmp-api.h"

#ifdef FALLBACK_SHELL
static pid_t fallback = 0;
#endif
static LIST_HEAD(, tty_node) tty_list = LIST_HEAD_INITIALIZER();

static char *canonicalize(char *tty)
{
	char buf[42];
	struct stat st;
	static char path[80];

	if (!tty)
		return NULL;

	/* Auto-detect serial console, for embedded devices mostly */
	if (!strcmp(tty, "@console")) {
		FILE *fp;

		fp = fopen("/sys/class/tty/console/active", "r");
		if (!fp) {
			_e("Cannot find system console, is sysfs not mounted?");
			return NULL;
		}

		if (fgets(buf, sizeof(buf), fp))
			tty = chomp(buf);
		fclose(fp);
	}

	strlcpy(path, tty, sizeof(path));
	if (stat(path, &st)) {
		if (!strncmp(path, _PATH_DEV, strlen(_PATH_DEV))) {
		unavailable:
			_d("TTY %s not available at the moment, registering anyway.", path);
			return path;
		}

		snprintf(path, sizeof(path), "%s%s", _PATH_DEV, tty);
		if (stat(path, &st))
			goto unavailable;
	}

	if (!S_ISCHR(st.st_mode))
		return NULL;

	return path;
}

void tty_mark(void)
{
	tty_node_t *tty;

	LIST_FOREACH(tty, &tty_list, link)
		tty->dirty = -1;
}

void tty_sweep(void)
{
	tty_node_t *tty, *tmp;

	LIST_FOREACH_SAFE(tty, &tty_list, link, tmp) {
		if (!tty->dirty)
			continue;

		_d("TTY %s dirty, stopping ...", tty->data.name);
		tty_stop(&tty->data);

		if (tty->dirty == -1) {
			_d("TTY %s removed, cleaning up.", tty->data.name);
			tty_unregister(tty);
		}
	}
}

/**
 * tty_register - Register a getty on a device
 * @line:   Configuration, text after initial "tty"
 * @rlimit: Limits for this service/task/run/inetd, may be global limits
 * @file:   The file name TTY was loaded from
 *
 * A Finit tty line can use the internal getty implementation or an
 * external one, like the BusyBox getty for instance.  This function
 * determines which one to use based on a leading '/dev' prefix.  If
 * a leading '/dev' is encountered the remaining options must be in
 * the following sequence:
 *
 *     tty [!1-9,S] <DEV> [BAUD[,BAUD,...]] [noclear] [nowait] [TERM]
 *
 * Otherwise the leading prefix must be the full path to an existing
 * getty implementation, with it's arguments following:
 *
 *     tty [!1-9,S] </path/to/getty> [ARGS] [noclear] [nowait]
 *
 * Different getty implementations prefer the TTY device argument in
 * different order, so take care to investigate this first.
 */
int tty_register(char *line, struct rlimit rlimit[], char *file)
{
	tty_node_t *entry;
	int         insert = 0, noclear = 0, nowait = 0;
	size_t      i, num = 0;
	char       *tok, *cmd = NULL, *args[TTY_MAX_ARGS];
	char             *dev = NULL, *baud = NULL;
	char       *runlevels = NULL, *term = NULL;

	if (!line) {
		_e("Missing argument");
		return errno = EINVAL;
	}

	/*
	 * Split line in separate arguments.  For an external getty
	 * this is used with execv(), for the built-in it simplifies
	 * further translation.
	 */
	tok = strtok(line, " \t");
	while (tok && num < NELEMS(args)) {
		if (!strcmp(tok, "noclear"))
			noclear = 1;
		else if (!strcmp(tok, "nowait"))
			nowait = 1;
		else
			args[num++] = tok;

		tok = strtok(NULL, " \t");
	}

	/* Iterate over all args */
	for (i = 0; i < num; i++) {
		/* First, figure out if built-in or external */
		if (!dev && !cmd) {
			if (args[i][0] == '[')
				runlevels = line;
			if (!strcmp(args[i], "@console"))
				dev = args[i];
			if (!strncmp(args[i], "/dev", 4))
				dev = args[i];
			if (!strncmp(args[i], "tty", 3))
				dev = args[i];
			if (!access(args[i], X_OK))
				cmd = strdup(args[i]);

			/* The first arg must be one of the above */
			continue;
		}

		/* Built-in getty args */
		if (dev) {
			if (isdigit(args[i][0])) {
				baud = args[i];
				continue;
			}

			/*
			 * Last arg, if not anything else, is the value
			 * to be used for the TERM environment variable.
			 */
			if (i + 1 == num)
				term = args[i];
		}

		/* External getty, figure out the device */
		if (cmd) {
			if (!strncmp(args[i], "/dev", 4))
				dev = args[i];

			if (!strncmp(args[i], "tty", 3))
				dev = args[i];
		}
	}

	if (!dev) {
	error:
		_e("Incomplete or non-existing TTY device given, cannot register.");
		if (cmd)
			free(cmd);

		return errno = EINVAL;
	}

	/* Ensure all getty (built-in + external) are registered with absolute path */
	dev = canonicalize(dev);
	if (!dev)
		goto error;
	dev = strdup(dev);

	entry = tty_find(dev);
	if (!entry) {
		insert = 1;
		entry = calloc(1, sizeof(*entry));
		if (!entry) {
			free(dev);
			if (cmd)
				free(cmd);
			return errno = ENOMEM;
		}
	}

	entry->data.name = dev;
	entry->data.baud = baud ? strdup(baud) : NULL;
	entry->data.term = term ? strdup(term) : NULL;
	entry->data.noclear = noclear;
	entry->data.nowait  = nowait;
	entry->data.runlevels = conf_parse_runlevels(runlevels);

	/* External getty */
	if (cmd) {
		int j = 0;

		tok = strrchr(cmd, '/');
		if (!tok)
			tok = cmd;
		else
			tok++;
		entry->data.cmd = cmd;
		args[1] = strdup(tok);

		for (i = 1; i < num; i++)
			entry->data.args[j++] = strdup(args[i]);
		entry->data.args[++j] = NULL;
	}

	_d("Registering %s getty on TTY %s at %s baud with term %s on runlevels %s",
	   cmd ? "external" : "built-in", dev, baud ?: "NULL", term ?: "N/A", runlevels ?: "[2-5]");

	if (insert)
		LIST_INSERT_HEAD(&tty_list, entry, link);

	/* Register configured limits */
	memcpy(entry->data.rlimit, rlimit, sizeof(entry->data.rlimit));

	if (file && conf_changed(file))
		entry->dirty = 1; /* Modified, restart */
	else
		entry->dirty = 0; /* Not modified */
	_d("TTY %s is %sdirty", dev, entry->dirty ? "" : "NOT ");

	return 0;
}

int tty_unregister(tty_node_t *tty)
{
	if (!tty) {
		_e("Missing argument");
		return errno = EINVAL;
	}

	LIST_REMOVE(tty, link);

	if (tty->data.name)
		free(tty->data.name);
	if (tty->data.baud)
		free(tty->data.baud);
	if (tty->data.term)
		free(tty->data.term);
	if (tty->data.cmd) {
		int i;

		free(tty->data.cmd);
		for (i = 0; i < TTY_MAX_ARGS; i++) {
			if (tty->data.args[i])
				free(tty->data.args[i]);
			tty->data.args[i] = NULL;
		}
	}
	free(tty);

	return 0;
}

tty_node_t *tty_find(char *dev)
{
	tty_node_t *entry;

	LIST_FOREACH(entry, &tty_list, link) {
		if (!strcmp(dev, entry->data.name))
			return entry;
	}

	return NULL;
}

size_t tty_num(void)
{
	size_t num = 0;
	tty_node_t *entry;

	LIST_FOREACH(entry, &tty_list, link)
		num++;

	return num;
}

size_t tty_num_active(void)
{
	size_t num = 0;
	tty_node_t *entry;

	LIST_FOREACH(entry, &tty_list, link) {
		if (entry->data.pid)
			num++;
	}

	return num;
}

tty_node_t *tty_find_by_pid(pid_t pid)
{
	tty_node_t *entry;

	LIST_FOREACH(entry, &tty_list, link) {
		if (entry->data.pid == pid)
			return entry;
	}

	return NULL;
}

static int tty_exist(char *dev)
{
	int fd, result;
	struct termios c;

	fd = open(dev, O_RDWR);
	if (-1 == fd)
		return 1;

	/* XXX: Add check for errno == EIO? */
	result = tcgetattr(fd, &c);
	close(fd);

	return result;
}

void tty_start(finit_tty_t *tty)
{
	char *dev;

	if (tty->pid) {
		_d("%s: TTY already active", tty->name);
		return;
	}

	dev = canonicalize(tty->name);
	if (!dev) {
		_d("%s: Cannot find TTY device: %s", tty->name, strerror(errno));
		return;
	}

	if (tty_exist(dev)) {
		_d("%s: Not a valid TTY: %s", dev, strerror(errno));
		return;
	}

	if (!tty->cmd)
		tty->pid = run_getty(dev, tty->baud, tty->term, tty->noclear, tty->nowait, tty->rlimit);
	else
		tty->pid = run_getty2(dev, tty->cmd, tty->args, tty->noclear, tty->nowait, tty->rlimit);
}

void tty_stop(finit_tty_t *tty)
{
	if (!tty->pid)
		return;

	/*
	 * XXX: TTY handling should be refactored to regular services,
	 * XXX: that way we could rely on the state machine to properly
	 * XXX: send SIGTERM, wait for max 2 sec to collect PID before
	 * XXX: sending SIGKILL.
	 */
	_d("Stopping TTY %s", tty->name);
	kill(tty->pid, SIGKILL);
	waitpid(tty->pid, NULL, 0);
	tty->pid = 0;
}

int tty_enabled(finit_tty_t *tty)
{
	if (!tty)
		return 0;

	if (ISSET(tty->runlevels, runlevel))
		return 1;

	return 0;
}

/*
 * Fallback shell if no TTYs are active
 */
int tty_fallback(pid_t lost)
{
#ifdef FALLBACK_SHELL
	if (lost == 1) {
		if (fallback) {
			kill(fallback, SIGKILL);
			fallback = 0;
		}

		return 0;
	}

	if (fallback != lost || tty_num_active())
		return 0;

	fallback = fork();
	if (fallback)
		return 1;

	/*
	 * Become session leader and set controlling TTY
	 * to enable Ctrl-C and job control in shell.
	 */
	setsid();
	ioctl(STDIN_FILENO, TIOCSCTTY, 1);

	_exit(execl(_PATH_BSHELL, _PATH_BSHELL, NULL));
#else
	(void)lost;
#endif /* FALLBACK_SHELL */

	return 0;
}

static void tty_action(tty_node_t *tty)
{
	if (!tty_enabled(&tty->data))
		tty_stop(&tty->data);
	else
		tty_start(&tty->data);
}

/*
 * TTY monitor, called by service_monitor()
 */
int tty_respawn(pid_t pid)
{
	tty_node_t *tty = tty_find_by_pid(pid);

	if (!tty)
		return tty_fallback(pid);

	/* Set DEAD_PROCESS UTMP entry */
	utmp_set_dead(pid);

	/* Clear PID to be able to respawn it. */
	tty->data.pid = 0;
	tty_action(tty);

	return 1;
}

/*
 * Called after reload of /etc/finit.d/, stop/start TTYs
 */
void tty_reload(char *dev)
{
	tty_node_t *tty;

	if (dev) {
		tty = tty_find(dev);
		if (!tty) {
			logit(LOG_WARNING, "No TTY registered for %s", dev);
			return;
		}

		tty_action(tty);
		tty->dirty = 0;
		return;
	}

	tty_sweep();

	LIST_FOREACH(tty, &tty_list, link) {
		tty_action(tty);
		tty->dirty = 0;
	}
}

/* Start all TTYs that exist in the system and are allowed at this runlevel */
void tty_runlevel(void)
{
	tty_node_t *tty;

	LIST_FOREACH(tty, &tty_list, link)
		tty_action(tty);

	/* Start fallback shell if enabled && no TTYs */
	tty_fallback(tty_num_active() > 0 ? 1 : 0);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
