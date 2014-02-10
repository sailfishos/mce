/**
 * @file tklock.c
 * This file implements the touchscreen/keypad lock component
 * of the Mode Control Entity
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
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

#include <linux/input.h>

#include <sys/time.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <mce/mode-names.h>
#include <systemui/dbus-names.h>
#include <systemui/tklock-dbus-names.h>

#include "mce.h"
#include "tklock.h"
#include "evdev.h"
#include "mce-io.h"
#include "mce-log.h"
#include "datapipe.h"
#include "mce-conf.h"
#include "mce-dbus.h"
#include "mce-gconf.h"

// FIXME: not used atm, need to implement something similar
#ifdef DEAD_CODE
/* Valid triggers for autorelock */
/** No autorelock triggers */
# define AUTORELOCK_NO_TRIGGERS  0
/** Autorelock on keyboard slide closed */
# define AUTORELOCK_KBD_SLIDE    (1 << 0)
/** Autorelock on lens cover */
# define AUTORELOCK_LENS_COVER   (1 << 1)
/** Autorelock on proximity sensor */
# define AUTORELOCK_ON_PROXIMITY (1 << 2)
#endif

/** Duration of exceptional UI states, in milliseconds */
enum
{
    EXCEPTION_LENGTH_CALL     = 3000, // [ms]
    EXCEPTION_LENGTH_ALARM    = 2500, // [ms]
    EXCEPTION_LENGTH_CHARGER  = 3000, // [ms]
    EXCEPTION_LENGTH_BATTERY  = 1000, // [ms]
    EXCEPTION_LENGTH_JACK     = 3000, // [ms]
    EXCEPTION_LENGTH_CAMERA   = 3000, // [ms]
#if 0 // debug
    EXCEPTION_LENGTH_VOLUME   = 9999, // [ms]
#else
    EXCEPTION_LENGTH_VOLUME   = 2000, // [ms]
#endif

    EXCEPTION_LENGTH_ACTIVITY = 2000, // [ms]

    /* Note: the notification durations and lengthening via
     *       activity must be long enough not to be cut off
     *       by periodic stopping of touch monitoring */
};

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** Max valid time_t value in milliseconds */
#define MAX_TICK (INT_MAX * (int64_t)1000)

/** Min valid time_t value in milliseconds */
#define MIN_TICK  0

/* ========================================================================= *
 * DATATYPES
 * ========================================================================= */

typedef struct
{
    datapipe_struct *datapipe;
    void (*output_cb)(gconstpointer data);
    void (*input_cb)(gconstpointer data);
    bool bound;
} datapipe_binding_t;

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

// time utilities

static void     tklock_monotime_get(struct timeval *tv);
static int64_t  tklock_monotick_get(void);

// datapipe values and triggers

static void     tklock_datapipe_system_state_cb(gconstpointer data);
static void     tklock_datapipe_device_lock_active_cb(gconstpointer data);
static void     tklock_datapipe_lipstick_available_cb(gconstpointer data);
static void     tklock_datapipe_display_state_cb(gconstpointer data);
static void     tklock_datapipe_proximity_sensor_cb(gconstpointer data);
static void     tklock_datapipe_call_state_cb(gconstpointer data);
static void     tklock_datapipe_alarm_ui_state_cb(gconstpointer data);
static void     tklock_datapipe_charger_state_cb(gconstpointer data);
static void     tklock_datapipe_battery_status_cb(gconstpointer data);
static void     tklock_datapipe_usb_cable_cb(gconstpointer data);
static void     tklock_datapipe_jack_sense_cb(gconstpointer data);
static void     tklock_datapipe_camera_button_cb(gconstpointer const data);
static void     tklock_datapipe_keypress_cb(gconstpointer const data);
static void     tklock_datapipe_exception_state_cb(gconstpointer data);
static void     tklock_datapipe_audio_route_cb(gconstpointer data);
static void     tklock_datapipe_tk_lock_cb(gconstpointer data);
static void     tklock_datapipe_submode_cb(gconstpointer data);
static void     tklock_datapipe_touchscreen_cb(gconstpointer const data);
static void     tklock_datapipe_lockkey_cb(gconstpointer const data);
static void     tklock_datapipe_heartbeat_cb(gconstpointer data);
static void     tklock_datapipe_keyboard_slide_cb(gconstpointer const data);
static void     tklock_datapipe_lid_cover_cb(gconstpointer data);
static void     tklock_datapipe_lens_cover_cb(gconstpointer data);
static void     tklock_datapipe_user_activity_cb(gconstpointer data);

static bool     tklock_datapipe_have_tklock_submode(void);
static void     tklock_datapipe_set_device_lock_active(int state);

static void     tklock_datapipe_append_triggers(datapipe_binding_t *bindings);
static void     tklock_datapipe_initialize_triggers(datapipe_binding_t *bindings);
static void     tklock_datapipe_remove_triggers(datapipe_binding_t *bindings);
static gboolean tklock_datapipe_init_cb(gpointer aptr);
static void     tklock_datapipe_init(void);
static void     tklock_datapipe_quit(void);

// autolock state machine

static gboolean tklock_autolock_cb(gpointer aptr);
static bool     tklock_autolock_exceeded(void);
static void     tklock_autolock_schedule(int delay);
static void     tklock_autolock_cancel(void);
static void     tklock_autolock_rethink(void);

// proximity locking state machine

static gboolean tklock_proxlock_cb(gpointer aptr);
static bool     tklock_proxlock_exceeded(void);
static void     tklock_proxlock_schedule(int delay);
static void     tklock_proxlock_cancel(void);
static void     tklock_proxlock_rethink(void);

// ui exception handling state machine

static uiexctype_t topmost_active(uiexctype_t mask);
static void     tklock_uiexcept_sync_to_datapipe(void);
static gboolean tklock_uiexcept_linger_cb(gpointer aptr);
static gboolean tklock_uiexcept_notif_cb(gpointer aptr);
static void     tklock_uiexcept_begin(uiexctype_t type, int64_t linger);
static void     tklock_uiexcept_end(uiexctype_t type, int64_t linger);
static void     tklock_uiexcept_cancel(void);
static void     tklock_uiexcept_deny_state_restore(void);
static void     tklock_uiexcept_rethink(void);

// legacy hw event input enable/disable state machine

static void     tklock_evctrl_set_state(output_state_t *output, bool enable);
static void     tklock_evctrl_set_kp_state(bool enable);
static void     tklock_evctrl_set_ts_state(bool enable);
static void     tklock_evctrl_set_dt_state(bool enable);
static void     tklock_evctrl_rethink(void);

// legacy hw double tap calibration

static void     tklock_dtcalib_now(void);
static void     tklock_dtcalib_from_heartbeat(void);
static gboolean tklock_dtcalib_cb(gpointer data);

static void     tklock_dtcalib_start(void);
static void     tklock_dtcalib_stop(void);

// settings from gconf

static void     tklock_gconf_cb(GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);

static void     tklock_gconf_init(void);
static void     tklock_gconf_quit(void);

// settings from mce.ini

static void     tklock_config_init(void);

// sysfs probing

static void     tklock_sysfs_probe(void);

// dbus ipc with systemui

static void     tklock_ui_eat_event(void);
static void     tklock_ui_open(void);
static void     tklock_ui_close(void);
static void     tklock_ui_set(bool enable);

static void     tklock_ui_get_device_lock_cb(DBusPendingCall *pc, void *aptr);
static void     tklock_ui_get_device_lock(void);

// dbus ipc

static gboolean tklock_dbus_send_tklock_mode(DBusMessage *const method_call);

static gboolean tklock_dbus_mode_get_req_cb(DBusMessage *const msg);
static gboolean tklock_dbus_mode_change_req_cb(DBusMessage *const msg);
static gboolean tklock_dbus_systemui_callback_cb(DBusMessage *const msg);

static gboolean tklock_dbus_device_lock_changed_cb(DBusMessage *const msg);

static void     mce_tklock_init_dbus(void);
static void     mce_tklock_quit_dbus(void);

// "module" load/unload
extern gboolean mce_tklock_init(void);
extern void     mce_tklock_exit(void);

/* ========================================================================= *
 * gconf settings
 * ========================================================================= */

/** Flag: Automatically lock (after ON->DIM->OFF cycle) */
static gboolean tk_autolock_enabled = DEFAULT_TK_AUTOLOCK;
/** GConf callback ID for tk_autolock_enabled */
static guint tk_autolock_enabled_cb_id = 0;

/** Touchscreen double tap gesture policy */
static gint doubletap_gesture_policy = DEFAULT_DOUBLETAP_GESTURE_POLICY;
/** GConf callback ID for doubletap_gesture_policy */
static guint doubletap_gesture_policy_cb_id = 0;

/** Flag: Disable automatic dim/blank from tklock */
static gint tklock_blank_disable = FALSE;
/** GConf notifier id for tracking tklock_blank_disable changes */
static guint tklock_blank_disable_id = 0;

/* ========================================================================= *
 * mce.ini settings
 * ========================================================================= */

/** Blank immediately on tklock instead of dim/blank */
static gboolean blank_immediately = DEFAULT_BLANK_IMMEDIATELY;

/** Dim immediately on tklock instead of timeout */
static gboolean dim_immediately = DEFAULT_DIM_IMMEDIATELY;

/** Touchscreen/keypad dim timeout */
static gint dim_delay = DEFAULT_DIM_DELAY;

/** Disable touchscreen immediately on tklock instead of at blank */
static gint disable_ts_immediately = DEFAULT_TS_OFF_IMMEDIATELY;

/** Disable keypad immediately on tklock instead of at blank */
static gint disable_kp_immediately = DEFAULT_KP_OFF_IMMEDIATELY;

/** Inhibit autolock when slide is open */
static gboolean autolock_with_open_slide = DEFAULT_AUTOLOCK_SLIDE_OPEN;

/** Inhibit proximity lock when slide is open */
static gboolean proximity_lock_with_open_slide = DEFAULT_PROXIMITY_LOCK_SLIDE_OPEN;

/** Unconditionally enable lock when keyboard slide is closed */
static gboolean always_lock_on_slide_close = DEFAULT_LOCK_ON_SLIDE_CLOSE;

/** Unlock the TKLock when the lens cover is opened */
static gboolean lens_cover_unlock = DEFAULT_LENS_COVER_UNLOCK;

/** Trigger unlock screen when volume keys are pressed */
static gboolean volkey_visual_trigger = DEFAULT_VOLKEY_VISUAL_TRIGGER;

/* ========================================================================= *
 * probed control file paths
 * ========================================================================= */

/** SysFS path to touchscreen event disable */
static output_state_t mce_touchscreen_sysfs_disable_output =
{
  .context = "touchscreen_disable",
  .truncate_file = TRUE,
  .close_on_exit = TRUE,
};

/** SysFS path to touchscreen double-tap gesture control */
static const gchar *mce_touchscreen_gesture_control_path = NULL;

/** SysFS path to touchscreen recalibration control */
static const gchar *mce_touchscreen_calibration_control_path = NULL;

/** SysFS path to keypad event disable */
static output_state_t mce_keypad_sysfs_disable_output =
{
  .context = "keypad_disable",
  .truncate_file = TRUE,
  .close_on_exit = TRUE,
};

/* ========================================================================= *
 * TIME UTILITIES
 * ========================================================================= */

/** Get timeval from monotonic source that advances during suspend too
 */
static void tklock_monotime_get(struct timeval *tv)
{
    struct timespec ts;

#ifdef CLOCK_BOOTTIME
    if( clock_gettime(CLOCK_BOOTTIME, &ts) == 0 )
        goto convert;
#endif

#ifdef CLOCK_REALTIME
    if( clock_gettime(CLOCK_REALTIME, &ts) == 0 )
        goto convert;
#endif

#ifdef CLOCK_MONOTONIC
    if( clock_gettime(CLOCK_MONOTONIC, &ts) == 0 )
        goto convert;
#endif

    gettimeofday(tv, 0);
    goto cleanup;

convert:
    TIMESPEC_TO_TIMEVAL(tv, &ts);

cleanup:
    return;
}

/** Get 64-bit millisecond resolution timestamp from monotonic source
 */
static int64_t tklock_monotick_get(void)
{
    struct timeval tv;
    tklock_monotime_get(&tv);

    int64_t res = tv.tv_sec;
    res *= 1000;
    res += (tv.tv_usec / 1000);

    return res;
}

/* ========================================================================= *
 * DATAPIPE VALUES AND TRIGGERS
 * ========================================================================= */

/** Current tklock ui state */
static bool tklock_ui_enabled = false;

/** Current tklock ui state that has been sent to lipstick */
static int  tklock_ui_sent    = -1; // does not match bool values

/** System state; is undefined at bootup, can't assume anything */
static system_state_t system_state = MCE_STATE_UNDEF;

/** Change notifications for system_state
 */
static void tklock_datapipe_system_state_cb(gconstpointer data)
{
    system_state_t prev = system_state;
    system_state = GPOINTER_TO_INT(data);

    if( prev == system_state )
        goto EXIT;

    mce_log(LL_DEBUG, "system_state: %d -> %d", prev, system_state);

    tklock_ui_set(false);

EXIT:
    return;
}

/** Device lock is active; assume false */
static bool device_lock_active = false;

/** Push device lock state value into device_lock_active_pipe datapipe
 */
static void tklock_datapipe_set_device_lock_active(int state)
{
    bool locked = (state != 0);

    if( device_lock_active != locked ) {
	execute_datapipe(&device_lock_active_pipe,
			 GINT_TO_POINTER(locked),
			 USE_INDATA, CACHE_INDATA);
    }
}

/** Change notifications for device_lock_active
 */
static void tklock_datapipe_device_lock_active_cb(gconstpointer data)
{
    bool prev = device_lock_active;
    device_lock_active = GPOINTER_TO_INT(data);

    if( device_lock_active == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "device_lock_active = %d -> %d", prev,
            device_lock_active);

    tklock_autolock_rethink();

EXIT:
    return;
}

/** Lipstick dbus name is reserved; assume false */
static bool lipstick_available = false;

/** Change notifications for lipstick_available
 */
static void tklock_datapipe_lipstick_available_cb(gconstpointer data)
{
    bool prev = lipstick_available;
    lipstick_available = GPOINTER_TO_INT(data);

    if( lipstick_available == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "lipstick_available = %d -> %d", prev,
            lipstick_available);

    // force tklock ipc
    tklock_ui_sent = -1;
    tklock_ui_set(false);

    if( lipstick_available ) {
	/* query initial device lock state on lipstick/mce startup */
	tklock_ui_get_device_lock();
    }
    else {
	/* assume device lock is off if lipstick exits */
	tklock_datapipe_set_device_lock_active(false);
    }

EXIT:
    return;
}

/** Display state; undefined initially, can't assume anything */
static display_state_t display_state = MCE_DISPLAY_UNDEF;

/** Change notifications for display_state
 */
static void tklock_datapipe_display_state_cb(gconstpointer data)
{
    display_state_t prev = display_state;
    display_state = GPOINTER_TO_INT(data);

    if( display_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state = %d -> %d", prev, display_state);

    if( display_state == MCE_DISPLAY_DIM )
        tklock_ui_eat_event();

    tklock_uiexcept_rethink();

    tklock_autolock_rethink();
    tklock_proxlock_rethink();

    tklock_evctrl_rethink();
EXIT:
    return;
}

/** Proximity state; assume not covered */
static cover_state_t proximity_state = COVER_OPEN;

/** Change notifications for proximity_state
 */
static void tklock_datapipe_proximity_sensor_cb(gconstpointer data)
{
    cover_state_t prev = proximity_state;
    proximity_state = GPOINTER_TO_INT(data);

    if( proximity_state == COVER_UNDEF )
        proximity_state = COVER_OPEN;

    if( proximity_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "proximity_state = %d -> %d", prev, proximity_state);

    tklock_uiexcept_rethink();

    tklock_proxlock_rethink();

    tklock_evctrl_rethink();

EXIT:
    return;
}

/** Call state; assume no active calls */
static call_state_t call_state = CALL_STATE_NONE;

/** Change notifications for call_state
 */
static void tklock_datapipe_call_state_cb(gconstpointer data)
{
    call_state_t prev = call_state;
    call_state = GPOINTER_TO_INT(data);

    if( call_state == CALL_STATE_INVALID )
        call_state = CALL_STATE_NONE;

    if( call_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "call_state = %d -> %d", prev, call_state);

    switch( call_state ) {
    case CALL_STATE_RINGING:
    case CALL_STATE_ACTIVE:
        tklock_uiexcept_begin(UIEXC_CALL, EXCEPTION_LENGTH_CALL);
        break;
    default:
        tklock_uiexcept_end(UIEXC_CALL, EXCEPTION_LENGTH_CALL);
        break;
    }

    // display on/off policy
    tklock_uiexcept_rethink();

    // volume keys during call
    tklock_evctrl_rethink();
EXIT:
    return;
}

/** Alarm state; assume no active alarms */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_OFF_INT32;

/** Change notifications for alarm_ui_state
 */
static void tklock_datapipe_alarm_ui_state_cb(gconstpointer data)
{
    alarm_ui_state_t prev = alarm_ui_state;
    alarm_ui_state = GPOINTER_TO_INT(data);

    if( alarm_ui_state == MCE_ALARM_UI_INVALID_INT32 )
        alarm_ui_state = MCE_ALARM_UI_OFF_INT32;

    if( alarm_ui_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "alarm_ui_state = %d -> %d", prev, alarm_ui_state);

    switch( alarm_ui_state ) {
    case MCE_ALARM_UI_RINGING_INT32:
    case MCE_ALARM_UI_VISIBLE_INT32:
        tklock_uiexcept_begin(UIEXC_ALARM, EXCEPTION_LENGTH_ALARM);
        break;
    default:
        tklock_uiexcept_end(UIEXC_ALARM, EXCEPTION_LENGTH_ALARM);
        break;
    }

    tklock_uiexcept_rethink();

EXIT:
    return;
}

/** Charger state; assume not charging */
static bool charger_state = false;

/** Change notifications for charger_state
 */
static void tklock_datapipe_charger_state_cb(gconstpointer data)
{
    bool prev = charger_state;
    charger_state = GPOINTER_TO_INT(data);

    if( charger_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "charger_state = %d -> %d", prev, charger_state);

    tklock_uiexcept_begin(UIEXC_NOTIF, EXCEPTION_LENGTH_CHARGER);
    tklock_uiexcept_rethink();

EXIT:
    return;
}

/** Battery status; not known initially, can't assume anything */
static battery_status_t battery_status = BATTERY_STATUS_UNDEF;

/** Change notifications for battery_status
 */
static void tklock_datapipe_battery_status_cb(gconstpointer data)
{
    battery_status_t prev = battery_status;
    battery_status = GPOINTER_TO_INT(data);

    if( battery_status == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "battery_status = %d -> %d", prev, battery_status);

    if( battery_status == BATTERY_STATUS_FULL ) {
        tklock_uiexcept_begin(UIEXC_NOTIF, EXCEPTION_LENGTH_BATTERY);
        tklock_uiexcept_rethink();
    }

EXIT:
    return;
}

/** USB cable status; assume disconnected */
static usb_cable_state_t usb_cable_state = USB_CABLE_DISCONNECTED;

/** Change notifications for usb_cable_state
 */
static void tklock_datapipe_usb_cable_cb(gconstpointer data)
{
    usb_cable_state_t prev = usb_cable_state;
    usb_cable_state = GPOINTER_TO_INT(data);

    if( usb_cable_state == USB_CABLE_UNDEF )
        usb_cable_state = USB_CABLE_DISCONNECTED;

    if( prev == usb_cable_state )
        goto EXIT;

    mce_log(LL_DEBUG, "usb_cable_state = %d -> %d", prev, usb_cable_state);

    tklock_uiexcept_begin(UIEXC_NOTIF, EXCEPTION_LENGTH_CHARGER);
    tklock_uiexcept_rethink();

EXIT:
        return;
}

/** Audio jack state; assume not inserted */
static cover_state_t jack_sense_state = COVER_OPEN;

/** Change notifications for jack_sense_state
 */
static void tklock_datapipe_jack_sense_cb(gconstpointer data)
{
    cover_state_t prev = jack_sense_state;
    jack_sense_state = GPOINTER_TO_INT(data);

    if( jack_sense_state == COVER_UNDEF )
        jack_sense_state = COVER_OPEN;

    if( prev == jack_sense_state )
        goto EXIT;

    mce_log(LL_DEBUG, "jack_sense_state = %d -> %d", prev, jack_sense_state);

    tklock_uiexcept_begin(UIEXC_NOTIF, EXCEPTION_LENGTH_JACK);
    tklock_uiexcept_rethink();

EXIT:
    return;
}

/** Change notifications for camera_button
 */
static void tklock_datapipe_camera_button_cb(gconstpointer const data)
{
    /* TODO: This might make no sense, need to check on HW that has
     *       dedicated camera button ... */
    (void)data;
    tklock_uiexcept_begin(UIEXC_NOTIF, EXCEPTION_LENGTH_CAMERA);
    tklock_uiexcept_rethink();
}

/** Change notifications for keypress
 */
static void tklock_datapipe_keypress_cb(gconstpointer const data)
{
    const struct input_event *const*evp;
    const struct input_event       *ev;

    if( !(evp = data) )
        goto EXIT;

    if( !(ev = *evp) )
        goto EXIT;

    // ignore non-key events
    if( ev->type != EV_KEY )
        goto EXIT;

    // ignore key up events
    if( ev->value == 0 )
        goto EXIT;

    switch( ev->code ) {
    case KEY_POWER:
        // power key events are handled in powerkey.c
        break;

    case KEY_CAMERA:
        mce_log(LL_DEBUG, "camera key");
        tklock_uiexcept_begin(UIEXC_NOTIF, EXCEPTION_LENGTH_CAMERA);
        tklock_uiexcept_rethink();
        break;

    case KEY_VOLUMEDOWN:
    case KEY_VOLUMEUP:
        mce_log(LL_DEBUG, "volume key");
        tklock_uiexcept_begin(UIEXC_NOTIF, EXCEPTION_LENGTH_VOLUME);
        tklock_uiexcept_rethink();
        break;

    default:
        break;
    }

EXIT:
    return;
}

/** UI exception state; initialized to none */
static uiexctype_t exception_state = UIEXC_NONE;

/** Change notifications for exception_state
 */
static void tklock_datapipe_exception_state_cb(gconstpointer data)
{
    uiexctype_t prev = exception_state;
    exception_state = GPOINTER_TO_INT(data);

    if( exception_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "exception_state = %d -> %d", prev, exception_state);

    tklock_autolock_rethink();
    tklock_proxlock_rethink();

EXIT:
    return;
}

/** Audio routing state; assume handset */
static audio_route_t audio_route = AUDIO_ROUTE_HANDSET;

/** Change notifications for audio_route
 */
static void tklock_datapipe_audio_route_cb(gconstpointer data)
{
    audio_route_t prev = audio_route;
    audio_route = GPOINTER_TO_INT(data);

    if( audio_route == AUDIO_ROUTE_UNDEF )
        audio_route = AUDIO_ROUTE_HANDSET;

    if( audio_route == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "audio_route = %d -> %d", prev, audio_route);
    tklock_uiexcept_rethink();

EXIT:
    return;
}

/** Change notifications for tk_lock_pipe
 *
 * Handles tklock requests from outside this module
 */
static void tklock_datapipe_tk_lock_cb(gconstpointer data)
{
    lock_state_t tk_lock_state = GPOINTER_TO_INT(data);

    mce_log(LL_DEBUG, "tk_lock_state = %d", tk_lock_state);

    bool enable = tklock_ui_enabled;

    switch( tk_lock_state ) {
    case LOCK_UNDEF:
    case LOCK_OFF:
    case LOCK_OFF_DELAYED:
        enable = false;
        break;
    default:
    case LOCK_OFF_PROXIMITY:
    case LOCK_ON:
    case LOCK_ON_DIMMED:
    case LOCK_ON_PROXIMITY:
    case LOCK_ON_DELAYED:
        enable = true;
        break;
    case LOCK_TOGGLE:
        enable = !enable;
    }
    tklock_ui_set(enable);
}

static submode_t submode = MCE_INVALID_SUBMODE;

/** Change notifications for submode
 */
static void tklock_datapipe_submode_cb(gconstpointer data)
{
    submode_t prev = submode;
    submode = GPOINTER_TO_INT(data);

    if( submode == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "submode = 0x%x", submode);

    // out of sync tklock state blocks state restore
    tklock_uiexcept_rethink();

    tklock_evctrl_rethink();
EXIT:
    return;
}

/** Query the touchscreen/keypad lock status
 *
 * @return TRUE if the touchscreen/keypad lock is enabled,
 *         FALSE if the touchscreen/keypad lock is disabled
 */
static bool tklock_datapipe_have_tklock_submode(void)
{
    return (submode & MCE_TKLOCK_SUBMODE) != 0;
}

/** Change notifications for tklock_datapipe_touchscreen_cb
 *
 * Note: Handles double tap gesture events only
 */
static void tklock_datapipe_touchscreen_cb(gconstpointer const data)
{
    struct input_event const *const *evp;
    struct input_event const *ev;

    if( !(evp = data) )
        goto EXIT;

    if( !(ev = *evp) )
        goto EXIT;

    mce_log(LL_DEBUG, "TS EVENT: %d %d %d", ev->type, ev->code, ev->value);

    /* Double tap ?*/
    if( ev->type != EV_MSC || ev->code != MSC_GESTURE || ev->value != 0x4 )
        goto EXIT;

    if( system_state != MCE_STATE_USER )
        goto EXIT;

    switch( display_state )
    {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_POWER_DOWN:
    case MCE_DISPLAY_LPM_ON:
        break;

    default:
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_UNDEF:
        goto EXIT;
    }

    switch( doubletap_gesture_policy ) {
    case 1: // unblank
    case 2: // unblank + unlock (= TODO)
        mce_log(LL_DEBUG, "double tap -> display on");
        execute_datapipe(&display_state_req_pipe,
                       GINT_TO_POINTER(MCE_DISPLAY_ON),
                       USE_INDATA, CACHE_INDATA);
        break;
    default:
        mce_log(LL_ERR, "Got a double tap gesture "
                "even though we haven't enabled "
                "gestures -- this shouldn't happen");
        break;
    }

EXIT:

  return;
}

/** Change notifications for lockkey_pipe
 */
static void tklock_datapipe_lockkey_cb(gconstpointer const data)
{
    /* TODO: IIRC lock key is N900 hw feature, I have not had a chance
     *       to test if this actually works ... */

    /* Ignore release events */
    if( GPOINTER_TO_INT(data) == 0 )
        goto EXIT;

    /* Try to give it the same treatment as power key would get.
     * Copy pasted from generic_powerkey_handler() @ powerkey.c */
    switch( display_state_get() ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_POWER_UP:
        mce_log(LL_DEBUG, "display -> off + lock");

        /* Do the locking before turning display off.
         *
         * The tklock requests get ignored in act dead
         * etc, so we can just blindly request it.
         */
        execute_datapipe(&tk_lock_pipe,
                         GINT_TO_POINTER(LOCK_ON),
                         USE_INDATA, CACHE_INDATA);

        execute_datapipe(&display_state_req_pipe,
                         GINT_TO_POINTER(MCE_DISPLAY_OFF),
                         USE_INDATA, CACHE_INDATA);
        break;

    default:
        mce_log(LL_DEBUG, "display -> on");
        execute_datapipe(&display_state_req_pipe,
                         GINT_TO_POINTER(MCE_DISPLAY_ON),
                         USE_INDATA, CACHE_INDATA);
        break;
    }

EXIT:
    return;
}

/** Change notifications for heartbeat_pipe
 */
static void tklock_datapipe_heartbeat_cb(gconstpointer data)
{
    (void)data;

    mce_log(LL_DEBUG, "heartbeat");
    tklock_dtcalib_from_heartbeat();
}

/** Keypad slide; assume closed */
static cover_state_t kbd_slide_state = COVER_CLOSED;

/** Change notifications from keyboard_slide_pipe
 */
static void tklock_datapipe_keyboard_slide_cb(gconstpointer const data)
{
    cover_state_t prev = kbd_slide_state;
    kbd_slide_state = GPOINTER_TO_INT(data);

    if( kbd_slide_state == COVER_UNDEF )
        kbd_slide_state = COVER_CLOSED;

    if( kbd_slide_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "kbd_slide_state = %d -> %d", prev, kbd_slide_state);

    // TODO: COVER_OPEN  -> display on, unlock, reason=AUTORELOCK_KBD_SLIDE
    // TODO: COVER_CLOSE -> display off, lock if reason==AUTORELOCK_KBD_SLIDE

EXIT:
    return;
}

/** Lid cover state (N770); assume open */
static cover_state_t lid_cover_state = COVER_OPEN;

/** Change notifications from lid_cover_pipe
 */
static void tklock_datapipe_lid_cover_cb(gconstpointer data)
{
    cover_state_t prev = lid_cover_state;
    lid_cover_state = GPOINTER_TO_INT(data);

    if( lid_cover_state == COVER_UNDEF )
        lid_cover_state = COVER_OPEN;

    if( lid_cover_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "lid_cover_state = %d -> %d", prev, lid_cover_state);

    // TODO: COVER_OPEN -> display on, unlock
    // TODO: COVER_CLOSE -> display off lock

EXIT:
    return;
}

/** Camera lens cover state; assume closed */
static cover_state_t lens_cover_state = COVER_CLOSED;

/** Change notifications from lens_cover_pipe
 */
static void tklock_datapipe_lens_cover_cb(gconstpointer data)
{
    cover_state_t prev = lens_cover_state;
    lens_cover_state = GPOINTER_TO_INT(data);

    if( lens_cover_state == COVER_UNDEF )
        lens_cover_state = COVER_CLOSED;

    if( lens_cover_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "lens_cover_state = %d -> %d", prev, lens_cover_state);

    // TODO: COVER_OPEN  -> display on, unlock, reason=AUTORELOCK_KBD_SLIDE
    // TODO: COVER_CLOSE -> display off, lock if reason==AUTORELOCK_KBD_SLIDE

EXIT:
    return;
}

/** Handle user_activity_pipe notifications
 *
 * @param data input_event as void pointer
 */
static void tklock_datapipe_user_activity_cb(gconstpointer data)
{
    const struct input_event *ev = data;

    if( !ev )
        goto EXIT;

    /* Touch events relevant unly when handling notification & linger */
    if( !(exception_state & (UIEXC_NOTIF | UIEXC_LINGER)) )
        goto EXIT;

    mce_log(LL_DEBUG, "type: %s, code: %s, value: %d",
            evdev_get_event_type_name(ev->type),
            evdev_get_event_code_name(ev->type, ev->code),
            ev->value);

    bool touched = false;
    switch( ev->type ) {
    case EV_SYN:
        switch( ev->code ) {
        case SYN_MT_REPORT:
            touched = true;
            break;
        default:
            break;
        }
        break;

    case EV_ABS:
        switch( ev->code ) {
        case ABS_MT_POSITION_X:
        case ABS_MT_POSITION_Y:
        case ABS_MT_PRESSURE:
            touched = true;
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }

    if( !touched )
        goto EXIT;

    /* N.B. the exception_state is bitmask, but only bit at time is
     *      visible in the exception_state datapipe */
    switch( exception_state ) {
    case UIEXC_LINGER:
        /* touch events during linger -> do not restore display state */
        mce_log(LL_DEBUG, "touch event; do not restore display/tklock state");
        tklock_uiexcept_deny_state_restore();
        break;

    case UIEXC_NOTIF:
        /* touchscreen activity makes notification exceptions to last longer */
        mce_log(LL_DEBUG, "touch event; lengthen notification exception");
        tklock_uiexcept_begin(UIEXC_NOTIF, EXCEPTION_LENGTH_ACTIVITY);
        break;

    default:
        break;
    }

EXIT:
    return;
}

/** Array of datapipe bindings */
static datapipe_binding_t tklock_datapipe_triggers[] =
{
    // output triggers
    {
        .datapipe = &lipstick_available_pipe,
        .output_cb = tklock_datapipe_lipstick_available_cb,
    },
    {
        .datapipe = &device_lock_active_pipe,
        .output_cb = tklock_datapipe_device_lock_active_cb,
    },
    {
        .datapipe = &display_state_pipe,
        .output_cb = tklock_datapipe_display_state_cb,
    },
    {
        .datapipe = &tk_lock_pipe,
        .output_cb = tklock_datapipe_tk_lock_cb,
    },
    {
        .datapipe = &proximity_sensor_pipe,
        .output_cb = tklock_datapipe_proximity_sensor_cb,
    },
    {
        .datapipe = &call_state_pipe,
        .output_cb = tklock_datapipe_call_state_cb,
    },
    {
        .datapipe = &alarm_ui_state_pipe,
        .output_cb = tklock_datapipe_alarm_ui_state_cb,
    },
    {
        .datapipe = &charger_state_pipe,
        .output_cb = tklock_datapipe_charger_state_cb,
    },
    {
        .datapipe = &battery_status_pipe,
        .output_cb = tklock_datapipe_battery_status_cb,
    },
    {
        .datapipe = &exception_state_pipe,
        .output_cb = tklock_datapipe_exception_state_cb,
    },
    {
        .datapipe = &audio_route_pipe,
        .output_cb = tklock_datapipe_audio_route_cb,
    },
    {
        .datapipe = &system_state_pipe,
        .output_cb = tklock_datapipe_system_state_cb,
    },
    {
        .datapipe = &usb_cable_pipe,
        .output_cb = tklock_datapipe_usb_cable_cb,
    },
    {
        .datapipe = &jack_sense_pipe,
        .output_cb = tklock_datapipe_jack_sense_cb,
    },
    {
        .datapipe = &heartbeat_pipe,
        .output_cb = tklock_datapipe_heartbeat_cb,
    },
    {
        .datapipe = &submode_pipe,
        .output_cb = tklock_datapipe_submode_cb,
    },
    {
        .datapipe = &lid_cover_pipe,
        .output_cb = tklock_datapipe_lid_cover_cb,
    },
    {
        .datapipe = &lens_cover_pipe,
        .output_cb = tklock_datapipe_lens_cover_cb,
    },
    {
        .datapipe = &user_activity_pipe,
        .output_cb = tklock_datapipe_user_activity_cb,

    },

    // input triggers
    {
        .datapipe = &touchscreen_pipe,
        .input_cb = tklock_datapipe_touchscreen_cb,
    },
    {
        .datapipe = &keypress_pipe,
        .input_cb = tklock_datapipe_keypress_cb,
    },
    {
        .datapipe = &lockkey_pipe,
        .input_cb = tklock_datapipe_lockkey_cb,
    },
    {
        .datapipe = &camera_button_pipe,
        .input_cb = tklock_datapipe_camera_button_cb,
    },
    {
        .datapipe = &keyboard_slide_pipe,
        .input_cb = tklock_datapipe_keyboard_slide_cb,
    },

    // sentinel
    {
        .datapipe = 0,
    }
};

static void tklock_datapipe_append_triggers(datapipe_binding_t *bindings)
{
    if( !bindings )
        goto EXIT;

    for( size_t i = 0; bindings[i].datapipe; ++i ) {
        if( bindings[i].bound )
            continue;

        if( bindings[i].input_cb )
            append_input_trigger_to_datapipe(bindings[i].datapipe,
                                             bindings[i].input_cb);

        if( bindings[i].output_cb )
            append_output_trigger_to_datapipe(bindings[i].datapipe,
                                              bindings[i].output_cb);
        bindings[i].bound = true;
    }

EXIT:
    return;
}

static void tklock_datapipe_initialize_triggers(datapipe_binding_t *bindings)
{
    if( !bindings )
        goto EXIT;

    for( size_t i = 0; bindings[i].datapipe; ++i ) {
        if( !bindings[i].bound )
            continue;

        if( bindings[i].output_cb )
          bindings[i].output_cb(bindings[i].datapipe->cached_data);
    }

EXIT:
    return;
}

static void tklock_datapipe_remove_triggers(datapipe_binding_t *bindings)
{
    if( !bindings )
        goto EXIT;

    for( size_t i = 0; bindings[i].datapipe; ++i ) {
        if( !bindings[i].bound )
            continue;

        if( bindings[i].input_cb )
            remove_input_trigger_from_datapipe(bindings[i].datapipe,
                                             bindings[i].input_cb);

        if( bindings[i].output_cb )
            remove_output_trigger_from_datapipe(bindings[i].datapipe,
                                              bindings[i].output_cb);
        bindings[i].bound = false;
    }

EXIT:
    return;
}

static guint tklock_datapipe_init_id = 0;

static gboolean tklock_datapipe_init_cb(gpointer aptr)
{
    (void)aptr;

    if( !tklock_datapipe_init_id )
        goto EXIT;

    tklock_datapipe_init_id = 0;

    tklock_datapipe_initialize_triggers(tklock_datapipe_triggers);

EXIT:
    return FALSE;
}

/** Append triggers/filters to datapipes
 */
static void tklock_datapipe_init(void)
{
    /* Set up datapipe callbacks */
    tklock_datapipe_append_triggers(tklock_datapipe_triggers);

    /* Get initial values for output triggers from idle
     * callback, i.e. when all modules have been loaded */
    tklock_datapipe_init_id = g_idle_add(tklock_datapipe_init_cb, 0);
}

/** Remove triggers/filters from datapipes
 */
static void tklock_datapipe_quit(void)
{
    /* Remove the get initial values timer if still active */
    if( tklock_datapipe_init_id )
        g_source_remove(tklock_datapipe_init_id),
        tklock_datapipe_init_id = 0;

    /* Remove datapipe callbacks */
    tklock_datapipe_remove_triggers(tklock_datapipe_triggers);
}

/* ========================================================================= *
 * AUTOLOCK STATE MACHINE
 * ========================================================================= */

/** Maximum time to delay enabling tklock after display is blanked */
#define AUTOLOCK_DELAY_MS (30 * 1000)

static int64_t tklock_autolock_tick = MAX_TICK;
static guint   tklock_autolock_id   = 0;

static gboolean tklock_autolock_cb(gpointer aptr)
{
    (void)aptr;

    mce_log(LL_DEBUG, "autolock timer triggered");

    if( tklock_autolock_id ) {
        tklock_autolock_id = 0;
        tklock_autolock_tick = MAX_TICK;
        tklock_ui_set(true);
    }
    return false;
}

static bool tklock_autolock_exceeded(void)
{
    bool res = tklock_monotick_get() > tklock_autolock_tick;
    if( res )
        mce_log(LL_DEBUG, "autolock time exceeded");
    return res;
}

static void tklock_autolock_cancel(void)
{
    if( tklock_autolock_id ) {
        tklock_autolock_tick = MAX_TICK;
        g_source_remove(tklock_autolock_id), tklock_autolock_id = 0;
        mce_log(LL_DEBUG, "autolock timer stopped");
    }
}

static void tklock_autolock_schedule(int delay)
{
    if( tklock_autolock_id )
        g_source_remove(tklock_autolock_id);

    tklock_autolock_id = g_timeout_add(delay, tklock_autolock_cb, 0);
    tklock_autolock_tick = tklock_monotick_get() + delay;
    mce_log(LL_DEBUG, "autolock timer started");
}

static void tklock_autolock_rethink(void)
{
    static display_state_t prev_display_state = MCE_DISPLAY_UNDEF;

    mce_log(LL_DEBUG, "display state: %d -> %d", prev_display_state,
            display_state);

    /* If we are already tklocked, handling exceptional ui state
     * or autolocking is disabled -> deactivate state machine */
    if( tklock_ui_enabled || exception_state || !tk_autolock_enabled ) {
        tklock_autolock_cancel();
        prev_display_state = MCE_DISPLAY_UNDEF;
        goto EXIT;
    }

    bool was_off = false;

    switch( prev_display_state ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_DOWN:
         was_off = true;
        break;
    default:
        break;
    }

    switch( display_state ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_DOWN:
        if( device_lock_active )
            tklock_ui_set(true);
        else if( !was_off )
            tklock_autolock_schedule(AUTOLOCK_DELAY_MS);
        break;

    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_POWER_UP:
        if( was_off && tklock_autolock_exceeded() )
            tklock_ui_set(true);
        tklock_autolock_cancel();
        break;

    default:
    case MCE_DISPLAY_UNDEF:
        break;
    }

    prev_display_state = display_state;

EXIT:

    return;
}

/* ========================================================================= *
 * PROXIMITY LOCKING STATE MACHINE
 * ========================================================================= */

/** Delay for enabling tklock from display off when proximity is covered */
#define PROXLOC_DELAY_MS (3000)

static int64_t tklock_proxlock_tick = MAX_TICK;
static guint   tklock_proxlock_id   = 0;

static gboolean tklock_proxlock_cb(gpointer aptr)
{
    (void)aptr;

    mce_log(LL_DEBUG, "proxlock timer triggered");

    if( tklock_proxlock_id ) {
        tklock_proxlock_id = 0;
        tklock_proxlock_tick = MAX_TICK;
        tklock_ui_set(true);
    }
    return false;
}

static bool tklock_proxlock_exceeded(void)
{
    bool res = tklock_monotick_get() > tklock_proxlock_tick;
    if( res )
        mce_log(LL_DEBUG, "proxlock time exceeded");
    return res;
}

static void tklock_proxlock_cancel(void)
{
    if( tklock_proxlock_id ) {
        tklock_proxlock_tick = MAX_TICK;
        g_source_remove(tklock_proxlock_id), tklock_proxlock_id = 0;
        mce_log(LL_DEBUG, "proxlock timer stopped");
    }
}

static void tklock_proxlock_schedule(int delay)
{
    if( tklock_proxlock_id )
        g_source_remove(tklock_proxlock_id);

    tklock_proxlock_id = g_timeout_add(delay, tklock_proxlock_cb, 0);
    tklock_proxlock_tick = tklock_monotick_get() + delay;
    mce_log(LL_DEBUG, "proxlock timer restarted");
}

static void tklock_proxlock_rethink(void)
{
    static cover_state_t prev_proximity_state = COVER_UNDEF;
    static display_state_t prev_display_state = MCE_DISPLAY_UNDEF;

    mce_log(LL_DEBUG, "display state: %d -> %d", prev_display_state,
            display_state);

    if( exception_state || tklock_ui_enabled ) {
        //mce_log(LL_DEBUG, "handling exception or already tklocked");
        tklock_proxlock_cancel();
        prev_display_state = MCE_DISPLAY_UNDEF;
        prev_proximity_state = COVER_UNDEF;
        goto EXIT;
    }

    bool was_off = false;

    bool is_covered  = (proximity_state      == COVER_CLOSED);
    bool was_covered = (prev_proximity_state == COVER_CLOSED);

    switch( prev_display_state ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_DOWN:
        was_off = true;
        break;
    default:
        break;
    }

    switch( display_state ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_DOWN:
        if( is_covered ) {
            if( !was_covered ){
                mce_log(LL_DEBUG, "proximity covered while display off");
                tklock_ui_set(true);
            }
            else if( !was_off ) {
                tklock_proxlock_schedule(PROXLOC_DELAY_MS);
            }
        }
        else {
            if( was_covered && tklock_proxlock_exceeded() )
                tklock_ui_set(true);
            tklock_proxlock_cancel();
        }
        break;

    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_POWER_UP:
        tklock_proxlock_cancel();
        break;

    default:
    case MCE_DISPLAY_UNDEF:
        break;
    }

    prev_proximity_state = proximity_state;
    prev_display_state = display_state;

EXIT:

    return;
}

/* ========================================================================= *
 * UI EXCEPTION HANDLING STATE MACHINE
 * ========================================================================= */

typedef struct
{
    uiexctype_t     mask;
    display_state_t display;
    bool            tklock;
    bool            insync;
    bool            restore;
    int64_t         linger_tick;
    guint           linger_id;
    int64_t         notif_tick;
    guint           notif_id;
} exception_t;

static exception_t exdata =
{
    .mask        = UIEXC_NONE,
    .display     = MCE_DISPLAY_UNDEF,
    .tklock      = false,
    .insync      = true,
    .restore     = true,
    .linger_tick = MIN_TICK,
    .linger_id   = 0,
    .notif_tick  = MIN_TICK,
    .notif_id    = 0,
};

static uiexctype_t topmost_active(uiexctype_t mask)
{
    /* Assume UI side priority is:
     * 1. notification dialogs
     * 2. alarm ui
     * 3. call ui
     * 4. rest
     */

    static const uiexctype_t pri[] = {
        UIEXC_NOTIF,
        UIEXC_ALARM,
        UIEXC_CALL,
        UIEXC_LINGER,
        0
    };

    for( size_t i = 0; pri[i]; ++i ) {
        if( mask & pri[i] )
            return pri[i];
    }

    return UIEXC_NONE;
}

static void  tklock_uiexcept_sync_to_datapipe(void)
{
    uiexctype_t in_pipe = datapipe_get_gint(exception_state_pipe);
    uiexctype_t active  = topmost_active(exdata.mask);

    if( in_pipe != active ) {
        execute_datapipe(&exception_state_pipe,
                         GINT_TO_POINTER(active),
                         USE_INDATA, CACHE_INDATA);
    }
}

/** Do not restore display/tklock state at the end of exceptional ui state
 */
static void tklock_uiexcept_deny_state_restore(void)
{
  if( exdata.mask && exdata.restore ) {
    exdata.restore = false;
    mce_log(LL_NOTICE, "DISABLING STATE RESTORE");
  }
}

static void tklock_uiexcept_rethink(void)
{
    static display_state_t display_prev = MCE_DISPLAY_UNDEF;

    static uiexctype_t active_prev = UIEXC_NONE;

    bool        activate = false;
    bool        blank    = false;
    uiexctype_t active   = topmost_active(exdata.mask);

    if( !active ) {
        mce_log(LL_DEBUG, "UIEXC_NONE");
        goto EXIT;
    }

    if( exdata.tklock != tklock_datapipe_have_tklock_submode() ) {
        if( exdata.restore )
            mce_log(LL_NOTICE, "DISABLING STATE RESTORE; tklock out of sync");
        exdata.restore = false;
    }

    // re-sync on display on transition
    if( display_prev != display_state ) {
        mce_log(LL_DEBUG, "display state: %d -> %d",
                display_prev, display_state);
        if( display_state == MCE_DISPLAY_ON ) {
            if( !exdata.insync )
                mce_log(LL_NOTICE, "display unblanked; assuming in sync again");
            exdata.insync = true;
        }
    }

    // re-sync on active exception change
    if( active_prev != active ) {
        active_prev = active;
        if( !exdata.insync )
            mce_log(LL_NOTICE, "exception state changed; assuming in sync again");
        exdata.insync = true;
    }

    switch( active ) {
    case UIEXC_NOTIF:
        mce_log(LL_DEBUG, "UIEXC_NOTIF");
        activate = true;
        break;

    case UIEXC_ALARM:
        mce_log(LL_DEBUG, "UIEXC_ALARM");
        activate = true;
        break;

    case UIEXC_CALL:
        mce_log(LL_DEBUG, "UIEXC_CALL");
        if( call_state == CALL_STATE_RINGING ) {
            mce_log(LL_DEBUG, "call=RINGING; activate");
            activate = true;
        }
        else if( audio_route != AUDIO_ROUTE_HANDSET ) {
            mce_log(LL_DEBUG, "audio!=HANDSET; activate");
            activate = true;
        }
        else if( proximity_state == COVER_CLOSED ) {
            mce_log(LL_DEBUG, "proximity=COVERED; blank");
            blank = true;
        }
        else {
            mce_log(LL_DEBUG, "proximity=NOT-COVERED; activate");
            activate = true;
        }
        break;

    case UIEXC_LINGER:
        mce_log(LL_DEBUG, "UIEXC_LINGER");
        activate = true;
        break;

    case UIEXC_NONE:
        // we should not get here
        break;

    default:
        // added new states and forgot to update state machine?
        mce_log(LL_CRIT, "unknown ui exception %d; have to ignore", active);
        mce_abort();
        break;
    }

    mce_log(LL_DEBUG, "blank=%d, activate=%d", blank, activate);

    if( blank ) {
        if( display_state != MCE_DISPLAY_OFF ) {
            mce_log(LL_DEBUG, "display blank");
            execute_datapipe(&display_state_req_pipe,
                             GINT_TO_POINTER(MCE_DISPLAY_OFF),
                             USE_INDATA, CACHE_INDATA);
        }
        else {
            mce_log(LL_DEBUG, "display already blanked");
        }
    }
    else if( activate ) {
        if( display_prev == MCE_DISPLAY_ON &&
            display_state != MCE_DISPLAY_ON ) {
            /* Assume: dim/blank timer took over the blanking.
             * Disable this state machine until display gets
             * turned back on */
          mce_log(LL_NOTICE, "AUTO UNBLANK DISABLED; display out of sync");
            exdata.insync = false;

            /* Disable state restore, unless we went out of
             * sync during call ui handling */
            if( exdata.restore && active != UIEXC_CALL ) {
                exdata.restore = false;
                mce_log(LL_NOTICE, "DISABLING STATE RESTORE; display out of sync");
            }
        }
        else if( !exdata.insync ) {
            mce_log(LL_NOTICE, "NOT UNBLANKING; still out of sync");
        }
        else if( proximity_state == COVER_CLOSED ) {
            mce_log(LL_NOTICE, "NOT UNBLANKING; proximity covered");
        }
        else if( display_state != MCE_DISPLAY_ON ) {
            mce_log(LL_DEBUG, "display unblank");
            execute_datapipe(&display_state_req_pipe,
                             GINT_TO_POINTER(MCE_DISPLAY_ON),
                             USE_INDATA, CACHE_INDATA);
        }
    }

EXIT:
    display_prev = display_state;

    return;
}

static void tklock_uiexcept_cancel(void)
{
    if( exdata.notif_id ) {
        g_source_remove(exdata.notif_id),
            exdata.notif_id = 0;
    }

    if( exdata.linger_id ) {
        g_source_remove(exdata.linger_id),
            exdata.linger_id = 0;
    }

    exdata.mask        = UIEXC_NONE;
    exdata.display     = MCE_DISPLAY_UNDEF;
    exdata.tklock      = false;
    exdata.insync      = true;
    exdata.restore     = true;
    exdata.linger_tick = MIN_TICK;
    exdata.linger_id   = 0;
    exdata.notif_tick  = MIN_TICK,
    exdata.notif_id    = 0;
}

static gboolean tklock_uiexcept_linger_cb(gpointer aptr)
{
    (void) aptr;

    if( !exdata.linger_id )
        goto EXIT;

    mce_log(LL_DEBUG, "linger timeout");

    /* mark timer inactive */
    exdata.linger_id = 0;

    /* operate on copy of data, in case the data
     * pipe operations cause feedback */
    exception_t exx = exdata;
    tklock_uiexcept_cancel();

    /* update exception data pipe first */
    tklock_uiexcept_sync_to_datapipe();

    /* check if restoring has been blocked */
    if( !exx.restore )
        goto EXIT;

    /* then flip the tklock  back on? Note that we
     * we do not unlock no matter what. */
    if( exx.tklock ) {
        execute_datapipe(&tk_lock_pipe,
                         GINT_TO_POINTER(LOCK_ON),
                         USE_INDATA, CACHE_INDATA);
    }

    /* and finally the display data pipe */
    switch( exx.display ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_ON:
      execute_datapipe(&display_state_req_pipe,
                       GINT_TO_POINTER(exx.display),
                       USE_INDATA, CACHE_INDATA);
      break;
    default:
        break;
    }

EXIT:
    return FALSE;
}

static void tklock_uiexcept_end(uiexctype_t type, int64_t linger)
{
    if( !(exdata.mask & type) )
        goto EXIT;

    int64_t now = tklock_monotick_get();

    exdata.mask &= ~type;

    linger += now;

    if( exdata.linger_tick < linger )
        exdata.linger_tick = linger;

    if( !exdata.mask ) {
        exdata.mask |= UIEXC_LINGER;
        int delay = (int)(exdata.linger_tick - now);
        if( delay <= 0 )
            exdata.linger_id = g_idle_add(tklock_uiexcept_linger_cb, 0);
        else
            exdata.linger_id = g_timeout_add(delay, tklock_uiexcept_linger_cb, 0);
    }

    tklock_uiexcept_sync_to_datapipe();

EXIT:
    return;
}

static gboolean tklock_uiexcept_notif_cb(gpointer aptr)
{
    (void) aptr;

    if( !exdata.notif_id )
        goto EXIT;

    mce_log(LL_DEBUG, "cancel notification exception");

    exdata.notif_id   = 0;
    exdata.notif_tick = MIN_TICK;
    tklock_uiexcept_end(UIEXC_NOTIF, 0);

EXIT:
    return FALSE;

}

static void tklock_uiexcept_begin(uiexctype_t type, int64_t linger)
{
    if( !exdata.mask ) {
        /* reset existing stats */
        tklock_uiexcept_cancel();

        /* save display and tklock state */
        exdata.tklock  = tklock_datapipe_have_tklock_submode();
        exdata.display = display_state;

        /* initially insync, restore state at end */
        exdata.insync      = true;
        exdata.restore     = true;

        /* deal with transitional display states */
        switch( exdata.display ) {
        case MCE_DISPLAY_POWER_DOWN:
            exdata.display = MCE_DISPLAY_OFF;
            break;
        default:
            break;
        }
    }

    exdata.mask |= type;

    int64_t now = tklock_monotick_get();

    linger += now;

    if( exdata.linger_tick < linger )
        exdata.linger_tick = linger;

    if( type & UIEXC_NOTIF ) {
        mce_log(LL_DEBUG, "setup notification exception");
        if( exdata.notif_id )
            g_source_remove(exdata.notif_id);
        if( exdata.notif_tick < linger )
            exdata.notif_tick = linger;
        int delay = (int)(exdata.notif_tick - now);
        if( delay <= 0 )
            exdata.notif_id = g_idle_add(tklock_uiexcept_notif_cb, 0);
        else
            exdata.notif_id = g_timeout_add(delay, tklock_uiexcept_notif_cb, 0);
    }

    tklock_uiexcept_sync_to_datapipe();
}

/* ========================================================================= *
 * LEGACY HW EVENT INPUT ENABLE/DISABLE STATE MACHINE
 * ========================================================================= */

/** Helper for dealing with enable/disable sysfs files
 *
 * @note Since nothing sensible can be done on error except reporting it,
 *       we don't return the status
 *
 * @param output control structure for enable/disable file
 * @param enable TRUE enable events, FALSE disable events
 */
static void tklock_evctrl_set_state(output_state_t *output, bool enable)
{
    if( !output->path )
        goto EXIT;

    if( !mce_write_number_string_to_file(output, !enable ? 1 : 0) ) {
        mce_log(LL_ERR, "%s: Event status *not* modified", output->path);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "%s: events %s", output->path,
            enable ? "enabled" : "disabled");
EXIT:
    return;
}

/** Disable/Enable keypad input events
 */
static void tklock_evctrl_set_kp_state(bool enable)
{
    static int enabled = -1; // does not match any bool value

    if( !mce_keypad_sysfs_disable_output.path )
        goto EXIT;

    if( enabled == enable )
        goto EXIT;

    mce_log(LL_DEBUG, "%s", enable ? "enable" : "disable");

    if( (enabled = enable) ) {
        /* Enable keypress interrupts (events will be generated by kernel)
         */
        tklock_evctrl_set_state(&mce_keypad_sysfs_disable_output, TRUE);
    }
    else {
        /* Disable keypress interrupts (no events will be generated by kernel)
         */
        tklock_evctrl_set_state(&mce_keypad_sysfs_disable_output, FALSE);
    }
EXIT:
    return;
}

/** Disable/Enable touch screen input events
 */
static void tklock_evctrl_set_ts_state(bool enable)
{
    static int enabled = -1; // does not match any bool value

    if( !mce_touchscreen_sysfs_disable_output.path )
        goto EXIT;

    if( enabled == enable )
        goto EXIT;

    mce_log(LL_DEBUG, "%s", enable ? "enable" : "disable");

    if( (enabled = enable) ) {
        /* Enable touchscreen interrupts
         * (events will be generated by kernel) */
        tklock_evctrl_set_state(&mce_touchscreen_sysfs_disable_output, TRUE);
        g_usleep(MCE_TOUCHSCREEN_CALIBRATION_DELAY);
    }
    else {
        /* Disable touchscreen interrupts
         * (no events will be generated by kernel) */
        tklock_evctrl_set_state(&mce_touchscreen_sysfs_disable_output, FALSE);
    }
EXIT:
    return;
}

/** Disable/Enable doubletap input events
 */
static void tklock_evctrl_set_dt_state(bool enable)
{
    static int enabled = -1; // does not match any bool value

    if( !mce_touchscreen_gesture_control_path )
        goto EXIT;

    if( enabled == enable )
        goto EXIT;

    mce_log(LL_DEBUG, "%s", enable ? "enable" : "disable");

    if( (enabled = enable) ) {
        mce_write_string_to_file(mce_touchscreen_gesture_control_path, "4");
        tklock_dtcalib_start();

        // NOTE: touchscreen inputs must be enabled too
    }
    else {
        tklock_dtcalib_stop();
        mce_write_string_to_file(mce_touchscreen_gesture_control_path, "0");

        /* Disabling the double tap gesture causes recalibration */
        g_usleep(MCE_TOUCHSCREEN_CALIBRATION_DELAY);
    }

EXIT:
    return;
}

/** Process event input enable state for maemo/meego devices
 *
 * This state machine is used for maemo/meego devices (N9, N950,
 * N900, etc) that have separate controls for disabling/enabling
 * input events.
 *
 * Devices that use android style power management (Jolla) handle
 * this implicitly via early/late suspend.
 */
static void tklock_evctrl_rethink(void)
{
    /* state variable hooks:
     *  proximity_state <-- tklock_datapipe_proximity_sensor_cb()
     *  display_state   <-- tklock_datapipe_display_state_cb()
     *  submode         <-- tklock_datapipe_submode_cb()
     *  call_state      <-- tklock_datapipe_call_state_cb()
     */

    bool enable_kp = true;
    bool enable_ts = true;
    bool enable_dt = true;

    /* - - - - - - - - - - - - - - - - - - - *
     * keypad interrupts
     * - - - - - - - - - - - - - - - - - - - */

    /* display must be on/dim */
    switch( display_state ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        break;
    default:
        enable_kp = false;
        break;
    }

    /* tklock must be off */
    if( submode & MCE_TKLOCK_SUBMODE ) {
        enable_kp = false;
    }

    /* If the cover is closed, don't bother */
#if 0 // TODO: keypad slide state is not tracked
    if( lid_cover_state == COVER_CLOSED ) {
        enable_kp = false;
    }
#endif

    // FIXME: USERMODE only?

    /*  Don't disable kp during call (volume keys must work) */
    switch( call_state ) {
    case CALL_STATE_RINGING:
    case CALL_STATE_ACTIVE:
      enable_kp = true;
      break;
    default:
      break;
    }

    /* - - - - - - - - - - - - - - - - - - - *
     * touchscreen interrupts
     * - - - - - - - - - - - - - - - - - - - */

    /* display must be on/dim */
    switch( display_state ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        break;
    default:
        enable_ts = false;
        break;
    }

    // FIXME: USERMODE or ACT_DEAD with alarm?

    /* - - - - - - - - - - - - - - - - - - - *
     * doubletap interrupts
     * - - - - - - - - - - - - - - - - - - - */

    /* display must be off */
    switch( display_state ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
      break;
    default:
      enable_dt = false;
      break;
    }

    /* doubletap gesture policy must not be 0/disabled */
    if( doubletap_gesture_policy == 0 ) {
      enable_dt = false;
    }

#if 0 /* FIXME: check if proximity via sensord works better
       *        with up to date nemomobile image */
    /* proximity sensor must not be covered */
    if( proximity_state != COVER_OPEN ) {
        enable_dt = false;
    }
#endif

    /* Finally, ensure that touchscreen interrupts are enabled
     * if doubletap gestures are enabled */
    if( enable_dt ) {
        enable_ts = true;
    }

    /* - - - - - - - - - - - - - - - - - - - *
     * overrides
     * - - - - - - - - - - - - - - - - - - - */

#if 0 // FIXME: should we pretend soft-off is still supported?
    if( submode & MCE_SOFTOFF_SUBMODE ) {
        enable_kp = false;
        enable_ts = false;
        enable_dt = false;
    }
#endif

#if 0 // FIXME: malf is not really supported yet
    if( submode & MCE_MALF_SUBMODE ) {
        enable_kp = false;
        enable_ts = false;
        enable_dt = false;
    }
#endif

    /* - - - - - - - - - - - - - - - - - - - *
     * set updated state
     * - - - - - - - - - - - - - - - - - - - */

    mce_log(LL_DEBUG, "kp=%d dt=%d ts=%d", enable_kp, enable_dt, enable_ts);
    tklock_evctrl_set_kp_state(enable_kp);
    tklock_evctrl_set_dt_state(enable_dt);
    tklock_evctrl_set_ts_state(enable_ts);
    return;
}

/* ========================================================================= *
 * LEGACY HW DOUBLE TAP CALIBRATION
 * ========================================================================= */

/** Do double tap recalibration on heartbeat */
static gboolean tklock_dtcalib_on_heartbeat = FALSE;

/** Double tap recalibration delays */
static const guint tklock_dtcalib_delays[] = { 2, 4, 8, 16, 30 };

/** Double tap recalibration index */
static guint tklock_dtcalib_index = 0;

/** Double tap recalibration timeout identifier */
static guint tklock_dtcalib_timeout_id = 0;

/** Kick the double tap recalibrating sysfs file unconditionally
 */
static void tklock_dtcalib_now(void)
{
  mce_log(LL_DEBUG, "Recalibrating double tap");
  mce_write_string_to_file(mce_touchscreen_calibration_control_path, "1");
}

/** Kick the double tap recalibrating sysfs file from heartbeat
 */
static void tklock_dtcalib_from_heartbeat(void)
{
    if( tklock_dtcalib_on_heartbeat ) {
        mce_log(LL_DEBUG, "double tap calibration @ heartbeat");
        tklock_dtcalib_now();
    }
}

/** Callback for doubletap recalibration timer
 *
 * @param data Not used.
 *
 * @return Always FALSE for remove event source
 */
static gboolean tklock_dtcalib_cb(gpointer data)
{
    (void)data;

    if( !tklock_dtcalib_timeout_id )
        goto EXIT;

    tklock_dtcalib_timeout_id = 0;

    mce_log(LL_DEBUG, "double tap calibration @ timer");
    tklock_dtcalib_now();

    /* If at last delay, start recalibrating on DSME heartbeat */
    if( tklock_dtcalib_index == G_N_ELEMENTS(tklock_dtcalib_delays) ) {
        tklock_dtcalib_on_heartbeat = TRUE;
        goto EXIT;
    }

    /* Otherwise use next delay */
    tklock_dtcalib_timeout_id =
        g_timeout_add_seconds(tklock_dtcalib_delays[tklock_dtcalib_index++],
                              tklock_dtcalib_cb, NULL);

EXIT:
    return FALSE;
}

/** Cancel doubletap recalibration timeouts
 */
static void tklock_dtcalib_stop(void)
{
    /* stop timer based kicking */
    if( tklock_dtcalib_timeout_id )
        g_source_remove(tklock_dtcalib_timeout_id),
        tklock_dtcalib_timeout_id = 0;

    /* stop heartbeat based kicking */
    tklock_dtcalib_on_heartbeat = FALSE;
}

/** Setup doubletap recalibration timeouts
 */
static void tklock_dtcalib_start(void)
{
    if( !mce_touchscreen_calibration_control_path )
        goto EXIT;

    tklock_dtcalib_stop();

    tklock_dtcalib_index = 0;

    tklock_dtcalib_timeout_id =
        g_timeout_add_seconds(tklock_dtcalib_delays[tklock_dtcalib_index++],
                              tklock_dtcalib_cb, NULL);

EXIT:
    return;
}

/* ========================================================================= *
 * SETTINGS FROM GCONF
 * ========================================================================= */

/** GConf callback for touchscreen/keypad lock related settings
 *
 * @param gcc Unused
 * @param id Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data Unused
 */
static void tklock_gconf_cb(GConfClient *const gcc, const guint id,
                            GConfEntry *const entry, gpointer const data)
{
    (void)gcc;
    (void)data;
    (void)id;

    const GConfValue *gcv = gconf_entry_get_value(entry);

    if( !gcv ) {
        mce_log(LL_DEBUG, "GConf Key `%s' has been unset",
                gconf_entry_get_key(entry));
        goto EXIT;
    }

    if( id == tk_autolock_enabled_cb_id ) {
        tk_autolock_enabled = gconf_value_get_bool(gcv) ? 1 : 0;
        tklock_autolock_rethink();
    }
    else if( id == doubletap_gesture_policy_cb_id ) {
        doubletap_gesture_policy = gconf_value_get_int(gcv);

        if( doubletap_gesture_policy < 0 || doubletap_gesture_policy > 2 ) {
            mce_log(LL_WARN, "Double tap gesture has invalid policy: %d; "
                    "using default", doubletap_gesture_policy);
            doubletap_gesture_policy = DEFAULT_DOUBLETAP_GESTURE_POLICY;
        }

        tklock_evctrl_rethink();
    }
    else if( id == tklock_blank_disable_id ) {
        gint old = tklock_blank_disable;

        tklock_blank_disable = gconf_value_get_int(gcv);

        mce_log(LL_NOTICE, "tklock_blank_disable: %d -> %d",
                old, tklock_blank_disable);

#if 0 // FIXME: this needs to be handled at modules/display.c
        if( tklock_blank_disable == old ) {
            // no need to change the timers
        }
        else if( tklock_visual_blank_timeout_cb_id ) {
            setup_tklock_visual_blank_timeout();
        }
        else if( tklock_dim_timeout_cb_id ) {
            setup_tklock_dim_timeout();
        }
#endif

    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }

EXIT:
    return;
}

/** Get intial gconf based settings and add change notifiers
 */
static void tklock_gconf_init(void)
{
    /* Config tracking for disabling automatic screen dimming/blanking
     * while showing lockscreen. This is demo/debugging feature, so sane
     * defaults must be used and no error checking is needed. */
    mce_gconf_notifier_add(MCE_GCONF_LOCK_PATH,
                           MCE_GCONF_TK_AUTO_BLANK_DISABLE_PATH,
                           tklock_gconf_cb,
                           &tklock_blank_disable_id);

    mce_gconf_get_int(MCE_GCONF_TK_AUTO_BLANK_DISABLE_PATH,
                      &tklock_blank_disable);

    /* Touchscreen/keypad autolock */
    /* Since we've set a default, error handling is unnecessary */
    mce_gconf_get_bool(MCE_GCONF_TK_AUTOLOCK_ENABLED_PATH,
                       &tk_autolock_enabled);

    /* Touchscreen/keypad autolock enabled/disabled */
    mce_gconf_notifier_add(MCE_GCONF_LOCK_PATH,
                           MCE_GCONF_TK_AUTOLOCK_ENABLED_PATH,
                           tklock_gconf_cb,
                           &tk_autolock_enabled_cb_id);

    /* Touchscreen/keypad double-tap gesture policy */
    mce_gconf_get_int(MCE_GCONF_TK_DOUBLE_TAP_GESTURE_PATH,
                      &doubletap_gesture_policy);

    if( doubletap_gesture_policy < 0 || doubletap_gesture_policy > 2 ) {
        mce_log(LL_WARN, "Double tap gesture has invalid policy: %d; "
                "using default", doubletap_gesture_policy);
        doubletap_gesture_policy = DEFAULT_DOUBLETAP_GESTURE_POLICY;
    }

    /* Touchscreen/keypad autolock enabled/disabled */
    mce_gconf_notifier_add(MCE_GCONF_LOCK_PATH,
                           MCE_GCONF_TK_DOUBLE_TAP_GESTURE_PATH,
                           tklock_gconf_cb,
                           &doubletap_gesture_policy_cb_id);
}

/** Remove gconf change notifiers
 */
static void tklock_gconf_quit(void)
{
    if( doubletap_gesture_policy_cb_id )
        mce_gconf_notifier_remove(GINT_TO_POINTER(doubletap_gesture_policy_cb_id), 0);

    if( tk_autolock_enabled_cb_id )
        mce_gconf_notifier_remove(GINT_TO_POINTER(tk_autolock_enabled_cb_id), 0);

    if( tklock_blank_disable_id )
        mce_gconf_notifier_remove(GINT_TO_POINTER(tklock_blank_disable_id), 0);
}

/* ========================================================================= *
 * SETTINGS FROM MCE.INI
 * ========================================================================= */

/** Get configuration options */
static void tklock_config_init(void)
{
    blank_immediately =
        mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
                          MCE_CONF_BLANK_IMMEDIATELY,
                          DEFAULT_BLANK_IMMEDIATELY);

    dim_immediately =
        mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
                          MCE_CONF_DIM_IMMEDIATELY,
                          DEFAULT_DIM_IMMEDIATELY);

    dim_delay =
        mce_conf_get_int(MCE_CONF_TKLOCK_GROUP,
                         MCE_CONF_DIM_DELAY,
                         DEFAULT_DIM_DELAY);

    disable_ts_immediately =
        mce_conf_get_int(MCE_CONF_TKLOCK_GROUP,
                         MCE_CONF_TS_OFF_IMMEDIATELY,
                         DEFAULT_TS_OFF_IMMEDIATELY);

    /* Fallback in case double tap event is not supported */
    if( !mce_touchscreen_gesture_control_path &&
        disable_ts_immediately == 2 )
        disable_ts_immediately = 1;

    disable_kp_immediately =
        mce_conf_get_int(MCE_CONF_TKLOCK_GROUP,
                         MCE_CONF_KP_OFF_IMMEDIATELY,
                         DEFAULT_KP_OFF_IMMEDIATELY);

    autolock_with_open_slide =
        mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
                          MCE_CONF_AUTOLOCK_SLIDE_OPEN,
                          DEFAULT_AUTOLOCK_SLIDE_OPEN);

    proximity_lock_with_open_slide =
        mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
                          MCE_CONF_PROXIMITY_LOCK_SLIDE_OPEN,
                          DEFAULT_PROXIMITY_LOCK_SLIDE_OPEN);

    always_lock_on_slide_close =
        mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
                          MCE_CONF_LOCK_ON_SLIDE_CLOSE,
                          DEFAULT_LOCK_ON_SLIDE_CLOSE);

    lens_cover_unlock =
        mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
                          MCE_CONF_LENS_COVER_UNLOCK,
                          DEFAULT_LENS_COVER_UNLOCK);

    volkey_visual_trigger =
        mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
                          MCE_CONF_VOLKEY_VISUAL_TRIGGER,
                          DEFAULT_VOLKEY_VISUAL_TRIGGER);
}

/* ========================================================================= *
 * SYSFS PROBING
 * ========================================================================= */

/** Init event control files
 */
static void tklock_sysfs_probe(void)
{
    /* touchscreen event control interface */
    if (g_access(MCE_RX51_KEYBOARD_SYSFS_DISABLE_PATH, W_OK) == 0) {
        mce_keypad_sysfs_disable_output.path =
            MCE_RX51_KEYBOARD_SYSFS_DISABLE_PATH;
    }
    else if (g_access(MCE_RX44_KEYBOARD_SYSFS_DISABLE_PATH, W_OK) == 0) {
        mce_keypad_sysfs_disable_output.path =
            MCE_RX44_KEYBOARD_SYSFS_DISABLE_PATH;
    }
    else if (g_access(MCE_KEYPAD_SYSFS_DISABLE_PATH, W_OK) == 0) {
        mce_keypad_sysfs_disable_output.path =
            MCE_KEYPAD_SYSFS_DISABLE_PATH;
    }
    else {
        mce_log(LL_INFO, "No touchscreen event control interface available");
    }

    /* keypress event control interface */
    if (g_access(MCE_RM680_TOUCHSCREEN_SYSFS_DISABLE_PATH, W_OK) == 0) {
        mce_touchscreen_sysfs_disable_output.path =
            MCE_RM680_TOUCHSCREEN_SYSFS_DISABLE_PATH;
    }
    else if (g_access(MCE_RX44_TOUCHSCREEN_SYSFS_DISABLE_PATH_KERNEL2637, W_OK) == 0) {
        mce_touchscreen_sysfs_disable_output.path =
            MCE_RX44_TOUCHSCREEN_SYSFS_DISABLE_PATH_KERNEL2637;
    }
    else if (g_access(MCE_RX44_TOUCHSCREEN_SYSFS_DISABLE_PATH, W_OK) == 0) {
        mce_touchscreen_sysfs_disable_output.path =
            MCE_RX44_TOUCHSCREEN_SYSFS_DISABLE_PATH;
    }
    else {
        mce_log(LL_INFO, "No keypress event control interface available");
    }

    /* touchscreen gesture control interface */
    if (g_access(MCE_RM680_DOUBLETAP_SYSFS_PATH, W_OK) == 0) {
        mce_touchscreen_gesture_control_path =
            MCE_RM680_DOUBLETAP_SYSFS_PATH;
    }
    else {
        mce_log(LL_INFO, "No touchscreen gesture control interface available");
    }

    /* touchscreen calibration control interface */
    if (g_access(MCE_RM680_TOUCHSCREEN_CALIBRATION_PATH, W_OK) == 0) {
        mce_touchscreen_calibration_control_path =
            MCE_RM680_TOUCHSCREEN_CALIBRATION_PATH;
    }
    else {
        mce_log(LL_INFO, "No touchscreen calibration control interface "
                "available");
    }
}

/* ========================================================================= *
 * DBUS IPC WITH SYSTEMUI
 * ========================================================================= */

static void tklock_ui_eat_event(void)
{
    /* FIXME: get rid of this function and all explicit event eater ipc */

    const char   *cb_service   = MCE_SERVICE;
    const char   *cb_path      = MCE_REQUEST_PATH;
    const char   *cb_interface = MCE_REQUEST_IF;
    const char   *cb_method    = MCE_TKLOCK_CB_REQ;
    dbus_bool_t   flicker_key  = has_flicker_key;
    dbus_uint32_t mode         = TKLOCK_ONEINPUT;
    dbus_bool_t   silent       = TRUE;

    mce_log(LL_DEBUG, "sending tklock ui event eater");

    /* org.nemomobile.lipstick.screenlock.tklock_open */
    dbus_send(SYSTEMUI_SERVICE, SYSTEMUI_REQUEST_PATH,
              SYSTEMUI_REQUEST_IF, SYSTEMUI_TKLOCK_OPEN_REQ,
              NULL,
              DBUS_TYPE_STRING, &cb_service,
              DBUS_TYPE_STRING, &cb_path,
              DBUS_TYPE_STRING, &cb_interface,
              DBUS_TYPE_STRING, &cb_method,
              DBUS_TYPE_UINT32, &mode,
              DBUS_TYPE_BOOLEAN, &silent,
              DBUS_TYPE_BOOLEAN, &flicker_key,
              DBUS_TYPE_INVALID);
}

static void tklock_ui_open(void)
{
    const char   *cb_service   = MCE_SERVICE;
    const char   *cb_path      = MCE_REQUEST_PATH;
    const char   *cb_interface = MCE_REQUEST_IF;
    const char   *cb_method    = MCE_TKLOCK_CB_REQ;
    dbus_bool_t   flicker_key  = has_flicker_key;
    dbus_uint32_t mode         = TKLOCK_ENABLE_VISUAL;
    dbus_bool_t   silent       = TRUE;

    mce_log(LL_DEBUG, "sending tklock ui open");
    /* org.nemomobile.lipstick.screenlock.tklock_open */
    dbus_send(SYSTEMUI_SERVICE, SYSTEMUI_REQUEST_PATH,
              SYSTEMUI_REQUEST_IF, SYSTEMUI_TKLOCK_OPEN_REQ,
              NULL,
              DBUS_TYPE_STRING, &cb_service,
              DBUS_TYPE_STRING, &cb_path,
              DBUS_TYPE_STRING, &cb_interface,
              DBUS_TYPE_STRING, &cb_method,
              DBUS_TYPE_UINT32, &mode,
              DBUS_TYPE_BOOLEAN, &silent,
              DBUS_TYPE_BOOLEAN, &flicker_key,
              DBUS_TYPE_INVALID);
}

static void tklock_ui_close(void)
{
    dbus_bool_t silent = TRUE;

    mce_log(LL_DEBUG, "sending tklock ui close");
    /* org.nemomobile.lipstick.screenlock.tklock_close */
    dbus_send(SYSTEMUI_SERVICE, SYSTEMUI_REQUEST_PATH,
              SYSTEMUI_REQUEST_IF, SYSTEMUI_TKLOCK_CLOSE_REQ,
              NULL,
              DBUS_TYPE_BOOLEAN, &silent,
              DBUS_TYPE_INVALID);
}

static void tklock_ui_set(bool enable)
{
    if( enable ) {
        if( system_state != MCE_STATE_USER ) {
            mce_log(LL_INFO, "deny tklock; not in user mode");
            enable = false;
        }
        else if( !lipstick_available ) {
            mce_log(LL_INFO, "deny tklock; lipstick not running");
            enable = false;
        }
    }

    if( tklock_ui_sent != enable ) {
        if( (tklock_ui_sent = tklock_ui_enabled = enable) ) {
            if( lipstick_available )
                tklock_ui_open();
            mce_add_submode_int32(MCE_TKLOCK_SUBMODE);
        }
        else {
            if( lipstick_available )
                tklock_ui_close();
            mce_rem_submode_int32(MCE_TKLOCK_SUBMODE);
        }
        tklock_dbus_send_tklock_mode(0);
    }
}

/** Handle reply to device lock state query
 */
static void tklock_ui_get_device_lock_cb(DBusPendingCall *pc, void *aptr)
{
    (void)aptr;

    DBusMessage  *rsp = 0;
    DBusError     err = DBUS_ERROR_INIT;
    dbus_int32_t  val = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) )
	goto EXIT;

    if( dbus_set_error_from_message(&err, rsp) ) {
	mce_log(LL_ERR, "%s: %s", err.name, err.message);
	goto EXIT;
    }
    if( !dbus_message_get_args(rsp, &err,
			       DBUS_TYPE_INT32, &val,
			       DBUS_TYPE_INVALID) ) {
	mce_log(LL_ERR, "%s: %s", err.name, err.message);
	goto EXIT;
    }

    mce_log(LL_INFO, "device lock status reply: state=%d", val);
    tklock_datapipe_set_device_lock_active(val);

EXIT:
    if( rsp ) dbus_message_unref(rsp);
    dbus_pending_call_unref(pc);
    dbus_error_free(&err);
}

/** Initiate asynchronous device lock state query
 */
static void tklock_ui_get_device_lock(void)
{
    mce_log(LL_DEBUG, "query device lock status");
    dbus_send(SYSTEMUI_SERVICE,
	      "/devicelock",
	      "org.nemomobile.lipstick.devicelock",
	      "state",
	      tklock_ui_get_device_lock_cb,
	      DBUS_TYPE_INVALID);
}

/* ========================================================================= *
 * DBUS MESSAGE HANDLERS
 * ========================================================================= */

/**
 * Send the touchscreen/keypad lock mode
 *
 * @param method_call A DBusMessage to reply to;
 *                    pass NULL to send a tklock mode signal instead
 * @return TRUE on success, FALSE on failure
 */
static gboolean tklock_dbus_send_tklock_mode(DBusMessage *const method_call)
{
    gboolean    status = FALSE;
    DBusMessage *msg   = NULL;
    const char  *mode  = (tklock_datapipe_have_tklock_submode() ?
                          MCE_TK_LOCKED : MCE_TK_UNLOCKED);

    /* If method_call is set, send a reply. Otherwise, send a signal. */
    if( method_call )
        msg = dbus_new_method_reply(method_call);
    else
        msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_TKLOCK_MODE_SIG);

    mce_log(LL_DEBUG, "send tklock mode %s: %s",
            method_call ? "reply" : "signal", mode);

    if( !dbus_message_append_args(msg,
                                  DBUS_TYPE_STRING, &mode,
                                  DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to append %sargument to D-Bus message "
                "for %s.%s",
                method_call ? "reply " : "",
                method_call ? MCE_REQUEST_IF : MCE_SIGNAL_IF,
                method_call ? MCE_TKLOCK_MODE_GET : MCE_TKLOCK_MODE_SIG);
        goto EXIT;
    }

    /* Send the message */
    status = dbus_send_message(msg), msg = 0;

EXIT:
    if( msg ) dbus_message_unref(msg);

    return status;
}

/**
 * D-Bus callback for the get tklock mode method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean tklock_dbus_mode_get_req_cb(DBusMessage *const msg)
{
    gboolean status = FALSE;

    mce_log(LL_DEBUG, "Received tklock mode get request");

    /* Try to send a reply that contains the current tklock mode */
    if( !tklock_dbus_send_tklock_mode(msg) )
        goto EXIT;

    status = TRUE;

EXIT:
    return status;
}

/**
 * D-Bus callback for the tklock mode change method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean tklock_dbus_mode_change_req_cb(DBusMessage *const msg)
{
    dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
    const char *mode = NULL;
    gboolean status = FALSE;
    DBusError error = DBUS_ERROR_INIT;

    mce_log(LL_DEBUG, "Received tklock mode change request");

    if( !dbus_message_get_args(msg, &error,
                               DBUS_TYPE_STRING, &mode,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to get argument from %s.%s: %s",
                MCE_REQUEST_IF, MCE_TKLOCK_MODE_CHANGE_REQ,
                error.message);
        goto EXIT;
    }

    int state = LOCK_UNDEF;

    if (!strcmp(MCE_TK_LOCKED, mode))
        state = LOCK_ON;
    else if (!strcmp(MCE_TK_LOCKED_DIM, mode))
        state = LOCK_ON_DIMMED;
    else if (!strcmp(MCE_TK_LOCKED_DELAY, mode))
        state = LOCK_ON_DELAYED;
    else if (!strcmp(MCE_TK_UNLOCKED, mode))
        state = LOCK_OFF;
    else
        mce_log(LL_WARN, "Received an invalid tklock mode; ignoring");

    mce_log(LL_DEBUG, "mode: %s/%d", mode, state);

    if( state != LOCK_UNDEF )
        tklock_datapipe_tk_lock_cb(GINT_TO_POINTER(state));

    if( no_reply )
        status = TRUE;
    else {
        DBusMessage *reply = dbus_new_method_reply(msg);
        status = dbus_send_message(reply);
    }

EXIT:
    dbus_error_free(&error);
    return status;
}

/**
 * D-Bus callback from SystemUI touchscreen/keypad lock
 *
 * @todo the calls to disable_tklock/open_tklock_ui need error handling
 *
 * @param msg D-Bus message with the lock status
 * @return TRUE on success, FALSE on failure
 */
static gboolean tklock_dbus_systemui_callback_cb(DBusMessage *const msg)
{
    dbus_int32_t result = INT_MAX;
    gboolean     status = FALSE;
    DBusError    error  = DBUS_ERROR_INIT;

    if( !dbus_message_get_args(msg, &error,
                              DBUS_TYPE_INT32, &result,
                              DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to get argument from %s.%s: %s",
                MCE_REQUEST_IF, MCE_TKLOCK_CB_REQ, error.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "tklock callback value: %d", result);

    switch( result ) {
    case TKLOCK_UNLOCK:
        tklock_ui_set(false);
        break;

    default:
    case TKLOCK_CLOSED:
        break;
    }

    status = TRUE;

EXIT:
    dbus_error_free(&error);
    return status;
}

/** D-Bus callback for handling device lock state changed signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean tklock_dbus_device_lock_changed_cb(DBusMessage *const msg)
{
    DBusError    err = DBUS_ERROR_INIT;
    dbus_int32_t val = 0;

    if( !msg )
	goto EXIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_INT32, &val,
                               DBUS_TYPE_INVALID) ) {
	mce_log(LL_ERR, "Failed to parse device lock signal: %s: %s",
		err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "received device lock signal: state=%d", val);
    tklock_datapipe_set_device_lock_active(val);

EXIT:
    dbus_error_free(&err);

    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t handlers[] =
{
    /* signals */
    {
        .interface = "org.nemomobile.lipstick.devicelock",
        .name      = "stateChanged",
        .rules     = "path='/devicelock'",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = tklock_dbus_device_lock_changed_cb,
    },
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_TKLOCK_MODE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_mode_get_req_cb,
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_TKLOCK_MODE_CHANGE_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_mode_change_req_cb,
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_TKLOCK_CB_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_systemui_callback_cb,
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Add dbus handlers
 */
static void mce_tklock_init_dbus(void)
{
    mce_dbus_handler_register_array(handlers);
}

/** Remove dbus handlers
 */
static void mce_tklock_quit_dbus(void)
{
    mce_dbus_handler_unregister_array(handlers);
}

/* ========================================================================= *
 * MODULE LOAD/UNLOAD
 * ========================================================================= */

/**
 * Init function for the touchscreen/keypad lock component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_tklock_init(void)
{
    gboolean status = FALSE;

    /* paths must be probed 1st, the results are used
     * to validate configuration and settings */
    tklock_sysfs_probe();

    /* get static configuration */
    tklock_config_init();

    /* get dynamic config, install change monitors */
    tklock_gconf_init();

    /* attach to internal state variables */
    tklock_datapipe_init();

    /* set up dbus message handlers */
    mce_tklock_init_dbus();

    status = TRUE;

    return status;
}

/**
 * Exit function for the touchscreen/keypad lock component
 */
void mce_tklock_exit(void)
{
    /* remove all handlers */
    mce_tklock_quit_dbus();
    tklock_datapipe_quit();
    tklock_gconf_quit();

    /* cancel all timers */
    tklock_autolock_cancel();
    tklock_proxlock_cancel();
    tklock_uiexcept_cancel();
    tklock_dtcalib_stop();

    // FIXME: check that final state is sane

    return;
}
