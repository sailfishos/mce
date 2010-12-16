/**
 * @file mcetool.c
 * Tool to test and remote control the Mode Control Entity
 * <p>
 * Copyright © 2005-2010 Nokia Corporation and/or its subsidiary(-ies).
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
/** Disabled string; used for arg-parsing */
#define DISABLED_STRING				"disabled"

/** Master string; used for arg-parsing */
#define RADIO_MASTER				"master"
/** Cellular string; used for arg-parsing */
#define RADIO_CELLULAR				"cellular"
/** WLAN string; used for arg-parsing */
#define RADIO_WLAN				"wlan"
/** Bluetooth string; used for arg-parsing */
#define RADIO_BLUETOOTH				"bluetooth"

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

static DBusConnection *dbus_connection;	/**< D-Bus connection */

static GConfClient *gconf_client = NULL;	/**< GConf client */

/**
 * Display usage information
 */
static void usage(void)
{
	fprintf(stdout,
		_("Usage: %s [OPTION]\n"
		  "Mode Control Entity tool\n"
		  "\n"
		  "      --blank-prevent             send blank prevent "
		  "request to MCE\n"
		  "      --cancel-blank-prevent      send cancel blank prevent "
		  "request to MCE\n"
		  "      --unblank-screen            send unblank request to "
		  "MCE\n"
		  "      --dim-screen                send dim request "
		  "to MCE\n"
		  "      --blank-screen              send blank request "
		  "to MCE\n"
		  "      --set-display-brightness=BRIGHTNESS\n"
		  "                                  set the display "
		  "brightness to BRIGHTNESS;\n"
		  "                                    valid values are: "
		  "1-5\n"
		  "      --set-inhibit-mode=MODE\n"
		  "                                  set the blanking inhibit "
		  "mode to MODE;\n"
		  "                                    valid modes "
		  "are:\n"
		  "                                    ``disabled'',\n"
		  "                                    ``stay-on-with-charger"
		  "'', ``stay-on'',\n"
		  "                                    ``stay-dim-with-charger"
		  "'', ``stay-dim''\n"
		  "      --set-cabc-mode=MODE\n"
		  "                                  set the CABC mode to "
		  "MODE;\n"
		  "                                    valid modes "
		  "are:\n"
		  "                                    ``off'', "
		  "``ui'',\n"
		  "                                    ``still-image' and "
		  "``moving-image''\n"
		  "      --set-call-state=STATE:TYPE\n"
		  "                                  set the call state to "
		  "STATE and the call type\n"
		  "                                    to TYPE; valid states "
		  "are:\n"
		  "                                    ``none'', "
		  "``ringing'',\n"
		  "                                    ``active'' and "
		  "``service''\n"
		  "                                    valid types are:\n"
		  "                                    ``normal'' and "
		  "``emergency''\n"
		  "      --enable-radio=RADIO\n"
		  "                                  enable the specified "
		  "radio; valid radios are:\n"
		  "                                    ``master'', "
		  "``cellular'',\n"
		  "                                    ``wlan'' and "
		  "``bluetooth'';\n"
		  "                                    ``master'' affects "
		  "all radios\n"
		  "      --disable-radio=RADIO\n"
		  "                                  disable the specified "
		  "radio; valid radios are:\n"
		  "                                    ``master'', "
		  "``cellular'',\n"
		  "                                    ``wlan'' and "
		  "``bluetooth'';\n"
		  "                                    ``master'' affects "
		  "all radios\n"
		  "      --set-power-saving-mode=MODE\n"
		  "                                  set the power saving "
		  "mode; valid modes are:\n"
		  "                                    ``enabled'' and "
		  "``disabled''\n"
		  "      --set-forced-psm=MODE\n"
		  "                                  the forced "
		  "power saving mode to MODE;\n"
		  "                                    valid modes are:\n"
		  "                                    ``enabled'' and "
		  "``disabled''\n"
		  "      --set-psm-threshold=VALUE\n"
		  "                                  set the threshold for "
		  "the power saving mode;\n"
		  "                                    valid values are:\n"
		  "                                    10, 20, 30, 40, 50\n"
		  "      --set-tklock-mode=MODE\n"
		  "                                  set the touchscreen/"
		  "keypad lock mode;\n"
		  "                                    valid modes are:\n"
		  "                                    ``locked'', "
		  "``locked-dim'',\n"
		  "                                    ``silent-locked'', "
		  "``silent-locked-dim'',\n"
		  "                                    ``unlocked'' and "
		  "``silent-unlocked''\n"
		  "      --enable-led                enable LED framework\n"
		  "      --disable-led               disable LED framework\n"
		  "      --activate-led-pattern=PATTERN\n"
		  "                                  activate a LED pattern\n"
		  "      --deactivate-led-pattern=PATTERN\n"
		  "                                  deactivate a LED "
		  "pattern\n"
		  "      --powerkey-event=TYPE       trigger a powerkey "
		  "event; valid types are:\n"
		  "                                    ``short'', ``double'' "
		  "and ``long''\n"
		  "      --status                    output MCE status\n"
		  "      --block                     block after executing "
		  "commands\n"
		  "  -S, --session                   use the session bus "
		  "instead of the system bus\n"
		  "                                    for D-Bus\n"
		  "      --help                      display this help and "
		  "exit\n"
		  "      --version                   output version "
		  "information and exit\n"
		  "\n"
		  "If no option is specified, the status is output.\n"
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
		  "Copyright (C) 2005-2010 Nokia Corporation.  "
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
		fprintf(stderr, "No memory for new signal!");
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
			"Cannot allocate memory for D-Bus method call!");
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
			"Cannot allocate memory for D-Bus method call!");
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
				"for %s",
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
			"Could not call method %s: %s; exiting",
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
			"Cannot allocate memory for D-Bus method call!");
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
				"for %s",
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
			"Could not call method %s: %s; exiting",
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
				"%s; exiting",
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
			"Cannot allocate memory for D-Bus method call!");
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
				"for %s",
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
			"Could not call method %s: %s; exiting",
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
				"%s; exiting",
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
			"Cannot allocate memory for D-Bus method call!");
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
				"for %s",
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
			"Could not call method %s: %s; exiting",
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
				"%s; exiting",
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
				"Failed to append arguments to D-Bus message");
			dbus_message_unref(msg);
			goto EXIT;
		}
	}

	/* Send the signal / call the method */
	if (dbus_send_message(msg) == FALSE) {
		if (service != NULL)
			fprintf(stderr, "Cannot call method %s", name);
		else
			fprintf(stderr, "Cannot send signal %s", name);

		goto EXIT;
	}

	status = TRUE;

EXIT:
	va_end(var_args);

	return status;
}

/**
 * Acquire D-Bus services
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbus_acquire_services(void)
{
	gboolean status = FALSE;
	int ret;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	ret = dbus_bus_request_name(dbus_connection, MCETOOL_SERVICE, 0,
				    &error);

	if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fprintf(stderr,
			"Cannot acquire service: %s",
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	status = TRUE;

EXIT:
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
			"Failed to open connection to message bus; %s",
			error.message);
		dbus_error_free(&error);
		status = EXIT_FAILURE;
		goto EXIT;
	}

	/* Acquire D-Bus service */
	if (dbus_acquire_services() == FALSE) {
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
 *             "locked", "locked-dim", "silent-locked", "silent-locked-dim",
 *             "unlocked", "silent-unlocked"
 * @return TRUE on success, FALSE on FAILURE
 */
static gboolean set_tklock_mode(gchar **mode)
{
	/* com.nokia.mce.request.req_tklock_mode_change */
	return mcetool_dbus_call_string(MCE_TKLOCK_MODE_CHANGE_REQ, mode, TRUE);
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

/**
 * Init function for the mcetool GConf handling
 *
 * @return TRUE on success, FALSE on failure
 */
static gint mcetool_gconf_init(void)
{
	gint status = 0;

	/* Init GType */
	g_type_init();

	gconf_client = gconf_client_get_default();

	if (gconf_client == NULL) {
		fprintf(stderr, "Could not get default GConf client");
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
	const gchar *inhibit_string = NULL;
	gchar *mce_version = NULL;
	gchar *callstate = NULL;
	gchar *calltype = NULL;
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

	/* Get the call state; just ignore if no reply */
	reply = mcetool_dbus_call_with_reply(MCE_CALL_STATE_GET, NULL);

	if (dbus_message_get_args(reply, &error,
				  DBUS_TYPE_STRING, &callstate,
				  DBUS_TYPE_STRING, &calltype,
				  DBUS_TYPE_INVALID) == FALSE) {
		fprintf(stderr,
			"Failed to get reply argument from %s: "
			"%s; exiting",
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
	gint newpsm = -1;
	gint newforcedpsm = -1;
	gint newpsmthreshold = -1;
	gint newbrightness = -1;
	gchar *newcabcmode = NULL;
	gchar *newcallstate = NULL;
	gchar *newcalltype = NULL;
	gchar *newtklockmode = NULL;
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
	dbus_uint32_t new_radio_states;
	dbus_uint32_t radio_states_mask;

	DBusBusType bus_type = DBUS_BUS_SYSTEM;

	const char optline[] = "S";

	struct option const options[] = {
		{ "block", no_argument, 0, 'B' },
		{ "blank-prevent", no_argument, 0, 'P' },
		{ "cancel-blank-prevent", no_argument, 0, 'v' },
		{ "unblank-screen", no_argument, 0, 'U' },
		{ "dim-screen", no_argument, 0, 'd' },
		{ "blank-screen", no_argument, 0, 'n' },
		{ "set-display-brightness", required_argument, 0, 'b' },
		{ "set-inhibit-mode", required_argument, 0, 'I' },
		{ "set-cabc-mode", required_argument, 0, 'C' },
		{ "set-call-state", required_argument, 0, 'c' },
		{ "enable-radio", required_argument, 0, 'r' },
		{ "disable-radio", required_argument, 0, 'R' },
		{ "set-power-saving-mode", required_argument, 0, 'p' },
		{ "set-forced-psm", required_argument, 0, 'F' },
		{ "set-psm-threshold", required_argument, 0, 'T' },
		{ "set-tklock-mode", required_argument, 0, 'k' },
		{ "enable-led", no_argument, 0, 'l' },
		{ "disable-led", no_argument, 0, 'L' },
		{ "activate-led-pattern", required_argument, 0, 'y' },
		{ "deactivate-led-pattern", required_argument, 0, 'Y' },
		{ "powerkey-event", required_argument, 0, 'e' },
		{ "modinfo", required_argument, 0, 'M' },
		{ "status", no_argument, 0, 'N' },
		{ "session", no_argument, 0, 'S' },
		{ "help", no_argument, 0, 'h' },
		{ "version", no_argument, 0, 'V' },
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

		case 'C':
			newcabcmode = strdup(optarg);
			get_mce_status = FALSE;
			break;

		case 'k':
			newtklockmode = strdup(optarg);
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
	mcetool_gconf_exit();
	mcetool_dbus_exit();

	return status;
}
