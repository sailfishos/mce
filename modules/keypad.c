/**
 * @file keypad.c
 * Keypad module -- this handles the keypress logic for MCE
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * @author Matti Lehtimäki <matti.lehtimaki@gmail.com>
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

#include "keypad.h"

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-io.h"
#include "../mce-lib.h"
#include "../mce-hal.h"
#include "../mce-conf.h"
#include "../mce-dbus.h"

#include "led.h"

#include <mce/dbus-names.h>

#include <unistd.h>
#include <glib/gstdio.h>
#include <gmodule.h>

/** Module name */
#define MODULE_NAME		"keypad"

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
	/** Name of the module */
	.name = MODULE_NAME,
	/** Module provides */
	.provides = provides,
	/** Module priority */
	.priority = 100
};

/**
 * The ID of the timeout used for the key backlight
 */
static guint key_backlight_timeout_cb_id = 0;

/** Default backlight brightness */
static gint key_backlight_timeout = DEFAULT_KEY_BACKLIGHT_TIMEOUT;

/** Key backlight enabled/disabled */
static gboolean key_backlight_is_enabled = FALSE;

/** Maximum backlight brightness, hw specific */
static gint backlight_brightness_level_maximum = DEFAULT_KEY_BACKLIGHT_LEVEL;

/** File used to get maximum display brightness */
static gchar *backlight_brightness_level_maximum_path = NULL;

/** File used to set backlight brightness */
static output_state_t backlight_brightness_level_output =
{
	.path = NULL,
	.context = "brightness",
	.truncate_file = TRUE,
	.close_on_exit = FALSE,
};

static void cancel_key_backlight_timeout(void);

/** Check if sysfs directory contains brightness and max_brightness entries
 *
 * @param sysfs directory to probe
 * @param setpath place to store path to brightness file
 * @param maxpath place to store path to max_brightness file
 * @return TRUE if brightness and max_brightness files were found,
 *         FALSE otherwise
 */
static gboolean probe_simple_backlight_directory(const gchar *dirpath,
					char **setpath, char **maxpath)
{
	gboolean  res = FALSE;

	gchar *set = g_strdup_printf("%s/brightness", dirpath);
	gchar *max = g_strdup_printf("%s/max_brightness", dirpath);

	if( set && max && !g_access(set, W_OK) && !g_access(max, R_OK) ) {
		*setpath = set, set = 0;
		*maxpath = max, max = 0;
		res = TRUE;
	}

	g_free(set);
	g_free(max);

	return res;
}

/**
 * Check if user-defined keyboard backlight exists
 */
static void probe_simple_backlight_brightness(void)
{
	gchar *set = 0;
	gchar *max = 0;
	gchar **vdir = 0;

	/* Check if we have a configured brightness directory
	 * that a) exists and b) contains both brightness and
	 * max_brightness files */

	vdir = mce_conf_get_string_list(MCE_CONF_KEYPAD_GROUP,
					MCE_CONF_KEY_BACKLIGHT_SYS_PATH, 0);
	if( vdir ) {
		for( size_t i = 0; vdir[i]; ++i ) {
			if( !*vdir[i] || g_access(vdir[i], F_OK) )
				continue;

			if( probe_simple_backlight_directory(vdir[i], &set, &max) )
				break;
		}
	}

	if( set && max ) {
		gulong tmp = 0;
		backlight_brightness_level_output.path  = set, set = 0;
		backlight_brightness_level_maximum_path = max, max = 0;
		if( mce_read_number_string_from_file(backlight_brightness_level_maximum_path,
							&tmp, NULL, FALSE, TRUE) ) {
			backlight_brightness_level_maximum = (gint)tmp;
		}
	}

	g_free(max);
	g_free(set);
	g_strfreev(vdir);
}

/**
 * Setup model specific key backlight values/paths
 */
static void setup_key_backlight(void)
{
	switch (get_product_id()) {
	default:
		/* Check for user-defined simple keyboard backlight */
		probe_simple_backlight_brightness();
		break;
	}
}
/**
 * Key backlight brightness for simple backlight
 *
 * @param brightness Backlight brightness
 */
static void set_simple_backlight_brightness(guint brightness)
{
	mce_write_number_string_to_file(&backlight_brightness_level_output, brightness);
}

/**
 * Set key backlight brightness
 *
 * @param data Backlight brightness passed as a gconstpointer
 */
static void set_key_backlight_brightness(gconstpointer data)
{
	static gint cached_brightness = -1;
	gint new_brightness = GPOINTER_TO_INT(data);

	/* If we're just rehashing the same brightness value, don't bother */
	if ((new_brightness == cached_brightness) || (new_brightness == -1))
		goto EXIT;

	cached_brightness = new_brightness;

	key_backlight_is_enabled = (new_brightness != 0);

	/* Product specific key backlight handling */
	switch (get_product_id()) {
	default:
		if (backlight_brightness_level_output.path) {
			set_simple_backlight_brightness(new_brightness);
		}
		break;
	}

EXIT:
	return;
}

/**
 * Disable key backlight
 */
static void disable_key_backlight(void)
{
	cancel_key_backlight_timeout();

	datapipe_exec_full(&key_backlight_brightness_pipe, GINT_TO_POINTER(0));
}

/**
 * Timeout callback for key backlight
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean key_backlight_timeout_cb(gpointer data)
{
	(void)data;

	key_backlight_timeout_cb_id = 0;

	disable_key_backlight();

	return FALSE;
}

/**
 * Cancel key backlight timeout
 */
static void cancel_key_backlight_timeout(void)
{
	if (key_backlight_timeout_cb_id != 0) {
		g_source_remove(key_backlight_timeout_cb_id);
		key_backlight_timeout_cb_id = 0;
	}
}

/**
 * Setup key backlight timeout
 */
static void setup_key_backlight_timeout(void)
{
	cancel_key_backlight_timeout();

	/* Setup a new timeout */
	key_backlight_timeout_cb_id =
		g_timeout_add_seconds(key_backlight_timeout,
				      key_backlight_timeout_cb, NULL);
}

/**
 * Enable key backlight
 */
static void enable_key_backlight(void)
{
	cancel_key_backlight_timeout();

	/* Only enable the key backlight if the slide is open */
	if (datapipe_get_gint(keyboard_slide_state_pipe) != COVER_OPEN)
		goto EXIT;

	setup_key_backlight_timeout();

	/* If the backlight is off, turn it on */
	if (datapipe_get_guint(key_backlight_brightness_pipe) == 0) {
		datapipe_exec_full(&key_backlight_brightness_pipe,
				   GINT_TO_POINTER(backlight_brightness_level_maximum));
	}

EXIT:
	return;
}

/**
 * Policy based enabling of key backlight
 */
static void enable_key_backlight_policy(void)
{
	cover_state_t kbd_slide_state = datapipe_get_gint(keyboard_slide_state_pipe);
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	alarm_ui_state_t alarm_ui_state =
		datapipe_get_gint(alarm_ui_state_pipe);

	/* If the keyboard slide isn't open, there's no point in enabling
	 * the backlight
	 *
	 * XXX: this policy will have to change if/when we get devices
	 * with external keypads that needs to be backlit, but for now
	 * that's not an issue
	 */
	if (kbd_slide_state != COVER_OPEN)
		goto EXIT;

	/* Only enable the key backlight in USER state
	 * and when the alarm dialog is visible
	 */
	if ((system_state == MCE_SYSTEM_STATE_USER) ||
	    ((alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32) ||
	     (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32))) {
		/* If there's a key backlight timeout active, restart it,
		 * else enable the backlight
		 */
		if (key_backlight_timeout_cb_id != 0)
			setup_key_backlight_timeout();
		else
			enable_key_backlight();
	}

EXIT:
	return;
}

/**
 * Send a key backlight state reply
 *
 * @param method_call A DBusMessage to reply to
 * @return TRUE on success, FALSE on failure
 */
static gboolean send_key_backlight_state(DBusMessage *const method_call)
{
	DBusMessage *msg = NULL;
	dbus_bool_t state = key_backlight_is_enabled;
	gboolean status = FALSE;

	mce_log(LL_DEBUG,
		"Sending key backlight state: %d",
		state);

	msg = dbus_new_method_reply(method_call);

	/* Append the display status */
	if (dbus_message_append_args(msg,
				     DBUS_TYPE_BOOLEAN, &state,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append reply argument to D-Bus message "
			"for %s.%s",
			MCE_REQUEST_IF, MCE_KEY_BACKLIGHT_STATE_GET);
		dbus_message_unref(msg);
		goto EXIT;
	}

	/* Send the message */
	status = dbus_send_message(msg);

EXIT:
	return status;
}

/**
 * D-Bus callback for the get key backlight state method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean key_backlight_state_get_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	mce_log(LL_DEVEL, "Received key backlight state get request from %s",
		mce_dbus_get_message_sender_ident(msg));

	/* Try to send a reply that contains the current key backlight state */
	if (send_key_backlight_state(msg) == FALSE)
		goto EXIT;

	status = TRUE;

EXIT:
	return status;
}

/**
 * Datapipe trigger for device inactivity
 *
 * @param data The inactivity stored in a pointer;
 *             TRUE if the device is inactive,
 *             FALSE if the device is active
 */
static void device_inactive_trigger(gconstpointer const data)
{
	gboolean device_inactive = GPOINTER_TO_INT(data);

	if (device_inactive == FALSE)
		enable_key_backlight_policy();
}

/**
 * Datapipe trigger for the keyboard slide
 *
 * @param data The keyboard slide state stored in a pointer;
 *             COVER_OPEN if the keyboard is open,
 *             COVER_CLOSED if the keyboard is closed
 */
static void keyboard_slide_state_trigger(gconstpointer const data)
{
	if ((GPOINTER_TO_INT(data) == COVER_OPEN) &&
	    ((mce_get_submode_int32() & MCE_SUBMODE_TKLOCK) == 0)) {
		enable_key_backlight_policy();
	} else {
		disable_key_backlight();
	}
}

/**
 * Datapipe trigger for display state
 *
 * @param data The display stated stored in a pointer
 */
static void display_state_curr_trigger(gconstpointer data)
{
	static display_state_t old_display_state = MCE_DISPLAY_UNDEF;
	display_state_t display_state_curr = GPOINTER_TO_INT(data);

	if (old_display_state == display_state_curr)
		goto EXIT;

	/* Disable the key backlight if the display dims */
	switch (display_state_curr) {
	case MCE_DISPLAY_OFF:
	case MCE_DISPLAY_LPM_OFF:
	case MCE_DISPLAY_LPM_ON:
	case MCE_DISPLAY_DIM:
	case MCE_DISPLAY_POWER_UP:
	case MCE_DISPLAY_POWER_DOWN:
		disable_key_backlight();
		break;

	case MCE_DISPLAY_ON:
		if (old_display_state != MCE_DISPLAY_ON)
			enable_key_backlight_policy();

		break;

	case MCE_DISPLAY_UNDEF:
	default:
		break;
	}

	old_display_state = display_state_curr;

EXIT:
	return;
}

/**
 * Handle system state change
 *
 * @param data The system state stored in a pointer
 */
static void system_state_trigger(gconstpointer data)
{
	system_state_t system_state = GPOINTER_TO_INT(data);

	/* If we're changing to another state than USER,
	 * disable the key backlight
	 */
	if (system_state != MCE_SYSTEM_STATE_USER)
		disable_key_backlight();
}

/** Array of dbus message handlers */
static mce_dbus_handler_t keypad_dbus_handlers[] =
{
	/* method calls */
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_KEY_BACKLIGHT_STATE_GET,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = key_backlight_state_get_dbus_cb,
		.args      =
			"    <arg direction=\"out\" name=\"backlight_state\" type=\"b\"/>\n"
	},
	/* sentinel */
	{
		.interface = 0
	}
};

/** Add dbus handlers
 */
static void mce_keypad_init_dbus(void)
{
	mce_dbus_handler_register_array(keypad_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mce_keypad_quit_dbus(void)
{
	mce_dbus_handler_unregister_array(keypad_dbus_handlers);
}

/** Array of datapipe handlers */
static datapipe_handler_t mce_keypad_datapipe_handlers[] =
{
	// output triggers
	{
		.datapipe  = &system_state_pipe,
		.output_cb = system_state_trigger,
	},
	{
		.datapipe  = &key_backlight_brightness_pipe,
		.output_cb = set_key_backlight_brightness,
	},
	{
		.datapipe  = &device_inactive_pipe,
		.output_cb = device_inactive_trigger,
	},
	{
		.datapipe  = &keyboard_slide_state_pipe,
		.output_cb = keyboard_slide_state_trigger,
	},
	{
		.datapipe  = &display_state_curr_pipe,
		.output_cb = display_state_curr_trigger,
	},
	// sentinel
	{
		.datapipe = 0,
	}
};

static datapipe_bindings_t mce_keypad_datapipe_bindings =
{
	.module   = "mce_keypad",
	.handlers = mce_keypad_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void mce_keypad_datapipe_init(void)
{
	mce_datapipe_init_bindings(&mce_keypad_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void mce_keypad_datapipe_quit(void)
{
	mce_datapipe_quit_bindings(&mce_keypad_datapipe_bindings);
}

/**
 * Init function for the keypad module
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
	gchar *status = NULL;

	(void)module;

	/* Append triggers/filters to datapipes */
	mce_keypad_datapipe_init();

	/* Get configuration options */
	key_backlight_timeout =
		mce_conf_get_int(MCE_CONF_KEYPAD_GROUP,
				 MCE_CONF_KEY_BACKLIGHT_TIMEOUT,
				 DEFAULT_KEY_BACKLIGHT_TIMEOUT);

	/* Add dbus handlers */
	mce_keypad_init_dbus();

	setup_key_backlight();

	return status;
}

/**
 * Exit function for the keypad module
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	/* Remove dbus handlers */
	mce_keypad_quit_dbus();

	g_free((void*)backlight_brightness_level_output.path);
	g_free(backlight_brightness_level_maximum_path);

	/* Remove triggers/filters from datapipes */
	mce_keypad_datapipe_quit();

	/* Remove all timer sources */
	cancel_key_backlight_timeout();

	return;
}
