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
#include "../mce-gconf.h"
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

/** Last proximity sensor state */
static cover_state_t old_proximity_sensor_state = COVER_UNDEF;

/** Cached call state */
static call_state_t call_state = CALL_STATE_INVALID;

/** Cached alarm UI state */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_INVALID_INT32;

/** Cached display state */
static display_state_t display_state = MCE_DISPLAY_UNDEF;

/** Cached submode state */
static submode_t submode = MCE_NORMAL_SUBMODE;

/** Broadcast proximity state within MCE
 *
 * @param state COVER_CLOSED or COVER_OPEN
 */
static void report_proximity(cover_state_t state)
{
	/* Get current proximity datapipe value */
	cover_state_t old_state = datapipe_get_gint(proximity_sensor_pipe);

	/* Execute datapipe if state has changed */

	/* FIXME: figure out where things break down if we do not
	 * omit the non-change datapipe execute ... */
	//if( old_state != state )
	{
		mce_log(LL_NOTICE, "state: %s -> %s",
			cover_state_repr(old_state),
			cover_state_repr(state));
		execute_datapipe(&proximity_sensor_pipe,
				 GINT_TO_POINTER(state),
				 USE_INDATA, CACHE_INDATA);
	}

	/* Update last-seen proximity state */
	old_proximity_sensor_state = state;
}

/**
 * I/O monitor callback for the proximity sensor (sensorfw)
 *
 * @param covered  proximity sensor covered
 */
static void ps_sensorfw_iomon_cb(bool covered)
{
	cover_state_t proximity_sensor_state = COVER_UNDEF;

	if( covered )
		proximity_sensor_state = COVER_CLOSED;
	else
		proximity_sensor_state = COVER_OPEN;

	report_proximity(proximity_sensor_state);

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

/** Configuration value for use proximity sensor */
static gboolean use_ps_conf_value = TRUE;

/** Configuration change id for use proximity sensor */
static guint use_ps_conf_id = 0;

/**
 * Update the proximity monitoring
 */
static void update_proximity_monitor(void)
{
	static gboolean old_enable = FALSE;

	gboolean enable = FALSE;
	gboolean fake_open = FALSE;

	/* Default to keeping the proximity sensor always enabled. */
	enable = TRUE;

	if( !use_ps_conf_value ) {
		fake_open = TRUE;
		enable = FALSE;
	}

	if( old_enable == enable )
		goto EXIT;

	if( (old_enable = enable) ) {
		enable_proximity_monitor();
	} else {
		disable_proximity_monitor();
	}

EXIT:
	if( !enable && fake_open )
		report_proximity(COVER_OPEN);
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

	if( id != use_ps_conf_id ) {
		mce_log(LL_WARN, "Spurious GConf value received; confused!");
		goto EXIT;
	}

	if( !(gcv = gconf_entry_get_value(entry)) ) {
		// config removed -> use proximity sensor
		use_ps_conf_value = TRUE;
	}
	else {
		use_ps_conf_value = gconf_value_get_bool(gcv);
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
static void display_state_trigger(gconstpointer data)
{
	display_state = GPOINTER_TO_INT(data);

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
	display_state = display_state_get();
	submode = datapipe_get_gint(submode_pipe);

	/* Append triggers/filters to datapipes */
	append_input_trigger_to_datapipe(&call_state_pipe,
					 call_state_trigger);
	append_input_trigger_to_datapipe(&alarm_ui_state_pipe,
					 alarm_ui_state_trigger);
	append_output_trigger_to_datapipe(&display_state_pipe,
					  display_state_trigger);
	append_output_trigger_to_datapipe(&submode_pipe,
					  submode_trigger);

	/* PS enabled setting */
	mce_gconf_notifier_add(MCE_GCONF_PROXIMITY_PATH,
			       MCE_GCONF_PROXIMITY_PS_ENABLED_PATH,
			       use_ps_conf_cb,
			       &use_ps_conf_id);

	mce_gconf_get_bool(MCE_GCONF_PROXIMITY_PS_ENABLED_PATH,
			   &use_ps_conf_value);

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

	/* Remove gconf notifications  */
	mce_gconf_notifier_remove(use_ps_conf_id),
		use_ps_conf_id = 0;

	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&display_state_pipe,
					    display_state_trigger);
	remove_input_trigger_from_datapipe(&alarm_ui_state_pipe,
					   alarm_ui_state_trigger);
	remove_input_trigger_from_datapipe(&call_state_pipe,
					   call_state_trigger);
	remove_output_trigger_from_datapipe(&submode_pipe,
					    submode_trigger);

	/* Disable proximity monitoring to remove callbacks
	 * to unloaded module */
	disable_proximity_monitor();
	return;
}
