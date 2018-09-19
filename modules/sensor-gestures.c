/**
 * @file sensor-gestures.c
 *
 * Sensor gesture module for the Mode Control Entity
 * <p>
 * Copyright © 2014 Jolla Ltd.
 * <p>
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

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-setting.h"
#include "../mce-dbus.h"
#include "display.h"

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include <gmodule.h>

/* ========================================================================= *
 * STATE_DATA
 * ========================================================================= */

/** Cached display state */
static display_state_t display_state_curr = MCE_DISPLAY_UNDEF;

/** Cached alarm ui state */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_INVALID_INT32;

/** Cached call state */
static call_state_t call_state = CALL_STATE_INVALID;

/** Cached raw orientation sensor value */
static orientation_state_t orientation_sensor_actual = MCE_ORIENTATION_UNDEFINED;

/** Cached delayed orientation sensor value */
static orientation_state_t orientation_sensor_effective = MCE_ORIENTATION_UNDEFINED;

/** Timer id for delayed orientation_sensor_effective updating */
static gint orientation_sensor_effective_id = 0;

/** Use of flipover gesture enabled */
static gboolean sg_flipover_gesture_enabled = MCE_DEFAULT_FLIPOVER_GESTURE_ENABLED;
static guint    sg_flipover_gesture_enabled_setting_id = 0;

/* ========================================================================= *
 * FUNCTIONS
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * FLIPOVER_GESTURE
 * ------------------------------------------------------------------------- */

static void     sg_send_flipover_signal     (const char *sig);
static void     sg_detect_flipover_gesture  (void);

/* ------------------------------------------------------------------------- *
 * DATAPIPE_TRACKING
 * ------------------------------------------------------------------------- */

static bool     sg_have_alarm_dialog        (void);
static bool     sg_have_incoming_call       (void);

static void     sg_call_state_cb            (gconstpointer const data);
static void     sg_alarm_ui_state_cb        (gconstpointer data);
static void     sg_display_state_curr_cb    (gconstpointer data);
static void     sg_orientation_sensor_update(void);
static gboolean sg_orientation_sensor_effective_cb (gpointer data);
static void     sg_orientation_sensor_actual_cb (gconstpointer data);

static void     sg_datapipe_init            (void);
static void     sg_datapipe_quit            (void);

/* ------------------------------------------------------------------------- *
 * DYNAMIC_SETTINGS
 * ------------------------------------------------------------------------- */

static void     sg_setting_cb               (GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);

static void     sg_setting_init             (void);
static void     sg_setting_quit             (void);

/* ------------------------------------------------------------------------- *
 * PLUGIN_LOAD_UNLOAD
 * ------------------------------------------------------------------------- */

G_MODULE_EXPORT const gchar *g_module_check_init (GModule *module);
G_MODULE_EXPORT void         g_module_unload     (GModule *module);

/* ========================================================================= *
 * FLIPOVER_GESTURE
 * ========================================================================= */

/** Helper for sending flipover dbus signal
 *
 * @param signal name
 */
static void sg_send_flipover_signal(const char *sig)
{
    /* Do not send the signals if orientation sensor happens to be
     * powered on for some other reasons than flipover detection */
    if( !sg_flipover_gesture_enabled )
        goto EXIT;

    // NOTE: introspection data shared with powerkey.c
    const char *arg = MCE_FEEDBACK_EVENT_FLIPOVER;
    mce_log(LL_DEVEL, "sending dbus signal: %s %s", sig, arg);
    dbus_send(0, MCE_SIGNAL_PATH, MCE_SIGNAL_IF,  sig, 0,
              DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);

EXIT:
    return;
}

/** Detect and broadcast device flipover during alarm
 *
 * While display is on and alarm active,
 * send "flipover" signal over dbus, if
 * we first see orientation = face up,
 * followed by orientation = face down.
 */
static void sg_detect_flipover_gesture(void)
{
    static bool primed = false;

    /* Check display state */
    if( display_state_curr != MCE_DISPLAY_ON ) {
        primed = false;
        goto EXIT;
    }

    /* Check active alarm / incoming call */
    if( !sg_have_alarm_dialog() &&
        !sg_have_incoming_call() ) {
        primed = false;
        goto EXIT;
    }

    /* Check for undefined orientation state */
    if( orientation_sensor_actual == MCE_ORIENTATION_UNDEFINED ||
        orientation_sensor_effective == MCE_ORIENTATION_UNDEFINED ) {
        primed = false;
        goto EXIT;
    }

    /* Check effective orientation state */
    if( orientation_sensor_effective == MCE_ORIENTATION_FACE_UP ) {
        primed = true;
    }
    else if( orientation_sensor_effective != MCE_ORIENTATION_FACE_DOWN ) {
        // nop
    }
    else if( primed ) {
        primed = false;

        if( sg_have_alarm_dialog() )
            sg_send_flipover_signal(MCE_ALARM_UI_FEEDBACK_SIG);

        if( sg_have_incoming_call() )
            sg_send_flipover_signal(MCE_CALL_UI_FEEDBACK_SIG);
    }

EXIT:

    return;
}

/* ========================================================================= *
 * DATAPIPE_TRACKING
 * ========================================================================= */

/** Helper for checking if there is an active alarm dialog
 *
 * @return true if there is alarm, false otherwise
 */
static bool sg_have_alarm_dialog(void)
{
    bool res = false;

    switch( alarm_ui_state ) {
    case MCE_ALARM_UI_RINGING_INT32:
    case MCE_ALARM_UI_VISIBLE_INT32:
        res = true;
        break;
    default:
        break;
    }

    return res;
}

/** Helper for checking if there is an incoming call
 *
 * @return true if there is incoming call, false otherwise
 */
static bool sg_have_incoming_call(void)
{
    bool res = false;

    switch( call_state ) {
    case CALL_STATE_RINGING:
        res = true;
        break;
    default:
        break;
    }

    return res;
}

/** Handle call_state_pipe notifications
 *
 * @param data The call state stored in a pointer
 */
static void sg_call_state_cb(gconstpointer const data)
{
    call_state_t prev = call_state;
    call_state = GPOINTER_TO_INT(data);

    if( call_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "call: %s -> %s",
            call_state_repr(prev),
            call_state_repr(call_state));

    sg_detect_flipover_gesture();
EXIT:

    return;
}

/** Handle alarm_ui_state_pipe notifications
 *
 * @param data The alarm state stored in a pointer
 */
static void sg_alarm_ui_state_cb(gconstpointer data)
{
    alarm_ui_state_t prev =  alarm_ui_state;
    alarm_ui_state = GPOINTER_TO_INT(data);

    if( alarm_ui_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "alarm: %s -> %s",
            alarm_state_repr(prev),
            alarm_state_repr(alarm_ui_state));

    sg_detect_flipover_gesture();

EXIT:
    return;
}

/** Handle display_state_curr_pipe notifications
 *
 * @param data The display state stored in a pointer
 */
static void sg_display_state_curr_cb(gconstpointer data)
{
    display_state_t prev = display_state_curr;
    display_state_curr = GPOINTER_TO_INT(data);

    if( display_state_curr == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "display: %s -> %s",
            display_state_repr(prev),
            display_state_repr(display_state_curr));

    sg_detect_flipover_gesture();

EXIT:
    return;
}

/** Update effective orientation state from raw sensor state
 */
static void sg_orientation_sensor_update(void)
{
    orientation_state_t prev = orientation_sensor_effective;
    orientation_sensor_effective = orientation_sensor_actual;

    if( orientation_sensor_effective == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "orient.eff: %s -> %s",
            orientation_state_repr(prev),
            orientation_state_repr(orientation_sensor_effective));

    sg_detect_flipover_gesture();

EXIT:
    return;
}

/** Handle delayed orientation_sensor_actual_pipe notifications
 *
 * @param data (unused)
 *
 * @return FALSE to stop the timer from repeating
 */
static gboolean sg_orientation_sensor_effective_cb(gpointer data)
{
    (void)data;

    if( !orientation_sensor_effective_id )
        goto EXIT;

    mce_log(LL_DEBUG, "orient.eff: timer triggered");

    orientation_sensor_effective_id = 0;

    sg_orientation_sensor_update();

EXIT:
    return FALSE;
}

/** Handle orientation_sensor_actual_pipe notifications
 *
 * @param data The orientation state stored in a pointer
 */
static void sg_orientation_sensor_actual_cb(gconstpointer data)
{
    orientation_state_t prev = orientation_sensor_actual;
    orientation_sensor_actual = GPOINTER_TO_INT(data);

    if( orientation_sensor_actual == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "orient.raw: %s -> %s",
            orientation_state_repr(prev),
            orientation_state_repr(orientation_sensor_actual));

    /* Unprime if orientation is unknown */
    sg_detect_flipover_gesture();

    /* When the orientation sensor is stopped and restarted,
     * sensord reports initially the last state that was seen
     * before the sensor was stopped.
     *
     * To avoid false positives, the acceptance of face up
     * orientation after sensor startup is delayed by bit
     * more than what the sensor ramp up is expected to take.
     */

    /* Remove existing delay timer */
    if( orientation_sensor_effective_id ) {
        g_source_remove(orientation_sensor_effective_id);
        orientation_sensor_effective_id = 0;

        mce_log(LL_DEBUG, "orient.eff: timer canceled");
    }

    if( prev == MCE_ORIENTATION_UNDEFINED &&
        orientation_sensor_actual == MCE_ORIENTATION_FACE_UP ) {
        /* Invalidate effective sensor value */
        orientation_sensor_effective = MCE_ORIENTATION_UNDEFINED;

        /* Schedule re-validation after 1000 ms */
        orientation_sensor_effective_id =
            g_timeout_add(1000, sg_orientation_sensor_effective_cb, 0);

        mce_log(LL_DEBUG, "orient.eff: timer started");
    }
    else {
        /* Update effective sensor value immediately */
        sg_orientation_sensor_update();
    }

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t sg_datapipe_handlers[] =
{
    // input triggers
    {
        .datapipe  = &call_state_pipe,
        .input_cb  = sg_call_state_cb,
    },

    // output triggers
    {
        .datapipe  = &orientation_sensor_actual_pipe,
        .output_cb = sg_orientation_sensor_actual_cb,
    },
    {
        .datapipe  = &display_state_curr_pipe,
        .output_cb = sg_display_state_curr_cb,
    },
    {
        .datapipe  = &alarm_ui_state_pipe,
        .output_cb = sg_alarm_ui_state_cb,
    },

    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t sg_datapipe_bindings =
{
    .module   = "sensor-gestures",
    .handlers = sg_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void sg_datapipe_init(void)
{
    mce_datapipe_init_bindings(&sg_datapipe_bindings);
}

/** Remove triggers/filters from datapipes */
static void sg_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&sg_datapipe_bindings);
}

/* ========================================================================= *
 * DYNAMIC_SETTINGS
 * ========================================================================= */

/**
 * GConf callback for display related settings
 *
 * @param gcc Unused
 * @param id Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data Unused
 */
static void sg_setting_cb(GConfClient *const gcc, const guint id,
                          GConfEntry *const entry, gpointer const data)
{
    const GConfValue *gcv = gconf_entry_get_value(entry);

    (void)gcc;
    (void)data;

    /* Key is unset */
    if (gcv == NULL) {
        mce_log(LL_DEBUG, "GConf Key `%s' has been unset",
                gconf_entry_get_key(entry));
        goto EXIT;
    }

    if( id == sg_flipover_gesture_enabled_setting_id ) {
        sg_flipover_gesture_enabled = gconf_value_get_bool(gcv);
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }

EXIT:
    return;
}

/** Get initial setting values and start tracking changes
 */
static void sg_setting_init(void)
{
    mce_setting_track_bool(MCE_SETTING_FLIPOVER_GESTURE_ENABLED,
                           &sg_flipover_gesture_enabled,
                           MCE_DEFAULT_FLIPOVER_GESTURE_ENABLED,
                           sg_setting_cb,
                           &sg_flipover_gesture_enabled_setting_id);
}

/** Stop tracking setting changes */
static void sg_setting_quit(void)
{
    mce_setting_notifier_remove(sg_flipover_gesture_enabled_setting_id),
        sg_flipover_gesture_enabled_setting_id = 0;
}

/* ========================================================================= *
 * PLUGIN_LOAD_UNLOAD
 * ========================================================================= */

/** Init function for the sensor-gestures module
 *
 * @param module (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    sg_setting_init();
    sg_datapipe_init();

    return NULL;
}

/** Exit function for the sensor-gestures module
 *
 * @param module (not used)
 */
void g_module_unload(GModule *module)
{
    (void)module;

    sg_datapipe_quit();
    sg_setting_quit();

    return;
}
