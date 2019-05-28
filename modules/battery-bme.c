/**
 * @file battery-bme.c
 * Battery module -- this implements battery and charger logic for MCE
 * <p>
 * Copyright © 2008-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2013-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
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

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-dbus.h"
#include "../bme-dbus-names.h"

#include <gmodule.h>

/** Module name */
#define MODULE_NAME		"battery"

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

/** Cached value of the charger connected state */
static gint cached_charger_connected = -1;

/**
 * D-Bus callback for the battery full signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean battery_full_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	(void)msg;

	mce_log(LL_DEBUG,
		"Received battery full signal");

	datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_BATTERY_CHARGING);
	datapipe_exec_full(&led_pattern_activate_pipe, MCE_LED_PATTERN_BATTERY_FULL);

	datapipe_exec_full(&battery_status_pipe,
			   GINT_TO_POINTER(BATTERY_STATUS_FULL));

	status = TRUE;

//EXIT:
	return status;
}

/**
 * D-Bus callback for the battery ok signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean battery_ok_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	(void)msg;

	mce_log(LL_DEBUG,
		"Received battery ok signal");

//	datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_BATTERY_LOW);

	datapipe_exec_full(&battery_status_pipe,
			   GINT_TO_POINTER(BATTERY_STATUS_OK));

	status = TRUE;

//EXIT:
	return status;
}

/**
 * D-Bus callback for the battery low signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean battery_low_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	(void)msg;

	mce_log(LL_DEBUG,
		"Received battery low signal");

//	datapipe_exec_full(&led_pattern_activate_pipe, MCE_LED_PATTERN_BATTERY_LOW);

	datapipe_exec_full(&battery_status_pipe,
			   GINT_TO_POINTER(BATTERY_STATUS_LOW));

	status = TRUE;

//EXIT:
	return status;
}

/**
 * D-Bus callback for the battery empty signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean battery_empty_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	(void)msg;

	mce_log(LL_DEBUG,
		"Received battery empty signal");

	datapipe_exec_full(&battery_status_pipe,
			   GINT_TO_POINTER(BATTERY_STATUS_EMPTY));

	status = TRUE;

//EXIT:
	return status;
}

/**
 * D-Bus callback for the battery state changed signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean battery_state_changed_dbus_cb(DBusMessage *const msg)
{
	dbus_uint32_t percentage;
	DBusMessageIter iter;
	dbus_uint32_t now;
	dbus_uint32_t max;
	gint argcount = 0;
	gint argtype;
	gboolean status = FALSE;

	mce_log(LL_DEBUG,
		"Received battery state changed signal");

	if (dbus_message_iter_init(msg, &iter) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_ERR,
			"Failed to initialise D-Bus message iterator; "
			"message has no arguments");
		goto EXIT;
	}

	while ((argtype = dbus_message_iter_get_arg_type(&iter)) != DBUS_TYPE_INVALID) {
		if (argtype == DBUS_TYPE_UINT32) {
			if (argcount == 0) {
				dbus_message_iter_get_basic(&iter, &now);
			} else if (argcount == 1) {
				dbus_message_iter_get_basic(&iter, &max);
			} else if (argcount == 2) {
				dbus_message_iter_get_basic(&iter, &percentage);
			}
		} else if (argcount < 3) {
			mce_log(LL_ERR,
				"Argument %d passed to %s.%s has "
				"incorrect type",
				argcount,
				BME_SIGNAL_IF,
				BME_BATTERY_STATE_UPDATE);
			goto EXIT;
		}

		argcount++;
		dbus_message_iter_next (&iter);
	}

	if (argcount < 2) {
		mce_log(LL_ERR,
			"Too few arguments received from "
			"%s.%s; "
			"got %d, expected %d-%d",
			BME_SIGNAL_IF, BME_BATTERY_STATE_UPDATE,
			argcount, 2, 3);
		goto EXIT;
	}

	if (argcount > 3) {
		mce_log(LL_INFO,
			"Too many arguments received from "
			"%s.%s; "
			"got %d, expected %d-%d -- ignoring extra arguments",
			BME_SIGNAL_IF, BME_BATTERY_STATE_UPDATE,
			argcount, 2, 3);
	}

	if (argcount == 2) {
		percentage = (now * 10 / max) * 10;
	}

	mce_log(LL_DEBUG,
		"Percentage: %d",
		percentage);

	datapipe_exec_full(&battery_level_pipe,
			   GINT_TO_POINTER(percentage));

	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus callback for the charger_charging_on signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean charger_charging_on_dbus_cb(DBusMessage *const msg)
{
	charger_state_t old_charger_state = datapipe_get_gint(charger_state_pipe);
	gboolean status = FALSE;

	(void)msg;

	mce_log(LL_DEBUG,
		"Received charger_charging_on signal");

	/* Only update the charger state if needed */
	if (old_charger_state != CHARGER_STATE_ON) {
		datapipe_exec_full(&charger_state_pipe,
				   GINT_TO_POINTER(CHARGER_STATE_ON));
	}

	/* In case these are active; there's no harm to call them anyway */
	datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_BATTERY_FULL);
//	datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_BATTERY_LOW);

	datapipe_exec_full(&led_pattern_activate_pipe, MCE_LED_PATTERN_BATTERY_CHARGING);

	status = TRUE;

//EXIT:
	return status;
}

/**
 * D-Bus callback for the charger_charging_off signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean charger_charging_off_dbus_cb(DBusMessage *const msg)
{
	charger_state_t old_charger_state = datapipe_get_gint(charger_state_pipe);
	gboolean status = FALSE;

	(void)msg;

	mce_log(LL_DEBUG,
		"Received charger_charging_off signal");

	/* Only update the charger state if needed */
	if (old_charger_state != CHARGER_STATE_OFF) {
		datapipe_exec_full(&charger_state_pipe,
				   GINT_TO_POINTER(CHARGER_STATE_OFF));
	}

	/* In case these are active; there's no harm to call them anyway */
	datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_BATTERY_CHARGING);

	status = TRUE;

//EXIT:
	return status;
}

/**
 * D-Bus callback for the charger_charging_failed signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean charger_charging_failed_dbus_cb(DBusMessage *const msg)
{
	charger_state_t old_charger_state = datapipe_get_gint(charger_state_pipe);
	gboolean status = FALSE;

	(void)msg;

	mce_log(LL_DEBUG,
		"Received charger_charging_failed signal");

	/* Only update the charger state if needed */
	if (old_charger_state != CHARGER_STATE_OFF) {
		datapipe_exec_full(&charger_state_pipe,
				   GINT_TO_POINTER(CHARGER_STATE_OFF));
	}

	/* In case these are active; there's no harm to call them anyway */
	datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_BATTERY_FULL);
	datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_BATTERY_CHARGING);

	/* Generate activity */
	mce_datapipe_generate_activity();

	status = TRUE;

//EXIT:
	return status;
}

/**
 * D-Bus callback for the charger_connected signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean charger_connected_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	(void)msg;

	mce_log(LL_DEBUG,
		"Received charger_connected signal");

	if (cached_charger_connected != 1) {
		/* Generate activity */
		mce_datapipe_generate_activity();
		cached_charger_connected = 1;
	}

	status = TRUE;

//EXIT:
	return status;
}

/**
 * D-Bus callback for the charger_disconnected signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean charger_disconnected_dbus_cb(DBusMessage *const msg)
{
	charger_state_t old_charger_state = datapipe_get_gint(charger_state_pipe);
	gboolean status = FALSE;

	(void)msg;

	mce_log(LL_DEBUG,
		"Received charger_disconnected signal");

	/* Only update the charger state if needed */
	if (old_charger_state != CHARGER_STATE_OFF) {
		datapipe_exec_full(&charger_state_pipe,
				   GINT_TO_POINTER(CHARGER_STATE_OFF));
	}

	/* In case these are active; there's no harm to call them anyway */
	datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_BATTERY_FULL);
	datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_BATTERY_CHARGING);

	if (cached_charger_connected != 0) {
		/* Generate activity */
		mce_datapipe_generate_activity();
		cached_charger_connected = 0;
	}

	status = TRUE;

//EXIT:
	return status;
}

/**
 * Request update of charger status
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean request_charger_status(void)
{
	return dbus_send(BME_SERVICE, BME_REQUEST_PATH, BME_REQUEST_IF,
			 BME_STATUS_INFO_REQ, NULL, DBUS_TYPE_INVALID);
}

/** Array of dbus message handlers */
static mce_dbus_handler_t battery_bme_dbus_handlers[] =
{
	/* signals */
	{
		.interface = BME_SIGNAL_IF,
		.name      = BME_BATTERY_FULL,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.callback  = battery_full_dbus_cb,
	},
	{
		.interface = BME_SIGNAL_IF,
		.name      = BME_BATTERY_OK,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.callback  = battery_ok_dbus_cb,
	},
	{
		.interface = BME_SIGNAL_IF,
		.name      = BME_BATTERY_LOW,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.callback  = battery_low_dbus_cb,
	},
	{
		.interface = BME_SIGNAL_IF,
		.name      = BME_BATTERY_EMPTY,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.callback  = battery_empty_dbus_cb,
	},
	{
		.interface = BME_SIGNAL_IF,
		.name      = BME_BATTERY_STATE_UPDATE,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.callback  = battery_state_changed_dbus_cb,
	},
	{
		.interface = BME_SIGNAL_IF,
		.name      = BME_CHARGER_CHARGING_ON,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.callback  = charger_charging_on_dbus_cb,
	},
	{
		.interface = BME_SIGNAL_IF,
		.name      = BME_CHARGER_CHARGING_OFF,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.callback  = charger_charging_off_dbus_cb,
	},
	{
		.interface = BME_SIGNAL_IF,
		.name      = BME_CHARGER_CHARGING_FAILED,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.callback  = charger_charging_failed_dbus_cb,
	},
	{
		.interface = BME_SIGNAL_IF,
		.name      = BME_CHARGER_CONNECTED,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.callback  = charger_connected_dbus_cb,
	},
	{
		.interface = BME_SIGNAL_IF,
		.name      = BME_CHARGER_DISCONNECTED,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.callback  = charger_disconnected_dbus_cb,
	},
	/* sentinel */
	{
		.interface = 0
	}
};

/** Add dbus handlers
 */
static void battery_bme_init_dbus(void)
{
	mce_dbus_handler_register_array(battery_bme_dbus_handlers);
}

/** Remove dbus handlers
 */
static void battery_bme_quit_dbus(void)
{
	mce_dbus_handler_unregister_array(battery_bme_dbus_handlers);
}

/**
 * Init function for the battery and charger module
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
	(void)module;

	/* Add dbus handlers */
	battery_bme_init_dbus();

	/* Update charger status */
	request_charger_status();

	return NULL;
}

/**
 * Exit function for the battery and charger module
 *
 * @todo D-Bus unregistration
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	/* Remove dbus handlers */
	battery_bme_quit_dbus();

	return;
}
