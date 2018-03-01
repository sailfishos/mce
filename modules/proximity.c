/**
 * @file proximity.c
 * Proximity sensor module
 * <p>
 * Copyright © 2010-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Tuomo Tanskanen <ext-tuomo.1.tanskanen@nokia.com>
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

#include "proximity.h"

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-setting.h"
#include "../mce-sensorfw.h"

#include <gmodule.h>

/** Module name */
#define MODULE_NAME		"proximity"

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

/** State of proximity sensor monitoring */
static gboolean proximity_monitor_active = FALSE;

/** Cached call state */
static call_state_t call_state = CALL_STATE_INVALID;

/** Cached alarm UI state */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_INVALID_INT32;

/** Cached display state */
static display_state_t display_state_curr = MCE_DISPLAY_UNDEF;

/** Cached submode state */
static submode_t submode = MCE_SUBMODE_NORMAL;

/** Configuration value for use proximity sensor */
static gboolean use_ps_conf_value = MCE_DEFAULT_PROXIMITY_PS_ENABLED;

/** Configuration change id for use_ps_conf_value */
static guint use_ps_conf_id = 0;

/** Configuration value for ps acts as lid sensor */
static gboolean ps_acts_as_lid = MCE_DEFAULT_PROXIMITY_PS_ACTS_AS_LID;

/** Configuration change id for ps_acts_as_lid */
static guint ps_acts_as_lid_conf_id = 0;

/** Broadcast proximity state within MCE
 *
 * @param state COVER_CLOSED or COVER_OPEN
 */
static void report_proximity(cover_state_t state)
{
	/* Get current proximity datapipe value */
	cover_state_t old_state = datapipe_get_gint(proximity_sensor_actual_pipe);

	/* Execute datapipe if state has changed */

	if( old_state != state )
	{
		mce_log(LL_CRUCIAL, "state: %s -> %s",
			cover_state_repr(old_state),
			cover_state_repr(state));

		datapipe_exec_full(&proximity_sensor_actual_pipe,
				   GINT_TO_POINTER(state),
				   USE_INDATA, CACHE_INDATA);
	}
}

/** Broadcast faked lid input state within mce
 *
 * @param state COVER_CLOSED, COVER_OPEN or COVER_UNDEF
 */
static void report_lid_input(cover_state_t state)
{
	cover_state_t old_state = datapipe_get_gint(lid_sensor_actual_pipe);

	if( state != old_state ) {
		mce_log(LL_CRUCIAL, "state: %s -> %s",
			cover_state_repr(old_state),
			cover_state_repr(state));

		datapipe_exec_full(&lid_sensor_actual_pipe,
				   GINT_TO_POINTER(state),
				   USE_INDATA, CACHE_INDATA);
	}
}

/**
 * I/O monitor callback for the proximity sensor (sensorfw)
 *
 * @param covered  proximity sensor covered
 */
static void ps_sensorfw_iomon_cb(bool covered)
{
	cover_state_t proximity_sensor_actual = COVER_UNDEF;

	if( covered )
		proximity_sensor_actual = COVER_CLOSED;
	else
		proximity_sensor_actual = COVER_OPEN;

	if( ps_acts_as_lid )
		report_lid_input(proximity_sensor_actual);
	else
		report_proximity(proximity_sensor_actual);

	return;
}

/** Enable the proximity monitoring
 */
static void enable_proximity_monitor(void)
{
	/* Already enabled? */
	if( proximity_monitor_active )
		goto EXIT;

	mce_log(LL_DEBUG, "enable PS monitoring");
	proximity_monitor_active = TRUE;

	/* install input processing hooks, update current state */
	mce_sensorfw_ps_set_notify(ps_sensorfw_iomon_cb);
	mce_sensorfw_ps_enable();

EXIT:
	return;

}

/** Disable the proximity monitoring
 */
static void disable_proximity_monitor(void)
{
	/* Already disabled? */
	if( !proximity_monitor_active )
		goto EXIT;

	mce_log(LL_DEBUG, "disable PS monitoring");
	proximity_monitor_active = FALSE;

	/* disable input */
	mce_sensorfw_ps_disable();

	/* remove input processing hooks */
	mce_sensorfw_ps_set_notify(0);

EXIT:
	return;
}

/**
 * Update the proximity monitoring
 */
static void update_proximity_monitor(void)
{
	static gboolean old_enable = FALSE;

	/* Default to keeping the proximity sensor always enabled. */
	gboolean enable = TRUE;

	if( !use_ps_conf_value ) {
		enable = FALSE;

		if( ps_acts_as_lid )
			report_lid_input(COVER_UNDEF);
		else
			report_proximity(COVER_OPEN);
	}

	if( old_enable == enable )
		goto EXIT;

	if( (old_enable = enable) ) {
		enable_proximity_monitor();
	} else {
		disable_proximity_monitor();
	}

EXIT:
	return;
}

/** GConf callback for use proximity sensor setting
 *
 * @param gcc   (not used)
 * @param id    Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data  (not used)
 */
static void use_ps_conf_cb(GConfClient *const gcc, const guint id,
			   GConfEntry *const entry, gpointer const data)
{
	(void)gcc; (void)data;

	const GConfValue *gcv;

	if( !(gcv = gconf_entry_get_value(entry)) ) {
		mce_log(LL_WARN, "GConf value removed; confused!");
		goto EXIT;
	}

	if( id == use_ps_conf_id ) {
		gboolean old = use_ps_conf_value;
		use_ps_conf_value = gconf_value_get_bool(gcv);

		if( use_ps_conf_value == old )
			goto EXIT;

	}
	else if( id == ps_acts_as_lid_conf_id ) {
		gboolean old = ps_acts_as_lid;
		ps_acts_as_lid = gconf_value_get_bool(gcv);

		if( ps_acts_as_lid == old )
			goto EXIT;

		if( ps_acts_as_lid ) {
			// ps is lid now -> set ps to open state
			report_proximity(COVER_OPEN);
		}
		else {
			// ps is ps again -> invalidate lid state
			report_lid_input(COVER_UNDEF);
		}
	}
	else {
		mce_log(LL_WARN, "Spurious GConf value received; confused!");
		goto EXIT;
	}

	update_proximity_monitor();
EXIT:
	return;
}

/**
 * Handle call state change
 *
 * @param data The call state stored in a pointer
 */
static void call_state_trigger(gconstpointer const data)
{
	call_state = GPOINTER_TO_INT(data);

	update_proximity_monitor();
}

/**
 * Handle alarm ui state change
 *
 * @param data The alarm state stored in a pointer
 */
static void alarm_ui_state_trigger(gconstpointer const data)
{
	alarm_ui_state = GPOINTER_TO_INT(data);

	update_proximity_monitor();
}

/**
 * Handle display state change
 *
 * @param data The display state stored in a pointer
 */
static void display_state_curr_trigger(gconstpointer data)
{
	display_state_curr = GPOINTER_TO_INT(data);

	update_proximity_monitor();
}

/** Handle submode change
 *
 * @param data The submode stored in a pointer
 */
static void submode_trigger(gconstpointer data)
{
	submode = GPOINTER_TO_INT(data);

	update_proximity_monitor();
}

/**
 * Init function for the proximity sensor module
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

	/* Get initial state of datapipes */
	call_state = datapipe_get_gint(call_state_pipe);
	alarm_ui_state = datapipe_get_gint(alarm_ui_state_pipe);
	display_state_curr = display_state_get();
	submode = datapipe_get_gint(submode_pipe);

	/* Append triggers/filters to datapipes */
	datapipe_add_input_trigger(&call_state_pipe,
				   call_state_trigger);
	datapipe_add_input_trigger(&alarm_ui_state_pipe,
				   alarm_ui_state_trigger);
	datapipe_add_output_trigger(&display_state_curr_pipe,
				    display_state_curr_trigger);
	datapipe_add_output_trigger(&submode_pipe,
				    submode_trigger);

	/* PS enabled setting */
	mce_setting_track_bool(MCE_SETTING_PROXIMITY_PS_ENABLED,
			       &use_ps_conf_value,
			       MCE_DEFAULT_PROXIMITY_PS_ENABLED,
			       use_ps_conf_cb,
			       &use_ps_conf_id);

	/* PS acts as LID sensor */
	mce_setting_track_bool(MCE_SETTING_PROXIMITY_PS_ACTS_AS_LID,
			       &ps_acts_as_lid,
			       MCE_DEFAULT_PROXIMITY_PS_ACTS_AS_LID,
			       use_ps_conf_cb,
			       &ps_acts_as_lid_conf_id);

	/* If the proximity sensor input is used for toggling
	 * lid state, we must take care not to leave proximity
	 * tracking to covered state. */
	if( ps_acts_as_lid )
		report_proximity(COVER_OPEN);

	/* enable/disable sensor based on initial conditions */
	update_proximity_monitor();

	return NULL;
}

/**
 * Exit function for the proximity sensor module
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	/* Stop tracking setting changes  */
	mce_setting_notifier_remove(use_ps_conf_id),
		use_ps_conf_id = 0;

	mce_setting_notifier_remove(ps_acts_as_lid_conf_id),
		ps_acts_as_lid_conf_id = 0;

	/* Remove triggers/filters from datapipes */
	datapipe_remove_output_trigger(&display_state_curr_pipe,
				       display_state_curr_trigger);
	datapipe_remove_input_trigger(&alarm_ui_state_pipe,
				      alarm_ui_state_trigger);
	datapipe_remove_input_trigger(&call_state_pipe,
				      call_state_trigger);
	datapipe_remove_output_trigger(&submode_pipe,
				       submode_trigger);

	/* Disable proximity monitoring to remove callbacks
	 * to unloaded module */
	disable_proximity_monitor();
	return;
}
