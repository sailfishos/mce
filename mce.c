/**
 * @file mce.c
 * Mode Control Entity - main file
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 *
 * mce is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * mce is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mce.h"
#include "mce-log.h"
#include "mce-conf.h"
#include "mce-gconf.h"
#include "mce-dbus.h"
#include "mce-dsme.h"
#include "mce-modules.h"
#include "mce-command-line.h"
#ifdef ENABLE_SENSORFW
# include "mce-sensorfw.h"
#endif
#include "tklock.h"
#include "powerkey.h"
#include "event-input.h"
#include "event-switches.h"
#include "modetransition.h"
#ifdef ENABLE_WAKELOCKS
# include "libwakelock.h"
#endif

#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <systemd/sd-daemon.h>

/** Path to the lockfile */
#define MCE_LOCKFILE			"/var/run/mce.pid"
/** Name shown by --help etc. */
#define PRG_NAME			"mce"

static const gchar *progname;	/**< Used to store the name of the program */

/** The GMainLoop used by MCE */
static GMainLoop *mainloop = 0;

/** Wrapper for write() for use when we do not care if it works or not
 *
 * Main purpose is to stop static analyzers from nagging us when
 * we really do not care whether the data gets written or not
 *
 * @param fd   file descriptor to write to
 * @param data data to write
 * @param size amount of data to write
 */
static void no_error_check_write(int fd, const void *data, size_t size)
{
	// do the write, then ...
	ssize_t rc = TEMP_FAILURE_RETRY(write(fd, data, size));
	// try to silence static analyzers by doing /something/ with rc
	if( rc == -1 )
		rc = rc;
}

/**
 * Initialise locale support
 *
 * @param name The program name to output in usage/version information
 * @return 0 on success, non-zero on failure
 */
static gint init_locales(const gchar *const name)
{
	gint status = 0;

#ifdef ENABLE_NLS
	setlocale(LC_ALL, "");

	if ((bindtextdomain(name, LOCALEDIR) == 0) && (errno == ENOMEM)) {
		status = errno;
		goto EXIT;
	}

	if ((textdomain(name) == 0) && (errno == ENOMEM)) {
		status = errno;
		goto EXIT;
	}

EXIT:
	/* In this error-message we don't use _(), since we don't
	 * know where the locales failed, and we probably won't
	 * get a reasonable result if we try to use them.
	 */
	if (status != 0) {
		fprintf(stderr,
			"%s: `%s' failed; %s. Aborting.\n",
			name, "init_locales", g_strerror(status));
	}

	if (errno != ENOMEM)
		errno = 0;
#endif /* ENABLE_NLS */
	progname = name;

	return status;
}

void mce_quit_mainloop(void)
{
#ifdef ENABLE_WAKELOCKS
	/* We are on exit path -> block suspend for good */
	wakelock_block_suspend_until_exit();
#endif

	/* Exit immediately if there is no mainloop to terminate */
	if( !mainloop ) {
		exit(EXIT_FAILURE);
	}

	/* Terminate mainloop */
	g_main_loop_quit(mainloop);
}

#ifdef ENABLE_WAKELOCKS
/** Disable automatic suspend and remove wakelocks mce might hold
 *
 * This function should be called just before mce process terminates
 * so that we do not leave the system in a non-functioning state
 */
static void mce_cleanup_wakelocks(void)
{
	/* We are on exit path -> block suspend for good */
	wakelock_block_suspend_until_exit();

	wakelock_unlock("mce_display_on");
	wakelock_unlock("mce_input_handler");
	wakelock_unlock("mce_cpu_keepalive");
	wakelock_unlock("mce_display_stm");
	wakelock_unlock("mce_powerkey_stm");
	wakelock_unlock("mce_proximity_stm");
	wakelock_unlock("mce_bluez_wait");
	wakelock_unlock("mce_led_breathing");
	wakelock_unlock("mce_lpm_off");
	wakelock_unlock("mce_tklock_notify");
}
#endif // ENABLE_WAKELOCKS

/** Disable autosuspend then exit via default signal handler
 *
 * @param signr the signal to exit through
 */
static void mce_exit_via_signal(int signr) __attribute__((noreturn));
static void mce_exit_via_signal(int signr)
{
	sigset_t ss;

	sigemptyset(&ss);
	sigaddset(&ss, SIGALRM);

	/* Give us N seconds to exit */
	signal(SIGALRM, SIG_DFL);
	alarm(3);
	sigprocmask(SIG_UNBLOCK, &ss, 0);

#ifdef ENABLE_WAKELOCKS
	/* Cancel auto suspend */
	mce_cleanup_wakelocks();
#endif
	/* Try to exit via default handler */
	signal(signr, SIG_DFL);
	sigaddset(&ss, signr);
	sigprocmask(SIG_UNBLOCK, &ss, 0);
	raise(signr);

	/* Or just abort as the last resort*/
	abort();
}

/** Suspend safe replacement for _exit(1), abort() etc
 */
void mce_abort(void)
{
	mce_exit_via_signal(SIGABRT);
}

static void mce_tx_signal_cb(int sig);

/**
 * Signal handler
 *
 * @param signr Signal type
 */
static void signal_handler(const gint signr)
{
	switch (signr) {
	case SIGUSR1:
		/* switch to debug verbosity */
		mce_log_set_verbosity(LL_DEBUG);
		mce_log(LL_DEBUG, "switching to DEBUG verbosity level");
		break;

	case SIGUSR2:
		/* switch to normal verbosity */
		mce_log_set_verbosity(LL_DEBUG);
		mce_log(LL_DEBUG, "switching to WARNING verbosity level");
		mce_log_set_verbosity(LL_WARN);
		break;

	case SIGHUP:
		/* Possibly for re-reading configuration? */
		break;

	case SIGINT:
	case SIGQUIT:
	case SIGTERM:
		/* Just die if we somehow get here without having a mainloop */
		if( !mainloop ) {
			mce_exit_via_signal(signr);
		}

		/* Terminate mainloop */
		mce_quit_mainloop();
		break;

	case SIGPIPE:
		break;

	default:
		/* Should never happen */
		break;
	}
}

/** Install handlers for signals we need to trap
 */
static void install_signal_handlers(void)
{
	static const int sig[] = {
		SIGUSR1,
		SIGUSR2,
		SIGHUP,

		SIGINT,
		SIGQUIT,
		SIGTERM,

#ifdef ENABLE_WAKELOCKS
		SIGABRT,
		SIGILL,
		SIGFPE,
		SIGSEGV,
		SIGPIPE,
		SIGALRM,
		SIGBUS,
		SIGTSTP,
#endif

		-1
	};

	for( size_t i = 0; sig[i] != -1; ++i ) {
		signal(sig[i], mce_tx_signal_cb);
	}
}

/** Pipe used for transferring signals out of signal handler context */
static int signal_pipe[2] = {-1, -1};

/** I/O watch id for signal_pipe */
static guint signal_pipe_id = 0;

/** GIO callback for reading signals from pipe
 *
 * @param channel   io channel for signal pipe
 * @param condition call reason
 * @param data      user data
 *
 * @return TRUE (or aborts on error)
 */
static gboolean mce_rx_signal_cb(GIOChannel *channel,
				 GIOCondition condition, gpointer data)
{
	// we just want the cb ...
	(void)channel; (void)condition; (void)data;

	int sig = 0;
	int got = TEMP_FAILURE_RETRY(read(signal_pipe[0], &sig, sizeof sig));

	if( got != sizeof sig ) {
		mce_abort();
	}

	/* handle the signal */
	signal_handler(sig);

	/* keep the io watch */
	return TRUE;
}

/** Signal handler callback for writing signals to pipe
 *
 * @param sig the signal number to pass to mainloop via pipe
 */
static void mce_tx_signal_cb(int sig)
{
	/* NOTE: this function must be kept async-signal-safe! */

	static volatile int exit_tries = 0;

	static const char msg[] = "\n*** BREAK ***\n";
#ifdef ENABLE_WAKELOCKS
	static const char die[] = "\n*** UNRECOVERABLE FAILURE ***\n";
#endif

	/* FIXME: Should really use sigaction() to avoid having
	 * the default handler active until we manage to restore
	 * our handler here ... */
	signal(sig, mce_tx_signal_cb);

	switch( sig )
	{
	case SIGINT:
	case SIGQUIT:
	case SIGTERM:
		/* Make sure that a stuck or non-existing mainloop does
		 * not stop us from handling at least repeated terminating
		 signals ... */

#ifdef ENABLE_WAKELOCKS
		/* We are on exit path -> block suspend for good */
		wakelock_block_suspend_until_exit();
#endif

		no_error_check_write(STDERR_FILENO, msg, sizeof msg - 1);

		if( !mainloop || ++exit_tries >= 2 ) {
			mce_abort();
		}
		break;

#ifdef ENABLE_WAKELOCKS
	case SIGABRT:
	case SIGILL:
	case SIGFPE:
	case SIGSEGV:
	case SIGALRM:
	case SIGBUS:
		/* Unrecoverable failures can't be handled in the mainloop
		 * Terminate but disable suspend first */
		no_error_check_write(STDERR_FILENO, die, sizeof die - 1);
		mce_exit_via_signal(sig);
		break;

	case SIGTSTP:
		/* Stopping mce could also lead to unrecoverable suspend */
		break;
#endif
	default:
		break;
	}

	/* transfer the signal to mainloop via pipe */
	int did = TEMP_FAILURE_RETRY(write(signal_pipe[1], &sig, sizeof sig));

	if( did != (int)sizeof sig ) {
		mce_abort();
	}
}

/** Remove pipe and io watch for handling signals
 */
static void mce_quit_signal_pipe(void)
{
	if( signal_pipe_id )
		g_source_remove(signal_pipe_id), signal_pipe_id = 0;

	if( signal_pipe[0] != -1 )
		close(signal_pipe[0]), signal_pipe[0] = -1;

	if( signal_pipe[1] != -1 )
		close(signal_pipe[1]), signal_pipe[1] = -1;
}

/** Create a pipe and io watch for handling signal from glib mainloop
 */
static gboolean mce_init_signal_pipe(void)
{
	int         result  = FALSE;
	GIOChannel *channel = 0;

	if( pipe(signal_pipe) == -1 )
		goto EXIT;

	if( (channel = g_io_channel_unix_new(signal_pipe[0])) == 0 )
		goto EXIT;

	signal_pipe_id =
		g_io_add_watch(channel,
			       G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
			       mce_rx_signal_cb, 0);
	if( !signal_pipe_id )
		goto EXIT;

	result = TRUE;

EXIT:
	if( channel != 0 ) g_io_channel_unref(channel);

	return result;
}

/**
 * Daemonize the program
 *
 * @return TRUE if MCE is started during boot, FALSE otherwise
 */
static gboolean daemonize(void)
{
	gint retries = 0;
	gint i = 0;
	gchar str[10];

	if (getppid() == 1)
		goto EXIT;	/* Already daemonized */

	/* Detach from process group */
	switch (fork()) {
	case -1:
		/* Parent - Failure */
		mce_log(LL_CRIT, "daemonize: fork failed: %s",
			g_strerror(errno));
		exit(EXIT_FAILURE);

	case 0:
		/* Child */
		break;

	default:
		/* Parent -- Success */

		/* One main() one exit() - in this case the parent
		 * must not call atexit handlers etc */
		_exit(EXIT_SUCCESS);
	}

	/* Detach TTY */
	setsid();

	/* Close all file descriptors and redirect stdio to /dev/null */
	if ((i = getdtablesize()) == -1)
		i = 256;

	while (--i >= 0) {
		if (close(i) == -1) {
			if (retries > 10) {
				mce_log(LL_CRIT,
					"close() was interrupted more than "
					"10 times. Exiting.");
				exit(EXIT_FAILURE);
			}

			if (errno == EINTR) {
				mce_log(LL_INFO,
					"close() was interrupted; retrying.");
				errno = 0;
				i++;
				retries++;
			} else if (errno == EBADF) {
				/* Ignore invalid file descriptors */
				errno = 0;
			} else {
				mce_log(LL_CRIT,
					"Failed to close() fd %d; %s. "
					"Exiting.",
					i + 1, g_strerror(errno));
				exit(EXIT_FAILURE);
			}
		} else {
			retries = 0;
		}
	}

	if ((i = open("/dev/null", O_RDWR)) == -1) {
		mce_log(LL_CRIT,
			"Cannot open `/dev/null'; %s. Exiting.",
			g_strerror(errno));
		exit(EXIT_FAILURE);
	}

	if ((dup(i) == -1)) {
		mce_log(LL_CRIT,
			"Failed to dup() `/dev/null'; %s. Exiting.",
			g_strerror(errno));
		exit(EXIT_FAILURE);
	}

	if ((dup(i) == -1)) {
		mce_log(LL_CRIT,
			"Failed to dup() `/dev/null'; %s. Exiting.",
			g_strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* Set umask */
	umask(022);

	/* Set working directory */
	if ((chdir("/tmp") == -1)) {
		mce_log(LL_CRIT,
			"Failed to chdir() to `/tmp'; %s. Exiting.",
			g_strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* Single instance */
	if ((i = open(MCE_LOCKFILE, O_RDWR | O_CREAT, 0640)) == -1) {
		mce_log(LL_CRIT,
			"Cannot open lockfile; %s. Exiting.",
			g_strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (lockf(i, F_TLOCK, 0) == -1) {
		mce_log(LL_CRIT, "Already running. Exiting.");
		exit(EXIT_FAILURE);
	}

	sprintf(str, "%d\n", getpid());
	no_error_check_write(i, str, strlen(str));
	close(i);

	/* Ignore TTY signals */
	signal(SIGTSTP, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);

	/* Ignore child terminate signal */
	signal(SIGCHLD, SIG_IGN);

EXIT:
	return 0;
}

/** Helper for determining how long common prefix two strings have
 *
 * @param str1 non null string
 * @param str2 non null string
 *
 * @return length of common prefix strings share
 */
static size_t common_length(const char *str1, const char *str2)
{
	size_t i;
	for( i = 0; str1[i] && str1[i] == str2[i]; ++i ) {}
	return i;
}

/** Handle --trace=flags options
 *
 * @param flags comma separated list of trace domains
 *
 * @return TRUE on success, FALSE if unknown domains used
 */
static gboolean mce_enable_trace(const char *flags)
{
	static const struct {
		const char *domain;
		void (*callback)(void);
	} lut[] = {
#ifdef ENABLE_WAKELOCKS
		{ "wakelocks", lwl_enable_logging },
#endif
		{ NULL, NULL }
	};

	gboolean  res = TRUE;
	gchar    *tmp = g_strdup(flags);

	gchar    *now, *zen;
	size_t    bi, bn;

	for( now = tmp; now; now = zen ) {
		if( (zen = strchr(now, ',')) )
			*zen++ = 0;

		// initialize to: no match
		bi = bn = 0;

		for( size_t ti = 0; lut[ti].domain; ++ti ) {
			size_t tn = common_length(lut[ti].domain, now);

			// all of flag mathed?
			if( now[tn] )
				continue;

			// better or equal as the previous best?
			if( bn <= tn )
				bi = ti, bn = tn;

			// full match found?
			if( !lut[ti].domain[tn] )
				break;
		}

		// did we find a match?
		if( !bn ) {
			fprintf(stderr, "unknown trace domain: '%s'\n", now);
			res = FALSE;
		}
		else {
			// report if non-full match was used
			if( lut[bi].domain[bn] )
				fprintf(stderr, "trace: %s\n", lut[bi].domain);
			lut[bi].callback();
		}
	}

	g_free(tmp);
	return res;
}

/* ========================================================================= *
 * COMMAND LINE OPTIONS
 * ========================================================================= */

static struct
{
	bool daemonflag;
	int  logtype;
	int  verbosity;
	bool systembus;
	bool show_module_info;
	bool systemd_notify;
	int  auto_exit;
} mce_args =
{
	.daemonflag       = false,
	.logtype          = MCE_LOG_SYSLOG,
	.verbosity        = LL_DEFAULT,
	.systembus        = true,
	.show_module_info = false,
	.systemd_notify   = false,
	.auto_exit        = -1,
};

static bool mce_do_help(const char *arg);
static bool mce_do_version(const char *arg);

static bool mce_do_daemonize(const char *arg)
{
	(void)arg;
	mce_args.daemonflag = true;
	return true;
}

static bool mce_do_force_stderr(const char *arg)
{
	(void)arg;
	mce_args.logtype = MCE_LOG_STDERR;
	return true;
}

static bool mce_do_force_syslog(const char *arg)
{
	(void)arg;
	mce_args.logtype = MCE_LOG_SYSLOG;
	return true;
}

static bool mce_do_auto_exit(const char *arg)
{
	mce_args.auto_exit = arg ? strtol(arg, 0, 0) : 5;
	return true;
}
static bool mce_do_log_function(const char *arg)
{
	mce_log_add_pattern(arg);
	return true;
}

static bool mce_do_verbose(const char *arg)
{
	(void)arg;
	if( mce_args.verbosity < LL_DEBUG )
		mce_args.verbosity++;
	return true;
}

static bool mce_do_quiet(const char *arg)
{
	(void)arg;
	if( mce_args.verbosity > LL_NONE )
		mce_args.verbosity--;
	return true;
}

static bool mce_do_session_bus(const char *arg)
{
	(void)arg;
	mce_args.systembus = false;
	return true;
}

static bool mce_do_show_module_info(const char *arg)
{
	(void)arg;
	mce_args.show_module_info = true;
	return true;
}

static bool mce_do_systemd(const char *arg)
{
	(void)arg;
	mce_args.systemd_notify = true;
	return true;
}

static bool mce_do_trace(const char *arg)
{
	return mce_enable_trace(arg);
}

static const mce_opt_t options[] =
{

	{
		.name        = "help",
		.flag        = 'h',
		.with_arg    = mce_do_help,
		.without_arg = mce_do_help,
		.values      = "option|\"all\"",
		.usage       =
			"Show usage information\n"
			"\n"
			"If the optional argument is given, more detailed information is\n"
			"given about matching options. Using \"all\" lists all options\n"
	},
	{
		.name        = "version",
		.flag        = 'V',
		.without_arg = mce_do_version,
		.usage       =
			"Output version information and exit\n"

	},

	{
		.name        = "verbose",
		.flag        = 'v',
		.without_arg = mce_do_verbose,
		.usage       =
			"Increase debug message verbosity\n"

	},
	{
		.name        = "quiet",
		.flag        = 'q',
		.without_arg = mce_do_quiet,
		.usage       =
			"Decrease debug message verbosity\n"

	},
	{
		.name        = "systemd",
		.flag        = 'n',
		.without_arg = mce_do_systemd,
		.usage       =
			"Notify systemd when started up\n"

	},
	{
		.name        = "daemonflag",
		.flag        = 'd',
		.without_arg = mce_do_daemonize,
		.usage       =
			"Run MCE as a daemon\n"

	},
	{
		.name        = "force-syslog",
		.flag        = 's',
		.without_arg = mce_do_force_syslog,
		.usage       =
			"Log to syslog even when not daemonized\n"

	},
	{
		.name        = "force-stderr",
		.flag        = 'T',
		.without_arg = mce_do_force_stderr,
		.usage       =
			"Log to stderr even when daemonized\n"

	},
	{
		.name        = "session",
		.flag        = 'S',
		.without_arg = mce_do_session_bus,
		.usage       =
			"Use the session bus instead of the system bus for D-Bus\n"
	},
	{
		.name        = "show-module-info",
		.flag        = 'M',
		.without_arg = mce_do_show_module_info,
		.usage       =
			"Show information about loaded modules\n"
	},
	{
		.name        = "trace",
		.flag        = 't',
		.with_arg    = mce_do_trace,
		.values      = "what",
		.usage       =
			"enable domain specific debug logging; supported values:\n"
			"  wakelocks\n"
	},
	{
		.name        = "log-function",
		.flag        = 'l',
		.with_arg    = mce_do_log_function,
		.values      = "file:func",
		.usage       =
			"Add function logging override"
	},
	{
		.name        = "auto-exit",
		.values      = "seconds",
		.with_arg    = mce_do_auto_exit,
		.without_arg = mce_do_auto_exit,
		.usage       =
			"Exit after mainloop gets idle\n"
			"\n"
			"This is usefult for mce startup debugging only.\n"
	},
	// sentinel
	{
		.name = 0
	}
};

static bool mce_do_version(const char *arg)
{
	(void)arg;

	static const char vers[] = G_STRINGIFY(PRG_VERSION);
	static const char info[] =
		"Written by David Weinehall.\n"
		"\n"
		"Copyright (C) 2004-2010 Nokia Corporation.  "
		"All rights reserved.\n";

	fprintf(stdout,	"%s v%s\n%s", progname, vers, info);
	exit(EXIT_SUCCESS);
}

static bool mce_do_help(const char *arg)
{
	fprintf(stdout,
		"Mode Control Entity\n"
		"\n"
		"USAGE\n"
		"\tmce [OPTION] ...\n"
		"\n"
		"OPTIONS\n");

	mce_command_line_usage(options, arg);

	if( !arg )
		goto EXIT;

	fprintf(stdout,
		"REPORTING BUGS\n"
		"\tSend e-mail to: <simo.piiroinen@jollamobile.com>\n");

EXIT:
	exit(EXIT_SUCCESS);
}

static gboolean mce_auto_exit_cb(gpointer aptr)
{
	(void)aptr;

	if( mce_args.auto_exit <= 0 ) {
		mce_log(LL_WARN, "exit");
		mce_quit_mainloop();
	}
	else {
		mce_log(LL_WARN, "idle");
		g_timeout_add_seconds(mce_args.auto_exit, mce_auto_exit_cb, 0);
		mce_args.auto_exit = 0;
	}
	return FALSE;
}

/* ========================================================================= *
 * MAIN ENTRY POINT
 * ========================================================================= */

/**
 * Main
 *
 * @param argc Number of command line arguments
 * @param argv Array with command line arguments
 * @return 0 on success, non-zero on failure
 */
int main(int argc, char **argv)
{
	int status = EXIT_FAILURE;

	/* Initialise support for locales, and set the program-name */
	if (init_locales(PRG_NAME) != 0)
		goto EXIT;

	/* Parse the command-line options */
	if( !mce_command_line_parse(options, argc, argv) )
		goto EXIT;

	/* We don't take any non-flag arguments */
	if ((argc - optind) > 0) {
		fprintf(stderr, "%s: Too many arguments\n"
			"Try: `%s --help' for more information.\n",
			progname, progname);
		exit(EXIT_FAILURE);
	}

	mce_log_open(PRG_NAME, LOG_DAEMON, mce_args.logtype);
	mce_log_set_verbosity(mce_args.verbosity);

#ifdef ENABLE_WAKELOCKS
	/* Since mce enables automatic suspend, we must try to
	 * disable it when mce process exits */
	atexit(mce_cleanup_wakelocks);
#endif

	/* Identify mce version & flavor on start up */
	mce_log(LL_WARN, "MCE %s (%s) starting up",
		G_STRINGIFY(PRG_VERSION),
		(LL_DEVEL == LL_EXTRA) ? "devel" : "release");

	/* Daemonize if requested */
	if( mce_args.daemonflag )
		daemonize();

	/* Register a mainloop */
	mainloop = g_main_loop_new(NULL, FALSE);

	/* Signal handlers can be installed once we have a mainloop */
	if( !mce_init_signal_pipe() ) {
		mce_log(LL_CRIT, "Failed to initialise signal pipe");
		exit(EXIT_FAILURE);
	}
	install_signal_handlers();

	/* Initialise subsystems */

	/* Get configuration options */
	if( !mce_conf_init() ) {
		mce_log(LL_CRIT,
			"Failed to initialise configuration options");
		exit(EXIT_FAILURE);
	}

	/* Initialise D-Bus */
	if( !mce_dbus_init(mce_args.systembus) ) {
		mce_log(LL_CRIT,
			"Failed to initialise D-Bus");
		exit(EXIT_FAILURE);
	}

	/* Initialise GConf
	 * pre-requisite: g_type_init()
	 */
	if (mce_gconf_init() == FALSE) {
		mce_log(LL_CRIT,
			"Cannot connect to default GConf engine");
		exit(EXIT_FAILURE);
	}

	/* Setup all datapipes */
	mce_datapipe_init();

	/* Initialise mode management
	 * pre-requisite: mce_gconf_init()
	 * pre-requisite: mce_dbus_init()
	 */
	if (mce_mode_init() == FALSE) {
		goto EXIT;
	}

	/* Initialise DSME
	 * pre-requisite: mce_gconf_init()
	 * pre-requisite: mce_dbus_init()
	 * pre-requisite: mce_mce_init()
	 */
	if( !mce_dsme_init() )
		goto EXIT;

	/* Initialise powerkey driver */
	if (mce_powerkey_init() == FALSE) {
		goto EXIT;
	}

	/* Initialise /dev/input driver
	 * pre-requisite: g_type_init()
	 */
	if (mce_input_init() == FALSE) {
		goto EXIT;
	}

	/* Initialise switch driver */
	if (mce_switches_init() == FALSE) {
		goto EXIT;
	}

	/* Initialise tklock driver */
	if (mce_tklock_init() == FALSE) {
		goto EXIT;
	}

#ifdef ENABLE_SENSORFW
	if( !mce_sensorfw_init() ) {
		goto EXIT;
	}
#endif

	/* Load all modules */
	if (mce_modules_init() == FALSE) {
		goto EXIT;
	}

	if( mce_args.show_module_info ) {
		mce_modules_dump_info();
		goto EXIT;
	}

	/* MCE startup succeeded */
	status = EXIT_SUCCESS;

	/* Tell systemd that we have started up */
	if( mce_args.systemd_notify ) {
		mce_log(LL_NOTICE, "notifying systemd");
		sd_notify(0, "READY=1");
	}
	/* Debug feature: exit after startup is finished */
	if( mce_args.auto_exit >= 0 ) {
		mce_log(LL_WARN, "auto-exit scheduled");
		g_idle_add(mce_auto_exit_cb, 0);
	}

	/* Run the main loop */
	g_main_loop_run(mainloop);

	/* If we get here, the main loop has terminated;
	 * either because we requested or because of an error
	 */
EXIT:
	/* Unload all modules */
	mce_modules_exit();

	/* Call the exit function for all components */
#ifdef ENABLE_SENSORFW
	mce_sensorfw_quit();
#endif
	mce_tklock_exit();
	mce_switches_exit();
	mce_input_exit();
	mce_powerkey_exit();
	mce_dsme_exit();
	mce_mode_exit();

	/* Free all datapipes */
	mce_datapipe_quit();

	/* Call the exit function for all subsystems */
	mce_gconf_exit();
	mce_dbus_exit();
	mce_conf_exit();

	/* If the mainloop is initialised, unreference it */
	if (mainloop != NULL) {
		g_main_loop_unref(mainloop);
		mainloop = 0;
	}

	/* Close signal pipe & remove io watch for it */
	mce_quit_signal_pipe();

	/* Log a farewell message and close the log */
	mce_log(LL_INFO, "Exiting...");

	/* No more logging expected */
	mce_log_close();

	return status;
}
