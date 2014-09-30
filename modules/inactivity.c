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

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-dbus.h"

#include <string.h>

#include <mce/dbus-names.h>

#include <gmodule.h>

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

	mce_log(LL_DEVEL, "Received inactivity status get request from %s",
	       mce_dbus_get_message_sender_ident(msg));

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

	remove_activity_cb(service);
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

	mce_log(LL_DEVEL, "Received add activity callback request from %s",
		mce_dbus_get_name_owner_ident(sender));

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

	mce_log(LL_DEVEL, "Received remove activity callback request from %s",
		mce_dbus_get_name_owner_ident(sender));

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
	cover_state_t   proximity_state = proximity_state_get();
	display_state_t display_state   = display_state_get();
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	call_state_t call_state = datapipe_get_gint(call_state_pipe);

	device_inactive = GPOINTER_TO_INT(data);

	/* nothing to filter if we are already inactive */
	if( device_inactive )
		goto EXIT;

	/* Never filter inactivity if display is in dimmed state.
	 *
	 * Whether we have arrived to dimmed state via expected or
	 * unexpected routes, the touch input is active and ui side
	 * event eater will ignore only the first event. If we do
	 * not allow activity (and turn on the display) we will get
	 * ui interaction in odd looking dimmed state that then gets
	 * abruptly ended by blanking timer.
	 */
	if( display_state == MCE_DISPLAY_DIM )
		goto EXIT;

	/* system state must be USER or ACT DEAD */
	switch( system_state ) {
	case MCE_STATE_USER:
	case MCE_STATE_ACTDEAD:
		break;
	default:
		mce_log(LL_DEBUG, "system_state != USER|ACTDEAD"
			"; ignoring activity");
		device_inactive = TRUE;
		goto EXIT;
	}

	/* tklock must be off, or there must be alarms or calls */
	if( submode & MCE_TKLOCK_SUBMODE ) {
		gboolean have_alarms = FALSE;
		gboolean have_calls  = FALSE;
		gboolean display_on  = FALSE;

		switch( alarm_ui_state ) {
		case MCE_ALARM_UI_RINGING_INT32:
		case MCE_ALARM_UI_VISIBLE_INT32:
			have_alarms = TRUE;
			break;
		default:
			break;
		}

		switch( call_state ) {
		case CALL_STATE_RINGING:
		case CALL_STATE_ACTIVE:
			have_calls = TRUE;
		default:
			break;
		}

		if( display_state == MCE_DISPLAY_ON )
			display_on = TRUE;

		if( !display_on && !have_alarms && !have_calls ) {
			mce_log(LL_DEBUG, "tklock enabled, no alarms or calls;"
				" ignoring activity");
			device_inactive = TRUE;
			goto EXIT;
		}
	}

	/* if proximity is covered, display must not be off */
	if( proximity_state == COVER_CLOSED ) {
		switch( display_state ) {
		case MCE_DISPLAY_OFF:
		case MCE_DISPLAY_LPM_OFF:
		case MCE_DISPLAY_LPM_ON:
		case MCE_DISPLAY_POWER_UP:
		case MCE_DISPLAY_POWER_DOWN:
			mce_log(LL_DEBUG, "display=off, proximity=covered; ignoring activity");
			device_inactive = TRUE;
			goto EXIT;

		default:
		case MCE_DISPLAY_UNDEF:
		case MCE_DISPLAY_DIM:
		case MCE_DISPLAY_ON:
			break;
		}
	}

EXIT:
	/* React to activity */
	if( !device_inactive ) {
		call_activity_callbacks();
		setup_inactivity_timeout();
	}

	/* Handle inactivity state change */
	if( old_device_inactive != device_inactive ) {
		old_device_inactive = device_inactive;

		send_inactivity_status(NULL);
	}

	/* Return filtered activity state */
	return GINT_TO_POINTER(device_inactive);
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

/** Generate activity from proximity sensor uncover
 *
 * @param data proximity sensor state as void pointer
 */
static void proximity_sensor_trigger(gconstpointer data)
{
	static cover_state_t old_proximity_state = COVER_OPEN;

	cover_state_t proximity_state = GPOINTER_TO_INT(data);

	/* generate activity if proximity sensor is
	 * uncovered and there is a incoming call */

	if( old_proximity_state == proximity_state )
		goto EXIT;

	old_proximity_state = proximity_state;

	if( proximity_state != COVER_OPEN )
		goto EXIT;

	call_state_t call_state = datapipe_get_gint(call_state_pipe);

	if( call_state != CALL_STATE_RINGING )
		goto EXIT;

	mce_log(LL_INFO, "proximity -> uncovered, call = ringing");
	execute_datapipe(&device_inactive_pipe, GINT_TO_POINTER(FALSE),
			 USE_INDATA, CACHE_INDATA);

EXIT:
	return;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t inactivity_dbus_handlers[] =
{
	/* signals - outbound (for Introspect purposes only) */
	{
		.interface = MCE_SIGNAL_IF,
		.name      = MCE_INACTIVITY_SIG,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.args      =
			"    <arg name=\"device_inactive\" type=\"b\"/>\n"
	},
	/* method calls */
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_INACTIVITY_STATUS_GET,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = inactivity_status_get_dbus_cb,
		.args      =
			"    <arg direction=\"out\" name=\"device_inactive\" type=\"b\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_ADD_ACTIVITY_CALLBACK_REQ,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = add_activity_callback_dbus_cb,
		.args      =
			"    <arg direction=\"in\" name=\"service_name\" type=\"s\"/>\n"
			"    <arg direction=\"in\" name=\"object_path\" type=\"s\"/>\n"
			"    <arg direction=\"in\" name=\"interface_name\" type=\"s\"/>\n"
			"    <arg direction=\"in\" name=\"method_name\" type=\"s\"/>\n"
			"    <arg direction=\"out\" name=\"added\" type=\"b\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_REMOVE_ACTIVITY_CALLBACK_REQ,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = remove_activity_callback_dbus_cb,
		.args      =
			""
	},
	/* sentinel */
	{
		.interface = 0
	}
};

/** Add dbus handlers
 */
static void mce_inactivity_init_dbus(void)
{
	mce_dbus_handler_register_array(inactivity_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mce_inactivity_quit_dbus(void)
{
	mce_dbus_handler_unregister_array(inactivity_dbus_handlers);
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
	append_output_trigger_to_datapipe(&proximity_sensor_pipe,
					  proximity_sensor_trigger);
	append_output_trigger_to_datapipe(&inactivity_timeout_pipe,
					  inactivity_timeout_trigger);

	/* Add dbus handlers */
	mce_inactivity_init_dbus();

	setup_inactivity_timeout();

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

	/* Remove dbus handlers */
	mce_inactivity_quit_dbus();

	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&inactivity_timeout_pipe,
					    inactivity_timeout_trigger);
	remove_output_trigger_from_datapipe(&proximity_sensor_pipe,
					    proximity_sensor_trigger);
	remove_filter_from_datapipe(&device_inactive_pipe,
				    device_inactive_filter);

	/* Remove all timer sources */
	cancel_inactivity_timeout();

	return;
}
