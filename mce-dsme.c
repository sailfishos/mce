/**
 * @file mce-dsme.c
 * Interface code and logic between
 * DSME (the Device State Management Entity)
 * and MCE (the Mode Control Entity)
 * <p>
 * Copyright © 2004-2010 Nokia Corporation and/or its subsidiary(-ies).
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
#include <glib.h>

#include <time.h>			/* time(), time_t */
#include <errno.h>			/* errno */
#include <stdlib.h>			/* exit(), free(), EXIT_FAILURE */
#include <unistd.h>			/* getpid() */

#include <dsme/state.h>			/* dsme_state_t */
#include <dsme/messages.h>		/* DSM_MSGTYPE_* */
#include <dsme/protocol.h>		/* dsmesock_send(),
					 * dsmesock_receive(),
					 * dsmesock_connect(),
					 * dsmesock_close(),
					 * dsmesock_connection_t
					 */
#include <dsme/processwd.h>             /* DSM_MSGTYPE_PROCESSWD_PING,
                                         * DSM_MSGTYPE_PROCESSWD_PONG,
                                         * DSM_MSGTYPE_PROCESSWD_CREATE,
                                         * DSM_MSGTYPE_PROCESSWD_DELETE
                                         */
#include <mce/mode-names.h>
#include "mce.h"			/* mce_add_submode_int32(),
					 * mce_rem_submode_int32(),
					 * mce_get_submode_int32(),
					 * MCE_INVALID_TRANSLATION,
					 * MCE_LED_PATTERN_DEVICE_ON,
					 * MCE_LED_PATTERN_DEVICE_SOFT_OFF,
					 * submode_t,
					 * system_state_t,
					 * MCE_SOFTOFF_SUBMODE,
					 * MCE_TRANSITION_SUBMODE,
					 * mainloop,
					 * charger_state_pipe,
					 * display_state_pipe,
					 * system_state_pipe,
					 * led_pattern_activate_pipe,
					 * led_pattern_deactivate_pipe
					 */
#include "mce-dsme.h"

#include "mce-lib.h"		/* mce_translate_string_to_int_with_default(),
				 * mce_translation_t
				 */
#include "mce-log.h"			/* mce_log(), LL_* */
#include "mce-dbus.h"			/* mce_dbus_handler_add(),
					 * DBUS_MESSAGE_TYPE_SIGNAL
					 */
#include "mce-conf.h"			/* mce_conf_get_string() */
#include "datapipe.h"			/* execute_datapipe(),
					 * execute_datapippe_output_triggers(),
					 * datapipe_get_gint(),
					 * append_output_trigger_to_datapipe(),
					 * remove_output_trigger_from_datapipe()
					 */
#include "connectivity.h"		/* get_connectivity_status() */

/** Charger state */
static gboolean charger_connected = FALSE;

/** Pointer to the dsmesock connection */
static dsmesock_connection_t *dsme_conn = NULL;
/** TRUE if dsme is disabled (for debugging), FALSE if dsme is enabled */
static gboolean dsme_disabled = FALSE;

/** ID for state transition timer source */
static guint transition_timeout_cb_id = 0;

/** Soft poweroff connectivity policy when connected to charger */
static gint softoff_connectivity_policy_charger =
					DEFAULT_SOFTOFF_CONNECTIVITY_CHARGER;

/** Soft poweroff connectivity policy when running on battery */
static gint softoff_connectivity_policy_battery =
					DEFAULT_SOFTOFF_CONNECTIVITY_BATTERY;

/** Soft poweroff connectivity policy on poweron */
static gint softoff_connectivity_policy_poweron =
					DEFAULT_SOFTOFF_CONNECTIVITY_POWERON;

/** Soft poweroff charger connect policy */
static gint softoff_charger_connect_policy =
					DEFAULT_SOFTOFF_CHARGER_CONNECT;

/** Previous master radio state */
static gint previous_radio_state = -1;

/** Mapping of soft poweroff connectivity integer <-> policy string */
static const mce_translation_t soft_poweroff_connectivity_translation[] = {
	{
		.number = SOFTOFF_CONNECTIVITY_RETAIN,
		.string = SOFTOFF_CONNECTIVITY_RETAIN_STR
	}, {
		.number = SOFTOFF_CONNECTIVITY_SOFT_OFFLINE,
		.string = SOFTOFF_CONNECTIVITY_SOFT_OFFLINE_STR
	}, {
		.number = SOFTOFF_CONNECTIVITY_FORCE_OFFLINE,
		.string = SOFTOFF_CONNECTIVITY_FORCE_OFFLINE_STR
	}, { /* MCE_INVALID_TRANSLATION marks the end of this array */
		.number = MCE_INVALID_TRANSLATION,
		.string = NULL
	}
};

/** Mapping of soft poweron connectivity integer <-> policy string */
static const mce_translation_t soft_poweron_connectivity_translation[] = {
	{
		.number = SOFTOFF_CONNECTIVITY_RETAIN,
		.string = SOFTOFF_CONNECTIVITY_RETAIN_STR
	}, {
		.number = SOFTOFF_CONNECTIVITY_SOFT_OFFLINE,
		.string = SOFTOFF_CONNECTIVITY_SOFT_OFFLINE_STR
	}, {
		.number = SOFTOFF_CONNECTIVITY_FORCE_OFFLINE,
		.string = SOFTOFF_CONNECTIVITY_FORCE_OFFLINE_STR
	}, { /* MCE_INVALID_TRANSLATION marks the end of this array */
		.number = MCE_INVALID_TRANSLATION,
		.string = NULL
	}
};

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

/** dsme I/O channel */
static GIOChannel *dsme_iochan = NULL;
/** dsme data channel GSource ID */
static guint dsme_data_source_id;
/** dsme error channel GSource ID */
static guint dsme_error_source_id;

static gboolean init_dsmesock(void);

/**
 * Generic send function for dsmesock messages
 * XXX: How should we handle sending failures?
 *
 * @param msg A pointer to the message to send
 */
static void mce_dsme_send(gpointer msg)
{
	if (dsme_disabled == TRUE)
		goto EXIT;

	if (dsme_conn == NULL) {
		mce_log(LL_CRIT,
			"Attempt to use dsme_conn uninitialised; aborting!");
		g_main_loop_quit(mainloop);
		exit(EXIT_FAILURE);
	}

	if ((dsmesock_send(dsme_conn, msg)) == -1) {
		mce_log(LL_CRIT,
			"dsmesock_send error: %s",
			g_strerror(errno));
#ifdef MCE_DSME_ERROR_POLICY
		g_main_loop_quit(mainloop);
		exit(EXIT_FAILURE);
#endif /* MCE_DSME_ERROR_POLICY */
	}

EXIT:
	return;
}

/**
 * Send pong message to the DSME process watchdog
 */
static void dsme_send_pong(void)
{
	/* Set up the message */
	DSM_MSGTYPE_PROCESSWD_PONG msg =
          DSME_MSG_INIT(DSM_MSGTYPE_PROCESSWD_PONG);
	msg.pid = getpid();

	/* Send the message */
	mce_dsme_send(&msg);
	mce_log(LL_DEBUG,
		"DSM_MSGTYPE_PROCESSWD_PONG sent to DSME");
}

/**
 * Register to DSME process watchdog
 */
static void dsme_init_processwd(void)
{
	/* Set up the message */
	DSM_MSGTYPE_PROCESSWD_CREATE msg =
          DSME_MSG_INIT(DSM_MSGTYPE_PROCESSWD_CREATE);
	msg.pid = getpid();

	/* Send the message */
	mce_dsme_send(&msg);
	mce_log(LL_DEBUG,
		"DSM_MSGTYPE_PROCESSWD_CREATE sent to DSME");
}

/**
 * Unregister from DSME process watchdog
 */
static void dsme_exit_processwd(void)
{
	mce_log(LL_DEBUG,
		"Disabling DSME process watchdog");

	/* Set up the message */
	DSM_MSGTYPE_PROCESSWD_DELETE msg =
          DSME_MSG_INIT(DSM_MSGTYPE_PROCESSWD_DELETE);
	msg.pid = getpid();

	/* Send the message */
	mce_dsme_send(&msg);
	mce_log(LL_DEBUG,
		"DSM_MSGTYPE_PROCESSWD_DELETE sent to DSME");
}

/**
 * Send system state inquiry
 */
static void query_system_state(void)
{
	/* Set up the message */
	DSM_MSGTYPE_STATE_QUERY msg = DSME_MSG_INIT(DSM_MSGTYPE_STATE_QUERY);

	/* Send the message */
	mce_dsme_send(&msg);
	mce_log(LL_DEBUG,
		"DSM_MSGTYPE_STATE_QUERY sent to DSME");
}

/**
 * Request powerup
 */
void request_powerup(void)
{
	/* Set up the message */
	DSM_MSGTYPE_POWERUP_REQ msg = DSME_MSG_INIT(DSM_MSGTYPE_POWERUP_REQ);

	/* Send the message */
	mce_dsme_send(&msg);
	mce_log(LL_DEBUG,
		"DSM_MSGTYPE_POWERUP_REQ sent to DSME");
}

/**
 * Request reboot
 */
void request_reboot(void)
{
	/* Set up the message */
	DSM_MSGTYPE_REBOOT_REQ msg = DSME_MSG_INIT(DSM_MSGTYPE_REBOOT_REQ);

	/* Send the message */
	mce_dsme_send(&msg);
	mce_log(LL_DEBUG,
		"DSM_MSGTYPE_REBOOT_REQ sent to DSME");
}

/**
 * Request soft poweron
 */
void request_soft_poweron(void)
{
	/* Disable the soft poweroff LED pattern */
	execute_datapipe_output_triggers(&led_pattern_deactivate_pipe,
					 MCE_LED_PATTERN_DEVICE_SOFT_OFF,
					 USE_INDATA);

	mce_rem_submode_int32(MCE_SOFTOFF_SUBMODE);
	execute_datapipe(&display_state_pipe,
			 GINT_TO_POINTER(MCE_DISPLAY_ON),
			 USE_INDATA, CACHE_INDATA);

	/* Connectivity policy */
	switch (softoff_connectivity_policy_poweron) {
	case SOFTOFF_CONNECTIVITY_FORCE_OFFLINE:
		/* Restore previous radio state */
		execute_datapipe(&master_radio_pipe,
				 GINT_TO_POINTER(previous_radio_state),
				 USE_INDATA, CACHE_INDATA);
		break;

	case SOFTOFF_CONNECTIVITY_OFFLINE:
	default:
		/* Do nothing */
		break;
	}
}

/**
 * Request soft poweroff
 */
void request_soft_poweroff(void)
{
	gboolean connected;
	gint policy;

	if (charger_connected == TRUE)
		policy = softoff_connectivity_policy_charger;
	else
		policy = softoff_connectivity_policy_battery;

	connected = get_connectivity_status();

	/* Connectivity policy */
	switch (policy) {
	case SOFTOFF_CONNECTIVITY_SOFT_OFFLINE:
		/* If there are open connections, abort */
		if (connected == TRUE)
			break;

		/* Fall-through */
	case SOFTOFF_CONNECTIVITY_FORCE_OFFLINE:
		/* Store radio state for restore on soft poweron */
		previous_radio_state = datapipe_get_gint(master_radio_pipe);

		/* Go offline */
		execute_datapipe(&master_radio_pipe,
				 GINT_TO_POINTER(0),
				 USE_INDATA, CACHE_INDATA);
		break;

	case SOFTOFF_CONNECTIVITY_RETAIN:
	default:
		break;
	}

	mce_add_submode_int32(MCE_SOFTOFF_SUBMODE);
	execute_datapipe(&display_state_pipe,
			 GINT_TO_POINTER(MCE_DISPLAY_OFF),
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
static gboolean transition_timeout_cb(gpointer data)
{
	(void)data;

	transition_timeout_cb_id = 0;

	mce_rem_submode_int32(MCE_TRANSITION_SUBMODE);

	return FALSE;
}

/**
 * Cancel state transition timeout
 */
static void cancel_state_transition_timeout(void)
{
	/* Remove the timeout source for state transitions */
	if (transition_timeout_cb_id != 0) {
		g_source_remove(transition_timeout_cb_id);
		transition_timeout_cb_id = 0;
	}
}

/**
 * Setup state transition timeout
 */
static void setup_transition_timeout(void)
{
	cancel_state_transition_timeout();

	/* Setup new timeout */
	transition_timeout_cb_id =
		g_timeout_add(TRANSITION_DELAY, transition_timeout_cb, NULL);
}

/**
 * Request normal shutdown
 */
void request_normal_shutdown(void)
{
	/* Set up the message */
	DSM_MSGTYPE_SHUTDOWN_REQ msg = DSME_MSG_INIT(DSM_MSGTYPE_SHUTDOWN_REQ);

	/* Send the message */
	mce_dsme_send(&msg);
	mce_log(LL_DEBUG,
		"DSM_MSGTYPE_SHUTDOWN_REQ (DSME_NORMAL_SHUTDOWN) "
		"sent to DSME");
}

/**
 * Convert DSME dsme state
 * to a state enum that we can export to datapipes
 *
 * @param dsmestate The DSME dsme_state_t with the value to convert
 * @return the converted value
 */
static system_state_t normalise_dsme_state(dsme_state_t dsmestate)
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
static gboolean io_data_ready_cb(GIOChannel *source,
				 GIOCondition condition,
				 gpointer data)
{
	dsmemsg_generic_t *msg;
	DSM_MSGTYPE_STATE_CHANGE_IND *msg2;
	system_state_t oldstate = datapipe_get_gint(system_state_pipe);
	system_state_t newstate = MCE_STATE_UNDEF;

	(void)source;
	(void)condition;
	(void)data;

	if (dsme_disabled == TRUE)
		goto EXIT;

	if ((msg = (dsmemsg_generic_t *)dsmesock_receive(dsme_conn)) == NULL)
		goto EXIT;

        if (DSMEMSG_CAST(DSM_MSGTYPE_CLOSE, msg)) {
		/* DSME socket closed: try once to reopen;
		 * if that fails, exit
		 */
		mce_log(LL_ERR,
			"DSME socket closed; trying to reopen");

		if ((init_dsmesock()) == FALSE) {
			g_main_loop_quit(mainloop);
			exit(EXIT_FAILURE);
		}
        } else if (DSMEMSG_CAST(DSM_MSGTYPE_PROCESSWD_PING, msg)) {
		dsme_send_pong();
        } else if ((msg2 = DSMEMSG_CAST(DSM_MSGTYPE_STATE_CHANGE_IND, msg))) {
		newstate = normalise_dsme_state(msg2->state);
		mce_log(LL_DEBUG,
			"DSME device state change: %d",
			newstate);

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

		execute_datapipe(&system_state_pipe,
				 GINT_TO_POINTER(newstate),
				 USE_INDATA, CACHE_INDATA);
        } else {
		mce_log(LL_DEBUG,
			"Unknown message type (%x) received from DSME!",
			msg->type_); /* <- unholy access of a private member */
	}

	free(msg);

EXIT:
	return TRUE;
}

/**
 * Callback for I/O errors from dsmesock
 *
 * @param source Unused
 * @param condition Unused
 * @param data Unused
 * @return Will never return; if there is an I/O-error we exit the mainloop
 */
static gboolean io_error_cb(GIOChannel *source,
			    GIOCondition condition,
			    gpointer data) G_GNUC_NORETURN;

static gboolean io_error_cb(GIOChannel *source,
			    GIOCondition condition,
			    gpointer data)
{
	/* Silence warnings */
	(void)source;
	(void)condition;
	(void)data;

	/* DSME socket closed/error */
	mce_log(LL_CRIT,
		"DSME socket closed/error, exiting...");
	g_main_loop_quit(mainloop);
	exit(EXIT_FAILURE);
}

/**
 * D-Bus callback for the init done notification signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean init_done_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	(void)msg;

	mce_log(LL_DEBUG,
		"Received init done notification");

	if ((mce_get_submode_int32() & MCE_TRANSITION_SUBMODE)) {
		setup_transition_timeout();
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
static void charger_state_trigger(gconstpointer const data)
{
	submode_t submode = mce_get_submode_int32();

	charger_connected = GPOINTER_TO_INT(data);

	if ((submode & MCE_SOFTOFF_SUBMODE) != 0) {
		if (softoff_charger_connect_policy == SOFTOFF_CHARGER_CONNECT_WAKEUP) {
			request_soft_poweron();
		}
	}
}

/**
 * Initialise dsmesock connection
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean init_dsmesock(void)
{
	gboolean status = FALSE;

	if (dsme_conn == NULL) {
		if ((dsme_conn = dsmesock_connect()) == NULL) {
			mce_log(LL_CRIT,
				"Failed to open DSME socket");
			goto EXIT;
		}
	}

	if ((dsme_iochan = g_io_channel_unix_new(dsme_conn->fd)) == NULL) {
		mce_log(LL_CRIT,
			"Failed to set up I/O channel for DSME socket");
		goto EXIT;
	}

	dsme_data_source_id = g_io_add_watch(dsme_iochan,
					     G_IO_IN | G_IO_PRI,
					     io_data_ready_cb, NULL);
	dsme_error_source_id = g_io_add_watch(dsme_iochan,
					      G_IO_ERR | G_IO_HUP,
					      io_error_cb, NULL);

	/* Query the current system state; if the mainloop isn't running,
	 * this will trigger an update when the mainloop starts
	 */
	query_system_state();

	status = TRUE;

EXIT:
	return status;
}

/**
 * Close dsmesock connection
 */
static void close_dsmesock(void)
{
	mce_log(LL_DEBUG,
		"Shutting down dsmesock I/O channel");

	if (dsme_iochan != NULL) {
		GError *error = NULL;
		g_source_remove(dsme_data_source_id);
		g_source_remove(dsme_error_source_id);
		g_io_channel_shutdown(dsme_iochan, FALSE, &error);
		g_io_channel_unref(dsme_iochan);
		g_clear_error(&error);
	}

	mce_log(LL_DEBUG,
		"Closing DSME sock");

	dsmesock_close(dsme_conn);
}

/**
 * Init function for the mce-dsme component
 *
 * @param debug_mode TRUE - do not exit if dsme fails
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_dsme_init(gboolean debug_mode)
{
	gboolean status = FALSE;
	gchar *tmp = NULL;

	/* Append triggers/filters to datapipes */
	append_output_trigger_to_datapipe(&charger_state_pipe,
					  charger_state_trigger);

	mce_log(LL_DEBUG,
		"Connecting to DSME sock");

	if (init_dsmesock() == FALSE) {
		if (debug_mode == TRUE) {
			dsme_disabled = TRUE;
		} else {
			goto EXIT;
		}
	}

	/* Register with DSME's process watchdog */
	dsme_init_processwd();

	/* init_done */
	if (mce_dbus_handler_add("com.nokia.startup.signal",
				 "init_done",
				 NULL,
				 DBUS_MESSAGE_TYPE_SIGNAL,
				 init_done_dbus_cb) == NULL)
		goto EXIT;

	/* Get configuration options */
	tmp = mce_conf_get_string(MCE_CONF_SOFTPOWEROFF_GROUP,
				  MCE_CONF_SOFTPOWEROFF_CONNECTIVITY_POLICY_CHARGER,
				  "",
				  NULL);

	softoff_connectivity_policy_charger = mce_translate_string_to_int_with_default(soft_poweroff_connectivity_translation, tmp, DEFAULT_SOFTOFF_CONNECTIVITY_CHARGER);
	g_free(tmp);

	tmp = mce_conf_get_string(MCE_CONF_SOFTPOWEROFF_GROUP,
				  MCE_CONF_SOFTPOWEROFF_CONNECTIVITY_POLICY_BATTERY,
				  "",
				  NULL);

	softoff_connectivity_policy_battery = mce_translate_string_to_int_with_default(soft_poweroff_connectivity_translation, tmp, DEFAULT_SOFTOFF_CONNECTIVITY_BATTERY);
	g_free(tmp);

	tmp = mce_conf_get_string(MCE_CONF_SOFTPOWEROFF_GROUP,
				  MCE_CONF_SOFTPOWEROFF_CONNECTIVITY_POLICY_POWERON,
				  "",
				  NULL);

	softoff_connectivity_policy_poweron = mce_translate_string_to_int_with_default(soft_poweron_connectivity_translation, tmp, DEFAULT_SOFTOFF_CONNECTIVITY_POWERON);
	g_free(tmp);

	tmp = mce_conf_get_string(MCE_CONF_SOFTPOWEROFF_GROUP,
				  MCE_CONF_SOFTPOWEROFF_CHARGER_POLICY_CONNECT,
				  "",
				  NULL);

	softoff_charger_connect_policy = mce_translate_string_to_int_with_default(soft_poweroff_charger_connect_translation, tmp, DEFAULT_SOFTOFF_CHARGER_CONNECT);
	g_free(tmp);

	status = TRUE;

EXIT:
	return status;
}

/**
 * Exit function for the mce-dsme component
 *
 * @todo D-Bus unregistration
 * @todo trigger unregistration
 */
void mce_dsme_exit(void)
{
	if (dsme_conn != NULL) {
		dsme_exit_processwd();
		close_dsmesock();
	}

	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&charger_state_pipe,
					    charger_state_trigger);

	/* Remove all timer sources */
	cancel_state_transition_timeout();

	return;
}
