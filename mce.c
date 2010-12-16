/**
 * @file mce.c
 * Mode Control Entity - main file
 * <p>
 * Copyright © 2004-2010 Nokia Corporation and/or its subsidiary(-ies).
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
#include <glib.h>
#include <glib-object.h>		/* g_type_init() */

#include <errno.h>			/* errno, ENOMEM */
#include <fcntl.h>			/* open(), O_RDWR, O_CREAT */
#include <stdio.h>			/* fprintf(), sprintf(),
					 * stdout, stderr
					 */
#include <getopt.h>			/* getopt_long(),
					 * struct options
					 */
#include <signal.h>			/* signal(),
					 * SIGTSTP, SIGTTOU, SIGTTIN,
					 * SIGCHLD, SIGUSR1, SIGHUP,
					 * SIGTERM, SIG_IGN
					 */
#include <stdlib.h>			/* exit(), EXIT_FAILURE, EXIT_SUCCESS */
#include <string.h>			/* strlen() */
#include <unistd.h>			/* close(), lockf(), fork(), chdir(),
					 * getpid(), getppid(), setsid(),
					 * write(), getdtablesize(), dup(),
					 * F_TLOCK
					 */
#include <sys/stat.h>			/* umask() */

#include "mce.h"			/* _(),
					 * setlocale() -- indirect,
					 * bindtextdomain(),
					 * textdomain(),
					 * mainloop,
					 * system_state_pipe,
					 * master_radio_pipe,
					 * call_state_pipe,
					 * call_type_pipe,
					 * submode_pipe,
					 * display_state_pipe,
					 * display_brightness_pipe,
					 * led_brightness_pipe,
					 * led_pattern_activate_pipe,
					 * led_pattern_deactivate_pipe,
					 * key_backlight_pipe,
					 * keypress_pipe,
					 * touchscreen_pipe,
					 * device_inactive_pipe,
					 * lockkey_pipe,
					 * keyboard_slide_pipe,
					 * lid_cover_pipe,
					 * lens_cover_pipe,
					 * proximity_sensor_pipe,
					 * tk_lock_pipe,
					 * charger_state_pipe,
					 * battery_status_pipe,
					 * battery_level_pipe,
					 * inactivity_timeout_pipe,
					 * audio_route_pipe,
					 * usb_cable_pipe,
					 * jack_sense_pipe,
					 * power_saving_mode_pipe,
					 * thermal_state_pipe,
					 * MCE_STATE_UNDEF,
					 * MCE_INVALID_MODE_INT32,
					 * CALL_STATE_NONE,
					 * NORMAL_CALL,
					 * MCE_ALARM_UI_INVALID_INT32,
					 * MCE_NORMAL_SUBMODE,
					 * MCE_DISPLAY_UNDEF,
					 * LOCK_UNDEF,
					 * BATTERY_STATUS_UNDEF,
					 * THERMAL_STATE_UNDEF,
					 * DEFAULT_INACTIVITY_TIMEOUT
					 */

#include "mce-log.h"			/* mce_log_open(), mce_log_close(),
					 * mce_log_set_verbosity(), mce_log(),
					 * LL_*
					 */
#include "mce-conf.h"			/* mce_conf_init(),
					 * mce_conf_exit()
					 */
#include "mce-dbus.h"			/* mce_dbus_init(),
					 * mce_dbus_exit()
					 */
#include "mce-dsme.h"			/* mce_dsme_init(),
					 * mce_dsme_exit()
					 */
#include "mce-gconf.h"			/* mce_gconf_init(),
					 * mce_gconf_exit()
					 */
#include "mce-modules.h"		/* mce_modules_dump_info(),
					 * mce_modules_init(),
					 * mce_modules_exit()
					 */
#include "event-input.h"		/* mce_input_init(),
					 * mce_input_exit()
					 */
#include "event-switches.h"		/* mce_switches_init(),
					 * mce_switches_exit()
					 */
#include "connectivity.h"		/* mce_connectivity_init(),
					 * mce_connectivity_exit()
					 */
#include "datapipe.h"			/* setup_datapipe(),
					 * free_datapipe()
					 */
#include "modetransition.h"		/* mce_mode_init(),
					 * mce_mode_exit()
					 */

/* "TBD" Modules; eventually this should be handled differently */
#include "tklock.h"			/* mce_tklock_init(),
					 * mce_tklock_exit()
					 */
#include "powerkey.h"			/* mce_powerkey_init(),
					 * mce_powerkey_exit()
					 */

/** Path to the lockfile */
#define MCE_LOCKFILE			"/var/run/mce.pid"
/** Name shown by --help etc. */
#define PRG_NAME			"mce"

extern int optind;			/**< Used by getopt */
extern char *optarg;			/**< Used by getopt */

static const gchar *progname;	/**< Used to store the name of the program */

/**
 * Display usage information
 */
static void usage(void)
{
	fprintf(stdout,
		_("Usage: %s [OPTION]...\n"
		  "Mode Control Entity\n"
		  "\n"
		  "  -d, --daemonflag           run MCE as a daemon\n"
		  "      --force-syslog         log to syslog even when not "
		  "daemonized\n"
		  "      --force-stderr         log to stderr even when "
		  "daemonized\n"
		  "  -S, --session              use the session bus instead\n"
		  "                               of the "
		  "system bus for D-Bus\n"
		  "      --show-module-info     show information about "
		  "loaded modules\n"
		  "      --debug-mode           run even if dsme fails\n"
		  "      --quiet                decrease debug message "
		  "verbosity\n"
		  "      --verbose              increase debug message "
		  "verbosity\n"
		  "      --help                 display this help and exit\n"
		  "      --version              output version information "
		  "and exit\n"
		  "\n"
		  "Report bugs to <david.weinehall@nokia.com>\n"),
		progname);
}

/**
 * Display version information
 */
static void version(void)
{
	fprintf(stdout, _("%s v%s\n%s"),
		progname,
		G_STRINGIFY(PRG_VERSION),
		_("Written by David Weinehall.\n"
		  "\n"
		  "Copyright (C) 2004-2010 Nokia Corporation.  "
		  "All rights reserved.\n"));
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

/**
 * Signal handler
 *
 * @param signr Signal type
 */
static void signal_handler(const gint signr)
{
	switch (signr) {
	case SIGUSR1:
		/* We'll probably want some way to communicate with MCE */
		break;

	case SIGHUP:
		/* Possibly for re-reading configuration? */
		break;

	case SIGTERM:
		/* This should be done through a pipe or signalfd instead */
		g_main_loop_quit(mainloop);
		break;

	default:
		/* Should never happen */
		break;
	}
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
		/* Failure */
		mce_log(LL_CRIT, "daemonize: fork failed: %s",
			g_strerror(errno));
		mce_log_close();
		exit(EXIT_FAILURE);

	case 0:
		/* Child */
		break;

	default:
		/* Parent -- exit */
		exit(EXIT_SUCCESS);
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
				mce_log_close();
				exit(EXIT_FAILURE);
			}

			if (errno == EINTR) {
				mce_log(LL_INFO,
					"close() was interrupted; retrying.");
				errno = 0;
				i++;
				retries++;
			} else if (errno == EBADF) {
				mce_log(LL_ERR,
					"Failed to close() fd %d; %s. "
					"Ignoring.",
					i + 1, g_strerror(errno));
				errno = 0;
			} else {
				mce_log(LL_CRIT,
					"Failed to close() fd %d; %s. "
					"Exiting.",
					i + 1, g_strerror(errno));
				mce_log_close();
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
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	if ((dup(i) == -1)) {
		mce_log(LL_CRIT,
			"Failed to dup() `/dev/null'; %s. Exiting.",
			g_strerror(errno));
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	if ((dup(i) == -1)) {
		mce_log(LL_CRIT,
			"Failed to dup() `/dev/null'; %s. Exiting.",
			g_strerror(errno));
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	/* Set umask */
	umask(022);

	/* Set working directory */
	if ((chdir("/tmp") == -1)) {
		mce_log(LL_CRIT,
			"Failed to chdir() to `/tmp'; %s. Exiting.",
			g_strerror(errno));
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	/* Single instance */
	if ((i = open(MCE_LOCKFILE, O_RDWR | O_CREAT, 0640)) == -1) {
		mce_log(LL_CRIT,
			"Cannot open lockfile; %s. Exiting.",
			g_strerror(errno));
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	if (lockf(i, F_TLOCK, 0) == -1) {
		mce_log(LL_CRIT, "Already running. Exiting.");
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	sprintf(str, "%d\n", getpid());
	write(i, str, strlen(str));
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

/**
 * Main
 *
 * @param argc Number of command line arguments
 * @param argv Array with command line arguments
 * @return 0 on success, non-zero on failure
 */
int main(int argc, char **argv)
{
	int optc;
	int opt_index;

	int verbosity = LL_DEFAULT;
	int logtype = -1;

	gint status = 0;
	gboolean show_module_info = FALSE;
	gboolean daemonflag = FALSE;
	gboolean systembus = TRUE;
	gboolean debugmode = FALSE;

	const char optline[] = "dS";

	struct option const options[] = {
		{ "daemonflag", no_argument, 0, 'd' },
		{ "force-syslog", no_argument, 0, 's' },
		{ "force-stderr", no_argument, 0, 'T' },
		{ "session", no_argument, 0, 'S' },
		{ "show-module-info", no_argument, 0, 'M' },
		{ "debug-mode", no_argument, 0, 'D' },
		{ "quiet", no_argument, 0, 'q' },
		{ "verbose", no_argument, 0, 'v' },
		{ "help", no_argument, 0, 'h' },
		{ "version", no_argument, 0, 'V' },
		{ 0, 0, 0, 0 }
	};

	/* NULL the mainloop */
	mainloop = NULL;

	/* Initialise support for locales, and set the program-name */
	if (init_locales(PRG_NAME) != 0)
		goto EXIT;

	/* Parse the command-line options */
	while ((optc = getopt_long(argc, argv, optline,
				   options, &opt_index)) != -1) {
		switch (optc) {
		case 'd':
			daemonflag = TRUE;
			break;

		case 's':
			if (logtype != -1) {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			logtype = MCE_LOG_SYSLOG;
			break;

		case 'T':
			if (logtype != -1) {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			logtype = MCE_LOG_STDERR;
			break;

		case 'S':
			systembus = FALSE;
			break;

		case 'M':
			show_module_info = TRUE;
			break;

		case 'D':
			debugmode = TRUE;
			break;

		case 'q':
			if (verbosity > LL_NONE)
				verbosity--;
			break;

		case 'v':
			if (verbosity < LL_DEBUG)
				verbosity++;
			break;

		case 'h':
			usage();
			goto EXIT;

		case 'V':
			version();
			goto EXIT;

		default:
			usage();
			status = EINVAL;
			goto EXIT;
		}
	}

	/* We don't take any non-flag arguments */
	if ((argc - optind) > 0) {
		fprintf(stderr,
			_("%s: Too many arguments\n"
			  "Try: `%s --help' for more information.\n"),
			progname, progname);
		status = EINVAL;
		goto EXIT;
	}

	if (logtype == -1)
		logtype = (daemonflag == TRUE) ? MCE_LOG_SYSLOG :
						 MCE_LOG_STDERR;

	mce_log_open(PRG_NAME, LOG_DAEMON, logtype);
	mce_log_set_verbosity(verbosity);

	/* Daemonize if requested */
	if (daemonflag == TRUE)
		daemonize();

	signal(SIGUSR1, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Initialise GType system */
	g_type_init();

	/* Register a mainloop */
	mainloop = g_main_loop_new(NULL, FALSE);

	/* Initialise subsystems */

	/* Get configuration options */
	/* ignore errors; this way the defaults will be used if
	 * the configuration file is invalid or unavailable
	 */
	(void)mce_conf_init();

	/* Initialise D-Bus */
	if (mce_dbus_init(systembus) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to initialise D-Bus");
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	/* Initialise GConf
	 * pre-requisite: g_type_init()
	 */
	if (mce_gconf_init() == FALSE) {
		mce_log(LL_CRIT,
			"Cannot connect to default GConf engine");
		mce_log_close();
		exit(EXIT_FAILURE);
	}

	/* Setup all datapipes */
	setup_datapipe(&system_state_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_STATE_UNDEF));
	setup_datapipe(&master_radio_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&call_state_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(CALL_STATE_NONE));
	setup_datapipe(&call_type_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(NORMAL_CALL));
	setup_datapipe(&alarm_ui_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_ALARM_UI_INVALID_INT32));
	setup_datapipe(&submode_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_NORMAL_SUBMODE));
	setup_datapipe(&display_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_DISPLAY_UNDEF));
	setup_datapipe(&display_brightness_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&led_brightness_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&led_pattern_activate_pipe, READ_ONLY, FREE_CACHE,
		       0, NULL);
	setup_datapipe(&led_pattern_deactivate_pipe, READ_ONLY, FREE_CACHE,
		       0, NULL);
	setup_datapipe(&key_backlight_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&keypress_pipe, READ_WRITE, FREE_CACHE,
		       sizeof (struct input_event), NULL);
	setup_datapipe(&touchscreen_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&device_inactive_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&lockkey_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&keyboard_slide_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&lid_cover_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&lens_cover_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&proximity_sensor_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&tk_lock_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(LOCK_UNDEF));
	setup_datapipe(&charger_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&battery_status_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(BATTERY_STATUS_UNDEF));
	setup_datapipe(&battery_level_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(100));
	setup_datapipe(&camera_button_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(CAMERA_BUTTON_UNDEF));
	setup_datapipe(&inactivity_timeout_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(DEFAULT_INACTIVITY_TIMEOUT));
	setup_datapipe(&audio_route_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(AUDIO_ROUTE_UNDEF));
	setup_datapipe(&usb_cable_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&jack_sense_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&power_saving_mode_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&thermal_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(THERMAL_STATE_UNDEF));

	/* Initialise connectivity monitoring
	 * pre-requisite: g_type_init()
	 */
	if (mce_connectivity_init() == FALSE) {
		status = EXIT_FAILURE;
		goto EXIT;
	}

	/* Initialise mode management
	 * pre-requisite: mce_gconf_init()
	 * pre-requisite: mce_dbus_init()
	 */
	if (mce_mode_init() == FALSE) {
		status = EXIT_FAILURE;
		goto EXIT;
	}

	/* Initialise DSME
	 * pre-requisite: mce_gconf_init()
	 * pre-requisite: mce_dbus_init()
	 * pre-requisite: mce_mce_init()
	 */
	if (mce_dsme_init(debugmode) == FALSE) {
		if (debugmode == FALSE) {
			mce_log(LL_CRIT, "Cannot connect to DSME");
			status = EXIT_FAILURE;
			goto EXIT;
		}
	}

	/* Initialise powerkey driver */
	if (mce_powerkey_init() == FALSE) {
		status = EXIT_FAILURE;
		goto EXIT;
	}

	/* Initialise /dev/input driver
	 * pre-requisite: g_type_init()
	 */
	if (mce_input_init() == FALSE) {
		status = EXIT_FAILURE;
		goto EXIT;
	}

	/* Initialise switch driver */
	if (mce_switches_init() == FALSE) {
		status = EXIT_FAILURE;
		goto EXIT;
	}

	/* Initialise tklock driver */
	if (mce_tklock_init() == FALSE) {
		status = EXIT_FAILURE;
		goto EXIT;
	}

	/* Load all modules */
	if (mce_modules_init() == FALSE) {
		status = EXIT_FAILURE;
		goto EXIT;
	}

	if (show_module_info == TRUE) {
		mce_modules_dump_info();
		goto EXIT;
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
	mce_tklock_exit();
	mce_switches_exit();
	mce_input_exit();
	mce_powerkey_exit();
	mce_dsme_exit();
	mce_mode_exit();
	mce_connectivity_exit();

	/* Free all datapipes */
	free_datapipe(&thermal_state_pipe);
	free_datapipe(&power_saving_mode_pipe);
	free_datapipe(&jack_sense_pipe);
	free_datapipe(&usb_cable_pipe);
	free_datapipe(&audio_route_pipe);
	free_datapipe(&inactivity_timeout_pipe);
	free_datapipe(&battery_level_pipe);
	free_datapipe(&battery_status_pipe);
	free_datapipe(&charger_state_pipe);
	free_datapipe(&tk_lock_pipe);
	free_datapipe(&proximity_sensor_pipe);
	free_datapipe(&lens_cover_pipe);
	free_datapipe(&lid_cover_pipe);
	free_datapipe(&keyboard_slide_pipe);
	free_datapipe(&lockkey_pipe);
	free_datapipe(&device_inactive_pipe);
	free_datapipe(&touchscreen_pipe);
	free_datapipe(&keypress_pipe);
	free_datapipe(&key_backlight_pipe);
	free_datapipe(&led_pattern_deactivate_pipe);
	free_datapipe(&led_pattern_activate_pipe);
	free_datapipe(&led_brightness_pipe);
	free_datapipe(&display_brightness_pipe);
	free_datapipe(&display_state_pipe);
	free_datapipe(&submode_pipe);
	free_datapipe(&alarm_ui_state_pipe);
	free_datapipe(&call_type_pipe);
	free_datapipe(&call_state_pipe);
	free_datapipe(&master_radio_pipe);
	free_datapipe(&system_state_pipe);

	/* Call the exit function for all subsystems */
	mce_gconf_exit();
	mce_dbus_exit();
	mce_conf_exit();

	/* If the mainloop is initialised, unreference it */
	if (mainloop != NULL)
		g_main_loop_unref(mainloop);

	/* Log a farewell message and close the log */
	mce_log(LL_INFO, "Exiting...");
	mce_log_close();

	return status;
}
