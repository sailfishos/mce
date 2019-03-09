/**
 * @file proximity.c
 * Proximity sensor module
 * <p>
 * Copyright © 2010-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Tuomo Tanskanen <ext-tuomo.1.tanskanen@nokia.com>
 * @author Tapio Rantala <ext-tapio.rantala@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
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

/* ========================================================================= *
 * Macros
 * ========================================================================= */

/** Module name */
#define MODULE_NAME             "proximity"

/* ========================================================================= *
 * Protos
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * MP_REPORT
 * ------------------------------------------------------------------------- */

static void  mp_report_proximity_state(cover_state_t state);
static void  mp_report_lid_state      (cover_state_t state);

/* ------------------------------------------------------------------------- *
 * MP_MONITOR
 * ------------------------------------------------------------------------- */

static void  mp_monitor_update_state(bool covered);
static void  mp_monitor_enable      (void);
static void  mp_monitor_disable     (void);
static void  mp_monitor_rethink     (void);

/* ------------------------------------------------------------------------- *
 * MP_SETTING
 * ------------------------------------------------------------------------- */

static void  mp_setting_cb(GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);

/* ------------------------------------------------------------------------- *
 * MP_DATAPIPE
 * ------------------------------------------------------------------------- */

static void  mp_datapipe_call_state_cb        (gconstpointer const data);
static void  mp_datapipe_alarm_ui_state_cb    (gconstpointer const data);
static void  mp_datapipe_display_state_curr_cb(gconstpointer data);
static void  mp_datapipe_submode_cb           (gconstpointer data);
static void  mp_datapipe_init                 (void);
static void  mp_datapipe_quit                 (void);

/* ------------------------------------------------------------------------- *
 * G_MODULE
 * ------------------------------------------------------------------------- */

const gchar  *g_module_check_init(GModule *module);
void          g_module_unload    (GModule *module);

/* ========================================================================= *
 * Data
 * ========================================================================= */

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

/** Cached call state */
static call_state_t call_state = CALL_STATE_INVALID;

/** Cached alarm UI state */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_INVALID_INT32;

/** Cached display state */
static display_state_t display_state_curr = MCE_DISPLAY_UNDEF;

/** Cached submode state */
static submode_t submode = MCE_SUBMODE_NORMAL;

/** State of proximity sensor monitoring */
static gboolean mp_monitor_enabled = FALSE;

/** Configuration value for use proximity sensor */
static gboolean use_ps = MCE_DEFAULT_PROXIMITY_PS_ENABLED;

/** Configuration change id for use_ps */
static guint use_ps_conf_id = 0;

/** Configuration value for ps acts as lid sensor */
static gboolean ps_acts_as_lid = MCE_DEFAULT_PROXIMITY_PS_ACTS_AS_LID;

/** Configuration change id for ps_acts_as_lid */
static guint ps_acts_as_lid_conf_id = 0;

/* ========================================================================= *
 * MP_REPORT
 * ========================================================================= */

/** Broadcast proximity sensor state within MCE
 *
 * @param state COVER_CLOSED or COVER_OPEN
 */
static void
mp_report_proximity_state(cover_state_t state)
{
    /* Get current proximity datapipe value */
    cover_state_t old_state = datapipe_get_gint(proximity_sensor_actual_pipe);

    /* Execute datapipe if state has changed */

    if( old_state != state ) {
        mce_log(LL_CRUCIAL, "state: %s -> %s",
                cover_state_repr(old_state),
                cover_state_repr(state));

        datapipe_exec_full(&proximity_sensor_actual_pipe,
                           GINT_TO_POINTER(state));
    }
}

/** Broadcast faked lid sensor state within mce
 *
 * @param state COVER_CLOSED, COVER_OPEN or COVER_UNDEF
 */
static void
mp_report_lid_state(cover_state_t state)
{
    cover_state_t old_state = datapipe_get_gint(lid_sensor_actual_pipe);

    if( state != old_state ) {
        mce_log(LL_CRUCIAL, "state: %s -> %s",
                cover_state_repr(old_state),
                cover_state_repr(state));

        datapipe_exec_full(&lid_sensor_actual_pipe,
                           GINT_TO_POINTER(state));
    }
}

/* ========================================================================= *
 * MP_MONITOR
 * ========================================================================= */

/** Callback for handling proximity sensor state changes
 *
 * @param covered  proximity sensor covered
 */
static void
mp_monitor_update_state(bool covered)
{
    cover_state_t proximity_sensor_actual = COVER_UNDEF;

    if( covered )
        proximity_sensor_actual = COVER_CLOSED;
    else
        proximity_sensor_actual = COVER_OPEN;

    if( ps_acts_as_lid )
        mp_report_lid_state(proximity_sensor_actual);
    else
        mp_report_proximity_state(proximity_sensor_actual);

    return;
}

/** Enable proximity monitoring
 */
static void
mp_monitor_enable(void)
{
    /* Already enabled? */
    if( mp_monitor_enabled )
        goto EXIT;

    mce_log(LL_DEBUG, "enable PS monitoring");
    mp_monitor_enabled = TRUE;

    /* install input processing hooks, update current state */
    mce_sensorfw_ps_set_notify(mp_monitor_update_state);
    mce_sensorfw_ps_enable();

EXIT:
    return;

}

/** Disable proximity monitoring
 */
static void
mp_monitor_disable(void)
{
    /* Already disabled? */
    if( !mp_monitor_enabled )
        goto EXIT;

    mce_log(LL_DEBUG, "disable PS monitoring");
    mp_monitor_enabled = FALSE;

    /* disable input */
    mce_sensorfw_ps_disable();

    /* remove input processing hooks */
    mce_sensorfw_ps_set_notify(0);

EXIT:
    return;
}

/** Re-evaluate need for proximity monitoring
 */
static void
mp_monitor_rethink(void)
{
    static gboolean old_enable = FALSE;

    /* Default to keeping the proximity sensor always enabled. */
    gboolean enable = TRUE;

    if( !use_ps ) {
        enable = FALSE;

        if( ps_acts_as_lid )
            mp_report_lid_state(COVER_UNDEF);
        else
            mp_report_proximity_state(COVER_OPEN);
    }

    if( old_enable == enable )
        goto EXIT;

    if( (old_enable = enable) )
        mp_monitor_enable();
    else
        mp_monitor_disable();

EXIT:
    return;
}

/* ========================================================================= *
 * MP_SETTING
 * ========================================================================= */

/** GConf callback for use proximity sensor setting
 *
 * @param gcc   (not used)
 * @param id    Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data  (not used)
 */
static void
mp_setting_cb(GConfClient *const gcc, const guint id,
              GConfEntry *const entry, gpointer const data)
{
    (void)gcc; (void)data;

    const GConfValue *gcv;

    if( !(gcv = gconf_entry_get_value(entry)) ) {
        mce_log(LL_WARN, "GConf value removed; confused!");
        goto EXIT;
    }

    if( id == use_ps_conf_id ) {
        gboolean old = use_ps;
        use_ps = gconf_value_get_bool(gcv);

        if( use_ps == old )
            goto EXIT;

    }
    else if( id == ps_acts_as_lid_conf_id ) {
        gboolean old = ps_acts_as_lid;
        ps_acts_as_lid = gconf_value_get_bool(gcv);

        if( ps_acts_as_lid == old )
            goto EXIT;

        if( ps_acts_as_lid ) {
            // ps is lid now -> set ps to open state
            mp_report_proximity_state(COVER_OPEN);
        }
        else {
            // ps is ps again -> invalidate lid state
            mp_report_lid_state(COVER_UNDEF);
        }
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
        goto EXIT;
    }

    mp_monitor_rethink();
EXIT:
    return;
}

/* ========================================================================= *
 * MP_DATAPIPE
 * ========================================================================= */

/** Handle call state change
 *
 * @param data The call state stored in a pointer
 */
static void
mp_datapipe_call_state_cb(gconstpointer const data)
{
    call_state = GPOINTER_TO_INT(data);

    mp_monitor_rethink();
}

/** Handle alarm ui state change
 *
 * @param data The alarm state stored in a pointer
 */
static void
mp_datapipe_alarm_ui_state_cb(gconstpointer const data)
{
    alarm_ui_state = GPOINTER_TO_INT(data);

    mp_monitor_rethink();
}

/** Handle display state change
 *
 * @param data The display state stored in a pointer
 */
static void
mp_datapipe_display_state_curr_cb(gconstpointer data)
{
    display_state_curr = GPOINTER_TO_INT(data);

    mp_monitor_rethink();
}

/** Handle submode change
 *
 * @param data The submode stored in a pointer
 */
static void
mp_datapipe_submode_cb(gconstpointer data)
{
    submode = GPOINTER_TO_INT(data);

    mp_monitor_rethink();
}

/** Array of datapipe handlers */
static datapipe_handler_t mp_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &call_state_pipe,
        .output_cb = mp_datapipe_call_state_cb,
    },
    {
        .datapipe  = &alarm_ui_state_pipe,
        .output_cb = mp_datapipe_alarm_ui_state_cb,
    },
    {
        .datapipe  = &display_state_curr_pipe,
        .output_cb = mp_datapipe_display_state_curr_cb,
    },
    {
        .datapipe  = &submode_pipe,
        .output_cb = mp_datapipe_submode_cb,
    },
    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t mp_datapipe_bindings =
{
    .module   = MODULE_NAME,
    .handlers = mp_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void
mp_datapipe_init(void)
{
    mce_datapipe_init_bindings(&mp_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void
mp_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&mp_datapipe_bindings);
}

/* ========================================================================= *
 * G_MODULE
 * ========================================================================= */

/** Init function for the proximity sensor module
 *
 * @param module Unused
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *
g_module_check_init(GModule *module)
{
    (void)module;

    /* Append triggers/filters to datapipes */
    mp_datapipe_init();

    /* PS enabled setting */
    mce_setting_track_bool(MCE_SETTING_PROXIMITY_PS_ENABLED,
                           &use_ps,
                           MCE_DEFAULT_PROXIMITY_PS_ENABLED,
                           mp_setting_cb,
                           &use_ps_conf_id);

    /* PS acts as LID sensor */
    mce_setting_track_bool(MCE_SETTING_PROXIMITY_PS_ACTS_AS_LID,
                           &ps_acts_as_lid,
                           MCE_DEFAULT_PROXIMITY_PS_ACTS_AS_LID,
                           mp_setting_cb,
                           &ps_acts_as_lid_conf_id);

    /* If the proximity sensor input is used for toggling
     * lid state, we must take care not to leave proximity
     * tracking to covered state. */
    if( ps_acts_as_lid )
        mp_report_proximity_state(COVER_OPEN);

    /* enable/disable sensor based on initial conditions */
    mp_monitor_rethink();

    return NULL;
}

/** Exit function for the proximity sensor module
 *
 * @param module Unused
 */
G_MODULE_EXPORT void
g_module_unload(GModule *module)
{
    (void)module;

    /* Stop tracking setting changes  */
    mce_setting_notifier_remove(use_ps_conf_id),
        use_ps_conf_id = 0;

    mce_setting_notifier_remove(ps_acts_as_lid_conf_id),
        ps_acts_as_lid_conf_id = 0;

    /* Remove triggers/filters from datapipes */
    mp_datapipe_quit();

    /* Disable proximity monitoring to remove callbacks
     * to unloaded module */
    mp_monitor_disable();
    return;
}
