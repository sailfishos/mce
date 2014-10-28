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

#include "tklock.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-io.h"
#include "mce-conf.h"
#include "mce-gconf.h"
#include "mce-dbus.h"
#include "evdev.h"

#ifdef ENABLE_WAKELOCKS
# include "libwakelock.h"
#endif

#include "modules/doubletap.h"

#include "systemui/dbus-names.h"
#include "systemui/tklock-dbus-names.h"

#include <linux/input.h>

#include <unistd.h>
#include <string.h>

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include <glib/gstdio.h>

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
    EXCEPTION_LENGTH_CALL_IN  = 5000, // [ms]
    EXCEPTION_LENGTH_CALL_OUT = 2500, // [ms]
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

/** Helper for evaluation number of items in an array */
#define numof(a) (sizeof(a)/sizeof*(a))

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** Max valid time_t value in milliseconds */
#define MAX_TICK (INT_MAX * (int64_t)1000)

/** Min valid time_t value in milliseconds */
#define MIN_TICK  0

/** Maximum number of concurrent notification ui exceptions */
#define TKLOCK_NOTIF_SLOTS 32

/** Signal to send when lpm ui state changes */
#define MCE_LPM_UI_MODE_SIG "lpm_ui_mode_ind"

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

typedef struct
{
    /** BOOTTIME tick when notification autostops */
    int64_t  ns_until;

    /** Amount of ms autostop extends from user input */
    int64_t  ns_renew;

    /** Private D-Bus name of the slot owner */
    gchar   *ns_owner;

    /** Assumed unique identification string */
    gchar   *ns_name;

} tklock_notif_slot_t;

typedef struct
{
    /** Array of notification slots */
    tklock_notif_slot_t tn_slot[TKLOCK_NOTIF_SLOTS];

    /** BOOTTIME linger tick from deactivated slots */
    int64_t             tn_linger;

    /** Timer id for autostopping notification slots */
    guint               tn_autostop_id;

    /** Slot owner D-Bus name monitoring list */
    GSList             *tn_monitor_list;

} tklock_notif_state_t;

/** Proximity sensor history */
typedef struct
{
    /** Monotonic timestamp, ms resolution */
    int64_t       tick;

    /** Proximity sensor state */
    cover_state_t state;

} ps_history_t;

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

// time utilities

static void     tklock_monotime_get(struct timeval *tv);
static int64_t  tklock_monotick_get(void);

// datapipe values and triggers

static void     tklock_datapipe_system_state_cb(gconstpointer data);
static void     tklock_datapipe_device_lock_active_cb(gconstpointer data);
static void     tklock_datapipe_device_resumed_cb(gconstpointer data);
static void     tklock_datapipe_lipstick_available_cb(gconstpointer data);
static void     tklock_datapipe_update_mode_cb(gconstpointer data);
static void     tklock_datapipe_display_state_cb(gconstpointer data);
static void     tklock_datapipe_proximity_update(void);
static gboolean tklock_datapipe_proximity_uncover_cb(gpointer data);
static void     tklock_datapipe_proximity_uncover_cancel(void);
static void     tklock_datapipe_proximity_uncover_schedule(void);
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
static void     tklock_autolock_reschedule(void);
static void     tklock_autolock_schedule(int delay);
static void     tklock_autolock_cancel(void);
static void     tklock_autolock_rethink(void);
static void     tklock_autolock_pre_transition_actions(void);

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
static void     tklock_uiexcept_begin(uiexctype_t type, int64_t linger);
static void     tklock_uiexcept_end(uiexctype_t type, int64_t linger);
static void     tklock_uiexcept_cancel(void);
static void     tklock_uiexcept_finish(void);
static void     tklock_uiexcept_deny_state_restore(void);
static void     tklock_uiexcept_rethink(void);

// low power mode ui state machine

static void     tklock_lpmui_set_state(bool enable);
static void     tklock_lpmui_reset_history(void);
static void     tklock_lpmui_update_history(cover_state_t state);
static bool     tklock_lpmui_probe_from_pocket(void);
static bool     tklock_lpmui_probe_on_table(void);
static bool     tklock_lpmui_probe(void);
static void     tklock_lpmui_rethink(void);
static void     tklock_lpmui_pre_transition_actions(void);

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

static void     tklock_gconf_sanitize_doubletap_gesture_policy(void);

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

static void     tklock_ui_send_lpm_signal(bool enabled);
static void     tklock_ui_enable_lpm(void);
static void     tklock_ui_disable_lpm(void);

// dbus ipc

static gboolean tklock_dbus_send_tklock_mode(DBusMessage *const method_call);

static gboolean tklock_dbus_mode_get_req_cb(DBusMessage *const msg);
static gboolean tklock_dbus_mode_change_req_cb(DBusMessage *const msg);
static gboolean tklock_dbus_systemui_callback_cb(DBusMessage *const msg);

static gboolean tklock_dbus_device_lock_changed_cb(DBusMessage *const msg);

static gboolean tklock_dbus_notification_beg_cb(DBusMessage *const msg);
static gboolean tklock_dbus_notification_end_cb(DBusMessage *const msg);

static void     mce_tklock_init_dbus(void);
static void     mce_tklock_quit_dbus(void);

// NOTIFICATION_SLOTS

static void     tklock_notif_slot_init(tklock_notif_slot_t *self);
static void     tklock_notif_slot_free(tklock_notif_slot_t *self);
static void     tklock_notif_slot_set(tklock_notif_slot_t *self, const char *owner, const char *name, int64_t until, int64_t renew);
static bool     tklock_notif_slot_is_free(const tklock_notif_slot_t *self);
static bool     tklock_notif_slot_has_name(const tklock_notif_slot_t *self, const char *name);
static bool     tklock_notif_slot_validate(tklock_notif_slot_t *self, int64_t now);
static bool     tklock_notif_slot_renew(tklock_notif_slot_t *self, int64_t now);
static bool     tklock_notif_slot_has_owner(const tklock_notif_slot_t *self, const char *owner);
static gchar   *tklock_notif_slot_steal_owner(tklock_notif_slot_t *self);

// NOTIFICATION_API

static void     tklock_notif_init(void);
static void     tklock_notif_quit(void);
static gboolean tklock_notif_autostop_cb(gpointer aptr);
static void     tklock_notif_cancel_autostop(void);
static void     tklock_notif_schedule_autostop(gint delay);
static void     tklock_notif_update_state(void);
static void     tklock_notif_extend_by_renew(void);
static void     tklock_notif_vacate_slot(const char *owner, const char *name, int64_t linger);
static void     tklock_notif_reserve_slot(const char *owner, const char *name, int64_t length, int64_t renew);
static void     tklock_notif_vacate_slots_from(const char *owner);
static size_t   tklock_notif_count_slots_from(const char *owner);

static gboolean tklock_notif_owner_dropped_cb(DBusMessage *const msg);
static void     tklock_notif_add_owner_monitor(const char *owner);
static void     tklock_notif_remove_owner_monitor(const char *owner);

static void     mce_tklock_begin_notification(const char *owner, const char *name, int64_t length, int64_t renew);
static void     mce_tklock_end_notification(const char *owner, const char *name, int64_t linger);

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
static gint doubletap_gesture_policy = DBLTAP_ACTION_DEFAULT;
/** GConf callback ID for doubletap_gesture_policy */
static guint doubletap_gesture_policy_cb_id = 0;

/** Touchscreen double tap gesture enable mode */
static gint doubletap_enable_mode = DBLTAP_ENABLE_DEFAULT;
/** GConf callback ID for doubletap_enable_mode */
static guint doubletap_enable_mode_cb_id = 0;

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

/** Proximity state history for triggering low power mode ui */
static ps_history_t tklock_lpmui_hist[8];

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
        mce_log(LL_DEVEL, "device lock state = %s", locked ? "locked" : "unlocked");
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

    tklock_uiexcept_rethink();

    tklock_autolock_rethink();

EXIT:
    return;
}

/** Resumed from suspend notification */
static void tklock_datapipe_device_resumed_cb(gconstpointer data)
{
        (void) data;

        /* We do not want to wakeup from suspend just to end the
         * grace period, so regular timer is used for it. However,
         * if we happen to resume for some other reason, check if
         * the timeout has already passed */

        tklock_autolock_reschedule();
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

/** Update mode is active; assume false */
static bool update_mode = false;

/** Change notifications for update_mode
 */
static void tklock_datapipe_update_mode_cb(gconstpointer data)
{
    bool prev = update_mode;
    update_mode = GPOINTER_TO_INT(data);

    if( update_mode == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "update_mode = %d -> %d", prev, update_mode);

    if( update_mode ) {
        /* undo tklock when update mode starts */
        execute_datapipe(&tk_lock_pipe,
                         GINT_TO_POINTER(LOCK_OFF),
                         USE_INDATA, CACHE_INDATA);
    }

EXIT:
    return;
}

/** Display state; undefined initially, can't assume anything */
static display_state_t display_state = MCE_DISPLAY_UNDEF;

/** Next Display state; undefined initially, can't assume anything */
static display_state_t display_state_next = MCE_DISPLAY_UNDEF;

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

/** Pre-change notifications for display_state
 */
static void tklock_datapipe_display_state_next_cb(gconstpointer data)
{
    display_state_next = GPOINTER_TO_INT(data);

    if( display_state_next == display_state )
        goto EXIT;

    tklock_autolock_pre_transition_actions();
    tklock_lpmui_pre_transition_actions();

EXIT:
    return;

}

/** Call state; assume no active calls */
static call_state_t call_state = CALL_STATE_NONE;

/** Actual proximity state; assume not covered */
static cover_state_t proximity_state_actual = COVER_OPEN;

/** Effective proximity state; assume not covered */
static cover_state_t proximity_state_effective = COVER_OPEN;

/** Timer id for delayed proximity uncovering */
static guint tklock_datapipe_proximity_uncover_id = 0;

/** Set effective proximity state from current sensor state
 */
static void tklock_datapipe_proximity_update(void)
{
    if( proximity_state_effective == proximity_state_actual )
        goto EXIT;

    mce_log(LL_DEVEL, "proximity_state_effective = %d -> %d",
            proximity_state_effective, proximity_state_actual);

    proximity_state_effective = proximity_state_actual;

    tklock_uiexcept_rethink();
    tklock_proxlock_rethink();
    tklock_evctrl_rethink();

    /* consider moving to lpm ui */
    tklock_lpmui_rethink();

EXIT:
    return;
}

/** Timer callback for handling delayed proximity uncover
 */
static gboolean tklock_datapipe_proximity_uncover_cb(gpointer data)
{
    (void)data;

    if( !tklock_datapipe_proximity_uncover_id )
        goto EXIT;

    tklock_datapipe_proximity_uncover_id = 0;

    tklock_datapipe_proximity_update();

    wakelock_unlock("mce_proximity_stm");

EXIT:
    return FALSE;
}

/** Cancel delayed proximity uncovering
 */
static void tklock_datapipe_proximity_uncover_cancel(void)
{
    if( tklock_datapipe_proximity_uncover_id ) {
        g_source_remove(tklock_datapipe_proximity_uncover_id),
            tklock_datapipe_proximity_uncover_id = 0;
        wakelock_unlock("mce_proximity_stm");
    }
}

enum {
    /** Default delay for delaying proximity uncovered handling [ms] */
    PROXIMITY_DELAY_DEFAULT = 100,

    /** Delay for delaying proximity uncovered handling during calls [ms] */
    PROXIMITY_DELAY_INCALL  = 500,
};

/** Schedule delayed proximity uncovering
 */
static void tklock_datapipe_proximity_uncover_schedule(void)
{
    if( tklock_datapipe_proximity_uncover_id )
        g_source_remove(tklock_datapipe_proximity_uncover_id);
    else
        wakelock_lock("mce_proximity_stm", -1);

    int delay = PROXIMITY_DELAY_DEFAULT;

    if( call_state == CALL_STATE_ACTIVE )
        delay = PROXIMITY_DELAY_INCALL;

    tklock_datapipe_proximity_uncover_id =
        g_timeout_add(delay, tklock_datapipe_proximity_uncover_cb, 0);
}

/** Change notifications for proximity_state_actual
 */
static void tklock_datapipe_proximity_sensor_cb(gconstpointer data)
{
    cover_state_t prev = proximity_state_actual;
    proximity_state_actual = GPOINTER_TO_INT(data);

    if( proximity_state_actual == COVER_UNDEF )
        proximity_state_actual = COVER_OPEN;

    if( proximity_state_actual == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "proximity_state_actual = %d -> %d", prev, proximity_state_actual);

    /* update lpm ui proximity history using raw data */
    tklock_lpmui_update_history(proximity_state_actual);

    if( proximity_state_actual == COVER_OPEN ) {
        tklock_datapipe_proximity_uncover_schedule();
    }
    else {
        tklock_datapipe_proximity_uncover_cancel();
        tklock_datapipe_proximity_update();
    }

EXIT:
    return;
}

/** Change notifications for call_state
 */
static void tklock_datapipe_call_state_cb(gconstpointer data)
{
    /* Default to using shorter outgoing call linger time */
    static int64_t linger_time = EXCEPTION_LENGTH_CALL_OUT;

    call_state_t prev = call_state;
    call_state = GPOINTER_TO_INT(data);

    if( call_state == CALL_STATE_INVALID )
        call_state = CALL_STATE_NONE;

    if( call_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "call_state = %d -> %d", prev, call_state);

    switch( call_state ) {
    case CALL_STATE_RINGING:
        /* Switch to using longer incoming call linger time */
        linger_time = EXCEPTION_LENGTH_CALL_IN;

        /* Fall through */

    case CALL_STATE_ACTIVE:
        tklock_uiexcept_begin(UIEXC_CALL, 0);
        break;

    default:
        tklock_uiexcept_end(UIEXC_CALL, linger_time);

        /* Restore linger time to default again */
        linger_time = EXCEPTION_LENGTH_CALL_OUT;
        break;
    }

    // display on/off policy
    tklock_uiexcept_rethink();

    // volume keys during call
    tklock_evctrl_rethink();
EXIT:
    return;
}

/** Music playback state; assume not playing */
static bool music_playback = false;

/** Change notifications for music_playback
 */
static void tklock_datapipe_music_playback_cb(gconstpointer data)
{
    bool prev = music_playback;
    music_playback = GPOINTER_TO_INT(data);

    if( music_playback == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "music_playback = %d -> %d", prev, music_playback);

    // volume keys during playback
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

    mce_tklock_begin_notification(0, "mce_charger_state",
                                  EXCEPTION_LENGTH_CHARGER, -1);

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

#if 0 /* At the moment there is no notification associated with
       * battery full -> no need to turn the display on */
    if( battery_status == BATTERY_STATUS_FULL ) {
        mce_tklock_begin_notification(0, "mce_battery_full",
                                      EXCEPTION_LENGTH_BATTERY, -1);
    }
#endif

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

    mce_tklock_begin_notification(0, "mce_usb_cable_state",
                                  EXCEPTION_LENGTH_CHARGER, -1);

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

    mce_tklock_begin_notification(0, "mce_jack_sense", EXCEPTION_LENGTH_JACK, -1);

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

    mce_tklock_begin_notification(0, "mce_camera_button",
                                  EXCEPTION_LENGTH_CAMERA, -1);
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
        mce_tklock_begin_notification(0, "mce_camera_key",
                                      EXCEPTION_LENGTH_CAMERA, -1);
        break;

    case KEY_VOLUMEDOWN:
    case KEY_VOLUMEUP:
        mce_log(LL_DEBUG, "volume key");
        mce_tklock_begin_notification(0, "mce_volume_key",
                                      EXCEPTION_LENGTH_VOLUME, -1);
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

    /* Forget lpm ui triggering history
     * whenever exception state changes */
    tklock_lpmui_reset_history();

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

    // was tklock removed?
    if( (prev & MCE_TKLOCK_SUBMODE) && !(submode & MCE_TKLOCK_SUBMODE) ) {
        switch( display_state_next ) {
        case MCE_DISPLAY_LPM_ON:
        case MCE_DISPLAY_LPM_OFF:
            /* We're currently in or transitioning to lpm display state
             * and tklock just got removed. Normally this should not
             * happen, so emit error message to journal. */
            mce_log(LL_ERR, "tklock submode was removed in lpm state");

            /* Nevertheless, removal of tklock means there is something
             * happening at the ui side - and probably the best course of
             * action is to cancel lpm state by turning on the display. */
            execute_datapipe(&display_state_req_pipe,
                             GINT_TO_POINTER(MCE_DISPLAY_ON),
                             USE_INDATA, CACHE_INDATA);
            break;

        default:
        case MCE_DISPLAY_UNDEF:
        case MCE_DISPLAY_OFF:
        case MCE_DISPLAY_DIM:
        case MCE_DISPLAY_ON:
            // nop
            break;
        }
    }
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

    switch( doubletap_enable_mode ) {
    case DBLTAP_ENABLE_NEVER:
        mce_log(LL_DEVEL, "[doubletap] ignored due to setting=never");
        goto EXIT;

    case DBLTAP_ENABLE_ALWAYS:
        break;

    default:
    case DBLTAP_ENABLE_NO_PROXIMITY:
        if( proximity_state_actual != COVER_OPEN ) {
            mce_log(LL_DEVEL, "[doubletap] ignored due to proximity");
            goto EXIT;
        }
        break;
    }

    switch( system_state ) {
    case MCE_STATE_USER:
    case MCE_STATE_ACTDEAD:
      break;
    default:
      mce_log(LL_DEVEL, "[doubletap] ignored due to system state");
        goto EXIT;
    }

    /* Note: In case we happen to be in middle of display state transition
     *       the double tap blocking must use the next stable display state
     *       rather than the current - potentially transitional - state.
     */
    switch( display_state_next )
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
        mce_log(LL_DEVEL, "[doubletap] ignored due to display state");
        goto EXIT;
    }

    switch( doubletap_gesture_policy ) {
    case DBLTAP_ACTION_UNBLANK:  // unblank
    case DBLTAP_ACTION_TKUNLOCK: // unblank + unlock
        mce_log(LL_DEBUG, "double tap -> display on");
        /* Double tap event that is about to be used for unblanking
         * the display counts as non-syntetized user activity */
        execute_datapipe_output_triggers(&user_activity_pipe,
                                         ev, USE_INDATA);

        /* Turn the display on */
        execute_datapipe(&display_state_req_pipe,
                       GINT_TO_POINTER(MCE_DISPLAY_ON),
                       USE_INDATA, CACHE_INDATA);

        /* Optionally remove tklock */
        if( doubletap_gesture_policy == DBLTAP_ACTION_TKUNLOCK ) {
            execute_datapipe(&tk_lock_pipe,
                             GINT_TO_POINTER(LOCK_OFF),
                             USE_INDATA, CACHE_INDATA);
        }
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
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_DOWN:
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

/** Lid cover state (N770); assume open
 *
 * Note that this is used also for hammerhead magnetic lid sensor.
 */
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

    switch( lid_cover_state ) {
    case COVER_CLOSED:
        /* lock ui + blank display */
        execute_datapipe(&tk_lock_pipe,
                         GINT_TO_POINTER(LOCK_ON),
                         USE_INDATA, CACHE_INDATA);
        execute_datapipe(&display_state_req_pipe,
                         GINT_TO_POINTER(MCE_DISPLAY_OFF),
                         USE_INDATA, CACHE_INDATA);
        break;

    case COVER_OPEN:
        /* unblank display */
        execute_datapipe(&display_state_req_pipe,
                         GINT_TO_POINTER(MCE_DISPLAY_ON),
                         USE_INDATA, CACHE_INDATA);
        break;

    default:
        break;
    }

    /* TODO: On devices that have means to detect physically covered
     *       display, it might be desirable to also power off proximity
     *       sensor and notification led while lid is on */

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
    static int64_t last_time = 0;

    const struct input_event *ev = data;

    if( !ev )
        goto EXIT;

    /* Touch events relevant unly when handling notification & linger */
    if( !(exception_state & (UIEXC_NOTIF | UIEXC_LINGER)) )
        goto EXIT;

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

    int64_t now = tklock_monotick_get();

    if( last_time + 200 > now )
        goto EXIT;

    last_time = now;

    mce_log(LL_DEBUG, "type: %s, code: %s, value: %d",
            evdev_get_event_type_name(ev->type),
            evdev_get_event_code_name(ev->type, ev->code),
            ev->value);

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
        tklock_notif_extend_by_renew();
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
        .datapipe = &device_resumed_pipe,
        .output_cb = tklock_datapipe_device_resumed_cb,
    },
    {
        .datapipe = &lipstick_available_pipe,
        .output_cb = tklock_datapipe_lipstick_available_cb,
    },
    {
        .datapipe = &update_mode_pipe,
        .output_cb = tklock_datapipe_update_mode_cb,
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
        .datapipe = &display_state_next_pipe,
        .output_cb = tklock_datapipe_display_state_next_cb,
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
        .datapipe = &music_playback_pipe,
        .output_cb = tklock_datapipe_music_playback_cb,
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

static void tklock_autolock_reschedule(void)
{
    /* Do we have a timer to re-evaluate? */
    if( !tklock_autolock_id )
        goto EXIT;

    /* Clear old timer */
    g_source_remove(tklock_autolock_id), tklock_autolock_id = 0;

    int64_t now = tklock_monotick_get();

    if( now >= tklock_autolock_tick ) {
        mce_log(LL_DEBUG, "autolock time passed while suspended; lock now");
        /* Trigger time passed while suspended */
        tklock_autolock_tick = MAX_TICK;
        tklock_ui_set(true);
    }
    else {
        /* Re-calculate wakeup time */
        mce_log(LL_DEBUG, "adjusting autolock time after resume");
        int delay = (int)(tklock_autolock_tick - now);
        tklock_autolock_id = g_timeout_add(delay, tklock_autolock_cb, 0);
    }

EXIT:
    return;
}

static void tklock_autolock_schedule(int delay)
{
    if( tklock_autolock_id )
        g_source_remove(tklock_autolock_id);

    tklock_autolock_id = g_timeout_add(delay, tklock_autolock_cb, 0);
    tklock_autolock_tick = tklock_monotick_get() + delay;
    mce_log(LL_DEBUG, "autolock timer started");
}

/** React to display state transitios that are about to be made
 */
static void tklock_autolock_pre_transition_actions(void)
{
    mce_log(LL_DEBUG, "prev=%d, next=%d", display_state, display_state_next);

    /* Check if we are about to start off -> on/dim transition */

    switch( display_state ) {
    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
        break;

    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        // was already on -> dontcare
        goto EXIT;
    }

    switch( display_state_next ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        break;

    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
        // going to off -> dontcare
        goto EXIT;
    }

    /* Apply ui autolock before the transition begins */
    if( tklock_autolock_exceeded() ) {
        tklock_ui_set(true);
    }

    /* Cancel autolock timeout */
    tklock_autolock_cancel();

EXIT:
    return;
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
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_POWER_UP:
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

    bool is_covered  = (proximity_state_effective == COVER_CLOSED);
    bool was_covered = (prev_proximity_state      == COVER_CLOSED);

    switch( prev_display_state ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_DOWN:
        was_off = true;
        break;

    default:
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_POWER_UP:
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

    prev_proximity_state = proximity_state_effective;
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
    bool            devicelock;
    bool            insync;
    bool            restore;
    bool            was_called;
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
    .devicelock  = false,
    .insync      = true,
    .restore     = true,
    .was_called  = false,
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

    bool        proximity_blank = false;

    /* Make sure "proximityblanking" state gets cleared if display
     * changes to non-off state. */
    if( display_prev != display_state ) {
        switch( display_state ) {
        case MCE_DISPLAY_OFF:
        case MCE_DISPLAY_POWER_DOWN:
            break;

        default:
        case MCE_DISPLAY_ON:
        case MCE_DISPLAY_DIM:
        case MCE_DISPLAY_UNDEF:
        case MCE_DISPLAY_LPM_OFF:
        case MCE_DISPLAY_LPM_ON:
        case MCE_DISPLAY_POWER_UP:
            execute_datapipe(&proximity_blank_pipe,
                             GINT_TO_POINTER(false),
                             USE_INDATA, CACHE_INDATA);
            break;
        }
    }

    if( !active ) {
        mce_log(LL_DEBUG, "UIEXC_NONE");
        goto EXIT;
    }

    /* If we started from tklocked state ... */
    if( exdata.tklock  ) {
        switch( call_state ) {
        case CALL_STATE_RINGING:
            /* When UI side is dealing with incoming call, it removes tklock
             * so that peeking shows home screen instead of lock screen. And
             * since we do not want that to cancel the state restoration after
             * the call ends -> we need to ignore it.
             */
            if( !exdata.was_called ) {
                mce_log(LL_NOTICE, "starting to ignore tklock removal");
                exdata.was_called = true;
            }
            break;

        case CALL_STATE_NONE:
            /* Start paying attention to tklock changes again if it gets
             * restored after all calls have ended */
            if( exdata.was_called && tklock_datapipe_have_tklock_submode() ) {
                mce_log(LL_NOTICE, "stopping to ignore tklock removal");
                exdata.was_called = false;
            }
            break;

        default:
            break;
        }
    }

    /* If tklock state has changed from initial state ... */
    if( exdata.tklock != tklock_datapipe_have_tklock_submode() ) {
        /* Disable state restore, unless we are handling incoming call */
        if( exdata.restore && !exdata.was_called ) {
            mce_log(LL_NOTICE, "DISABLING STATE RESTORE; tklock out of sync");
            exdata.restore = false;
        }
    }

    if( exdata.restore && exdata.devicelock != device_lock_active ) {
        mce_log(LL_NOTICE, "DISABLING STATE RESTORE; devicelock out of sync");
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
        else if( proximity_state_effective == COVER_CLOSED ) {
            mce_log(LL_DEBUG, "proximity=COVERED; blank");
            /* blanking due to proximity sensor */
            blank = proximity_blank = true;
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
            /* expose blanking due to proximity via datapipe */
            if( proximity_blank ) {
                mce_log(LL_DEVEL, "display proximity blank");
                execute_datapipe(&proximity_blank_pipe,
                                 GINT_TO_POINTER(true),
                                 USE_INDATA, CACHE_INDATA);
            }
            else {
                mce_log(LL_DEBUG, "display blank");
            }
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
        else if( proximity_state_effective == COVER_CLOSED ) {
            mce_log(LL_NOTICE, "NOT UNBLANKING; proximity covered");
        }
        else if( display_state != MCE_DISPLAY_ON ) {
            mce_log(LL_DEBUG, "display unblank");
            execute_datapipe(&display_state_req_pipe,
                             GINT_TO_POINTER(MCE_DISPLAY_ON),
                             USE_INDATA, CACHE_INDATA);
        }
    }

    /* Make sure "proximityblanking" state gets cleared if display
     * state is no longer controlled by this state machine. */
    if( !exdata.insync ) {
        execute_datapipe(&proximity_blank_pipe,
                         GINT_TO_POINTER(false),
                         USE_INDATA, CACHE_INDATA);
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
    exdata.devicelock  = false;
    exdata.insync      = true;
    exdata.restore     = true;
    exdata.was_called  = false;
    exdata.linger_tick = MIN_TICK;
    exdata.linger_id   = 0;
    exdata.notif_tick  = MIN_TICK,
    exdata.notif_id    = 0;
}

static void tklock_uiexcept_finish(void)
{
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
    if( exx.display != MCE_DISPLAY_ON ) {
        /* If the display was not clearly ON when exception started,
         * turn it OFF after exceptions are over. */
        execute_datapipe(&display_state_req_pipe,
                         GINT_TO_POINTER(MCE_DISPLAY_OFF),
                         USE_INDATA, CACHE_INDATA);
    }
    else if( proximity_state_actual == COVER_OPEN ) {
        /* Unblank only if proximity sensor is not covered when
         * the linger time has passed.
         *
         * Note: Because linger times are relatively short,
         * we use raw sensor data here instead of the filtered
         * proximity_state_effective that is normally used
         * with unblanking policies. */
        execute_datapipe(&display_state_req_pipe,
                         GINT_TO_POINTER(MCE_DISPLAY_ON),
                         USE_INDATA, CACHE_INDATA);
    }
EXIT:
    return;
}

static gboolean tklock_uiexcept_linger_cb(gpointer aptr)
{
    (void) aptr;

    if( !exdata.linger_id )
        goto EXIT;

    /* mark timer inactive */
    exdata.linger_id = 0;

    /* Ignore unless linger bit and only linger bit is set */
    if( exdata.mask != UIEXC_LINGER ) {
        mce_log(LL_WARN, "spurious linger timeout");
        goto EXIT;
    }

    mce_log(LL_DEBUG, "linger timeout");
    tklock_uiexcept_finish();

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

    if( exdata.linger_id )
        g_source_remove(exdata.linger_id), exdata.linger_id = 0;

    if( !exdata.mask ) {
        int delay = (int)(exdata.linger_tick - now);
        if( delay > 0 ) {
            mce_log(LL_DEBUG, "finish after %d ms linger", delay);
            exdata.mask |= UIEXC_LINGER;
            exdata.linger_id = g_timeout_add(delay, tklock_uiexcept_linger_cb, 0);
        }
        else {
            mce_log(LL_DEBUG, "finish without linger");
            tklock_uiexcept_finish();
        }
    }

    tklock_uiexcept_sync_to_datapipe();

EXIT:
    return;
}

static void tklock_uiexcept_begin(uiexctype_t type, int64_t linger)
{
    if( !exdata.mask ) {
        /* reset existing stats */
        tklock_uiexcept_cancel();

        /* save display, tklock and device lock states */
        exdata.display    = display_state;
        exdata.tklock     = tklock_datapipe_have_tklock_submode();
        exdata.devicelock = device_lock_active;

        /* initially insync, restore state at end */
        exdata.insync      = true;
        exdata.restore     = true;
    }

    exdata.mask &= ~UIEXC_LINGER;
    exdata.mask |= type;

    int64_t now = tklock_monotick_get();

    linger += now;

    if( exdata.linger_tick < linger )
        exdata.linger_tick = linger;

    if( exdata.linger_id )
        g_source_remove(exdata.linger_id), exdata.linger_id = 0;

    tklock_uiexcept_sync_to_datapipe();
}

/* ========================================================================= *
 * LOW POWER MODE UI STATE MACHINE
 * ========================================================================= */

/** Bitmap of automatic lpm triggering modes */
static gint tklock_lpmui_triggering = LPMUI_TRIGGERING_FROM_POCKET;

/** GConf notifier id for tklock_lpmui_triggering */
static guint tklock_lpmui_triggering_cb_id = 0;

/* Proximity change time limits for low power mode triggering */
enum
{
    /** Minimum time [ms] the proximity needs to be in stable state */
    LPMUI_LIM_STABLE = 3000,

    /** Maximum time [ms] in between proximity changes */
    LPMUI_LIM_CHANGE = 1500,
};

/** Set lpm ui state
 *
 * Broadcast changes over D-Bus
 *
 * @param enable true if lpm ui should be enabled, false otherwise
 */
static void tklock_lpmui_set_state(bool enable)
{
    /* Initialize cached state to a value that will not match true/false
     * input to ensure that a notification is always sent on mce startup.
     */
    static int enabled = -1;

    if( !enable ) {
        // nop
    }
    else if( enable && system_state != MCE_STATE_USER ) {
        mce_log(LL_DEBUG, "deny lpm; not in user mode");
        enable = false;
    }
    else if( !lipstick_available ) {
        mce_log(LL_DEBUG, "deny lpm; lipstick not running");
        enable = false;
    }

    if( enabled == enable )
        goto EXIT;

    enabled = enable;

    if( enabled ) {
        /* make sure ui is locked before we enter LPM display modes */
        execute_datapipe(&tk_lock_pipe,
                         GINT_TO_POINTER(LOCK_ON),
                         USE_INDATA, CACHE_INDATA);

        /* Tell lipstick that we are in lpm mode */
        tklock_ui_enable_lpm();
    }
    else {
        /* Tell lipstick that we are out of lpm mode */
        tklock_ui_disable_lpm();
    }

    /* Broadcast a signal too */
    tklock_ui_send_lpm_signal(enabled);
EXIT:
    return;
}

/** Reset LPM UI proximity sensor history
 *
 * Triggering LPM UI is not possible until stable state is
 * reached again.
 */
static void tklock_lpmui_reset_history(void)
{
    int64_t now = tklock_monotick_get();

    for( size_t i = 0; i < numof(tklock_lpmui_hist); ++i ) {
        tklock_lpmui_hist[i].tick  = now;
        tklock_lpmui_hist[i].state = proximity_state_actual;
    }
}

/** Update LPM UI proximity sensor history
 *
 * @param state proximity sensor state (raw, undelayed)
 */
static void tklock_lpmui_update_history(cover_state_t state)
{
    if( state == tklock_lpmui_hist[0].state )
        goto EXIT;

    memmove(tklock_lpmui_hist+1, tklock_lpmui_hist+0,
            sizeof tklock_lpmui_hist - sizeof *tklock_lpmui_hist);

    tklock_lpmui_hist[0].tick  = tklock_monotick_get();
    tklock_lpmui_hist[0].state = state;

EXIT:
    return;
}

/** Check if LPM UI proximity sensor history equals "out of pocket" state
 *
 * Proximity was covered for LPMUI_LIM_STABLE ms, then uncovered less
 * than LPMUI_LIM_CHANGE ms ago.
 *
 * @return true if conditions met, false otherwise
 */
static bool tklock_lpmui_probe_from_pocket(void)
{
    bool    res = false;

    if( !(tklock_lpmui_triggering & LPMUI_TRIGGERING_FROM_POCKET) )
        goto EXIT;

    int64_t now = tklock_monotick_get();
    int64_t t;

    /* Uncovered < LPMUI_LIM_CHANGE ms ago ? */
    if( tklock_lpmui_hist[0].state != COVER_OPEN )
        goto EXIT;
    t = now - tklock_lpmui_hist[0].tick;
    if( t > LPMUI_LIM_CHANGE )
        goto EXIT;

    /* After being covered for LPMUI_LIM_STABLE ms ? */
    if( tklock_lpmui_hist[1].state != COVER_CLOSED )
        goto EXIT;
    t = tklock_lpmui_hist[0].tick - tklock_lpmui_hist[1].tick;
    if( t < LPMUI_LIM_STABLE )
        goto EXIT;

    res = true;
EXIT:

    return res;
}

/** Check if LPM UI proximity sensor history equals "covered on table" state
 *
 * Proximity was uncovered for LPMUI_LIM_STABLE ms, them covered and
 * uncovered within LPMUI_LIM_CHANGE ms, possibly several times.
 *
 * @return true if conditions met, false otherwise
 */
static bool tklock_lpmui_probe_on_table(void)
{
    bool    res  = false;

    if( !(tklock_lpmui_triggering & LPMUI_TRIGGERING_HOVER_OVER) )
        goto EXIT;

    int64_t t = tklock_monotick_get();

    for( size_t i = 0; ; i += 2 ) {

        /* Need to check 3 slots: OPEN, CLOSED, OPEN */
        if( i + 3 > numof(tklock_lpmui_hist) )
            goto EXIT;

        /* Covered and uncovered within LPMUI_LIM_CHANGE ms? */
        if( tklock_lpmui_hist[i+0].state != COVER_OPEN )
            goto EXIT;
        if( t - tklock_lpmui_hist[i+0].tick > LPMUI_LIM_CHANGE )
            goto EXIT;

        if( tklock_lpmui_hist[i+1].state != COVER_CLOSED )
            goto EXIT;
        if( t - tklock_lpmui_hist[i+1].tick > LPMUI_LIM_CHANGE )
            goto EXIT;

        /* After being uncovered longer than LPMUI_LIM_STABLE ms? */
        if( tklock_lpmui_hist[i+2].state != COVER_OPEN )
            goto EXIT;
        t = tklock_lpmui_hist[i+1].tick - tklock_lpmui_hist[i+2].tick;
        if( t > LPMUI_LIM_STABLE )
            break;

        t = tklock_lpmui_hist[i+1].tick;
    }

    res = true;

EXIT:

    return res;
}
/** Check if proximity sensor history should trigger LPM UI mode
 *
 * @return true if LPM UI can be enabled, false otherwise
 */
static bool tklock_lpmui_probe(void)
{
    bool glance = false;

    if( tklock_lpmui_probe_from_pocket() ) {
        mce_log(LL_DEBUG, "from pocket");
        glance = true;
    }
    else if( tklock_lpmui_probe_on_table() ) {
        mce_log(LL_DEBUG, "hovering over");
        glance = true;
    }
    else {
        mce_log(LL_DEBUG, "proximity noise");
    }

    return glance;
}

/** Check if LPM UI mode should be enabled
 */
static void tklock_lpmui_rethink(void)
{
    /* prerequisites: in user state, lipstick running and display off */
    if( system_state != MCE_STATE_USER )
        goto EXIT;

    if( !lipstick_available )
        goto EXIT;

    if( display_state != MCE_DISPLAY_OFF )
        goto EXIT;

    /* but not during calls, alarms, etc */
    if( exception_state != UIEXC_NONE )
        goto EXIT;

    /* or when proximity is covered */
    if( proximity_state_effective != COVER_OPEN )
        goto EXIT;

    /* Switch to lpm mode if the proximity sensor history matches activity
     * we expect to see when "the device is taken from pocket" etc */
    if( tklock_lpmui_probe() ) {
        mce_log(LL_DEBUG, "switching to LPM UI");

        /* Note: Display plugin handles MCE_DISPLAY_LPM_ON request as
         *       MCE_DISPLAY_OFF unless lpm mode is both supported
         *       and enabled. */
        execute_datapipe(&display_state_req_pipe,
                         GINT_TO_POINTER(MCE_DISPLAY_LPM_ON),
                         USE_INDATA, CACHE_INDATA);
    }

EXIT:

    return;
}

/** LPM UI related actions that should be done before display state transition
 */
static void tklock_lpmui_pre_transition_actions(void)
{
    mce_log(LL_DEBUG, "prev=%d, next=%d", display_state, display_state_next);

    switch( display_state_next ) {
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_LPM_OFF:
        /* We are about to make transition to LPM state */
        tklock_lpmui_set_state(true);
        break;

    case MCE_DISPLAY_OFF:
        switch( display_state ) {
        case MCE_DISPLAY_ON:
        case MCE_DISPLAY_DIM:
            /* We are about to power off from ON/DIM */

            /* If display is turned off via pull from top gesture
             * it is highly likely that the proximity sensor gets
             * covered -> to avoid immediate bounce back to lpm
             * state we need to reset proximity state history */
            tklock_lpmui_reset_history();
            break;
        default:
            break;
        }
        break;

    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        /* We are about to make transition to ON/DIM state */
        tklock_lpmui_set_state(false);
        break;

    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
        // dontcare
        break;
    }
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
     *  proximity_state_effective <-- tklock_datapipe_proximity_sensor_cb()
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
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
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

    /* enable volume keys if music playing */
    if( music_playback )
        enable_kp = true;

    /* - - - - - - - - - - - - - - - - - - - *
     * touchscreen interrupts
     * - - - - - - - - - - - - - - - - - - - */

    /* display must be on/dim */
    switch( display_state ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        break;

    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
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
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
        enable_dt = false;
        break;
    }

    /* doubletap gesture policy must not be 0/disabled */
    if( doubletap_gesture_policy == DBLTAP_ACTION_DISABLED ) {
        enable_dt = false;
    }

#if 0 /* FIXME: check if proximity via sensord works better
       *        with up to date nemomobile image */
    /* proximity sensor must not be covered */
    if( proximity_state_effective != COVER_OPEN ) {
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

    /* - - - - - - - - - - - - - - - - - - - *
     * in case emitting of touch events can't
     * be controlled, we use evdev input grab
     * to block ui from seeing them while the
     * display is off
     * - - - - - - - - - - - - - - - - - - - */

    bool grab_ts = datapipe_get_gint(touch_grab_wanted_pipe);

    switch( display_state ) {
    default:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_POWER_DOWN:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_LPM_OFF:
        // want grab
        grab_ts = true;
        break;

    case MCE_DISPLAY_POWER_UP:
        // keep grab state
        break;

    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        // release grab
        grab_ts = false;
        break;
    }

    /* Grabbing touch input is always permitted, but ungrabbing
     * only when proximity sensor is not covered */
    if( grab_ts || proximity_state_effective == COVER_OPEN ) {
        execute_datapipe(&touch_grab_wanted_pipe,
                         GINT_TO_POINTER(grab_ts),
                         USE_INDATA, CACHE_INDATA);
    }

    /* - - - - - - - - - - - - - - - - - - - *
     * in case emitting of keypad events can't
     * be controlled, we use evdev input grab
     * to block ui from seeing them while the
     * display is off
     * - - - - - - - - - - - - - - - - - - - */

    bool grab_kp = !enable_kp;

    execute_datapipe(&keypad_grab_wanted_pipe,
                     GINT_TO_POINTER(grab_kp),
                     USE_INDATA, CACHE_INDATA);

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

static void tklock_gconf_sanitize_doubletap_gesture_policy(void)
{
    switch( doubletap_gesture_policy ) {
    case DBLTAP_ACTION_DISABLED:
    case DBLTAP_ACTION_UNBLANK:
    case DBLTAP_ACTION_TKUNLOCK:
        break;

    default:
        mce_log(LL_WARN, "Double tap gesture has invalid policy: %d; "
                "using default", doubletap_gesture_policy);
        doubletap_gesture_policy = DBLTAP_ACTION_DEFAULT;
        break;
    }
}

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
        tklock_gconf_sanitize_doubletap_gesture_policy();
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
    else if( id == doubletap_enable_mode_cb_id ) {
        gint old = doubletap_enable_mode;
        doubletap_enable_mode = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "doubletap_enable_mode: %d -> %d",
                old, doubletap_enable_mode);
    }
    else if( id == tklock_lpmui_triggering_cb_id ) {
        gint old = tklock_lpmui_triggering;
        tklock_lpmui_triggering = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "tklock_lpmui_triggering: %d -> %d",
                old, tklock_lpmui_triggering);
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
    mce_gconf_notifier_add(MCE_GCONF_LOCK_PATH,
                           MCE_GCONF_TK_AUTOLOCK_ENABLED_PATH,
                           tklock_gconf_cb,
                           &tk_autolock_enabled_cb_id);

    mce_gconf_get_bool(MCE_GCONF_TK_AUTOLOCK_ENABLED_PATH,
                       &tk_autolock_enabled);

    /* Touchscreen/keypad double-tap gesture policy */
    mce_gconf_notifier_add(MCE_GCONF_LOCK_PATH,
                           MCE_GCONF_TK_DOUBLE_TAP_GESTURE_PATH,
                           tklock_gconf_cb,
                           &doubletap_gesture_policy_cb_id);

    mce_gconf_get_int(MCE_GCONF_TK_DOUBLE_TAP_GESTURE_PATH,
                      &doubletap_gesture_policy);
    tklock_gconf_sanitize_doubletap_gesture_policy();

    /** Touchscreen double tap gesture mode */
    mce_gconf_notifier_add(MCE_GCONF_DOUBLETAP_PATH,
                           MCE_GCONF_DOUBLETAP_MODE,
                           tklock_gconf_cb,
                           &doubletap_enable_mode_cb_id);

    mce_gconf_get_int(MCE_GCONF_DOUBLETAP_MODE, &doubletap_enable_mode);

    /* Bitmap of automatic lpm triggering modes */
    mce_gconf_notifier_add(MCE_GCONF_LOCK_PATH,
                           MCE_GCONF_LPMUI_TRIGGERING,
                           tklock_gconf_cb,
                           &tklock_lpmui_triggering_cb_id);

    mce_gconf_get_int(MCE_GCONF_LPMUI_TRIGGERING, &tklock_lpmui_triggering);
}

/** Remove gconf change notifiers
 */
static void tklock_gconf_quit(void)
{
    mce_gconf_notifier_remove(doubletap_gesture_policy_cb_id),
        doubletap_gesture_policy_cb_id = 0;

    mce_gconf_notifier_remove(tk_autolock_enabled_cb_id),
        tk_autolock_enabled_cb_id = 0;

    mce_gconf_notifier_remove(tklock_blank_disable_id),
        tklock_blank_disable_id = 0;

    mce_gconf_notifier_remove(doubletap_enable_mode_cb_id),
        doubletap_enable_mode_cb_id = 0;

    mce_gconf_notifier_remove(tklock_lpmui_triggering_cb_id),
        tklock_lpmui_triggering_cb_id = 0;
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
    bool requested = enable;

    if( enable ) {
        if( system_state != MCE_STATE_USER ) {
            mce_log(LL_INFO, "deny tklock; not in user mode");
            enable = false;
        }
        else if( !lipstick_available ) {
            mce_log(LL_INFO, "deny tklock; lipstick not running");
            enable = false;
        }
        else if( update_mode ) {
            mce_log(LL_INFO, "deny tklock; os update in progress");
            enable = false;
        }
    }

    if( tklock_ui_sent != enable || requested != enable ) {
        mce_log(LL_DEVEL, "tklock state = %s", enable ? "locked" : "unlocked");

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

/** Broadcast LPM UI state over D-Bus
 *
 * @param enabled
 */
static void tklock_ui_send_lpm_signal(bool enabled)
{
    const char *sig = MCE_LPM_UI_MODE_SIG;
    const char *arg = enabled ? "enabled" : "disabled";
    mce_log(LL_DEVEL, "sending dbus signal: %s %s", sig, arg);
    dbus_send(0, MCE_SIGNAL_PATH, MCE_SIGNAL_IF,  sig, 0,
              DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
}

/** Tell lipstick that lpm ui mode is enabled
 */
static void tklock_ui_enable_lpm(void)
{
    /* Use the N9 legacy D-Bus method call */

    const char   *cb_service   = MCE_SERVICE;
    const char   *cb_path      = MCE_REQUEST_PATH;
    const char   *cb_interface = MCE_REQUEST_IF;
    const char   *cb_method    = MCE_TKLOCK_CB_REQ;
    dbus_bool_t   flicker_key  = has_flicker_key;
    dbus_uint32_t mode         = TKLOCK_ENABLE_LPM_UI;
    dbus_bool_t   silent       = TRUE;

    mce_log(LL_DEBUG, "sending tklock ui lpm enable");

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

/** Tell lipstick that lpm ui mode is disabled
 */
static void tklock_ui_disable_lpm(void)
{
    // FIXME: we do not have method call for cancelling lpm state
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

    mce_log(LL_DEVEL, "Received tklock mode get request from %s",
            mce_dbus_get_message_sender_ident(msg));

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

    if( !dbus_message_get_args(msg, &error,
                               DBUS_TYPE_STRING, &mode,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to get argument from %s.%s: %s",
                MCE_REQUEST_IF, MCE_TKLOCK_MODE_CHANGE_REQ,
                error.message);
        goto EXIT;
    }

    mce_log(LL_DEVEL, "Received tklock mode change request '%s' from %s",
            mode, mce_dbus_get_message_sender_ident(msg));

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

    mce_log(LL_DEVEL, "tklock callback value: %d, from %s", result,
            mce_dbus_get_message_sender_ident(msg));

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

/** D-Bus callback for notification begin request
 *
 * @param msg D-Bus message with the notification name and duration
 *
 * @return TRUE
 */
static gboolean tklock_dbus_notification_beg_cb(DBusMessage *const msg)
{
    DBusError     err  = DBUS_ERROR_INIT;
    const char   *name = 0;
    dbus_int32_t  dur  = 0;
    dbus_int32_t  add  = 0;
    const char   *from = dbus_message_get_sender(msg);

    if( !from )
        goto EXIT;

    if( !dbus_message_get_args(msg, &err,
                              DBUS_TYPE_STRING,&name,
                              DBUS_TYPE_INT32, &dur,
                              DBUS_TYPE_INT32, &add,
                              DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to get arguments: %s: %s",
                err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEVEL, "notification begin from %s",
            mce_dbus_get_message_sender_ident(msg));

    mce_tklock_begin_notification(from, name, dur, add);

EXIT:
    /* Send dummy reply if requested */
    if( !dbus_message_get_no_reply(msg) )
        dbus_send_message(dbus_new_method_reply(msg));

    dbus_error_free(&err);
    return TRUE;
}

/** D-Bus callback for notification end request
 *
 * @param msg D-Bus message with the notification name and duration
 *
 * @return TRUE
 */
static gboolean tklock_dbus_notification_end_cb(DBusMessage *const msg)
{
    DBusError     err  = DBUS_ERROR_INIT;
    const char   *name = 0;
    dbus_int32_t  dur  = 0;
    const char   *from = dbus_message_get_sender(msg);

    if( !from )
        goto EXIT;

    if( !dbus_message_get_args(msg, &err,
                              DBUS_TYPE_STRING,&name,
                              DBUS_TYPE_INT32, &dur,
                              DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to get arguments: %s: %s",
                err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEVEL, "notification end from %s",
            mce_dbus_get_message_sender_ident(msg));

    mce_tklock_end_notification(from, name, dur);

EXIT:
    /* Send dummy reply if requested */
    if( !dbus_message_get_no_reply(msg) )
        dbus_send_message(dbus_new_method_reply(msg));

    dbus_error_free(&err);
    return TRUE;
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
static mce_dbus_handler_t tklock_dbus_handlers[] =
{
    /* signals - inbound */
    {
        .interface = "org.nemomobile.lipstick.devicelock",
        .name      = "stateChanged",
        .rules     = "path='/devicelock'",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = tklock_dbus_device_lock_changed_cb,
    },
    /* signals - outbound (for Introspect purposes only) */
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_TKLOCK_MODE_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
                        "    <arg name=\"tklock_mode\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_LPM_UI_MODE_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
                        "    <arg name=\"lpm_mode\" type=\"s\"/>\n"
    },
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_TKLOCK_MODE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_mode_get_req_cb,
        .args      =
            "    <arg direction=\"out\" name=\"mode_name\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_TKLOCK_MODE_CHANGE_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_mode_change_req_cb,
        .args      =
            "    <arg direction=\"in\" name=\"mode_name\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_TKLOCK_CB_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_systemui_callback_cb,
        .args      =
            "    <arg direction=\"in\" name=\"lock_status\" type=\"i\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = "notification_begin_req",
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_notification_beg_cb,
        .args      =
            "    <arg direction=\"in\" name=\"notification_name\" type=\"s\"/>\n"
            "    <arg direction=\"in\" name=\"duration_time\" type=\"i\"/>\n"
            "    <arg direction=\"in\" name=\"activity_extend_time\" type=\"i\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = "notification_end_req",
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_notification_end_cb,
        .args      =
            "    <arg direction=\"in\" name=\"notification_name\" type=\"s\"/>\n"
            "    <arg direction=\"in\" name=\"linger_time\" type=\"i\"/>\n"
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
    mce_dbus_handler_register_array(tklock_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mce_tklock_quit_dbus(void)
{
    mce_dbus_handler_unregister_array(tklock_dbus_handlers);
}

/* ========================================================================= *
 * NOTIFICATION_SLOTS
 * ========================================================================= */

static void
tklock_notif_slot_init(tklock_notif_slot_t *self)
{
    self->ns_owner = 0;
    self->ns_name  = 0;
    self->ns_until = 0;
    self->ns_renew = 0;
}

static void
tklock_notif_slot_free(tklock_notif_slot_t *self)
{
    gchar *owner = tklock_notif_slot_steal_owner(self);

    if( self->ns_name )
        mce_log(LL_DEVEL, "notification '%s' removed", self->ns_name);

    g_free(self->ns_name), self->ns_name = 0;
    self->ns_until = 0;
    self->ns_renew = 0;

    tklock_notif_remove_owner_monitor(owner);
    g_free(owner);
}

static void
tklock_notif_slot_set(tklock_notif_slot_t *self,
                      const char *owner, const char *name,
                      int64_t until, int64_t renew)
{
    tklock_notif_slot_free(self);

    self->ns_owner = owner ? g_strdup(owner) : 0;
    self->ns_name  = g_strdup(name);
    self->ns_until = until;
    self->ns_renew = renew;

    if( self->ns_name )
        mce_log(LL_DEVEL, "notification '%s' added", self->ns_name);

    tklock_notif_add_owner_monitor(owner);
}

static bool
tklock_notif_slot_is_free(const tklock_notif_slot_t *self)
{
    return self->ns_name == 0;
}

static bool
tklock_notif_slot_has_name(const tklock_notif_slot_t *self, const char *name)
{
    return self->ns_name && !strcmp(self->ns_name, name);
}

static bool
tklock_notif_slot_validate(tklock_notif_slot_t *self, int64_t now)
{
    if( now <= self->ns_until )
        return true;

    tklock_notif_slot_free(self);
    return false;
}

static bool
tklock_notif_slot_renew(tklock_notif_slot_t *self, int64_t now)
{
    int64_t tmo = now + self->ns_renew;

    if( tmo <= self->ns_until )
        return false;

    self->ns_until = tmo;
    return true;
}

static inline bool eq(const char *s1, const char *s2)
{
    return (s1 && s2) ? !strcmp(s1, s2) : (s1 == s2);
}

static bool
tklock_notif_slot_has_owner(const tklock_notif_slot_t *self, const char *owner)
{
    return eq(self->ns_owner, owner);
}

static gchar *
tklock_notif_slot_steal_owner(tklock_notif_slot_t *self)
{
    gchar *owner = self->ns_owner;
    self->ns_owner = 0;
    return owner;
}

/* ========================================================================= *
 * NOTIFICATION_API
 * ========================================================================= */

static tklock_notif_state_t tklock_notif_state;

static void
tklock_notif_init(void)
{
    tklock_notif_state.tn_linger = MIN_TICK;
    tklock_notif_state.tn_autostop_id = 0;
    tklock_notif_state.tn_monitor_list = 0;

    for( size_t i = 0; i < TKLOCK_NOTIF_SLOTS; ++i ) {
        tklock_notif_slot_t *slot = tklock_notif_state.tn_slot + i;
        tklock_notif_slot_init(slot);
    }
}

static void
tklock_notif_quit(void)
{
    tklock_notif_cancel_autostop();

    for( size_t i = 0; i < TKLOCK_NOTIF_SLOTS; ++i ) {
        tklock_notif_slot_t *slot = tklock_notif_state.tn_slot + i;
        tklock_notif_slot_free(slot);
    }

    /* Make sure the above loop removed all the monitoring callbacks */
    if( tklock_notif_state.tn_monitor_list )
        mce_log(LL_WARN, "entries left in owner monitor list");
    mce_dbus_owner_monitor_remove_all(&tklock_notif_state.tn_monitor_list);
}

static gboolean tklock_notif_autostop_cb(gpointer aptr)
{
    (void)aptr;

    if( !tklock_notif_state.tn_autostop_id )
        goto EXIT;

    mce_log(LL_DEBUG, "triggered");

    tklock_notif_state.tn_autostop_id = 0;

    tklock_notif_update_state();

EXIT:
    return FALSE;
}

static void
tklock_notif_cancel_autostop(void)
{
    if( tklock_notif_state.tn_autostop_id ) {
        mce_log(LL_DEBUG, "cancelled");
        g_source_remove(tklock_notif_state.tn_autostop_id),
            tklock_notif_state.tn_autostop_id = 0;
    }
}

static void
tklock_notif_schedule_autostop(gint delay)
{
    tklock_notif_cancel_autostop();
    mce_log(LL_DEBUG, "scheduled in %d ms", delay);
    tklock_notif_state.tn_autostop_id =
        g_timeout_add(delay, tklock_notif_autostop_cb, 0);
}

static void
tklock_notif_update_state(void)
{
    int64_t now = tklock_monotick_get();
    int64_t tmo = MAX_TICK;

    for( size_t i = 0; i < TKLOCK_NOTIF_SLOTS; ++i ) {
        tklock_notif_slot_t *slot = tklock_notif_state.tn_slot + i;

        if( tklock_notif_slot_is_free(slot) )
            continue;

        if( !tklock_notif_slot_validate(slot, now) )
            continue;

        if( tmo > slot->ns_until )
            tmo = slot->ns_until;
    }

    tklock_notif_cancel_autostop();

    if( tmo < MAX_TICK ) {
        tklock_notif_schedule_autostop((gint)(tmo - now));

        tklock_uiexcept_begin(UIEXC_NOTIF, 0);
        tklock_uiexcept_rethink();
    }
    else {
        if( (tmo = tklock_notif_state.tn_linger - now) < 0 )
            tmo = 0;
        tklock_uiexcept_end(UIEXC_NOTIF, tmo);
        tklock_uiexcept_rethink();
    }
}

static void
tklock_notif_extend_by_renew(void)
{
    int64_t now = tklock_monotick_get();

    bool changed = false;

    for( size_t i = 0; i < TKLOCK_NOTIF_SLOTS; ++i ) {
        tklock_notif_slot_t *slot = tklock_notif_state.tn_slot + i;

        if( tklock_notif_slot_is_free(slot) )
            continue;

        if( !tklock_notif_slot_validate(slot, now) )
            changed = true;
        else if( tklock_notif_slot_renew(slot, now) )
            changed = true;
    }
    if( changed )
        tklock_notif_update_state();
}

static void
tklock_notif_vacate_slot(const char *owner, const char *name, int64_t linger)
{
    for( size_t i = 0; i < TKLOCK_NOTIF_SLOTS; ++i ) {
        tklock_notif_slot_t *slot = tklock_notif_state.tn_slot + i;

        if( !tklock_notif_slot_has_name(slot, name) )
            continue;

        if( !tklock_notif_slot_has_owner(slot, owner) )
            continue;

        tklock_notif_slot_free(slot);

        int64_t now = tklock_monotick_get();
        int64_t tmo = now + linger;

        if( tklock_notif_state.tn_linger < tmo )
            tklock_notif_state.tn_linger = tmo;

        tklock_notif_update_state();
        goto EXIT;
    }

    mce_log(LL_WARN, "attempt to end non-existing notification");

EXIT:

    return;
}

static void
tklock_notif_reserve_slot(const char *owner, const char *name, int64_t length, int64_t renew)
{
    int64_t now = tklock_monotick_get();
    int64_t tmo = now + length;

    /* first check if slot is already reserved */

    for( size_t i = 0; i < TKLOCK_NOTIF_SLOTS; ++i ) {
        tklock_notif_slot_t *slot = tklock_notif_state.tn_slot + i;

        if( !tklock_notif_slot_has_name(slot, name) )
            continue;

        tklock_notif_slot_set(slot, owner, name, tmo, renew);
        tklock_notif_update_state();
        goto EXIT;
    }

    /* then try to find unused slot */

    for( size_t i = 0; i < TKLOCK_NOTIF_SLOTS; ++i ) {
        tklock_notif_slot_t *slot = tklock_notif_state.tn_slot + i;

        if( !tklock_notif_slot_is_free(slot) )
            continue;

        tklock_notif_slot_set(slot, owner, name, tmo, renew);
        tklock_notif_update_state();
        goto EXIT;
    }

    mce_log(LL_WARN, "too many concurrent notifications");

EXIT:
    return;
}

static void
tklock_notif_vacate_slots_from(const char *owner)
{
    bool changed = false;
    for( size_t i = 0; i < TKLOCK_NOTIF_SLOTS; ++i ) {
        tklock_notif_slot_t *slot = tklock_notif_state.tn_slot + i;

        if( tklock_notif_slot_is_free(slot) )
            continue;

        if( !tklock_notif_slot_has_owner(slot, owner) )
            continue;

        tklock_notif_slot_free(slot);
        changed = true;

    }
    if( changed )
        tklock_notif_update_state();
}

static size_t
tklock_notif_count_slots_from(const char *owner)
{
    size_t count = 0;
    for( size_t i = 0; i < TKLOCK_NOTIF_SLOTS; ++i ) {
        tklock_notif_slot_t *slot = tklock_notif_state.tn_slot + i;
        if( tklock_notif_slot_has_owner(slot, owner) )
            ++count;
    }
    return count;
}

static gboolean
tklock_notif_owner_dropped_cb(DBusMessage *const msg)
{
    const char *name = 0;
    const char *prev = 0;
    const char *curr = 0;
    DBusError   err  = DBUS_ERROR_INIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_STRING, &prev,
                               DBUS_TYPE_STRING, &curr,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "failed to get args: %s: %s",
                err.name, err.message);
        goto EXIT;
    }

    if( !*curr )
        tklock_notif_vacate_slots_from(name);

EXIT:
    dbus_error_free(&err);
    return TRUE;

}

static void
tklock_notif_add_owner_monitor(const char *owner)
{
    if( !owner )
        return;

    if( tklock_notif_count_slots_from(owner) != 1 )
        return;

    /* first slot added */
    mce_log(LL_DEBUG, "adding dbus monitor for: %s" ,owner);
    mce_dbus_owner_monitor_add(owner, tklock_notif_owner_dropped_cb,
                               &tklock_notif_state.tn_monitor_list,
                               TKLOCK_NOTIF_SLOTS);
}

static void
tklock_notif_remove_owner_monitor(const char *owner)
{
    if( !owner )
        return;

    if( tklock_notif_count_slots_from(owner) != 0 )
        return;

    /* last slot removed */
    mce_log(LL_DEBUG, "removing dbus monitor for: %s" ,owner);
    mce_dbus_owner_monitor_remove(owner,
                                  &tklock_notif_state.tn_monitor_list);
}

/** Interface for registering notification state
 *
 * @param name   assumed unique notification identifier
 * @param length minimum length of notification [ms]
 * @param renew  extend length on user input [ms]
 */
static void
mce_tklock_begin_notification(const char *owner, const char *name, int64_t length, int64_t renew)
{
    /* cap length to [1,30] second range */
    if( length > 30000 )
        length = 30000;
    else if( length < 1000 )
        length = 1000;

    /* cap renew to [0,5] second range, negative means use default */
    if( renew > 5000 )
        renew = 5000;
    else if( renew < 0 )
        renew = EXCEPTION_LENGTH_ACTIVITY;

    mce_log(LL_DEBUG, "name: %s, length: %d, renew: %d",
            name, (int)length, (int)renew);
    tklock_notif_reserve_slot(owner, name, length, renew);
}

/** Interface for removing notification state
 *
 * @param name   assumed unique notification identifier
 * @param linger duration to keep display on [ms]
 */
static void
mce_tklock_end_notification(const char *owner, const char *name, int64_t linger)
{
    /* cap linger to [0, 10] second range */
    if( linger > 10000 )
        linger = 10000;
    else if( linger < 0 )
        linger = 0;

    mce_log(LL_DEBUG, "name: %s, linger: %d", name, (int)linger);
    tklock_notif_vacate_slot(owner, name, linger);
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

    /* initialize notification book keeping */
    tklock_notif_init();

    /* initialize proximity history to sane state */
    tklock_lpmui_reset_history();

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
    tklock_datapipe_proximity_uncover_cancel();
    tklock_notif_quit();

    // FIXME: check that final state is sane

    return;
}
