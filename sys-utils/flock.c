/*   Copyright 2003-2005 H. Peter Anvin - All Rights Reserved
 *
 *   Permission is hereby granted, free of charge, to any person
 *   obtaining a copy of this software and associated documentation
 *   files (the "Software"), to deal in the Software without
 *   restriction, including without limitation the rights to use,
 *   copy, modify, merge, publish, distribute, sublicense, and/or
 *   sell copies of the Software, and to permit persons to whom
 *   the Software is furnished to do so, subject to the following
 *   conditions:
 *
 *   The above copyright notice and this permission notice shall
 *   be included in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 *   OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 *   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 *   OTHER DEALINGS IN THE SOFTWARE.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "c.h"
#include "nls.h"

static void __attribute__((__noreturn__)) usage(int ex)
{
	fprintf(stderr, USAGE_HEADER);
	fprintf(stderr,
		_(" %1$s [options] <file descriptor number>\n"
		  " %1$s [options] <file> -c <command>\n"
		  " %1$s [options} <directory> -c <command>\n"),
		program_invocation_short_name);
	fputs(USAGE_OPTIONS, stderr);
	fputs(_(  " -s  --shared             get a shared lock\n"), stderr);
	fputs(_(  " -x  --exclusive          get an exclusive lock (default)\n"), stderr);
	fputs(_(  " -u  --unlock             remove a lock\n"), stderr);
	fputs(_(  " -n  --nonblock           fail rather than wait\n"), stderr);
	fputs(_(  " -w  --timeout <secs>     wait for a limited amount of time\n"), stderr);
	fputs(_(  " -o  --close              close file descriptor before running command\n"), stderr);
	fputs(_(  " -c  --command <command>  run a single command string through the shell\n"), stderr);
	fprintf(stderr, USAGE_SEPARATOR);
	fprintf(stderr, USAGE_HELP);
	fprintf(stderr, USAGE_VERSION);
	fprintf(stderr, USAGE_MAN_TAIL("flock(1)"));
	exit(ex);
}

static sig_atomic_t timeout_expired = 0;

static void timeout_handler(int sig __attribute__((__unused__)))
{
	timeout_expired = 1;
}

static char *strtotimeval(const char *str, struct timeval *tv)
{
	char *s;
	/* Fractional seconds */
	long fs;
	int i;

	tv->tv_sec = strtol(str, &s, 10);
	fs = 0;
	if (*s == '.') {
		s++;
		for (i = 0; i < 6; i++) {
			if (!isdigit(*s))
				break;
			fs *= 10;
			fs += *s++ - '0';
		}
		for (; i < 6; i++)
			fs *= 10;
		while (isdigit(*s))
			s++;
	}
	tv->tv_usec = fs;
	return s;
}

int main(int argc, char *argv[])
{
	struct itimerval timeout, old_timer;
	int have_timeout = 0;
	int type = LOCK_EX;
	int block = 0;
	int open_accmode;
	int fd = -1;
	int opt, ix;
	int do_close = 0;
	int status;
	char *eon;
	char **cmd_argv = NULL, *sh_c_argv[4];
	const char *filename = NULL;
	struct sigaction sa, old_sa;

	static const struct option long_options[] = {
		{"shared", no_argument, NULL, 's'},
		{"exclusive", no_argument, NULL, 'x'},
		{"unlock", no_argument, NULL, 'u'},
		{"nonblocking", no_argument, NULL, 'n'},
		{"nb", no_argument, NULL, 'n'},
		{"timeout", required_argument, NULL, 'w'},
		{"wait", required_argument, NULL, 'w'},
		{"close", no_argument, NULL, 'o'},
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'V'},
		{NULL, 0, NULL, 0}
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	if (argc < 2)
		usage(EX_USAGE);

	memset(&timeout, 0, sizeof timeout);

	optopt = 0;
	while ((opt =
		getopt_long(argc, argv, "+sexnouw:hV?", long_options,
			    &ix)) != EOF) {
		switch (opt) {
		case 's':
			type = LOCK_SH;
			break;
		case 'e':
		case 'x':
			type = LOCK_EX;
			break;
		case 'u':
			type = LOCK_UN;
			break;
		case 'o':
			do_close = 1;
			break;
		case 'n':
			block = LOCK_NB;
			break;
		case 'w':
			have_timeout = 1;
			eon = strtotimeval(optarg, &timeout.it_value);
			if (*eon)
				usage(EX_USAGE);
			break;
		case 'V':
			printf("flock (%s)\n", PACKAGE_STRING);
			exit(EX_OK);
		default:
			/* optopt will be set if this was an unrecognized
			 * option, i.e.  *not* 'h' or '?
			 */
			usage(optopt ? EX_USAGE : 0);
			break;
		}
	}

	if (argc > optind + 1) {
		/* Run command */
		if (!strcmp(argv[optind + 1], "-c") ||
		    !strcmp(argv[optind + 1], "--command")) {
			if (argc != optind + 3)
				errx(EX_USAGE,
				     _("%s requires exactly one command argument"),
				     argv[optind + 1]);
			cmd_argv = sh_c_argv;
			cmd_argv[0] = getenv("SHELL");
			if (!cmd_argv[0] || !*cmd_argv[0])
				cmd_argv[0] = _PATH_BSHELL;
			cmd_argv[1] = "-c";
			cmd_argv[2] = argv[optind + 2];
			cmd_argv[3] = 0;
		} else {
			cmd_argv = &argv[optind + 1];
		}

		filename = argv[optind];
		open_accmode =
		    ((type == LOCK_SH
		      || access(filename,
				R_OK | W_OK) < 0) ? O_RDONLY : O_RDWR);
		fd = open(filename, open_accmode | O_NOCTTY | O_CREAT, 0666);
		/* Linux doesn't like O_CREAT on a directory, even though it
		 * should be a no-op; POSIX doesn't allow O_RDWR or O_WRONLY
		 */
		if (fd < 0 && errno == EISDIR)
			fd = open(filename, O_RDONLY | O_NOCTTY);

		if (fd < 0) {
			warn(_("cannot open lock file %s"), argv[optind]);
			if (errno == ENOMEM || errno == EMFILE || errno == ENFILE)
				exit(EX_OSERR);
			if (errno == EROFS || errno == ENOSPC)
				exit(EX_CANTCREAT);
			exit(EX_NOINPUT);
		}
	} else if (optind < argc) {
		/* Use provided file descriptor */
		fd = (int)strtol(argv[optind], &eon, 10);
		if (*eon || !argv[optind]) {
			errx(EX_USAGE, _("bad number: %s"), argv[optind]);
		}
	} else {
		/* Bad options */
		errx(EX_USAGE, _("requires file descriptor, file or directory"));
	}

	if (have_timeout) {
		if (timeout.it_value.tv_sec == 0 &&
		    timeout.it_value.tv_usec == 0) {
			/* -w 0 is equivalent to -n; this has to be
			 * special-cased because setting an itimer to zero
			 * means disabled!
			 */
			have_timeout = 0;
			block = LOCK_NB;
		} else {
			memset(&sa, 0, sizeof sa);
			sa.sa_handler = timeout_handler;
			sa.sa_flags = SA_RESETHAND;
			sigaction(SIGALRM, &sa, &old_sa);
			setitimer(ITIMER_REAL, &timeout, &old_timer);
		}
	}

	while (flock(fd, type | block)) {
		switch (errno) {
		case EWOULDBLOCK:
			/* -n option set and failed to lock. The numeric
			 * exit value is specified in man flock.1
			 */
			exit(1);
		case EINTR:
			/* Signal received */
			if (timeout_expired)
				/* -w option set and failed to lock. The
				 * numeric exit value is specified in man
				 * flock.1
				 */
				exit(1);
			/* otherwise try again */
			continue;
		default:
			/* Other errors */
			if (filename)
				warn("%s", filename);
			else
				warn("%d", fd);
			exit((errno == ENOLCK
			      || errno == ENOMEM) ? EX_OSERR : EX_DATAERR);
		}
	}

	if (have_timeout) {
		/* Cancel itimer */
		setitimer(ITIMER_REAL, &old_timer, NULL);
		/* Cancel signal handler */
		sigaction(SIGALRM, &old_sa, NULL);
	}

	status = EX_OK;

	if (cmd_argv) {
		pid_t w, f;
		/* Clear any inherited settings */
		signal(SIGCHLD, SIG_DFL);
		f = fork();

		if (f < 0) {
			err(EX_OSERR, _("fork failed"));
		} else if (f == 0) {
			if (do_close)
				close(fd);
			execvp(cmd_argv[0], cmd_argv);
			/* execvp() failed */
			warn("%s", cmd_argv[0]);
			_exit((errno == ENOMEM) ? EX_OSERR : EX_UNAVAILABLE);
		} else {
			do {
				w = waitpid(f, &status, 0);
				if (w == -1 && errno != EINTR)
					break;
			} while (w != f);

			if (w == -1) {
				status = EXIT_FAILURE;
				warn(_("waitpid failed"));
			} else if (WIFEXITED(status))
				status = WEXITSTATUS(status);
			else if (WIFSIGNALED(status))
				status = WTERMSIG(status) + 128;
			else
				/* WTF? */
				status = EX_OSERR;
		}
	}

	return status;
}
