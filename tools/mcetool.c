/**
 * @file mcetool.c
 * Tool to test and remote control the Mode Control Entity
 * <p>
 * Copyright © 2005-2011 Nokia Corporation and/or its subsidiary(-ies).
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
#include <glib-object.h>		/* g_type_init(), g_object_unref() */

#include <errno.h>			/* errno,
					 * ENOMEM
					 */
#include <stdio.h>			/* fprintf() */
#include <getopt.h>			/* getopt_long(),
					 * struct option
					 */
#include <stdlib.h>			/* exit(), EXIT_FAILURE */
#include <string.h>			/* strcmp(), strlen(), strdup() */
#include <dbus/dbus.h>
#include <gconf/gconf-client.h>

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include "mcetool.h"

#include "tklock.h"			/* For GConf paths */
#include "modules/display.h"		/* For GConf paths */
#include "modules/powersavemode.h"	/* For GConf paths */

/** Whether to use demo mode hack or the real thing */
#define MCETOOL_USE_DEMOMODE_HACK 0

/** Whether to enable development time debugging */
#define MCETOOL_ENABLE_DEBUG 0

/** Name shown by --help etc. */
#define PRG_NAME			"mcetool"

/** Short powerkey event string; used for arg-parsing */
#define SHORT_EVENT_STR			"short"
/** Double powerkey event string; used for arg-parsing */
#define DOUBLE_EVENT_STR		"double"
/** Long powerkey event string; used for arg-parsing */
#define LONG_EVENT_STR			"long"

/** Blanking inhibit disabled string; used for arg-parsing */
#define BLANKING_INHIBIT_DISABLED		"disabled"
/** Blanking inhibit stay on with charger string; used for arg-parsing */
#define BLANKING_INHIBIT_STAY_ON_WITH_CHARGER	"stay-on-with-charger"
/** Blanking inhibit stay dim with charger string; used for arg-parsing */
#define BLANKING_INHIBIT_STAY_DIM_WITH_CHARGER	"stay-dim-with-charger"
/** Blanking inhibit stay on string; used for arg-parsing */
#define BLANKING_INHIBIT_STAY_ON		"stay-on"
/** Blanking inhibit stay dim string; used for arg-parsing */
#define BLANKING_INHIBIT_STAY_DIM		"stay-dim"

/** Enabled string; used for arg-parsing */
#define ENABLED_STRING				"enabled"
/**
 * Disabled string; used for arg-parsing and for output
 * do not mark this string for translation here, since
 * it's used for parsing too
 */
#define DISABLED_STRING				"disabled"

/** Show unlock screen string; used for output */
#define SHOW_UNLOCK_SCREEN_STRING		_("show unlock screen")
/** Unlock string; used for output */
#define UNLOCK_STRING				_("unlock")
/** Invalid string; used for output */
#define INVALID_STRING				_("invalid")
/** <unset> string; used for output */
#define UNSET_STRING				_("unset")

/** Master string; used for arg-parsing */
#define RADIO_MASTER				"master"
/** Cellular string; used for arg-parsing */
#define RADIO_CELLULAR				"cellular"
/** WLAN string; used for arg-parsing */
#define RADIO_WLAN				"wlan"
/** Bluetooth string; used for arg-parsing */
#define RADIO_BLUETOOTH				"bluetooth"
/** NFC string; used for arg-parsing */
#define RADIO_NFC				"nfc"
/** FM transmitter string; used for arg-parsing */
#define RADIO_FMTX				"fmtx"

/** Define demo mode DBUS method */
#define MCE_DBUS_DEMO_MODE_REQ      		"display_set_demo_mode"

/** Define get config DBUS method */
#define MCE_DBUS_GET_CONFIG_REQ      		"get_config"

/** Define set config DBUS method */
#define MCE_DBUS_SET_CONFIG_REQ      		"set_config"

/** Enums for powerkey events */
enum {
	INVALID_EVENT = -1,		/**< Event not set */
	SHORT_EVENT = 0,		/**< Short powerkey press event */
	LONG_EVENT = 1,			/**< Long powerkey press event */
	DOUBLE_EVENT = 2		/**< Double powerkey press event */
};

extern int optind;			/**< Used by getopt */
extern char *optarg;			/**< Used by getopt */

static const gchar *progname;	/**< Used to store the name of the program */

static DBusConnection *dbus_connection = NULL;	/**< D-Bus connection */

static GConfClient *gconf_client = NULL;	/**< GConf client */

#if MCETOOL_ENABLE_DEBUG
# define debugf(FMT, ARGS...) fprintf(stderr, "D: "FMT, ##ARGS)
#else
# define debugf(FMT, ARGS...) do { }while(0)
#endif

/**
 * Display usage information
 */
static void usage(void)
{
	static const char fmt[] =
		"Usage: %s [OPTION]\n"
		"Mode Control Entity tool\n"
		"\n"
		"  -P, --blank-prevent             send blank prevent request to MCE\n"
		"  -v, --cancel-blank-prevent      send cancel blank prevent request to MCE\n"
		"  -U, --unblank-screen            send unblank request to MCE\n"
		"  -d, --dim-screen                send dim request to MCE\n"
		"  -n, --blank-screen              send blank request to MCE\n"
		"  -b, --set-display-brightness=BRIGHTNESS\n"
		"                                  set the display brightness to BRIGHTNESS;\n"
		"                                    valid values are: 1-5\n"
		"  -I, --set-inhibit-mode=MODE\n"
		"                                  set the blanking inhibit mode to MODE;\n"
		"                                    valid modes are:\n"
		"                                    ``disabled'',\n"
		"                                    ``stay-on-with-charger'', ``stay-on'',\n"
		"                                    ``stay-dim-with-charger'', ``stay-dim''\n"
		"  -D, --set-demo-mode=STATE\n"
		"                                    set the display demo mode  to STATE;\n"
		"                                       valid states are: 'on' and 'off'\n"
		"  -C, --set-cabc-mode=MODE\n"
		"                                  set the CABC mode to MODE;\n"
		"                                    valid modes are:\n"
		"                                    ``off'', ``ui'',\n"
		"                                    ``still-image' and ``moving-image''\n"
		"  -A, --set-color-profile=ID\n"
		"                                  set the color profile id to ID; use --get-color-profile-ids\n"
		"                                    to get available values\n"
		"  -a, --get-color-profile-ids\n"
		"                                  get available color profile ids (see --set-color-profile)\n"
		"  -c, --set-call-state=STATE:TYPE\n"
		"                                  set the call state to STATE and the call type\n"
		"                                    to TYPE; valid states are:\n"
		"                                    ``none'', ``ringing'',\n"
		"                                    ``active'' and ``service''\n"
		"                                    valid types are:\n"
		"                                    ``normal'' and ``emergency''\n"
		"  -r, --enable-radio=RADIO\n"
		"                                  enable the specified radio; valid radios are:\n"
		"                                    ``master'', ``cellular'',\n"
		"                                    ``wlan'' and ``bluetooth'';\n"
		"                                    ``master'' affects all radios\n"
		"  -R, --disable-radio=RADIO\n"
		"                                  disable the specified radio; valid radios are:\n"
		"                                    ``master'', ``cellular'',\n"
		"                                    ``wlan'' and ``bluetooth'';\n"
		"                                    ``master'' affects all radios\n"
		"  -p, --set-power-saving-mode=MODE\n"
		"                                  set the power saving mode; valid modes are:\n"
		"                                    ``enabled'' and ``disabled''\n"
		"  -F, --set-forced-psm=MODE\n"
		"                                  the forced power saving mode to MODE;\n"
		"                                    valid modes are:\n"
		"                                    ``enabled'' and ``disabled''\n"
		"  -T, --set-psm-threshold=VALUE\n"
		"                                  set the threshold for the power saving mode;\n"
		"                                    valid values are:\n"
		"                                    10, 20, 30, 40, 50\n"
		"  -k, --set-tklock-mode=MODE\n"
		"                                  set the touchscreen/keypad lock mode;\n"
		"                                    valid modes are:\n"
		"                                    ``locked'', ``locked-dim'',\n"
		"                                    ``locked-delay'',\n"
		"                                    and ``unlocked''\n"
		"  -l, --enable-led                enable LED framework\n"
		"  -L, --disable-led               disable LED framework\n"
		"  -y, --activate-led-pattern=PATTERN\n"
		"                                  activate a LED pattern\n"
		"  -Y, --deactivate-led-pattern=PATTERN\n"
		"                                  deactivate a LED pattern\n"
		"  -e, --powerkey-event=TYPE       trigger a powerkey event; valid types are:\n"
		"                                    ``short'', ``double'' and ``long''\n"
		"  -N, --status                    output MCE status\n"
		"  -B, --block                     block after executing commands\n"
		"  -S, --session                   use the session bus instead of the system bus\n"
		"                                    for D-Bus\n"
		"  -h, --help                      display this help and exit\n"
		"  -V, --version                   output version information and exit\n"
		"\n"
		"If no option is specified, the status is output.\n"
		"\n"
		"Report bugs to <david.weinehall@nokia.com>\n"
		;

	fprintf(stdout, fmt, progname);
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
		  "Copyright (C) 2005-2011 Nokia Corporation.  "
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
		return 0;
	}

EXIT:
	/* In this error-message we don't use _(), since we don't
	 * know where the locales failed, and we probably won't
	 * get a reasonable result if we try to use them.
	 */
	if (status != 0) {
		fprintf(stderr,
			"%s: `%s' failed; %s. Aborting.\n",
			name, "init_locales", g_strerror(errno));
	} else {
		progname = name;
		errno = 0;
	}
#else
	progname = name;
#endif /* ENABLE_NLS */

	return status;
}

/**
 * Create a new D-Bus signal, with proper error checking
 * will exit if an error occurs
 *
 * @param path The signal path
 * @param interface The signal interface
 * @param name The name of the signal to send
 * @return A new DBusMessage
 */
static DBusMessage *dbus_new_signal(const gchar *const path,
				    const gchar *const interface,
				    const gchar *const name)
{
	DBusMessage *msg;

	if ((msg = dbus_message_new_signal(path, interface, name)) == NULL) {
		fprintf(stderr, "No memory for new signal!\n");
		exit(EXIT_FAILURE);
	}

	return msg;
}

/**
 * Create a new D-Bus method call, with proper error checking
 * will exit if an error occurs
 *
 * @param service The method call service
 * @param path The method call path
 * @param interface The method call interface
 * @param name The name of the method to call
 * @return A new DBusMessage
 */
static DBusMessage *dbus_new_method_call(const gchar *const service,
					 const gchar *const path,
					 const gchar *const interface,
					 const gchar *const name)
{
	DBusMessage *msg;

	if ((msg = dbus_message_new_method_call(service, path,
						interface, name)) == NULL) {
		fprintf(stderr,
			"Cannot allocate memory for D-Bus method call!\n");
		exit(EXIT_FAILURE);
	}

	return msg;
}

/**
 * Send a D-Bus message
 * Side effects: frees msg
 *
 * @param msg The D-Bus message to send
 * @return TRUE on success, FALSE on out of memory
 */
static gboolean dbus_send_message(DBusMessage *const msg)
{
	if (dbus_connection_send(dbus_connection, msg, NULL) == FALSE) {
		dbus_message_unref(msg);
		return FALSE;
	}

	dbus_connection_flush(dbus_connection);
	dbus_message_unref(msg);

	return TRUE;
}

/**
 * Call a D-Bus method and return the reply as a DBusMessage
 *
 * @param method The method to call
 * @param arg A pointer to the string to append;
 *            the string is freed after use
 * @return a DbusMessage on success, NULL on failure
 */
static DBusMessage *mcetool_dbus_call_with_reply(const gchar *const method,
						 gchar **arg)
{
	DBusMessage *reply = NULL;
	DBusMessage *msg;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	if ((msg = dbus_message_new_method_call(MCE_SERVICE,
						MCE_REQUEST_PATH,
						MCE_REQUEST_IF,
						method)) == NULL) {
		fprintf(stderr,
			"Cannot allocate memory for D-Bus method call!\n");
		goto EXIT;
	}

	/* Is there an argument to append? */
	if (arg != NULL && *arg != NULL) {
		if (dbus_message_append_args(msg,
					     DBUS_TYPE_STRING, arg,
					     DBUS_TYPE_INVALID) != TRUE) {
			dbus_message_unref(msg);
			fprintf(stderr,
				"Failed to append argument to D-Bus message "
				"for %s\n",
				method);
			goto EXIT;
		}
	}

	reply = dbus_connection_send_with_reply_and_block(dbus_connection, msg, -1, &error);

	dbus_message_unref(msg);

	if (arg && *arg) {
		free(*arg);
		*arg = NULL;
	}

	if (dbus_error_is_set(&error) == TRUE) {
		fprintf(stderr,
			"Could not call method %s: %s; exiting\n",
			method, error.message);
		dbus_error_free(&error);
		reply = NULL;
		goto EXIT;
	}

EXIT:
	return reply;
}

/**
 * Call a D-Bus method and return the reply as a string
 *
 * @param method The method to call
 * @param[in,out] arg An pointer to the string to append;
 *                    the string is freed after use.
 *                    if a reply is expected, a string with the reply will be
 *                    returned through this pointer
 * @param no_reply TRUE if no reply is expected, FALSE if a reply is expected
 * @return 0 on success, EXIT_FAILURE on failure
 */
static gint mcetool_dbus_call_string(const gchar *const method,
				     gchar **arg, const gboolean no_reply)
{
	DBusMessage *reply = NULL;
	DBusMessage *msg;
	gint status = 0;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	if ((msg = dbus_message_new_method_call(MCE_SERVICE,
						MCE_REQUEST_PATH,
						MCE_REQUEST_IF,
						method)) == NULL) {
		fprintf(stderr,
			"Cannot allocate memory for D-Bus method call!\n");
		status = EXIT_FAILURE;
		goto EXIT;
	}

	/* Is there an argument to append? */
	if (arg != NULL && *arg != NULL) {
		if (dbus_message_append_args(msg,
					     DBUS_TYPE_STRING, arg,
					     DBUS_TYPE_INVALID) != TRUE) {
			dbus_message_unref(msg);
			fprintf(stderr,
				"Failed to append argument to D-Bus message "
				"for %s\n",
				method);
			status = EXIT_FAILURE;
			goto EXIT;
		}
	}

	if (no_reply == TRUE) {
		dbus_message_set_no_reply(msg, TRUE);

		if (dbus_connection_send(dbus_connection, msg, NULL) == FALSE) {
			dbus_message_unref(msg);
			goto EXIT;
		}
	} else {
		reply = dbus_connection_send_with_reply_and_block(dbus_connection, msg, -1, &error);
	}

	dbus_message_unref(msg);

	if (arg && *arg) {
		free(*arg);
		*arg = NULL;
	}

	if (dbus_error_is_set(&error) == TRUE) {
		fprintf(stderr,
			"Could not call method %s: %s; exiting\n",
			method, error.message);
		dbus_error_free(&error);
		status = EXIT_FAILURE;
		goto EXIT;
	} else if (reply) {
		gchar *tmp = NULL;

		if (dbus_message_get_args(reply, &error,
					  DBUS_TYPE_STRING, &tmp,
					  DBUS_TYPE_INVALID) == FALSE) {
			fprintf(stderr,
				"Failed to get reply argument from %s: "
				"%s; exiting\n",
				method, error.message);
			dbus_message_unref(reply);
			dbus_error_free(&error);
			status = EXIT_FAILURE;
			goto EXIT;
		}

		*arg = strdup(tmp);
		dbus_message_unref(reply);
	}

EXIT:
	return status;
}

/**
 * Call a D-Bus method and return the reply as a boolean
 *
 * @param method The method to call
 * @param arg A pointer to the string to append;
 *            the string is freed after use
 * @param[out] retval A pointer to pass the reply through
 * @return 0 on success, EXIT_FAILURE on failure
 */
static gint mcetool_dbus_call_bool(const gchar *const method,
				   gchar **arg, gboolean *retval)
{
	DBusMessage *reply = NULL;
	DBusMessage *msg;
	gint status = 0;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	if ((msg = dbus_message_new_method_call(MCE_SERVICE,
						MCE_REQUEST_PATH,
						MCE_REQUEST_IF,
						method)) == NULL) {
		fprintf(stderr,
			"Cannot allocate memory for D-Bus method call!\n");
		status = EXIT_FAILURE;
		goto EXIT;
	}

	/* Is there an argument to append? */
	if (arg != NULL && *arg != NULL) {
		if (dbus_message_append_args(msg,
					     DBUS_TYPE_STRING, arg,
					     DBUS_TYPE_INVALID) != TRUE) {
			dbus_message_unref(msg);
			fprintf(stderr,
				"Failed to append argument to D-Bus message "
				"for %s\n",
				method);
			status = EXIT_FAILURE;
			goto EXIT;
		}
	}

	reply = dbus_connection_send_with_reply_and_block(dbus_connection, msg, -1, &error);

	dbus_message_unref(msg);

	if (arg && *arg) {
		free(*arg);
		*arg = NULL;
	}

	if (dbus_error_is_set(&error) == TRUE) {
		fprintf(stderr,
			"Could not call method %s: %s; exiting\n",
			method, error.message);
		dbus_error_free(&error);
		status = EXIT_FAILURE;
		goto EXIT;
	} else if (reply) {
		gboolean tmp;

		if (dbus_message_get_args(reply, &error,
					  DBUS_TYPE_BOOLEAN, &tmp,
					  DBUS_TYPE_INVALID) == FALSE) {
			fprintf(stderr,
				"Failed to get reply argument from %s: "
				"%s; exiting\n",
				method, error.message);
			dbus_message_unref(reply);
			dbus_error_free(&error);
			status = EXIT_FAILURE;
			goto EXIT;
		}

		*retval = tmp;
		dbus_message_unref(reply);
	}

EXIT:
	return status;
}

/**
 * Call a D-Bus method and return the reply as an unsigned integer
 *
 * @param method The method to call
 * @param arg A pointer to the string to append;
 *            the string is freed after use
 * @param[out] retval A pointer to pass the reply through
 * @return 0 on success, EXIT_FAILURE on failure
 */
static gint mcetool_dbus_call_uint(const gchar *const method,
				   gchar **arg, guint32 *retval)
{
	DBusMessage *reply = NULL;
	DBusMessage *msg;
	gint status = 0;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	if ((msg = dbus_message_new_method_call(MCE_SERVICE,
						MCE_REQUEST_PATH,
						MCE_REQUEST_IF,
						method)) == NULL) {
		fprintf(stderr,
			"Cannot allocate memory for D-Bus method call!\n");
		status = EXIT_FAILURE;
		goto EXIT;
	}

	/* Is there an argument to append? */
	if (arg != NULL && *arg != NULL) {
		if (dbus_message_append_args(msg,
					     DBUS_TYPE_STRING, arg,
					     DBUS_TYPE_INVALID) != TRUE) {
			dbus_message_unref(msg);
			fprintf(stderr,
				"Failed to append argument to D-Bus message "
				"for %s\n",
				method);
			status = EXIT_FAILURE;
			goto EXIT;
		}
	}

	reply = dbus_connection_send_with_reply_and_block(dbus_connection, msg, -1, &error);

	dbus_message_unref(msg);

	if (arg && *arg) {
		free(*arg);
		*arg = NULL;
	}

	if (dbus_error_is_set(&error) == TRUE) {
		fprintf(stderr,
			"Could not call method %s: %s; exiting\n",
			method, error.message);
		dbus_error_free(&error);
		status = EXIT_FAILURE;
		goto EXIT;
	} else if (reply) {
		guint32 tmp;

		if (dbus_message_get_args(reply, &error,
					  DBUS_TYPE_UINT32, &tmp,
					  DBUS_TYPE_INVALID) == FALSE) {
			fprintf(stderr,
				"Failed to get reply argument from %s: "
				"%s; exiting\n",
				method, error.message);
			dbus_message_unref(reply);
			dbus_error_free(&error);
			status = EXIT_FAILURE;
			goto EXIT;
		}

		*retval = tmp;
		dbus_message_unref(reply);
	}

EXIT:
	return status;
}

/**
 * Generic function to send D-Bus messages and signals
 * to send a signal, call dbus_send with service == NULL
 *
 * @param service D-Bus service; for signals, set to NULL
 * @param path D-Bus path
 * @param interface D-Bus interface
 * @param name D-Bus method or signal name to send to
 * @param no_reply FALSE if a reply is expected, TRUE if reply is ignored;
 *                 for signals this is ignored, but for consistency,
 *                 please use TRUE
 * @param first_arg_type The DBUS_TYPE of the first argument in the list
 * @param ... The arguments to append to the D-Bus message; terminate with NULL
 *            Note: the arguments MUST be passed by reference
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbus_send(const gchar *const service, const gchar *const path,
			  const gchar *const interface, const gchar *const name,
			  const gboolean no_reply, int first_arg_type, ...)
{
	DBusMessage *msg;
	gboolean status = FALSE;
	va_list var_args;

	if (service != NULL) {
		msg = dbus_new_method_call(service, path, interface, name);

		if (no_reply == TRUE)
			dbus_message_set_no_reply(msg, TRUE);
	} else {
		msg = dbus_new_signal(path, interface, name);
	}

	/* Append the arguments, if any */
	va_start(var_args, first_arg_type);

	if (first_arg_type != DBUS_TYPE_INVALID) {
		if (dbus_message_append_args_valist(msg,
						    first_arg_type,
						    var_args) == FALSE) {
			fprintf(stderr,
				"Failed to append arguments to D-Bus message\n");
			dbus_message_unref(msg);
			goto EXIT;
		}
	}

	/* Send the signal / call the method */
	if (dbus_send_message(msg) == FALSE) {
		if (service != NULL)
			fprintf(stderr, "Cannot call method %s\n", name);
		else
			fprintf(stderr, "Cannot send signal %s\n", name);

		goto EXIT;
	}

	status = TRUE;

EXIT:
	va_end(var_args);

	return status;
}

/**
 * D-Bus initialisation
 *
 * @param bus_type "system" or "session" bus
 * @return 0 on success, non-zero on failure
 */
static gint mcetool_dbus_init(DBusBusType bus_type)
{
	gint status = 0;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	/* Establish D-Bus connection */
	if ((dbus_connection = dbus_bus_get(bus_type, &error)) == 0) {
		fprintf(stderr,
			"Failed to open connection to message bus; %s\n",
			error.message);
		dbus_error_free(&error);
		status = EXIT_FAILURE;
		goto EXIT;
	}

EXIT:
	return status;
}

/**
 * D-Bus exit
 */
static void mcetool_dbus_exit(void)
{
	/* If there is an established D-Bus connection, unreference it */
	if (dbus_connection != NULL) {
		dbus_connection_unref(dbus_connection);
		dbus_connection = NULL;
	}
}

/**
 * Enable/disable the tklock
 *
 * @param mode The mode to change to; valid modes:
 *             "locked", "locked-dim", "locked-delay", "unlocked"
 * @return TRUE on success, FALSE on FAILURE
 */
static gboolean set_tklock_mode(gchar **mode)
{
	/* com.nokia.mce.request.req_tklock_mode_change */
	return mcetool_dbus_call_string(MCE_TKLOCK_MODE_CHANGE_REQ, mode, TRUE);
}

/**
 * Get and print available color profile ids
 *
 * @return TRUE on success, FALSE on FAILURE
 */
static gboolean get_color_profile_ids(void)
{
	/* com.nokia.mce.request.get_color_profile_ids */
	DBusMessage *reply = NULL;
	DBusError error;
	gchar **ids = NULL;
	gint ids_count = 0, i = 0;
	gboolean status = FALSE;

	reply = mcetool_dbus_call_with_reply(MCE_COLOR_PROFILE_IDS_GET, NULL);

	if (dbus_message_get_args(reply, &error,
				  DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &ids, &ids_count,
				  DBUS_TYPE_INVALID) == FALSE) {
		fprintf(stderr,
			"Failed to get reply argument from %s: "
			"%s; exiting\n",
			MCE_COLOR_PROFILE_IDS_GET, error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	fprintf(stdout, "Available color profiles ids are: \n");

	for(i=0; i < ids_count; ++i) {
		fprintf(stdout, "%s\n", ids[i]);
	}

	dbus_free_string_array(ids);
	status = TRUE;

EXIT:
	dbus_message_unref(reply);
	return status;
}

/**
 * Set color profile id
 *
 * @param id The color profile id;
 *             available ids are printed
 *             by get_color_profile_ids(void)
 * @return TRUE on success, FALSE on FAILURE
 */
static gboolean set_color_profile(gchar **id)
{
	/* com.nokia.mce.request.req_color_profile_change */
	return mcetool_dbus_call_string(MCE_COLOR_PROFILE_CHANGE_REQ, id, TRUE);
}

/**
 * Trigger a powerkey event
 *
 * @param type The type of event to trigger; valid types:
 *             "short", "double", "long"
 * @return TRUE on success, FALSE on FAILURE
 */
static gboolean trigger_powerkey_event(const gint type)
{
	/* com.nokia.mce.request.req_trigger_powerkey_event */
	return dbus_send(MCE_SERVICE, MCE_REQUEST_PATH, MCE_REQUEST_IF,
			 MCE_TRIGGER_POWERKEY_EVENT_REQ, TRUE,
			 DBUS_TYPE_UINT32, &type,
			 DBUS_TYPE_INVALID);
}

/**
 * Enable/Disable the LED
 *
 * @param enable TRUE to enable the LED, FALSE to disable the LED
 * @return TRUE on success, FALSE on FAILURE
 */
static gboolean set_led_state(const gboolean enable)
{
	/* com.nokia.mce.request.req_led_{enable,disable} */
	return dbus_send(MCE_SERVICE, MCE_REQUEST_PATH, MCE_REQUEST_IF,
			 enable ? MCE_ENABLE_LED : MCE_DISABLE_LED, TRUE,
			 DBUS_TYPE_INVALID);
}

/**
 * Activate/Deactivate a LED pattern
 *
 * @param pattern The name of the pattern to activate/deactivate
 * @param activate TRUE to activate pattern, FALSE to deactivate pattern
 * @return TRUE on success, FALSE on FAILURE
 */
static gboolean set_led_pattern_state(const gchar *const pattern,
				      const gboolean activate)
{
	/* com.nokia.mce.request.req_{activate,deactivate}_led_pattern */
	return dbus_send(MCE_SERVICE, MCE_REQUEST_PATH, MCE_REQUEST_IF,
			 activate ? MCE_ACTIVATE_LED_PATTERN :
				    MCE_DEACTIVATE_LED_PATTERN, TRUE,
			 DBUS_TYPE_STRING, &pattern,
			 DBUS_TYPE_INVALID);
}

#ifdef ENABLE_BUILTIN_GCONF
/* Assume ENABLE_BUILTIN_GCONF means mce supports config
 * settings via D-Bus system bus interface. */

/** Helper for getting dbus data type as string
 *
 * @param type dbus data type (DBUS_TYPE_BOOLEAN etc)
 *
 * @return type name with out common prefix (BOOLEAN etc)
 */
static const char *dbushelper_get_type_name(int type)
{
	const char *res = "UNKNOWN";
	switch( type ) {
	case DBUS_TYPE_INVALID:     res = "INVALID";     break;
	case DBUS_TYPE_BYTE:        res = "BYTE";        break;
	case DBUS_TYPE_BOOLEAN:     res = "BOOLEAN";     break;
	case DBUS_TYPE_INT16:       res = "INT16";       break;
	case DBUS_TYPE_UINT16:      res = "UINT16";      break;
	case DBUS_TYPE_INT32:       res = "INT32";       break;
	case DBUS_TYPE_UINT32:      res = "UINT32";      break;
	case DBUS_TYPE_INT64:       res = "INT64";       break;
	case DBUS_TYPE_UINT64:      res = "UINT64";      break;
	case DBUS_TYPE_DOUBLE:      res = "DOUBLE";      break;
	case DBUS_TYPE_STRING:      res = "STRING";      break;
	case DBUS_TYPE_OBJECT_PATH: res = "OBJECT_PATH"; break;
	case DBUS_TYPE_SIGNATURE:   res = "SIGNATURE";   break;
	case DBUS_TYPE_UNIX_FD:     res = "UNIX_FD";     break;
	case DBUS_TYPE_ARRAY:       res = "ARRAY";       break;
	case DBUS_TYPE_VARIANT:     res = "VARIANT";     break;
	case DBUS_TYPE_STRUCT:      res = "STRUCT";      break;
	case DBUS_TYPE_DICT_ENTRY:  res = "DICT_ENTRY";  break;
	default: break;
	}
	return res;
}

/** Helper for testing that iterator points to expected data type
 *
 * @param iter D-Bus message iterator
 * @param want_type D-Bus data type
 *
 * @return TRUE if iterator points to give data type, FALSE otherwise
 */
static gboolean dbushelper_require_type(DBusMessageIter *iter,
					int want_type)
{
	int have_type = dbus_message_iter_get_arg_type(iter);

	if( want_type != have_type ) {
		fprintf(stderr, "expected DBUS_TYPE_%s, got %s\n",
			dbushelper_get_type_name(want_type),
			dbushelper_get_type_name(have_type));
		return FALSE;
	}

	return TRUE;
}

/** Helper for making blocking D-Bus method calls
 *
 * @param req D-Bus method call message to send
 *
 * @return D-Bus method reply message, or NULL on failure
 */
static DBusMessage *dbushelper_call_method(DBusMessage *req)
{
        DBusMessage *rsp = 0;
        DBusError    err = DBUS_ERROR_INIT;

        rsp = dbus_connection_send_with_reply_and_block(dbus_connection,
							req, -1, &err);

        if( !rsp ) {
		fprintf(stderr, "%s.%s: %s: %s\n",
			dbus_message_get_interface(req),
			dbus_message_get_member(req),
			err.name, err.message);
                goto EXIT;
        }

EXIT:
        dbus_error_free(&err);

        return rsp;
}

/** Helper for parsing int value from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param value Where to store the value (not modified on failure)
 *
 * @return TRUE if value could be read, FALSE on failure
 */
static gboolean dbushelper_read_int(DBusMessageIter *iter, gint *value)
{
	dbus_int32_t data = 0;

	if( !dbushelper_require_type(iter, DBUS_TYPE_INT32) )
		return FALSE;

	dbus_message_iter_get_basic(iter, &data);
	dbus_message_iter_next(iter);

	return *value = data, TRUE;
}

/** Helper for parsing boolean value from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param value Where to store the value (not modified on failure)
 *
 * @return TRUE if value could be read, FALSE on failure
 */
static gboolean dbushelper_read_boolean(DBusMessageIter *iter, gboolean *value)
{
	dbus_bool_t data = 0;

	if( !dbushelper_require_type(iter, DBUS_TYPE_BOOLEAN) )
		return FALSE;

	dbus_message_iter_get_basic(iter, &data);
	dbus_message_iter_next(iter);

	return *value = data, TRUE;
}

/** Helper for entering variant container from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param sub  D-Bus message iterator for variant (not modified on failure)
 *
 * @return TRUE if container could be entered, FALSE on failure
 */
static gboolean dbushelper_read_variant(DBusMessageIter *iter, DBusMessageIter *sub)
{
  if( !dbushelper_require_type(iter, DBUS_TYPE_VARIANT) )
  return FALSE;

  dbus_message_iter_recurse(iter, sub);
  dbus_message_iter_next(iter);

  return TRUE;
}

/** Helper for initializing D-Bus message read iterator
 *
 * @param rsp  D-Bus message
 * @param iter D-Bus iterator for parsing message (not modified on failure)
 *
 * @return TRUE if read iterator could be initialized, FALSE on failure
 */
static gboolean dbushelper_init_read_iterator(DBusMessage *rsp,
					      DBusMessageIter *iter)
{
  if( !dbus_message_iter_init(rsp, iter) ) {
    fprintf(stderr, "failed to initialize dbus read iterator\n");
    return FALSE;
  }
  return TRUE;
}

/** Helper for initializing D-Bus message write iterator
 *
 * @param rsp  D-Bus message
 * @param iter D-Bus iterator for appending message (not modified on failure)
 *
 * @return TRUE if append iterator could be initialized, FALSE on failure
 */
static gboolean dbushelper_init_write_iterator(DBusMessage *req,
					       DBusMessageIter *iter)
{
	dbus_message_iter_init_append(req, iter);
	return TRUE;
}

/** Helper for adding int value to D-Bus iterator
 *
 * @param iter Write iterator where to add the value
 * @param value the value to add
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_write_int(DBusMessageIter *iter, gint value)
{
	dbus_int32_t data = value;
	int          type = DBUS_TYPE_INT32;

	if( !dbus_message_iter_append_basic(iter, type, &data) ) {
		fprintf(stderr, "failed to add %s data\n",
			dbushelper_get_type_name(type));
		return FALSE;
	}

	return TRUE;
}

/** Helper for adding boolean value to D-Bus iterator
 *
 * @param iter Write iterator where to add the value
 * @param value the value to add
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_write_boolean(DBusMessageIter *iter, gboolean value)
{
	dbus_bool_t data = value;
	int         type = DBUS_TYPE_BOOLEAN;

	if( !dbus_message_iter_append_basic(iter, type, &data) ) {
		fprintf(stderr, "failed to add %s data\n",
			dbushelper_get_type_name(type));
		return FALSE;
	}

	return TRUE;
}

/** Helper for adding object path value to D-Bus iterator
 *
 * @param iter Write iterator where to add the value
 * @param value the value to add
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_write_path(DBusMessageIter *iter, const gchar *value)
{
	const char *data = value;
	int         type = DBUS_TYPE_OBJECT_PATH;

	if( !dbus_message_iter_append_basic(iter, type, &data) ) {
		fprintf(stderr, "failed to add %s data\n",
			dbushelper_get_type_name(type));
		return FALSE;
	}

	return TRUE;
}

/** Helper for opening a variant container
 *
 * @param stack pointer to D-Bus message iterator pointer (not
                modified on failure)
 *
 * @param signature signature string of the data that will be added to the
 *                  variant container
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_push_variant(DBusMessageIter **stack,
					const char *signature)
{
	DBusMessageIter *iter = *stack;
	DBusMessageIter *sub  = iter + 1;

	if( !dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
					      signature, sub) ) {
		fprintf(stderr, "failed to initialize variant write iterator\n");
		return FALSE;
	}

	*stack = sub;
	return TRUE;
}

/** Helper for closing a container
 *
 * @param stack pointer to D-Bus message iterator pointer
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_pop_container(DBusMessageIter **stack)
{
	DBusMessageIter *sub  = *stack;
	DBusMessageIter *iter = sub - 1;

	gboolean res = dbus_message_iter_close_container(iter, sub);

	*stack = iter;
	return res;
}

/** Helper for abandoning message iterator stack
 *
 * @param stack Start of iterator stack
 * @param iter  Current iterator within the stack
 */
static void dbushelper_abandon_stack(DBusMessageIter *stack,
				     DBusMessageIter *iter)
{
	while( iter-- > stack )
		dbus_message_iter_abandon_container(iter, iter+1);
}

/** Helper for making MCE D-Bus method calls
 *
 * @param method name of the method in mce request interface
 * @param first_arg_type as with dbus_message_append_args()
 * @param ... must be terminated with DBUS_TYPE_INVALID
 */
static DBusMessage *mcetool_config_request(const gchar *const method)
{
        DBusMessage *req = 0;

	req = dbus_message_new_method_call(MCE_SERVICE,
					   MCE_REQUEST_PATH,
					   MCE_REQUEST_IF,
					   method);
        if( !req ) {
                fprintf(stderr,
                        "%s.%s: can't allocate method call\n",
			MCE_REQUEST_IF, method);
	}

        return req;
}

/**
 * Init function for the mcetool GConf handling
 *
 * @return TRUE on success, FALSE on failure
 */
static gint mcetool_gconf_init(void)
{
        gint      res = EXIT_SUCCESS;
        DBusError err = DBUS_ERROR_INIT;

	if( !dbus_connection ) {
		fprintf(stderr, "No D-Bus connection, blocking config access\n");
		goto EXIT;
	}

	if( !dbus_bus_name_has_owner(dbus_connection, MCE_SERVICE, &err) ) {
		if( dbus_error_is_set(&err) ) {
			fprintf(stderr, "%s: %s: %s\n", MCE_SERVICE,
				err.name, err.message);
		}
		fprintf(stderr, "MCE not running, blocking config access\n");
		goto EXIT;
	}

        /* just provide non-null pointer */
        gconf_client = calloc(1,1);

EXIT:
        dbus_error_free(&err);

        return res;
}

/**
 * Exit function for the mcetool GConf handling
 */
static void mcetool_gconf_exit(void)
{
        /* just free the dummy pointer */
        free(gconf_client), gconf_client = 0;
}

/**
 * Return a boolean from the specified GConf key
 *
 * @param key The GConf key to get the value from
 * @param value Will contain the value on return
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_get_bool(const gchar *const key, gboolean *value)
{
	debugf("@%s(%s)\n", __FUNCTION__, key);

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

	DBusMessageIter body, variant;

        if( !gconf_client )
                goto EXIT;

        if( !(req = mcetool_config_request(MCE_DBUS_GET_CONFIG_REQ)) )
		goto EXIT;
	if( !dbushelper_init_write_iterator(req, &body) )
		goto EXIT;
	if( !dbushelper_write_path(&body, key) )
		goto EXIT;

	if( !(rsp = dbushelper_call_method(req)) )
		goto EXIT;
	if( !dbushelper_init_read_iterator(rsp, &body) )
		goto EXIT;
	if( !dbushelper_read_variant(&body, &variant) )
		goto EXIT;

	res = dbushelper_read_boolean(&variant, value);

EXIT:
        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/**
 * Return an integer from the specified GConf key
 *
 * @param key The GConf key to get the value from
 * @param value Will contain the value on return
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_get_int(const gchar *const key, gint *value)
{
	debugf("@%s(%s)\n", __FUNCTION__, key);

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

	DBusMessageIter body, variant;

        if( !gconf_client )
                goto EXIT;

        if( !(req = mcetool_config_request(MCE_DBUS_GET_CONFIG_REQ)) )
		goto EXIT;
	if( !dbushelper_init_write_iterator(req, &body) )
		goto EXIT;
	if( !dbushelper_write_path(&body, key) )
		goto EXIT;

	if( !(rsp = dbushelper_call_method(req)) )
		goto EXIT;
	if( !dbushelper_init_read_iterator(rsp, &body) )
		goto EXIT;
	if( !dbushelper_read_variant(&body, &variant) )
		goto EXIT;

	res = dbushelper_read_int(&variant, value);

EXIT:
        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/**
 * Set a boolean GConf key to the specified value
 *
 * @param key The GConf key to set the value of
 * @param value The value to set the key to
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_set_bool(const gchar *const key,
                                       const gboolean value)
{
	debugf("@%s(%s, %d)\n", __FUNCTION__, key, value);

	static const char sig[] = DBUS_TYPE_BOOLEAN_AS_STRING;

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

	DBusMessageIter stack[2];
	DBusMessageIter *wpos = stack;
	DBusMessageIter *rpos = stack;

        if( !gconf_client )
                goto EXIT;

        if( !(req = mcetool_config_request(MCE_DBUS_SET_CONFIG_REQ)) )
		goto EXIT;
	if( !dbushelper_init_write_iterator(req, wpos) )
		goto EXIT;
	if( !dbushelper_write_path(wpos, key) )
		goto EXIT;
	if( !dbushelper_push_variant(&wpos, sig) )
		goto EXIT;
	if( !dbushelper_write_boolean(wpos, value) )
		goto EXIT;
	if( !dbushelper_pop_container(&wpos) )
		goto EXIT;
	if( wpos != stack )
		abort();

	if( !(rsp = dbushelper_call_method(req)) )
		goto EXIT;
	if( !dbushelper_init_read_iterator(rsp, rpos) )
		goto EXIT;
	if( !dbushelper_read_boolean(rpos, &res) )
		res = FALSE;

EXIT:
	// make sure write iterator stack is collapsed
	dbushelper_abandon_stack(stack, wpos);

        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/**
 * Set an integer GConf key to the specified value
 *
 * @param key The GConf key to set the value of
 * @param value The value to set the key to
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_set_int(const gchar *const key, const gint value)
{
	debugf("@%s(%s, %d)\n", __FUNCTION__, key, value);

	static const char sig[] = DBUS_TYPE_INT32_AS_STRING;

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

	DBusMessageIter stack[2];
	DBusMessageIter *wpos = stack;
	DBusMessageIter *rpos = stack;

        if( !gconf_client )
                goto EXIT;

	// construct request
        if( !(req = mcetool_config_request(MCE_DBUS_SET_CONFIG_REQ)) )
		goto EXIT;
	if( !dbushelper_init_write_iterator(req, wpos) )
		goto EXIT;
	if( !dbushelper_write_path(wpos, key) )
		goto EXIT;
	if( !dbushelper_push_variant(&wpos, sig) )
		goto EXIT;
	if( !dbushelper_write_int(wpos, value) )
		goto EXIT;
	if( !dbushelper_pop_container(&wpos) )
		goto EXIT;
	if( wpos != stack )
		abort();

	// get reply and process it
	if( !(rsp = dbushelper_call_method(req)) )
		goto EXIT;
	if( !dbushelper_init_read_iterator(rsp, rpos) )
		goto EXIT;
	if( !dbushelper_read_boolean(rpos, &res) )
		res = FALSE;

EXIT:
	// make sure write iterator stack is collapsed
	dbushelper_abandon_stack(stack, wpos);

        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}
#else // !defined(ENABLE_BUILTIN_GCONF)
/* Use regular GConf API must be used for mce settings queries */

/**
 * Init function for the mcetool GConf handling
 *
 * @return TRUE on success, FALSE on failure
 */
static gint mcetool_gconf_init(void)
{
	gint status = 0;

	/* Trying to use gconf without already existing session
	 * bus can only yield problems -> disable gconf access
	 */
	if( !getenv("DBUS_SESSION_BUS_ADDRESS") ) {
		fprintf(stderr, "No session bus - disabling gconf accesss\n");
		goto EXIT;
	}

	gconf_client = gconf_client_get_default();

	if (gconf_client == NULL) {
		fprintf(stderr, "Could not get default GConf client\n");
		status = EXIT_FAILURE;
		goto EXIT;
	}

EXIT:
	return status;
}

/**
 * Exit function for the mcetool GConf handling
 */
static void mcetool_gconf_exit(void)
{
	/* Unreference GConf */
	if (gconf_client != NULL)
		g_object_unref(gconf_client);
}

/**
 * Return a boolean from the specified GConf key
 *
 * @param key The GConf key to get the value from
 * @param value Will contain the value on return
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_get_bool(const gchar *const key, gboolean *value)
{
	gboolean status = FALSE;
	GError *error = NULL;
	GConfValue *gcv;

	if( !gconf_client )
		goto EXIT;

	gcv = gconf_client_get(gconf_client, key, &error);

	/* If the value isn't set, just return */
	if (gcv == NULL)
		goto EXIT;

	if (gcv->type != GCONF_VALUE_BOOL) {
		fprintf(stderr,
			"\n"
			"GConf key %s should have type: %d, "
			"but has type: %d\n"
			"\n",
			key, GCONF_VALUE_BOOL, gcv->type);
		goto EXIT;
	}

	*value = gconf_value_get_bool(gcv);

	status = TRUE;

EXIT:
	g_clear_error(&error);

	return status;
}

/**
 * Return an integer from the specified GConf key
 *
 * @param key The GConf key to get the value from
 * @param value Will contain the value on return
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_get_int(const gchar *const key, gint *value)
{
	gboolean status = FALSE;
	GError *error = NULL;
	GConfValue *gcv;

	if( !gconf_client )
		goto EXIT;

	gcv = gconf_client_get(gconf_client, key, &error);

	/* If the value isn't set, just return */
	if (gcv == NULL)
		goto EXIT;

	if (gcv->type != GCONF_VALUE_INT) {
		fprintf(stderr,
			"\n"
			"GConf key %s should have type: %d, "
			"but has type: %d\n"
			"\n",
			key, GCONF_VALUE_INT, gcv->type);
		goto EXIT;
	}

	*value = gconf_value_get_int(gcv);

	status = TRUE;

EXIT:
	g_clear_error(&error);

	return status;
}

/**
 * Set a boolean GConf key to the specified value
 *
 * @param key The GConf key to set the value of
 * @param value The value to set the key to
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_set_bool(const gchar *const key,
				       const gboolean value)
{
	gboolean status = FALSE;

	if( !gconf_client )
		goto EXIT;

	if (gconf_client_set_bool(gconf_client, key, value, NULL) == FALSE) {
		fprintf(stderr,
			"Failed to write %s to GConf\n", key);
		goto EXIT;
	}

	/* synchronise if possible, ignore errors */
	gconf_client_suggest_sync(gconf_client, NULL);

	status = TRUE;

EXIT:
	return status;
}

/**
 * Set an integer GConf key to the specified value
 *
 * @param key The GConf key to set the value of
 * @param value The value to set the key to
 * @return TRUE on success, FALSE on failure
 */
static gboolean mcetool_gconf_set_int(const gchar *const key, const gint value)
{
	gboolean status = FALSE;

	if( !gconf_client )
		goto EXIT;

	if (gconf_client_set_int(gconf_client, key, value, NULL) == FALSE) {
		fprintf(stderr,
			"Failed to write %s to GConf\n", key);
		goto EXIT;
	}

	/* synchronise if possible, ignore errors */
	gconf_client_suggest_sync(gconf_client, NULL);

	status = TRUE;

EXIT:
	return status;
}
#endif // !defined(ENABLE_BUILTIN_GCONF)

/**
 * Print mce related information
 *
 * @return 0 on success, EXIT_FAILURE on failure
 */
static gint mcetool_get_status(void)
{
	gint status = 0;
	gchar *cabc = NULL;
	gchar *tklock = NULL;
	gchar *display = NULL;
	gchar *color_profile = NULL;
	const gchar *inhibit_string = NULL;
	gchar *mce_version = NULL;
	gchar *callstate = NULL;
	gchar *calltype = NULL;
	const gchar *policy_string = NULL;
	gint brightness = DEFAULT_DISP_BRIGHTNESS;
	gint dim_timeout = DEFAULT_DIM_TIMEOUT;
	gint blank_timeout = DEFAULT_BLANK_TIMEOUT;
	gint adaptive_dimming_threshold = -1;
	gboolean retval;
	gboolean inactive = FALSE;
	gboolean keyboard_backlight = FALSE;
	gboolean power_saving_mode = TRUE;
	gboolean adaptive_dimming_enabled = TRUE;
	gboolean active_psm_state = DEFAULT_POWER_SAVING_MODE;
	gboolean forced_psm = FALSE;
	gboolean tklock_autolock = DEFAULT_TK_AUTOLOCK;
	gint blank_inhibit = -1;
	gint psm_threshold = DEFAULT_PSM_THRESHOLD;
	gint doubletap_gesture_policy = DEFAULT_DOUBLETAP_GESTURE_POLICY;
	dbus_uint32_t radio_states = 0;
	DBusMessage *reply = NULL;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	/* Get radio states */
	status = mcetool_dbus_call_uint(MCE_RADIO_STATES_GET, NULL,
					&radio_states);

	if (status != 0)
		goto EXIT;

	fprintf(stdout,
		_("\n"
		  "MCE status:\n"
		  "-----------\n"));

	/* Get the version; just ignore if no reply */
	status = mcetool_dbus_call_string(MCE_VERSION_GET, &mce_version, FALSE);

	if (status == 0) {
		fprintf(stdout,
			" %-40s %s\n",
			_("MCE version:"),
			(mce_version == NULL) ? _("unknown") : mce_version);
	}

	free(mce_version);

	fprintf(stdout,
		" %-40s\n",
		_("Radio states:"));
	fprintf(stdout,
		"         %-32s %s\n",
		_("Master:"),
		radio_states & MCE_RADIO_STATE_MASTER ? _("enabled (Online)") : _("disabled (Offline)"));
	fprintf(stdout,
		"         %-32s %s\n",
		_("Cellular:"),
		radio_states & MCE_RADIO_STATE_CELLULAR ? _("enabled") : _("disabled"));
	fprintf(stdout,
		"         %-32s %s\n",
		_("WLAN:"),
		radio_states & MCE_RADIO_STATE_WLAN ? _("enabled") : _("disabled"));
	fprintf(stdout,
		"         %-32s %s\n",
		_("Bluetooth:"),
		radio_states & MCE_RADIO_STATE_BLUETOOTH ? _("enabled") : _("disabled"));
	fprintf(stdout,
		"         %-32s %s\n",
		_("NFC:"),
		radio_states & MCE_RADIO_STATE_NFC ? _("enabled") : _("disabled"));
	fprintf(stdout,
		"         %-32s %s\n",
		_("FM transmitter:"),
		radio_states & MCE_RADIO_STATE_FMTX ? _("enabled") : _("disabled"));

	/* Get the call state; just ignore if no reply */
	reply = mcetool_dbus_call_with_reply(MCE_CALL_STATE_GET, NULL);

	if (dbus_message_get_args(reply, &error,
				  DBUS_TYPE_STRING, &callstate,
				  DBUS_TYPE_STRING, &calltype,
				  DBUS_TYPE_INVALID) == FALSE) {
		fprintf(stderr,
			"Failed to get reply argument from %s: "
			"%s; exiting\n",
			MCE_CALL_STATE_GET, error.message);
		dbus_message_unref(reply);
		dbus_error_free(&error);
		reply = NULL;
	}

	fprintf(stdout,
		" %-40s %s (%s)\n",
		_("Call state (type):"),
		  (callstate == NULL) ? _("unknown") : callstate,
		  (calltype == NULL) ? _("unknown") : calltype);

	dbus_message_unref(reply);

	/* Get display state */
	status = mcetool_dbus_call_string(MCE_DISPLAY_STATUS_GET,
					  &display, FALSE);

	if (status != 0)
		goto EXIT;

	fprintf(stdout,
		" %-40s %s\n",
		_("Display state:"),
		display);

	/* Get color profile */
	status = mcetool_dbus_call_string(MCE_COLOR_PROFILE_GET,
					  &color_profile, FALSE);

	if (status != 0)
		goto EXIT;

	fprintf(stdout,
		" %-40s %s\n",
		_("Color profile:"),
		color_profile);

	g_free(color_profile);
	color_profile = NULL;

	/* Display brightness */
	retval = mcetool_gconf_get_int(MCE_GCONF_DISPLAY_BRIGHTNESS_PATH,
				       &brightness);

	if (retval == TRUE) {
		fprintf(stdout,
			" %-40s %d %s\n",
			_("Brightness:"),
			brightness,
			_("(1-5)"));
	} else {
		fprintf(stdout,
			" %-40s %s\n",
			_("Brightness:"),
			_("<unset>"));
	}

	/* Get CABC mode */
	status = mcetool_dbus_call_string(MCE_CABC_MODE_GET, &cabc, FALSE);

	if (status != 0)
		goto EXIT;

	fprintf(stdout,
		" %-40s %s\n",
		_("CABC mode:"),
		cabc);

	/* Get dim timeout */
	retval = mcetool_gconf_get_int(MCE_GCONF_DISPLAY_DIM_TIMEOUT_PATH,
				       &dim_timeout);

	if (retval == TRUE) {
		fprintf(stdout,
			" %-40s %d %s\n",
			_("Dim timeout:"),
			dim_timeout,
			_("seconds"));
	} else {
		fprintf(stdout,
			" %-40s %s\n",
			_("Dim timeout:"),
			_("<unset>"));
	}

	/* Get the adaptive dimming setting */
	retval = mcetool_gconf_get_bool(MCE_GCONF_DISPLAY_ADAPTIVE_DIMMING_PATH,
				        &adaptive_dimming_enabled);

	if (retval == TRUE) {
		fprintf(stdout,
			" %-40s %s\n",
			_("Adaptive dimming:"),
			adaptive_dimming_enabled ? _("enabled") :
						   _("disabled"));
	} else {
		fprintf(stdout,
			" %-40s %s\n",
			_("Adaptive dimming:"),
			_("<unset>"));
	}

	/* Get the adaptive dimming threshold */
	retval = mcetool_gconf_get_int(MCE_GCONF_DISPLAY_ADAPTIVE_DIM_THRESHOLD_PATH, &adaptive_dimming_threshold);

	if (retval == TRUE) {
		fprintf(stdout,
			" %-40s %d %s\n",
			_("Adaptive dimming threshold:"),
			adaptive_dimming_threshold,
			_("milliseconds"));
	} else {
		fprintf(stdout,
			" %-40s %s\n",
			_("Adaptive dimming threshold:"),
			_("<unset>"));
	}

	/* Get blank timeout */
	retval = mcetool_gconf_get_int(MCE_GCONF_DISPLAY_BLANK_TIMEOUT_PATH,
				       &blank_timeout);

	if (retval == TRUE) {
		fprintf(stdout,
			" %-40s %d %s\n",
			_("Blank timeout:"),
			blank_timeout,
			_("seconds"));
	} else {
		fprintf(stdout,
			" %-40s %s\n",
			_("Blank timeout:"),
			_("<unset>"));
	}

	/* Get blanking inhibit policy */
	(void)mcetool_gconf_get_int(MCE_GCONF_BLANKING_INHIBIT_MODE_PATH,
				    &blank_inhibit);

	switch (blank_inhibit) {
	case 0:
		inhibit_string = _("disabled");
		break;

	case 1:
		inhibit_string = _("stay on with charger");
		break;

	case 2:
		inhibit_string = _("stay dim with charger");
		break;

	case 3:
		inhibit_string = _("stay on");
		break;

	case 4:
		inhibit_string = _("stay dim");
		break;

	case -1:
		inhibit_string = _("<unset>");
		break;

	default:
		inhibit_string = _("<invalid>");
		break;
	}

	fprintf(stdout,
		" %-40s %s\n",
		_("Blank inhibit:"),
		inhibit_string);

	/* Get keyboard backlight state */
	status = mcetool_dbus_call_bool(MCE_KEY_BACKLIGHT_STATE_GET, NULL,
					&keyboard_backlight);

	if (status != 0)
		goto EXIT;

	fprintf(stdout,
		" %-40s %s\n",
		_("Keyboard backlight:"),
		keyboard_backlight ? _("enabled") : _("disabled"));

	/* Get inactivity status */
	status = mcetool_dbus_call_bool(MCE_INACTIVITY_STATUS_GET, NULL,
					&inactive);

	if (status != 0)
		goto EXIT;

	fprintf(stdout,
		" %-40s %s\n",
		_("Inactivity status:"),
		inactive ? _("inactive") : _("active"));

	/* Get the automatic power saving mode setting */
	retval = mcetool_gconf_get_bool(MCE_GCONF_PSM_PATH,
				        &power_saving_mode);

	/* Get PSM state */
	status = mcetool_dbus_call_bool(MCE_PSM_STATE_GET, NULL,
					&active_psm_state);

	if (status != 0)
		goto EXIT;

	fprintf(stdout,
		" %-40s %s (%s)\n",
		_("Power saving mode:"),
		retval ? (power_saving_mode ? _("enabled") : _("disabled")) : _("<unset>"),
		active_psm_state ? _("active") : _("inactive"));

	/* Get the forced power saving mode setting */
	retval = mcetool_gconf_get_bool(MCE_GCONF_FORCED_PSM_PATH,
				        &forced_psm);

	fprintf(stdout,
		" %-40s %s\n",
		_("Forced power saving mode:"),
		retval ? (forced_psm ? _("enabled") : _("disabled")) : _("<unset>"));

	/* Get PSM threshold */
	retval = mcetool_gconf_get_int(MCE_GCONF_PSM_THRESHOLD_PATH,
				       &psm_threshold);

	if (retval == TRUE) {
		fprintf(stdout,
			" %-40s %d%%\n",
			_("PSM threshold:"),
			psm_threshold);
	} else {
		fprintf(stdout,
			" %-40s %s\n",
			_("PSM threshold:"),
			_("<unset>"));
	}

	/* Get touchscreen/keypad lock mode */
	status = mcetool_dbus_call_string(MCE_TKLOCK_MODE_GET, &tklock, FALSE);

	if (status != 0)
		goto EXIT;

	fprintf(stdout,
		" %-40s %s\n",
		_("Touchscreen/Keypad lock:"),
		tklock);

	/* Get touchscreen/keypad autolock mode */
	retval = mcetool_gconf_get_bool(MCE_GCONF_TK_AUTOLOCK_ENABLED_PATH,
				        &tklock_autolock);

	fprintf(stdout,
		" %-40s %s\n",
		_("Touchscreen/Keypad autolock:"),
		retval ? (tklock_autolock ? _("enabled") : _("disabled")) : _("<unset>"));

	/* Get touchscreen/keypad double tap gesture policy */
	retval = mcetool_gconf_get_int(MCE_GCONF_TK_DOUBLE_TAP_GESTURE_PATH,
				       &doubletap_gesture_policy);

	if (retval == FALSE) {
		policy_string = UNSET_STRING;
	} else {
		switch (doubletap_gesture_policy) {
		case 0:
			policy_string = DISABLED_STRING;
			break;

		case 1:
			policy_string = SHOW_UNLOCK_SCREEN_STRING;
			break;

		case 2:
			policy_string = UNLOCK_STRING;
			break;

		default:
			policy_string = _(INVALID_STRING);
			break;
		}
	}

	fprintf(stdout,
		" %-40s %s\n",
		_("Double-tap gesture policy:"),
		policy_string);

EXIT:
	fprintf(stdout, "\n");

	return status;
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

	int status = 0;

	gint powerkeyevent = INVALID_EVENT;
	gint newinhibitmode = -1;
#if MCETOOL_USE_DEMOMODE_HACK
	gint demomode = -1;
#endif
	gint newpsm = -1;
	gint newforcedpsm = -1;
	gint newpsmthreshold = -1;
	gint newbrightness = -1;
#if MCETOOL_USE_DEMOMODE_HACK
	gchar *newdemostate = NULL;
#endif
	gchar *newcabcmode = NULL;
	gchar *newcallstate = NULL;
	gchar *newcalltype = NULL;
	gchar *newtklockmode = NULL;
	gchar *newcolorprofile = NULL;
	gchar *ledpattern = NULL;
	gint led_enable = -1;
	gboolean block = FALSE;
	gboolean ledpattern_activate = TRUE;
	gboolean get_mce_status = TRUE;
	gboolean force_mce_status = FALSE;
	gboolean send_prevent = FALSE;
	gboolean send_cancel_prevent = FALSE;
	gboolean send_unblank = FALSE;
	gboolean send_dim = FALSE;
	gboolean send_blank = FALSE;
	gboolean request_color_profile_ids = FALSE;
	dbus_uint32_t new_radio_states;
	dbus_uint32_t radio_states_mask;

	DBusBusType bus_type = DBUS_BUS_SYSTEM;

	const char optline[] =
		"B"  // --block,
		"P"  // --blank-prevent,
		"v"  // --cancel-blank-prevent,
		"U"  // --unblank-screen,
		"d"  // --dim-screen,
		"n"  // --blank-screen,
		"b:" // --set-display-brightness,
		"I:" // --set-inhibit-mode,
		"D:" // --set-demo-mode,
		"C:" // --set-cabc-mode,
		"a"  // --get-color-profile-ids,
		"A:" // --set-color-profile,
		"c:" // --set-call-state,
		"r:" // --enable-radio,
		"R:" // --disable-radio,
		"p:" // --set-power-saving-mode,
		"F:" // --set-forced-psm,
		"T:" // --set-psm-threshold,
		"k:" // --set-tklock-mode,
		"l"  // --enable-led,
		"L"  // --disable-led,
		"y:" // --activate-led-pattern,
		"Y:" // --deactivate-led-pattern,
		"e:" // --powerkey-event,
		"M:" // --modinfo,
		"N"  // --status,
		"S"  // --session,
		"h"  // --help,
		"V"  // --version,
		;

	struct option const options[] = {
                { "block",                  no_argument,       0, 'B' },
                { "blank-prevent",          no_argument,       0, 'P' },
                { "cancel-blank-prevent",   no_argument,       0, 'v' },
                { "unblank-screen",         no_argument,       0, 'U' },
                { "dim-screen",             no_argument,       0, 'd' },
                { "blank-screen",           no_argument,       0, 'n' },
                { "set-display-brightness", required_argument, 0, 'b' },
                { "set-inhibit-mode",       required_argument, 0, 'I' },
                { "set-demo-mode",          required_argument, 0, 'D' },
                { "set-cabc-mode",          required_argument, 0, 'C' },
                { "get-color-profile-ids",  no_argument,       0, 'a' },
                { "set-color-profile",      required_argument, 0, 'A' },
                { "set-call-state",         required_argument, 0, 'c' },
                { "enable-radio",           required_argument, 0, 'r' },
                { "disable-radio",          required_argument, 0, 'R' },
                { "set-power-saving-mode",  required_argument, 0, 'p' },
                { "set-forced-psm",         required_argument, 0, 'F' },
                { "set-psm-threshold",      required_argument, 0, 'T' },
                { "set-tklock-mode",        required_argument, 0, 'k' },
                { "enable-led",             no_argument,       0, 'l' },
                { "disable-led",            no_argument,       0, 'L' },
                { "activate-led-pattern",   required_argument, 0, 'y' },
                { "deactivate-led-pattern", required_argument, 0, 'Y' },
                { "powerkey-event",         required_argument, 0, 'e' },
                { "modinfo",                required_argument, 0, 'M' },
                { "status",                 no_argument,       0, 'N' },
                { "session",                no_argument,       0, 'S' },
                { "help",                   no_argument,       0, 'h' },
                { "version",                no_argument,       0, 'V' },
                { 0, 0, 0, 0 }
        };

	/* By default, don't change any radio state */
	new_radio_states = 0;
	radio_states_mask = 0;

	/* Initialise support for locales, and set the program-name */
	if (init_locales(PRG_NAME) != 0)
		goto EXIT;

	/* Parse the command-line options */
	while ((optc = getopt_long(argc, argv, optline,
				   options, &opt_index)) != -1) {
		switch (optc) {
		case 'B':
			block = TRUE;
			break;

		case 'P':
			send_prevent = TRUE;
			get_mce_status = FALSE;
			break;

		case 'v':
			send_cancel_prevent = TRUE;
			get_mce_status = FALSE;
			break;

		case 'U':
			send_unblank = TRUE;
			get_mce_status = FALSE;
			break;

		case 'd':
			send_dim = TRUE;
			get_mce_status = FALSE;
			break;

		case 'n':
			send_blank = TRUE;
			get_mce_status = FALSE;
			break;

		case 'r':
			if (!strcmp(optarg, RADIO_MASTER)) {
				new_radio_states |= MCE_RADIO_STATE_MASTER;
				radio_states_mask |= MCE_RADIO_STATE_MASTER;
			} else if (!strcmp(optarg, RADIO_CELLULAR)) {
				new_radio_states |= MCE_RADIO_STATE_CELLULAR;
				radio_states_mask |= MCE_RADIO_STATE_CELLULAR;
			} else if (!strcmp(optarg, RADIO_WLAN)) {
				new_radio_states |= MCE_RADIO_STATE_WLAN;
				radio_states_mask |= MCE_RADIO_STATE_WLAN;
			} else if (!strcmp(optarg, RADIO_BLUETOOTH)) {
				new_radio_states |= MCE_RADIO_STATE_BLUETOOTH;
				radio_states_mask |= MCE_RADIO_STATE_BLUETOOTH;
			} else if (!strcmp(optarg, RADIO_NFC)) {
				new_radio_states |= MCE_RADIO_STATE_NFC;
				radio_states_mask |= MCE_RADIO_STATE_NFC;
			} else if (!strcmp(optarg, RADIO_FMTX)) {
				new_radio_states |= MCE_RADIO_STATE_FMTX;
				radio_states_mask |= MCE_RADIO_STATE_FMTX;
			} else {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			break;

		case 'R':
			if (!strcmp(optarg, RADIO_MASTER)) {
				new_radio_states &= ~MCE_RADIO_STATE_MASTER;
				radio_states_mask |= MCE_RADIO_STATE_MASTER;
			} else if (!strcmp(optarg, RADIO_CELLULAR)) {
				new_radio_states &= ~MCE_RADIO_STATE_CELLULAR;
				radio_states_mask |= MCE_RADIO_STATE_CELLULAR;
			} else if (!strcmp(optarg, RADIO_WLAN)) {
				new_radio_states &= ~MCE_RADIO_STATE_WLAN;
				radio_states_mask |= MCE_RADIO_STATE_WLAN;
			} else if (!strcmp(optarg, RADIO_BLUETOOTH)) {
				new_radio_states &= ~MCE_RADIO_STATE_BLUETOOTH;
				radio_states_mask |= MCE_RADIO_STATE_BLUETOOTH;
			} else if (!strcmp(optarg, RADIO_NFC)) {
				new_radio_states &= ~MCE_RADIO_STATE_NFC;
				radio_states_mask |= MCE_RADIO_STATE_NFC;
			} else if (!strcmp(optarg, RADIO_FMTX)) {
				new_radio_states &= ~MCE_RADIO_STATE_FMTX;
				radio_states_mask |= MCE_RADIO_STATE_FMTX;
			} else {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			break;

		case 'p':
			if (!strcmp(optarg, ENABLED_STRING)) {
				newpsm = TRUE;
			} else if (!strcmp(optarg, DISABLED_STRING)) {
				newpsm = FALSE;
			} else {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			get_mce_status = FALSE;
			break;

		case 'F':
			if (!strcmp(optarg, ENABLED_STRING)) {
				newforcedpsm = TRUE;
			} else if (!strcmp(optarg, DISABLED_STRING)) {
				newforcedpsm = FALSE;
			} else {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			get_mce_status = FALSE;
			break;

		case 'T':
			{
				gint tmp;

				if (sscanf(optarg, "%d", &tmp) != 1) {
					usage();
					status = EINVAL;
					goto EXIT;
				}

				if ((tmp < 10) || (tmp > 50) ||
				    ((tmp % 10) != 0)) {
					usage();
					status = EINVAL;
					goto EXIT;
				}

				newpsmthreshold = tmp;
			}

			get_mce_status = FALSE;
			break;

		case 'b':
			{
				gint tmp;

				if (sscanf(optarg, "%d", &tmp) != 1) {
					usage();
					status = EINVAL;
					goto EXIT;
				}

				if ((tmp < 1) || (tmp > 5)) {
					usage();
					status = EINVAL;
					goto EXIT;
				}

				newbrightness = tmp;
			}

			get_mce_status = FALSE;
			break;

		case 'c':
			{
				char *s1 = strdup(optarg);
				char *s2 = strchr(s1, ':');

				if (s2 == NULL) {
					usage();
					status = EINVAL;
					goto EXIT;
				}

				*s2 = '\0';

				if (++s2 == '\0') {
					usage();
					status = EINVAL;
					goto EXIT;
				}

				newcallstate = s1;
				newcalltype = strdup(s2);
			}

			get_mce_status = FALSE;
			break;

		case 'I':
			if (!strcmp(optarg, BLANKING_INHIBIT_DISABLED)) {
				newinhibitmode = 0;
			} else if (!strcmp(optarg, BLANKING_INHIBIT_STAY_ON_WITH_CHARGER)) {
				newinhibitmode = 1;
			} else if (!strcmp(optarg, BLANKING_INHIBIT_STAY_DIM_WITH_CHARGER)) {
				newinhibitmode = 2;
			} else if (!strcmp(optarg, BLANKING_INHIBIT_STAY_ON)) {
				newinhibitmode = 3;
			} else if (!strcmp(optarg, BLANKING_INHIBIT_STAY_DIM)) {
				newinhibitmode = 4;
			} else {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			get_mce_status = FALSE;
			break;

		case 'D':
			if(!strcmp(optarg, "on")){
#if MCETOOL_USE_DEMOMODE_HACK
				newdemostate = strdup(optarg);
				demomode = 1;
#else
				// mcetool --unblank-screen --set-inhibit-mode=stay-on --set-tklock-mode=unlocked
				send_unblank   = TRUE;
				newinhibitmode = 3;
				newtklockmode  = strdup("unlocked");
#endif
			}
			else if(!strcmp(optarg, "off")) {
#if MCETOOL_USE_DEMOMODE_HACK
				newdemostate = strdup(optarg);
				demomode = 0;
#else
				// mcetool --unblank-screen --dim-screen --blank-screen
				//         --set-inhibit-mode=disabled --set-tklock-mode=locked
				send_unblank   = TRUE;
				send_dim       = TRUE;
				send_blank     = TRUE;
				newinhibitmode = 0;
				newtklockmode  = strdup("locked");
#endif
			}
			else {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			get_mce_status = FALSE;
			break;

		case 'C':
			newcabcmode = strdup(optarg);
			get_mce_status = FALSE;
			break;

		case 'k':
			newtklockmode = strdup(optarg);
			get_mce_status = FALSE;
			break;

		case 'a':
			request_color_profile_ids = TRUE;
			get_mce_status = FALSE;
			break;

		case 'A':
			newcolorprofile = strdup(optarg);
			get_mce_status = FALSE;
			break;

		case 'l':
			if (led_enable != -1) {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			led_enable = 1;
			get_mce_status = FALSE;
			break;

		case 'L':
			if (led_enable != -1) {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			led_enable = 0;
			get_mce_status = FALSE;
			break;

		case 'y':
			if (ledpattern != NULL) {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			ledpattern = strdup(optarg);
			ledpattern_activate = TRUE;
			get_mce_status = FALSE;
			break;

		case 'Y':
			if (ledpattern != NULL) {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			ledpattern = strdup(optarg);
			ledpattern_activate = FALSE;
			get_mce_status = FALSE;
			break;

		case 'e':
			if (!strcmp(optarg, LONG_EVENT_STR)) {
				powerkeyevent = LONG_EVENT;
			} else if (!strcmp(optarg, DOUBLE_EVENT_STR)) {
				powerkeyevent = DOUBLE_EVENT;
			} else if (!strcmp(optarg, SHORT_EVENT_STR)) {
				powerkeyevent = SHORT_EVENT;
			} else {
				usage();
				status = EINVAL;
				goto EXIT;
			}

			get_mce_status = FALSE;
			break;

		case 'N':
			force_mce_status = TRUE;
			break;

		case 'S':
			bus_type = DBUS_BUS_SESSION;
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

	/* Any non-flag arguments is an error */
	if ((argc - optind) > 0) {
		usage();
		status = EINVAL;
		goto EXIT;
	}

	/* Init GType */
	g_type_init();

	/* Initialise D-Bus */
	if ((status = mcetool_dbus_init(bus_type)) != 0)
		goto EXIT;

	/* Init GConf */
	if ((status = mcetool_gconf_init()) != 0)
		goto EXIT;

	if (send_prevent == TRUE) {
		if ((status = mcetool_dbus_call_string(MCE_PREVENT_BLANK_REQ,
						       NULL, TRUE)) != 0)
			goto EXIT;

		fprintf(stdout, "Blank prevent requested\n");
	}

	if (send_cancel_prevent == TRUE) {
		if ((status = mcetool_dbus_call_string(MCE_CANCEL_PREVENT_BLANK_REQ,
						       NULL, TRUE)) != 0)
			goto EXIT;

		fprintf(stdout, "Cancel blank prevent requested\n");
	}

	if (send_unblank == TRUE) {
		if ((status = mcetool_dbus_call_string(MCE_DISPLAY_ON_REQ,
						       NULL, TRUE)) != 0)
			goto EXIT;

		fprintf(stdout, "Display on requested\n");
	}

	if (send_dim == TRUE) {
		if ((status = mcetool_dbus_call_string(MCE_DISPLAY_DIM_REQ,
						       NULL, TRUE)) != 0)
			goto EXIT;

		fprintf(stdout, "Display dim requested\n");
	}

	if (send_blank == TRUE) {
		if ((status = mcetool_dbus_call_string(MCE_DISPLAY_OFF_REQ,
						       NULL, TRUE)) != 0)
			goto EXIT;

		fprintf(stdout, "Display off requested\n");
	}

	/* Change the display brightness */
	if (newbrightness != -1) {
		if (mcetool_gconf_set_int(MCE_GCONF_DISPLAY_BRIGHTNESS_PATH,
					  newbrightness) == FALSE)
			goto EXIT;
	}

	/* Change the tklock mode */
	if (newtklockmode != NULL) {
		set_tklock_mode(&newtklockmode);
	}

	if (newcolorprofile != NULL) {
		set_color_profile(&newcolorprofile);
	}

	if (request_color_profile_ids == TRUE) {
		get_color_profile_ids();
	}

	if (powerkeyevent != INVALID_EVENT) {
		trigger_powerkey_event(powerkeyevent);
	}

	if (led_enable != -1) {
		set_led_state(led_enable);
	}

	if (ledpattern != NULL) {
		set_led_pattern_state(ledpattern,
				      ledpattern_activate);
	}

	if (newinhibitmode != -1) {
		if (mcetool_gconf_set_int(MCE_GCONF_BLANKING_INHIBIT_MODE_PATH,
					  newinhibitmode) == FALSE)
			goto EXIT;
	}

#if MCETOOL_USE_DEMOMODE_HACK
	if (demomode != -1) {
		if (dbus_send(MCE_SERVICE, MCE_REQUEST_PATH,
			      MCE_REQUEST_IF, MCE_DBUS_DEMO_MODE_REQ, TRUE,
			      DBUS_TYPE_STRING, &newdemostate,
			      DBUS_TYPE_INVALID) == FALSE) {
				status = EXIT_FAILURE;
				goto EXIT;
		}
	}
#endif

	if (radio_states_mask != 0) {
		/* Change radio states */
		if (dbus_send(MCE_SERVICE, MCE_REQUEST_PATH,
			      MCE_REQUEST_IF, MCE_RADIO_STATES_CHANGE_REQ, TRUE,
			      DBUS_TYPE_UINT32, &new_radio_states,
			      DBUS_TYPE_UINT32, &radio_states_mask,
			      DBUS_TYPE_INVALID) == FALSE) {
			status = EXIT_FAILURE;
			goto EXIT;
		}
	}

	if (newpsm != -1) {
		if (mcetool_gconf_set_bool(MCE_GCONF_PSM_PATH,
					   newpsm) == FALSE)
			goto EXIT;
	}

	if (newforcedpsm != -1) {
		if (mcetool_gconf_set_bool(MCE_GCONF_FORCED_PSM_PATH,
					   newforcedpsm) == FALSE)
			goto EXIT;
	}

	if (newpsmthreshold != -1) {
		if (mcetool_gconf_set_int(MCE_GCONF_PSM_THRESHOLD_PATH,
					  newpsmthreshold) == FALSE)
			goto EXIT;
	}

	if (newcabcmode != NULL) {
		/* Change the cabc mode */
		if (dbus_send(MCE_SERVICE, MCE_REQUEST_PATH,
			      MCE_REQUEST_IF, MCE_CABC_MODE_REQ, TRUE,
			      DBUS_TYPE_STRING, &newcabcmode,
			      DBUS_TYPE_INVALID) == FALSE) {
			status = EXIT_FAILURE;
			goto EXIT;
		}
	}

	if ((newcallstate != NULL) && (newcalltype != NULL)) {
		/* Change the call state/type */
		if (dbus_send(MCE_SERVICE, MCE_REQUEST_PATH,
			      MCE_REQUEST_IF, MCE_CALL_STATE_CHANGE_REQ, TRUE,
			      DBUS_TYPE_STRING, &newcallstate,
			      DBUS_TYPE_STRING, &newcalltype,
			      DBUS_TYPE_INVALID) == FALSE) {
			status = EXIT_FAILURE;
			goto EXIT;
		}
	}

	if ((get_mce_status == TRUE) || (force_mce_status == TRUE))
		mcetool_get_status();

	while (block == TRUE)
		/* Do nothing */;

EXIT:
	g_free(newcolorprofile);

	mcetool_gconf_exit();
	mcetool_dbus_exit();

	return status;
}
