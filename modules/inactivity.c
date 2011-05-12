/**
 * @file inactivity.c
 * Inactivity module -- this implements inactivity logic for MCE
 * <p>
 * Copyright © 2007-2011 Nokia Corporation and/or its subsidiary(-ies).
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
					 * dbus_new_signal(),
					 * dbus_message_append_args(),
					 * dbus_message_unref(),
					 * DBusMessage,
					 * DBUS_MESSAGE_TYPE_METHOD_CALL,
					 * DBUS_TYPE_BOOLEAN,
					 * DBUS_TYPE_INVALID,
					 * dbus_bool_t
					 *
					 * Indirect:
					 * ---
					 * MCE_SIGNAL_IF,
					 * MCE_SIGNAL_PATH,
					 * MCE_REQUEST_IF
					 * MCE_INACTIVITY_STATUS_GET,
					 * MCE_INACTIVITY_SIG,
					 * MCE_ADD_ACTIVITY_CALLBACK_REQ,
					 * MCE_REMOVE_ACTIVITY_CALLBACK_REQ
					 */
#include "datapipe.h"			/* datapipe_get_gbool(),
					 * append_filter_to_datapipe(),
					 * remove_filter_from_datapipe()
					 */

/** Module name */
#define MODULE_NAME		"inactivity"

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

/** D-Bus activity callback */
typedef struct {
	/** D-Bus activity callback owner */
	gchar *owner;
	/** D-Bus service */
	gchar *service;
	/** D-Bus path */
	gchar *path;
	/** D-Bus interface */
	gchar *interface;
	/** D-Bus method name */
	gchar *method_name;
} activity_cb_t;

/** Maximum amount of monitored activity callbacks */
#define ACTIVITY_CB_MAX_MONITORED	16

/** List of activity callbacks */
static GSList *activity_callbacks = NULL;

/** List of monitored activity requesters */
static GSList *activity_cb_monitor_list = NULL;

/** ID for inactivity timeout source */
static guint inactivity_timeout_cb_id = 0;

/** Device inactivity state */
static gboolean device_inactive = FALSE;

/**
 * Send an inactivity status reply or signal
 *
 * @param method_call A DBusMessage to reply to;
 *                    pass NULL to send an inactivity status signal instead
 * @return TRUE on success, FALSE on failure
 */
static gboolean send_inactivity_status(DBusMessage *const method_call)
{
	DBusMessage *msg = NULL;
	gboolean status = FALSE;

	mce_log(LL_DEBUG,
		"Sending inactivity status: %s",
		device_inactive ? "inactive" : "active");

	/* If method_call is set, send a reply,
	 * otherwise, send a signal
	 */
	if (method_call != NULL) {
		msg = dbus_new_method_reply(method_call);
	} else {
		/* system_inactivity_ind */
		msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
				      MCE_INACTIVITY_SIG);
	}

	/* Append the inactivity status */
	if (dbus_message_append_args(msg,
				     DBUS_TYPE_BOOLEAN, &device_inactive,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append %sargument to D-Bus message "
			"for %s.%s",
			method_call ? "reply " : "",
			method_call ? MCE_REQUEST_IF :
				      MCE_SIGNAL_IF,
			method_call ? MCE_INACTIVITY_STATUS_GET :
				      MCE_INACTIVITY_SIG);
		dbus_message_unref(msg);
		goto EXIT;
	}

	/* Send the message */
	status = dbus_send_message(msg);

EXIT:
	return status;
}

/**
 * D-Bus callback for the get inactivity status method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean inactivity_status_get_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	mce_log(LL_DEBUG, "Received inactivity status get request");

	/* Try to send a reply that contains the current inactivity status */
	if (send_inactivity_status(msg) == FALSE)
		goto EXIT;

	status = TRUE;

EXIT:
	return status;
}

/**
 * Remove an activity cb from the list of monitored processes
 * and the callback itself
 *
 * @param owner The D-Bus owner of the callback
 */
static void remove_activity_cb(const gchar *owner)
{
	GSList *tmp = activity_callbacks;

	/* Remove the name monitor for the activity callback
	 * and the activity callback itself
	 */
	(void)mce_dbus_owner_monitor_remove(owner,
					    &activity_cb_monitor_list);

	while (tmp != NULL) {
		activity_cb_t *cb;

		cb = tmp->data;

		/* Is this the matching sender? */
		if (!strcmp(cb->owner, owner)) {
			g_free(cb->owner);
			g_free(cb->service);
			g_free(cb->path);
			g_free(cb->interface);
			g_free(cb->method_name);
			g_free(cb);

			activity_callbacks =
				g_slist_remove(activity_callbacks,
					       cb);
			break;
		}

		tmp = g_slist_next(tmp);
	};
}

/**
 * D-Bus callback used for monitoring processes that add activity callbacks;
 * if the process exits, unregister the callback
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean activity_cb_monitor_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;
	const gchar *old_name;
	const gchar *new_name;
	const gchar *service;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	/* Extract result */
	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &service,
				  DBUS_TYPE_STRING, &old_name,
				  DBUS_TYPE_STRING, &new_name,
				  DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_ERR,
			"Failed to get argument from %s.%s; %s",
			"org.freedesktop.DBus", "NameOwnerChanged",
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	remove_activity_cb(old_name);
	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus callback for the add activity callback method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean add_activity_callback_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	const gchar *sender = dbus_message_get_sender(msg);
	const gchar *service = NULL;
	const gchar *path = NULL;
	const gchar *interface = NULL;
	const gchar *method_name = NULL;
	activity_cb_t *tmp = NULL;
	gboolean result = FALSE;
	gboolean status = FALSE;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	if (sender == NULL) {
		mce_log(LL_ERR,
			"Received invalid add activity callback request "
			"(sender == NULL)");
		goto EXIT;
	}

	mce_log(LL_DEBUG,
		"Received add activity callback request from %s",
		sender);

	/* Extract result */
	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &service,
				  DBUS_TYPE_STRING, &path,
				  DBUS_TYPE_STRING, &interface,
				  DBUS_TYPE_STRING, &method_name,
				  DBUS_TYPE_INVALID) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_CRIT,
			"Failed to get argument from %s.%s; %s",
			MCE_REQUEST_IF, MCE_ADD_ACTIVITY_CALLBACK_REQ,
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	if (mce_dbus_owner_monitor_add(sender,
				       activity_cb_monitor_dbus_cb,
				       &activity_cb_monitor_list,
				       ACTIVITY_CB_MAX_MONITORED) == -1) {
		mce_log(LL_ERR,
			"Failed to add name owner monitoring for `%s'",
			sender);
		goto EXIT2;
	}

	tmp = g_malloc(sizeof (activity_cb_t));

	tmp->owner = g_strdup(sender);
	tmp->service = g_strdup(service);
	tmp->path = g_strdup(path);
	tmp->interface = g_strdup(interface);
	tmp->method_name = g_strdup(method_name);

	activity_callbacks = g_slist_prepend(activity_callbacks, tmp);

	result = TRUE;

EXIT2:
	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		if (dbus_message_append_args(reply,
					     DBUS_TYPE_BOOLEAN, &result,
					     DBUS_TYPE_INVALID) == FALSE) {
			mce_log(LL_CRIT,
				"Failed to append reply argument to "
				"D-Bus message for %s.%s",
				MCE_REQUEST_IF,
				MCE_ADD_ACTIVITY_CALLBACK_REQ);
			dbus_message_unref(reply);
			goto EXIT;
		}

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

EXIT:
	return status;
}

/**
 * D-Bus callback for the remove activity callback method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean remove_activity_callback_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	const gchar *sender = dbus_message_get_sender(msg);
	gboolean status = FALSE;

	if (sender == NULL) {
		mce_log(LL_ERR,
			"Received invalid remove activity callback request "
			"(sender == NULL)");
		goto EXIT;
	}

	mce_log(LL_DEBUG,
		"Received remove activity callback request from %s",
		sender);

	status = TRUE;

	remove_activity_cb(sender);

EXIT:
	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

	return status;
}

/**
 * Call all activity callbacks, then unregister them
 */
static void call_activity_callbacks(void)
{
	GSList *tmp = activity_callbacks;

	while (tmp != NULL) {
		activity_cb_t *cb;

		cb = tmp->data;

		/* Call the calback */
		(void)dbus_send(cb->service, cb->path,
				cb->interface, cb->method_name,
				NULL,
				DBUS_TYPE_INVALID);

		g_free(cb->owner);
		g_free(cb->service);
		g_free(cb->path);
		g_free(cb->interface);
		g_free(cb->method_name);
		g_free(cb);

		tmp = g_slist_next(tmp);
	}

	g_slist_free(activity_callbacks);
	activity_callbacks = NULL;

	mce_dbus_owner_monitor_remove_all(&activity_cb_monitor_list);
}

/**
 * Timeout callback for inactivity
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean inactivity_timeout_cb(gpointer data)
{
	(void)data;

	inactivity_timeout_cb_id = 0;

	(void)execute_datapipe(&device_inactive_pipe, GINT_TO_POINTER(TRUE),
			       USE_INDATA, CACHE_INDATA);

	return FALSE;
}

/**
 * Cancel inactivity timeout
 */
static void cancel_inactivity_timeout(void)
{
	/* Remove inactivity timeout source */
	if (inactivity_timeout_cb_id != 0) {
		g_source_remove(inactivity_timeout_cb_id);
		inactivity_timeout_cb_id = 0;
	}
}

/**
 * Setup inactivity timeout
 */
static void setup_inactivity_timeout(void)
{
	gint timeout = datapipe_get_gint(inactivity_timeout_pipe);

	cancel_inactivity_timeout();

	/* Sanitise timeout */
	if (timeout <= 0)
		timeout = 30;

	/* Setup new timeout */
	inactivity_timeout_cb_id =
		g_timeout_add_seconds(timeout, inactivity_timeout_cb, NULL);
}

/**
 * Datapipe filter for inactivity
 *
 * @param data The unfiltered inactivity state;
 *             TRUE if the device is inactive,
 *             FALSE if the device is active
 * @return The filtered inactivity state;
 *             TRUE if the device is inactive,
 *             FALSE if the device is active
 */
static gpointer device_inactive_filter(gpointer data)
{
	static gboolean old_device_inactive = FALSE;
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	submode_t submode = mce_get_submode_int32();
	gpointer retval;

	device_inactive = GPOINTER_TO_INT(data);

	/* If the tklock is enabled,
	 * filter activity, unless there's an active alarm
	 */
	if ((device_inactive == FALSE) &&
	    ((submode & MCE_TKLOCK_SUBMODE) != 0) &&
	    (((alarm_ui_state != MCE_ALARM_UI_VISIBLE_INT32) &&
	      (alarm_ui_state != MCE_ALARM_UI_RINGING_INT32)) ||
	     (((submode & MCE_AUTORELOCK_SUBMODE) != 0)))) {
		device_inactive = TRUE;
		goto EXIT;
	}

	/* We got activity; restart timeouts */
	if (device_inactive == FALSE) {
		call_activity_callbacks();
		setup_inactivity_timeout();
	}

	/* Only send the inactivity status if it changed */
	if ((old_device_inactive != device_inactive) &&
	    (((submode & MCE_TKLOCK_SUBMODE) == 0) ||
	     (device_inactive == TRUE)))
		send_inactivity_status(NULL);

	old_device_inactive = device_inactive;

EXIT:
	retval = GINT_TO_POINTER(device_inactive);

	return retval;
}

/**
 * Inactivity timeout trigger
 *
 * @param data Unused
 */
static void inactivity_timeout_trigger(gconstpointer data)
{
	(void)data;

	setup_inactivity_timeout();
}

/**
 * Init function for the inactivity module
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

	/* Append triggers/filters to datapipes */
	append_filter_to_datapipe(&device_inactive_pipe,
				  device_inactive_filter);
	append_output_trigger_to_datapipe(&inactivity_timeout_pipe,
					  inactivity_timeout_trigger);

	/* get_inactivity_status */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_INACTIVITY_STATUS_GET,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 inactivity_status_get_dbus_cb) == NULL)
		goto EXIT;

	/* add_activity_callback */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_ADD_ACTIVITY_CALLBACK_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 add_activity_callback_dbus_cb) == NULL)
		goto EXIT;

	/* remove_activity_callback */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_REMOVE_ACTIVITY_CALLBACK_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 remove_activity_callback_dbus_cb) == NULL)
		goto EXIT;

	setup_inactivity_timeout();

EXIT:
	return NULL;
}

/**
 * Exit function for the inactivity module
 *
 * @todo D-Bus unregistration
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&inactivity_timeout_pipe,
					    inactivity_timeout_trigger);
	remove_filter_from_datapipe(&device_inactive_pipe,
				    device_inactive_filter);

	/* Remove all timer sources */
	cancel_inactivity_timeout();

	return;
}
