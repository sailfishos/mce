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

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * RADIO_STATES
 * ------------------------------------------------------------------------- */

static const char  *radio_states_change_repr(guint prev, guint curr);
static const char  *radio_states_repr       (guint state);

/* ------------------------------------------------------------------------- *
 * MRS
 * ------------------------------------------------------------------------- */

static guint     mrs_get_default_radio_states            (void);
static void      mrs_restore_radio_states                (void);
static void      mrs_save_radio_states                   (void);
static void      mrs_modify_radio_states                 (const guint states, const guint mask);
static void      mrs_sync_radio_state                    (void);
static gboolean  mrs_radio_state_sync_cb                 (gpointer aptr);
static void      mrs_cancel_radio_state_sync             (void);
static void      mrs_schedule_radio_state_sync           (void);
static void      mrs_datapipe_update_master_radio_enabled(void);
static void      mrs_datapipe_master_radio_enabled_cb    (gconstpointer data);
static void      mrs_datapipe_init                       (void);
static void      mrs_datapipe_quit                       (void);

/* ------------------------------------------------------------------------- *
 * MRS_DBUS
 * ------------------------------------------------------------------------- */

static gboolean  mrs_dbus_send_radio_states  (DBusMessage *const method_call);
static gboolean  mrs_dbus_get_radio_states_cb(DBusMessage *const msg);
static gboolean  mrs_dbus_set_radio_states_cb(DBusMessage *const msg);
static void      mrs_dbus_init               (void);
static void      mrs_dbus_quit               (void);

/* ------------------------------------------------------------------------- *
 * XCONNMAN
 * ------------------------------------------------------------------------- */

static void               xconnman_set_property_cb               (DBusPendingCall *pc, void *user_data);
static gboolean           xconnman_set_property_bool             (const char *key, gboolean val);
static void               xconnman_sync_offline_to_master        (void);
static void               xconnman_sync_master_to_offline        (void);
static void               xconnman_property_changed              (const char *key, int type, const dbus_any_t *val);
static void               xconnman_handle_property_changed_signal(DBusMessage *msg);
static void               xconnman_get_properties_cb             (DBusPendingCall *pc, void *user_data);
static gboolean           xconnman_get_properties                (void);
static void               xconnman_set_runstate                  (gboolean running);
static void               xconnman_check_service_cb              (DBusPendingCall *pc, void *user_data);
static gboolean           xconnman_check_service                 (void);
static void               xconnman_handle_name_owner_change      (DBusMessage *msg);
static DBusHandlerResult  xconnman_dbus_filter_cb                (DBusConnection *con, DBusMessage *msg, void *user_data);
static void               xconnman_quit                          (void);
static gboolean           xconnman_init                          (void);

/* ------------------------------------------------------------------------- *
 * G_MODULE
 * ------------------------------------------------------------------------- */

const gchar  *g_module_check_init(GModule *module);
void          g_module_unload    (GModule *module);

/* ========================================================================= *
 * Data
 * ========================================================================= */

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

/** Short names - keep in sync with mcetool */
static const char * const radio_state_repr[RADIO_STATES_COUNT] = {
	"master",
	"cellular",
	"wlan",
	"bluetooth",
	"nfc",
	"fmtx",
};

/** radio state flag */
static const guint radio_state_flags[RADIO_STATES_COUNT] = {
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

/** Copy of radio states from master disable time */
static guint saved_radio_states = 0;

/** Active radio states (master switch disables all radios) */
static guint active_radio_states = 0;

/* ========================================================================= *
 * RADIO_STATES
 * ========================================================================= */

/** Translate radio state bitmap change into human readable form
 *
 * @param prev previously active radio states
 * @param curr currently active radio states
 *
 * @return human readable bitmap change description
 */
static const char *
radio_states_change_repr(guint prev, guint curr)
{
	static char repr[128];
	char *pos = repr;
	char *end = repr + sizeof repr - 1;

	auto void add(const char *str) {
		if( !str )
			str = "(null)";
		while( *str && pos < end )
			*pos++ = *str++;
	}

	guint diff = prev ^ curr;

	for( int i = 0; i < RADIO_STATES_COUNT; ++i ) {
		guint mask = radio_state_flags[i];
		if( (diff | curr) & mask ) {
			if( diff & mask )
				add((curr & mask) ? "+" : "-");
			add(radio_state_repr[i]);
			add(" ");
		}
	}

	if( pos > repr )
		--pos;
	else
		add("(none)");

	*pos = 0;

	return repr;
}

/** Translate radio state bitmap into human readable form
 *
 * @param state active radio states
 *
 * @return human readable bitmap description
 */
static const char *
radio_states_repr(guint state)
{
	return radio_states_change_repr(state, state);
}

/* ========================================================================= *
 * MRS
 * ========================================================================= */

/** Get default radio states from customisable settings
 *
 * @return radio states
 */
static guint
mrs_get_default_radio_states(void)
{
	guint default_radio_states = 0;

	for( size_t i = 0; i < RADIO_STATES_COUNT; ++i ) {
		gboolean flag = mce_conf_get_bool(MCE_CONF_RADIO_STATES_GROUP,
						  radio_state_names[i],
						  radio_state_defaults[i]);
		if( flag ) {
			default_radio_states |= radio_state_flags[i];
		}
	}

	mce_log(LL_DEBUG, "default_radio_states = %s",
		radio_states_repr(default_radio_states));

	return default_radio_states;
}

/** Restore the radio states from persistant storage
 */
static void
mrs_restore_radio_states(void)
{
	static const char online_file[]  = MCE_ONLINE_RADIO_STATES_PATH;
	static const char offline_file[] = MCE_OFFLINE_RADIO_STATES_PATH;

	/* Apply configured defaults */
	active_radio_states = saved_radio_states = mrs_get_default_radio_states();

	/* FIXME: old maemo backup/restore handling - can be removed? */
	if( mce_are_settings_locked() ) {
		if( mce_unlock_settings() )
			mce_log(LL_INFO, "Removed stale settings lockfile");
		else
			mce_log(LL_ERR, "Failed to remove settings lockfile; %m");
	}

	/* The files get generated by mce on 1st bootup. Skip the
	 * read attempt and associated diagnostic logging if the
	 * files do not exist */
	if( access(online_file, F_OK) == -1 && errno == ENOENT )
		goto EXIT;

	gulong online_states  = 0;
	gulong offline_states = 0;

	if( mce_read_number_string_from_file(online_file, &online_states,
					     NULL, TRUE, TRUE) )
		active_radio_states = (guint)online_states;

	if( mce_read_number_string_from_file(offline_file, &offline_states,
					     NULL, TRUE, TRUE) )
		saved_radio_states  = (guint)offline_states;

EXIT:
	mce_log(LL_DEBUG, "active_radio_states: %s",
		radio_states_repr(active_radio_states));
	mce_log(LL_DEBUG, "saved_radio_states: %s",
		radio_states_repr(saved_radio_states));
	return;
}

/** Save the radio states to persistant storage
 */
static void
mrs_save_radio_states(void)
{
	const guint online_states  = active_radio_states;
	const guint offline_states = saved_radio_states;

	/* FIXME: old maemo backup/restore handling - can be removed? */
	if (mce_are_settings_locked() == TRUE) {
		mce_log(LL_WARN,
			"Cannot save radio states; backup/restore "
			"or device clear/factory reset pending");
		goto EXIT;
	}

	mce_write_number_string_to_file_atomic(MCE_ONLINE_RADIO_STATES_PATH,
					       online_states);
	mce_write_number_string_to_file_atomic(MCE_OFFLINE_RADIO_STATES_PATH,
					       offline_states);

EXIT:
	return;
}

/** Set the radio states
 *
 * @param states The raw radio states
 * @param mask   The raw radio states mask
 */
static void
mrs_modify_radio_states(const guint states, const guint mask)
{
	mce_log(LL_DEBUG, "states: %s",
		radio_states_change_repr(states ^ mask, states));

	guint prev = active_radio_states;

	/* Deal with master bit changes first */
	if( (mask & MCE_RADIO_STATE_MASTER) &&
	    ((active_radio_states ^ states) & MCE_RADIO_STATE_MASTER) ) {
		if( active_radio_states & MCE_RADIO_STATE_MASTER ) {
			/* Master disable: save & clear state */
			saved_radio_states = active_radio_states;
			active_radio_states = 0;
		}
		else {
			/* Master enable: resture saved state */
			active_radio_states = saved_radio_states;
		}
	}

	/* Then update active features bits */
	active_radio_states = (active_radio_states & ~mask) | (states & mask);

	/* Immediate actions on state change */
	if( prev != active_radio_states ) {
		mce_log(LL_DEBUG, "active_radio_states: %s",
			radio_states_change_repr(prev, active_radio_states));

		/* Update persistent values */
		mrs_save_radio_states();

		/* Broadcast changes */
		mrs_dbus_send_radio_states(NULL);
	}

	/* Do datapipe & connman sync from idle callback */
	mrs_schedule_radio_state_sync();
}

/** Immediately sync active radio state to datapipes and connman
 */
static void
mrs_sync_radio_state(void)
{
	/* Remove pending timer */
	mrs_cancel_radio_state_sync();

	/* Sync to master_radio_enabled_pipe */
	mrs_datapipe_update_master_radio_enabled();

	/* After datapipe execution the radio state should
	 * be stable - sync connman offline property to it */
	xconnman_sync_master_to_offline();
}

/** Timer id for: delayed radio state sync
 */
static guint mrs_radio_state_sync_id = 0;

/** Timer callback for: delayed radio state sync
 */
static gboolean
mrs_radio_state_sync_cb(gpointer aptr)
{
	(void)aptr;
	mrs_radio_state_sync_id = 0;
	mrs_sync_radio_state();
	return G_SOURCE_REMOVE;
}

/** Cancel delayed radio state sync
 */
static void
mrs_cancel_radio_state_sync(void)
{
	if( mrs_radio_state_sync_id ) {
		g_source_remove(mrs_radio_state_sync_id),
			mrs_radio_state_sync_id = 0;
	}
}

/** Schedule delayed radio state sync
 */
static void
mrs_schedule_radio_state_sync(void)
{
	if( !mrs_radio_state_sync_id ) {
		mrs_radio_state_sync_id =
			g_idle_add(mrs_radio_state_sync_cb, 0);
	}
}

/* ========================================================================= *
 * MRS_DATAPIPES
 * ========================================================================= */

/** Sync active radio state -> master_radio_enabled_pipe
 */
static void
mrs_datapipe_update_master_radio_enabled(void)
{
	int prev = datapipe_get_gint(master_radio_enabled_pipe);
	int next = (active_radio_states & MCE_RADIO_STATE_MASTER) ? 1 : 0;

	if( prev != next )
		datapipe_exec_full(&master_radio_enabled_pipe, GINT_TO_POINTER(next));
}

/** Sync master_radio_enabled_pipe -> active radio state
 *
 * @param data master radio state as a pointer
 */
static void
mrs_datapipe_master_radio_enabled_cb(gconstpointer data)
{
	guint prev = (active_radio_states & MCE_RADIO_STATE_MASTER);
	guint next = data ? MCE_RADIO_STATE_MASTER : 0;

	if( prev != next )
		mrs_modify_radio_states(next, MCE_RADIO_STATE_MASTER);
}

/** Array of datapipe handlers
 */
static datapipe_handler_t mrs_datapipe_handlers[] =
{
	// output triggers
	{
		.datapipe  = &master_radio_enabled_pipe,
		.output_cb = mrs_datapipe_master_radio_enabled_cb,
	},
	// sentinel
	{
		.datapipe = 0,
	}
};

/** Datapipe bindings for this module
 */
static datapipe_bindings_t mrs_datapipe_bindings =
{
	.module   = MODULE_NAME,
	.handlers = mrs_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void mrs_datapipe_init(void)
{
	mce_datapipe_init_bindings(&mrs_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void mrs_datapipe_quit(void)
{
	mce_datapipe_quit_bindings(&mrs_datapipe_bindings);
}

/* ========================================================================= *
 * MRS_DBUS
 * ========================================================================= */

/** Send the radio states
 *
 * @param method_call A DBusMessage to reply to;
 *                    pass NULL to send a signal instead
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean
mrs_dbus_send_radio_states(DBusMessage *const method_call)
{
	static dbus_uint32_t prev = ~0u;

	DBusMessage *msg = NULL;
	gboolean status = FALSE;

	dbus_uint32_t data = active_radio_states;

	if( method_call ) {
		/* Send reply to a method call */
		msg = dbus_new_method_reply(method_call);
	}
	else if( prev == data ) {
		/* Skip duplicate signals */
		goto EXIT;
	}
	else {
		/* Broadcast change signal */
		prev = data;
		msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
				      MCE_RADIO_STATES_SIG);
	}

	mce_log(LL_DEBUG, "Sending radio states %s: %s",
		method_call ? "reply" : "signal",
		radio_states_repr(data));

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

/** D-Bus callback for the get radio states method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean
mrs_dbus_get_radio_states_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	mce_log(LL_DEVEL, "Received get radio states request from %s",
		mce_dbus_get_message_sender_ident(msg));

	/* Try to send a reply that contains the current radio states */
	if (mrs_dbus_send_radio_states(msg) == FALSE)
		goto EXIT;

	status = TRUE;

EXIT:
	return status;
}

/** D-Bus callback for radio states change method call
 *
 * @todo Decide on error handling policy
 *
 * @param msg The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean mrs_dbus_set_radio_states_cb(DBusMessage *const msg)
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

	mrs_modify_radio_states(states, mask);

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

/** Array of dbus message handlers
 */
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
			.callback   = mrs_dbus_get_radio_states_cb,
			.args       =
			"    <arg direction=\"out\" name=\"radio_states\" type=\"u\"/>\n"
	},
	{
		.interface  = MCE_REQUEST_IF,
			.name       = MCE_RADIO_STATES_CHANGE_REQ,
			.type       = DBUS_MESSAGE_TYPE_METHOD_CALL,
			.callback   = mrs_dbus_set_radio_states_cb,
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
static void
mrs_dbus_init(void)
{
	mce_dbus_handler_register_array(radiostates_dbus_handlers);
}

/** Remove dbus handlers
 */
static void
mrs_dbus_quit(void)
{
	mce_dbus_handler_unregister_array(radiostates_dbus_handlers);
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
static guint connman_master = ~0lu;

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
		mrs_modify_radio_states(connman_master, MCE_RADIO_STATE_MASTER);
	}
}

/** Synchronize MCE master radio state -> connman OfflineMode
 */
static void xconnman_sync_master_to_offline(void)
{
	guint master;

	if( !connman_running )
		return;

	master = active_radio_states & MCE_RADIO_STATE_MASTER;

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

/* ========================================================================= *
 * G_MODULE
 * ========================================================================= */

/** Init function for the radio states module
 *
 * @param module (Unused)
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *
g_module_check_init(GModule *module)
{
	(void)module;

	/* Read persistent values */
	mrs_restore_radio_states();

	/* Append triggers/filters to datapipes */
	mrs_datapipe_init();

	/* Add dbus handlers */
	mrs_dbus_init();

	if( !xconnman_init() )
		mce_log(LL_WARN, "failed to set up connman mirroring");

	/* Process and broadcast initial state */
	mrs_datapipe_update_master_radio_enabled();
	mrs_dbus_send_radio_states(NULL);

	return NULL;
}

/** Exit function for the radio states module
 *
 * @param module (Unused)
 */
G_MODULE_EXPORT void
g_module_unload(GModule *module)
{
	(void)module;

	/* Remove dbus handlers */
	mrs_dbus_quit();

	xconnman_quit();

	/* Remove triggers/filters from datapipes */
	mrs_datapipe_quit();

	mrs_cancel_radio_state_sync();

	return;
}
