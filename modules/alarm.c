/**
 * @file alarm.c
 * Alarm interface module for the Mode Control Entity
 * <p>
 * Copyright © 2005-2009 Nokia Corporation and/or its subsidiary(-ies).
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
#include <gmodule.h>

#include <string.h>			/* strcmp() */

#include "mce.h"

#include "mce-log.h"			/* mce_log(), LL_* */
#include "mce-dbus.h"			/* Direct:
					 * ---
					 * mce_dbus_handler_add(),
					 * dbus_send_message(),
					 * dbus_new_method_reply(),
					 * dbus_message_get_args(),
					 * dbus_message_get_no_reply(),
					 * dbus_error_init(),
					 * dbus_error_free(),
					 * DBUS_MESSAGE_TYPE_METHOD_CALL,
					 * DBUS_TYPE_INT32,
					 * DBUS_TYPE_INVALID,
					 * DBusMessage, DBusError,
					 * dbus_int32_t
					 */
#include "datapipe.h"			/* execute_datapipe() */

#ifndef VISUAL_REMINDERS_SERVICE
typedef enum {
	VISUAL_REMINDER_ON_SCREEN,
	VISUAL_REMINDER_NOT_ON_SCREEN,
	VISUAL_REMINDER_ON_SCREEN_NO_SOUND
} visual_reminders_status;

#define VISUAL_REMINDERS_SERVICE	"com.nokia.voland"
#define VISUAL_REMINDERS_SIGNAL_IF	"com.nokia.voland.signal"
#define VISUAL_REMINDERS_SIGNAL_PATH	"/com/nokia/voland/signal"
#define VISUAL_REMINDER_STATUS_SIG	"visual_reminders_status"
#endif /* VISUAL_REMINDERS_SERVICE */

/** Module name */
#define MODULE_NAME		"alarm"

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
	/** Name of the module */
	.name = MODULE_NAME,
	/** Module provides */
	.provides = provides,
	/** Module priority */
	.priority = 250
};

/**
 * D-Bus callback for the alarm dialog status signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean alarm_dialog_status_dbus_cb(DBusMessage *const msg)
{
	alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_INVALID_INT32;
	gboolean status = FALSE;
	DBusError error;
	dbus_int32_t dialog_status;

	/* Register error channel */
	dbus_error_init(&error);

	mce_log(LL_DEBUG,
		"Received alarm dialog status signal");

	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_INT32, &dialog_status,
				  DBUS_TYPE_INVALID) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_CRIT,
			"Failed to get argument from %s.%s: %s",
			VISUAL_REMINDERS_SIGNAL_IF,
			VISUAL_REMINDER_STATUS_SIG,
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	/* Convert alarm dialog status to to MCE alarm ui enum */
	switch (dialog_status) {
	case VISUAL_REMINDER_ON_SCREEN:
		alarm_ui_state = MCE_ALARM_UI_RINGING_INT32;
		break;

	case VISUAL_REMINDER_ON_SCREEN_NO_SOUND:
		alarm_ui_state = MCE_ALARM_UI_VISIBLE_INT32;
		break;

	case VISUAL_REMINDER_NOT_ON_SCREEN:
		alarm_ui_state = MCE_ALARM_UI_OFF_INT32;
		break;

	default:
		mce_log(LL_ERR,
			"Received invalid alarm dialog status; "
			"defaulting to OFF");
		alarm_ui_state = MCE_ALARM_UI_OFF_INT32;
		break;
	}

	(void)execute_datapipe(&alarm_ui_state_pipe,
			       GINT_TO_POINTER(alarm_ui_state),
			       USE_INDATA, CACHE_INDATA);

	status = TRUE;

EXIT:
	return status;
}

/**
 * Init function for the alarm interface module
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

	/* visual_reminders_status */
	if (mce_dbus_handler_add(VISUAL_REMINDERS_SIGNAL_IF,
				 VISUAL_REMINDER_STATUS_SIG,
				 NULL,
				 DBUS_MESSAGE_TYPE_SIGNAL,
				 alarm_dialog_status_dbus_cb) == NULL)
		goto EXIT;

EXIT:
	return NULL;
}

/**
 * Exit function for the alarm interface module
 *
 * @todo D-Bus unregistration
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	return;
}
