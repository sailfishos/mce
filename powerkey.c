/**
 * @file powerkey.c
 * Power key logic for the Mode Control Entity
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
#include <glib.h>

#include <stdlib.h>			/* exit(), EXIT_FAILURE */
#include <string.h>			/* strcmp() */
#include <linux/input.h>		/* struct input_event */

#include "mce.h"			/* mce_get_submode_int32(),
					 * mce_add_submode_int32(),
					 * mce_rem_submode_int32(),
					 * submode_pipe,
					 * system_state_pipe,
					 * tk_lock_pipe,
					 * keypress_pipe,
					 * system_state_t,
					 * submode_t
					 */
#include "powerkey.h"

#include "mce-log.h"			/* mce_log(), LL_* */
#include "mce-conf.h"			/* mce_conf_get_int(),
					 * mce_conf_get_string()
					 */
#include "mce-dbus.h"			/* mce_dbus_handler_add(),
					 * dbus_send(),
					 * dbus_send_message(),
					 * dbus_new_method_reply(),
					 * dbus_message_get_no_reply(),
					 * dbus_message_iter_init(),
					 * dbus_message_iter_get_arg_type(),
					 * dbus_message_iter_get_basic(),
					 * dbus_message_iter_next(),
					 * dbus_error_init(),
					 * dbus_error_free(),
					 * DBUS_MESSAGE_TYPE_METHOD_CALL,
					 * DBUS_TYPE_BOOLEAN,
					 * DBUS_TYPE_UINT32,
					 * DBUS_TYPE_INVALID,
					 * DBusMessage, DBusMessageIter,
					 * DBusError,
					 * dbus_bool_t,
					 * dbus_uint32_t
					 */
#include "mce-dsme.h"			/* request_normal_shutdown(),
					 * request_soft_poweron(),
					 * request_soft_poweroff(),
					 * request_powerup(),
					 * request_reboot()
					 */
#include "datapipe.h"			/* execute_datapipe(),
					 * execute_datapipipe_output_triggers(),
					 * datapipe_get_gint(),
					 * append_input_trigger_to_datapipe(),
					 * remove_input_trigger_from_datapipe()
					 */

#if 0 // DEBUG: make all logging from this module "critical"
# undef mce_log
# define mce_log(LEV, FMT, ARGS...) \
	mce_log_file(LL_CRIT, __FILE__, __FUNCTION__, FMT , ## ARGS)
#endif

/**
 * The ID of the timeout used when determining
 * whether the key press was short or long
 */
static guint powerkey_timeout_cb_id = 0;

/**
 * The ID of the timeout used when determining
 * whether the key press was a double press
 */
static guint doublepress_timeout_cb_id = 0;

/** Time in milliseconds before the key press is considered medium */
static gint mediumdelay = DEFAULT_POWER_MEDIUM_DELAY;
/** Time in milliseconds before the key press is considered long */
static gint longdelay = DEFAULT_POWER_LONG_DELAY;
/** Timeout in milliseconds during which key press is considered double */
static gint doublepressdelay = DEFAULT_POWER_DOUBLE_DELAY;
/** Action to perform on a short key press */
static poweraction_t shortpressaction = DEFAULT_POWERKEY_SHORT_ACTION;
/** Action to perform on a long key press */
static poweraction_t longpressaction = DEFAULT_POWERKEY_LONG_ACTION;
/** Action to perform on a double key press */
static poweraction_t doublepressaction = DEFAULT_POWERKEY_DOUBLE_ACTION;

/** D-Bus signal to send on short [power] press */
static gchar *shortpresssignal = NULL;
/** D-Bus signal to send on long [power] press */
static gchar *longpresssignal = NULL;
/** D-Bus signal to send on double [power] press */
static gchar *doublepresssignal = NULL;

static void cancel_powerkey_timeout(void);

/**
 * Generic logic for key presses
 *
 * @param action The action to take
 * @param dbus_signal A D-Bus signal to send
 */
static void generic_powerkey_handler(poweraction_t action,
				     gchar *dbus_signal)
{
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	submode_t submode = mce_get_submode_int32();

	/* Ignore keypress if the alarm UI is visible */
	if ((alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32) ||
	    (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32))
		goto EXIT;

	switch (action) {
	case POWER_DISABLED:
		break;

	case POWER_POWEROFF:
	default:
		/* Do not shutdown if the tklock is active
		 * or if we're in alarm state
		 */
		if ((submode & MCE_TKLOCK_SUBMODE) == 0) {
			mce_log(LL_WARN,
				"Requesting shutdown (action: %d)",
				action);

			request_normal_shutdown();
		}

		break;

	case POWER_SOFT_POWEROFF:
		/* Only soft poweroff if the tklock isn't active */
		if ((submode & MCE_TKLOCK_SUBMODE) == 0) {
			request_soft_poweroff();
		}

		break;

	case POWER_TKLOCK_LOCK:
		/* FIXME: This just happens to be the default place to
		 *        get hit when processing power key events.
		 *        The rest should also be adjusted... */

		switch( display_state_get() ) {
		case MCE_DISPLAY_ON:
		case MCE_DISPLAY_DIM:
		case MCE_DISPLAY_POWER_UP:
			mce_log(LL_DEBUG, "display -> off + lock");

			/* Do the locking before turning display off.
			 *
			 * The tklock requests get ignored in act dead
			 * etc, so we can just blindly request it.
			 */
			execute_datapipe(&tk_lock_pipe,
					 GINT_TO_POINTER(LOCK_ON),
					 USE_INDATA, CACHE_INDATA);

			execute_datapipe(&display_state_req_pipe,
					 GINT_TO_POINTER(MCE_DISPLAY_OFF),
					 USE_INDATA, CACHE_INDATA);
			break;

		default:
			mce_log(LL_DEBUG, "display -> on");
			execute_datapipe(&display_state_req_pipe,
					 GINT_TO_POINTER(MCE_DISPLAY_ON),
					 USE_INDATA, CACHE_INDATA);
			break;
		}
		break;

	case POWER_TKLOCK_UNLOCK:
		/* Request disabling of touchscreen/keypad lock
		 * if the tklock isn't already inactive
		 */
		if ((submode & MCE_TKLOCK_SUBMODE) != 0) {
			execute_datapipe(&tk_lock_pipe,
					 GINT_TO_POINTER(LOCK_OFF),
					 USE_INDATA, CACHE_INDATA);
		}

		break;

	case POWER_TKLOCK_BOTH:
		/* Request enabling of touchscreen/keypad lock
		 * if the tklock isn't active,
		 * and disabling if the tklock is active
		 */
		if ((submode & MCE_TKLOCK_SUBMODE) == 0) {
			execute_datapipe(&tk_lock_pipe,
					 GINT_TO_POINTER(LOCK_ON),
					 USE_INDATA, CACHE_INDATA);
		} else {
			execute_datapipe(&tk_lock_pipe,
					 GINT_TO_POINTER(LOCK_OFF),
					 USE_INDATA, CACHE_INDATA);
		}

		break;

	case POWER_DBUS_SIGNAL:
		/* Send a D-Bus signal */
		dbus_send(NULL, MCE_REQUEST_PATH,
			  MCE_REQUEST_IF, dbus_signal,
			  NULL,
			  DBUS_TYPE_INVALID);
	}

EXIT:
	return;
}

/**
 * Timeout callback for double key press
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean doublepress_timeout_cb(gpointer data)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);

	(void)data;

	doublepress_timeout_cb_id = 0;

	/* doublepress timer expired without any secondary press;
	 * thus this was a short press
	 */
	if (system_state == MCE_STATE_USER)
		generic_powerkey_handler(shortpressaction,
					 shortpresssignal);

	return FALSE;
}

/**
 * Cancel doublepress timeout
 */
static void cancel_doublepress_timeout(void)
{
	/* Remove the timeout source for the [power] double key press handler */
	if (doublepress_timeout_cb_id != 0) {
		g_source_remove(doublepress_timeout_cb_id);
		doublepress_timeout_cb_id = 0;
	}
}

/**
 * Setup doublepress timeout
 *
 * @return TRUE if the doublepress action was setup,
 *         FALSE if no action was setup
 */
static gboolean setup_doublepress_timeout(void)
{
	submode_t submode = mce_get_submode_int32();
	gboolean status = FALSE;

	/* Only setup the doublepress timeout when needed */
	if (doublepressaction == POWER_DISABLED)
		goto EXIT;

	cancel_doublepress_timeout();

	/* If the tklock is enabled, but doublepress to unlock is disabled,
	 * or if the tklock isn't enabled and short press to lock is enabled,
	 * exit
	 */
	if (doublepressaction != POWER_DBUS_SIGNAL) {
		if ((submode & MCE_TKLOCK_SUBMODE) != 0) {
			if ((doublepressaction != POWER_TKLOCK_UNLOCK) &&
			    (doublepressaction != POWER_TKLOCK_BOTH))
				goto EXIT;
		} else {
			if ((shortpressaction == POWER_TKLOCK_LOCK) ||
			    (shortpressaction == POWER_TKLOCK_BOTH))
				goto EXIT;
		}
	}

	/* Setup new timeout */
	doublepress_timeout_cb_id =
		g_timeout_add(doublepressdelay, doublepress_timeout_cb, NULL);
	status = TRUE;

EXIT:
	return status;
}

/**
 * Logic for short key press
 */
static void handle_shortpress(void)
{
	cancel_powerkey_timeout();

	if (doublepress_timeout_cb_id == 0) {
		if (setup_doublepress_timeout() == FALSE)
			generic_powerkey_handler(shortpressaction,
						 shortpresssignal);
	} else {
		cancel_doublepress_timeout();
		generic_powerkey_handler(doublepressaction,
					 doublepresssignal);
	}
}

/**
 * Logic for long key press
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean handle_longpress(void)
{
	system_state_t state = datapipe_get_gint(system_state_pipe);
	alarm_ui_state_t alarm_ui_state =
		datapipe_get_gint(alarm_ui_state_pipe);
	submode_t submode = mce_get_submode_int32();
	gboolean status = TRUE;

	/* Ignore keypress if the alarm UI is visible */
	if ((alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32) ||
	    (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32))
		goto EXIT;

	/* Ignore if we're already shutting down/rebooting */
	switch (state) {
	case MCE_STATE_SHUTDOWN:
	case MCE_STATE_REBOOT:
		status = FALSE;
		break;

	case MCE_STATE_ACTDEAD:
		/* activate power on led pattern and power up to user mode*/
		mce_log(LL_CRIT, "ENABLE MCE_LED_PATTERN_POWER_ON");
		execute_datapipe_output_triggers(&led_pattern_activate_pipe,
						 MCE_LED_PATTERN_POWER_ON,
						 USE_INDATA);
		request_powerup();
		break;

	case MCE_STATE_USER:
		/* If softoff is enabled, wake up
		 * Otherwise, perform long press action
		 */
		if ((submode & MCE_SOFTOFF_SUBMODE)) {
			request_soft_poweron();
		} else {
			generic_powerkey_handler(longpressaction,
						 longpresssignal);
		}

		break;

	default:
		/* If no special cases are needed,
		 * just do a regular shutdown
		 */
		mce_log(LL_WARN,
			"Requesting shutdown; state: %d",
			state);

		request_normal_shutdown();
		break;
	}

EXIT:
	return status;
}

/**
 * Timeout callback for long key press
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean powerkey_timeout_cb(gpointer data)
{
	(void)data;

	powerkey_timeout_cb_id = 0;

	handle_longpress();

	return FALSE;
}

/**
 * Cancel powerkey timeout
 */
static void cancel_powerkey_timeout(void)
{
	/* Remove the timeout source for the [power] long key press handler */
	if (powerkey_timeout_cb_id != 0) {
		g_source_remove(powerkey_timeout_cb_id);
		powerkey_timeout_cb_id = 0;
	}
}

/**
 * Setup powerkey timeout
 */
static void setup_powerkey_timeout(gint powerkeydelay)
{
	cancel_powerkey_timeout();

	/* Setup new timeout */
	powerkey_timeout_cb_id =
		g_timeout_add(powerkeydelay, powerkey_timeout_cb, NULL);
}

/**
 * D-Bus callback for powerkey event triggering
 *
 * @param msg D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean trigger_powerkey_event_req_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	DBusMessageIter iter;
	dbus_uint32_t uintval;
	dbus_bool_t boolval;
	gint argcount = 0;
	gint argtype;
	gboolean status = FALSE;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	mce_log(LL_DEBUG, "Received [power] button trigger request");

	if (dbus_message_iter_init(msg, &iter) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_ERR,
			"Failed to initialise D-Bus message iterator; "
			"message has no arguments");
		goto EXIT;
	}

	argtype = dbus_message_iter_get_arg_type(&iter);
	argcount++;

	switch (argtype) {
	case DBUS_TYPE_BOOLEAN:
		dbus_message_iter_get_basic(&iter, &boolval);
		uintval = (boolval == TRUE) ? 1 : 0;
		break;

	case DBUS_TYPE_UINT32:
		dbus_message_iter_get_basic(&iter, &uintval);

		if (uintval > 2) {
			mce_log(LL_ERR,
				"Incorrect powerkey event passed to %s.%s; "
				"ignoring request",
				MCE_REQUEST_IF, MCE_TRIGGER_POWERKEY_EVENT_REQ);
			goto EXIT;
		}

		break;

	default:
		mce_log(LL_ERR,
			"Argument %d passed to %s.%s has incorrect type",
			argcount,
			MCE_REQUEST_IF, MCE_TRIGGER_POWERKEY_EVENT_REQ);
		goto EXIT;
	}

	while (dbus_message_iter_next(&iter) == TRUE)
		argcount++;

	if (argcount > 1) {
		mce_log(LL_WARN,
			"Too many arguments passed to %s.%s; "
			"got %d, expected %d -- ignoring extra arguments",
			MCE_REQUEST_IF, MCE_TRIGGER_POWERKEY_EVENT_REQ,
			argcount, 1);
	}

	mce_log(LL_DEBUG, "[power] button event trigger value: %d", uintval);

	cancel_powerkey_timeout();
	cancel_doublepress_timeout();

	switch (uintval) {
	default:
	case 0:
		/* short press */
		generic_powerkey_handler(shortpressaction,
					 shortpresssignal);
		break;

	case 1:
		/* long press */
		handle_longpress();
		break;

	case 2:
		/* double press */
		generic_powerkey_handler(doublepressaction,
					 doublepresssignal);
		break;
	}

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

EXIT:
	return status;
}

/**
 * Datapipe trigger for the [power] key
 *
 * @param data A pointer to the input_event struct
 */
static void powerkey_trigger(gconstpointer const data)
{
        system_state_t system_state = datapipe_get_gint(system_state_pipe);
	submode_t submode = mce_get_submode_int32();
	struct input_event const *const *evp;
	struct input_event const *ev;

	/* Don't dereference until we know it's safe */
	if (data == NULL)
		goto EXIT;

	evp = data;
	ev = *evp;

	if ((ev != NULL) && (ev->code == KEY_POWER)) {
		/* If set, the [power] key was pressed */
		if (ev->value == 1) {
			mce_log(LL_DEBUG, "[power] pressed");

			/* Are we waiting for a doublepress? */
			if (doublepress_timeout_cb_id != 0) {
				handle_shortpress();
			} else if ((system_state == MCE_STATE_ACTDEAD) ||
			           ((submode & MCE_SOFTOFF_SUBMODE) != 0)) {
				/* Setup new timeout */

				/* Shorter delay for startup
				 * than for shutdown
				 */
				setup_powerkey_timeout(mediumdelay);
			} else {
				setup_powerkey_timeout(longdelay);
			}
		} else if (ev->value == 0) {
			mce_log(LL_DEBUG, "[power] released");

			/* Short key press */
			if (powerkey_timeout_cb_id != 0) {
				handle_shortpress();
			}
		}
	}

EXIT:
	return;
}

/**
 * Parse the [power] action string
 *
 * @todo Implement this using string to enum mappings instead,
 *       to allow for better debugging messages and a generic parser
 *
 * @param string The string to parse
 * @param dbus_signal A D-Bus signal to send
 * @param action A pointer to the variable to store the action in
 * @return TRUE if the string contained a valid action,
 *         FALSE if the action was invalid
 */
static gboolean parse_action(char *string,
			     char **dbus_signal,
			     poweraction_t *action)
{
	gboolean status = FALSE;

	if (!strcmp(string, POWER_DISABLED_STR)) {
		*action = POWER_DISABLED;
	} else if (!strcmp(string, POWER_MENU_STR)) {
		*action = POWER_MENU;
	} else if (!strcmp(string, POWER_POWEROFF_STR)) {
		*action = POWER_POWEROFF;
	} else if (!strcmp(string, POWER_SOFT_POWEROFF_STR)) {
		*action = POWER_SOFT_POWEROFF;
	} else if (!strcmp(string, POWER_TKLOCK_LOCK_STR)) {
		*action = POWER_TKLOCK_LOCK;
	} else if (!strcmp(string, POWER_TKLOCK_UNLOCK_STR)) {
		*action = POWER_TKLOCK_UNLOCK;
	} else if (!strcmp(string, POWER_TKLOCK_BOTH_STR)) {
		*action = POWER_TKLOCK_BOTH;
	} else if (!strncmp(string,
			    POWER_DBUS_SIGNAL_STR,
			    strlen(POWER_DBUS_SIGNAL_STR))) {
		gchar *tmp = string + strlen(POWER_DBUS_SIGNAL_STR);

		if (strlen(tmp) == 0) {
			mce_log(LL_ERR,
				"No signal name provided to action "
				"`dbus-signal-'; ignoring");
			goto EXIT;
		}

		*action = POWER_DBUS_SIGNAL;
		*dbus_signal = g_strdup(tmp);
	} else {
		mce_log(LL_WARN,
			"Unknown [power] action; "
			"using default");
		goto EXIT;
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * Init function for the powerkey component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_powerkey_init(void)
{
	gboolean status = FALSE;
	gchar *tmp = NULL;

	/* Append triggers/filters to datapipes */
	append_input_trigger_to_datapipe(&keypress_pipe,
					 powerkey_trigger);

	/* req_trigger_powerkey_event */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_TRIGGER_POWERKEY_EVENT_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 trigger_powerkey_event_req_dbus_cb) == NULL)
		goto EXIT;

	/* Get configuration options */
	longdelay = mce_conf_get_int(MCE_CONF_POWERKEY_GROUP,
				     MCE_CONF_POWERKEY_LONG_DELAY,
				     DEFAULT_POWER_LONG_DELAY);
	mediumdelay = mce_conf_get_int(MCE_CONF_POWERKEY_GROUP,
				       MCE_CONF_POWERKEY_MEDIUM_DELAY,
				       DEFAULT_POWER_MEDIUM_DELAY);
	tmp = mce_conf_get_string(MCE_CONF_POWERKEY_GROUP,
				  MCE_CONF_POWERKEY_SHORT_ACTION,
				  "");

	/* Since we've set a default, error handling is unnecessary */
	(void)parse_action(tmp, &shortpresssignal, &shortpressaction);
	g_free(tmp);

	tmp = mce_conf_get_string(MCE_CONF_POWERKEY_GROUP,
				  MCE_CONF_POWERKEY_LONG_ACTION,
				  "");

	/* Since we've set a default, error handling is unnecessary */
	(void)parse_action(tmp, &longpresssignal, &longpressaction);
	g_free(tmp);

	doublepressdelay = mce_conf_get_int(MCE_CONF_POWERKEY_GROUP,
					    MCE_CONF_POWERKEY_DOUBLE_DELAY,
					    DEFAULT_POWER_DOUBLE_DELAY);
	tmp = mce_conf_get_string(MCE_CONF_POWERKEY_GROUP,
				  MCE_CONF_POWERKEY_DOUBLE_ACTION,
				  "");

	/* Since we've set a default, error handling is unnecessary */
	(void)parse_action(tmp, &doublepresssignal, &doublepressaction);
	g_free(tmp);

	status = TRUE;

EXIT:
	return status;
}

/**
 * Exit function for the powerkey component
 *
 * @todo D-Bus unregistration
 */
void mce_powerkey_exit(void)
{
	/* Remove triggers/filters from datapipes */
	remove_input_trigger_from_datapipe(&keypress_pipe,
					   powerkey_trigger);

	/* Remove all timer sources */
	cancel_powerkey_timeout();
	cancel_doublepress_timeout();

	g_free(doublepresssignal);
	g_free(longpresssignal);
	g_free(shortpresssignal);

	return;
}
