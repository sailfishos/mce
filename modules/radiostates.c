/**
 * @file radiostates.c
 * Radio state module for the Mode Control Entity
 * <p>
 * Copyright © 2010-2011 Nokia Corporation and/or its subsidiary(-ies).
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

#include "radiostates.h"

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-io.h"
#include "../mce-conf.h"
#include "../mce-dbus.h"

#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include <gmodule.h>

/* Forward declarations needed to keep connman related static
 * functions cleanly separated from the legacy mce code */
static void xconnman_sync_master_to_offline(void);

/** Module name */
#define MODULE_NAME		"radiostates"

/** Number of radio states */
#define RADIO_STATES_COUNT	6

/** Names of radio state configuration keys */
static const gchar *const radio_state_names[RADIO_STATES_COUNT] = {
	MCE_CONF_MASTER_RADIO_STATE,
	MCE_CONF_CELLULAR_RADIO_STATE,
	MCE_CONF_WLAN_RADIO_STATE,
	MCE_CONF_BLUETOOTH_RADIO_STATE,
	MCE_CONF_NFC_RADIO_STATE,
	MCE_CONF_FMTX_RADIO_STATE
};

/** Radio state default values */
static const gboolean radio_state_defaults[RADIO_STATES_COUNT] = {
	DEFAULT_MASTER_RADIO_STATE,
	DEFAULT_CELLULAR_RADIO_STATE,
	DEFAULT_WLAN_RADIO_STATE,
	DEFAULT_BLUETOOTH_RADIO_STATE,
	DEFAULT_NFC_RADIO_STATE,
	DEFAULT_FMTX_RADIO_STATE
};

/** radio state flag */
static const gint radio_state_flags[RADIO_STATES_COUNT] = {
	MCE_RADIO_STATE_MASTER,
	MCE_RADIO_STATE_CELLULAR,
	MCE_RADIO_STATE_WLAN,
	MCE_RADIO_STATE_BLUETOOTH,
	MCE_RADIO_STATE_NFC,
	MCE_RADIO_STATE_FMTX
};

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

/** Real radio states */
static gulong radio_states = 0;

/** Active radio states (master switch disables all radios) */
static gulong active_radio_states = 0;

/**
 * Get default radio states from customisable settings
 *
 * @return -1 in case of error, radio states otherwise
 */
static gint get_default_radio_states(void)
{
	gint default_radio_states = 0;

	for( size_t i = 0; i < RADIO_STATES_COUNT; ++i ) {
		gboolean flag = mce_conf_get_bool(MCE_CONF_RADIO_STATES_GROUP,
						  radio_state_names[i],
						  radio_state_defaults[i]);
		if( flag ) {
			default_radio_states |= radio_state_flags[i];
		}
	}

	mce_log(LL_DEBUG, "default_radio_states = %x", default_radio_states);

	return default_radio_states;
}

/**
 * Send the radio states
 *
 * @param method_call A DBusMessage to reply to;
 *                    pass NULL to send a signal instead
 * @return TRUE on success, FALSE on failure
 */
static gboolean send_radio_states(DBusMessage *const method_call)
{
	DBusMessage *msg = NULL;
	gboolean status = FALSE;

	dbus_uint32_t data = active_radio_states;

	mce_log(LL_DEBUG, "Sending radio states: %x", data);

	/* If method_call is set, send a reply,
	 * otherwise, send a signal
	 */
	if (method_call != NULL) {
		msg = dbus_new_method_reply(method_call);
	} else {
		/* radio_states_ind */
		msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
				      MCE_RADIO_STATES_SIG);
	}

	/* Append the radio states */
	if (dbus_message_append_args(msg,
				     DBUS_TYPE_UINT32, &data,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append %sargument to D-Bus message "
			"for %s.%s",
			method_call ? "reply " : "",
			method_call ? MCE_REQUEST_IF :
				      MCE_SIGNAL_IF,
			method_call ? MCE_RADIO_STATES_GET :
				      MCE_RADIO_STATES_SIG);
		dbus_message_unref(msg);
		goto EXIT;
	}

	/* Send the message */
	status = dbus_send_message(msg);

EXIT:
	return status;
}

/**
 * Set the radio states
 *
 * @param states The raw radio states
 * @param mask The raw radio states mask
 */
static void set_radio_states(const gulong states, const gulong mask)
{
	radio_states = (radio_states & ~mask) | (states & mask);

	if( (mask & MCE_RADIO_STATE_MASTER) && !(states & MCE_RADIO_STATE_MASTER) ) {
		active_radio_states = states & mask;
	}
	else if( !(radio_states & MCE_RADIO_STATE_MASTER) ) {
		active_radio_states = (active_radio_states & ~mask) | (states & mask);
	}
	else {
		active_radio_states = (radio_states & ~mask) | (states & mask);
	}
}

/**
 * Save the radio states to persistant storage
 *
 * @param online_states The online radio states to store
 * @param offline_states The offline radio states to store
 * @return TRUE on success, FALSE on failure
 */
static gboolean save_radio_states(const gulong online_states,
				  const gulong offline_states)
{
	gboolean status = FALSE;

	if (mce_are_settings_locked() == TRUE) {
		mce_log(LL_WARN,
			"Cannot save radio states; backup/restore "
			"or device clear/factory reset pending");
		goto EXIT;
	}

	status = mce_write_number_string_to_file_atomic(MCE_ONLINE_RADIO_STATES_PATH, online_states);

	if (status == FALSE)
		goto EXIT;

	status = mce_write_number_string_to_file_atomic(MCE_OFFLINE_RADIO_STATES_PATH, offline_states);

EXIT:
	return status;
}

/**
 * Read radio states from persistent storage
 *
 * @param online_file The path to the online radio states file
 * @param[out] online_states A pointer to the restored online radio states
 * @param offline_file The path to the offline radio states file
 * @param[out] offline_states A pointer to the restored offline radio states
 * @return TRUE on success, FALSE on failure
 */
static gboolean read_radio_states(const gchar *const online_file,
				  gulong *online_states,
				  const gchar *const offline_file,
				  gulong *offline_states)
{
	gboolean status = FALSE;

	/* The files get generated by mce on 1st bootup. Skip the
	 * read attempt and associated diagnostic logging if the
	 * files do not exist */
	if( access(offline_file, F_OK) == -1 && errno == ENOENT )
		goto EXIT;

	status = mce_read_number_string_from_file(offline_file, offline_states,
						  NULL, TRUE, TRUE);

	if (status == FALSE)
		goto EXIT;

	status = mce_read_number_string_from_file(online_file, online_states,
						  NULL, TRUE, TRUE);

EXIT:
	return status;
}

/**
 * Restore the radio states from persistant storage
 *
 * @param[out] online_states A pointer to the restored online radio states
 * @param[out] offline_states A pointer to the restored offline radio states
 * @return TRUE on success, FALSE on failure
 */
static gboolean restore_radio_states(gulong *online_states,
				     gulong *offline_states)
{
	if (mce_are_settings_locked() == TRUE) {
		mce_log(LL_INFO, "Removing stale settings lockfile");

		if (mce_unlock_settings() == FALSE) {
			mce_log(LL_ERR, "Failed to remove settings lockfile; %m");
		}
	}

	return read_radio_states(MCE_ONLINE_RADIO_STATES_PATH, online_states, MCE_OFFLINE_RADIO_STATES_PATH, offline_states);
}

/**
 * Restore the default radio states from persistent storage
 *
 * @param[out] online_states A pointer to the restored online radio states
 * @param[out] offline_states A pointer to the restored offline radio states
 * @return TRUE on success, FALSE on failure
 */
static gboolean restore_default_radio_states(gulong *online_states,
					     gulong *offline_states)
{
	gint default_radio_states = get_default_radio_states();
	gboolean status = FALSE;

	*offline_states = 0;
	*online_states = 0;

	if (default_radio_states == -1)
		goto EXIT;

	*offline_states = default_radio_states;

	if (default_radio_states & MCE_RADIO_STATE_MASTER)
		*online_states = default_radio_states;

	if( !save_radio_states(*online_states, *offline_states) ) {
		mce_log(LL_ERR, "Could not save restored radio states");
		goto EXIT;
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus callback for the get radio states method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean get_radio_states_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	mce_log(LL_DEVEL, "Received get radio states request from %s",
		mce_dbus_get_message_sender_ident(msg));

	/* Try to send a reply that contains the current radio states */
	if (send_radio_states(msg) == FALSE)
		goto EXIT;

	status = TRUE;

EXIT:
	return status;
}

/** Process radio state change within mce
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean radio_states_change(gulong states, gulong mask)
{
	gboolean status = TRUE;
	gulong   old_radio_states = active_radio_states;

	set_radio_states(states, mask);

	/* If we fail to write the radio states, restore the old states */
	if( !save_radio_states(active_radio_states, radio_states) ) {
		set_radio_states(old_radio_states, ~0);
		status = FALSE;
	}

	if (old_radio_states != active_radio_states) {
		send_radio_states(NULL);
		gint master = (radio_states & MCE_RADIO_STATE_MASTER) ? 1 : 0;

		/* This is just to make sure that the cache is up to date
		 * and that all callbacks are called; the trigger inside
		 * radiostates.c has already had all its actions performed
		 */
		datapipe_exec_full(&master_radio_enabled_pipe, GINT_TO_POINTER(master), DATAPIPE_CACHE_INDATA);
	}

	/* After datapipe execution the radio state should
	 * be stable - sync connman offline property to it */
	xconnman_sync_master_to_offline();

	return status;
}

/**
 * D-Bus callback for radio states change method call
 *
 * @todo Decide on error handling policy
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean req_radio_states_change_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t   no_reply = dbus_message_get_no_reply(msg);
	gboolean      status   = FALSE;
	dbus_uint32_t states   = 0;
	dbus_uint32_t mask     = 0;
	DBusError     error    = DBUS_ERROR_INIT;

	mce_log(LL_DEVEL, "Received radio states change request from %s",
		mce_dbus_get_message_sender_ident(msg));

	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_UINT32, &states,
				  DBUS_TYPE_UINT32, &mask,
				  DBUS_TYPE_INVALID) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_CRIT,
			"Failed to get argument from %s.%s: %s",
			MCE_REQUEST_IF, MCE_RADIO_STATES_CHANGE_REQ,
			error.message);
		goto EXIT;
	}

	radio_states_change(states, mask);

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

EXIT:
	dbus_error_free(&error);
	return status;
}

/**
 * Handle master radio state
 *
 * @param data The master radio state stored in a pointer
 */
static void master_radio_enabled_trigger(gconstpointer data)
{
	gulong new_radio_states;

	if (GPOINTER_TO_UINT(data) != 0)
		new_radio_states = (radio_states | MCE_RADIO_STATE_MASTER);
	else
		new_radio_states = (radio_states & ~MCE_RADIO_STATE_MASTER);

	/* If we fail to write the radio states, use the old states */
	if( !save_radio_states(active_radio_states, radio_states) )
		new_radio_states = radio_states;

	if (radio_states != new_radio_states) {
		set_radio_states(new_radio_states, MCE_RADIO_STATE_MASTER);
		send_radio_states(NULL);
	}
}

/* ------------------------------------------------------------------------- *
 * Functionality for keeping MCE Master radio state synchronized with
 * connman OfflineMode property.
 *
 * If OfflineMode property is changed via connman, the mce master radio
 * state is changed accordingly and legacy mce radio state change signals
 * are broadcast.
 *
 * If MCE master radio switch is toggled, the connman OfflineMode property
 * is changed accordingly.
 *
 * The mce master radio state is preserved over reboots and mce restarts
 * and will take priority over the OfflineMode setting kept by connman.
 *
 * If connman for any reason chooses not to obey OfflineMode property
 * change, mce will modify master radio state instead.
 * ------------------------------------------------------------------------- */

/** org.freedesktop.DBus.NameOwnerChanged D-Bus signal */
#define DBUS_NAME_OWNER_CHANGED_SIG "NameOwnerChanged"

/** Connman D-Bus service name; mce is tracking ownership of this */
#define CONNMAN_SERVICE         "net.connman"

/** Connman D-Bus interface */
#define CONNMAN_INTERFACE       "net.connman.Manager"

/** Default connman D-Bus object path */
#define CONNMAN_OBJECT_PATH     "/"

/** net.connman.Manager.GetProperties D-Bus method call */
#define CONNMAN_GET_PROPERTIES_REQ   "GetProperties"

/** net.connman.Manager.SetProperty D-Bus method call */
#define CONNMAN_SET_PROPERTY_REQ     "SetProperty"

/** net.connman.Manager.PropertyChanged D-Bus signal */
#define CONNMAN_PROPERTY_CHANGED_SIG "PropertyChanged"

/** Initializer for dbus_any_t; largest union member set to zero */
#define DBUS_ANY_INIT { .i64 = 0 }

/** Rule for matching connman service name owner changes */
static const char xconnman_name_owner_rule[] =
"type='signal'"
",sender='"DBUS_SERVICE_DBUS"'"
",interface='"DBUS_INTERFACE_DBUS"'"
",member='"DBUS_NAME_OWNER_CHANGED_SIG"'"
",path='"DBUS_PATH_DBUS"'"
",arg0='"CONNMAN_SERVICE"'"
;

/** Rule for matching connman property value changes */
static const char xconnman_prop_change_rule[] =
"type='signal'"
",sender='"CONNMAN_SERVICE"'"
",interface='"CONNMAN_INTERFACE"'"
",member='"CONNMAN_PROPERTY_CHANGED_SIG"'"
",path='"CONNMAN_OBJECT_PATH"'"
;

/** D-Bus connection for doing ipc with connman */
static DBusConnection *connman_bus = 0;

/** Availability of connman D-Bus service */
static gboolean connman_running = FALSE;

/** Last MCE master radio state sent to connman; initialized to invalid value */
static gulong connman_master = ~0lu;

/** Flag: query connman properties if no change signal received
 *
 * FIXME/HACK: connman might ignore property setting without
 * complaining a bit -> set a flag when we are expecting a
 * property changed signal before getting reply to a set property
 * method call. Note that this will cease to work if connman ever
 * starts to send replies before signaling the changes */
static gboolean connman_verify_property_setting = FALSE;

static gboolean xconnman_get_properties(void);

/** Handle reply to asynchronous connman property change D-Bus method call
 *
 * @param pc        State data for asynchronous D-Bus method call
 * @param user_data (not used)
 */
static void xconnman_set_property_cb(DBusPendingCall *pc, void *user_data)
{
	(void)user_data;

	DBusMessage *rsp   = 0;
	DBusError    err   = DBUS_ERROR_INIT;

	if( !(rsp = dbus_pending_call_steal_reply(pc)) )
		goto EXIT;

	if( dbus_set_error_from_message(&err, rsp) ) {
		mce_log(LL_WARN, "%s: %s", err.name, err.message);
		goto EXIT;
	}

	/* NOTE: there is either empty or error reply message, we have
	 * no clue whether connman actually modified the property */

	mce_log(LL_DEBUG, "set property acked by connman");

	/* Query properties if missing an expected property changed signal */
	if( connman_verify_property_setting ) {
		connman_verify_property_setting = FALSE;
		mce_log(LL_DEBUG, "no change signal seen, querying props");
		if( !xconnman_get_properties() )
			mce_log(LL_WARN, "failed to query connman properties");
	}

EXIT:
	if( rsp ) dbus_message_unref(rsp);
	dbus_error_free(&err);
}

/** Initiate asynchronous connman property change D-Bus method call
 *
 * @param key property name
 * @param val value to set
 *
 * @return TRUE if the method call was initiated, or FALSE in case of errors
 */
static gboolean xconnman_set_property_bool(const char *key, gboolean val)
{
	gboolean         res  = FALSE;
	DBusMessage     *req  = 0;
	DBusPendingCall *pc   = 0;
	dbus_bool_t      dta  = val;

	DBusMessageIter miter, viter;

	mce_log(LL_DEBUG, "%s = %s", key, val ? "true" : "false");

	if( !(req = dbus_message_new_method_call(CONNMAN_SERVICE,
						 CONNMAN_OBJECT_PATH,
						 CONNMAN_INTERFACE,
						 CONNMAN_SET_PROPERTY_REQ)) )
		goto EXIT;

	dbus_message_iter_init_append(req, &miter);
	dbus_message_iter_append_basic(&miter, DBUS_TYPE_STRING, &key);

	if( !dbus_message_iter_open_container(&miter, DBUS_TYPE_VARIANT,
					      DBUS_TYPE_BOOLEAN_AS_STRING,
					      &viter) ) {
		mce_log(LL_WARN, "container open failed");
		goto EXIT;
	}

	dbus_message_iter_append_basic(&viter, DBUS_TYPE_BOOLEAN, &dta);

	if( !dbus_message_iter_close_container(&miter, &viter) ) {
		mce_log(LL_WARN, "container close failed");
		goto EXIT;
	}

	if( !dbus_connection_send_with_reply(connman_bus, req, &pc, -1) )
		goto EXIT;

	if( !pc )
		goto EXIT;

	mce_dbus_pending_call_blocks_suspend(pc);

	if( !dbus_pending_call_set_notify(pc, xconnman_set_property_cb, 0, 0) )
		goto EXIT;

	// success
	res = TRUE;

EXIT:
	if( pc )  dbus_pending_call_unref(pc);
	if( req ) dbus_message_unref(req);

	return res;
}

/** Synchronize connman OfflineMode -> MCE master radio state
 */
static void xconnman_sync_offline_to_master(void)
{
	if( (connman_master ^ active_radio_states) & MCE_RADIO_STATE_MASTER ) {
		mce_log(LL_DEBUG, "sync connman OfflineMode -> mce master");
		radio_states_change(connman_master, MCE_RADIO_STATE_MASTER);
	}
}

/** Synchronize MCE master radio state -> connman OfflineMode
 */
static void xconnman_sync_master_to_offline(void)
{
	gulong master;

	if( !connman_running )
		return;

	master = radio_states & MCE_RADIO_STATE_MASTER;

	if( connman_master != master ) {
		connman_master = master;
		mce_log(LL_DEBUG, "sync mce master -> connman OfflineMode");

		/* Expect property change signal ... */
		connman_verify_property_setting = TRUE;

		/* ... before we get reply to set property */
		xconnman_set_property_bool("OfflineMode", !connman_master);
	}
}

/** Process connman property value change
 *
 * @param key  property name
 * @param type dbus type of the property (DBUS_TYPE_BOOLEAN, ...)
 * @param val  value union; consult type for actual content
 */
static void xconnman_property_changed(const char *key, int type,
				      const dbus_any_t *val)
{
	switch( type ) {
	case DBUS_TYPE_STRING:
		mce_log(LL_DEBUG, "%s -> '%s'", key, val->s);
		break;

	case DBUS_TYPE_BOOLEAN:
		mce_log(LL_DEBUG, "%s -> %s", key, val->b ? "true" : "false");
		break;

	default:
		mce_log(LL_DEBUG, "%s -> (unhandled)", key);
		break;
	}

	if( !strcmp(key, "OfflineMode") && type == DBUS_TYPE_BOOLEAN ) {
		/* Got it, no need for explicit query */
		connman_verify_property_setting = FALSE;

		connman_master = val->b ? 0 : MCE_RADIO_STATE_MASTER;
		xconnman_sync_offline_to_master();
	}
}

/** Handle connman property changed signals
 *
 * @param msg net.connman.Manager.PropertyChanged D-Bus signal
 */
static void xconnman_handle_property_changed_signal(DBusMessage *msg)
{
	const char  *key = 0;
	dbus_any_t   val = DBUS_ANY_INIT;

	int          vtype;

	DBusMessageIter miter, viter;

	if( !dbus_message_iter_init(msg, &miter) )
		goto EXIT;

	if( dbus_message_iter_get_arg_type(&miter) != DBUS_TYPE_STRING )
		goto EXIT;

	dbus_message_iter_get_basic(&miter, &key);
	dbus_message_iter_next(&miter);

	if( dbus_message_iter_get_arg_type(&miter) != DBUS_TYPE_VARIANT )
		goto EXIT;

	dbus_message_iter_recurse(&miter, &viter);
	vtype = dbus_message_iter_get_arg_type(&viter);
	if( !dbus_type_is_basic(vtype) )
		goto EXIT;

	dbus_message_iter_get_basic(&viter, &val);
	xconnman_property_changed(key, vtype, &val);

EXIT:
	return;
}

/** Handle reply to asynchronous connman properties query
 *
 * @param pc        State data for asynchronous D-Bus method call
 * @param user_data (not used)
 */
static void xconnman_get_properties_cb(DBusPendingCall *pc, void *user_data)
{
	(void)user_data;

	DBusMessage *rsp = 0;
	DBusError    err = DBUS_ERROR_INIT;
	const char  *key = 0;
	dbus_any_t   val = DBUS_ANY_INIT;

	int          vtype;

	DBusMessageIter miter, aiter, diter, viter;

	if( !(rsp = dbus_pending_call_steal_reply(pc)) )
		goto EXIT;

	if( dbus_set_error_from_message(&err, rsp) ) {
		mce_log(LL_WARN, "%s: %s", err.name, err.message);
		goto EXIT;
	}

	if( !dbus_message_iter_init(rsp, &miter) )
		goto EXIT;

	if( dbus_message_iter_get_arg_type(&miter) != DBUS_TYPE_ARRAY )
		goto EXIT;

	dbus_message_iter_recurse(&miter, &aiter);

	while( dbus_message_iter_get_arg_type(&aiter) == DBUS_TYPE_DICT_ENTRY ) {
		dbus_message_iter_recurse(&aiter, &diter);
		dbus_message_iter_next(&aiter);

		if( dbus_message_iter_get_arg_type(&diter) != DBUS_TYPE_STRING )
			goto EXIT;

		dbus_message_iter_get_basic(&diter, &key);
		dbus_message_iter_next(&diter);

		if( dbus_message_iter_get_arg_type(&diter) != DBUS_TYPE_VARIANT )
			goto EXIT;

		dbus_message_iter_recurse(&diter, &viter);
		vtype = dbus_message_iter_get_arg_type(&viter);
		if( !dbus_type_is_basic(vtype) )
			continue;

		dbus_message_iter_get_basic(&viter, &val);
		xconnman_property_changed(key, vtype, &val);
	}

EXIT:
	if( rsp ) dbus_message_unref(rsp);
	dbus_error_free(&err);
}

/** Initiate asynchronous connman properties query
 *
 * @return TRUE if the method call was initiated, or FALSE in case of errors
 */
static gboolean xconnman_get_properties(void)
{
	gboolean         res  = FALSE;
	DBusMessage     *req  = 0;
	DBusPendingCall *pc   = 0;

	if( !(req = dbus_message_new_method_call(CONNMAN_SERVICE,
						 CONNMAN_OBJECT_PATH,
						 CONNMAN_INTERFACE,
						 CONNMAN_GET_PROPERTIES_REQ)) )
		goto EXIT;

	if( !dbus_connection_send_with_reply(connman_bus, req, &pc, -1) )
		goto EXIT;

	if( !pc )
		goto EXIT;

	mce_dbus_pending_call_blocks_suspend(pc);

	if( !dbus_pending_call_set_notify(pc, xconnman_get_properties_cb, 0, 0) )
		goto EXIT;

	// success
	res = TRUE;

EXIT:
	if( pc )  dbus_pending_call_unref(pc);
	if( req ) dbus_message_unref(req);

	return res;
}

/** Process connman D-Bus service availability change
 *
 * @param running Reported connman availability state
 */
static void xconnman_set_runstate(gboolean running)
{
	if( connman_running == running )
		return;

	connman_running = running;
	mce_log(LL_NOTICE, "%s: %s", CONNMAN_SERVICE,
		connman_running ? "available" : "stopped");

	if( connman_running ) {
		xconnman_sync_master_to_offline();
	}
	else {
		/* force master -> offlinemode sync on connman restart */
		connman_master = ~0lu;
	}
}

/** Handle reply to asynchronous connman service name ownership query
 *
 * @param pc        State data for asynchronous D-Bus method call
 * @param user_data (not used)
 */
static void xconnman_check_service_cb(DBusPendingCall *pc, void *user_data)
{
	(void)user_data;

	DBusMessage *rsp   = 0;
	const char  *owner = 0;
	DBusError    err   = DBUS_ERROR_INIT;

	if( !(rsp = dbus_pending_call_steal_reply(pc)) )
		goto EXIT;

	if( dbus_set_error_from_message(&err, rsp) ||
	    !dbus_message_get_args(rsp, &err,
				   DBUS_TYPE_STRING, &owner,
				   DBUS_TYPE_INVALID) )
	{
		if( strcmp(err.name, DBUS_ERROR_NAME_HAS_NO_OWNER) ) {
			mce_log(LL_WARN, "%s: %s", err.name, err.message);
		}
		goto EXIT;
	}

	xconnman_set_runstate(owner && *owner);

EXIT:
	if( rsp ) dbus_message_unref(rsp);
	dbus_error_free(&err);
}

/** Initiate asynchronous connman service name ownership query
 *
 * @return TRUE if the method call was initiated, or FALSE in case of errors
 */
static gboolean xconnman_check_service(void)
{
	gboolean         res  = FALSE;
	DBusMessage     *req  = 0;
	DBusPendingCall *pc   = 0;
	const char      *name = CONNMAN_SERVICE;

	if( !(req = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
					   DBUS_PATH_DBUS,
					   DBUS_INTERFACE_DBUS,
					   "GetNameOwner")) )
		goto EXIT;

	if( !dbus_message_append_args(req,
				      DBUS_TYPE_STRING, &name,
				      DBUS_TYPE_INVALID) )
		goto EXIT;

	if( !dbus_connection_send_with_reply(connman_bus, req, &pc, -1) )
		goto EXIT;

	if( !pc )
		goto EXIT;

	mce_dbus_pending_call_blocks_suspend(pc);

	if( !dbus_pending_call_set_notify(pc, xconnman_check_service_cb, 0, 0) )
		goto EXIT;

	// success
	res = TRUE;

EXIT:
	if( pc )  dbus_pending_call_unref(pc);
	if( req ) dbus_message_unref(req);

	return res;
}

/** Handle connman dbus service name ownership change signals
 *
 * @param msg org.freedesktop.DBus.NameOwnerChanged D-Bus signal
 */
static void xconnman_handle_name_owner_change(DBusMessage *msg)
{
	const char *name = 0;
	const char *prev = 0;
	const char *curr = 0;
	DBusError   err  = DBUS_ERROR_INIT;

	if( !dbus_message_get_args(msg, &err,
				   DBUS_TYPE_STRING, &name,
				   DBUS_TYPE_STRING, &prev,
				   DBUS_TYPE_STRING, &curr,
				   DBUS_TYPE_INVALID) )
	{
		mce_log(LL_WARN, "%s: %s", err.name, err.message);
		goto EXIT;
	}

	if( strcmp(name, CONNMAN_SERVICE) )
		goto EXIT;

	xconnman_set_runstate(curr && *curr);

EXIT:
	dbus_error_free(&err);
	return;
}

/** D-Bus message filter for handling connman related signals
 *
 * @param con       dbus connection
 * @param msg       message to be acted upon
 * @param user_data (not used)
 *
 * @return DBUS_HANDLER_RESULT_NOT_YET_HANDLED (other filters see the msg too)
 */
static DBusHandlerResult xconnman_dbus_filter_cb(DBusConnection *con,
						 DBusMessage *msg,
						 void *user_data)
{
	(void)user_data;

	DBusHandlerResult res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if( con != connman_bus )
		goto EXIT;

	if( dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL )
		goto EXIT;

	if( dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS,
				   DBUS_NAME_OWNER_CHANGED_SIG) ) {
		xconnman_handle_name_owner_change(msg);
	}
	else if( dbus_message_is_signal(msg, CONNMAN_INTERFACE,
					CONNMAN_PROPERTY_CHANGED_SIG) ) {
		xconnman_handle_property_changed_signal(msg);
	}

EXIT:
	return res;
}

/** Stop connman OfflineMode property mirroring
 */
static void xconnman_quit(void)
{
	if( connman_bus ) {
		dbus_connection_remove_filter(connman_bus,
					      xconnman_dbus_filter_cb, 0);

		dbus_bus_remove_match(connman_bus, xconnman_prop_change_rule, 0);
		dbus_bus_remove_match(connman_bus, xconnman_name_owner_rule, 0);

		dbus_connection_unref(connman_bus), connman_bus = 0;
	}
}

/** Start mirroring connman OfflineMode property
 */
static gboolean xconnman_init(void)
{
	gboolean ack = FALSE;

	if( !(connman_bus = dbus_connection_get()) ) {
		mce_log(LL_WARN, "mce has no dbus connection");
		goto EXIT;
	}

	dbus_connection_add_filter(connman_bus, xconnman_dbus_filter_cb, 0, 0);

	dbus_bus_add_match(connman_bus, xconnman_prop_change_rule, 0);
	dbus_bus_add_match(connman_bus, xconnman_name_owner_rule, 0);

	ack = TRUE;

	if( !xconnman_check_service() )
		ack = FALSE;

EXIT:
	return ack;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t radiostates_dbus_handlers[] =
{
	/* signals - outbound (for Introspect purposes only) */
	{
		.interface  = MCE_SIGNAL_IF,
		.name       = MCE_RADIO_STATES_SIG,
		.type       = DBUS_MESSAGE_TYPE_SIGNAL,
		.args       =
			"    <arg name=\"radio_states\" type=\"u\"/>\n"
	},
	/* method calls */
	{
		.interface  = MCE_REQUEST_IF,
		.name       = MCE_RADIO_STATES_GET,
		.type       = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback   = get_radio_states_dbus_cb,
		.args       =
			"    <arg direction=\"out\" name=\"radio_states\" type=\"u\"/>\n"
	},
	{
		.interface  = MCE_REQUEST_IF,
		.name       = MCE_RADIO_STATES_CHANGE_REQ,
		.type       = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback   = req_radio_states_change_dbus_cb,
		.privileged = true,
		.args       =
			"    <arg direction=\"in\" name=\"radio_states\" type=\"u\"/>\n"
			"    <arg direction=\"in\" name=\"states_to_chage\" type=\"u\"/>\n"
	},
	/* sentinel */
	{
		.interface = 0
	}
};

/** Add dbus handlers
 */
static void mce_radiostates_init_dbus(void)
{
	mce_dbus_handler_register_array(radiostates_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mce_radiostates_quit_dbus(void)
{
	mce_dbus_handler_unregister_array(radiostates_dbus_handlers);
}

/** Array of datapipe handlers */
static datapipe_handler_t mce_radiostates_datapipe_handlers[] =
{
	// output triggers
	{
		.datapipe  = &master_radio_enabled_pipe,
		.output_cb = master_radio_enabled_trigger,
	},
	// sentinel
	{
		.datapipe = 0,
	}
};

static datapipe_bindings_t mce_radiostates_datapipe_bindings =
{
	.module   = "mce_radiostates",
	.handlers = mce_radiostates_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void mce_radiostates_datapipe_init(void)
{
	mce_datapipe_init_bindings(&mce_radiostates_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void mce_radiostates_datapipe_quit(void)
{
	mce_datapipe_quit_bindings(&mce_radiostates_datapipe_bindings);
}

/**
 * Init function for the radio states module
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

	/* If we fail to restore the radio states, default to offline */
	if( !restore_radio_states(&active_radio_states, &radio_states) &&
	    !restore_default_radio_states(&active_radio_states, &radio_states) ) {
		active_radio_states = radio_states = 0;
	}

	mce_log(LL_DEBUG, "active_radio_states: %lx, radio_states: %lx",
		active_radio_states, radio_states);

	/* Append triggers/filters to datapipes */
	mce_radiostates_datapipe_init();

	/* Add dbus handlers */
	mce_radiostates_init_dbus();

	if( !xconnman_init() )
		mce_log(LL_WARN, "failed to set up connman mirroring");

	return NULL;
}

/**
 * Exit function for the radio states module
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
	mce_radiostates_quit_dbus();

	xconnman_quit();

	/* Remove triggers/filters from datapipes */
	mce_radiostates_datapipe_quit();

	return;
}
