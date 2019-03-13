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
#include "../mce-lib.h"
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
 * MP_MONITOR
 * ------------------------------------------------------------------------- */

static gboolean  mp_monitor_linger_end_cb   (gpointer aptr);
static void      mp_monitor_cancel_linger   (void);
static void      mp_monitor_start_linger    (void);
static gboolean  mp_monitor_enable_delay_cb (gpointer aptr);
static void      mp_monitor_report_state    (void);
static void      mp_monitor_update_state    (bool covered);
static void      mp_monitor_set_enabled     (bool enable);
static bool      mp_monitor_on_demand       (void);
static void      mp_monitor_rethink         (void);

/* ------------------------------------------------------------------------- *
 * MP_SETTING
 * ------------------------------------------------------------------------- */

static void  mp_setting_cb  (GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);
static void  mp_setting_init(void);
static void  mp_setting_quit(void);

/* ------------------------------------------------------------------------- *
 * MP_DATAPIPE
 * ------------------------------------------------------------------------- */

static void  mp_datapipe_set_proximity_sensor_actual (cover_state_t state);
static void  mp_datapipe_set_lid_sensor_actual       (cover_state_t state);
static void  mp_datapipe_call_state_cb               (gconstpointer const data);
static void  mp_datapipe_alarm_ui_state_cb           (gconstpointer const data);
static void  mp_datapipe_display_state_next_cb       (gconstpointer data);
static void  mp_datapipe_display_state_curr_cb       (gconstpointer data);
static void  mp_datapipe_submode_cb                  (gconstpointer data);
static void  mp_datapipe_uiexception_type_cb         (gconstpointer data);
static void  mp_datapipe_proximity_sensor_required_cb(gconstpointer data);
static void  mp_datapipe_init                        (void);
static void  mp_datapipe_quit                        (void);

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

/** State of proximity sensor monitoring */
static bool mp_monitor_enabled = FALSE;

/** Linger time for disabling sensor in on-demand mode [ms] */
static gint  mp_monitor_linger_end_ms = 5000;

/** Timer id for disabling sensor in on-demand mode */
static guint mp_monitor_linger_end_id = 0;

/** Time reserved for sensor power up to reach stable state [ms] */
static gint mp_monitor_enable_delay_ms = 500;

/** Timer id for ending sensor power up delay */
static guint mp_monitor_enable_delay_id = 0;

/** Currently active on-demand reasons */
static GHashTable *mp_datapipe_proximity_sensor_required_lut = 0;

/* ------------------------------------------------------------------------- *
 * datapipes
 * ------------------------------------------------------------------------- */

/** Cached proximity_sensor_required_pipe state */
static bool proximity_sensor_required = false;

/** Cached call_state_pipe state */
static call_state_t call_state = CALL_STATE_INVALID;

/** Cached alarm_ui_state_pipe state */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_INVALID_INT32;

/** Cached display_state_next_pipe state */
static display_state_t display_state_next = MCE_DISPLAY_UNDEF;

/** Cached display_state_curr_pipe state */
static display_state_t display_state_curr = MCE_DISPLAY_UNDEF;

/** Cached submode_pipe state */
static submode_t submode = MCE_SUBMODE_NORMAL;

/** Cached uiexception_type_pipe state */
static uiexception_type_t uiexception_type = UIEXCEPTION_TYPE_NONE;

/* ------------------------------------------------------------------------- *
 * settings
 * ------------------------------------------------------------------------- */

/** Configuration value for use proximity sensor */
static gboolean setting_use_ps = MCE_DEFAULT_PROXIMITY_PS_ENABLED;
static guint setting_use_ps_conf_id = 0;

/** Configuration value for on-demand proximity sensor use */
static gboolean setting_on_demand_ps = MCE_DEFAULT_PROXIMITY_ON_DEMAND;
static guint setting_on_demand_ps_conf_id = 0;

/** Configuration value for ps acts as lid sensor */
static gboolean setting_ps_acts_as_lid = MCE_DEFAULT_PROXIMITY_PS_ACTS_AS_LID;
static guint setting_ps_acts_as_lid_conf_id = 0;

/* ========================================================================= *
 * MP_MONITOR
 * ========================================================================= */

static gboolean
mp_monitor_linger_end_cb(gpointer aptr)
{
    (void)aptr;
    mce_log(LL_DEBUG, "PS monitoring: linger timeout");
    mp_monitor_linger_end_id = 0;
    mp_monitor_set_enabled(false);
    return G_SOURCE_REMOVE;
}

static void
mp_monitor_cancel_linger(void)
{
    if( mp_monitor_linger_end_id ) {
        mce_log(LL_DEBUG, "PS monitoring: linger stopped");
        g_source_remove(mp_monitor_linger_end_id),
            mp_monitor_linger_end_id = 0;
    }
}

static void
mp_monitor_start_linger(void)
{
    if( !mp_monitor_linger_end_id ) {
        mce_log(LL_DEBUG, "PS monitoring: linger started");
        mp_monitor_linger_end_id =
            mce_wakelocked_timeout_add(mp_monitor_linger_end_ms,
                                       mp_monitor_linger_end_cb,
                                       0);
    }
}

static gboolean mp_monitor_enable_delay_cb(gpointer aptr)
{
    (void)aptr;
    mce_log(LL_DEBUG, "PS monitoring: sensor power up finished");
    mp_monitor_enable_delay_id = 0;
    mp_monitor_report_state();
    return G_SOURCE_REMOVE;
}

static cover_state_t proximity_sensor_actual = COVER_UNDEF;

static void
mp_monitor_report_state(void)
{
    /* Skip if the sensor is not fully powered up yet */
    if( !mp_monitor_enabled || mp_monitor_enable_delay_id )
        goto EXIT;

    cover_state_t state = proximity_sensor_actual;
    if( setting_ps_acts_as_lid )
        mp_datapipe_set_lid_sensor_actual(state);
    else
        mp_datapipe_set_proximity_sensor_actual(state);

EXIT:
    return;
}

/** Callback for handling proximity sensor state changes
 *
 * @param covered  proximity sensor covered
 */
static void
mp_monitor_update_state(bool covered)
{
    mce_log(LL_DEBUG, "PS monitoring: %s from sensorfwd",
            covered ? "COVERED" : "NOT-COVERED");

    if( covered )
        proximity_sensor_actual = COVER_CLOSED;
    else
        proximity_sensor_actual = COVER_OPEN;

    mp_monitor_report_state();
}

/** Enable / disable proximity monitoring
 */

static void
mp_monitor_set_enabled(bool enable)
{
    mp_monitor_cancel_linger();

    if( mp_monitor_enabled == enable )
        goto EXIT;

    if( mp_monitor_enable_delay_id ) {
        g_source_remove(mp_monitor_enable_delay_id),
            mp_monitor_enable_delay_id = 0;
    }

    if( (mp_monitor_enabled = enable) ) {
        mce_log(LL_DEBUG, "PS monitoring: start sensor power up");
        /* Start sensor power up hold-out timer */
        mp_monitor_enable_delay_id =
            mce_wakelocked_timeout_add(mp_monitor_enable_delay_ms,
                                       mp_monitor_enable_delay_cb,
                                       0);
        /* Install input processing hooks, update current state */
        mce_sensorfw_ps_set_notify(mp_monitor_update_state);
        mce_sensorfw_ps_enable();
    }
    else {
        mce_log(LL_DEBUG, "PS monitoring: sensor power down");
        /* disable input */
        mce_sensorfw_ps_disable();
        /* remove input processing hooks */
        mce_sensorfw_ps_set_notify(0);
        /* Artificially flip to unknown state */
        if( setting_ps_acts_as_lid )
            mp_datapipe_set_lid_sensor_actual(COVER_UNDEF);
        else
            mp_datapipe_set_proximity_sensor_actual(COVER_UNDEF);
    }

EXIT:
    return;
}

/** Evaluate whether there is demand for proximity sensor
 */
static bool
mp_monitor_on_demand(void)
{
    /* Assume proximity state is not needed */
    bool enable = false;

    /* LPM states are controlled by proximity sensor */
    switch( display_state_next ) {
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_LPM_OFF:
        enable = true;
        break;
    default:
        break;
    }
    switch( display_state_curr ) {
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_LPM_OFF:
        enable = true;
        break;
    default:
        break;
    }

    /* Unblank on ringing / in-call blank/unblank */
    switch( call_state ) {
    case CALL_STATE_RINGING:
    case CALL_STATE_ACTIVE:
        enable = true;
        break;
    default:
        break;
    }

    /* Unblank on alarm */
    switch( alarm_ui_state ) {
    case MCE_ALARM_UI_RINGING_INT32:
    case MCE_ALARM_UI_VISIBLE_INT32:
        enable = true;
        break;
    default:
        break;
    }

    /* All exceptional display on conditions require
     * proximity information. */
    if( uiexception_type != UIEXCEPTION_TYPE_NONE )
        enable = true;

    /* Need for proximity sensor, as signaled via
     * proximity_sensor_required_pipe */
    if( proximity_sensor_required )
        enable = true;

    return enable;
}

/** Re-evaluate need for proximity monitoring
 */
static void
mp_monitor_rethink(void)
{
    mce_log(LL_DEBUG, "setting_use_ps=%d setting_on_demand_ps=%d",
            setting_use_ps, setting_on_demand_ps);

    if( !setting_use_ps ) {
        /* Disable without delay */
        mp_monitor_set_enabled(false);

        /* As there will be no updates, feign value
         * appropriate for target datapipe. */
        if( setting_ps_acts_as_lid )
            mp_datapipe_set_lid_sensor_actual(COVER_UNDEF);
        else
            mp_datapipe_set_proximity_sensor_actual(COVER_OPEN);
    }
    else if( !setting_on_demand_ps ) {
        /* Enable without delay and wait for sensor data */
        mp_monitor_set_enabled(true);
    }
    else {
        /* Act on demand */
        bool enable = mp_monitor_on_demand();

        mce_log(LL_DEBUG, "enable=%d enabled=%d",
                enable, mp_monitor_enabled);

        if( enable ) {
            /* Enable without delay, wait for data */
            mp_monitor_set_enabled(true);
        }
        else if( mp_monitor_enabled ) {
            /* Disable after delay, then switch to COVER_UNDEF */
            mp_monitor_start_linger();
        }
    }
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

    if( id == setting_use_ps_conf_id ) {
        gboolean prev = setting_use_ps;
        setting_use_ps = gconf_value_get_bool(gcv);

        if( prev != setting_use_ps ) {
            /* If sensor was disabled, we need to clear feigned
             * targer datapipe values. */
            if( setting_use_ps ) {
                if( setting_ps_acts_as_lid )
                    mp_datapipe_set_lid_sensor_actual(COVER_UNDEF);
                else
                    mp_datapipe_set_proximity_sensor_actual(COVER_UNDEF);
            }
            mp_monitor_rethink();
        }
    }
    else if( id == setting_on_demand_ps_conf_id ) {
        gboolean prev = setting_on_demand_ps;
        setting_on_demand_ps = gconf_value_get_bool(gcv);

        if( prev != setting_on_demand_ps )
            mp_monitor_rethink();
    }
    else if( id == setting_ps_acts_as_lid_conf_id ) {
        gboolean prev = setting_ps_acts_as_lid;
        setting_ps_acts_as_lid = gconf_value_get_bool(gcv);

        if( prev != setting_ps_acts_as_lid ) {
            /* Transfer exposed value to current target datapipe
             * and clear the previous target datapipe. */
            if( setting_ps_acts_as_lid ) {
                /* ps is lid now */
                cover_state_t curr = datapipe_get_gint(proximity_sensor_actual_pipe);
                mp_datapipe_set_proximity_sensor_actual(COVER_OPEN);
                mp_datapipe_set_lid_sensor_actual(curr);
            }
            else {
                /* ps is ps again */
                cover_state_t curr = datapipe_get_gint(lid_sensor_actual_pipe);
                mp_datapipe_set_lid_sensor_actual(COVER_UNDEF);
                mp_datapipe_set_proximity_sensor_actual(curr);
            }
        }
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }
EXIT:
    return;
}

/** Start tracking dynamic settings
 */
static void
mp_setting_init(void)
{
    /* PS enabled setting */
    mce_setting_track_bool(MCE_SETTING_PROXIMITY_PS_ENABLED,
                           &setting_use_ps,
                           MCE_DEFAULT_PROXIMITY_PS_ENABLED,
                           mp_setting_cb,
                           &setting_use_ps_conf_id);

    /* on-demand setting */
    mce_setting_track_bool(MCE_SETTING_PROXIMITY_ON_DEMAND,
                           &setting_on_demand_ps,
                           MCE_DEFAULT_PROXIMITY_ON_DEMAND,
                           mp_setting_cb,
                           &setting_on_demand_ps_conf_id);

    /* PS acts as LID sensor */
    mce_setting_track_bool(MCE_SETTING_PROXIMITY_PS_ACTS_AS_LID,
                           &setting_ps_acts_as_lid,
                           MCE_DEFAULT_PROXIMITY_PS_ACTS_AS_LID,
                           mp_setting_cb,
                           &setting_ps_acts_as_lid_conf_id);
}

/** Stop tracking dynamic settings
 */
static void
mp_setting_quit(void)
{
    mce_setting_notifier_remove(setting_use_ps_conf_id),
        setting_use_ps_conf_id = 0;

    mce_setting_notifier_remove(setting_use_ps_conf_id),
        setting_on_demand_ps_conf_id = 0;

    mce_setting_notifier_remove(setting_ps_acts_as_lid_conf_id),
        setting_ps_acts_as_lid_conf_id = 0;
}

/* ========================================================================= *
 * MP_DATAPIPE
 * ========================================================================= */

/** Broadcast proximity sensor state within MCE
 *
 * @param state COVER_CLOSED or COVER_OPEN
 */
static void
mp_datapipe_set_proximity_sensor_actual(cover_state_t state)
{
    /* Get current proximity datapipe value */
    cover_state_t curr = datapipe_get_gint(proximity_sensor_actual_pipe);

    /* Execute datapipe if state has changed */

    if( curr != state ) {
        mce_log(LL_CRUCIAL, "state: %s -> %s",
                cover_state_repr(curr),
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
mp_datapipe_set_lid_sensor_actual(cover_state_t state)
{
    cover_state_t curr = datapipe_get_gint(lid_sensor_actual_pipe);

    if( state != curr ) {
        mce_log(LL_CRUCIAL, "state: %s -> %s",
                cover_state_repr(curr),
                cover_state_repr(state));

        datapipe_exec_full(&lid_sensor_actual_pipe,
                           GINT_TO_POINTER(state));
    }
}

/** Handle call state change
 *
 * @param data The call state stored in a pointer
 */
static void
mp_datapipe_call_state_cb(gconstpointer const data)
{
    call_state_t prev = call_state;
    call_state = GPOINTER_TO_INT(data);

    if( prev == call_state )
        goto EXIT;

    mce_log(LL_DEBUG, "call_state: %s -> %s",
            call_state_repr(prev),
            call_state_repr(call_state));

    mp_monitor_rethink();

EXIT:
    return;
}

/** Handle alarm ui state change
 *
 * @param data The alarm state stored in a pointer
 */
static void
mp_datapipe_alarm_ui_state_cb(gconstpointer const data)
{
    alarm_ui_state_t prev = alarm_ui_state;
    alarm_ui_state = GPOINTER_TO_INT(data);

    if( prev == alarm_ui_state )
        goto EXIT;

    mce_log(LL_DEBUG, "alarm_ui_state: %s -> %s",
            alarm_state_repr(prev),
            alarm_state_repr(alarm_ui_state));

    mp_monitor_rethink();

EXIT:
    return;
}

/** Handle display state change
 *
 * @param data The display state stored in a pointer
 */
static void
mp_datapipe_display_state_next_cb(gconstpointer data)
{
    display_state_t prev = display_state_next;
    display_state_next = GPOINTER_TO_INT(data);

    if( prev == display_state_next )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_next: %s -> %s",
            display_state_repr(prev),
            display_state_repr(display_state_next));

    mp_monitor_rethink();

EXIT:
    return;
}

/** Handle display state change
 *
 * @param data The display state stored in a pointer
 */
static void
mp_datapipe_display_state_curr_cb(gconstpointer data)
{
    display_state_t prev = display_state_curr;
    display_state_curr = GPOINTER_TO_INT(data);

    if( prev == display_state_curr )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_curr: %s -> %s",
            display_state_repr(prev),
            display_state_repr(display_state_curr));

    mp_monitor_rethink();

EXIT:
    return;
}

/** Handle submode change
 *
 * @param data The submode stored in a pointer
 */
static void
mp_datapipe_submode_cb(gconstpointer data)
{
    submode_t prev = submode;
    submode = GPOINTER_TO_INT(data);

    if( prev == submode )
        goto EXIT;

    mce_log(LL_DEBUG, "submode: %s",
            submode_change_repr(prev, submode));

EXIT:
    return;
}

/** Change notifications for uiexception_type
 */
static void mp_datapipe_uiexception_type_cb(gconstpointer data)
{
    uiexception_type_t prev = uiexception_type;
    uiexception_type = GPOINTER_TO_INT(data);

    if( uiexception_type == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "uiexception_type = %s -> %s",
            uiexception_type_repr(prev),
            uiexception_type_repr(uiexception_type));

    mp_monitor_rethink();

EXIT:
    return;
}

/** Input notifications for proximity_sensor_required_pipe
 */
static void mp_datapipe_proximity_sensor_required_cb(gconstpointer data)
{
    /* Assumption: The tags are statically allocated const strings */
    const char *tag = data;

    if( !tag )
        goto EXIT;

    mce_log(LL_DEBUG, "proximity_sensor_required: %s", tag);

    if( !mp_datapipe_proximity_sensor_required_lut )
        goto EXIT;

    switch( *tag++ ){
    case '+':
        g_hash_table_add(mp_datapipe_proximity_sensor_required_lut,
                         (gpointer)tag);
        break;
    case '-':
        g_hash_table_remove(mp_datapipe_proximity_sensor_required_lut,
                            (gpointer)tag);
        break;
    default:
        goto EXIT;
    }

    bool prev = proximity_sensor_required;
    proximity_sensor_required =
        g_hash_table_size(mp_datapipe_proximity_sensor_required_lut) != 0;

    if( prev == proximity_sensor_required )
        goto EXIT;

    mce_log(LL_DEBUG, "proximity_sensor_required: %d -> %d",
            prev, proximity_sensor_required);

    mp_monitor_rethink();

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t mp_datapipe_handlers[] =
{
    // input triggers
    {
        .datapipe  = &proximity_sensor_required_pipe,
        .input_cb  = mp_datapipe_proximity_sensor_required_cb,
    },
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
        .datapipe  = &display_state_next_pipe,
        .output_cb = mp_datapipe_display_state_next_cb,
    },
    {
        .datapipe  = &display_state_curr_pipe,
        .output_cb = mp_datapipe_display_state_curr_cb,
    },
    {
        .datapipe  = &submode_pipe,
        .output_cb = mp_datapipe_submode_cb,
    },
    {
        .datapipe  = &uiexception_type_pipe,
        .output_cb = mp_datapipe_uiexception_type_cb,
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
    if( !mp_datapipe_proximity_sensor_required_lut ) {
        /* Assumption: Set of statically allocated const strings */
        mp_datapipe_proximity_sensor_required_lut =
            g_hash_table_new(g_str_hash, g_str_equal);
    }

    mce_datapipe_init_bindings(&mp_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void
mp_datapipe_quit(void)
{
    if( mp_datapipe_proximity_sensor_required_lut ) {
        g_hash_table_unref(mp_datapipe_proximity_sensor_required_lut),
            mp_datapipe_proximity_sensor_required_lut = 0;
    }

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

    /* Get settings and start tracking changes */
    mp_setting_init();

    /* If the proximity sensor input is used for toggling
     * lid state, we must take care not to leave proximity
     * tracking to covered/unknown state. */
    if( setting_ps_acts_as_lid )
        mp_datapipe_set_proximity_sensor_actual(COVER_OPEN);

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
    mp_setting_quit();

    /* Remove triggers/filters from datapipes */
    mp_datapipe_quit();

    /* Disable proximity monitoring to remove callbacks
     * to unloaded module */
    mp_monitor_set_enabled(false);
    return;
}
