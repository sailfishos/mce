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
#include <glib.h>
#include <gmodule.h>

#include <errno.h>		/* errno */
#include <string.h>		/* strlen(), strncmp() */

#include <mce/mode-names.h>	/* MCE_RADIO_STATE_MASTER */

#include "mce.h"
#include "radiostates.h"	/* MCE_RADIO_STATES_PATH */

#include "mce-io.h"		/* mce_read_number_string_from_file(),
				 * mce_write_number_string_to_file_atomic(),
				 * mce_are_settings_locked(),
				 * mce_unlock_settings()
				 */
#include "mce-log.h"		/* mce_log(), LL_* */
#include "mce-conf.h"		/* mce_conf_read_conf_file(),
				 * mce_conf_free_conf_file(),
				 * mce_conf_get_bool()
				 */
#include "mce-dbus.h"		/* Direct:
				 * ---
				 * mce_dbus_handler_add(),
				 * dbus_send_message(),
				 * dbus_new_method_reply(),
				 * dbus_new_signal(),
				 * dbus_message_append_args(),
				 * dbus_message_unref(),
				 * DBusMessage,
				 * DBUS_MESSAGE_TYPE_METHOD_CALL,
				 * DBUS_TYPE_UINT64,
				 * DBUS_TYPE_INVALID,
				 * dbus_uint64_t
				 *
				 * Indirect:
				 * ---
				 * MCE_SIGNAL_IF,
				 * MCE_SIGNAL_PATH,
				 * MCE_REQUEST_IF,
				 * MCE_RADIO_STATES_GET,
				 * MCE_RADIO_STATES_CHANGE_REQ,
				 * MCE_RADIO_STATES_SIG
				 */
#include "datapipe.h"		/* execute_datapipe(),
				 * append_output_trigger_to_datapipe(),
				 * remove_output_trigger_from_datapipe()
				 */

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
static dbus_uint32_t radio_states = 0;

/** Active radio states (master switch disables all radios) */
static dbus_uint32_t active_radio_states = 0;

/**
 * Get default radio states from customisable settings
 *
 * @return -1 in case of error, radio states otherwise
 */
static gint get_default_radio_states(void)
{
	GKeyFile *radio_states_conf = NULL;
	gint default_radio_states = -1;
	gboolean radio_state = FALSE;
	gint i = 0;

	if ((radio_states_conf = mce_conf_read_conf_file(G_STRINGIFY(MCE_RADIO_STATES_CONF_FILE))) == NULL)
		goto EXIT;

	for (i = 0, default_radio_states = 0; i < RADIO_STATES_COUNT; ++i) {
		radio_state = mce_conf_get_bool(MCE_CONF_RADIO_STATES_GROUP,
						radio_state_names[i],
						radio_state_defaults[i],
						radio_states_conf);
		default_radio_states |= radio_state_flags[i] *
					(radio_state == TRUE ? 1 : 0);
	}

	mce_log(LL_DEBUG, "default_radio_states = %x", default_radio_states);

	mce_conf_free_conf_file(radio_states_conf);

EXIT:
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

	mce_log(LL_DEBUG,
		"Sending radio states: %x", active_radio_states);

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
				     DBUS_TYPE_UINT32, &active_radio_states,
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
static void set_radio_states(const dbus_uint32_t states,
			     const dbus_uint32_t mask)
{
	radio_states = (radio_states & ~mask) | (states & mask);

	if (((mask & MCE_RADIO_STATE_MASTER) != 0) &&
	    ((states & MCE_RADIO_STATE_MASTER) == 0)) {
		active_radio_states = states & mask;
	} else if ((radio_states & MCE_RADIO_STATE_MASTER) == 0) {
		active_radio_states = (active_radio_states & ~mask) | (states & mask);
	} else {
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
		mce_log(LL_INFO,
			"Removing stale settings lockfile");

		if (mce_unlock_settings() == FALSE) {
			mce_log(LL_ERR,
				"Failed to remove settings lockfile; %s",
				g_strerror(errno));
			errno = 0;
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

	if (save_radio_states((gulong)*online_states,
			      (gulong)*offline_states) == FALSE) {
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

	mce_log(LL_DEBUG,
		"Received get radio states request");

	/* Try to send a reply that contains the current radio states */
	if (send_radio_states(msg) == FALSE)
		goto EXIT;

	status = TRUE;

EXIT:
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
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	dbus_uint32_t old_radio_states = active_radio_states;
	gboolean status = FALSE;
	dbus_uint32_t states;
	dbus_uint32_t mask;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	mce_log(LL_DEBUG, "Received radio states change request");

	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_UINT32, &states,
				  DBUS_TYPE_UINT32, &mask,
				  DBUS_TYPE_INVALID) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_CRIT,
			"Failed to get argument from %s.%s: %s",
			MCE_REQUEST_IF, MCE_RADIO_STATES_CHANGE_REQ,
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	set_radio_states(states, mask);

	/* If we fail to write the radio states, restore the old states */
	if (save_radio_states((gulong)active_radio_states,
			      (gulong)radio_states) == FALSE)
		set_radio_states(old_radio_states, ~0);

	/* Once we're here radio_states will hold the new radio states,
	 * or the fallback value
	 */

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

	if (old_radio_states != active_radio_states) {
		send_radio_states(NULL);

		/* This is just to make sure that the cache is up to date
		 * and that all callbacks are called; the trigger inside
		 * radiostates.c has already had all its actions performed
		 */
		execute_datapipe(&master_radio_pipe, GINT_TO_POINTER((gint)(radio_states & MCE_RADIO_STATE_MASTER)), USE_INDATA, CACHE_INDATA);
	}

EXIT:
	return status;
}

/**
 * Handle master radio state
 *
 * @param data The master radio state stored in a pointer
 */
static void master_radio_trigger(gconstpointer data)
{
	dbus_uint32_t new_radio_states;

	if (GPOINTER_TO_UINT(data) != 0)
		new_radio_states = (radio_states | MCE_RADIO_STATE_MASTER);
	else
		new_radio_states = (radio_states & ~MCE_RADIO_STATE_MASTER);

	/* If we fail to write the radio states, use the old states */
	if (save_radio_states((gulong)active_radio_states,
			      (gulong)radio_states) == FALSE)
		new_radio_states = radio_states;

	if (radio_states != new_radio_states) {
		set_radio_states(new_radio_states, MCE_RADIO_STATE_MASTER);
		send_radio_states(NULL);
	}
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
	gulong tmp, tmp2;

	/* If we fail to restore the radio states, default to offline */
	if (restore_radio_states(&tmp, &tmp2) == FALSE) {
		if (restore_default_radio_states(&tmp, &tmp2) == FALSE) {
			tmp2 = 0;
			tmp = 0;
		}
	}

	radio_states = tmp2;
	active_radio_states = tmp;

	mce_log(LL_DEBUG,
		"active_radio_states: %x, radio_states: %x",
		active_radio_states, radio_states);

	/* Append triggers/filters to datapipes */
	append_output_trigger_to_datapipe(&master_radio_pipe,
					  master_radio_trigger);

	/* get_radio_states */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_RADIO_STATES_GET,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 get_radio_states_dbus_cb) == NULL)
		goto EXIT;

	/* req_radio_states_change */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_RADIO_STATES_CHANGE_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 req_radio_states_change_dbus_cb) == NULL)
		goto EXIT;

EXIT:
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

	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&master_radio_pipe,
					    master_radio_trigger);

	return;
}
