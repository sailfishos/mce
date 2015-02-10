/**
 * @file mce-dsme.c
 * Interface code and logic between
 * DSME (the Device State Management Entity)
 * and MCE (the Mode Control Entity)
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Ismo Laitinen <ismo.laitinen@nokia.com>
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

#include "mce-dsme.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-lib.h"
#include "mce-conf.h"
#include "mce-dbus.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <dsme/state.h>
#include <dsme/protocol.h>
#include <dsme/processwd.h>

/** Well known dbus name of dsme */
#define DSME_DBUS_SERVICE "com.nokia.dsme"

/** Charger state; from charger_state_pipe */
static charger_state_t charger_state = CHARGER_STATE_UNDEF;

/** Availability of dsme; from dsme_available_pipe */
static bool dsme_available = false;

/** Pointer to the dsmesock connection */
static dsmesock_connection_t *mce_dsme_connection = NULL;

/** I/O watch for mce_dsme_connection */
static guint mce_dsme_iowatch_id = 0;

/** ID for delayed state transition reporting timer */
static guint mce_dsme_state_report_id = 0;

/** Soft poweroff charger connect policy */
static gint softoff_charger_connect_policy = DEFAULT_SOFTOFF_CHARGER_CONNECT;

/** Mapping of soft poweroff charger connect integer <-> policy string */
static const mce_translation_t soft_poweroff_charger_connect_translation[] = {
	{
		.number = SOFTOFF_CHARGER_CONNECT_WAKEUP,
		.string = SOFTOFF_CHARGER_CONNECT_WAKEUP_STR
	}, {
		.number = SOFTOFF_CHARGER_CONNECT_IGNORE,
		.string = SOFTOFF_CHARGER_CONNECT_IGNORE_STR
	}, { /* MCE_INVALID_TRANSLATION marks the end of this array */
		.number = MCE_INVALID_TRANSLATION,
		.string = NULL
	}
};

/* Internal functions */

static bool           mce_dsme_send(gpointer msg, const char *request_name);
static void           mce_dsme_send_pong(void);
static void           mce_dsme_init_processwd(void);
static void           mce_dsme_exit_processwd(void);
static void           mce_dsme_query_system_state(void);

static gboolean       mce_dsme_state_report_cb(gpointer data);
static void           mce_dsme_cancel_state_report(void);
static void           mce_dsme_schedule_state_report(void);

static system_state_t mce_dsme_normalise_system_state(dsme_state_t dsmestate);

static gboolean       mce_dsme_iowatch_cb(GIOChannel *source, GIOCondition condition, gpointer data);
static gboolean       mce_dsme_init_done_cb(DBusMessage *const msg);
static void           mce_dsme_charger_state_cb(gconstpointer const data);
static void           mce_dsme_dsme_available_cb(gconstpointer const data);

static bool           mce_dsme_connect(void);
static void           mce_dsme_disconnect(void);
static void           mce_dsme_reconnect(void);

static void           mce_dsme_set_availability(bool available);

static void           mce_dsme_query_name_owner_cb(DBusPendingCall *pc, void *user_data);
static bool           mce_dsme_query_name_owner(void);
static gboolean       mce_dsme_name_owner_changed_cb(DBusMessage *const msg);

static void           mce_dsme_init_dbus(void);
static void           mce_dsme_quit_dbus(void);
static void           mce_dsme_init_config(void);
static void           mce_dsme_init_datapipes(void);
static void           mce_dsme_quit_datapipes(void);

/**
 * Generic send function for dsmesock messages
 *
 * @param msg A pointer to the message to send
 */
static bool mce_dsme_send(gpointer msg, const char *request_name)
{
	bool res = false;

	if( !mce_dsme_connection ) {
		mce_log(LL_WARN, "failed to send %s to dsme; %s",
			request_name, "not connected");
		goto EXIT;
	}

	if( dsmesock_send(mce_dsme_connection, msg) == -1) {
		mce_log(LL_ERR, "failed to send %s to dsme; %m",
			request_name);

		/* close and try to re-connect */
		mce_dsme_reconnect();
		goto EXIT;
	}

	mce_log(LL_DEBUG, "%s sent to DSME", request_name);

	res = true;

EXIT:
	return res;
}

/**
 * Send pong message to the DSME process watchdog
 */
static void mce_dsme_send_pong(void)
{
	/* Set up the message */
	DSM_MSGTYPE_PROCESSWD_PONG msg =
	  DSME_MSG_INIT(DSM_MSGTYPE_PROCESSWD_PONG);
	msg.pid = getpid();

	/* Send the message */
	mce_dsme_send(&msg, "DSM_MSGTYPE_PROCESSWD_PONG");

	/* Execute hearbeat actions even if ping-pong ipc failed */
	execute_datapipe(&heartbeat_pipe, GINT_TO_POINTER(0),
			 USE_INDATA, DONT_CACHE_INDATA);
}

/**
 * Register to DSME process watchdog
 */
static void mce_dsme_init_processwd(void)
{
	/* Set up the message */
	DSM_MSGTYPE_PROCESSWD_CREATE msg =
	  DSME_MSG_INIT(DSM_MSGTYPE_PROCESSWD_CREATE);
	msg.pid = getpid();

	/* Send the message */
	mce_dsme_send(&msg, "DSM_MSGTYPE_PROCESSWD_CREATE");
}

/**
 * Unregister from DSME process watchdog
 */
static void mce_dsme_exit_processwd(void)
{
	mce_log(LL_DEBUG,
		"Disabling DSME process watchdog");

	/* Set up the message */
	DSM_MSGTYPE_PROCESSWD_DELETE msg =
	  DSME_MSG_INIT(DSM_MSGTYPE_PROCESSWD_DELETE);
	msg.pid = getpid();

	/* Send the message */
	mce_dsme_send(&msg, "DSM_MSGTYPE_PROCESSWD_DELETE");
}

/**
 * Send system state inquiry
 */
static void mce_dsme_query_system_state(void)
{
	/* Set up the message */
	DSM_MSGTYPE_STATE_QUERY msg = DSME_MSG_INIT(DSM_MSGTYPE_STATE_QUERY);

	/* Send the message */
	mce_dsme_send(&msg, "DSM_MSGTYPE_STATE_QUERY");
}

/**
 * Request powerup
 */
void mce_dsme_request_powerup(void)
{
	/* Set up the message */
	DSM_MSGTYPE_POWERUP_REQ msg = DSME_MSG_INIT(DSM_MSGTYPE_POWERUP_REQ);

	/* Send the message */
	mce_dsme_send(&msg, "DSM_MSGTYPE_POWERUP_REQ");
}

/**
 * Request reboot
 */
void mce_dsme_request_reboot(void)
{
	if( datapipe_get_gint(update_mode_pipe) ) {
		mce_log(LL_WARN, "reboot blocked; os update in progress");
		goto EXIT;
	}

	/* Set up the message */
	DSM_MSGTYPE_REBOOT_REQ msg = DSME_MSG_INIT(DSM_MSGTYPE_REBOOT_REQ);

	/* Send the message */
	mce_dsme_send(&msg, "DSM_MSGTYPE_REBOOT_REQ");
EXIT:
	return;
}

/**
 * Request soft poweron
 */
void mce_dsme_request_soft_poweron(void)
{
	/* Disable the soft poweroff LED pattern */
	execute_datapipe_output_triggers(&led_pattern_deactivate_pipe,
					 MCE_LED_PATTERN_DEVICE_SOFT_OFF,
					 USE_INDATA);

	mce_rem_submode_int32(MCE_SOFTOFF_SUBMODE);
	execute_datapipe(&display_state_req_pipe,
			 GINT_TO_POINTER(MCE_DISPLAY_ON),
			 USE_INDATA, CACHE_INDATA);
}

/**
 * Request soft poweroff
 */
void mce_dsme_request_soft_poweroff(void)
{
	mce_add_submode_int32(MCE_SOFTOFF_SUBMODE);
	execute_datapipe(&display_state_req_pipe,
			 GINT_TO_POINTER(MCE_DISPLAY_LPM_OFF),
			 USE_INDATA, CACHE_INDATA);

	/* Enable the soft poweroff LED pattern */
	execute_datapipe_output_triggers(&led_pattern_activate_pipe,
					 MCE_LED_PATTERN_DEVICE_SOFT_OFF,
					 USE_INDATA);
}

/**
 * Timeout callback for transition
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean mce_dsme_state_report_cb(gpointer data)
{
	(void)data;

	mce_dsme_state_report_id = 0;

	mce_rem_submode_int32(MCE_TRANSITION_SUBMODE);

	return FALSE;
}

/**
 * Cancel state transition timeout
 */
static void mce_dsme_cancel_state_report(void)
{
	/* Remove the timeout source for state transitions */
	if (mce_dsme_state_report_id != 0) {
		g_source_remove(mce_dsme_state_report_id);
		mce_dsme_state_report_id = 0;
	}
}

/**
 * Setup state transition timeout
 */
static void mce_dsme_schedule_state_report(void)
{
	mce_dsme_cancel_state_report();

#if TRANSITION_DELAY > 0
	/* Setup new timeout */
	mce_dsme_state_report_id =
		g_timeout_add(TRANSITION_DELAY, mce_dsme_state_report_cb, NULL);
#elif TRANSITION_DELAY == 0
	/* Set up idle callback */
	mce_dsme_state_report_id =
		g_idle_add(mce_dsme_state_report_cb, NULL);
#else
	/* Trigger immediately */
	mce_dsme_state_report_cb(0);
#endif
}

/**
 * Request normal shutdown
 */
void mce_dsme_request_normal_shutdown(void)
{
	if( datapipe_get_gint(update_mode_pipe) ) {
		mce_log(LL_WARN, "shutdown blocked; os update in progress");
		goto EXIT;
	}

	/* Set up the message */
	DSM_MSGTYPE_SHUTDOWN_REQ msg = DSME_MSG_INIT(DSM_MSGTYPE_SHUTDOWN_REQ);

	/* Send the message */
	mce_dsme_send(&msg, "DSM_MSGTYPE_SHUTDOWN_REQ(DSME_NORMAL_SHUTDOWN)");
EXIT:
	return;
}

/**
 * Convert DSME dsme state
 * to a state enum that we can export to datapipes
 *
 * @param dsmestate The DSME dsme_state_t with the value to convert
 * @return the converted value
 */
static system_state_t mce_dsme_normalise_system_state(dsme_state_t dsmestate)
{
	system_state_t state = MCE_STATE_UNDEF;

	switch (dsmestate) {
	case DSME_STATE_SHUTDOWN:
		state = MCE_STATE_SHUTDOWN;
		break;

	case DSME_STATE_USER:
		state = MCE_STATE_USER;
		break;

	case DSME_STATE_ACTDEAD:
		state = MCE_STATE_ACTDEAD;
		break;

	case DSME_STATE_REBOOT:
		state = MCE_STATE_REBOOT;
		break;

	case DSME_STATE_BOOT:
		state = MCE_STATE_BOOT;
		break;

	case DSME_STATE_NOT_SET:
		break;

	case DSME_STATE_TEST:
		mce_log(LL_WARN,
			"Received DSME_STATE_TEST; treating as undefined");
		break;

	case DSME_STATE_MALF:
		mce_log(LL_WARN,
			"Received DSME_STATE_MALF; treating as undefined");
		break;

	case DSME_STATE_LOCAL:
		mce_log(LL_WARN,
			"Received DSME_STATE_LOCAL; treating as undefined");
		break;

	default:
		mce_log(LL_ERR,
			"Received an unknown state from DSME; "
			"treating as undefined");
		break;
	}

	return state;
}

/**
 * Callback for pending I/O from dsmesock
 *
 * XXX: is the error policy reasonable?
 *
 * @param source Unused
 * @param condition Unused
 * @param data Unused
 * @return TRUE on success, FALSE on failure
 */
static gboolean mce_dsme_iowatch_cb(GIOChannel *source,
				    GIOCondition condition,
				    gpointer data)
{
	gboolean keep_going = TRUE;
	dsmemsg_generic_t *msg = 0;

	DSM_MSGTYPE_STATE_CHANGE_IND *msg2;
	system_state_t oldstate = datapipe_get_gint(system_state_pipe);
	system_state_t newstate = MCE_STATE_UNDEF;

	(void)source;
	(void)data;

	if( condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) ) {
		mce_log(LL_CRIT, "DSME socket hangup/error");
		keep_going = FALSE;
		goto EXIT;
	}

	if( !(msg = dsmesock_receive(mce_dsme_connection)) )
		goto EXIT;

	if( DSMEMSG_CAST(DSM_MSGTYPE_CLOSE, msg) ) {
		mce_log(LL_WARN, "DSME socket closed");
		keep_going = FALSE;
	}
	else if( DSMEMSG_CAST(DSM_MSGTYPE_PROCESSWD_PING, msg) ) {
		mce_dsme_send_pong();
	}
	else if( (msg2 = DSMEMSG_CAST(DSM_MSGTYPE_STATE_CHANGE_IND, msg)) ) {
		newstate = mce_dsme_normalise_system_state(msg2->state);
		mce_log(LL_DEVEL, "DSME device state change: %d", newstate);

		/* If we're changing to a different state,
		 * add the transition flag, UNLESS the old state
		 * was MCE_STATE_UNDEF
		 */
		if ((oldstate != newstate) && (oldstate != MCE_STATE_UNDEF))
			mce_add_submode_int32(MCE_TRANSITION_SUBMODE);

		switch (newstate) {
		case MCE_STATE_USER:
			execute_datapipe_output_triggers(&led_pattern_activate_pipe, MCE_LED_PATTERN_DEVICE_ON, USE_INDATA);
			break;

		case MCE_STATE_ACTDEAD:
		case MCE_STATE_BOOT:
		case MCE_STATE_UNDEF:
			break;

		case MCE_STATE_SHUTDOWN:
		case MCE_STATE_REBOOT:
			execute_datapipe_output_triggers(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_DEVICE_ON, USE_INDATA);
			break;

		default:
			break;
		}

		mce_log(LL_DEVEL, "system_state: %s -> %s",
			system_state_repr(oldstate),
			system_state_repr(newstate));

		execute_datapipe(&system_state_pipe,
				 GINT_TO_POINTER(newstate),
				 USE_INDATA, CACHE_INDATA);
	}
	else {
		mce_log(LL_DEBUG, "Unknown message type (%x) received from DSME!",
			msg->type_); /* <- unholy access of a private member */
	}

EXIT:
	free(msg);

	if( !keep_going ) {
		mce_log(LL_WARN, "DSME i/o notifier disabled;"
			" trying to reconnect");

		/* mark notifier as removed */
		mce_dsme_iowatch_id = 0;

		/* close and try to re-connect */
		mce_dsme_reconnect();
	}

	return keep_going;
}

/**
 * D-Bus callback for the init done notification signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mce_dsme_init_done_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	(void)msg;

	mce_log(LL_DEVEL, "Received init done notification");

	if ((mce_get_submode_int32() & MCE_TRANSITION_SUBMODE)) {
		mce_dsme_schedule_state_report();
	}

	status = TRUE;

//EXIT:
	return status;
}

/**
 * Datapipe trigger for the charger state
 *
 * @param data TRUE if the charger was connected,
 *	       FALSE if the charger was disconnected
 */
static void mce_dsme_charger_state_cb(gconstpointer const data)
{
	submode_t submode = mce_get_submode_int32();

	charger_state = GPOINTER_TO_INT(data);

	if ((submode & MCE_SOFTOFF_SUBMODE) != 0) {
		if (softoff_charger_connect_policy == SOFTOFF_CHARGER_CONNECT_WAKEUP) {
			mce_dsme_request_soft_poweron();
		}
	}
}

/** Datapipe trigger for dsme availability
 */
static void mce_dsme_dsme_available_cb(gconstpointer const data)
{
	bool prev = dsme_available;
	dsme_available = GPOINTER_TO_INT(data);

	if( dsme_available == prev )
		goto EXIT;

	mce_log(LL_DEVEL, "DSME is %s",
		dsme_available ? "running" : "stopped");

	if( dsme_available )
		mce_dsme_connect();
	else
		mce_dsme_disconnect();

EXIT:
	return;
}

/**
 * Initialise dsmesock connection
 *
 * @return true on success, false on failure
 */
static bool mce_dsme_connect(void)
{
	bool        status = false;
	GIOChannel *iochan = NULL;

	/* Make sure we start from closed state */
	mce_dsme_disconnect();

	mce_log(LL_DEBUG, "Opening DSME socket");

	if( !(mce_dsme_connection = dsmesock_connect()) ) {
		mce_log(LL_ERR, "Failed to open DSME socket");
		goto EXIT;
	}

	mce_log(LL_DEBUG, "Adding DSME socket notifier");

	if( !(iochan = g_io_channel_unix_new(mce_dsme_connection->fd)) ) {
		mce_log(LL_ERR,"Failed to set up I/O channel for DSME socket");
		goto EXIT;
	}

	mce_dsme_iowatch_id =
		g_io_add_watch(iochan,
			       G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
			       mce_dsme_iowatch_cb, NULL);

	/* Query the current system state; if the mainloop isn't running,
	 * this will trigger an update when the mainloop starts
	 */
	mce_dsme_query_system_state();

	/* Register with DSME's process watchdog */
	mce_dsme_init_processwd();

	status = true;

EXIT:
	if( iochan ) g_io_channel_unref(iochan);

	return status;
}

/**
 * Close dsmesock connection
 */
static void mce_dsme_disconnect(void)
{
	if( mce_dsme_iowatch_id ) {
		mce_log(LL_DEBUG, "Removing DSME socket notifier");
		g_source_remove(mce_dsme_iowatch_id);
		mce_dsme_iowatch_id = 0;
	}

	if( mce_dsme_connection ) {
		mce_log(LL_DEBUG, "Closing DSME socket");
		dsmesock_close(mce_dsme_connection);
		mce_dsme_connection = 0;
	}

	// FIXME: should we assume something about the system state?
}

/** Close dsmesock connection and reconnect if/when dsme is available
 */
static void mce_dsme_reconnect(void)
{
	/* set availability to false -> disconnects */
	mce_dsme_set_availability(false);

	/* reconnect if/when dsme has/gets name owner */
	mce_dsme_query_name_owner();
}

/** Feed dsme availability to dsme_available_pipe datapipe
 */
static void mce_dsme_set_availability(bool available)
{
	execute_datapipe(&dsme_available_pipe,
			 GINT_TO_POINTER(available),
			 USE_INDATA, CACHE_INDATA);
}

/** Handle reply to asynchronous dsme service name ownership query
 *
 * @param pc        State data for asynchronous D-Bus method call
 * @param user_data (not used)
 */
static void mce_dsme_query_name_owner_cb(DBusPendingCall *pc, void *user_data)
{
	(void)user_data;

	DBusMessage *rsp   = 0;
	const char  *owner = 0;
	DBusError    err   = DBUS_ERROR_INIT;

	mce_log(LL_DEBUG, "got dsme name owner reply");

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

	mce_dsme_set_availability(owner && *owner);

EXIT:
	if( rsp ) dbus_message_unref(rsp);
	dbus_error_free(&err);
}

/** Initiate asynchronous dsme service name ownership query
 *
 * @return true if the method call was initiated, or false in case of errors
 */
static bool mce_dsme_query_name_owner(void)
{
	bool             res  = false;
	DBusMessage     *req  = 0;
	DBusPendingCall *pc   = 0;
	const char      *name = DSME_DBUS_SERVICE;

	DBusConnection  *bus  = 0;

	mce_log(LL_DEBUG, "start dsme name owner query");

	if( !(bus = dbus_connection_get()) )
		goto EXIT;

	if( !(req = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
						 DBUS_PATH_DBUS,
						 DBUS_INTERFACE_DBUS,
						 "GetNameOwner")) )
		goto EXIT;

	if( !dbus_message_append_args(req,
				      DBUS_TYPE_STRING, &name,
				      DBUS_TYPE_INVALID) )
		goto EXIT;

	if( !dbus_connection_send_with_reply(bus, req, &pc, -1) )
		goto EXIT;

	if( !pc )
		goto EXIT;

	if( !dbus_pending_call_set_notify(pc, mce_dsme_query_name_owner_cb, 0, 0) )
		goto EXIT;

	res = true;

EXIT:
	if( pc )  dbus_pending_call_unref(pc);
	if( req ) dbus_message_unref(req);
	if( bus ) dbus_connection_unref(bus);

	return res;
}

/** Handle name owner changed signals for com.nokia.dsme
 */
static gboolean mce_dsme_name_owner_changed_cb(DBusMessage *const msg)
{
	DBusError   err  = DBUS_ERROR_INIT;
	const char *name = 0;
	const char *prev = 0;
	const char *curr = 0;

	mce_log(LL_DEBUG, "got dsme name owner change");

	if( !msg )
		goto EXIT;

	if( !dbus_message_get_args(msg, &err,
				   DBUS_TYPE_STRING, &name,
				   DBUS_TYPE_STRING, &prev,
				   DBUS_TYPE_STRING, &curr,
				   DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "Failed to parse name owner signal: %s: %s",
			err.name, err.message);
		goto EXIT;
	}

	if( !name || strcmp(name, DSME_DBUS_SERVICE) )
		goto EXIT;

	mce_dsme_set_availability(curr && *curr);

EXIT:
	dbus_error_free(&err);

	return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t mce_dsme_dbus_handlers[] =
{
	/* signals */
	{
		.interface = DBUS_INTERFACE_DBUS,
		.name      = "NameOwnerChanged",
		.rules     = "arg0='"DSME_DBUS_SERVICE"'",
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.callback  = mce_dsme_name_owner_changed_cb,
	},
	{
		.interface = "com.nokia.startup.signal",
		.name      = "init_done",
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.callback  = mce_dsme_init_done_cb,
	},

	/* sentinel */
	{
		.interface = 0
	}
};

/** Add dbus handlers
 */
static void mce_dsme_init_dbus(void)
{
	mce_dbus_handler_register_array(mce_dsme_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mce_dsme_quit_dbus(void)
{
	mce_dbus_handler_unregister_array(mce_dsme_dbus_handlers);
}

/** Get configuration options
 */
static void mce_dsme_init_config(void)
{
	gchar *tmp = mce_conf_get_string(MCE_CONF_SOFTPOWEROFF_GROUP,
					 MCE_CONF_SOFTPOWEROFF_CHARGER_POLICY_CONNECT,
					 "");
	softoff_charger_connect_policy =
		mce_translate_string_to_int_with_default(soft_poweroff_charger_connect_translation,
							 tmp, DEFAULT_SOFTOFF_CHARGER_CONNECT);
	g_free(tmp);
}

/** Append triggers/filters to datapipes
 */
static void mce_dsme_init_datapipes(void)
{
	append_output_trigger_to_datapipe(&charger_state_pipe,
					  mce_dsme_charger_state_cb);

	append_output_trigger_to_datapipe(&dsme_available_pipe,
					  mce_dsme_dsme_available_cb);
}

/** Remove triggers/filters from datapipes
 */
static void mce_dsme_quit_datapipes(void)
{
	remove_output_trigger_from_datapipe(&charger_state_pipe,
					    mce_dsme_charger_state_cb);

	remove_output_trigger_from_datapipe(&dsme_available_pipe,
					    mce_dsme_dsme_available_cb);
}

/**
 * Init function for the mce-dsme component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_dsme_init(void)
{
	mce_dsme_init_config();

	mce_dsme_init_datapipes();

	mce_dsme_init_dbus();

	/* Start async query to check if dsme is already on dbus */
	mce_dsme_query_name_owner();

	return TRUE;
}

/**
 * Exit function for the mce-dsme component
 *
 * @todo D-Bus unregistration
 * @todo trigger unregistration
 */
void mce_dsme_exit(void)
{
	mce_dsme_quit_dbus();

	mce_dsme_exit_processwd();
	mce_dsme_disconnect();

	mce_dsme_quit_datapipes();

	/* Remove all timer sources before returning */
	mce_dsme_cancel_state_report();

	return;
}
