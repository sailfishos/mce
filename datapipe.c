/**
 * @file datapipe.c
 * This file implements the sinmple datapipe framework;
 * this can be used to filter data and to setup data triggers
 * <p>
 * Copyright © 2007-2008 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2014-2019 Jolla Ltd.
 * Copyright (c) 2019 Open Mobile Platform LLC.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
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

#include "mce.h"
#include "mce-log.h"
#include "mce-lib.h"
#include "evdev.h"

#include <mce/mode-names.h>

#include <linux/input.h>

#include <stdio.h>

/* ========================================================================= *
 * Macros
 * ========================================================================= */

#define cat2(a,b) a##b
#define cat3(a,b,c) a##b##c

#define datapipe_log(PIPE_, FMT_, ARGS_...)\
     do {\
         if( mce_log_p(LL_DEBUG) || \
             mce_log_p_(LL_DEBUG,__FILE__,PIPE_->dp_name) ) {\
             mce_log_unconditional(LL_DEBUG, __FILE__, __func__,\
                          FMT_ , ## ARGS_);\
         }\
     } while(0)

/* ========================================================================= *
 * Types
 * ========================================================================= */

struct datapipe_t
{
    const char           *dp_name;             /**< Name of the datapipe */
    GSList               *dp_filters;          /**< The filters */
    GSList               *dp_input_triggers;   /**< Triggers called on indata */
    GSList               *dp_output_triggers;  /**< Triggers called on outdata */
    gconstpointer         dp_cached_data;      /**< Latest cached data */
    gsize                 dp_datasize;         /**< Size of data; NULL == automagic */
    datapipe_filtering_t  dp_read_only;        /**< Datapipe is read only */
    datapipe_cache_t      dp_cache;
    guint                 dp_gc_id;
    guint                 dp_token;
    const char         *(*dp_value_repr_cb)(gconstpointer value);
    const char         *(*dp_change_repr_cb)(gconstpointer prev, gconstpointer curr);
};

#define DATAPIPE_INIT(NAME_,TYPE_,VALUE_,SIZE_,FILTERING_,CACHING_)\
     {\
         .dp_name = #NAME_ "_pipe",\
         .dp_filters = 0,\
         .dp_input_triggers = 0,\
         .dp_output_triggers = 0,\
         .dp_cached_data = GINT_TO_POINTER(VALUE_),\
         .dp_datasize = SIZE_,\
         .dp_read_only = FILTERING_,\
         .dp_cache = CACHING_,\
         .dp_gc_id = 0,\
         .dp_token = 0,\
         .dp_value_repr_cb = cat3(datapipe_hook_,TYPE_,_value),\
         .dp_change_repr_cb = cat3(datapipe_hook_,TYPE_,_change),\
     }

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * DATAPIPE_HOOK
 * ------------------------------------------------------------------------- */

static const char  *datapipe_hook_int_value                (gconstpointer data);
static const char  *datapipe_hook_boolean_value            (gconstpointer data);
static const char  *datapipe_hook_string_value             (gconstpointer data);
static const char  *datapipe_hook_impulse_value            (gconstpointer data);
static const char  *datapipe_hook_input_event_value        (gconstpointer data);
static const char  *datapipe_hook_input_event_ptr_value    (gconstpointer data);
static const char  *datapipe_hook_display_state_value      (gconstpointer data);
static const char  *datapipe_hook_uiexception_type_value   (gconstpointer data);
static const char  *datapipe_hook_lockkey_state_value      (gconstpointer data);
static const char  *datapipe_hook_tristate_value           (gconstpointer data);
static const char  *datapipe_hook_cover_state_value        (gconstpointer data);
static const char  *datapipe_hook_orientation_state_value  (gconstpointer data);
static const char  *datapipe_hook_alarm_ui_state_value     (gconstpointer data);
static const char  *datapipe_hook_system_state_value       (gconstpointer data);
static const char  *datapipe_hook_submode_value            (gconstpointer data);
static const char  *datapipe_hook_submode_change           (gconstpointer prev, gconstpointer curr);
static const char  *datapipe_hook_call_state_value         (gconstpointer data);
static const char  *datapipe_hook_call_type_value          (gconstpointer data);
static const char  *datapipe_hook_tklock_request_value     (gconstpointer data);
static const char  *datapipe_hook_charger_state_value      (gconstpointer data);
static const char  *datapipe_hook_charger_type_value       (gconstpointer data);
static const char  *datapipe_hook_battery_status_value     (gconstpointer data);
static const char  *datapipe_hook_battery_state_value      (gconstpointer data);
static const char  *datapipe_hook_camera_button_state_value(gconstpointer data);
static const char  *datapipe_hook_audio_route_value        (gconstpointer data);
static const char  *datapipe_hook_usb_cable_state_value    (gconstpointer data);
static const char  *datapipe_hook_thermal_state_value      (gconstpointer data);
static const char  *datapipe_hook_service_state_value      (gconstpointer data);
static const char  *datapipe_hook_devicelock_state_value   (gconstpointer data);
static const char  *datapipe_hook_fpstate_value            (gconstpointer data);

/* ------------------------------------------------------------------------- *
 * DATAPIPE
 * ------------------------------------------------------------------------- */

const char       *datapipe_name                 (const datapipe_t *self);
gconstpointer     datapipe_value                (const datapipe_t *self);
void              datapipe_set_value            (datapipe_t *self, gconstpointer data);
static void       datapipe_gc                   (datapipe_t *self);
static gboolean   datapipe_gc_cb                (gpointer aptr);
static void       datapipe_schedule_gc          (datapipe_t *self);
static void       datapipe_cancel_gc            (datapipe_t *self);
gconstpointer     datapipe_exec_full_real       (datapipe_t *self, gconstpointer indata, const char *file, const char *func);
static void       datapipe_add_filter           (datapipe_t *self, gpointer (*filter)(gpointer data));
static void       datapipe_remove_filter        (datapipe_t *self, gpointer (*filter)(gpointer data));
static void       datapipe_add_input_trigger    (datapipe_t *self, void (*trigger)(gconstpointer data));
static void       datapipe_remove_input_trigger (datapipe_t *self, void (*trigger)(gconstpointer data));
static void       datapipe_add_output_trigger   (datapipe_t *self, void (*trigger)(gconstpointer data));
static void       datapipe_remove_output_trigger(datapipe_t *self, void (*trigger)(gconstpointer data));
static void       datapipe_free                 (datapipe_t *self);

/* ------------------------------------------------------------------------- *
 * MCE_DATAPIPE
 * ------------------------------------------------------------------------- */

void             mce_datapipe_init               (void);
void             mce_datapipe_quit               (void);
static void      mce_datapipe_install_handlers   (datapipe_handler_t *bindings);
static void      mce_datapipe_remove_handlers    (datapipe_handler_t *bindings);
static void      mce_datapipe_execute_handlers   (datapipe_handler_t *bindings);
static gboolean  mce_datapipe_execute_handlers_cb(gpointer aptr);
void             mce_datapipe_init_bindings      (datapipe_bindings_t *self);
void             mce_datapipe_quit_bindings      (datapipe_bindings_t *self);
void             mce_datapipe_generate_activity  (void);
void             mce_datapipe_generate_inactivity(void);

/* ------------------------------------------------------------------------- *
 * SUBMODE
 * ------------------------------------------------------------------------- */

const char  *submode_change_repr(submode_t prev, submode_t curr);
const char  *submode_repr       (submode_t submode);

/* ------------------------------------------------------------------------- *
 * SYSTEM_STATE
 * ------------------------------------------------------------------------- */

const char  *system_state_repr(system_state_t state);

/* ------------------------------------------------------------------------- *
 * DEVICELOCK_STATE
 * ------------------------------------------------------------------------- */

const char  *devicelock_state_repr(devicelock_state_t state);

/* ------------------------------------------------------------------------- *
 * THERMAL_STATE
 * ------------------------------------------------------------------------- */

const char  *thermal_state_repr(thermal_state_t state);

/* ------------------------------------------------------------------------- *
 * UIEXCEPTION_TYPE
 * ------------------------------------------------------------------------- */

const char  *uiexception_type_repr   (uiexception_type_t type);
const char  *uiexception_type_to_dbus(uiexception_type_t type);

/* ------------------------------------------------------------------------- *
 * SERVICE_STATE
 * ------------------------------------------------------------------------- */

const char  *service_state_repr(service_state_t state);

/* ------------------------------------------------------------------------- *
 * USB_CABLE_STATE
 * ------------------------------------------------------------------------- */

const char  *usb_cable_state_repr   (usb_cable_state_t state);
const char  *usb_cable_state_to_dbus(usb_cable_state_t state);

/* ------------------------------------------------------------------------- *
 * CHARGER_TYPE
 * ------------------------------------------------------------------------- */

const char  *charger_type_repr   (charger_type_t type);
const char  *charger_type_to_dbus(charger_type_t type);

/* ------------------------------------------------------------------------- *
 * CHARGER_STATE
 * ------------------------------------------------------------------------- */

const char  *charger_state_repr   (charger_state_t state);
const char  *charger_state_to_dbus(charger_state_t state);

/* ------------------------------------------------------------------------- *
 * CAMERA_BUTTON_STATE
 * ------------------------------------------------------------------------- */

const char  *camera_button_state_repr(camera_button_state_t state);

/* ------------------------------------------------------------------------- *
 * AUDIO_ROUTE
 * ------------------------------------------------------------------------- */

const char  *audio_route_repr(audio_route_t state);

/* ------------------------------------------------------------------------- *
 * TKLOCK_REQUEST
 * ------------------------------------------------------------------------- */

const char  *tklock_request_repr(tklock_request_t state);

/* ------------------------------------------------------------------------- *
 * TKLOCK_STATUS
 * ------------------------------------------------------------------------- */

const char  *tklock_status_repr(int status);

/* ------------------------------------------------------------------------- *
 * BATTERY_STATUS
 * ------------------------------------------------------------------------- */

const char  *battery_status_repr   (battery_status_t state);
const char  *battery_status_to_dbus(battery_status_t state);

/* ------------------------------------------------------------------------- *
 * BATTERY_STATE
 * ------------------------------------------------------------------------- */

const char  *battery_state_repr   (battery_state_t state);
const char  *battery_state_to_dbus(battery_state_t state);

/* ------------------------------------------------------------------------- *
 * ALARM_STATE
 * ------------------------------------------------------------------------- */

const char  *alarm_state_repr(alarm_ui_state_t state);

/* ------------------------------------------------------------------------- *
 * CALL_STATE
 * ------------------------------------------------------------------------- */

const char    *call_state_to_dbus  (call_state_t state);
call_state_t   call_state_from_dbus(const char *name);
const char    *call_state_repr     (call_state_t state);

/* ------------------------------------------------------------------------- *
 * CALL_TYPE
 * ------------------------------------------------------------------------- */

const char   *call_type_repr (call_type_t type);
call_type_t   call_type_parse(const char *name);

/* ------------------------------------------------------------------------- *
 * COVER_STATE
 * ------------------------------------------------------------------------- */

const char  *cover_state_repr(cover_state_t state);

/* ------------------------------------------------------------------------- *
 * PROXIMITY_STATE
 * ------------------------------------------------------------------------- */

const char  *proximity_state_repr(cover_state_t state);

/* ------------------------------------------------------------------------- *
 * DISPLAY_STATE
 * ------------------------------------------------------------------------- */

const char  *display_state_repr(display_state_t state);

/* ------------------------------------------------------------------------- *
 * ORIENTATION_STATE
 * ------------------------------------------------------------------------- */

const char  *orientation_state_repr(orientation_state_t state);

/* ------------------------------------------------------------------------- *
 * KEY_STATE
 * ------------------------------------------------------------------------- */

const char  *key_state_repr(key_state_t state);

/* ------------------------------------------------------------------------- *
 * TRISTATE
 * ------------------------------------------------------------------------- */

const char  *tristate_repr(tristate_t state);

/* ------------------------------------------------------------------------- *
 * FPSTATE
 * ------------------------------------------------------------------------- */

fpstate_t    fpstate_parse(const char *name);
const char  *fpstate_repr (fpstate_t state);

/* ========================================================================= *
 * DATAPIPE_HOOK
 * ========================================================================= */

/* For each datatype used in datapipes there exist:
 * - datapipe_hook_<type>_value
 * - datapipe_hook_<type>_changes
 *
 * These are used in expansion of #DATAPIPE_INIT() macro and must resolve
 * either to a valid callback function or null pointer.
 */

static const char *
datapipe_hook_int_value(gconstpointer data)
{
    static char buf[32];
    snprintf(buf, sizeof buf, "%d", GPOINTER_TO_INT(data));
    return buf;
}
#define datapipe_hook_int_change 0

static const char *
datapipe_hook_boolean_value(gconstpointer data)
{
    return data ? "true" : "false";
}
#define datapipe_hook_boolean_change 0

static const char *
datapipe_hook_string_value(gconstpointer data)
{
    return data;
}
#define datapipe_hook_string_change 0

static const char *
datapipe_hook_impulse_value(gconstpointer data)
{
    (void)data;
    return "impulse";
}
#define datapipe_hook_impulse_change 0

static const char *
datapipe_hook_input_event_value(gconstpointer data)
{
    static char buf[64];
    const struct input_event *ev = data;
    if( !ev )
        return "NULL event!";
    snprintf(buf, sizeof buf, "type: %s, code: %s, value: %d",
             evdev_get_event_type_name(ev->type),
             evdev_get_event_code_name(ev->type, ev->code),
             ev->value);
    return buf;
}
#define datapipe_hook_input_event_change 0

static const char *
datapipe_hook_input_event_ptr_value(gconstpointer data)
{
    const struct input_event *const*evp = data;
    if( !evp )
        return "NULL event ptr!";
    return datapipe_hook_input_event_value(*evp);
}
#define datapipe_hook_input_event_ptr_change 0

static const char *
datapipe_hook_display_state_value(gconstpointer data)
{
    display_state_t display_state = GPOINTER_TO_INT(data);
    return display_state_repr(display_state);
}
#define datapipe_hook_display_state_change 0

static const char *
datapipe_hook_uiexception_type_value(gconstpointer data)
{
    uiexception_type_t value = GPOINTER_TO_INT(data);
    return uiexception_type_repr(value);
}
#define datapipe_hook_uiexception_type_change 0

static const char *
datapipe_hook_lockkey_state_value(gconstpointer data)
{
    key_state_t key_state = GPOINTER_TO_INT(data);
    return key_state_repr(key_state);
}
#define datapipe_hook_lockkey_state_change 0

static const char *
datapipe_hook_tristate_value(gconstpointer data)
{
    tristate_t value = GPOINTER_TO_INT(data);
    return tristate_repr(value);
}
#define datapipe_hook_tristate_change 0

static const char *
datapipe_hook_cover_state_value(gconstpointer data)
{
    cover_state_t value = GPOINTER_TO_INT(data);
    return cover_state_repr(value);
}
#define datapipe_hook_cover_state_change 0

static const char *
datapipe_hook_orientation_state_value(gconstpointer data)
{
    orientation_state_t value = GPOINTER_TO_INT(data);
    return orientation_state_repr(value);
}
#define datapipe_hook_orientation_state_change 0

static const char *
datapipe_hook_alarm_ui_state_value(gconstpointer data)
{
    alarm_ui_state_t value = GPOINTER_TO_INT(data);
    return alarm_state_repr(value);
}
#define datapipe_hook_alarm_ui_state_change 0

static const char *
datapipe_hook_system_state_value(gconstpointer data)
{
    system_state_t value = GPOINTER_TO_INT(data);
    return system_state_repr(value);
}
#define datapipe_hook_system_state_change 0

static const char *
datapipe_hook_submode_value(gconstpointer data)
{
    submode_t value = GPOINTER_TO_INT(data);
    return submode_repr(value);
}
static const char *
datapipe_hook_submode_change(gconstpointer prev, gconstpointer curr)
{
    submode_t v1 = GPOINTER_TO_INT(prev);
    submode_t v2 = GPOINTER_TO_INT(curr);
    return submode_change_repr(v1, v2);
}
static const char *
datapipe_hook_call_state_value(gconstpointer data)
{
    call_state_t value = GPOINTER_TO_INT(data);
    return call_state_repr(value);
}
#define datapipe_hook_call_state_change 0

static const char *
datapipe_hook_call_type_value(gconstpointer data)
{
    call_type_t value = GPOINTER_TO_INT(data);
    return call_type_repr(value);
}
#define datapipe_hook_call_type_change 0

static const char *
datapipe_hook_tklock_request_value(gconstpointer data)
{
    tklock_request_t value = GPOINTER_TO_INT(data);
    return tklock_request_repr(value);
}
#define datapipe_hook_tklock_request_change 0

static const char *
datapipe_hook_charger_type_value(gconstpointer data)
{
    charger_type_t value = GPOINTER_TO_INT(data);
    return charger_type_repr(value);
}
#define datapipe_hook_charger_type_change 0

static const char *
datapipe_hook_charger_state_value(gconstpointer data)
{
    charger_state_t value = GPOINTER_TO_INT(data);
    return charger_state_repr(value);
}
#define datapipe_hook_charger_state_change 0

static const char *
datapipe_hook_battery_status_value(gconstpointer data)
{
    battery_status_t value = GPOINTER_TO_INT(data);
    return battery_status_repr(value);
}
#define datapipe_hook_battery_status_change 0

static const char *
datapipe_hook_battery_state_value(gconstpointer data)
{
    battery_state_t value = GPOINTER_TO_INT(data);
    return battery_state_repr(value);
}
#define datapipe_hook_battery_state_change 0

static const char *
datapipe_hook_camera_button_state_value(gconstpointer data)
{
    camera_button_state_t value = GPOINTER_TO_INT(data);
    return camera_button_state_repr(value);
}
#define datapipe_hook_camera_button_state_change 0

static const char *
datapipe_hook_audio_route_value(gconstpointer data)
{
    audio_route_t value = GPOINTER_TO_INT(data);
    return audio_route_repr(value);
}
#define datapipe_hook_audio_route_change 0

static const char *
datapipe_hook_usb_cable_state_value(gconstpointer data)
{
    usb_cable_state_t value = GPOINTER_TO_INT(data);
    return usb_cable_state_repr(value);
}
#define datapipe_hook_usb_cable_state_change 0

static const char *
datapipe_hook_thermal_state_value(gconstpointer data)
{
    thermal_state_t value = GPOINTER_TO_INT(data);
    return thermal_state_repr(value);
}
#define datapipe_hook_thermal_state_change 0

static const char *
datapipe_hook_service_state_value(gconstpointer data)
{
    service_state_t value = GPOINTER_TO_INT(data);
    return service_state_repr(value);
}
#define datapipe_hook_service_state_change 0

static const char *
datapipe_hook_devicelock_state_value(gconstpointer data)
{
    devicelock_state_t value = GPOINTER_TO_INT(data);
    return devicelock_state_repr(value);
}
#define datapipe_hook_devicelock_state_change 0

static const char *
datapipe_hook_fpstate_value(gconstpointer data)
{
    fpstate_t value = GPOINTER_TO_INT(data);
    return fpstate_repr(value);
}
#define datapipe_hook_fpstate_change 0

/* ========================================================================= *
 * Data
 * ========================================================================= */

/* Available datapipes */

/** LED brightness */
datapipe_t led_brightness_pipe                  = DATAPIPE_INIT(led_brightness, int, 0, 0, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_CACHE_INDATA);

/** LPM brightness */
datapipe_t lpm_brightness_pipe                  = DATAPIPE_INIT(lpm_brightness, int, 0, 0, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_CACHE_INDATA);

/** State of device; read only */
datapipe_t device_inactive_pipe                 = DATAPIPE_INIT(device_inactive, boolean, true, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Device inactivity events; read only */
datapipe_t inactivity_event_pipe                = DATAPIPE_INIT(inactivity_event, boolean, true, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** LED pattern to activate; read only */
datapipe_t led_pattern_activate_pipe            = DATAPIPE_INIT(led_pattern_activate, string, 0, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_NOTHING);

/** LED pattern to deactivate; read only */
datapipe_t led_pattern_deactivate_pipe          = DATAPIPE_INIT(led_pattern_deactivate, string, 0, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_NOTHING);

/** resumed from suspend notification; read only */
datapipe_t resume_detected_event_pipe           = DATAPIPE_INIT(resume_detected_event, impulse, 0, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_NOTHING);

/** Non-synthetized user activity; read only */
datapipe_t user_activity_event_pipe             = DATAPIPE_INIT(user_activity_event, input_event, 0, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_NOTHING);

/** State of display; read only */
datapipe_t display_state_curr_pipe              = DATAPIPE_INIT(display_state_curr, display_state, MCE_DISPLAY_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Desired state of display; write only */
datapipe_t display_state_request_pipe           = DATAPIPE_INIT(display_state_request, display_state, MCE_DISPLAY_UNDEF, 0, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_CACHE_DEFAULT);

/** Next (non-transitional) state of display; read only */
datapipe_t display_state_next_pipe              = DATAPIPE_INIT(display_state_next, display_state, MCE_DISPLAY_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** exceptional ui state; read write */
datapipe_t uiexception_type_pipe                = DATAPIPE_INIT(uiexception_type, uiexception_type, UIEXCEPTION_TYPE_NONE, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/**
 * Display brightness;
 * bits 0-7 is brightness in percent (0-100)
 * upper 8 bits is high brightness boost (0-2)
 */
datapipe_t display_brightness_pipe              = DATAPIPE_INIT(display_brightness, int, 3, 0, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_CACHE_INDATA);

/** Key backlight */
datapipe_t key_backlight_brightness_pipe        = DATAPIPE_INIT(key_backlight_brightness, int, 0, 0, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_CACHE_INDATA);

/** A key has been pressed */
datapipe_t keypress_event_pipe                  = DATAPIPE_INIT(keypress_event, input_event_ptr, 0, sizeof (struct input_event), DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_NOTHING);

/** Touchscreen activity took place */
datapipe_t touchscreen_event_pipe               = DATAPIPE_INIT(touchscreen_event, input_event_ptr, 0, sizeof (struct input_event), DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_NOTHING);

/** The lock-key has been pressed; read only */
datapipe_t lockkey_state_pipe                   = DATAPIPE_INIT(lockkey_state, lockkey_state, KEY_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** The init-done condition has been reached; read only */
datapipe_t init_done_pipe                       = DATAPIPE_INIT(init_done, tristate, TRISTATE_UNKNOWN, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Keyboard open/closed; read only */
datapipe_t keyboard_slide_state_pipe            = DATAPIPE_INIT(keyboard_slide_state, cover_state, COVER_CLOSED, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Keyboard available; read only */
datapipe_t keyboard_available_state_pipe        = DATAPIPE_INIT(keyboard_available_state, cover_state, COVER_CLOSED, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Lid sensor is working state; read/write */
datapipe_t lid_sensor_is_working_pipe           = DATAPIPE_INIT(lid_sensor_is_working, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Lid cover sensor open/closed; read only */
datapipe_t lid_sensor_actual_pipe               = DATAPIPE_INIT(lid_sensor_actual, cover_state, COVER_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Lid cover policy state; read only */
datapipe_t lid_sensor_filtered_pipe             = DATAPIPE_INIT(lid_sensor_filtered, cover_state, COVER_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Lens cover open/closed; read only */
datapipe_t lens_cover_state_pipe                = DATAPIPE_INIT(lens_cover_state, cover_state, COVER_CLOSED, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Proximity sensor; read only */
datapipe_t proximity_sensor_actual_pipe         = DATAPIPE_INIT(proximity_sensor_actual, cover_state, COVER_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Proximity sensor; read only */
datapipe_t proximity_sensor_effective_pipe      = DATAPIPE_INIT(proximity_sensor_effective, cover_state, COVER_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Proximity sensor on-demand control; write only
 *
 * The data fed into proximity_sensor_required_pipe needs to
 * be a statically allocated const string with format like:
 *
 *   "<PREFIX><NAME>"
 *
 * Where <PREFIX> is one of:
 *
 * - PROXIMITY_SENSOR_REQUIRED_ADD: <NAME> is added to the
 *   list of resons to keep on-demand proximity sensor mode
 *   active.
 *
 * - PROXIMITY_SENSOR_REQUIRED_REM: <NAME> is from the the
 *   list of resons to keep on-demand proximity sensor mode
 *   active.
 */
datapipe_t proximity_sensor_required_pipe       = DATAPIPE_INIT(proximity_sensor_required, string, 0, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_NOTHING);

/** proximity blanking; read only */
datapipe_t proximity_blanked_pipe               = DATAPIPE_INIT(proximity_blanked, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Ambient light sensor; read only */
datapipe_t light_sensor_actual_pipe             = DATAPIPE_INIT(light_sensor_actual, int, 400, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Filtered ambient light level; read only */
datapipe_t light_sensor_filtered_pipe           = DATAPIPE_INIT(light_sensor_filtered, int, 400, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Temporary ambient light sensor enable; read/write */
datapipe_t light_sensor_poll_request_pipe       = DATAPIPE_INIT(light_sensor_poll_request, boolean, false, 0, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_CACHE_DEFAULT);

/** Orientation sensor; read only */
datapipe_t orientation_sensor_actual_pipe       = DATAPIPE_INIT(orientation_sensor_actual, orientation_state, MCE_ORIENTATION_UNDEFINED, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** The alarm UI state */
datapipe_t alarm_ui_state_pipe                  = DATAPIPE_INIT(alarm_ui_state, alarm_ui_state, MCE_ALARM_UI_INVALID_INT32, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** The device state; read only */
datapipe_t system_state_pipe                    = DATAPIPE_INIT(system_state, system_state, MCE_SYSTEM_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Enable/disable radios */
datapipe_t master_radio_enabled_pipe            = DATAPIPE_INIT(master_radio_enabled, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** The device submode */
datapipe_t submode_pipe                         = DATAPIPE_INIT(submode, submode, MCE_SUBMODE_NORMAL, 0, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_CACHE_DEFAULT);

/** The call state */
datapipe_t call_state_pipe                      = DATAPIPE_INIT(call_state, call_state, CALL_STATE_NONE, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** The ignore incoming call "state".
 *
 * Note: Related actions happen when true is executed on the
 * datapipe, but the cached value always remains at false.
 */
datapipe_t ignore_incoming_call_event_pipe      = DATAPIPE_INIT(ignore_incoming_call_event, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_NOTHING);

/** The call type */
datapipe_t call_type_pipe                       = DATAPIPE_INIT(call_type, call_type, CALL_TYPE_NORMAL, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** The touchscreen/keypad lock state */
datapipe_t tklock_request_pipe                  = DATAPIPE_INIT(tklock_request, tklock_request, TKLOCK_REQUEST_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** UI side is in a state where user interaction is expected */
datapipe_t interaction_expected_pipe            = DATAPIPE_INIT(interaction_expected, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Charger type; read only */
datapipe_t charger_type_pipe                    = DATAPIPE_INIT(charger_type, charger_type, CHARGER_TYPE_NONE, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Charger state; read only */
datapipe_t charger_state_pipe                   = DATAPIPE_INIT(charger_state, charger_state, CHARGER_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Battery status; read only */
datapipe_t battery_status_pipe                  = DATAPIPE_INIT(battery_status, battery_status, BATTERY_STATUS_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Battery state; read only */
datapipe_t battery_state_pipe                   = DATAPIPE_INIT(battery_state, battery_state, BATTERY_STATE_UNKNOWN, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Battery charge level; read only */
datapipe_t battery_level_pipe                   = DATAPIPE_INIT(battery_level, int, BATTERY_LEVEL_INITIAL, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Topmost window PID; read only */
datapipe_t topmost_window_pid_pipe              = DATAPIPE_INIT(topmost_window_pid, int, -1, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Camera button; read only */
datapipe_t camera_button_state_pipe             = DATAPIPE_INIT(camera_button_state, camera_button_state, CAMERA_BUTTON_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** The inactivity timeout; read only */
datapipe_t inactivity_delay_pipe                = DATAPIPE_INIT(inactivity_delay, int, DEFAULT_INACTIVITY_DELAY, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Audio routing state; read only */
datapipe_t audio_route_pipe                     = DATAPIPE_INIT(audio_route, audio_route, AUDIO_ROUTE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** USB cable has been connected/disconnected; read only */
datapipe_t usb_cable_state_pipe                 = DATAPIPE_INIT(usb_cable_state, usb_cable_state, USB_CABLE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** A jack connector has been connected/disconnected; read only */
datapipe_t jack_sense_state_pipe                = DATAPIPE_INIT(jack_sense_state, cover_state, COVER_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Power save mode is active; read only */
datapipe_t power_saving_mode_active_pipe        = DATAPIPE_INIT(power_saving_mode_active, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Thermal state; read only */
datapipe_t thermal_state_pipe                   = DATAPIPE_INIT(thermal_state, thermal_state, THERMAL_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Heartbeat; read only */
datapipe_t heartbeat_event_pipe                 = DATAPIPE_INIT(heartbeat_event, impulse, 0, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_NOTHING);

/** compositor availability; read only */
datapipe_t compositor_service_state_pipe        = DATAPIPE_INIT(compositor_service_state, service_state, SERVICE_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** lipstick availability; read only */
datapipe_t lipstick_service_state_pipe          = DATAPIPE_INIT(lipstick_service_state, service_state, SERVICE_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** devicelock availability; read only */
datapipe_t devicelock_service_state_pipe        = DATAPIPE_INIT(devicelock_service_state, service_state, SERVICE_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** usbmoded availability; read only */
datapipe_t usbmoded_service_state_pipe          = DATAPIPE_INIT(usbmoded_service_state, service_state, SERVICE_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** ngfd availability; read only */
datapipe_t ngfd_service_state_pipe              = DATAPIPE_INIT(ngfd_service_state, service_state, SERVICE_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Request ngfd event playing */
datapipe_t ngfd_event_request_pipe              = DATAPIPE_INIT(ngfd_event_request, string, 0, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_NOTHING);

/** dsme availability; read only */
datapipe_t dsme_service_state_pipe              = DATAPIPE_INIT(dsme_service_state, service_state, SERVICE_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** thermalmanager availability; read only */
datapipe_t thermalmanager_service_state_pipe    = DATAPIPE_INIT(thermalmanager_service_state, service_state, SERVICE_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** bluez availability; read only */
datapipe_t bluez_service_state_pipe             = DATAPIPE_INIT(bluez_service_state, service_state, SERVICE_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** PackageKit Locked status; read only */
datapipe_t packagekit_locked_pipe               = DATAPIPE_INIT(packagekit_locked, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Update mode active status; read only */
datapipe_t osupdate_running_pipe                = DATAPIPE_INIT(osupdate_running, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Shutting down; read only */
datapipe_t shutting_down_pipe                   = DATAPIPE_INIT(shutting_down, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** Device Lock state; read only */
datapipe_t devicelock_state_pipe                = DATAPIPE_INIT(devicelock_state, devicelock_state, DEVICELOCK_STATE_UNDEFINED, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** touchscreen input detected; read only */
datapipe_t touch_detected_pipe                  = DATAPIPE_INIT(touch_detected, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** touchscreen input grab required; read/write */
datapipe_t touch_grab_wanted_pipe               = DATAPIPE_INIT(touch_grab_wanted, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** touchscreen input grab active; read only */
datapipe_t touch_grab_active_pipe               = DATAPIPE_INIT(touch_grab_active, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** keypad input grab required; read/write */
datapipe_t keypad_grab_wanted_pipe              = DATAPIPE_INIT(keypad_grab_wanted, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** keypad input grab active; read only */
datapipe_t keypad_grab_active_pipe              = DATAPIPE_INIT(keypad_grab_active, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** music playback active; read only */
datapipe_t music_playback_ongoing_pipe          = DATAPIPE_INIT(music_playback_ongoing, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** fingerprint daemon availability; read only */
datapipe_t fpd_service_state_pipe               = DATAPIPE_INIT(fpd_service_state, service_state, SERVICE_STATE_UNDEF, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** fingerprint daemon state; read only */
datapipe_t fpstate_pipe                         = DATAPIPE_INIT(fpstate, fpstate, FPSTATE_UNSET, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/** fingerprint is enrolling; read only */
datapipe_t enroll_in_progress_pipe              = DATAPIPE_INIT(enroll_in_progress, boolean, false, 0, DATAPIPE_FILTERING_DENIED, DATAPIPE_CACHE_DEFAULT);

/* ========================================================================= *
 * Functions
 * ========================================================================= */

/** Get name of datapipe
 *
 * @param self The datapipe, or NULL
 *
 * @return datapipe name, or suitable placeholder value
 */
const char *
datapipe_name(const datapipe_t *self)
{
    const char *name = "invalid";
    if( self )
        name = self->dp_name ?: "unnamed";
    return name;
}

/** Get value of datapipe
 *
 * @param self The datapipe
 *
 * @return datapipe value, as void pointer
 */
gconstpointer
datapipe_value(const datapipe_t *self)
{
    return self->dp_cached_data;
}

/** Set value of datapipe
 *
 * Note: Do not call this function from outside datapipe.c.
 *
 * While such calls exist to facilitate legacy hacks in the
 * display plugin - no new code like that should be added.
 *
 * FIXME: Remove offending call sites and make this function
 *        static.
 *
 * The proper way to modify datapipe content is to use
 * #datapipe_exec_full() function.
 *
 * @param self The datapipe
 * @paran data The value, as void pointer
 */
void
datapipe_set_value(datapipe_t *self, gconstpointer data)
{
    gconstpointer prev = self->dp_cached_data;
    self->dp_cached_data = data;

    if( prev != data ) {
        if( self->dp_change_repr_cb ) {
            datapipe_log(self, "%s: %s",
                         datapipe_name(self),
                         self->dp_change_repr_cb(prev, data));
        }
        else if( self->dp_value_repr_cb ) {
            gchar *saved = g_strdup(self->dp_value_repr_cb(prev));
            datapipe_log(self, "%s: %s -> %s",
                         datapipe_name(self),
                         saved,
                         self->dp_value_repr_cb(data));
            g_free(saved);
        }
        else {
            datapipe_log(self, "%s: %p -> %p",
                         datapipe_name(self),
                         prev, data);
        }
    }
}

/** Garbage collect stale callback slots
 *
 * While it can be assumed to be extremely rare, filters and triggers can end
 * up being added/removed due to datapipe activity. In order to allow O(N)
 * traversal in datapipe_exec_full() slots with removed callbacks are
 * zeroed and left behind to be collected from idle callback.
 *
 * @param self The datapipe
 */
static void
datapipe_gc(datapipe_t *self)
{
    /* Cancel pending idle callback */
    datapipe_cancel_gc(self);

    /* Flush out slots with null callback */
    self->dp_input_triggers  = g_slist_remove_all(self->dp_input_triggers, 0);
    self->dp_filters         = g_slist_remove_all(self->dp_filters, 0);
    self->dp_output_triggers = g_slist_remove_all(self->dp_output_triggers, 0);
}

/** Garbage collect idle callback
 *
 * @param self The datapipe, as void pointer
 *
 * @return G_SOURCE_REMOVE
 */
static gboolean
datapipe_gc_cb(gpointer aptr)
{
    datapipe_t *self = aptr;
    self->dp_gc_id = 0;
    datapipe_gc(self);
    return G_SOURCE_REMOVE;
}

/** Schedule garbage collection
 *
 * @param self The datapipe, as void pointer
 */
static void
datapipe_schedule_gc(datapipe_t *self)
{
    if( !self->dp_gc_id ) {
        self->dp_gc_id = g_idle_add(datapipe_gc_cb, self);
    }
}

/** Cancel garbage collection
 *
 * @param self The datapipe, as void pointer
 */
static void
datapipe_cancel_gc(datapipe_t *self)
{
    if( self->dp_gc_id ) {
        g_source_remove(self->dp_gc_id),
            self->dp_gc_id = 0;
    }
}

/** Execute the datapipe
 *
 * Note: Use #datapipe_exec_full() macro instead of calling
 *       this function directly.
 *
 * @param self The datapipe to execute
 * @param indata The input data to run through the datapipe
 * @return The processed data
 */
gconstpointer
datapipe_exec_full_real(datapipe_t *self, gconstpointer indata,
                        const char *file, const char *func)
{
    gconstpointer outdata = NULL;

    if (self == NULL) {
        mce_log(LL_ERR,
                "datapipe_exec_full_real() called "
                "without a valid datapipe");
        goto EXIT;
    }

    if( mce_log_p(LL_DEBUG) ||
        mce_log_p_(LL_DEBUG, file, func) ||
        mce_log_p_(LL_DEBUG, __FILE__, datapipe_name(self)) ) {

        if( self->dp_value_repr_cb ) {
            mce_log_unconditional(LL_DEBUG, file, func, "%s: execute %s",
                         datapipe_name(self),
                         self->dp_value_repr_cb(indata));
        }
        else {
            mce_log_unconditional(LL_DEBUG, file, func, "%s: execute %p",
                                  datapipe_name(self), indata);
        }
    }

    guint token = ++self->dp_token;

    datapipe_cache_t cache_indata = self->dp_cache;

    /* Optionally cache the value at the input stage */
    if( cache_indata & (DATAPIPE_CACHE_INDATA | DATAPIPE_CACHE_OUTDATA) ) {
        datapipe_set_value(self, indata);
    }

    /* Execute input value callbacks */
    for( GSList *item = self->dp_input_triggers; item; item = item->next ) {
        void (*trigger)(gconstpointer input) = item->data;
        if( !trigger )
            continue;

        if( self->dp_token != token ) {
            mce_log(LL_WARN, "%s: recursion detected at input triggers",
                    datapipe_name(self));
            goto EXIT;
        }

        trigger(indata);
    }

    /* Determine output value */
    outdata = indata;
    if( self->dp_read_only == DATAPIPE_FILTERING_ALLOWED ) {
        for( GSList *item = self->dp_filters; item; item = item->next ) {
            gconstpointer (*filter)(gconstpointer input) = item->data;
            if( !filter )
                continue;

            if( self->dp_token != token ) {
                mce_log(LL_WARN, "%s: recursion detected at input filters",
                        datapipe_name(self));
                goto EXIT;
            }

            outdata = filter(outdata);
        }
    }
    /* Optionally cache the value at the output stage */
    if( cache_indata & DATAPIPE_CACHE_OUTDATA ) {
        datapipe_set_value(self, outdata);
    }

    /* Execute output value callbacks */
    for( GSList *item = self->dp_output_triggers; item; item = item->next ) {
        void (*trigger)(gconstpointer input) = item->data;
        if( !trigger )
            continue;

        if( self->dp_token != token ) {
            mce_log(LL_WARN, "%s: recursion detected at output triggers",
                    datapipe_name(self));
            goto EXIT;
        }

        trigger(outdata);
    }

EXIT:
    return outdata;
}

/**
 * Append a filter to an existing datapipe
 *
 * @param self The datapipe to manipulate
 * @param filter The filter to add to the datapipe
 */
static void
datapipe_add_filter(datapipe_t *self,
                    gpointer (*filter)(gpointer data))
{
    if (self == NULL) {
        mce_log(LL_ERR, "called with NULL datapipe");
        goto EXIT;
    }

    if (filter == NULL) {
        mce_log(LL_ERR, "called with NULL filter");
        goto EXIT;
    }

    if (self->dp_read_only == DATAPIPE_FILTERING_DENIED) {
        mce_log(LL_ERR, "called on read only datapipe");
        goto EXIT;
    }

    self->dp_filters = g_slist_append(self->dp_filters, filter);

EXIT:
    return;
}

/**
 * Remove a filter from an existing datapipe
 * Non-existing filters are ignored
 *
 * @param self The datapipe to manipulate
 * @param filter The filter to remove from the datapipe
 */
static void
datapipe_remove_filter(datapipe_t *self,
                       gpointer (*filter)(gpointer data))
{
    if (self == NULL) {
        mce_log(LL_ERR, "called with NULL datapipe");
        goto EXIT;
    }

    if (filter == NULL) {
        mce_log(LL_ERR, "called with NULL filter");
        goto EXIT;
    }

    if (self->dp_read_only == DATAPIPE_FILTERING_DENIED) {
        mce_log(LL_ERR, "called on read only datapipe");
        goto EXIT;
    }

    GSList *itm = g_slist_find(self->dp_filters, filter);
    if( !itm )
        mce_log(LL_DEBUG, "called with non-existing filter");
    else
        itm->data = 0, datapipe_schedule_gc(self);

EXIT:
    return;
}

/**
 * Append an input trigger to an existing datapipe
 *
 * @param self The datapipe to manipulate
 * @param trigger The trigger to add to the datapipe
 */
static void
datapipe_add_input_trigger(datapipe_t *self,
                           void (*trigger)(gconstpointer data))
{
    if (self == NULL) {
        mce_log(LL_ERR, "called with NULL datapipe");
        goto EXIT;
    }

    if (trigger == NULL) {
        mce_log(LL_ERR, "called with NULL trigger");
        goto EXIT;
    }

    self->dp_input_triggers = g_slist_append(self->dp_input_triggers,
                                             trigger);

EXIT:
    return;
}

/**
 * Remove an input trigger from an existing datapipe
 * Non-existing triggers are ignored
 *
 * @param self The datapipe to manipulate
 * @param trigger The trigger to remove from the datapipe
 */
static void
datapipe_remove_input_trigger(datapipe_t *self,
                              void (*trigger)(gconstpointer data))
{
    if (self == NULL) {
        mce_log(LL_ERR, "called with NULL datapipe");
        goto EXIT;
    }

    if (trigger == NULL) {
        mce_log(LL_ERR, "called with NULL trigger");
        goto EXIT;
    }

    GSList *itm = g_slist_find(self->dp_input_triggers, trigger);
    if( !itm )
        mce_log(LL_DEBUG, "called with non-existing trigger");
    else
        itm->data = 0, datapipe_schedule_gc(self);

EXIT:
    return;
}

/**
 * Append an output trigger to an existing datapipe
 *
 * @param self The datapipe to manipulate
 * @param trigger The trigger to add to the datapipe
 */
static void
datapipe_add_output_trigger(datapipe_t *self,
                            void (*trigger)(gconstpointer data))
{
    if (self == NULL) {
        mce_log(LL_ERR, "called with NULL datapipe");
        goto EXIT;
    }

    if (trigger == NULL) {
        mce_log(LL_ERR, "called with NULL trigger");
        goto EXIT;
    }

    self->dp_output_triggers = g_slist_append(self->dp_output_triggers,
                                              trigger);

EXIT:
    return;
}

/**
 * Remove an output trigger from an existing datapipe
 * Non-existing triggers are ignored
 *
 * @param self The datapipe to manipulate
 * @param trigger The trigger to remove from the datapipe
 */
static void
datapipe_remove_output_trigger(datapipe_t *self,
                               void (*trigger)(gconstpointer data))
{
    if (self == NULL) {
        mce_log(LL_ERR, "called with NULL datapipe");
        goto EXIT;
    }

    if (trigger == NULL) {
        mce_log(LL_ERR, "called with NULL trigger");
        goto EXIT;
    }

    GSList *itm = g_slist_find(self->dp_output_triggers, trigger);
    if( !itm )
        mce_log(LL_DEBUG, "called with non-existing trigger");
    else
        itm->data = 0, datapipe_schedule_gc(self);
EXIT:
    return;
}

/**
 * Deinitialize a datapipe
 *
 * @param self The datapipe to manipulate
 */
static void
datapipe_free(datapipe_t *self)
{
    if (self == NULL) {
        mce_log(LL_ERR,
                "datapipe_free() called "
                "without a valid datapipe");
        goto EXIT;
    }

    datapipe_gc(self);

    /* Warn about still registered filters/triggers */
    if (self->dp_filters != NULL) {
        mce_log(LL_INFO,
                "datapipe_free() called on a datapipe that "
                "still has registered filter(s)");
    }

    if (self->dp_input_triggers != NULL) {
        mce_log(LL_INFO,
                "datapipe_free() called on a datapipe that "
                "still has registered input_trigger(s)");
    }

    if (self->dp_output_triggers != NULL) {
        mce_log(LL_INFO,
                "datapipe_free() called on a datapipe that "
                "still has registered output_trigger(s)");
    }

EXIT:
    return;
}

/** Setup all datapipes
 */
void mce_datapipe_init(void)
{
    /* Currently all datapipes are initialized statically
     * and this function exists for the sake of symmetry.
     */
}

/** Free all datapipes
 */
void mce_datapipe_quit(void)
{
    datapipe_free(&thermal_state_pipe);
    datapipe_free(&power_saving_mode_active_pipe);
    datapipe_free(&jack_sense_state_pipe);
    datapipe_free(&usb_cable_state_pipe);
    datapipe_free(&audio_route_pipe);
    datapipe_free(&inactivity_delay_pipe);
    datapipe_free(&battery_level_pipe);
    datapipe_free(&topmost_window_pid_pipe);
    datapipe_free(&camera_button_state_pipe);
    datapipe_free(&battery_status_pipe);
    datapipe_free(&battery_state_pipe);
    datapipe_free(&charger_type_pipe);
    datapipe_free(&charger_state_pipe);
    datapipe_free(&interaction_expected_pipe);
    datapipe_free(&tklock_request_pipe);
    datapipe_free(&proximity_sensor_actual_pipe);
    datapipe_free(&proximity_sensor_effective_pipe);
    datapipe_free(&proximity_sensor_required_pipe);
    datapipe_free(&proximity_blanked_pipe);
    datapipe_free(&light_sensor_actual_pipe);
    datapipe_free(&light_sensor_filtered_pipe);
    datapipe_free(&light_sensor_poll_request_pipe);
    datapipe_free(&orientation_sensor_actual_pipe);
    datapipe_free(&lens_cover_state_pipe);
    datapipe_free(&lid_sensor_is_working_pipe);
    datapipe_free(&lid_sensor_actual_pipe);
    datapipe_free(&lid_sensor_filtered_pipe);
    datapipe_free(&keyboard_slide_state_pipe);
    datapipe_free(&keyboard_available_state_pipe);
    datapipe_free(&lockkey_state_pipe);
    datapipe_free(&init_done_pipe);
    datapipe_free(&device_inactive_pipe);
    datapipe_free(&inactivity_event_pipe);
    datapipe_free(&touchscreen_event_pipe);
    datapipe_free(&keypress_event_pipe);
    datapipe_free(&key_backlight_brightness_pipe);
    datapipe_free(&user_activity_event_pipe);
    datapipe_free(&led_pattern_deactivate_pipe);
    datapipe_free(&led_pattern_activate_pipe);
    datapipe_free(&resume_detected_event_pipe);
    datapipe_free(&led_brightness_pipe);
    datapipe_free(&lpm_brightness_pipe);
    datapipe_free(&display_brightness_pipe);
    datapipe_free(&display_state_curr_pipe);
    datapipe_free(&display_state_request_pipe);
    datapipe_free(&display_state_next_pipe);
    datapipe_free(&uiexception_type_pipe);
    datapipe_free(&submode_pipe);
    datapipe_free(&alarm_ui_state_pipe);
    datapipe_free(&call_type_pipe);
    datapipe_free(&call_state_pipe);
    datapipe_free(&ignore_incoming_call_event_pipe);
    datapipe_free(&master_radio_enabled_pipe);
    datapipe_free(&system_state_pipe);
    datapipe_free(&heartbeat_event_pipe);
    datapipe_free(&compositor_service_state_pipe);
    datapipe_free(&lipstick_service_state_pipe);
    datapipe_free(&devicelock_service_state_pipe);
    datapipe_free(&usbmoded_service_state_pipe);
    datapipe_free(&ngfd_service_state_pipe);
    datapipe_free(&ngfd_event_request_pipe);
    datapipe_free(&dsme_service_state_pipe);
    datapipe_free(&thermalmanager_service_state_pipe);
    datapipe_free(&bluez_service_state_pipe);
    datapipe_free(&packagekit_locked_pipe);
    datapipe_free(&osupdate_running_pipe);
    datapipe_free(&shutting_down_pipe);
    datapipe_free(&devicelock_state_pipe);
    datapipe_free(&touch_grab_active_pipe);
    datapipe_free(&touch_grab_wanted_pipe);
    datapipe_free(&touch_detected_pipe);
    datapipe_free(&keypad_grab_active_pipe);
    datapipe_free(&keypad_grab_wanted_pipe);
    datapipe_free(&music_playback_ongoing_pipe);
    datapipe_free(&fpd_service_state_pipe);
    datapipe_free(&fpstate_pipe);
    datapipe_free(&enroll_in_progress_pipe);
}

/** Convert submode_t bitmap changes to human readable string
 *
 * @param prev  submode_t bitmap changed from
 * @param curr  submode_t bitmap changed to
 *
 * @return human readable representation of submode
 */
const char *submode_change_repr(submode_t prev, submode_t curr)
{
    static const struct {
        submode_t bit; const char *name;
    } lut[] = {
        { MCE_SUBMODE_INVALID,          "INVALID"          },
        { MCE_SUBMODE_TKLOCK,           "TKLOCK"           },
        { MCE_SUBMODE_EVEATER,          "EVEATER"          },
        { MCE_SUBMODE_BOOTUP,           "BOOTUP"           },
        { MCE_SUBMODE_TRANSITION,       "TRANSITION"       },
        { MCE_SUBMODE_AUTORELOCK,       "AUTORELOCK"       },
        { MCE_SUBMODE_VISUAL_TKLOCK,    "VISUAL_TKLOCK"    },
        { MCE_SUBMODE_POCKET,           "POCKET"           },
        { MCE_SUBMODE_PROXIMITY_TKLOCK, "PROXIMITY_TKLOCK" },
        { MCE_SUBMODE_MALF,             "MALF"             },
        { 0,0 }
    };

    static char buff[128];
    char *pos = buff;
    char *end = buff + sizeof buff - 1;

    auto inline void add(const char *str)
    {
        while( pos < end && *str )
            *pos++ = *str++;
    }

    for( int i = 0; lut[i].name; ++i ) {
        const char *tag = 0;

        if( curr & lut[i].bit ) {
            tag = (prev & lut[i].bit) ? "" : "+";
        }
        else if( prev & lut[i].bit ) {
            tag = "-";
        }

        if( tag ) {
            if( pos> buff ) add(" ");
            add(tag);
            add(lut[i].name);
        }
    }
    if( pos == buff ) {
        add("NONE");
    }
    *pos = 0;
    return buff;
}

/** Convert submode_t bitmap to human readable string
 *
 * @param submode submode_t bitmap
 *
 * @return human readable representation of submode
 */
const char *submode_repr(submode_t submode)
{
    return submode_change_repr(submode, submode);
}

/** Convert system_state_t enum to human readable string
 *
 * @param state system_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *system_state_repr(system_state_t state)
{
    const char *res = "UNKNOWN";

    switch( state ) {
    case MCE_SYSTEM_STATE_UNDEF:    res = "UNDEF";    break;
    case MCE_SYSTEM_STATE_SHUTDOWN: res = "SHUTDOWN"; break;
    case MCE_SYSTEM_STATE_USER:     res = "USER";     break;
    case MCE_SYSTEM_STATE_ACTDEAD:  res = "ACTDEAD";  break;
    case MCE_SYSTEM_STATE_REBOOT:   res = "REBOOT";   break;
    case MCE_SYSTEM_STATE_BOOT:     res = "BOOT";     break;
    default: break;
    }

    return res;
}

/** Convert devicelock_state_t enum to human readable string
 *
 * @param state devicelock_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *devicelock_state_repr(devicelock_state_t state)
{
    const char *res = "unknown";

    switch( state ) {
    case DEVICELOCK_STATE_UNLOCKED:  res = "unlocked";  break;
    case DEVICELOCK_STATE_LOCKED:    res = "locked";    break;
    case DEVICELOCK_STATE_UNDEFINED: res = "undefined"; break;
    default: break;
    }

    return res;
}

/** Convert thermal_state_t enum to human readable string
 *
 * @param state thermal state as thermal_state_t type
 *
 * @return human readable representation of state
 */
const char *thermal_state_repr(thermal_state_t state)
{
    const char *res = "unknown";
    switch( state ) {
    case THERMAL_STATE_UNDEF:      res = "undef";      break;
    case THERMAL_STATE_OK:         res = "ok";         break;
    case THERMAL_STATE_OVERHEATED: res = "overheated"; break;
    default: break;
    }
    return res;
}

/** Convert uiexception_type_t enum to human readable string
 *
 * Note that while uiexception_type_t actually is a bitmask
 * and this function is expected to be used in situations
 * where at maximum one bit is set.
 *
 * @param type uiexception_type_t exception stata
 *
 * @return human readable representation of type
 */
const char *uiexception_type_repr(uiexception_type_t type)
{
    const char *res = "unknown";
    switch( type ) {
    case UIEXCEPTION_TYPE_NONE:   res = "none";   break;
    case UIEXCEPTION_TYPE_LINGER: res = "linger"; break;
    case UIEXCEPTION_TYPE_CALL:   res = "call";   break;
    case UIEXCEPTION_TYPE_ALARM:  res = "alarm";  break;
    case UIEXCEPTION_TYPE_NOTIF:  res = "notif";  break;
    case UIEXCEPTION_TYPE_NOANIM: res = "noanim"; break;
    default: break;
    }
    return res;
}

/** Convert uiexception_type_t enum to dbus argument string
 *
 * Note that while uiexception_type_t actually is a bitmask
 * and this function is expected to be used in situations
 * where at maximum one bit is set.
 *
 * @param type uiexception_type_t exception stata
 *
 * @return representation of type for use over dbus
 */
const char *uiexception_type_to_dbus(uiexception_type_t type)
{
    const char *res = MCE_BLANKING_POLICY_DEFAULT_STRING;

    switch( type ) {
    case UIEXCEPTION_TYPE_NOTIF:
    case UIEXCEPTION_TYPE_NOANIM:
        res = MCE_BLANKING_POLICY_NOTIFICATION_STRING;
        break;

    case UIEXCEPTION_TYPE_ALARM:
        res = MCE_BLANKING_POLICY_ALARM_STRING;
        break;

    case UIEXCEPTION_TYPE_CALL:
        res = MCE_BLANKING_POLICY_CALL_STRING;
        break;

    case UIEXCEPTION_TYPE_LINGER:
        res = MCE_BLANKING_POLICY_LINGER_STRING;
        break;

    case UIEXCEPTION_TYPE_NONE:
        break;

    default:
        mce_log(LL_WARN, "unknown blanking policy; using default");
        break;
    }
    return res;
}

/** Convert service_state_t enum to human readable string
 *
 * @param state service_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *service_state_repr(service_state_t state)
{
    const char *res = "unknown";

    switch( state ) {
    case SERVICE_STATE_UNDEF:   res = "undefined"; break;
    case SERVICE_STATE_STOPPED: res = "stopped";   break;
    case SERVICE_STATE_RUNNING: res = "running";   break;
    default: break;
    }

    return res;
}

/** Convert usb_cable_state_t enum to human readable string
 *
 * @param state usb_cable_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *usb_cable_state_repr(usb_cable_state_t state)
{
    const char *res = "unknown";

    switch( state ) {
    case USB_CABLE_UNDEF:        res = "undefined";    break;
    case USB_CABLE_DISCONNECTED: res = "disconnected"; break;
    case USB_CABLE_CONNECTED:    res = "connected";    break;
    case USB_CABLE_ASK_USER:     res = "ask_user";     break;
    default: break;
    }

    return res;
}

/** Convert usb_cable_state_t enum to dbus argument string
 *
 * @param state usb_cable_state_t enumeration value
 *
 * @return representation of state for use over dbus
 */
const char *usb_cable_state_to_dbus(usb_cable_state_t state)
{
    const char *res = MCE_USB_CABLE_STATE_UNKNOWN;

    switch( state ) {
    case USB_CABLE_DISCONNECTED:
        res = MCE_USB_CABLE_STATE_DISCONNECTED;
        break;

    case USB_CABLE_ASK_USER:
    case USB_CABLE_CONNECTED:
        res = MCE_USB_CABLE_STATE_CONNECTED;
        break;
    default:
        break;
    }

    return res;
}

/** Convert charger_type_t enum to human readable string
 *
 * @param type charger_type_t enumeration value
 *
 * @return human readable representation of type
 */
const char *
charger_type_repr(charger_type_t type)
{
    const char *repr = "unknown";
    switch( type ) {
    case CHARGER_TYPE_NONE:     repr = "none";     break;
    case CHARGER_TYPE_USB:      repr = "usb";      break;
    case CHARGER_TYPE_DCP:      repr = "dcp";      break;
    case CHARGER_TYPE_HVDCP:    repr = "hwdcp";    break;
    case CHARGER_TYPE_CDP:      repr = "cdp";      break;
    case CHARGER_TYPE_WIRELESS: repr = "wireless"; break;
    case CHARGER_TYPE_OTHER:    repr = "other";    break;
    default: break;
    }
    return repr;
}

/** Convert charger_type_t enum to dbus argument string
 *
 * @param type charger_type_t enumeration value
 *
 * @return representation of type for use over dbus
 */
const char *
charger_type_to_dbus(charger_type_t type)
{
    const char *repr = MCE_CHARGER_TYPE_OTHER;
    switch( type ) {
    case CHARGER_TYPE_NONE:     repr = MCE_CHARGER_TYPE_NONE;     break;
    case CHARGER_TYPE_USB:      repr = MCE_CHARGER_TYPE_USB;      break;
    case CHARGER_TYPE_DCP:      repr = MCE_CHARGER_TYPE_DCP;      break;
    case CHARGER_TYPE_HVDCP:    repr = MCE_CHARGER_TYPE_HVDCP;    break;
    case CHARGER_TYPE_CDP:      repr = MCE_CHARGER_TYPE_CDP;      break;
    case CHARGER_TYPE_WIRELESS: repr = MCE_CHARGER_TYPE_WIRELESS; break;
    default: break;
    }
    return repr;
}

/** Convert charger_state_t enum to human readable string
 *
 * @param state charger_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *charger_state_repr(charger_state_t state)
{
    const char *res = "unknown";

    switch( state ) {
    case CHARGER_STATE_UNDEF: res = "undefined"; break;
    case CHARGER_STATE_OFF:   res = "off";       break;
    case CHARGER_STATE_ON:    res = "on";        break;
    default: break;
    }

    return res;
}

/** Convert charger_state_t enum to dbus argument string
 *
 * @param state charger_state_t enumeration value
 *
 * @return representation of state for use over dbus
 */
const char *charger_state_to_dbus(charger_state_t state)
{
    const char *res = MCE_CHARGER_STATE_UNKNOWN;

    switch( state ) {
    case CHARGER_STATE_OFF:
        res = MCE_CHARGER_STATE_OFF;
        break;
    case CHARGER_STATE_ON:
        res = MCE_CHARGER_STATE_ON;
        break;
    default:
        break;
    }

    return res;
}

/** Convert camera_button_state_t enum to human readable string
 *
 * @param state camera_button_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *camera_button_state_repr(camera_button_state_t state)
{
    const char *res = "unknown";

    switch( state ) {
    case CAMERA_BUTTON_UNDEF:     res = "undef";     break;
    case CAMERA_BUTTON_UNPRESSED: res = "unpressed"; break;
    case CAMERA_BUTTON_LAUNCH:    res = "launch";    break;
    default: break;
    }

    return res;
}

/** Convert audio_route_t enum to human readable string
 *
 * @param state audio_route_t enumeration value
 *
 * @return human readable representation of state
 */
const char *audio_route_repr(audio_route_t state)
{
    const char *res = "unknown";

    switch( state ) {
    case AUDIO_ROUTE_UNDEF:   res = "undef";   break;
    case AUDIO_ROUTE_HANDSET: res = "handset"; break;
    case AUDIO_ROUTE_SPEAKER: res = "speaker"; break;
    case AUDIO_ROUTE_HEADSET: res = "headset"; break;
    default: break;
    }

    return res;
}

/** Convert tklock_request_t enum to human readable string
 *
 * @param state tklock_request_t enumeration value
 *
 * @return human readable representation of state
 */
const char *tklock_request_repr(tklock_request_t state)
{
    const char *res = "unknown";
    switch( state ) {
    case TKLOCK_REQUEST_UNDEF:         res = "undef";         break;
    case TKLOCK_REQUEST_OFF:           res = "off";           break;
    case TKLOCK_REQUEST_OFF_DELAYED:   res = "off_delayed";   break;
    case TKLOCK_REQUEST_OFF_PROXIMITY: res = "off_proximity"; break;
    case TKLOCK_REQUEST_ON:            res = "on";            break;
    case TKLOCK_REQUEST_ON_DIMMED:     res = "on_dimmed";     break;
    case TKLOCK_REQUEST_ON_PROXIMITY:  res = "on_proximity";  break;
    case TKLOCK_REQUEST_TOGGLE:        res = "toggle";        break;
    case TKLOCK_REQUEST_ON_DELAYED:    res = "on_delayed";    break;
    default: break;
    }
    return res;
}

/** Conver tklock status code from system ui to human readable string
 *
 * @param status TKLOCK_xxx value defined in mode-names.h from mce-dev.
 *
 * @return human readable representation of status
 */
const char *tklock_status_repr(int status)
{
    const char *repr = "TKLOCK_UNKNOWN";

    switch( status ) {
    case TKLOCK_UNLOCK:  repr = "TKLOCK_UNLOCK";  break;
    case TKLOCK_RETRY:   repr = "TKLOCK_RETRY";   break;
    case TKLOCK_TIMEOUT: repr = "TKLOCK_TIMEOUT"; break;
    case TKLOCK_CLOSED:  repr = "TKLOCK_CLOSED";  break;
    default: break;
    }

    return repr;
}

/** Convert battery_state_t enum to human readable string
 *
 * @param state battery_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *
battery_state_repr(battery_state_t state)
{
    const char *repr = "invalid";

    switch( state ) {
    case BATTERY_STATE_UNKNOWN:      repr = "unknown";      break;
    case BATTERY_STATE_CHARGING:     repr = "charging";     break;
    case BATTERY_STATE_DISCHARGING:  repr = "discharging";  break;
    case BATTERY_STATE_NOT_CHARGING: repr = "not_charging"; break;
    case BATTERY_STATE_FULL:         repr = "full";         break;
    default: break;
    }

    return repr;
}

/** Convert battery_state_t enum to dbus argument string
 *
 * @param state battery_state_t enumeration value
 *
 * @return representation of state for use over dbus
 */
const char *
battery_state_to_dbus(battery_state_t state)
{
    const char *res = MCE_BATTERY_STATE_UNKNOWN;

    switch( state ) {
    case BATTERY_STATE_CHARGING:
      res = MCE_BATTERY_STATE_CHARGING;
      break;
    case BATTERY_STATE_DISCHARGING:
      res = MCE_BATTERY_STATE_DISCHARGING;
      break;
    case BATTERY_STATE_NOT_CHARGING:
      res = MCE_BATTERY_STATE_NOT_CHARGING;
      break;
    case BATTERY_STATE_FULL:
      res = MCE_BATTERY_STATE_FULL;
      break;
    default:
      break;
    }

    return res;
}

/** Convert battery_status_t enum to human readable string
 *
 * @param state battery_status_t enumeration value
 *
 * @return human readable representation of state
 */
const char *battery_status_repr(battery_status_t state)
{
    const char *res = "unknown";
    switch( state ) {
    case BATTERY_STATUS_UNDEF: res = "undefined"; break;
    case BATTERY_STATUS_FULL:  res = "full";      break;
    case BATTERY_STATUS_OK:    res = "ok";        break;
    case BATTERY_STATUS_LOW:   res = "low";       break;
    case BATTERY_STATUS_EMPTY: res = "empty";     break;
    default: break;
    }
    return res;
}

/** Convert battery_status_t enum to dbus argument string
 *
 * @param state battery_status_t enumeration value
 *
 * @return representation of state for use over dbus
 */
const char *battery_status_to_dbus(battery_status_t state)
{
    const char *res = MCE_BATTERY_STATUS_UNKNOWN;
    switch( state ) {
    case BATTERY_STATUS_FULL:
        res = MCE_BATTERY_STATUS_FULL;
        break;
    case BATTERY_STATUS_OK:
        res = MCE_BATTERY_STATUS_OK;
        break;
    case BATTERY_STATUS_LOW:
        res = MCE_BATTERY_STATUS_LOW;
        break;
    case BATTERY_STATUS_EMPTY:
        res = MCE_BATTERY_STATUS_EMPTY;
        break;
    default:
        break;
    }
    return res;
}

/** Convert alarm_ui_state_t enum to human readable string
 *
 * @param state alarm_ui_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *alarm_state_repr(alarm_ui_state_t state)
{
    const char *res = "UNKNOWN";

    switch( state ) {
    case MCE_ALARM_UI_INVALID_INT32: res = "INVALID"; break;
    case MCE_ALARM_UI_OFF_INT32:     res = "OFF";     break;
    case MCE_ALARM_UI_RINGING_INT32: res = "RINGING"; break;
    case MCE_ALARM_UI_VISIBLE_INT32: res = "VISIBLE"; break;
    default: break;
    }

    return res;
}

/** Mapping of call state integer <-> call state string */
static const mce_translation_t call_state_translation[] =
{
    {
        .number = CALL_STATE_NONE,
        .string = MCE_CALL_STATE_NONE
    },
    {
        .number = CALL_STATE_RINGING,
        .string = MCE_CALL_STATE_RINGING,
    },
    {
        .number = CALL_STATE_ACTIVE,
        .string = MCE_CALL_STATE_ACTIVE,
    },
    {
        .number = CALL_STATE_SERVICE,
        .string = MCE_CALL_STATE_SERVICE
    },
    { /* MCE_INVALID_TRANSLATION marks the end of this array */
        .number = MCE_INVALID_TRANSLATION,
        .string = MCE_CALL_STATE_NONE
    }
};

/** MCE call state number to string on D-Bus
 */
const char *call_state_to_dbus(call_state_t state)
{
    return mce_translate_int_to_string(call_state_translation, state);
}

/** String from D-Bus to MCE call state number */
call_state_t call_state_from_dbus(const char *name)
{
    return mce_translate_string_to_int(call_state_translation, name);
}

/** MCE call state number to string (for diagnostic logging)
 */
const char *call_state_repr(call_state_t state)
{
    const char *repr = "invalid";

    switch( state ) {
    case CALL_STATE_NONE:
        repr = MCE_CALL_STATE_NONE;
        break;
    case CALL_STATE_RINGING:
        repr = MCE_CALL_STATE_RINGING;
        break;
    case CALL_STATE_ACTIVE:
        repr = MCE_CALL_STATE_ACTIVE;
        break;
    case CALL_STATE_SERVICE:
        repr = MCE_CALL_STATE_SERVICE;
        break;
    case CALL_STATE_IGNORED:
        repr = "ignored";
        break;
    default:
        break;
    }
    return repr;
}

/** Mapping of call type integer <-> call type string */
static const mce_translation_t call_type_translation[] =
{
    {
        .number = CALL_TYPE_NORMAL,
        .string = MCE_NORMAL_CALL
    },
    {
        .number = CALL_TYPE_EMERGENCY,
        .string = MCE_EMERGENCY_CALL
    },
    { /* MCE_INVALID_TRANSLATION marks the end of this array */
        .number = MCE_INVALID_TRANSLATION,
        .string = MCE_NORMAL_CALL
    }
};

/** MCE call type number to string
 */
const char *call_type_repr(call_type_t type)
{
    return mce_translate_int_to_string(call_type_translation, type);
}

/** String to MCE call type number
 */
call_type_t call_type_parse(const char *name)
{
    return mce_translate_string_to_int(call_type_translation, name);
}

/** Helper for getting cover_state_t in human readable form
 *
 * @param state cover state enum value
 *
 * @return enum name without "COVER_" prefix
 */
const char *cover_state_repr(cover_state_t state)
{
    const char *res = "UNKNOWN";
    switch( state ) {
    case COVER_UNDEF:  res = "UNDEF";  break;
    case COVER_CLOSED: res = "CLOSED"; break;
    case COVER_OPEN:   res = "OPEN";   break;
    default: break;
    }
    return res;
}

/** Proximity state enum to human readable string
 *
 * @param state Cover state enumeration value
 *
 * @return cover state as human readable proximity state name
 */
const char *proximity_state_repr(cover_state_t state)
{
    const char *repr = "unknown";
    switch( state ) {
    case COVER_UNDEF:  repr = "undefined";   break;
    case COVER_CLOSED: repr = "covered";     break;
    case COVER_OPEN:   repr = "not covered"; break;
    default:
        break;
    }
    return repr;
}

/** Display state to human readable string
 */
const char *display_state_repr(display_state_t state)
{
    const char *repr = "UNKNOWN";

    switch( state ) {
    case MCE_DISPLAY_UNDEF:      repr = "UNDEF";      break;
    case MCE_DISPLAY_OFF:        repr = "OFF";        break;
    case MCE_DISPLAY_LPM_OFF:    repr = "LPM_OFF";    break;
    case MCE_DISPLAY_LPM_ON:     repr = "LPM_ON";     break;
    case MCE_DISPLAY_DIM:        repr = "DIM";        break;
    case MCE_DISPLAY_ON:         repr = "ON";         break;
    case MCE_DISPLAY_POWER_UP:   repr = "POWER_UP";   break;
    case MCE_DISPLAY_POWER_DOWN: repr = "POWER_DOWN"; break;
    default: break;
    }

    return repr;
}

/** Translate orientation state to human readable form */
const char *orientation_state_repr(orientation_state_t state)
{
    const char *name = "UNKNOWN";
    switch( state ) {
    case MCE_ORIENTATION_UNDEFINED:   name = "UNDEFINED";   break;
    case MCE_ORIENTATION_LEFT_UP:     name = "LEFT_UP";     break;
    case MCE_ORIENTATION_RIGHT_UP:    name = "RIGHT_UP";    break;
    case MCE_ORIENTATION_BOTTOM_UP:   name = "BOTTOM_UP";   break;
    case MCE_ORIENTATION_BOTTOM_DOWN: name = "BOTTOM_DOWN"; break;
    case MCE_ORIENTATION_FACE_DOWN:   name = "FACE_DOWN";   break;
    case MCE_ORIENTATION_FACE_UP:     name = "FACE_UP";     break;
    default: break;
    }
    return name;
}

/** Translate key state to human readable form */
const char *key_state_repr(key_state_t state)
{
    const char *repr = "UNKNOWN";
    switch( state ) {
    case KEY_STATE_UNDEF:    repr = "UNDEF";    break;
    case KEY_STATE_RELEASED: repr = "RELEASED"; break;
    case KEY_STATE_PRESSED:  repr = "PRESSED";  break;
    default: break;
    }
    return repr;
}

/** Translate tristate to human readable form */
const char *tristate_repr(tristate_t state)
{
    const char *repr = "invalid";
    switch( state ) {
    case TRISTATE_UNKNOWN: repr = "unknown"; break;
    case TRISTATE_FALSE:   repr = "false";   break;
    case TRISTATE_TRUE:    repr = "true";    break;
    default: break;
    }
    return repr;
}

/** Lookup table for fpstate_t <--> string */
static const mce_translation_t fpstate_lut[] =
{
    { FPSTATE_UNSET,           "FPSTATE_UNSET"       },
    { FPSTATE_ENUMERATING,     "FPSTATE_ENUMERATING" },
    { FPSTATE_IDLE,            "FPSTATE_IDLE"        },
    { FPSTATE_ENROLLING,       "FPSTATE_ENROLLING"   },
    { FPSTATE_IDENTIFYING,     "FPSTATE_IDENTIFYING" },
    { FPSTATE_REMOVING,        "FPSTATE_REMOVING"    },
    { FPSTATE_VERIFYING,       "FPSTATE_VERIFYING"   },
    { FPSTATE_ABORTING,        "FPSTATE_ABORTING"    },
    { FPSTATE_TERMINATING,     "FPSTATE_TERMINATING" },
    // sentinel
    { MCE_INVALID_TRANSLATION, 0                     }
};

/** Parse fpd state from dbus string */
fpstate_t fpstate_parse(const char *name)
{
    return mce_translate_string_to_int_with_default(fpstate_lut,
                                                    name,
                                                    FPSTATE_UNSET);
}

/** Translate fpd state to human readable form */
const char *fpstate_repr(fpstate_t state)
{
    return mce_translate_int_to_string_with_default(fpstate_lut,
                                                    state,
                                                    "FPSTATE_UNKNOWN");
}

static void
mce_datapipe_install_handlers(datapipe_handler_t *bindings)
{
    if( !bindings )
        goto EXIT;

    for( size_t i = 0; bindings[i].datapipe; ++i ) {
        if( bindings[i].bound )
            continue;

        if( bindings[i].filter_cb )
            datapipe_add_filter(bindings[i].datapipe,
                                bindings[i].filter_cb);

        if( bindings[i].input_cb )
            datapipe_add_input_trigger(bindings[i].datapipe,
                                       bindings[i].input_cb);

        if( bindings[i].output_cb )
            datapipe_add_output_trigger(bindings[i].datapipe,
                                        bindings[i].output_cb);
        bindings[i].bound = true;
    }

EXIT:
    return;
}

static void
mce_datapipe_remove_handlers(datapipe_handler_t *bindings)
{
    if( !bindings )
        goto EXIT;

    for( size_t i = 0; bindings[i].datapipe; ++i ) {
        if( !bindings[i].bound )
            continue;

        if( bindings[i].filter_cb )
            datapipe_remove_filter(bindings[i].datapipe,
                                   bindings[i].filter_cb);

        if( bindings[i].input_cb )
            datapipe_remove_input_trigger(bindings[i].datapipe,
                                          bindings[i].input_cb);

        if( bindings[i].output_cb )
            datapipe_remove_output_trigger(bindings[i].datapipe,
                                           bindings[i].output_cb);
        bindings[i].bound = false;
    }

EXIT:
    return;
}

static void
mce_datapipe_execute_handlers(datapipe_handler_t *bindings)
{
    if( !bindings )
        goto EXIT;

    for( size_t i = 0; bindings[i].datapipe; ++i ) {
        if( !bindings[i].bound )
            continue;

        if( bindings[i].output_cb )
            bindings[i].output_cb(bindings[i].datapipe->dp_cached_data);
    }

EXIT:
    return;
}

static gboolean mce_datapipe_execute_handlers_cb(gpointer aptr)
{
    datapipe_bindings_t *self = aptr;

    if( !self )
        goto EXIT;

    if( !self->execute_id )
        goto EXIT;

    self->execute_id = 0;

    mce_log(LL_INFO, "module=%s", self->module ?: "unknown");
    mce_datapipe_execute_handlers(self->handlers);

EXIT:
    return FALSE;
}

/** Append triggers/filters to datapipes
 */
void mce_datapipe_init_bindings(datapipe_bindings_t *self)
{
    mce_log(LL_INFO, "module=%s", self->module ?: "unknown");

    /* Set up datapipe callbacks */
    mce_datapipe_install_handlers(self->handlers);

    /* Get initial values for output triggers from idle
     * callback, i.e. when all modules have been loaded */
    if( !self->execute_id )
        self->execute_id = g_idle_add(mce_datapipe_execute_handlers_cb, self);
}

/** Remove triggers/filters from datapipes
 */
void mce_datapipe_quit_bindings(datapipe_bindings_t *self)
{
    mce_log(LL_INFO, "module=%s", self->module ?: "unknown");

    /* Remove the get initial values timer if still active */
    if( self->execute_id ) {
        g_source_remove(self->execute_id),
            self->execute_id = 0;
    }

    /* Remove datapipe callbacks */
    mce_datapipe_remove_handlers(self->handlers);
}

/** Helper for attempting to switch to active state
 */
void mce_datapipe_generate_activity(void)
{
    datapipe_exec_full(&inactivity_event_pipe, GINT_TO_POINTER(FALSE));
}

/** Helper for switching to inactive state
 */
void mce_datapipe_generate_inactivity(void)
{
    datapipe_exec_full(&inactivity_event_pipe, GINT_TO_POINTER(TRUE));
}
