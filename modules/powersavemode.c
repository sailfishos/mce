/**
 * @file powersavemode.c
 * Power saving mode module -- this handles the power saving mode
 * for MCE
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

#include "../mce.h"
#include "powersavemode.h"

#include "../mce-log.h"
#include "../mce-dbus.h"
#include "../mce-gconf.h"
#include "../datapipe.h"

/** Module name */
#define MODULE_NAME		"powersavemode"

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

/** Battery charge level */
static gint battery_level = 100;
/** Charger state */
static gboolean charger_state = FALSE;

/** GConf callback ID for power saving mode setting */
static guint psm_gconf_cb_id = 0;
/** Power saving mode from GConf */
static gboolean power_saving_mode = DEFAULT_POWER_SAVING_MODE;

/** GConf callback ID for forced power saving mode setting */
static guint force_psm_gconf_cb_id = 0;
/** Forced power saving mode from GConf */
static gboolean force_psm = FALSE;

/** GConf callback ID for power saving mode threshold */
static guint psm_threshold_gconf_cb_id = 0;
/** Power saving mode threshold from GConf */
static gint psm_threshold = DEFAULT_PSM_THRESHOLD;

/** Active power saving mode */
static gboolean active_power_saving_mode = FALSE;

/** Device thermal state */
static thermal_state_t thermal_state = THERMAL_STATE_UNDEF;

/**
 * Send the PSM state
 *
 * @param method_call A DBusMessage to reply to;
 *                    pass NULL to send a signal instead
 * @return TRUE on success, FALSE on failure
 */
static gboolean send_psm_state(DBusMessage *const method_call)
{
	DBusMessage *msg = NULL;
	gboolean status = FALSE;

	mce_log(LL_DEBUG,
		"Sending PSM state: %s",
		active_power_saving_mode ? "TRUE" : "FALSE");

	/* If method_call is set, send a reply,
	 * otherwise, send a signal
	 */
	if (method_call != NULL) {
		msg = dbus_new_method_reply(method_call);
	} else {
		/* psm_state_ind */
		msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
				      MCE_PSM_STATE_SIG);
	}

	/* Append the power saving mode */
	if (dbus_message_append_args(msg,
				     DBUS_TYPE_BOOLEAN, &active_power_saving_mode,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append %sargument to D-Bus message "
			"for %s.%s",
			method_call ? "reply " : "",
			method_call ? MCE_REQUEST_IF :
				      MCE_SIGNAL_IF,
			method_call ? MCE_PSM_STATE_GET :
				      MCE_PSM_STATE_SIG);
		dbus_message_unref(msg);
		goto EXIT;
	}

	/* Send the message */
	status = dbus_send_message(msg);

EXIT:
	return status;
}

/**
 * Update the power saving mode
 */
static void update_power_saving_mode(void)
{
	gint new_power_saving_mode;

	/* XXX: later on we should make overheating another
	 *      trigger for power saving mode too
	 */
	if (((((battery_level <= psm_threshold) &&
	       (power_saving_mode == TRUE)) ||
	      (force_psm == TRUE)) &&
	     (charger_state == FALSE)) ||
	    (thermal_state == THERMAL_STATE_OVERHEATED)) {
		/* If the battery charge level is lower than the threshold,
		 * and the user has enable power saving mode,
		 * or if the device is overheated,
		 * activate low power mode
		 */
		new_power_saving_mode = TRUE;
	} else {
		new_power_saving_mode = FALSE;
	}

	if (active_power_saving_mode != new_power_saving_mode) {
		active_power_saving_mode = new_power_saving_mode;
		(void)execute_datapipe(&power_saving_mode_pipe,
				       GINT_TO_POINTER(active_power_saving_mode),
				       USE_INDATA, CACHE_INDATA);
		send_psm_state(NULL);
	}
}

/**
 * Datapipe trigger for the battery charge level
 *
 * @param data A pointer representation of the battery charge level in percent
 */
static void battery_level_trigger(gconstpointer const data)
{
	battery_level = GPOINTER_TO_INT(data);

	update_power_saving_mode();
}

/**
 * Datapipe trigger for the charger state
 *
 * @param data A pointer representation of the charger state
 */
static void charger_state_trigger(gconstpointer const data)
{
	(void)data;

	charger_state = datapipe_get_gbool(charger_state_pipe);

	update_power_saving_mode();
}

/**
 * Datapipe trigger for the thermal state
 *
 * @param data THERMAL_STATE_OK if the device temperature is normal,
 *             THERMAL_STATE_OVERHEATED if the device is overheated
 */
static void thermal_state_trigger(gconstpointer const data)
{
	thermal_state = GPOINTER_TO_INT(data);

	update_power_saving_mode();
}

/**
 * GConf callback for power saving related settings
 *
 * @param gcc Unused
 * @param id Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data Unused
 */
static void psm_gconf_cb(GConfClient *const gcc, const guint id,
			 GConfEntry *const entry, gpointer const data)
{
	const GConfValue *gcv = gconf_entry_get_value(entry);

	(void)gcc;
	(void)data;

	/* Key is unset */
	if (gcv == NULL) {
		mce_log(LL_DEBUG,
			"GConf Key `%s' has been unset",
			gconf_entry_get_key(entry));
		goto EXIT;
	}

	if (id == psm_gconf_cb_id) {
		power_saving_mode = gconf_value_get_bool(gcv);
		update_power_saving_mode();
	} else if (id == force_psm_gconf_cb_id) {
		force_psm = gconf_value_get_bool(gcv);
		update_power_saving_mode();
	} else if (id == psm_threshold_gconf_cb_id) {
		psm_threshold = gconf_value_get_int(gcv);
		update_power_saving_mode();
	} else {
		mce_log(LL_WARN,
			"Spurious GConf value received; confused!");
	}

EXIT:
	return;
}

/**
 * D-Bus callback for the get PSM mode method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean psm_state_get_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	mce_log(LL_DEVEL, "Received PSM state get request from %s",
	       mce_dbus_get_message_sender_ident(msg));

	/* Try to send a reply that contains the current PSM state */
	if (send_psm_state(msg) == FALSE)
		goto EXIT;

	status = TRUE;

EXIT:
	return status;
}

/**
 * Init function for the power saving mode module
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
	append_output_trigger_to_datapipe(&battery_level_pipe,
					  battery_level_trigger);
	append_output_trigger_to_datapipe(&charger_state_pipe,
					  charger_state_trigger);
	append_output_trigger_to_datapipe(&thermal_state_pipe,
					  thermal_state_trigger);

	/* Power saving mode setting */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_bool(MCE_GCONF_PSM_PATH,
				 &power_saving_mode);

	if (mce_gconf_notifier_add(MCE_GCONF_EM_PATH,
				   MCE_GCONF_PSM_PATH,
				   psm_gconf_cb,
				   &psm_gconf_cb_id) == FALSE)
		goto EXIT;

	/* Forced power saving mode setting */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_bool(MCE_GCONF_FORCED_PSM_PATH,
				 &force_psm);

	if (mce_gconf_notifier_add(MCE_GCONF_EM_PATH,
				   MCE_GCONF_FORCED_PSM_PATH,
				   psm_gconf_cb,
				   &force_psm_gconf_cb_id) == FALSE)
		goto EXIT;

	/* Power saving mode threshold */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_int(MCE_GCONF_PSM_THRESHOLD_PATH,
				&psm_threshold);

	if (mce_gconf_notifier_add(MCE_GCONF_EM_PATH,
				   MCE_GCONF_PSM_THRESHOLD_PATH,
				   psm_gconf_cb,
				   &psm_threshold_gconf_cb_id) == FALSE)
		goto EXIT;

	/* get_psm_state */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_PSM_STATE_GET,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 psm_state_get_dbus_cb) == NULL)
		goto EXIT;

EXIT:
	return NULL;
}

/**
 * Exit function for the power saving mode module
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
	remove_output_trigger_from_datapipe(&thermal_state_pipe,
					    thermal_state_trigger);
	remove_output_trigger_from_datapipe(&battery_level_pipe,
					    battery_level_trigger);
	remove_output_trigger_from_datapipe(&charger_state_pipe,
					    charger_state_trigger);

	return;
}
