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
#include "../mce-dbus.h"
#include "../mce-sensorfw.h"

#include <mce/dbus-names.h>

#include <gmodule.h>

/** Cached display state */
static display_state_t display_state = MCE_DISPLAY_UNDEF;

/** Cached alarm ui state */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_INVALID_INT32;

/** Cached call state */
static call_state_t call_state = CALL_STATE_INVALID;

/** Cached raw orientation sensor value */
static orientation_state_t orientation_state_raw = MCE_ORIENTATION_UNDEFINED;

/** Cached delayed orientation sensor value */
static orientation_state_t orientation_state_eff = MCE_ORIENTATION_UNDEFINED;

/** Timer id for delayed orientation_state_eff updating */
static gint orientation_state_eff_id = 0;

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

/** Helper for sending flipover dbus signal
 *
 * @param signal name
 */
static void sg_send_flipover_signal(const char *sig)
{
    // NOTE: introspection data shared with powerkey.c
    const char *arg = "flipover";
    mce_log(LL_DEVEL, "sending dbus signal: %s %s", sig, arg);
    dbus_send(0, MCE_SIGNAL_PATH, MCE_SIGNAL_IF,  sig, 0,
              DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
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
    if( display_state != MCE_DISPLAY_ON ) {
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
    if( orientation_state_raw == MCE_ORIENTATION_UNDEFINED ||
        orientation_state_eff == MCE_ORIENTATION_UNDEFINED ) {
        primed = false;
        goto EXIT;
    }

    /* Check effective orientation state */
    if( orientation_state_eff == MCE_ORIENTATION_FACE_UP ) {
        primed = true;
    }
    else if( orientation_state_eff != MCE_ORIENTATION_FACE_DOWN ) {
        // nop
    }
    else if( primed ) {
        primed = false;

        if( sg_have_alarm_dialog() )
            sg_send_flipover_signal("alarm_ui_feedback_ind");

        if( sg_have_incoming_call() )
            sg_send_flipover_signal("call_ui_feedback_ind");
    }

EXIT:

    return;
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

/** Handle display_state_pipe notifications
 *
 * @param data The display state stored in a pointer
 */
static void sg_display_state_cb(gconstpointer data)
{
    display_state_t prev = display_state;
    display_state = GPOINTER_TO_INT(data);

    if( display_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "display: %s -> %s",
            display_state_repr(prev),
            display_state_repr(display_state));

    sg_detect_flipover_gesture();

EXIT:
    return;
}

/** Update effective orientation state from raw sensor state
 */
static void sg_orientation_state_update(void)
{
    orientation_state_t prev = orientation_state_eff;
    orientation_state_eff = orientation_state_raw;

    if( orientation_state_eff == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "orient.eff: %s -> %s",
            orientation_state_repr(prev),
            orientation_state_repr(orientation_state_eff));

    sg_detect_flipover_gesture();

EXIT:
    return;
}

/** Handle delayed orientation_sensor_pipe notifications
 *
 * @param data (unused)
 *
 * @return FALSE to stop the timer from repeating
 */
static gboolean sg_orientation_state_eff_cb(gpointer data)
{
    (void)data;

    if( !orientation_state_eff_id )
        goto EXIT;

    mce_log(LL_DEBUG, "orient.eff: timer triggered");

    orientation_state_eff_id = 0;

    sg_orientation_state_update();

EXIT:
    return FALSE;
}

/** Handle orientation_sensor_pipe notifications
 *
 * @param data The orientation state stored in a pointer
 */
static void sg_orientation_state_raw_cb(gconstpointer data)
{
    orientation_state_t prev = orientation_state_raw;
    orientation_state_raw = GPOINTER_TO_INT(data);

    if( orientation_state_raw == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "orient.raw: %s -> %s",
            orientation_state_repr(prev),
            orientation_state_repr(orientation_state_raw));

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
    if( orientation_state_eff_id ) {
        g_source_remove(orientation_state_eff_id);
        orientation_state_eff_id = 0;

        mce_log(LL_DEBUG, "orient.eff: timer canceled");
    }

    if( prev == MCE_ORIENTATION_UNDEFINED &&
        orientation_state_raw == MCE_ORIENTATION_FACE_UP ) {
        /* Invalidate effective sensor value */
        orientation_state_eff = MCE_ORIENTATION_UNDEFINED;

        /* Schedule re-validation after 1000 ms */
        orientation_state_eff_id =
            g_timeout_add(1000, sg_orientation_state_eff_cb, 0);

        mce_log(LL_DEBUG, "orient.eff: timer started");
    }
    else {
        /* Update effective sensor value immediately */
        sg_orientation_state_update();
    }

EXIT:
    return;
}

/** Init function for the sensor-gestures module
 *
 * @param module (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    /* Add datapipe triggers */
    append_output_trigger_to_datapipe(&orientation_sensor_pipe,
                                      sg_orientation_state_raw_cb);
    append_output_trigger_to_datapipe(&display_state_pipe,
                                      sg_display_state_cb);
    append_output_trigger_to_datapipe(&alarm_ui_state_pipe,
                                      sg_alarm_ui_state_cb);
    append_input_trigger_to_datapipe(&call_state_pipe,
                                     sg_call_state_cb);
    return NULL;
}

/** Exit function for the sensor-gestures module
 *
 * @param module (not used)
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
    (void)module;

    /* Remove datapipe triggers */
    remove_output_trigger_from_datapipe(&orientation_sensor_pipe,
                                        sg_orientation_state_raw_cb);
    remove_output_trigger_from_datapipe(&display_state_pipe,
                                        sg_display_state_cb);
    remove_output_trigger_from_datapipe(&alarm_ui_state_pipe,
                                        sg_alarm_ui_state_cb);
    remove_input_trigger_from_datapipe(&call_state_pipe,
                                       sg_call_state_cb);
    return;
}
