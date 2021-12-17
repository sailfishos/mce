/**
 * @file tklock.c
 * This file implements the touchscreen/keypad lock component
 * of the Mode Control Entity
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Tapio Rantala <ext-tapio.rantala@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Jukka Turunen <ext-jukka.t.turunen@nokia.com>
 * @author Irina Bezruk <ext-irina.bezruk@nokia.com>
 * @author Kalle Jokiniemi <kalle.jokiniemi@jolla.com>
 * @author Mika Laitio <lamikr@pilppa.org>
 * @author Markus Lehtonen <markus.lehtonen@iki.fi>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * @author Vesa Halttunen <vesa.halttunen@jollamobile.com>
 * @author Andrew den Exter <andrew.den.exter@jolla.com>
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

#include "mce-common.h"
#include "mce-log.h"
#include "mce-lib.h"
#include "mce-io.h"
#include "mce-setting.h"
#include "mce-dbus.h"
#include "mce-hbtimer.h"
#include "evdev.h"

#ifdef ENABLE_WAKELOCKS
# include "libwakelock.h"
#endif

#include "modules/doubletap.h"
#include "modules/display.h"

#include "systemui/dbus-names.h"
#include "systemui/tklock-dbus-names.h"

#include <linux/input.h>

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include <glib/gstdio.h>

typedef enum
{
    /** No autorelock triggers */
    AUTORELOCK_NO_TRIGGERS,

    /** Autorelock on keyboard slide closed */
    AUTORELOCK_KBD_SLIDE,

    /** Autorelock on lens cover */
    AUTORELOCK_LENS_COVER,
} autorelock_t;

/** Helper for evaluation number of items in an array */
#define numof(a) (sizeof(a)/sizeof*(a))

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

#define MODULE_NAME "tklock"

/** Max valid time_t value in milliseconds */
#define MAX_TICK (INT_MAX * (int64_t)1000)

/** Min valid time_t value in milliseconds */
#define MIN_TICK  0

/** Maximum number of concurrent notification ui exceptions */
#define TKLOCK_NOTIF_SLOTS 32

/** How long to wait for lid close after low lux [ms] */
#define TKLOCK_LIDFILTER_SET_WAIT_FOR_CLOSE_DELAY 1500

/** How long to wait for low lux after lid close [ms] */
#define TKLOCK_LIDFILTER_SET_WAIT_FOR_DARK_DELAY  1200

/** How long to wait for high lux after lid open [ms] */
#define TKLOCK_LIDFILTER_SET_WAIT_FOR_LIGHT_DELAY 1200

/* ========================================================================= *
 * DATATYPES
 * ========================================================================= */

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

/** Ambient light lux value mapped into enumerated states
 *
 * In case the lid sensor can't be trusted for some reason, data from
 * ambient light sensor heuristics can be used for avoiding incorrect
 * blank/unblank actions.
 *
 * For this purpose the raw data from ambient light sensor is tracked
 * and mapped in to three states:
 *
 * - TKLOCK_LIDLIGHT_NA: The data from als is not applicable for filtering.
 * - TKLOCK_LIDLIGHT_LO: The als indicates darkness.
 * - TKLOCK_LIDLIGHT_HI: The als indicates some amount of light.
 */
typedef enum
{
    /* Light level is not applicable for state evaluation */
    TKLOCK_LIDLIGHT_NA,

    /* Light level equals complete darkness */
    TKLOCK_LIDLIGHT_LO,

    /* Light level equals at least some light */
    TKLOCK_LIDLIGHT_HI,
} tklock_lidlight_t;

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

// datapipe values and triggers

static void     tklock_datapipe_system_state_cb(gconstpointer data);
static void     tklock_datapipe_devicelock_state_cb(gconstpointer data);
static void     tklock_datapipe_devicelock_state_cb2(gpointer aptr);
static void     tklock_datapipe_resume_detected_event_cb(gconstpointer data);
static void     tklock_datapipe_devicelock_service_state_cb(gconstpointer data);
static void     tklock_datapipe_lipstick_service_state_cb(gconstpointer data);
static void     tklock_datapipe_osupdate_running_cb(gconstpointer data);
static void     tklock_datapipe_shutting_down_cb(gconstpointer data);
static void     tklock_datapipe_display_state_curr_cb(gconstpointer data);
static void     tklock_datapipe_display_state_next_cb(gconstpointer data);
static void     tklock_datapipe_proximity_eval_led(void);
static void     tklock_datapipe_proximity_update(void);
static gboolean tklock_datapipe_proximity_uncover_cb(gpointer data);
static void     tklock_datapipe_proximity_uncover_cancel(void);
static void     tklock_datapipe_proximity_uncover_schedule(void);
static void     tklock_datapipe_proximity_sensor_actual_cb(gconstpointer data);
static void     tklock_datapipe_call_state_cb(gconstpointer data);
static void     tklock_datapipe_music_playback_ongoing_cb(gconstpointer data);
static void     tklock_datapipe_alarm_ui_state_cb(gconstpointer data);
static void     tklock_datapipe_charger_state_cb(gconstpointer data);
static void     tklock_datapipe_battery_status_cb(gconstpointer data);
static void     tklock_datapipe_usb_cable_state_cb(gconstpointer data);
static void     tklock_datapipe_jack_sense_state_cb(gconstpointer data);
static void     tklock_datapipe_camera_button_state_cb(gconstpointer const data);
static void     tklock_datapipe_keypress_event_cb(gconstpointer const data);
static void     tklock_datapipe_uiexception_type_cb(gconstpointer data);
static void     tklock_datapipe_audio_route_cb(gconstpointer data);
static void     tklock_datapipe_tklock_request_cb(gconstpointer data);
static void     tklock_datapipe_interaction_expected_cb(gconstpointer data);
static gpointer tklock_datapipe_submode_filter_cb(gpointer data);
static void     tklock_datapipe_submode_cb(gconstpointer data);
static void     tklock_datapipe_lockkey_state_cb(gconstpointer const data);
static void     tklock_datapipe_heartbeat_event_cb(gconstpointer data);
static void     tklock_datapipe_keyboard_slide_input_state_cb(gconstpointer const data);
static void     tklock_datapipe_keyboard_slide_output_state_cb(gconstpointer const data);
static void     tklock_datapipe_keyboard_available_state_cb(gconstpointer const data);
static void     tklock_datapipe_mouse_available_state_cb(gconstpointer const data);
static void     tklock_datapipe_light_sensor_poll_request_cb(gconstpointer const data);
static void     tklock_datapipe_topmost_window_pid_cb(gconstpointer data);
static void     tklock_datapipe_light_sensor_actual_cb(gconstpointer data);
static void     tklock_datapipe_lid_sensor_is_working_cb(gconstpointer data);
static void     tklock_datapipe_lid_sensor_actual_cb(gconstpointer data);
static void     tklock_datapipe_lid_sensor_filtered_cb(gconstpointer data);
static void     tklock_datapipe_lens_cover_state_cb(gconstpointer data);
static bool     tklock_touch_activity_event_p(const struct input_event *ev);
static void     tklock_datapipe_user_activity_event_cb(gconstpointer data);
static void     tklock_datapipe_init_done_cb(gconstpointer data);

static bool     tklock_datapipe_in_tklock_submode(void);
static void     tklock_datapipe_set_tklock_submode(bool lock);
static void     tklock_datapipe_set_devicelock_state(devicelock_state_t state);
static void     tklock_datapipe_rethink_interaction_expected(void);
static void     tklock_datapipe_update_interaction_expected(bool expected);

static void     tklock_datapipe_init(void);
static void     tklock_datapipe_quit(void);

// LID_SENSOR

static bool              tklock_lidsensor_is_enabled          (void);
static void              tklock_lidsensor_init                (void);

// LID_LIGHT

static const char       *tklock_lidlight_repr                 (tklock_lidlight_t state);
static tklock_lidlight_t tklock_lidlight_from_lux             (int lux);

// LID_FILTER

static bool              tklock_lidfilter_is_enabled          (void);

static void              tklock_lidfilter_set_allow_close     (bool allow);

static tklock_lidlight_t tklock_lidfilter_map_als_state       (void);
static void              tklock_lidfilter_set_als_state       (tklock_lidlight_t state);

static gboolean          tklock_lidfilter_wait_for_close_cb   (gpointer aptr);
static bool              tklock_lidfilter_get_wait_for_close  (void);
static void              tklock_lidfilter_set_wait_for_close  (bool state);

static gboolean          tklock_lidfilter_wait_for_dark_cb    (gpointer aptr);
static bool              tklock_lidfilter_get_wait_for_dark   (void);
static void              tklock_lidfilter_set_wait_for_dark   (bool state);

static gboolean          tklock_lidfilter_wait_for_light_cb   (gpointer aptr);
static bool              tklock_lidfilter_get_wait_for_light  (void);
static void              tklock_lidfilter_set_wait_for_light  (bool state);

static void              tklock_lidfilter_rethink_als_poll    (void);
static void              tklock_lidfilter_rethink_allow_close (void);
static void              tklock_lidfilter_rethink_als_state   (void);
static void              tklock_lidfilter_rethink_lid_state   (void);

// LID_POLICY

static void              tklock_lidpolicy_rethink             (void);

// keyboard slide state machine

static void     tklock_keyboard_slide_opened(void);
static void     tklock_keyboard_slide_opened_cb(gpointer aptr);
static void     tklock_keyboard_slide_closed(void);
static void     tklock_keyboard_slide_rethink(void);

// autolock state machine

static gboolean tklock_autolock_cb(gpointer aptr);
static void     tklock_autolock_evaluate(void);
static void     tklock_autolock_enable(void);
static void     tklock_autolock_disable(void);
static void     tklock_autolock_rethink(void);
static void     tklock_autolock_init(void);
static void     tklock_autolock_quit(void);

// proximity locking state machine

static gboolean tklock_proxlock_cb(gpointer aptr);
static void     tklock_proxlock_resume(void);
static void     tklock_proxlock_evaluate(void);
static void     tklock_proxlock_enable(void);
static void     tklock_proxlock_disable(void);
static void     tklock_proxlock_rethink(void);

// autolock based on device lock changes

static void     tklock_autolock_on_devlock_block(int duration_ms);
static void     tklock_autolock_on_devlock_prime(void);
static void     tklock_autolock_on_devlock_trigger(void);

// ui exception handling state machine

static uiexception_type_t topmost_active(uiexception_type_t mask);

static void     tklock_uiexception_sync_to_datapipe(void);
static gboolean tklock_uiexception_linger_cb(gpointer aptr);
static void     tklock_uiexception_begin(uiexception_type_t type, int64_t linger);
static void     tklock_uiexception_end(uiexception_type_t type, int64_t linger);
static void     tklock_uiexception_cancel(void);
static void     tklock_uiexception_finish(void);
static bool     tklock_uiexception_deny_state_restore(bool force, const char *cause);
static void     tklock_uiexception_rethink(void);

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

// DYNAMIC_SETTINGS

static void     tklock_setting_sanitize_lid_open_actions(void);
static void     tklock_setting_sanitize_lid_close_actions(void);
static void     tklock_setting_sanitize_kbd_open_trigger(void);
static void     tklock_setting_sanitize_kbd_open_actions(void);
static void     tklock_setting_sanitize_kbd_close_trigger(void);
static void     tklock_setting_sanitize_kbd_close_actions(void);

static void     tklock_setting_cb(GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);

static void     tklock_setting_init(void);
static void     tklock_setting_quit(void);

// sysfs probing

static void     tklock_sysfs_probe(void);

// dbus ipc with systemui

static void     tklock_ui_send_tklock_signal(void);
static void     tklock_ui_notify_rethink_wakelock(void);
static bool     tklock_ui_notify_must_be_delayed(void);
static gboolean tklock_ui_notify_end_cb(gpointer data);
static gboolean tklock_ui_notify_beg_cb(gpointer data);
static void     tklock_ui_notify_schdule(void);
static gboolean tklock_ui_sync_cb(gpointer aptr);
static void     tklock_ui_notify_cancel(void);

static void     tklock_ui_eat_event(void);
static void     tklock_ui_open(void);
static void     tklock_ui_close(void);
static bool     tklock_ui_is_enabled(void);
static void     tklock_ui_set_enabled(bool enable);

static void     tklock_ui_get_devicelock_cb(DBusPendingCall *pc, void *aptr);
static void     tklock_ui_get_devicelock(void);

static void     tklock_ui_send_lpm_signal(void);
static void     tklock_ui_enable_lpm(void);
static void     tklock_ui_disable_lpm(void);
static void     tklock_ui_show_device_unlock(void);;

// dbus ipc

static void     tklock_dbus_send_display_blanking_policy(DBusMessage *const req);
static gboolean tklock_dbus_display_blanking_policy_get_cb(DBusMessage *const msg);

static void     tklock_dbus_send_keyboard_slide_state(DBusMessage *const req);
static gboolean tklock_dbus_keyboard_slide_state_get_req_cb(DBusMessage *const msg);

static void     tklock_dbus_send_keyboard_available_state(DBusMessage *const req);
static gboolean tklock_dbus_keyboard_available_state_get_req_cb(DBusMessage *const msg);

static void     tklock_dbus_send_mouse_available_state(DBusMessage *const req);
static gboolean tklock_dbus_mouse_available_state_get_req_cb(DBusMessage *const msg);

static gboolean tklock_dbus_send_tklock_mode(DBusMessage *const method_call);

static gboolean tklock_dbus_mode_get_req_cb(DBusMessage *const msg);
static tklock_request_t tklock_dbus_sanitize_requested_mode(tklock_request_t state);

static gboolean tklock_dbus_mode_change_req_cb(DBusMessage *const msg);
static gboolean tklock_dbus_interaction_expected_cb(DBusMessage *const msg);
static gboolean tklock_dbus_systemui_callback_cb(DBusMessage *const msg);

static gboolean tklock_dbus_devicelock_changed_cb(DBusMessage *const msg);

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
extern void     mce_tklock_unblank(display_state_t to_state);

/* ========================================================================= *
 * DYNAMIC_SETTINGS
 * ========================================================================= */

/** Flag: Devicelock is handled in lockscreen */
static gboolean tklock_devicelock_in_lockscreen = MCE_DEFAULT_TK_DEVICELOCK_IN_LOCKSCREEN;
static guint    tklock_devicelock_in_lockscreen_setting_id = 0;

/** Flag: Convert denied tklock removal attempt to: show device unlock view */
static bool tklock_devicelock_want_to_unlock = false;

/** Flag: Automatically lock (after ON->DIM->OFF cycle) */
static gboolean tk_autolock_enabled = MCE_DEFAULT_TK_AUTOLOCK_ENABLED;
static guint    tk_autolock_enabled_setting_id = 0;

/** Flag: Grabbing input devices is allowed */
static gboolean tk_input_policy_enabled = MCE_DEFAULT_TK_INPUT_POLICY_ENABLED;
static guint    tk_input_policy_enabled_setting_id = 0;

/** Delay for automatick locking (after ON->DIM->OFF cycle) */
static gint    tklock_autolock_delay = MCE_DEFAULT_TK_AUTOLOCK_DELAY;
static guint   tklock_autolock_delay_setting_id = 0;

/** Flag: Proximity sensor can block touch input */
static gboolean proximity_blocks_touch = MCE_DEFAULT_TK_PROXIMITY_BLOCKS_TOUCH;
static guint    proximity_blocks_touch_setting_id = 0;

/** Volume key input policy */
static gint  volkey_policy = MCE_DEFAULT_TK_VOLKEY_POLICY;
static guint volkey_policy_setting_id = 0;

/** Touchscreen gesture (doubletap etc) enable mode */
static gint  touchscreen_gesture_enable_mode = MCE_DEFAULT_DOUBLETAP_MODE;
static guint touchscreen_gesture_enable_mode_setting_id = 0;

/** Lid sensor open actions */
static gint  tklock_lid_open_actions = MCE_DEFAULT_TK_LID_OPEN_ACTIONS;
static guint tklock_lid_open_actions_setting_id = 0;

/** Lid sensor close actions */
static gint  tklock_lid_close_actions = MCE_DEFAULT_TK_LID_CLOSE_ACTIONS;
static guint tklock_lid_close_actions_setting_id = 0;

/** Flag: Is the lid sensor used for display blanking */
static gboolean lid_sensor_enabled = MCE_DEFAULT_TK_LID_SENSOR_ENABLED;
static guint    lid_sensor_enabled_setting_id = 0;

/** When to react to keyboard open */
static gint   tklock_kbd_open_trigger = MCE_DEFAULT_TK_KBD_OPEN_TRIGGER;
static guint  tklock_kbd_open_trigger_setting_id = 0;

/** How to react to keyboard open */
static gint   tklock_kbd_open_actions = MCE_DEFAULT_TK_KBD_OPEN_ACTIONS;
static guint  tklock_kbd_open_actions_setting_id = 0;

/** When to react to keyboard close */
static gint   tklock_kbd_close_trigger = MCE_DEFAULT_TK_KBD_CLOSE_TRIGGER;
static guint  tklock_kbd_close_trigger_setting_id = 0;

/** How to react to keyboard close */
static gint   tklock_kbd_close_actions = MCE_DEFAULT_TK_KBD_CLOSE_ACTIONS;
static guint  tklock_kbd_close_actions_setting_id = 0;

/** Flag for: Using ALS is allowed */
static gboolean als_enabled = MCE_DEFAULT_DISPLAY_ALS_ENABLED;
static guint    als_enabled_setting_id = 0;

/** Flag: Use ALS for lid close filtering */
static gboolean filter_lid_with_als = MCE_DEFAULT_TK_FILTER_LID_WITH_ALS;
static guint    filter_lid_with_als_setting_id = 0;

/** Maximum amount of light ALS should report when LID is closed */
static gint  filter_lid_als_limit = MCE_DEFAULT_TK_FILTER_LID_ALS_LIMIT;
static guint filter_lid_als_limit_setting_id = 0;

/** How long to keep display on after incoming call ends [ms] */
static gint  exception_length_call_in = MCE_DEFAULT_TK_EXCEPT_LEN_CALL_IN;
static guint exception_length_call_in_setting_id = 0;

/** How long to keep display on after outgoing call ends [ms] */
static gint  exception_length_call_out = MCE_DEFAULT_TK_EXCEPT_LEN_CALL_OUT;
static guint exception_length_call_out_setting_id = 0;

/** How long to keep display on after alarm is handled [ms] */
static gint  exception_length_alarm = MCE_DEFAULT_TK_EXCEPT_LEN_ALARM;
static guint exception_length_alarm_setting_id = 0;

/** How long to keep display on when usb cable is connected [ms] */
static gint  exception_length_usb_connect = MCE_DEFAULT_TK_EXCEPT_LEN_USB_CONNECT;
static guint exception_length_usb_connect_setting_id = 0;

/** How long to keep display on when usb mode dialog is shown [ms] */
static gint  exception_length_usb_dialog = MCE_DEFAULT_TK_EXCEPT_LEN_USB_DIALOG;
static guint exception_length_usb_dialog_setting_id = 0;

/** How long to keep display on when charging starts [ms] */
static gint  exception_length_charger = MCE_DEFAULT_TK_EXCEPT_LEN_CHARGER;
static guint exception_length_charger_setting_id = 0;

/** How long to keep display on after battery full [ms] */
static gint  exception_length_battery = MCE_DEFAULT_TK_EXCEPT_LEN_BATTERY;
static guint exception_length_battery_setting_id = 0;

/** How long to keep display on when audio jack is inserted [ms] */
static gint  exception_length_jack_in = MCE_DEFAULT_TK_EXCEPT_LEN_JACK_IN;
static guint exception_length_jack_in_setting_id = 0;

/** How long to keep display on when audio jack is removed [ms] */
static gint  exception_length_jack_out = MCE_DEFAULT_TK_EXCEPT_LEN_JACK_OUT;
static guint exception_length_jack_out_setting_id = 0;

/** How long to keep display on when camera button is pressed [ms] */
static gint  exception_length_camera = MCE_DEFAULT_TK_EXCEPT_LEN_CAMERA;
static guint exception_length_camera_setting_id = 0;

/** How long to keep display on when volume button is pressed [ms] */
static gint  exception_length_volume = MCE_DEFAULT_TK_EXCEPT_LEN_VOLUME;
static guint exception_length_volume_setting_id = 0;

/** How long to extend display on when there is user activity [ms] */
static gint  exception_length_activity = MCE_DEFAULT_TK_EXCEPT_LEN_ACTIVITY;
static guint exception_length_activity_setting_id = 0;

/** Flag for: Allow lockscreen animation during unblanking */
static gboolean lockscreen_anim_enabled = MCE_DEFAULT_TK_LOCKSCREEN_ANIM_ENABLED;
static guint    lockscreen_anim_enabled_setting_id = 0;

/** Default delay for delaying proximity uncovered handling [ms] */
static gint  tklock_proximity_delay_default = MCE_DEFAULT_TK_PROXIMITY_DELAY_DEFAULT;
static guint tklock_proximity_delay_default_setting_id = 0;

/** Delay for delaying proximity uncovered handling during calls [ms] */
static gint  tklock_proximity_delay_incall = MCE_DEFAULT_TK_PROXIMITY_DELAY_INCALL;
static guint tklock_proximity_delay_incall_setting_id = 0;

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
static const gchar *mce_touchscreen_gesture_enable_path = NULL;

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
 * DATAPIPE VALUES AND TRIGGERS
 * ========================================================================= */

/** Cached submode_pipe state; assume invalid */
static submode_t submode = MCE_SUBMODE_INVALID;

/** Cached PID of process owning the topmost window on UI */
static int topmost_window_pid = -1;

/** Cached init_done state; assume unknown */
static tristate_t init_done = TRISTATE_UNKNOWN;

/** Proximity state history for triggering low power mode ui */
static ps_history_t tklock_lpmui_hist[8];

/** Current tklock ui state
 *
 * Access only via tklock_ui_is_enabled() / tklock_ui_set_enabled().
 */
static bool tklock_ui_enabled_pvt = false;

/** Current tklock ui state that has been sent to lipstick */
static int  tklock_ui_notified = -1; // does not match bool values

/** System state; is undefined at bootup, can't assume anything */
static system_state_t system_state = MCE_SYSTEM_STATE_UNDEF;

/** Display state; undefined initially, can't assume anything */
static display_state_t display_state_curr = MCE_DISPLAY_UNDEF;

/** Next Display state; undefined initially, can't assume anything */
static display_state_t display_state_next = MCE_DISPLAY_UNDEF;

/** Call state; assume no active calls */
static call_state_t call_state = CALL_STATE_NONE;

/** Actual proximity state; assume not covered */
static cover_state_t proximity_sensor_actual = COVER_UNDEF;

/** Effective proximity state; assume not covered */
static cover_state_t proximity_sensor_effective = COVER_UNDEF;

/** Lid cover sensor state; assume unkown
 *
 * When in covered state, it is assumed that it is not physically
 * possible to see/interact with the display and thus it should
 * stay powered off.
 *
 * Originally was used to track Nokia N770 slidable cover. Now
 * it is used also for things like the hammerhead magnetic lid
 * sensor.
 */
static cover_state_t lid_sensor_actual = COVER_UNDEF;

/** Lid cover policy state; assume unknown */
static cover_state_t lid_sensor_filtered = COVER_UNDEF;

/** Change notifications for system_state
 */
static void tklock_datapipe_system_state_cb(gconstpointer data)
{
    system_state_t prev = system_state;
    system_state = GPOINTER_TO_INT(data);

    if( prev == system_state )
        goto EXIT;

    mce_log(LL_DEBUG, "system_state: %s -> %s",
            system_state_repr(prev),
            system_state_repr(system_state));

    tklock_ui_set_enabled(false);

EXIT:
    return;
}

/** Device lock state; assume undefined */
static devicelock_state_t devicelock_state = DEVICELOCK_STATE_UNDEFINED;

/** Push device lock state value into devicelock_state_pipe datapipe
 */
static void tklock_datapipe_set_devicelock_state(devicelock_state_t state)
{
    switch( state ) {
    case DEVICELOCK_STATE_UNLOCKED:
    case DEVICELOCK_STATE_UNDEFINED:
    case DEVICELOCK_STATE_LOCKED:
        break;

    default:
        mce_log(LL_WARN, "unknown device lock state=%d; assuming locked",
                state);
        state = DEVICELOCK_STATE_LOCKED;
        break;
    }

    if( devicelock_state != state ) {
        datapipe_exec_full(&devicelock_state_pipe,
                           GINT_TO_POINTER(state));
    }
}

/** Change notifications for devicelock_state
 */
static void tklock_datapipe_devicelock_state_cb(gconstpointer data)
{
    devicelock_state_t prev = devicelock_state;
    devicelock_state = GPOINTER_TO_INT(data);

    if( devicelock_state == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "devicelock_state = %s -> %s",
            devicelock_state_repr(prev),
            devicelock_state_repr(devicelock_state));

    tklock_uiexception_rethink();

    tklock_autolock_rethink();

    /* When lipstick is starting up we see device lock
     * going through undefined -> locked/unlocked change.
     * We must not trigger autolock due to these initial
     * device lock transitions */
    switch( prev ) {
    case DEVICELOCK_STATE_UNDEFINED:
        /* Block autolock for 60 seconds when leaving
         * undefined state (= lipstick startup) */
        tklock_autolock_on_devlock_block(60 * 1000);
        break;

    case DEVICELOCK_STATE_LOCKED:
        /* Unblock autolock earlier if we see transition
         * away from locked state (=unlocked by user) */
        tklock_autolock_on_devlock_block(0);
        break;

    default:
        break;
    }

    switch( devicelock_state ) {
    case DEVICELOCK_STATE_LOCKED:
        tklock_autolock_on_devlock_trigger();
        break;
    case DEVICELOCK_STATE_UNLOCKED:
        switch( display_state_next ) {
        case MCE_DISPLAY_OFF:
        case MCE_DISPLAY_LPM_OFF:
        case MCE_DISPLAY_LPM_ON:
            /* Transitions from undefined -> unlocked state occur
             * during device bootup / mce restart and should not
             * cause any actions.
             */
            if( prev == DEVICELOCK_STATE_UNDEFINED )
                break;

            /* Devicelock ui keeps fingerprint scanner active in LPM state
             * and unlocks device on identify, but omits unlock feedback
             * and leaves the display state as-is.
             *
             * As a workaround, execute unlock feedback from mce. Then
             * exit from LPM by requesting display power up and removal
             * of tklock submode.
             *
             * While this is mostly relevant to LPM, apply the same logic
             * also when in actually powered off display states to guard
             * against timing glitches (getting fingerprint identification
             * when already decided to exit LPM, etc) and changes in device
             * lock ui side logic.
             */
            mce_log(LL_WARN, "device got unlocked while display is off; "
                    "assume fingerprint authentication occurred");
            datapipe_exec_full(&ngfd_event_request_pipe,
                               "unlock_device");

            /* Delay display state / tklock processing until proximity
             * sensor state is known */
            common_on_proximity_schedule(MODULE_NAME,
                                         tklock_datapipe_devicelock_state_cb2,
                                         0);
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }

EXIT:
    return;
}

/** Wait for proximity sensor -callback for fingerprint unlock handling
 *
 * @param aptr unused
 */
static void tklock_datapipe_devicelock_state_cb2(gpointer aptr)
{
    (void)aptr;

    /* Still unlocked ? */
    if( devicelock_state == DEVICELOCK_STATE_UNLOCKED ) {
        if( proximity_sensor_actual != COVER_OPEN ) {
            mce_log(LL_WARN, "unblank skipped due to proximity sensor");
        }
        else {
            mce_datapipe_request_display_state(MCE_DISPLAY_ON);
            mce_datapipe_request_tklock(TKLOCK_REQUEST_OFF);
        }
    }

}

/** Resumed from suspend notification */
static void tklock_datapipe_resume_detected_event_cb(gconstpointer data)
{
    (void) data;

    /* Re-evaluate proximity locking after resuming from
     * suspend. */
    tklock_proxlock_resume();
}

/** devicelock dbus name is reserved; assume unknown */
static service_state_t devicelock_service_state = SERVICE_STATE_UNDEF;

/** Change notifications for devicelock_service_state
 */
static void tklock_datapipe_devicelock_service_state_cb(gconstpointer data)
{
    service_state_t prev = devicelock_service_state;
    devicelock_service_state = GPOINTER_TO_INT(data);

    if( devicelock_service_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "devicelock_service_state = %s -> %s",
            service_state_repr(prev),
            service_state_repr(devicelock_service_state));

    if( devicelock_service_state == SERVICE_STATE_RUNNING ) {
        /* query initial device lock state on devicelock/mce startup */
        tklock_ui_get_devicelock();
    }
    else {
        /* if device lock service is not running, the device lock
         * state is undefined */
        tklock_datapipe_set_devicelock_state(DEVICELOCK_STATE_UNDEFINED);
    }

EXIT:
    return;
}

/** Lipstick dbus name is reserved; assume false */
static service_state_t lipstick_service_state = SERVICE_STATE_UNDEF;

/** Change notifications for lipstick_service_state
 */
static void tklock_datapipe_lipstick_service_state_cb(gconstpointer data)
{
    service_state_t prev = lipstick_service_state;
    lipstick_service_state = GPOINTER_TO_INT(data);

    if( lipstick_service_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "lipstick_service_state = %s -> %s",
            service_state_repr(prev),
            service_state_repr(lipstick_service_state));

    bool enable_tklock = false;

    /* Tklock is applicable only when lipstick is running */
    if( lipstick_service_state == SERVICE_STATE_RUNNING ) {
        /* STOPPED -> RUNNING: Implies lipstick start / restart.
         * In this case lockscreen status is decided by lipstick.
         * We achieve tklock state synchronization by making a
         * lockscreen deactivation request - which lipstick can
         * then choose to honor or override.
         *
         * UNDEF -> RUNNING: Implies a mce restart while lipstick
         * is running. What we would like to happen is that
         * things stay exactly as they were. However there is
         * no way to recover lockscreen state from lipstick.
         * So in order to err on the safer side, we activate
         * lockscreen to get tklock state in sync again.
         */
        if( prev == SERVICE_STATE_UNDEF )
            enable_tklock = true;
    }

    // force tklock ipc
    tklock_ui_notified = -1;
    tklock_ui_set_enabled(enable_tklock);

EXIT:
    return;
}

/** Update mode is active; assume false */
static bool osupdate_running = false;

/** Change notifications for osupdate_running
 */
static void tklock_datapipe_osupdate_running_cb(gconstpointer data)
{
    bool prev = osupdate_running;
    osupdate_running = GPOINTER_TO_INT(data);

    if( osupdate_running == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "osupdate_running = %d -> %d", prev, osupdate_running);

    if( osupdate_running ) {
        /* undo tklock when update mode starts */
        mce_datapipe_request_tklock(TKLOCK_REQUEST_OFF);
    }

EXIT:
    return;
}

/** Device is shutting down; assume false */
static bool shutting_down = false;

/** Change notifications for shutting_down
 */
static void tklock_datapipe_shutting_down_cb(gconstpointer data)
{
    bool prev = shutting_down;
    shutting_down = GPOINTER_TO_INT(data);

    if( shutting_down == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "shutting_down = %d -> %d",
            prev, shutting_down);

    tklock_evctrl_rethink();

EXIT:
    return;
}

/** Autorelock trigger: assume disabled */
static autorelock_t autorelock_trigger = AUTORELOCK_NO_TRIGGERS;

/** Change notifications for display_state_curr
 */
static void tklock_datapipe_display_state_curr_cb(gconstpointer data)
{
    display_state_t prev = display_state_curr;
    display_state_curr = GPOINTER_TO_INT(data);

    if( display_state_curr == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_curr = %s -> %s",
            display_state_repr(prev),
            display_state_repr(display_state_curr));

    tklock_datapipe_rethink_interaction_expected();

    tklock_lidfilter_rethink_allow_close();

    /* Disable "wakeup with fake policy" hack
     * when any stable display state is reached */
    if( display_state_curr != MCE_DISPLAY_POWER_UP &&
        display_state_curr != MCE_DISPLAY_POWER_DOWN )
        tklock_uiexception_end(UIEXCEPTION_TYPE_NOANIM, 0);

    if( display_state_curr == MCE_DISPLAY_DIM )
        tklock_ui_eat_event();

    tklock_uiexception_rethink();

    tklock_autolock_rethink();
    tklock_proxlock_rethink();

    tklock_evctrl_rethink();

    tklock_ui_notify_schdule();

EXIT:
    return;
}

/** Pre-change notifications for display_state_curr
 */
static void tklock_datapipe_display_state_next_cb(gconstpointer data)
{
    display_state_next = GPOINTER_TO_INT(data);

    mce_log(LL_DEBUG, "display_state_next = %s -> %s",
            display_state_repr(display_state_curr),
            display_state_repr(display_state_next));

    if( display_state_next == display_state_curr )
        goto EXIT;

    /* Cancel autorelock on display off */
    switch( display_state_next ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        /* display states that use normal ui */
        break;

    default:
        /* display powered off, showing lpm, etc */
        if( autorelock_trigger != AUTORELOCK_NO_TRIGGERS ) {
            mce_log(LL_DEBUG, "autorelock canceled: display off");
            autorelock_trigger = AUTORELOCK_NO_TRIGGERS;
        }
        break;
    }

    tklock_autolock_on_devlock_prime();

    tklock_autolock_rethink();
    tklock_proxlock_rethink();

    tklock_lpmui_pre_transition_actions();

    tklock_ui_notify_schdule();

EXIT:
    return;

}

/** Timer id for delayed proximity uncovering */
static guint tklock_datapipe_proximity_uncover_id = 0;

/** Re-evaluate proximity sensor debugging led pattern state
 */
static void tklock_datapipe_proximity_eval_led(void)
{
    typedef enum {
        PROXIMITY_LED_STATE_UNDEFINED  = 0,
        PROXIMITY_LED_STATE_COVERED    = 1,
        PROXIMITY_LED_STATE_UNCOVERING = 2,
        PROXIMITY_LED_STATE_UNCOVERED  = 3,
    } proximity_led_state_t;

    static proximity_led_state_t prev = PROXIMITY_LED_STATE_UNDEFINED;

    /* Evaluate what led pattern should be active */
    proximity_led_state_t curr = PROXIMITY_LED_STATE_UNDEFINED;

    if( proximity_sensor_effective == COVER_OPEN )
        curr = PROXIMITY_LED_STATE_UNCOVERED;
    else if( proximity_sensor_actual == COVER_OPEN )
        curr = PROXIMITY_LED_STATE_UNCOVERING;
    else if( proximity_sensor_actual == COVER_CLOSED )
        curr = PROXIMITY_LED_STATE_COVERED;

    if( prev == curr )
        goto EXIT;

    /* Activate new pattern 1st, then deactivate old pattern
     * to avoid transition via no active pattern.
     */

    switch( curr )
    {
    case PROXIMITY_LED_STATE_UNCOVERED:
        datapipe_exec_full(&led_pattern_activate_pipe,
                           MCE_LED_PATTERN_PROXIMITY_UNCOVERED);
        break;
    case PROXIMITY_LED_STATE_UNCOVERING:
        datapipe_exec_full(&led_pattern_activate_pipe,
                           MCE_LED_PATTERN_PROXIMITY_UNCOVERING);
        break;
    case PROXIMITY_LED_STATE_COVERED:
        datapipe_exec_full(&led_pattern_activate_pipe,
                           MCE_LED_PATTERN_PROXIMITY_COVERED);
        break;
    default:
        break;
    }

    switch( prev )
    {
    case PROXIMITY_LED_STATE_UNCOVERED:
        datapipe_exec_full(&led_pattern_deactivate_pipe,
                           MCE_LED_PATTERN_PROXIMITY_UNCOVERED);
        break;
    case PROXIMITY_LED_STATE_UNCOVERING:
        datapipe_exec_full(&led_pattern_deactivate_pipe,
                           MCE_LED_PATTERN_PROXIMITY_UNCOVERING);
        break;
    case PROXIMITY_LED_STATE_COVERED:
        datapipe_exec_full(&led_pattern_deactivate_pipe,
                           MCE_LED_PATTERN_PROXIMITY_COVERED);
        break;
    default:
        break;
    }

    prev = curr;

EXIT:
    return;
}

/** Set effective proximity state from current sensor state
 */
static void tklock_datapipe_proximity_update(void)
{
    if( proximity_sensor_effective == proximity_sensor_actual )
        goto EXIT;

    mce_log(LL_DEBUG, "proximity_sensor_effective = %s -> %s",
            proximity_state_repr(proximity_sensor_effective),
            proximity_state_repr(proximity_sensor_actual));

    proximity_sensor_effective = proximity_sensor_actual;

    datapipe_exec_full(&proximity_sensor_effective_pipe,
                       GINT_TO_POINTER(proximity_sensor_effective));

    tklock_datapipe_proximity_eval_led();
    tklock_uiexception_rethink();
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

/** Schedule delayed proximity uncovering
 */
static void tklock_datapipe_proximity_uncover_schedule(void)
{
    if( tklock_datapipe_proximity_uncover_id )
        g_source_remove(tklock_datapipe_proximity_uncover_id);
    else
        wakelock_lock("mce_proximity_stm", -1);

    int delay = tklock_proximity_delay_default;

    if( call_state == CALL_STATE_ACTIVE )
        delay = tklock_proximity_delay_incall;

    if( delay < MCE_MINIMUM_TK_PROXIMITY_DELAY )
        delay = MCE_MINIMUM_TK_PROXIMITY_DELAY;
    else if( delay > MCE_MAXIMUM_TK_PROXIMITY_DELAY )
        delay = MCE_MAXIMUM_TK_PROXIMITY_DELAY;

    tklock_datapipe_proximity_uncover_id =
        g_timeout_add(delay, tklock_datapipe_proximity_uncover_cb, 0);
}

/** Change notifications for proximity_sensor_actual
 */
static void tklock_datapipe_proximity_sensor_actual_cb(gconstpointer data)
{
    cover_state_t prev = proximity_sensor_actual;
    proximity_sensor_actual = GPOINTER_TO_INT(data);

    if( proximity_sensor_actual == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "proximity_sensor_actual = %s -> %s",
            proximity_state_repr(prev),
            proximity_state_repr(proximity_sensor_actual));

    tklock_datapipe_proximity_eval_led();

    /* update lpm ui proximity history using raw data */
    tklock_lpmui_update_history(proximity_sensor_actual);

    if( proximity_sensor_actual == COVER_OPEN ) {
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
    static bool incoming = false;

    call_state_t prev = call_state;
    call_state = GPOINTER_TO_INT(data);

    if( call_state == CALL_STATE_INVALID )
        call_state = CALL_STATE_NONE;

    if( call_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "call_state = %s -> %s",
            call_state_repr(prev),
            call_state_repr(call_state));

    switch( call_state ) {
    case CALL_STATE_RINGING:
        /* Switch to using longer incoming call linger time */
        incoming = true;

        /* Fall through */

    case CALL_STATE_ACTIVE:
        tklock_uiexception_begin(UIEXCEPTION_TYPE_CALL, 0);
        break;

    default:
        tklock_uiexception_end(UIEXCEPTION_TYPE_CALL, incoming ?
                               exception_length_call_in :
                               exception_length_call_out);

        /* Restore linger time to default again */
        incoming = false;
        break;
    }

    // display on/off policy
    tklock_uiexception_rethink();

    // volume keys during call
    tklock_evctrl_rethink();
EXIT:
    return;
}

/** Music playback state; assume not playing */
static bool music_playback_ongoing = false;

/** Change notifications for music_playback_ongoing
 */
static void tklock_datapipe_music_playback_ongoing_cb(gconstpointer data)
{
    bool prev = music_playback_ongoing;
    music_playback_ongoing = GPOINTER_TO_INT(data);

    if( music_playback_ongoing == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "music_playback_ongoing = %d -> %d",
            prev, music_playback_ongoing);

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

    mce_log(LL_DEBUG, "alarm_ui_state = %s -> %s",
            alarm_state_repr(prev),
            alarm_state_repr(alarm_ui_state));

    switch( alarm_ui_state ) {
    case MCE_ALARM_UI_RINGING_INT32:
    case MCE_ALARM_UI_VISIBLE_INT32:
        tklock_uiexception_begin(UIEXCEPTION_TYPE_ALARM, 0);
        break;
    default:
        tklock_uiexception_end(UIEXCEPTION_TYPE_ALARM, exception_length_alarm);
        break;
    }

    tklock_uiexception_rethink();

EXIT:
    return;
}

/** Charger state; assume not charging */
static charger_state_t charger_state = CHARGER_STATE_UNDEF;

/** Change notifications for charger_state
 */
static void tklock_datapipe_charger_state_cb(gconstpointer data)
{
    charger_state_t prev = charger_state;
    charger_state = GPOINTER_TO_INT(data);

    if( charger_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "charger_state = %s -> %s",
            charger_state_repr(prev),
            charger_state_repr(charger_state));

    /* No exception on mce startup */
    if( prev == CHARGER_STATE_UNDEF )
        goto EXIT;

    /* Notification expected when charging starts */
    if( charger_state == CHARGER_STATE_ON )
        mce_tklock_begin_notification(0, "mce_charger_state",
                                      exception_length_charger, -1);

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

    mce_log(LL_DEBUG, "battery_status = %s -> %s",
            battery_status_repr(prev),
            battery_status_repr(battery_status));

    if( battery_status == BATTERY_STATUS_FULL ) {
        mce_tklock_begin_notification(0, "mce_battery_full",
                                      exception_length_battery, -1);
    }

EXIT:
    return;
}

/** USB cable status; assume disconnected */
static usb_cable_state_t usb_cable_state = USB_CABLE_UNDEF;

/** Change notifications for usb_cable_state
 */
static void tklock_datapipe_usb_cable_state_cb(gconstpointer data)
{
    usb_cable_state_t prev = usb_cable_state;
    usb_cable_state = GPOINTER_TO_INT(data);

    if( prev == usb_cable_state )
        goto EXIT;

    mce_log(LL_DEBUG, "usb_cable_state = %s -> %s",
            usb_cable_state_repr(prev),
            usb_cable_state_repr(usb_cable_state));

    /* No exception on mce startup */
    if( prev == USB_CABLE_UNDEF )
        goto EXIT;

    switch( usb_cable_state ) {
    case USB_CABLE_DISCONNECTED:
        mce_tklock_end_notification(0, "mce_usb_connect", 0);
        mce_tklock_end_notification(0, "mce_usb_dialog", 0);
        break;

    case USB_CABLE_CONNECTED:
        mce_tklock_begin_notification(0, "mce_usb_connect",
                                      exception_length_usb_connect, -1);
        break;

    case USB_CABLE_ASK_USER:
        mce_tklock_begin_notification(0, "mce_usb_dialog",
                                      exception_length_usb_dialog, -1);
        break;

    default:
        goto EXIT;
    }

EXIT:
    return;
}

/** Audio jack state; assume not known yet */
static cover_state_t jack_sense_state = COVER_UNDEF;

/** Change notifications for jack_sense_state
 */
static void tklock_datapipe_jack_sense_state_cb(gconstpointer data)
{
    cover_state_t prev = jack_sense_state;
    jack_sense_state = GPOINTER_TO_INT(data);

    if( prev == jack_sense_state )
        goto EXIT;

    mce_log(LL_DEBUG, "jack_sense_state = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(jack_sense_state));

    if( prev == COVER_UNDEF )
        goto EXIT;

    int64_t length = -1;

    switch( jack_sense_state ) {
    case COVER_CLOSED:
        length = exception_length_jack_in;
        break;
    case COVER_OPEN:
        length = exception_length_jack_out;
        break;
    default:
        break;
    }

    mce_tklock_begin_notification(0, "mce_jack_sense", length, -1);

EXIT:
    return;
}

/** Change notifications for camera_button
 */
static void tklock_datapipe_camera_button_state_cb(gconstpointer const data)
{
    /* TODO: This might make no sense, need to check on HW that has
     *       dedicated camera button ... */
    (void)data;

    mce_tklock_begin_notification(0, "mce_camera_button",
                                  exception_length_camera, -1);
}

/** Change notifications for keypress
 */
static void tklock_datapipe_keypress_event_cb(gconstpointer const data)
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
                                      exception_length_camera, -1);
        break;

    case KEY_VOLUMEDOWN:
    case KEY_VOLUMEUP:
        if( datapipe_get_gint(keypad_grab_wanted_pipe) ) {
            mce_log(LL_DEVEL, "volume key ignored");
            break;
        }
        mce_log(LL_DEBUG, "volume key");
        mce_tklock_begin_notification(0, "mce_volume_key",
                                      exception_length_volume, -1);
        break;

    default:
        break;
    }

EXIT:
    return;
}

/** UI exception state; initialized to none */
static uiexception_type_t uiexception_type = UIEXCEPTION_TYPE_NONE;

/** Change notifications for uiexception_type
 */
static void tklock_datapipe_uiexception_type_cb(gconstpointer data)
{
    uiexception_type_t prev = uiexception_type;
    uiexception_type = GPOINTER_TO_INT(data);

    if( uiexception_type == prev )
        goto EXIT;

    mce_log(LL_CRUCIAL, "uiexception_type = %s -> %s",
            uiexception_type_repr(prev),
            uiexception_type_repr(uiexception_type));

    /* Cancel autorelock if there is a call or alarm */
    if( (uiexception_type & (UIEXCEPTION_TYPE_CALL | UIEXCEPTION_TYPE_ALARM)) &&
        autorelock_trigger != AUTORELOCK_NO_TRIGGERS ) {
        mce_log(LL_DEBUG, "autorelock canceled: handling call/alarm");
        autorelock_trigger = AUTORELOCK_NO_TRIGGERS;
    }

    /* Forget lpm ui triggering history
     * whenever exception state changes */
    tklock_lpmui_reset_history();

    tklock_autolock_rethink();
    tklock_proxlock_rethink();

    /* Broadcast blanking policy change */
    tklock_dbus_send_display_blanking_policy(0);

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

    mce_log(LL_DEBUG, "audio_route = %s -> %s",
            audio_route_repr(prev), audio_route_repr(audio_route));

    tklock_uiexception_rethink();

EXIT:
    return;
}

/** Change notifications for tklock_request_pipe
 *
 * Handles tklock requests from outside this module
 */
static void tklock_datapipe_tklock_request_cb(gconstpointer data)
{
    tklock_request_t tklock_request = GPOINTER_TO_INT(data);

    mce_log(LL_DEBUG, "tklock_request = %s",
            tklock_request_repr(tklock_request));

    bool enable = tklock_ui_is_enabled();

    switch( tklock_request ) {
    case TKLOCK_REQUEST_UNDEF:
    case TKLOCK_REQUEST_OFF:
    case TKLOCK_REQUEST_OFF_DELAYED:
        enable = false;
        break;
    default:
    case TKLOCK_REQUEST_OFF_PROXIMITY:
    case TKLOCK_REQUEST_ON:
    case TKLOCK_REQUEST_ON_DIMMED:
    case TKLOCK_REQUEST_ON_PROXIMITY:
    case TKLOCK_REQUEST_ON_DELAYED:
        enable = true;
        break;
    case TKLOCK_REQUEST_TOGGLE:
        enable = !enable;
    }
    tklock_ui_set_enabled(enable);
}

/** Interaction expected; assume false */
static bool interaction_expected = false;

/** Interaction expected; unfiltered info from compositor */
static bool interaction_expected_raw = false;

/** Change notifications for interaction_expected_pipe
 */
static void tklock_datapipe_interaction_expected_cb(gconstpointer data)
{
    bool prev = interaction_expected;
    interaction_expected = GPOINTER_TO_INT(data);

    if( prev == interaction_expected )
        goto EXIT;

    mce_log(LL_DEBUG, "interaction_expected: %d -> %d",
            prev, interaction_expected);

    /* All changes must be ignored when handling exceptional things
     * like calls and alarms that are shown on top of lockscreen ui.
     */
    if( uiexception_type & (UIEXCEPTION_TYPE_CALL | UIEXCEPTION_TYPE_ALARM) )
        goto EXIT;

    /* Edge triggered action: When interaction becomes expected
     * while lockscreen is still active (e.g. display has been
     * unblanked to show notification on the lockscreen and
     * user has swiped from plain lockscreen view to device unlock
     * code entry view) the display state restore should be disabled.
     */
    if( display_state_next == MCE_DISPLAY_ON &&
        tklock_ui_is_enabled() && interaction_expected ) {
        tklock_uiexception_deny_state_restore(true, "interaction expected");
    }

EXIT:
    return;
}

/** Re-evaluate effective interaction_expected value
 *
 * The notifications from compositor side do not always make
 * sense from mce point of view.
 *
 * This function:
 * - Normalizes the interaction_expected value by filtering out
 *   obviously impossible situations such as having interacation
 *   expected while display is powered off.
 * - Should be called whenever state variables used in the
 *   filtering have changed.
 */
static void tklock_datapipe_rethink_interaction_expected(void)
{
    bool use = interaction_expected_raw;

    switch( display_state_curr ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        /* Display is in state that allows interaction */
        if( submode & MCE_SUBMODE_TKLOCK ) {
            /* Lockscreen active
             * -> use reported state
             */
        }
        else if( topmost_window_pid == -1 ) {
            /* Home screen active
             * -> use reported state
             */
        }
        else {
            /* Application active
             * -> ignore reported state
             */
            use = true;
        }
        break;
    default:
        /* Display is not in state allowing interaction
         * -> ignore reported state */
        use = false;
        break;
    }

    if( interaction_expected != use )
        datapipe_exec_full(&interaction_expected_pipe, GINT_TO_POINTER(use));
}

/** Update raw interaction expected state
 *
 * Updates cached raw value and re-calculates effective value.
 *
 * @param expected  state as reported by compositor
 */
static void tklock_datapipe_update_interaction_expected(bool expected)
{
    if( interaction_expected_raw == expected )
        goto EXIT;

    mce_log(LL_DEBUG, "interaction_expected_raw: %d -> %d",
            interaction_expected_raw, expected);

    interaction_expected_raw = expected;
    tklock_datapipe_rethink_interaction_expected();

EXIT:
    return;

}

/** Filter tklock submode changes
 *
 * All tklock submode changes are subjected to policy implemented
 * at tklock_ui_xxx() functions.
 *
 * Basically this ensures tklock_datapipe_submode_cb() will never
 * see submode values where tklock would not agree with policy.
 */
static gpointer tklock_datapipe_submode_filter_cb(gpointer data)
{
    submode_t input  = GPOINTER_TO_INT(data);
    submode_t output = input;

    tklock_ui_set_enabled(input & MCE_SUBMODE_TKLOCK);

    if( tklock_ui_is_enabled() )
        output |= MCE_SUBMODE_TKLOCK;
    else
        output &= ~MCE_SUBMODE_TKLOCK;

    if( input != output )
        mce_log(LL_DEBUG, "submode filter: %s", submode_change_repr(input, output));

    return GINT_TO_POINTER(output);
}

/** Change notifications for submode
 */
static void tklock_datapipe_submode_cb(gconstpointer data)
{
    submode_t prev = submode;
    submode = GPOINTER_TO_INT(data);

    if( submode == prev )
        goto EXIT;

    /* Note: Due to filtering at tklock_datapipe_submode_filter_cb()
     *       the submode value seen here is always in sync with policy
     *       implemented at tklock_ui_xxx() functions.
     */

    mce_log(LL_DEBUG, "submode = %s", submode_change_repr(prev, submode));

    // out of sync tklock state blocks state restore
    tklock_uiexception_rethink();

    // block tklock removal while autolock rules apply
    tklock_autolock_rethink();
    tklock_proxlock_rethink();

    tklock_evctrl_rethink();

    // skip the rest if tklock did not change
    if( !((prev ^ submode) & MCE_SUBMODE_TKLOCK) )
        goto EXIT;

    tklock_datapipe_rethink_interaction_expected();

    if( submode & MCE_SUBMODE_TKLOCK ) {
        // tklock added
    }
    else {
        // tklock removed
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
            mce_datapipe_request_display_state(MCE_DISPLAY_ON);
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
static bool tklock_datapipe_in_tklock_submode(void)
{
    return (submode & MCE_SUBMODE_TKLOCK) != 0;
}

static void tklock_datapipe_set_tklock_submode(bool lock)
{
    /* This function should be called only via:
     *
     * tklock_ui_set_enabled()
     *    tklock_ui_sync_cb()
     *       tklock_datapipe_set_tklock_submode()
     */

    mce_log(LL_DEBUG, "tklock submode request: %s",
            lock ? "LOCK" : "UNLOCK");

    if( lock )
        mce_add_submode_int32(MCE_SUBMODE_TKLOCK);
    else
        mce_rem_submode_int32(MCE_SUBMODE_TKLOCK);
}

/** Change notifications for lockkey_state_pipe
 */
static void tklock_datapipe_lockkey_state_cb(gconstpointer const data)
{
    /* TODO: IIRC lock key is N900 hw feature, I have not had a chance
     *       to test if this actually works ... */

    key_state_t key_state = GPOINTER_TO_INT(data);

    mce_log(LL_DEBUG, "lockkey: %s", key_state_repr(key_state));

    /* Ignore release events */
    if( key_state != KEY_STATE_PRESSED )
        goto EXIT;

    /* Try to give it the same treatment as power key would get.
     * Copy pasted from generic_powerkey_handler() @ powerkey.c */
    switch( display_state_next ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_POWER_UP:
        mce_log(LL_DEBUG, "display -> off + lock");

        /* Do the locking before turning display off.
         *
         * The tklock requests get ignored in act dead
         * etc, so we can just blindly request it.
         */
        mce_datapipe_request_tklock(TKLOCK_REQUEST_ON);
        mce_datapipe_request_display_state(MCE_DISPLAY_OFF);
        break;

    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_DOWN:
        mce_log(LL_DEBUG, "display -> on");
        mce_datapipe_request_display_state(MCE_DISPLAY_ON);
        break;
    }

EXIT:
    return;
}

/** Change notifications for heartbeat_event_pipe
 */
static void tklock_datapipe_heartbeat_event_cb(gconstpointer data)
{
    (void)data;

    mce_log(LL_DEBUG, "heartbeat");
    tklock_dtcalib_from_heartbeat();
}

/** Keypad slide input state; assume closed */
static cover_state_t keyboard_slide_input_state = COVER_CLOSED;

/** Change notifications from keyboard_slide_state_pipe
 */
static void tklock_datapipe_keyboard_slide_input_state_cb(gconstpointer const data)
{
    cover_state_t prev = keyboard_slide_input_state;
    keyboard_slide_input_state = GPOINTER_TO_INT(data);

    if( keyboard_slide_input_state == COVER_UNDEF )
        keyboard_slide_input_state = COVER_CLOSED;

    if( keyboard_slide_input_state == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "keyboard_slide_input_state = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(keyboard_slide_input_state));

    tklock_keyboard_slide_rethink();

EXIT:
    return;
}

/** Keypad slide output state; assume unknown */
static cover_state_t keyboard_slide_output_state = COVER_UNDEF;

static void
tklock_datapipe_keyboard_slide_output_state_cb(gconstpointer const data)
{
    cover_state_t prev = keyboard_slide_output_state;
    keyboard_slide_output_state = GPOINTER_TO_INT(data);

    if( keyboard_slide_output_state == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "keyboard_slide_output_state = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(keyboard_slide_output_state));

    tklock_dbus_send_keyboard_slide_state(0);

EXIT:
    return;
}

/** Keypad available output state; assume unknown */
static cover_state_t keyboard_available_state = COVER_UNDEF;

static void
tklock_datapipe_keyboard_available_state_cb(gconstpointer const data)
{
    cover_state_t prev = keyboard_available_state;
    keyboard_available_state = GPOINTER_TO_INT(data);

    if( keyboard_available_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "keyboard_available_state = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(keyboard_available_state));

    tklock_dbus_send_keyboard_available_state(0);

EXIT:
    return;
}

/** Mouse available output state; assume unknown */
static cover_state_t mouse_available_state = COVER_UNDEF;

static void
tklock_datapipe_mouse_available_state_cb(gconstpointer const data)
{
    cover_state_t prev = mouse_available_state;
    mouse_available_state = GPOINTER_TO_INT(data);

    if( mouse_available_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "mouse_available_state = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(mouse_available_state));

    tklock_dbus_send_mouse_available_state(0);

EXIT:
    return;
}

/** Cached als poll state; tracked via tklock_datapipe_light_sensor_poll_request_cb() */
static gboolean light_sensor_polling = FALSE;

/** Ambient Light Sensor filter for temporary sensor enable
 *
 * @param data Polling enabled/disabled bool (as void pointer)
 */
static void
tklock_datapipe_light_sensor_poll_request_cb(gconstpointer const data)
{
    gboolean prev = light_sensor_polling;
    light_sensor_polling = GPOINTER_TO_INT(data) ? TRUE : FALSE;

    mce_log(LL_DEBUG, "light_sensor_polling: %s -> %s",
            prev ? "true" : "false",
            light_sensor_polling ? "true" : "false");

    /* Check without comparing to previous state. The poll
     * request can be denied by datapipe filter at the als
     * plugin - in which case we see a false->false transition
     * here at datapipe output trigger callback. */
    tklock_lidfilter_rethink_als_poll();
}

/** Change notifications for topmost_window_pid_pipe
 */
static void
tklock_datapipe_topmost_window_pid_cb(gconstpointer data)
{
    int prev = topmost_window_pid;
    topmost_window_pid = GPOINTER_TO_INT(data);

    if( prev == topmost_window_pid )
        goto EXIT;

    mce_log(LL_DEBUG, "topmost_window_pid: %d -> %d",
            prev, topmost_window_pid);

    tklock_datapipe_rethink_interaction_expected();

EXIT:
    return;
}

/** Raw ambient light sensor state; assume unknown */
static int light_sensor_actual = -1;

/** Change notifications from light_sensor_actual_pipe
 */
static void tklock_datapipe_light_sensor_actual_cb(gconstpointer data)
{
    cover_state_t prev = light_sensor_actual;
    light_sensor_actual = GPOINTER_TO_INT(data);

    if( light_sensor_actual == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "light_sensor_actual = %d -> %d",
            prev, light_sensor_actual);

    tklock_lidfilter_rethink_als_state();

EXIT:
    return;
}

/** Assume lid sensor is broken until we have seen closed->open transition
 *
 * If the lid sensor is used for display blanking, a faulty sensor can
 * cause a lot of problems.
 *
 * To avoid this mce tracks persistently whether the sensor on the device
 * has been seen to function on previous mce invocations.
 *
 * This cached state must be recovered before the datapipe callbacks
 * that depend on it are hooked up.
 */
static bool tklock_lid_sensor_is_working = false;

/** Path to the flag file for persistent tklock_lid_sensor_is_working */
#define LID_SENSOR_IS_WORKING_FLAG_FILE "/var/lib/mce/lid_sensor_is_working"

/** Keep flag file in sync with lid_sensor_is_working_pipe status
 */
static void tklock_datapipe_lid_sensor_is_working_cb(gconstpointer data)
{
    bool prev = tklock_lid_sensor_is_working;
    tklock_lid_sensor_is_working = GPOINTER_TO_INT(data);

    if( tklock_lid_sensor_is_working == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "lid_sensor_is_working = %s -> %s",
            prev ? "true" : "false",
            tklock_lid_sensor_is_working ? "true" : "false");

    if( tklock_lid_sensor_is_working ) {
        /* Create flag file */
        int fd = open(LID_SENSOR_IS_WORKING_FLAG_FILE, O_WRONLY|O_CREAT, 0644);
        if( fd == -1 )
            mce_log(LL_WARN, "%s: could not create flag file: %m",
                    LID_SENSOR_IS_WORKING_FLAG_FILE);
        else
            close(fd);

        tklock_lidpolicy_rethink();
    }
    else {
        /* Remove flag file */
        if( unlink(LID_SENSOR_IS_WORKING_FLAG_FILE) == -1 && errno != ENOENT )
            mce_log(LL_WARN, "%s: could not remove flag file: %m",
                    LID_SENSOR_IS_WORKING_FLAG_FILE);

        /* Invalidate sensor data */
        datapipe_exec_full(&lid_sensor_actual_pipe,
                           GINT_TO_POINTER(COVER_UNDEF));
    }

EXIT:
    return;
}

/** Change notifications from lid_sensor_actual_pipe
 */
static void tklock_datapipe_lid_sensor_actual_cb(gconstpointer data)
{
    cover_state_t prev = lid_sensor_actual;
    lid_sensor_actual = GPOINTER_TO_INT(data);

    if( lid_sensor_actual == prev )
        goto EXIT;

    if( prev == COVER_CLOSED &&  lid_sensor_actual == COVER_OPEN ) {
        /* We have seen the sensor flip from closed to open position,
         * so we can stop assuming it stays closed forever */
        datapipe_exec_full(&lid_sensor_is_working_pipe,
                           GINT_TO_POINTER(true));
    }

    mce_log(LL_DEVEL, "lid_sensor_actual = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(lid_sensor_actual));

    tklock_lidfilter_rethink_lid_state();

EXIT:
    return;
}

/** Change notifications from lid_sensor_filtered_pipe
 */
static void tklock_datapipe_lid_sensor_filtered_cb(gconstpointer data)
{
    cover_state_t prev = lid_sensor_filtered;
    lid_sensor_filtered = GPOINTER_TO_INT(data);

    if( lid_sensor_filtered == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "lid_sensor_filtered = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(lid_sensor_filtered));

    /* TODO: On devices that have means to detect physically covered
     *       display, it might be desirable to also power off:
     *       - proximity sensor
     *       - notification led
     *       - double tap detection
     *
     * Note: Logic for volume key control exists, but is not used atm */

    /* Re-evaluate need for touch blocking */
    tklock_evctrl_rethink();

EXIT:
    return;
}

/** Camera lens cover state; assume closed */
static cover_state_t lens_cover_state = COVER_CLOSED;

/** Change notifications from lens_cover_state_pipe
 */
static void tklock_datapipe_lens_cover_state_cb(gconstpointer data)
{
    cover_state_t prev = lens_cover_state;
    lens_cover_state = GPOINTER_TO_INT(data);

    if( lens_cover_state == COVER_UNDEF )
        lens_cover_state = COVER_CLOSED;

    if( lens_cover_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "lens_cover_state = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(lens_cover_state));

    // TODO: COVER_OPEN  -> display on, unlock, reason=AUTORELOCK_KBD_SLIDE
    // TODO: COVER_CLOSE -> display off, lock if reason==AUTORELOCK_KBD_SLIDE

EXIT:
    return;
}

/** Check if event relates to ongoing user activity on screen
 *
 * Detect touch screen events that signify finger on screen
 * situation.
 *
 * To make things work in SDK do mouse click detection too.
 */
static bool tklock_touch_activity_event_p(const struct input_event *ev)
{
    bool activity = false;

    switch( ev->type ) {
    case EV_KEY:
        switch( ev->code ) {
        case BTN_MOUSE:
        case BTN_TOUCH:
            activity = (ev->value != 0);
            break;

        default:
            break;
        }
        break;

    case EV_ABS:
        switch( ev->code ) {
        case ABS_MT_POSITION_X:
        case ABS_MT_POSITION_Y:
            activity = true;
            break;

        case ABS_MT_PRESSURE:
        case ABS_MT_TOUCH_MAJOR:
        case ABS_MT_WIDTH_MAJOR:
            activity = (ev->value > 0);
            break;

        case ABS_MT_TRACKING_ID:
            activity = (ev->value != -1);
            break;

        default:
            break;
        }
        break;

    default:
        break;
    }

    return activity;
}

/** Handle user_activity_event_pipe notifications
 *
 * @param data input_event as void pointer
 */
static void tklock_datapipe_user_activity_event_cb(gconstpointer data)
{
    static int64_t last_time = 0;

    const struct input_event *ev = data;

    if( !ev )
        goto EXIT;

    /* We are only interested in touch activity */
    if( !tklock_touch_activity_event_p(ev) )
        goto EXIT;

    /* Deal with autorelock cancellation 1st */
    if( autorelock_trigger != AUTORELOCK_NO_TRIGGERS ) {
        mce_log(LL_DEBUG, "autorelock canceled: touch activity");
        autorelock_trigger = AUTORELOCK_NO_TRIGGERS;
    }

    /* Touch events relevant unly when handling notification & linger */
    if( !(uiexception_type & (UIEXCEPTION_TYPE_NOTIF | UIEXCEPTION_TYPE_LINGER)) )
        goto EXIT;

    int64_t now = mce_lib_get_boot_tick();

    if( last_time + 200 > now )
        goto EXIT;

    last_time = now;

    mce_log(LL_DEBUG, "type: %s, code: %s, value: %d",
            evdev_get_event_type_name(ev->type),
            evdev_get_event_code_name(ev->type, ev->code),
            ev->value);

    /* N.B. the uiexception_type is bitmask, but only bit at time is
     *      visible in the uiexception_type datapipe */
    switch( uiexception_type ) {
    case UIEXCEPTION_TYPE_LINGER:
        /* touch events during linger -> do not restore display state */
        tklock_uiexception_deny_state_restore(true,
                                              "touch event during linger");
        break;

    case UIEXCEPTION_TYPE_NOTIF:
        /* touch events while device is not locked -> do not restore display state */
        if( tklock_uiexception_deny_state_restore(false,
                                                  "touch event during notification") ) {
            break;
        }
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

/** Change notifications for init_done
 */
static void tklock_datapipe_init_done_cb(gconstpointer data)
{
    tristate_t prev = init_done;
    init_done = GPOINTER_TO_INT(data);

    if( init_done == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "init_done = %s -> %s",
            tristate_repr(prev),
            tristate_repr(init_done));

    /* No direct actions, but restoring display state
     * after notifications etc is disabled until init
     * done is reached. See tklock_uiexception_begin().
     */

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t tklock_datapipe_handlers[] =
{
    // input filters
    {
        .datapipe  = &submode_pipe,
        .filter_cb = tklock_datapipe_submode_filter_cb,
    },
    // output triggers
    {
        .datapipe  = &resume_detected_event_pipe,
        .output_cb = tklock_datapipe_resume_detected_event_cb,
    },
    {
        .datapipe  = &lipstick_service_state_pipe,
        .output_cb = tklock_datapipe_lipstick_service_state_cb,
    },
    {
        .datapipe  = &devicelock_service_state_pipe,
        .output_cb = tklock_datapipe_devicelock_service_state_cb,
    },
    {
        .datapipe  = &osupdate_running_pipe,
        .output_cb = tklock_datapipe_osupdate_running_cb,
    },
    {
        .datapipe  = &shutting_down_pipe,
        .output_cb = tklock_datapipe_shutting_down_cb,
    },
    {
        .datapipe  = &devicelock_state_pipe,
        .output_cb = tklock_datapipe_devicelock_state_cb,
    },
    {
        .datapipe  = &display_state_curr_pipe,
        .output_cb = tklock_datapipe_display_state_curr_cb,
    },
    {
        .datapipe  = &display_state_next_pipe,
        .output_cb = tklock_datapipe_display_state_next_cb,
    },
    {
        .datapipe  = &interaction_expected_pipe,
        .output_cb = tklock_datapipe_interaction_expected_cb,
    },
    {
        .datapipe  = &proximity_sensor_actual_pipe,
        .output_cb = tklock_datapipe_proximity_sensor_actual_cb,
    },
    {
        .datapipe  = &call_state_pipe,
        .output_cb = tklock_datapipe_call_state_cb,
    },
    {
        .datapipe  = &music_playback_ongoing_pipe,
        .output_cb = tklock_datapipe_music_playback_ongoing_cb,
    },
    {
        .datapipe  = &alarm_ui_state_pipe,
        .output_cb = tklock_datapipe_alarm_ui_state_cb,
    },
    {
        .datapipe  = &charger_state_pipe,
        .output_cb = tklock_datapipe_charger_state_cb,
    },
    {
        .datapipe = &battery_status_pipe,
        .output_cb = tklock_datapipe_battery_status_cb,
    },
    {
        .datapipe  = &uiexception_type_pipe,
        .output_cb = tklock_datapipe_uiexception_type_cb,
    },
    {
        .datapipe  = &audio_route_pipe,
        .output_cb = tklock_datapipe_audio_route_cb,
    },
    {
        .datapipe  = &system_state_pipe,
        .output_cb = tklock_datapipe_system_state_cb,
    },
    {
        .datapipe  = &usb_cable_state_pipe,
        .output_cb = tklock_datapipe_usb_cable_state_cb,
    },
    {
        .datapipe  = &jack_sense_state_pipe,
        .output_cb = tklock_datapipe_jack_sense_state_cb,
    },
    {
        .datapipe  = &heartbeat_event_pipe,
        .output_cb = tklock_datapipe_heartbeat_event_cb,
    },
    {
        .datapipe  = &submode_pipe,
        .output_cb = tklock_datapipe_submode_cb,
    },
    {
        .datapipe  = &light_sensor_actual_pipe,
        .output_cb = tklock_datapipe_light_sensor_actual_cb,
    },
    {
        .datapipe  = &lid_sensor_is_working_pipe,
        .output_cb = tklock_datapipe_lid_sensor_is_working_cb,
    },
    {
        .datapipe  = &lid_sensor_actual_pipe,
        .output_cb = tklock_datapipe_lid_sensor_actual_cb,
    },
    {
        .datapipe  = &lid_sensor_filtered_pipe,
        .output_cb = tklock_datapipe_lid_sensor_filtered_cb,
    },
    {
        .datapipe  = &lens_cover_state_pipe,
        .output_cb = tklock_datapipe_lens_cover_state_cb,
    },
    {
        .datapipe  = &user_activity_event_pipe,
        .output_cb = tklock_datapipe_user_activity_event_cb,

    },
    {
        .datapipe  = &init_done_pipe,
        .output_cb = tklock_datapipe_init_done_cb,
    },
    {
        /* Note: Keybaord slide state signaling must reflect
         *       the actual state -> uses output triggering
         *       unlike the display state logic that is bound
         *       to datapipe input. */
        .datapipe  = &keyboard_slide_state_pipe,
        .output_cb = tklock_datapipe_keyboard_slide_output_state_cb,
    },
    {
        .datapipe  = &keyboard_available_state_pipe,
        .output_cb = tklock_datapipe_keyboard_available_state_cb,
    },
    {
        .datapipe  = &mouse_available_state_pipe,
        .output_cb = tklock_datapipe_mouse_available_state_cb,
    },
    {
        .datapipe  = &light_sensor_poll_request_pipe,
        .output_cb = tklock_datapipe_light_sensor_poll_request_cb,
    },
    {
        .datapipe  = &topmost_window_pid_pipe,
        .output_cb = tklock_datapipe_topmost_window_pid_cb,
    },

    // input triggers
    {
        .datapipe = &tklock_request_pipe,
        .input_cb = tklock_datapipe_tklock_request_cb,
    },
    {
        .datapipe = &keypress_event_pipe,
        .input_cb = tklock_datapipe_keypress_event_cb,
    },
    {
        .datapipe = &lockkey_state_pipe,
        .input_cb = tklock_datapipe_lockkey_state_cb,
    },
    {
        .datapipe = &camera_button_state_pipe,
        .input_cb = tklock_datapipe_camera_button_state_cb,
    },
    {
        /* Note: Logically we should use output trigger for keyboard slide,
         *       but input triggering is used to avoid turning display
         *       on if mce happens to restart while keyboard is open.
         *       As long as the slide input is not filtered, there is
         *       no harm in this. */
        .datapipe = &keyboard_slide_state_pipe,
        .input_cb = tklock_datapipe_keyboard_slide_input_state_cb,
    },

    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t tklock_datapipe_bindings =
{
    .module   = MODULE_NAME,
    .handlers = tklock_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void tklock_datapipe_init(void)
{
    mce_datapipe_init_bindings(&tklock_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void tklock_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&tklock_datapipe_bindings);
}

/* ========================================================================= *
 * AUTOLOCK AFTER DEVICELOCK STATE MACHINE
 * ========================================================================= */

/** Time limit for triggering autolock after display on */
static int64_t tklock_autolock_on_devlock_limit_trigger = 0;

/** Time limit for blocking autolock after lipstick startup */
static int64_t tklock_autolock_on_devlock_limit_block = 0;

/** Set autolock blocking limit after lipstick startup
 */
static void tklock_autolock_on_devlock_block(int duration_ms)
{
    tklock_autolock_on_devlock_limit_block =
        mce_lib_get_boot_tick() + duration_ms;
}

static void tklock_autolock_on_devlock_prime(void)
{
    /* While we want to trap only device lock that happens immediately
     * after unblanking the display, scheduling etc makes it difficult
     * to specify some exact figure for "immediately".
     *
     * Since devicelock timeouts have granularity of 1 minute, assume
     * that device locking that happens less than 60 seconds after
     * unblanking was related to what happened during display off time. */
    const int autolock_limit = 60 * 1000;

    /* Do nothing during startup */
    if( display_state_curr == MCE_DISPLAY_UNDEF )
        goto EXIT;

    /* Unprime if we are going to powered off state */
    switch( display_state_next ) {
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_ON:
        break;

    default:
        if( tklock_autolock_on_devlock_limit_trigger )
            mce_log(LL_DEBUG, "autolock after devicelock: unprimed");
        tklock_autolock_on_devlock_limit_trigger = 0;
        goto EXIT;
    }

    /* Prime if we are coming from powered off state */
    switch( display_state_curr ) {
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_ON:
        break;

    default:
        if( !tklock_autolock_on_devlock_limit_trigger )
            mce_log(LL_DEBUG, "autolock after devicelock: primed");
        tklock_autolock_on_devlock_limit_trigger =
            mce_lib_get_boot_tick() + autolock_limit;
        break;
    }

EXIT:
    return;
}

static void tklock_autolock_on_devlock_trigger(void)
{
    /* Device lock must be active */
    if( devicelock_state != DEVICELOCK_STATE_LOCKED )
        goto EXIT;

    /* Not while handling calls or alarms */
    switch( uiexception_type ) {
    case UIEXCEPTION_TYPE_CALL:
    case UIEXCEPTION_TYPE_ALARM:
        goto EXIT;

    default:
        break;
    }

    /* Autolock time limit must be set and not reached yet */
    if( !tklock_autolock_on_devlock_limit_trigger )
        goto EXIT;

    int64_t now = mce_lib_get_boot_tick();

    if( now >= tklock_autolock_on_devlock_limit_trigger )
        goto EXIT;

    /* Autolock must not be blocked by recent lipstick restart */
    if( now < tklock_autolock_on_devlock_limit_block )
        goto EXIT;

    /* We get here if: Device lock got applied right after
     * display was powered up.
     *
     * Most likely the device lock should have been applied
     * already when the display was off, but the devicelock
     * timer did not trigger while the device was suspended.
     *
     * It is also possible that the last used application
     * is still visible and active.
     *
     * Setting the tklock moves the application to background
     * and lockscreen / devicelock is shown instead.
     */

    mce_log(LL_DEBUG, "autolock after devicelock: triggered");
    mce_datapipe_request_tklock(TKLOCK_REQUEST_ON);
EXIT:
    return;
}

/* ========================================================================= *
 * LID_SENSOR
 * ========================================================================= */

/** Predicate for: Lid sensor is enabled
 *
 * It is assumed that any lid sensors present on the device are always
 * enabled by default. The mce setting just makes mce either ignore or
 * act on the change events that might or might not be coming in.
 *
 * @return true if lid state changes should be reacted to, false otherwise
 */
static bool tklock_lidsensor_is_enabled(void)
{
    return lid_sensor_enabled;
}

/** Initialize lid sensor tracking
 *
 * Note: This must be called before installing datapipe callbacks.
 */
static void tklock_lidsensor_init(void)
{
    /* Initialize state based on flag file presense */
    tklock_lid_sensor_is_working =
        (access(LID_SENSOR_IS_WORKING_FLAG_FILE, F_OK) == 0);

    mce_log(LL_DEVEL, "lid_sensor_is_working = %s",
            tklock_lid_sensor_is_working ? "true" : "false");

    /* Broadcast initial state */
    datapipe_exec_full(&lid_sensor_is_working_pipe,
                       GINT_TO_POINTER(tklock_lid_sensor_is_working));
}

/* ========================================================================= *
 * LID_LIGHT
 * ========================================================================= */

/** Convert lid light state to human readable string
 *
 * @param state  lid state value
 *
 * @return human readable name of the state
 */
static const char *tklock_lidlight_repr(tklock_lidlight_t state)
{
    const char *repr = "UNKNOWN";
    switch( state ) {
    case TKLOCK_LIDLIGHT_NA: repr = "NA"; break;
    case TKLOCK_LIDLIGHT_LO: repr = "LO"; break;
    case TKLOCK_LIDLIGHT_HI: repr = "HI"; break;
    default: break;
    }
    return repr;
}

/** Convert lux value to lid light state
 *
 * @param lux  lux value from light sensor
 *
 * @return corresponding lid state value
 */
static tklock_lidlight_t tklock_lidlight_from_lux(int lux)
{
    /* Sensor is off? */
    if( lux < 0 )
        return TKLOCK_LIDLIGHT_NA;

    /* Sensor does not see light? */
    if( lux <= filter_lid_als_limit )
        return TKLOCK_LIDLIGHT_LO;

    /* It is not completely dark */
    return TKLOCK_LIDLIGHT_HI;
}

/* ========================================================================= *
 * LID_FILTER
 * ========================================================================= */

/** Convert last seen lux value to lid light state
 *
 * @return lid state value corresponding with the latest reported lux value
 */
static tklock_lidlight_t tklock_lidfilter_map_als_state(void)
{
    return tklock_lidlight_from_lux(light_sensor_actual);
}

/** Predicate for: ALS data is used for filtering Lid sensor state
 *
 * @return true if filtering should be done, false otherwise
 */
static bool tklock_lidfilter_is_enabled(void)
{
    return tklock_lidsensor_is_enabled() && als_enabled && filter_lid_with_als;
}

/** Flag for: lid=closed + lux=low -> blank display */
static bool tklock_lidfilter_allow_close = false;

/** Allow/deny blanking if lid is closed in low light situation
 */
static void tklock_lidfilter_set_allow_close(bool allow)
{
    if( tklock_lidfilter_allow_close != allow ) {
        mce_log(LL_DEBUG, "allow_close: %s -> %s",
                tklock_lidfilter_allow_close ? "true" : "false",
                allow ? "true" : "false");
        tklock_lidfilter_allow_close = allow;
    }
}

/** Cached light sensor state */
static tklock_lidlight_t tklock_lidfilter_als_state = TKLOCK_LIDLIGHT_NA;

/** Set light sensor state
 *
 * @param state TKLOCK_LIDLIGHT_LO/HI/NA
 */
static void tklock_lidfilter_set_als_state(tklock_lidlight_t state)
{
    if( tklock_lidfilter_als_state != state ) {
        mce_log(LL_DEBUG, "als_state: %s -> %s",
                tklock_lidlight_repr(tklock_lidfilter_als_state),
                tklock_lidlight_repr(state));
        tklock_lidfilter_als_state = state;

        /* Check if futre lid close should be ignored or acted on */
        tklock_lidfilter_rethink_allow_close();
    }

    /* If we know we have lo/hi light, stop waiting for als data */
    if( tklock_lidfilter_als_state != TKLOCK_LIDLIGHT_NA )
        tklock_lidfilter_set_wait_for_light(false);

    /* If we know we have hi light, stop waiting for darkness*/
    if( tklock_lidfilter_als_state == TKLOCK_LIDLIGHT_LO )
        tklock_lidfilter_set_wait_for_dark(false);
}

/** Timer ID for: Stop waiting for lid close event */
static guint tklock_lidfilter_wait_for_close_id = 0;

/** Timer Callback for: Stop waiting for lid close event */
static gboolean tklock_lidfilter_wait_for_close_cb(gpointer aptr)
{
    (void)aptr;

    if( !tklock_lidfilter_wait_for_close_id )
        goto EXIT;

    mce_log(LL_DEBUG, "wait_close: timeout");
    tklock_lidfilter_wait_for_close_id = 0;

    tklock_lidfilter_set_als_state(TKLOCK_LIDLIGHT_NA);
    tklock_lidfilter_set_allow_close(false);

    /* Invalidate sensor data */
    datapipe_exec_full(&lid_sensor_actual_pipe,
                       GINT_TO_POINTER(COVER_UNDEF));

EXIT:
    return FALSE;
}

/** Predicate for: Waiting to see lid close event
 */
static bool tklock_lidfilter_get_wait_for_close(void)
{
    return tklock_lidfilter_wait_for_close_id != 0;
}

/** Start/stop waiting for lid close event
 *
 * @param state true when expecting lid close, false otherwise
 *
 * Used when als drop is noticed while lid is not closed.
 *
 * If lid closes soon after, blank screen.
 *
 * Otherwise disable blanking until some light is seen.
 */
static void tklock_lidfilter_set_wait_for_close(bool state)
{
    if( lid_sensor_actual != COVER_OPEN )
        state = false;

    if( display_state_next != MCE_DISPLAY_ON &&
        display_state_next != MCE_DISPLAY_DIM )
        state = false;

    if( state == tklock_lidfilter_get_wait_for_close() )
        goto EXIT;

    mce_log(LL_DEBUG, "wait_close: %s", state ? "start" : "cancel");

    if( state ) {
        tklock_lidfilter_wait_for_close_id =
            g_timeout_add(TKLOCK_LIDFILTER_SET_WAIT_FOR_CLOSE_DELAY,
                          tklock_lidfilter_wait_for_close_cb, 0);
    }
    else {
        g_source_remove(tklock_lidfilter_wait_for_close_id),
            tklock_lidfilter_wait_for_close_id = 0;
    }

EXIT:
    return;

}

/** Timer ID for: Stop waiting for ALS drop */
static guint tklock_lidfilter_wait_for_dark_id = 0;

/** Timer Callback for: Stop waiting for ALS drop
 */
static gboolean tklock_lidfilter_wait_for_dark_cb(gpointer aptr)
{
    (void)aptr;

    if( !tklock_lidfilter_wait_for_dark_id )
        goto EXIT;

    mce_log(LL_DEBUG, "wait_dark: timeout");
    tklock_lidfilter_wait_for_dark_id = 0;

    tklock_lidfilter_set_als_state(TKLOCK_LIDLIGHT_NA);

    /* Invalidate sensor data */
    datapipe_exec_full(&lid_sensor_actual_pipe,
                       GINT_TO_POINTER(COVER_UNDEF));

EXIT:
    return FALSE;
}

/** Predicate for: Waiting for ALS drop to occur
 */
static bool tklock_lidfilter_get_wait_for_dark(void)
{
    return tklock_lidfilter_wait_for_dark_id != 0;
}

/** Start/stop waiting for als drop event
 *
 * @param state true when expecting als drop, false otherwise
 *
 * Used when lid is closed in non-dark environment.
 *
 * If als level drops soon after, blank screen.
 *
 * Otherwise ignore lid state until it changes again.
 */
static void tklock_lidfilter_set_wait_for_dark(bool state)
{
    if( state == tklock_lidfilter_get_wait_for_dark() )
        goto EXIT;

    mce_log(LL_DEBUG, "wait_dark: %s", state ? "start" : "cancel");

    if( state ) {
        tklock_lidfilter_wait_for_dark_id =
            g_timeout_add(TKLOCK_LIDFILTER_SET_WAIT_FOR_DARK_DELAY,
                          tklock_lidfilter_wait_for_dark_cb, 0);
    }
    else {
        g_source_remove(tklock_lidfilter_wait_for_dark_id),
            tklock_lidfilter_wait_for_dark_id = 0;
    }

EXIT:
    return;

}

/** Timer ID for: Stop waiting for ALS data */
static guint tklock_lidfilter_wait_for_light_id = 0;

/** Timer callback for: Stop waiting for ALS data
 */
static gboolean tklock_lidfilter_wait_for_light_cb(gpointer aptr)
{
    (void)aptr;

    if( !tklock_lidfilter_wait_for_light_id )
        goto EXIT;

    mce_log(LL_DEBUG, "wait_light: timeout");
    tklock_lidfilter_wait_for_light_id = 0;

    tklock_lidfilter_set_als_state(tklock_lidfilter_map_als_state());

    tklock_lidpolicy_rethink();

EXIT:
    return FALSE;
}

/** Predicate for: Waiting for ALS data
 */
static bool tklock_lidfilter_get_wait_for_light(void)
{
    return tklock_lidfilter_wait_for_light_id != 0;
}

/** Start/stop waiting for als change event
 *
 * @param state true when expecting als change, false otherwise
 *
 * Used when lid is opened and we need to wait for als powerup.
 *
 * If als reports light soon after, unblank screen.
 *
 * Otherwise leave display state as it were.
 */

static void tklock_lidfilter_set_wait_for_light(bool state)
{
    if( state == tklock_lidfilter_get_wait_for_light() )
        goto EXIT;

    mce_log(LL_DEBUG, "wait_light: %s", state ? "start" : "cancel");

    if( state ) {
        tklock_lidfilter_wait_for_light_id =
            g_timeout_add(TKLOCK_LIDFILTER_SET_WAIT_FOR_LIGHT_DELAY,
                          tklock_lidfilter_wait_for_light_cb, 0);
        tklock_lidfilter_set_als_state(TKLOCK_LIDLIGHT_NA);
        tklock_lidpolicy_rethink();
    }
    else {
        g_source_remove(tklock_lidfilter_wait_for_light_id),
            tklock_lidfilter_wait_for_light_id = 0;
    }

EXIT:
    return;

}

/** React to end of temporary als poll periods
 */
static void tklock_lidfilter_rethink_als_poll(void)
{
    // when als polling stops, we must stop waiting for light level
    if( !light_sensor_polling ) {
        tklock_lidfilter_set_wait_for_light(false);
        tklock_lidfilter_rethink_als_state();
    }
}

/** Update allow-close flag based on display state and logical als state
 */
static void tklock_lidfilter_rethink_allow_close(void)
{
    switch( display_state_curr ) {
    case MCE_DISPLAY_POWER_UP:
      /* After display power cycling we  need to see a high lux value
       * before lid close can be used for display blanking again. */
      tklock_lidfilter_set_allow_close(false);

      /* Display power up while sensor is in closed state. Assume this
       * is due to user pressing power key and ignore the lid sensor
       * state until further changes are received. */
      if( lid_sensor_actual == COVER_CLOSED ) {
          mce_log(LL_DEVEL, "unblank while lid closed; ignore lid");
          datapipe_exec_full(&lid_sensor_actual_pipe,
                             GINT_TO_POINTER(COVER_UNDEF));
      }
      break;

    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_LPM_ON:
      if( tklock_lidfilter_als_state == TKLOCK_LIDLIGHT_HI )
            tklock_lidfilter_set_allow_close(true);
        break;

    default:
        break;
    }
}

/** Re-evaluate reaction to lid sensor state
 */
static void tklock_lidfilter_rethink_lid_state(void)
{
    if( !tklock_lidfilter_is_enabled() ) {
        tklock_lidfilter_set_wait_for_dark(false);
        tklock_lidfilter_set_wait_for_light(false);
        tklock_lidfilter_set_wait_for_close(false);
        goto EXIT;
    }

    /* Keep ALS powered up for a while after lid state change */
    if( lid_sensor_actual != COVER_UNDEF ) {
        datapipe_exec_full(&light_sensor_poll_request_pipe,
                           GINT_TO_POINTER(TRUE));
    }

    switch( lid_sensor_actual ) {
    case COVER_OPEN:
        tklock_lidfilter_set_wait_for_dark(false);
        tklock_lidfilter_set_wait_for_light(true);
        break;

    case COVER_CLOSED:
        tklock_lidfilter_set_wait_for_light(false);
        if( tklock_lidfilter_get_wait_for_close() )
            tklock_lidfilter_set_wait_for_close(false);
        else
            tklock_lidfilter_set_wait_for_dark(true);
        break;

    default:
        tklock_lidfilter_set_wait_for_dark(false);
        tklock_lidfilter_set_wait_for_light(false);
        break;
    }
EXIT:
    tklock_lidfilter_rethink_als_state();
}

/** Re-evaluate reaction to ambient light sensor state
 *
 * Augment lid sensor data with als data so that:
 *
 * - lid close followed by darkness  -> blank
 * - darkness followed by lid close  -> blank
 * - lid open followed by light seen -> unblank
 *
 * Timers are used to set maximum wait periods for the "followed by"
 * events. In case of timeout the lid state is ignored temporarily or
 * until the next time it changes.
 */
static void tklock_lidfilter_rethink_als_state(void)
{
    /* Initialize to "ALS is powered down" value */
    static int prev = -1;

    /* Evaluate sensor state we ought to be in
     * based on current lux value */

    if( tklock_lidfilter_is_enabled() ) {
        switch( tklock_lidfilter_map_als_state() ) {
        default:
        case TKLOCK_LIDLIGHT_NA:
            /* Ignore: Sensor down time */
            break;

        case TKLOCK_LIDLIGHT_LO:
            /* Handle: Darkness */
            if( tklock_lidfilter_get_wait_for_dark() ) {
                tklock_lidfilter_set_als_state(TKLOCK_LIDLIGHT_LO);
            }
            else if( tklock_lidfilter_get_wait_for_light() ) {
                tklock_lidfilter_set_als_state(TKLOCK_LIDLIGHT_NA);
            }
            else {
                tklock_lidfilter_set_als_state(TKLOCK_LIDLIGHT_LO);
                tklock_lidfilter_set_wait_for_close(true);
            }
            break;

        case TKLOCK_LIDLIGHT_HI:
            /* Handle: Light */
            if( tklock_lidfilter_get_wait_for_light() ) {
                /* During als power up we might see the previously
                 * seen high light value, but rise in level means
                 * the sensor is up and sees light -> we can stop
                 * waiting */
                if( prev < light_sensor_actual )
                    tklock_lidfilter_set_als_state(TKLOCK_LIDLIGHT_HI);
                else
                    tklock_lidfilter_set_als_state(TKLOCK_LIDLIGHT_NA);
            }
            else if( tklock_lidfilter_get_wait_for_dark() ) {
                tklock_lidfilter_set_als_state(TKLOCK_LIDLIGHT_NA);
            }
            else {
                tklock_lidfilter_set_als_state(TKLOCK_LIDLIGHT_HI);
            }
            break;
        }
    }

    /* Update previous value unless ALS is powered down */
    if( light_sensor_actual >= 0 )
        prev = light_sensor_actual;

    tklock_lidpolicy_rethink();
}

/* ========================================================================= *
 * LID_POLICY
 * ========================================================================= */

/** Evaluate lid policy state based on lid and light sensor states
 *
 * While lid cover sensor use is enabled, by default:
 *
 * - Closing lid blanks the screen and activates lockscreen
 * - Opening lid unblanks the screen
 *
 * Settings can be used to:
 *
 * - Select whether lid sensor state should be applied as such or
 *   augmented by tracking ambient light sensor based heuristics
 *   to avoid possible false positives from the lid sensor itself
 *
 * - Select what actions are taken when the policy change occurs
 */
static void tklock_lidpolicy_rethink(void)
{
    /* We have not seen COVER_CLOSED state yet */
    static bool lid_has_been_closed = false;

    /* Assume lid is neither open nor closed */
    cover_state_t action = COVER_UNDEF;

    /* Evaluate required policy state */
    if( !tklock_lidsensor_is_enabled() ) {
        /* The lid sensor is not used */
    }
    else if( !tklock_lid_sensor_is_working ) {
        /* No policy decisions until the sensor is known to work */
    }
    else if( !tklock_lidfilter_is_enabled() )
    {
        /* No filtering -> use sensor state as is */
        action = lid_sensor_actual;
    }
    else if( lid_sensor_actual == COVER_CLOSED &&
        tklock_lidfilter_als_state == TKLOCK_LIDLIGHT_LO ) {
        if( tklock_lidfilter_allow_close )
            action = COVER_CLOSED;
    }
    else if( lid_sensor_actual == COVER_OPEN &&
             tklock_lidfilter_als_state == TKLOCK_LIDLIGHT_HI ) {
        action = COVER_OPEN;
    }

    /* To avoid unblanking on mce restart while lid is open, stay in
     * undecided state until we have observed lid closed state too. */
    if( action == COVER_OPEN && !lid_has_been_closed )
        action = COVER_UNDEF;

    /* Skip the rest if there is no change */
    if( lid_sensor_filtered == action )
        goto EXIT;

    mce_log(LL_DEBUG, "lid policy: %s -> %s",
            cover_state_repr(lid_sensor_filtered),
            cover_state_repr(action));

    /* First make the policy decision known */
    datapipe_exec_full(&lid_sensor_filtered_pipe,
                       GINT_TO_POINTER(action));

    /* Then execute the required actions */
    switch( action ) {
    case COVER_CLOSED:
        /* Allow unblanking when lid is opened again. */
        lid_has_been_closed = true;

        /* Blank display + lock ui */
        if( tklock_lid_close_actions != LID_CLOSE_ACTION_DISABLED ) {
            mce_log(LL_DEVEL, "lid closed - blank");
            mce_datapipe_request_display_state(MCE_DISPLAY_OFF);
        }

        if( tklock_lid_close_actions == LID_CLOSE_ACTION_TKLOCK ) {
            mce_log(LL_DEBUG, "lid closed - tklock");
            mce_datapipe_request_tklock(TKLOCK_REQUEST_ON);
        }
        break;

    case COVER_OPEN:
        /* Unblank display + unlock ui */
        if( tklock_lid_open_actions != LID_OPEN_ACTION_DISABLED ) {
            mce_log(LL_DEVEL, "lid open - unblank");
            mce_datapipe_request_display_state(MCE_DISPLAY_ON);
        }

        if( tklock_lid_open_actions == LID_OPEN_ACTION_TKUNLOCK ) {
            mce_log(LL_DEBUG, "lid open - untklock");
            mce_datapipe_request_tklock(TKLOCK_REQUEST_OFF);
        }
        break;

    default:
        mce_log(LL_DEBUG, "lid ignored");
        /* NOP */
        break;
    }

EXIT:
    return;
}

/* ========================================================================= *
 * KEYBOARD SLIDE STATE MACHINE
 * ========================================================================= */

static void tklock_keyboard_slide_opened(void)
{
    /* In any case opening the kbd slide will cancel
     * other autorelock triggers */
    if( autorelock_trigger != AUTORELOCK_NO_TRIGGERS ) {
        mce_log(LL_DEBUG, "autorelock canceled: kbd slide opened");
        autorelock_trigger = AUTORELOCK_NO_TRIGGERS;
    }

    /* Display must be off */
    switch( display_state_next ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        goto EXIT;

    default:
        break;
    }

    /* Check if actions are wanted */
    switch( tklock_kbd_open_trigger ) {
    default:
    case KBD_OPEN_TRIGGER_NEVER:
        goto EXIT;

    case KBD_OPEN_TRIGGER_ALWAYS:
        break;

    case KBD_OPEN_TRIGGER_NO_PROXIMITY:
        if( proximity_sensor_actual != COVER_OPEN ||
            lid_sensor_filtered == COVER_CLOSED )
            goto EXIT;
        break;
    }

    /* Check what actions are wanted */
    if( tklock_kbd_open_actions != LID_OPEN_ACTION_DISABLED ) {
        mce_log(LL_DEVEL, "kbd slide open - unblank");
        mce_datapipe_request_display_state(MCE_DISPLAY_ON);
    }

    if( tklock_kbd_open_actions == LID_OPEN_ACTION_TKUNLOCK ) {
        mce_log(LL_DEBUG, "kbd slide open - untklock");
        mce_datapipe_request_tklock(TKLOCK_REQUEST_OFF);
    }

    /* Mark down we unblanked due to keyboard open */
    mce_log(LL_DEBUG, "autorelock primed: on kbd slide close");
    autorelock_trigger = AUTORELOCK_KBD_SLIDE;

EXIT:
    return;
}

/** Wait for proximity sensor -callback for keyboard slide handling
 *
 * @param aptr unused
 */
static void tklock_keyboard_slide_opened_cb(gpointer aptr)
{
    (void)aptr;

    /* Slide still open? */
    if( keyboard_slide_input_state == COVER_OPEN ) {
        tklock_keyboard_slide_opened();
    }
}

static void tklock_keyboard_slide_closed(void)
{
    /* Must not blank during active alarms / calls */
    if( uiexception_type & (UIEXCEPTION_TYPE_CALL | UIEXCEPTION_TYPE_ALARM) )
        goto EXIT;

    /* Check if actions are wanted */
    switch( tklock_kbd_close_trigger ) {
    default:
    case KBD_CLOSE_TRIGGER_NEVER:
        goto EXIT;

    case KBD_CLOSE_TRIGGER_ALWAYS:
        break;

    case KBD_CLOSE_TRIGGER_AFTER_OPEN:
        if( autorelock_trigger != AUTORELOCK_KBD_SLIDE )
            goto EXIT;

        mce_log(LL_DEBUG, "autorelock triggered: kbd slide closed");
        autorelock_trigger = AUTORELOCK_NO_TRIGGERS;
        break;
    }

    /* Check what actions are wanted */
    if( tklock_kbd_close_actions != LID_CLOSE_ACTION_DISABLED ) {
        mce_log(LL_DEVEL, "kbd slide closed - blank");
        mce_datapipe_request_display_state(MCE_DISPLAY_OFF);
    }

    if( tklock_kbd_close_actions == LID_CLOSE_ACTION_TKLOCK ) {
        mce_log(LL_DEBUG, "kbd slide closed - tklock");
        mce_datapipe_request_tklock(TKLOCK_REQUEST_ON);
    }

EXIT:
    /* In any case closing the kbd slide will cancel autorelock triggers */
    if( autorelock_trigger != AUTORELOCK_NO_TRIGGERS ) {
        mce_log(LL_DEBUG, "autorelock canceled: kbd slide closed");
        autorelock_trigger = AUTORELOCK_NO_TRIGGERS;
    }

    return;
}

static void tklock_keyboard_slide_rethink(void)
{
    switch( keyboard_slide_input_state ) {
    case COVER_OPEN:
        /* Delay processing until proximity sensor state is known */
        common_on_proximity_schedule(MODULE_NAME,
                                     tklock_keyboard_slide_opened_cb, 0);
        break;

    case COVER_CLOSED:
        tklock_keyboard_slide_closed();
        break;

    default:
        break;
    }
}

/* ========================================================================= *
 * AUTOLOCK STATE MACHINE
 *
 * Automatically apply tklock when
 * 1) display has been off for tklock_autolock_delay ms
 * 2) autolocking is enabled
 * 3) we are not handling call/alarm/etc
 *
 * ========================================================================= */

static int64_t tklock_autolock_tick = MAX_TICK;
static mce_hbtimer_t *tklock_autolock_timer = 0;

static void tklock_autolock_evaluate(void)
{
    // display must be currently off
    if( display_state_curr != MCE_DISPLAY_OFF )
        goto EXIT;

    // tklock unset
    if( tklock_datapipe_in_tklock_submode() )
        goto EXIT;

    // autolocking enabled
    if( !tk_autolock_enabled )
        goto EXIT;

    // not handling calls, alarms, etc
    if( uiexception_type != UIEXCEPTION_TYPE_NONE )
        goto EXIT;

    // if device lock is on, apply tklock immediately
    if( devicelock_state == DEVICELOCK_STATE_LOCKED )
        goto LOCK;

    // autolock delay to passed
    if( mce_lib_get_boot_tick() < tklock_autolock_tick )
        goto EXIT;

LOCK:
    mce_log(LL_DEBUG, "autolock applied");
    tklock_ui_set_enabled(true);

EXIT:
    return;
}

static gboolean tklock_autolock_cb(gpointer aptr)
{
    (void)aptr;

    tklock_autolock_tick = MIN_TICK;
    mce_log(LL_DEBUG, "autolock timer triggered");
    tklock_autolock_evaluate();

    return FALSE;
}

static void tklock_autolock_disable(void)
{
    tklock_autolock_tick = MAX_TICK;

    if( !mce_hbtimer_is_active(tklock_autolock_timer) )
        goto EXIT;

    mce_hbtimer_stop(tklock_autolock_timer);
    mce_log(LL_DEBUG, "autolock timer stopped");

EXIT:
    return;
}

static void tklock_autolock_enable(void)
{
    if( mce_hbtimer_is_active(tklock_autolock_timer) )
        goto EXIT;

    int delay = mce_clip_int(MINIMUM_AUTOLOCK_DELAY,
                             MAXIMUM_AUTOLOCK_DELAY,
                             tklock_autolock_delay);

    tklock_autolock_tick = mce_lib_get_boot_tick() + delay;

    mce_hbtimer_set_period(tklock_autolock_timer, delay);
    mce_hbtimer_start(tklock_autolock_timer);
    mce_log(LL_DEBUG, "autolock timer started (%d ms)", delay);

EXIT:
    return;
}

static void tklock_autolock_rethink(void)
{
    if( display_state_next != MCE_DISPLAY_OFF ) {
        // not in OFF or moving away from OFF
        tklock_autolock_disable();
    }
    else if( display_state_next != display_state_curr ) {
        // making transition to OFF
        tklock_autolock_enable();
    }
    else {
        // stable display OFF state
        tklock_autolock_evaluate();
    }
}

static void
tklock_autolock_init(void)
{
    tklock_autolock_timer = mce_hbtimer_create("autolock-timer",
                                               tklock_autolock_delay,
                                               tklock_autolock_cb, 0);
}

static void
tklock_autolock_quit(void)
{
    mce_hbtimer_delete(tklock_autolock_timer),
        tklock_autolock_timer = 0;
}

/* ========================================================================= *
 * PROXIMITY LOCKING STATE MACHINE
 *
 * Automatically apply tklock when
 * 1) display has been off for PROXLOC_DELAY_MS
 * 2) proximity sensor is covered
 * 3) we are not handling call/alarm/etc
 * ========================================================================= */

/** Proximity sensor on-demand tag for proximity locking purposes */
#define PROXLOC_ON_DEMAND_TAG "proxlock"

/** Delay for enabling tklock from display off when proximity is covered */
#define PROXLOC_DELAY_MS (3000)

static int64_t tklock_proxlock_tick = MAX_TICK;
static guint   tklock_proxlock_id   = 0;

static void tklock_proxlock_evaluate(void)
{
    // display must be currently off
    if( display_state_curr != MCE_DISPLAY_OFF )
        goto EXIT;

    // tklock unset
    if( tklock_datapipe_in_tklock_submode() )
        goto EXIT;

    // proximity covered
    if( proximity_sensor_effective != COVER_CLOSED )
        goto EXIT;

    // not handling call, alarm, etc
    if( uiexception_type != UIEXCEPTION_TYPE_NONE )
        goto EXIT;

    // proximity lock delay passed
    if( mce_lib_get_boot_tick() < tklock_proxlock_tick )
        goto EXIT;

    // lock
    mce_log(LL_DEBUG, "proxlock applied");
    tklock_ui_set_enabled(true);

EXIT:
    return;
}

static gboolean tklock_proxlock_cb(gpointer aptr)
{
    (void)aptr;

    if( tklock_proxlock_id ) {
        tklock_proxlock_id = 0;
        tklock_proxlock_tick = MIN_TICK;

        mce_log(LL_DEBUG, "proxlock timer triggered");
        tklock_proxlock_evaluate();

        /* Timer did not get re-activated, ps not needed anymore */
        if( !tklock_proxlock_id )
            datapipe_exec_full(&proximity_sensor_required_pipe,
                               PROXIMITY_SENSOR_REQUIRED_REM
                               PROXLOC_ON_DEMAND_TAG);
    }
    return false;
}

static void tklock_proxlock_disable(void)
{
    tklock_proxlock_tick = MAX_TICK;

    if( tklock_proxlock_id ) {
        g_source_remove(tklock_proxlock_id), tklock_proxlock_id = 0;
        mce_log(LL_DEBUG, "proxlock timer stopped");

        /* Timer canceled, ps not needed anymore */
        datapipe_exec_full(&proximity_sensor_required_pipe,
                           PROXIMITY_SENSOR_REQUIRED_REM
                           PROXLOC_ON_DEMAND_TAG);
    }
}

static void tklock_proxlock_enable(void)
{
    int delay = PROXLOC_DELAY_MS;

    if( !tklock_proxlock_id ) {
        tklock_proxlock_tick = mce_lib_get_boot_tick() + delay;
        tklock_proxlock_id = g_timeout_add(delay, tklock_proxlock_cb, 0);
        mce_log(LL_DEBUG, "proxlock timer started (%d ms)", delay);
        /* Timer started, ps is needed */
        datapipe_exec_full(&proximity_sensor_required_pipe,
                           PROXIMITY_SENSOR_REQUIRED_ADD
                           PROXLOC_ON_DEMAND_TAG);
    }
}

static void tklock_proxlock_resume(void)
{
    /* Do we have a timer to re-evaluate? */
    if( !tklock_proxlock_id )
        goto EXIT;

    /* Clear old timer */
    g_source_remove(tklock_proxlock_id), tklock_proxlock_id = 0;

    int64_t now = mce_lib_get_boot_tick();

    if( now >= tklock_proxlock_tick ) {
        /* Opportunistic triggering on resume */
        mce_log(LL_DEBUG, "proxlock time passed while suspended");
        tklock_proxlock_tick = MIN_TICK;
        tklock_proxlock_evaluate();
    }
    else {
        /* Re-calculate wakeup time */
        int delay = (int)(tklock_proxlock_tick - now);
        mce_log(LL_DEBUG, "adjusting proxlock time after resume (%d ms)", delay);
        tklock_proxlock_id = g_timeout_add(delay, tklock_proxlock_cb, 0);
    }

    /* Timer canceled, ps not needed anymore */
    if( !tklock_proxlock_id )
        datapipe_exec_full(&proximity_sensor_required_pipe,
                           PROXIMITY_SENSOR_REQUIRED_REM
                           PROXLOC_ON_DEMAND_TAG);

EXIT:
    return;
}

static void tklock_proxlock_rethink(void)
{
    if( display_state_next != MCE_DISPLAY_OFF ) {
        // not in OFF or moving away from OFF
        tklock_proxlock_disable();
    }
    else if( display_state_next != display_state_curr ) {
        // making transition to OFF
        tklock_proxlock_enable();
    }
    else {
        // check if proxlock conditions are met
        tklock_proxlock_evaluate();
    }
}

/* ========================================================================= *
 * UI EXCEPTION HANDLING STATE MACHINE
 * ========================================================================= */

typedef struct
{
    uiexception_type_t mask;
    uiexception_type_t last;
    display_state_t     display;
    bool                tklock;
    devicelock_state_t devicelock;
    bool                insync;
    bool                restore;
    bool                was_called;
    int64_t             linger_tick;
    guint               linger_id;
    int64_t             notif_tick;
    guint               notif_id;
} exception_t;

static exception_t exdata =
{
    .mask        = UIEXCEPTION_TYPE_NONE,
    .last        = UIEXCEPTION_TYPE_NONE,
    .display     = MCE_DISPLAY_UNDEF,
    .tklock      = false,
    .devicelock  = DEVICELOCK_STATE_UNDEFINED,
    .insync      = true,
    .restore     = true,
    .was_called  = false,
    .linger_tick = MIN_TICK,
    .linger_id   = 0,
    .notif_tick  = MIN_TICK,
    .notif_id    = 0,
};

static uiexception_type_t topmost_active(uiexception_type_t mask)
{
    /* Assume UI side priority is:
     * 1. notification dialogs
     * 2. alarm ui
     * 3. call ui
     * 4. rest
     */

    static const uiexception_type_t pri[] = {
        UIEXCEPTION_TYPE_NOTIF,
        UIEXCEPTION_TYPE_ALARM,
        UIEXCEPTION_TYPE_CALL,
        UIEXCEPTION_TYPE_LINGER,
        UIEXCEPTION_TYPE_NOANIM,
        0
    };

    for( size_t i = 0; pri[i]; ++i ) {
        if( mask & pri[i] )
            return pri[i];
    }

    return UIEXCEPTION_TYPE_NONE;
}

static void  tklock_uiexception_sync_to_datapipe(void)
{
    uiexception_type_t in_pipe = datapipe_get_gint(uiexception_type_pipe);
    uiexception_type_t active  = topmost_active(exdata.mask);

    if( in_pipe != active ) {
        datapipe_exec_full(&uiexception_type_pipe,
                           GINT_TO_POINTER(active));
    }
}

/** Do not restore display/tklock state at the end of exceptional ui state
 *
 * @param force  true for unconditionally canceling the state restore; or
 *               false for canceling only if neither tklock nor devicelock
 *               is active
 */
static bool tklock_uiexception_deny_state_restore(bool force, const char *cause)
{
    bool changed = false;

    // must have restore to deny
    if( !exdata.restore || !exdata.mask )
        goto EXIT;

    // must be forced or unlocked
    if( !force && (exdata.tklock || exdata.devicelock) )
        goto EXIT;

    mce_log(LL_DEVEL, "%s; state restore disabled", cause);

    exdata.restore = false;
    changed = true;

EXIT:
    return changed;
}

static void tklock_uiexception_rethink(void)
{
    static display_state_t display_prev = MCE_DISPLAY_UNDEF;
    static call_state_t call_state_prev = CALL_STATE_INVALID;
    static uiexception_type_t active_prev = UIEXCEPTION_TYPE_NONE;

    bool                activate        = false;
    bool                blank           = false;
    uiexception_type_t active          = topmost_active(exdata.mask);
    bool                proximity_blank = false;

    /* Make sure "proximityblanking" state gets cleared if display
     * changes to non-off state. */
    if( display_prev != display_state_curr ) {
        switch( display_state_curr ) {
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
            datapipe_exec_full(&proximity_blanked_pipe,
                               GINT_TO_POINTER(false));
            break;
        }
    }

    if( !active ) {
        mce_log(LL_DEBUG, "UIEXCEPTION_TYPE_NONE");
        goto EXIT;
    }

    /* Track states that have gotten topmost before linger */
    if( active != UIEXCEPTION_TYPE_LINGER )
        exdata.last  = UIEXCEPTION_TYPE_NONE;
    else if( active_prev != UIEXCEPTION_TYPE_LINGER )
        exdata.last  = active_prev;

    /* Special case: tklock changes during incoming calls */
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
            if( exdata.was_called && tklock_datapipe_in_tklock_submode() ) {
                mce_log(LL_NOTICE, "stopping to ignore tklock removal");
                exdata.was_called = false;
            }
            break;

        default:
            break;
        }
    }

    /* Canceling state restore due to tklock changes */
    if( tklock_datapipe_in_tklock_submode() ) {
        // getting locked does not cancel state restore
        exdata.tklock = true;
    }
    else if( exdata.tklock && !exdata.was_called && exdata.restore ) {
        // but getting unlocked outside incoming call does
        mce_log(LL_NOTICE, "DISABLING STATE RESTORE; tklock out of sync");
        exdata.restore = false;
    }

    /* Canceling state restore due to device lock changes */
    if( devicelock_state == DEVICELOCK_STATE_LOCKED ) {
        // getting locked does not cancel state restore
        exdata.devicelock = devicelock_state;
    }
    else if( exdata.devicelock != devicelock_state && exdata.restore ) {
        // but getting unlocked  does
        mce_log(LL_NOTICE, "DISABLING STATE RESTORE; devicelock out of sync");
        exdata.restore = false;
    }

    /* Re-sync on incoming call */
    if( call_state_prev != call_state ) {
        if( !exdata.insync && call_state == CALL_STATE_RINGING ) {
            mce_log(LL_NOTICE, "incoming call; assuming in sync again");
            exdata.insync = true;
        }
        call_state_prev = call_state;
    }

    // re-sync on display on transition
    if( display_prev != display_state_curr ) {
        mce_log(LL_DEBUG, "display state: %s -> %s",
                display_state_repr(display_prev),
                display_state_repr(display_state_curr));
        if( display_state_curr == MCE_DISPLAY_ON ) {
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
    case UIEXCEPTION_TYPE_NOANIM:
        /* The noanim exception is used only during display power up.
         * It also has the lowest priority, which means that if it
         * ever gets on top of the exception stack, we need to disable
         * state restore. */
        if( exdata.restore ) {
            mce_log(LL_DEBUG, "noanim exception state; disable state restore");
            exdata.restore = false;
        }
        break;

    case UIEXCEPTION_TYPE_NOTIF:
        mce_log(LL_DEBUG, "UIEXCEPTION_TYPE_NOTIF");
        activate = true;
        break;

    case UIEXCEPTION_TYPE_ALARM:
        mce_log(LL_DEBUG, "UIEXCEPTION_TYPE_ALARM");
        activate = true;
        break;

    case UIEXCEPTION_TYPE_CALL:
        mce_log(LL_DEBUG, "UIEXCEPTION_TYPE_CALL");
        if( call_state == CALL_STATE_RINGING ) {
            mce_log(LL_DEBUG, "call=RINGING; activate");
            activate = true;
        }
        else if( audio_route != AUDIO_ROUTE_HANDSET ) {
            mce_log(LL_DEBUG, "audio!=HANDSET; activate");
            activate = true;
        }
        else if( proximity_sensor_effective == COVER_CLOSED ) {
            mce_log(LL_DEBUG, "proximity=COVERED; blank");
            /* blanking due to proximity sensor */
            blank = proximity_blank = true;
        }
        else {
            mce_log(LL_DEBUG, "proximity=NOT-COVERED; activate");
            activate = true;
        }
        break;

    case UIEXCEPTION_TYPE_LINGER:
        mce_log(LL_DEBUG, "UIEXCEPTION_TYPE_LINGER");
        activate = true;
        break;

    case UIEXCEPTION_TYPE_NONE:
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
        if( display_state_curr != MCE_DISPLAY_OFF ) {
            /* expose blanking due to proximity via datapipe */
            if( proximity_blank ) {
                mce_log(LL_DEVEL, "display proximity blank");
                datapipe_exec_full(&proximity_blanked_pipe,
                                   GINT_TO_POINTER(true));
            }
            else {
                mce_log(LL_DEBUG, "display blank");
            }
            mce_datapipe_request_display_state(MCE_DISPLAY_OFF);
        }
        else {
            mce_log(LL_DEBUG, "display already blanked");
        }
    }
    else if( activate ) {
        if( display_prev == MCE_DISPLAY_ON &&
            display_state_curr != MCE_DISPLAY_ON ) {
            /* Assume: dim/blank timer took over the blanking.
             * Disable this state machine until display gets
             * turned back on */
            mce_log(LL_NOTICE, "AUTO UNBLANK DISABLED; display out of sync");
            exdata.insync = false;

            /* Disable state restore, unless we went out of
             * sync during call ui handling */
            if( exdata.restore && active != UIEXCEPTION_TYPE_CALL ) {
                exdata.restore = false;
                mce_log(LL_NOTICE, "DISABLING STATE RESTORE; display out of sync");
            }
        }
        else if( !exdata.insync ) {
            mce_log(LL_NOTICE, "NOT UNBLANKING; still out of sync");
        }
        else if( lid_sensor_filtered == COVER_CLOSED ) {
            mce_log(LL_NOTICE, "NOT UNBLANKING; lid covered");
        }
        else if( proximity_sensor_effective != COVER_OPEN ) {
            mce_log(LL_NOTICE, "NOT UNBLANKING; proximity covered");
        }
        else if( display_state_curr != MCE_DISPLAY_ON ) {
            mce_log(LL_DEBUG, "display unblank");
            mce_datapipe_request_display_state(MCE_DISPLAY_ON);
        }
    }

    /* Make sure "proximityblanking" state gets cleared if display
     * state is no longer controlled by this state machine. */
    if( !exdata.insync ) {
        datapipe_exec_full(&proximity_blanked_pipe,
                           GINT_TO_POINTER(false));
    }

EXIT:
    display_prev = display_state_curr;

    return;
}

static void tklock_uiexception_cancel(void)
{
    if( exdata.notif_id ) {
        g_source_remove(exdata.notif_id),
            exdata.notif_id = 0;
    }

    if( exdata.linger_id ) {
        g_source_remove(exdata.linger_id),
            exdata.linger_id = 0;
    }

    exdata.mask        = UIEXCEPTION_TYPE_NONE;
    exdata.last        = UIEXCEPTION_TYPE_NONE;
    exdata.display     = MCE_DISPLAY_UNDEF;
    exdata.tklock      = false;
    exdata.devicelock  = DEVICELOCK_STATE_UNDEFINED;
    exdata.insync      = true;
    exdata.restore     = true;
    exdata.was_called  = false;
    exdata.linger_tick = MIN_TICK;
    exdata.linger_id   = 0;
    exdata.notif_tick  = MIN_TICK,
    exdata.notif_id    = 0;
}

static void tklock_uiexception_finish(void)
{
    /* operate on copy of data, in case the data
     * pipe operations cause feedback */
    exception_t exx = exdata;
    tklock_uiexception_cancel();

    /* update exception data pipe first */
    tklock_uiexception_sync_to_datapipe();

    /* check if restoring has been blocked */
    if( !exx.restore )
        goto EXIT;

    /* then flip the tklock  back on? Note that we
     * we do not unlock no matter what. */
    if( exx.tklock ) {
        mce_datapipe_request_tklock(TKLOCK_REQUEST_ON);
    }

    /* and finally the display data pipe */
    switch( exx.display ) {
    default:
        /* If the display was not clearly ON when exception started,
         * turn it OFF after exceptions are over. */
        mce_datapipe_request_display_state(MCE_DISPLAY_OFF);
        break;

    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        /* Unblank only if proximity sensor is not covered when
         * the linger time has passed.
         *
         * Note: Because linger times are relatively short,
         * we use raw sensor data here instead of the filtered
         * proximity_sensor_effective that is normally used
         * with unblanking policies. */
        if( proximity_sensor_actual != COVER_OPEN ||
            lid_sensor_filtered == COVER_CLOSED )
            break;

        mce_datapipe_request_display_state(exx.display);
        break;
    }
EXIT:
    return;
}

static gboolean tklock_uiexception_linger_cb(gpointer aptr)
{
    (void) aptr;

    if( !exdata.linger_id )
        goto EXIT;

    /* mark timer inactive */
    exdata.linger_id = 0;

    /* Ignore unless linger bit and only linger bit is set */
    if( exdata.mask != UIEXCEPTION_TYPE_LINGER ) {
        mce_log(LL_WARN, "spurious linger timeout");
        goto EXIT;
    }

    mce_log(LL_DEBUG, "linger timeout");

    /* Disable state restore if lockscreen is active and interaction
     * expected after linger. */
    if( display_state_next == MCE_DISPLAY_ON &&
        tklock_ui_is_enabled() && interaction_expected ) {
        if( exdata.last == UIEXCEPTION_TYPE_CALL ) {
            /* End of call is exception within exception because
             * the call ui can be left on top of the lockscreen and
             * there is no way to know whether that happened or not.
             *
             * Do not disable state restore and assume the linger
             * time has been long enough for the user to have done
             * significant enough actions during it to have disabled
             * the state restore in other ways.
             */
        }
        else {
            tklock_uiexception_deny_state_restore(true,
                                                  "interaction during linger");
        }
    }

    tklock_uiexception_finish();

EXIT:
    return FALSE;
}

static void tklock_uiexception_end(uiexception_type_t type, int64_t linger)
{
    if( !(exdata.mask & type) )
        goto EXIT;

    int64_t now = mce_lib_get_boot_tick();

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
            exdata.mask |= UIEXCEPTION_TYPE_LINGER;
            exdata.linger_id = g_timeout_add(delay, tklock_uiexception_linger_cb, 0);
        }
        else {
            mce_log(LL_DEBUG, "finish without linger");
            tklock_uiexception_finish();
        }
    }

    tklock_uiexception_sync_to_datapipe();

EXIT:
    return;
}

static void tklock_uiexception_begin(uiexception_type_t type, int64_t linger)
{
    if( !exdata.mask ) {
        /* reset existing stats */
        tklock_uiexception_cancel();

        /* save display, tklock and device lock states */
        exdata.display    = display_state_next;
        exdata.tklock     = tklock_datapipe_in_tklock_submode();
        exdata.devicelock = devicelock_state;

        /* initially insync, restore state at end */
        exdata.insync      = true;
        exdata.restore     = (type != UIEXCEPTION_TYPE_NOANIM);

        /* Display should be on after booting up to user mode.
         * If something like "charger connected" notification gets
         * triggered during bootup, we need to disable state restore
         * in order not to cause return to some non-intentional
         * transient state.
         */
        if( exdata.restore &&
            init_done != TRISTATE_TRUE &&
            system_state == MCE_SYSTEM_STATE_USER ) {
            mce_log(LL_DEVEL, "suppressing display state restore");
            exdata.restore = false;
        }
    }

    exdata.mask &= ~UIEXCEPTION_TYPE_LINGER;
    exdata.mask |= type;

    int64_t now = mce_lib_get_boot_tick();

    linger += now;

    if( exdata.linger_tick < linger )
        exdata.linger_tick = linger;

    if( exdata.linger_id )
        g_source_remove(exdata.linger_id), exdata.linger_id = 0;

    tklock_uiexception_sync_to_datapipe();
}

/* ========================================================================= *
 * LOW POWER MODE UI STATE MACHINE
 * ========================================================================= */

/** Bitmap of automatic lpm triggering modes */
static gint  tklock_lpmui_triggering = MCE_DEFAULT_TK_LPMUI_TRIGGERING;
static guint tklock_lpmui_triggering_setting_id = 0;

/* Proximity change time limits for low power mode triggering */
enum
{
    /** Minimum time [ms] the proximity needs to be in stable state */
    LPMUI_LIM_STABLE = 3000,

    /** Maximum time [ms] in between proximity changes */
    LPMUI_LIM_CHANGE = 1500,
};

/** The latest lpm ui state that was broadcast; initialized to invalid value */
static int tklock_lpmui_state_signaled = -1;

/** The currently wanted lpm ui state; initialized to invalid value */
static int tklock_lpmui_state_wanted   = -1;

/** Set lpm ui state
 *
 * Broadcast changes over D-Bus
 *
 * @param enable true if lpm ui should be enabled, false otherwise
 */
static void tklock_lpmui_set_state(bool enable)
{
    if( tklock_lpmui_state_wanted == enable )
        goto EXIT;

    tklock_lpmui_state_wanted = enable;

    if( enable ) {
        /* The LPM lockscreen is activated when both tklock and
         * lpm state are set. To avoid going through normal
         * lockscreen state, send lpm indication 1st */
        tklock_ui_send_lpm_signal();

        /* Make sure ui locking is initiated before we enter LPM
         * display modes, the dbus signaling happens after some
         * delay.
         */
        mce_datapipe_request_tklock(TKLOCK_REQUEST_ON);
    }
    else {
        /* Do delayed signaling in sync with possible tklock
         * state changes. */
        tklock_ui_notify_schdule();
    }

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
    int64_t now = mce_lib_get_boot_tick();

    for( size_t i = 0; i < numof(tklock_lpmui_hist); ++i ) {
        tklock_lpmui_hist[i].tick  = now;
        tklock_lpmui_hist[i].state = proximity_sensor_actual;
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

    tklock_lpmui_hist[0].tick  = mce_lib_get_boot_tick();
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

    int64_t now = mce_lib_get_boot_tick();
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

    int64_t t = mce_lib_get_boot_tick();

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
    if( system_state != MCE_SYSTEM_STATE_USER )
        goto EXIT;

    if( lipstick_service_state != SERVICE_STATE_RUNNING )
        goto EXIT;

    if( display_state_curr != MCE_DISPLAY_OFF )
        goto EXIT;

    /* but not during calls, alarms, etc */
    if( uiexception_type != UIEXCEPTION_TYPE_NONE )
        goto EXIT;

    /* when lid is closed */
    if( lid_sensor_filtered == COVER_CLOSED )
        goto EXIT;

    /* or when proximity is covered */
    if( proximity_sensor_effective != COVER_OPEN )
        goto EXIT;

    /* Switch to lpm mode if the proximity sensor history matches activity
     * we expect to see when "the device is taken from pocket" etc */
    if( tklock_lpmui_probe() ) {
        mce_log(LL_DEBUG, "switching to LPM UI");

        /* Note: Display plugin handles MCE_DISPLAY_LPM_ON request as
         *       MCE_DISPLAY_OFF unless lpm mode is both supported
         *       and enabled. */
        mce_datapipe_request_display_state(MCE_DISPLAY_LPM_ON);
    }

EXIT:

    return;
}

/** LPM UI related actions that should be done before display state transition
 */
static void tklock_lpmui_pre_transition_actions(void)
{
    mce_log(LL_DEBUG, "prev=%d, next=%d", display_state_curr, display_state_next);

    switch( display_state_next ) {
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_LPM_OFF:
        /* We are about to make transition to LPM state */
        tklock_lpmui_set_state(true);
        break;

    case MCE_DISPLAY_OFF:
        switch( display_state_curr ) {
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

    if( !mce_touchscreen_gesture_enable_path )
        goto EXIT;

    if( enabled == enable )
        goto EXIT;

    mce_log(LL_DEBUG, "%s", enable ? "enable" : "disable");

    if( (enabled = enable) ) {
        mce_write_string_to_file(mce_touchscreen_gesture_enable_path, "4");
        tklock_dtcalib_start();

        // NOTE: touchscreen inputs must be enabled too
    }
    else {
        tklock_dtcalib_stop();
        mce_write_string_to_file(mce_touchscreen_gesture_enable_path, "0");

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
     *  proximity_sensor_effective <-- tklock_datapipe_proximity_sensor_actual_cb()
     *  display_state_curr   <-- tklock_datapipe_display_state_curr_cb()
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
    switch( display_state_curr ) {
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

    /* If the cover is closed, don't bother */
#if 0 /* TODO: Lid cover state is tracked, but volume keys should be
       *       disabled only if they are unlikely to be useful i.e.
       *       depends on where physical buttons are located and
       *       whether the cover makes pressing them impossible or not.
       *
       *       In absense of such info, better to do nothing.
       */
    if( lid_sensor_filtered == COVER_CLOSED ) {
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
    if( music_playback_ongoing )
        enable_kp = true;

    /* - - - - - - - - - - - - - - - - - - - *
     * touchscreen interrupts
     * - - - - - - - - - - - - - - - - - - - */

    /* display must be on/dim */
    switch( display_state_curr ) {
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
    switch( display_state_curr ) {
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

    /* check if touchscreen gestures are disabled */
    switch( touchscreen_gesture_enable_mode ) {
    case DBLTAP_ENABLE_ALWAYS:
        break;
    case DBLTAP_ENABLE_NEVER:
        enable_dt = false;
        break;
    default:
    case DBLTAP_ENABLE_NO_PROXIMITY:
        if( proximity_sensor_effective != COVER_OPEN )
            enable_dt = false;
        break;
    }

    /* Finally, ensure that touchscreen interrupts are enabled
     * if doubletap gestures are enabled */
    if( enable_dt ) {
        enable_ts = true;
    }

    /* - - - - - - - - - - - - - - - - - - - *
     * overrides
     * - - - - - - - - - - - - - - - - - - - */

#if 0 // FIXME: malf is not really supported yet
    if( submode & MCE_SUBMODE_MALF ) {
        enable_kp = false;
        enable_ts = false;
        enable_dt = false;
    }
#endif

    /* No interaction during shutdown */
    if( shutting_down ) {
        enable_kp = false;
        enable_ts = false;
        enable_dt = false;
    }

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

    switch( display_state_curr ) {
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
        // grab/ungrab based on policy
        grab_ts = !enable_ts;
        break;
    }

    if( !tk_input_policy_enabled )
        grab_ts = false;

    /* Grabbing touch input is always permitted, but ungrabbing
     * only when proximity sensor is not covered / proximity
     * blocks input feature is disabled */
    if( grab_ts ||
        ( (proximity_sensor_effective == COVER_OPEN ||
           !proximity_blocks_touch) &&
          (lid_sensor_filtered != COVER_CLOSED) ) ) {
        datapipe_exec_full(&touch_grab_wanted_pipe,
                           GINT_TO_POINTER(grab_ts));
    }

    /* - - - - - - - - - - - - - - - - - - - *
     * in case emitting of keypad events can't
     * be controlled, we use evdev input grab
     * to block ui from seeing them while the
     * display is off
     * - - - - - - - - - - - - - - - - - - - */

    bool grab_kp = !enable_kp;

    switch( volkey_policy ) {
    case VOLKEY_POLICY_MEDIA_ONLY:
        if( !music_playback_ongoing )
            grab_kp = true;
        break;

    default:
        break;
    }

    if( !tk_input_policy_enabled )
        grab_kp = false;

    datapipe_exec_full(&keypad_grab_wanted_pipe,
                       GINT_TO_POINTER(grab_kp));

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
 * DYNAMIC_SETTINGS
 * ========================================================================= */

static void tklock_setting_sanitize_lid_open_actions(void)
{
    switch( tklock_lid_open_actions ) {
    case LID_OPEN_ACTION_DISABLED:
    case LID_OPEN_ACTION_UNBLANK:
    case LID_OPEN_ACTION_TKUNLOCK:
        break;

    default:
        mce_log(LL_WARN, "Lid open has invalid policy: %d; "
                "using default", tklock_lid_open_actions);
        tklock_lid_open_actions = MCE_DEFAULT_TK_LID_OPEN_ACTIONS;
        break;
    }
}

static void tklock_setting_sanitize_lid_close_actions(void)
{
    switch( tklock_lid_close_actions ) {
    case LID_CLOSE_ACTION_DISABLED:
    case LID_CLOSE_ACTION_BLANK:
    case LID_CLOSE_ACTION_TKLOCK:
        break;

    default:
        mce_log(LL_WARN, "Lid close has invalid policy: %d; "
                "using default", tklock_lid_close_actions);
        tklock_lid_close_actions = MCE_DEFAULT_TK_LID_CLOSE_ACTIONS;
        break;
    }
}

static void tklock_setting_sanitize_kbd_open_trigger(void)
{
    switch( tklock_kbd_open_trigger ) {
    case KBD_OPEN_TRIGGER_NEVER:
    case KBD_OPEN_TRIGGER_ALWAYS:
    case KBD_OPEN_TRIGGER_NO_PROXIMITY:
        break;

    default:
        mce_log(LL_WARN, "Invalid kbd open trigger: %d; using default",
                tklock_kbd_open_trigger);
        tklock_kbd_open_trigger = MCE_DEFAULT_TK_KBD_OPEN_TRIGGER;
        break;
    }
}

static void tklock_setting_sanitize_kbd_open_actions(void)
{
    switch( tklock_kbd_open_actions ) {
    case LID_OPEN_ACTION_DISABLED:
    case LID_OPEN_ACTION_UNBLANK:
    case LID_OPEN_ACTION_TKUNLOCK:
        break;

    default:
        mce_log(LL_WARN, "Invalid kbd open actions: %d; using default",
                tklock_kbd_open_actions);
        tklock_kbd_open_actions = MCE_DEFAULT_TK_KBD_OPEN_ACTIONS;
        break;
    }
}

static void tklock_setting_sanitize_kbd_close_trigger(void)
{
    switch( tklock_kbd_close_trigger ) {
    case KBD_CLOSE_TRIGGER_NEVER:
    case KBD_CLOSE_TRIGGER_ALWAYS:
    case KBD_CLOSE_TRIGGER_AFTER_OPEN:
        break;

    default:
        mce_log(LL_WARN, "Invalid kbd close trigger: %d; using default",
                tklock_kbd_close_trigger);
        tklock_kbd_close_trigger = MCE_DEFAULT_TK_KBD_CLOSE_TRIGGER;
        break;
    }
}

static void tklock_setting_sanitize_kbd_close_actions(void)
{
    switch( tklock_kbd_close_actions ) {
    case LID_CLOSE_ACTION_DISABLED:
    case LID_CLOSE_ACTION_BLANK:
    case LID_CLOSE_ACTION_TKLOCK:
        break;

    default:
        mce_log(LL_WARN, "Invalid kbd close actions: %d; using default",
                tklock_kbd_close_actions);
        tklock_kbd_close_actions = MCE_DEFAULT_TK_KBD_CLOSE_ACTIONS;
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
static void tklock_setting_cb(GConfClient *const gcc, const guint id,
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

    if( id == tk_autolock_enabled_setting_id ) {
        tk_autolock_enabled = gconf_value_get_bool(gcv) ? 1 : 0;
        tklock_autolock_rethink();
    }
    else if( id == tk_input_policy_enabled_setting_id ) {
        gboolean old = tk_input_policy_enabled;
        tk_input_policy_enabled = gconf_value_get_bool(gcv) ? 1 : 0;
        if( tk_input_policy_enabled != old ) {
            mce_log(LL_NOTICE, "input grabbing %s",
                    tk_input_policy_enabled ? "allowed" : "denied");
            tklock_evctrl_rethink();
        }
    }
    else if( id == lid_sensor_enabled_setting_id ) {
        lid_sensor_enabled = gconf_value_get_bool(gcv) ? 1 : 0;
        tklock_lidfilter_rethink_lid_state();
    }
    else if( id == als_enabled_setting_id ) {
        als_enabled = gconf_value_get_bool(gcv);
        tklock_lidfilter_rethink_lid_state();
    }
    else if( id == filter_lid_with_als_setting_id ) {
        filter_lid_with_als = gconf_value_get_bool(gcv);
        tklock_lidfilter_rethink_lid_state();
    }
    else if( id == filter_lid_als_limit_setting_id ) {
        filter_lid_als_limit = gconf_value_get_int(gcv);
        tklock_lidfilter_rethink_lid_state();
    }
    else if( id == lockscreen_anim_enabled_setting_id ) {
        lockscreen_anim_enabled= gconf_value_get_bool(gcv);
    }
    else if( id == tklock_autolock_delay_setting_id ) {
        gint old = tklock_autolock_delay;
        tklock_autolock_delay = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "tklock_autolock_delay: %d -> %d",
                old, tklock_autolock_delay);
        // Note: takes effect the next time display turns off
    }
    else if( id == proximity_blocks_touch_setting_id ) {
        proximity_blocks_touch = gconf_value_get_bool(gcv) ? 1 : 0;
        tklock_evctrl_rethink();
    }
    else if( id == volkey_policy_setting_id ) {
        volkey_policy = gconf_value_get_int(gcv);
        tklock_evctrl_rethink();
    }
    else if( id == tklock_lid_open_actions_setting_id ) {
        tklock_lid_open_actions = gconf_value_get_int(gcv);
        tklock_setting_sanitize_lid_open_actions();
        tklock_evctrl_rethink();
    }
    else if( id == tklock_lid_close_actions_setting_id ) {
        tklock_lid_close_actions = gconf_value_get_int(gcv);
        tklock_setting_sanitize_lid_close_actions();
        tklock_evctrl_rethink();
    }
    else if( id == tklock_kbd_open_trigger_setting_id ) {
        tklock_kbd_open_trigger = gconf_value_get_int(gcv);
        tklock_setting_sanitize_kbd_open_trigger();
    }
    else if( id == tklock_kbd_open_actions_setting_id ) {
        tklock_kbd_open_actions = gconf_value_get_int(gcv);
        tklock_setting_sanitize_kbd_open_actions();
    }
    else if( id == tklock_kbd_close_trigger_setting_id ) {
        tklock_kbd_close_trigger = gconf_value_get_int(gcv);
        tklock_setting_sanitize_kbd_close_trigger();
    }
    else if( id == tklock_kbd_close_actions_setting_id ) {
        tklock_kbd_close_actions = gconf_value_get_int(gcv);
        tklock_setting_sanitize_kbd_close_actions();
    }
    else if( id == touchscreen_gesture_enable_mode_setting_id ) {
        gint old = touchscreen_gesture_enable_mode;
        touchscreen_gesture_enable_mode = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "touchscreen_gesture_enable_mode: %d -> %d",
                old, touchscreen_gesture_enable_mode);
        tklock_evctrl_rethink();
    }
    else if( id == tklock_lpmui_triggering_setting_id ) {
        gint old = tklock_lpmui_triggering;
        tklock_lpmui_triggering = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "tklock_lpmui_triggering: %d -> %d",
                old, tklock_lpmui_triggering);
    }
    else if( id == tklock_devicelock_in_lockscreen_setting_id ) {
        gboolean old = tklock_devicelock_in_lockscreen;
        tklock_devicelock_in_lockscreen = gconf_value_get_bool(gcv);
        mce_log(LL_NOTICE, "tklock_devicelock_in_lockscreen: %d -> %d",
                old, tklock_devicelock_in_lockscreen);
    }
    else if( id == exception_length_call_in_setting_id ) {
        gint old = exception_length_call_in;
        exception_length_call_in = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "exception_length_call_in: %d -> %d",
                old, exception_length_call_in);
    }
    else if( id == exception_length_call_out_setting_id ) {
        gint old = exception_length_call_out;
        exception_length_call_out = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "exception_length_call_out: %d -> %d",
                old, exception_length_call_out);
    }
    else if( id == exception_length_alarm_setting_id ) {
        gint old = exception_length_alarm;
        exception_length_alarm = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "exception_length_alarm: %d -> %d",
                old, exception_length_alarm);
    }
    else if( id == exception_length_usb_connect_setting_id ) {
        gint old = exception_length_usb_connect;
        exception_length_usb_connect = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "exception_length_usb_connect: %d -> %d",
                old, exception_length_usb_connect);
    }
    else if( id == exception_length_usb_dialog_setting_id ) {
        gint old = exception_length_usb_dialog;
        exception_length_usb_dialog = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "exception_length_usb_dialog: %d -> %d",
                old, exception_length_usb_dialog);
    }
    else if( id == exception_length_charger_setting_id ) {
        gint old = exception_length_charger;
        exception_length_charger = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "exception_length_charger: %d -> %d",
                old, exception_length_charger);
    }
    else if( id == exception_length_battery_setting_id ) {
        gint old = exception_length_battery;
        exception_length_battery = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "exception_length_battery: %d -> %d",
                old, exception_length_battery);
    }
    else if( id == exception_length_jack_in_setting_id ) {
        gint old = exception_length_jack_in;
        exception_length_jack_in = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "exception_length_jack_in: %d -> %d",
                old, exception_length_jack_in);
    }
    else if( id == exception_length_jack_out_setting_id ) {
        gint old = exception_length_jack_out;
        exception_length_jack_out = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "exception_length_jack_out: %d -> %d",
                old, exception_length_jack_out);
    }
    else if( id == exception_length_camera_setting_id ) {
        gint old = exception_length_camera;
        exception_length_camera = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "exception_length_camera: %d -> %d",
                old, exception_length_camera);
    }
    else if( id == exception_length_volume_setting_id ) {
        gint old = exception_length_volume;
        exception_length_volume = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "exception_length_volume: %d -> %d",
                old, exception_length_volume);
    }
    else if( id == exception_length_activity_setting_id ) {
        gint old = exception_length_activity;
        exception_length_activity = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "exception_length_activity: %d -> %d",
                old, exception_length_activity);
    }
    else if( id == tklock_proximity_delay_default_setting_id ) {
        gint old = tklock_proximity_delay_default;
        tklock_proximity_delay_default = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "proximity_delay_default: %d -> %d",
                old, tklock_proximity_delay_default);
    }
    else if( id == tklock_proximity_delay_incall_setting_id ) {
        gint old = tklock_proximity_delay_incall;
        tklock_proximity_delay_incall = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "proximity_delay_incall: %d -> %d",
                old, tklock_proximity_delay_incall);
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }

EXIT:
    return;
}

/** Get intial setting values and start tracking changes
 */
static void tklock_setting_init(void)
{
    /* Touchscreen/keypad autolock enabled */
    mce_setting_track_bool(MCE_SETTING_TK_AUTOLOCK_ENABLED,
                           &tk_autolock_enabled,
                           MCE_DEFAULT_TK_AUTOLOCK_ENABLED,
                           tklock_setting_cb,
                           &tk_autolock_enabled_setting_id);

    /* Grabbing input devices allowed */
    mce_setting_track_bool(MCE_SETTING_TK_INPUT_POLICY_ENABLED,
                           &tk_input_policy_enabled,
                           MCE_DEFAULT_TK_INPUT_POLICY_ENABLED,
                           tklock_setting_cb,
                           &tk_input_policy_enabled_setting_id);

    /* Touchscreen/keypad autolock delay */
    mce_setting_track_int(MCE_SETTING_TK_AUTOLOCK_DELAY,
                          &tklock_autolock_delay,
                          MCE_DEFAULT_TK_AUTOLOCK_DELAY,
                          tklock_setting_cb,
                          &tklock_autolock_delay_setting_id);

    /* Volume key input policy */
    mce_setting_track_int(MCE_SETTING_TK_VOLKEY_POLICY,
                          &volkey_policy,
                          MCE_DEFAULT_TK_VOLKEY_POLICY,
                          tklock_setting_cb,
                          &volkey_policy_setting_id);

    /* Lid sensor open policy */
    mce_setting_track_int(MCE_SETTING_TK_LID_OPEN_ACTIONS,
                          &tklock_lid_open_actions,
                          MCE_DEFAULT_TK_LID_OPEN_ACTIONS,
                          tklock_setting_cb,
                          &tklock_lid_open_actions_setting_id);

    tklock_setting_sanitize_lid_open_actions();

    /* Lid sensor close policy */
    mce_setting_track_int(MCE_SETTING_TK_LID_CLOSE_ACTIONS,
                          &tklock_lid_close_actions,
                          MCE_DEFAULT_TK_LID_CLOSE_ACTIONS,
                          tklock_setting_cb,
                          &tklock_lid_close_actions_setting_id);

    tklock_setting_sanitize_lid_close_actions();

    /* Kbd slide open policy */
    mce_setting_track_int(MCE_SETTING_TK_KBD_OPEN_TRIGGER,
                          &tklock_kbd_open_trigger,
                          MCE_DEFAULT_TK_KBD_OPEN_TRIGGER,
                          tklock_setting_cb,
                          &tklock_kbd_open_trigger_setting_id);

    tklock_setting_sanitize_kbd_open_trigger();

    mce_setting_track_int(MCE_SETTING_TK_KBD_OPEN_ACTIONS,
                          &tklock_kbd_open_actions,
                          MCE_DEFAULT_TK_KBD_OPEN_ACTIONS,
                          tklock_setting_cb,
                          &tklock_kbd_open_actions_setting_id);

    tklock_setting_sanitize_kbd_open_actions();

    /* Kbd slide close policy */
    mce_setting_track_int(MCE_SETTING_TK_KBD_CLOSE_TRIGGER,
                          &tklock_kbd_close_trigger,
                          MCE_DEFAULT_TK_KBD_CLOSE_TRIGGER,
                          tklock_setting_cb,
                          &tklock_kbd_close_trigger_setting_id);

    tklock_setting_sanitize_kbd_close_trigger();

    mce_setting_track_int(MCE_SETTING_TK_KBD_CLOSE_ACTIONS,
                          &tklock_kbd_close_actions,
                          MCE_DEFAULT_TK_KBD_CLOSE_ACTIONS,
                          tklock_setting_cb,
                          &tklock_kbd_close_actions_setting_id);

    tklock_setting_sanitize_kbd_close_actions();

    /** Touchscreen double tap gesture mode */
    mce_setting_track_int(MCE_SETTING_DOUBLETAP_MODE,
                          &touchscreen_gesture_enable_mode,
                          MCE_DEFAULT_DOUBLETAP_MODE,
                          tklock_setting_cb,
                          &touchscreen_gesture_enable_mode_setting_id);

    /* Bitmap of automatic lpm triggering modes */
    mce_setting_track_int(MCE_SETTING_TK_LPMUI_TRIGGERING,
                          &tklock_lpmui_triggering,
                          MCE_DEFAULT_TK_LPMUI_TRIGGERING,
                          tklock_setting_cb,
                          &tklock_lpmui_triggering_setting_id);

    /* Proximity can block touch input */
    mce_setting_track_bool(MCE_SETTING_TK_PROXIMITY_BLOCKS_TOUCH,
                           &proximity_blocks_touch,
                           MCE_DEFAULT_TK_PROXIMITY_BLOCKS_TOUCH,
                           tklock_setting_cb,
                           &proximity_blocks_touch_setting_id);

    /* Devicelock is in lockscreen */
    mce_setting_track_bool(MCE_SETTING_TK_DEVICELOCK_IN_LOCKSCREEN,
                           &tklock_devicelock_in_lockscreen,
                           MCE_DEFAULT_TK_DEVICELOCK_IN_LOCKSCREEN,
                           tklock_setting_cb,
                           &tklock_devicelock_in_lockscreen_setting_id);

    /* Touchscreen/keypad autolock enabled */
    mce_setting_track_bool(MCE_SETTING_TK_LID_SENSOR_ENABLED,
                           &lid_sensor_enabled,
                           MCE_DEFAULT_TK_LID_SENSOR_ENABLED,
                           tklock_setting_cb,
                           &lid_sensor_enabled_setting_id);

    mce_setting_track_bool(MCE_SETTING_DISPLAY_ALS_ENABLED,
                           &als_enabled,
                           MCE_DEFAULT_DISPLAY_ALS_ENABLED,
                           tklock_setting_cb,
                           &als_enabled_setting_id);

    mce_setting_track_bool(MCE_SETTING_TK_FILTER_LID_WITH_ALS,
                           &filter_lid_with_als,
                           MCE_DEFAULT_TK_FILTER_LID_WITH_ALS,
                           tklock_setting_cb,
                           &filter_lid_with_als_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_FILTER_LID_ALS_LIMIT,
                          &filter_lid_als_limit,
                          MCE_DEFAULT_TK_FILTER_LID_ALS_LIMIT,
                          tklock_setting_cb,
                          &filter_lid_als_limit_setting_id);

    /* Display on exception lengths */
    mce_setting_track_int(MCE_SETTING_TK_EXCEPT_LEN_CALL_IN,
                          &exception_length_call_in,
                          MCE_DEFAULT_TK_EXCEPT_LEN_CALL_IN,
                          tklock_setting_cb,
                          &exception_length_call_in_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_EXCEPT_LEN_CALL_OUT,
                          &exception_length_call_out,
                          MCE_DEFAULT_TK_EXCEPT_LEN_CALL_OUT,
                          tklock_setting_cb,
                          &exception_length_call_out_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_EXCEPT_LEN_ALARM,
                          &exception_length_alarm,
                          MCE_DEFAULT_TK_EXCEPT_LEN_ALARM,
                          tklock_setting_cb,
                          &exception_length_alarm_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_EXCEPT_LEN_USB_CONNECT,
                          &exception_length_usb_connect,
                          MCE_DEFAULT_TK_EXCEPT_LEN_USB_CONNECT,
                          tklock_setting_cb,
                          &exception_length_usb_connect_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_EXCEPT_LEN_USB_DIALOG,
                          &exception_length_usb_dialog,
                          MCE_DEFAULT_TK_EXCEPT_LEN_USB_DIALOG,
                          tklock_setting_cb,
                          &exception_length_usb_dialog_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_EXCEPT_LEN_CHARGER,
                          &exception_length_charger,
                          MCE_DEFAULT_TK_EXCEPT_LEN_CHARGER,
                          tklock_setting_cb,
                          &exception_length_charger_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_EXCEPT_LEN_BATTERY,
                          &exception_length_battery,
                          MCE_DEFAULT_TK_EXCEPT_LEN_BATTERY,
                          tklock_setting_cb,
                          &exception_length_battery_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_EXCEPT_LEN_JACK_IN,
                          &exception_length_jack_in,
                          MCE_DEFAULT_TK_EXCEPT_LEN_JACK_IN,
                          tklock_setting_cb,
                          &exception_length_jack_in_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_EXCEPT_LEN_JACK_OUT,
                          &exception_length_jack_out,
                          MCE_DEFAULT_TK_EXCEPT_LEN_JACK_OUT,
                          tklock_setting_cb,
                          &exception_length_jack_out_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_EXCEPT_LEN_CAMERA,
                          &exception_length_camera,
                          MCE_DEFAULT_TK_EXCEPT_LEN_CAMERA,
                          tklock_setting_cb,
                          &exception_length_camera_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_EXCEPT_LEN_VOLUME,
                          &exception_length_volume,
                          MCE_DEFAULT_TK_EXCEPT_LEN_VOLUME,
                          tklock_setting_cb,
                          &exception_length_volume_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_EXCEPT_LEN_ACTIVITY,
                          &exception_length_activity,
                          MCE_DEFAULT_TK_EXCEPT_LEN_ACTIVITY,
                          tklock_setting_cb,
                          &exception_length_activity_setting_id);

    mce_setting_track_bool(MCE_SETTING_TK_LOCKSCREEN_ANIM_ENABLED,
                           &lockscreen_anim_enabled,
                           MCE_DEFAULT_TK_LOCKSCREEN_ANIM_ENABLED,
                           tklock_setting_cb,
                           &lockscreen_anim_enabled_setting_id);

    /* Delays for proximity sensor uncover handling */
    mce_setting_track_int(MCE_SETTING_TK_PROXIMITY_DELAY_DEFAULT,
                          &tklock_proximity_delay_default,
                          MCE_DEFAULT_TK_PROXIMITY_DELAY_DEFAULT,
                          tklock_setting_cb,
                          &tklock_proximity_delay_default_setting_id);

    mce_setting_track_int(MCE_SETTING_TK_PROXIMITY_DELAY_INCALL,
                          &tklock_proximity_delay_incall,
                          MCE_DEFAULT_TK_PROXIMITY_DELAY_INCALL,
                          tklock_setting_cb,
                          &tklock_proximity_delay_incall_setting_id);
}

/** Stop tracking setting changes
 */
static void tklock_setting_quit(void)
{
    mce_setting_notifier_remove(volkey_policy_setting_id),
        volkey_policy_setting_id = 0;

    mce_setting_notifier_remove(tklock_lid_open_actions_setting_id),
        tklock_lid_open_actions_setting_id = 0;

    mce_setting_notifier_remove(tklock_lid_close_actions_setting_id),
        tklock_lid_close_actions_setting_id = 0;

    mce_setting_notifier_remove(tklock_kbd_open_trigger_setting_id),
        tklock_kbd_open_trigger_setting_id = 0;

    mce_setting_notifier_remove(tklock_kbd_open_actions_setting_id),
        tklock_kbd_open_actions_setting_id = 0;

    mce_setting_notifier_remove(tklock_kbd_close_trigger_setting_id),
        tklock_kbd_close_trigger_setting_id = 0;

    mce_setting_notifier_remove(tklock_kbd_close_actions_setting_id),
        tklock_kbd_close_actions_setting_id = 0;

    mce_setting_notifier_remove(tk_autolock_enabled_setting_id),
        tk_autolock_enabled_setting_id = 0;

    mce_setting_notifier_remove(tk_input_policy_enabled_setting_id),
        tk_input_policy_enabled_setting_id = 0;

    mce_setting_notifier_remove(tklock_autolock_delay_setting_id),
        tklock_autolock_delay_setting_id = 0;

    mce_setting_notifier_remove(touchscreen_gesture_enable_mode_setting_id),
        touchscreen_gesture_enable_mode_setting_id = 0;

    mce_setting_notifier_remove(tklock_lpmui_triggering_setting_id),
        tklock_lpmui_triggering_setting_id = 0;

    mce_setting_notifier_remove(proximity_blocks_touch_setting_id),
        proximity_blocks_touch_setting_id = 0;

    mce_setting_notifier_remove(tklock_devicelock_in_lockscreen_setting_id),
        tklock_devicelock_in_lockscreen_setting_id = 0;

    mce_setting_notifier_remove(lid_sensor_enabled_setting_id),
        lid_sensor_enabled_setting_id = 0;

    mce_setting_notifier_remove(als_enabled_setting_id),
        als_enabled_setting_id = 0;

    mce_setting_notifier_remove(filter_lid_with_als_setting_id),
        filter_lid_with_als_setting_id = 0;

    mce_setting_notifier_remove(filter_lid_als_limit_setting_id),
        filter_lid_als_limit_setting_id = 0;

    mce_setting_notifier_remove(exception_length_call_in_setting_id),
        exception_length_call_in_setting_id = 0;

    mce_setting_notifier_remove(exception_length_call_out_setting_id),
        exception_length_call_out_setting_id = 0;

    mce_setting_notifier_remove(exception_length_alarm_setting_id),
        exception_length_alarm_setting_id = 0;

    mce_setting_notifier_remove(exception_length_usb_connect_setting_id),
        exception_length_usb_connect_setting_id = 0;

    mce_setting_notifier_remove(exception_length_usb_dialog_setting_id),
        exception_length_usb_dialog_setting_id = 0;

    mce_setting_notifier_remove(exception_length_charger_setting_id),
        exception_length_charger_setting_id = 0;

    mce_setting_notifier_remove(exception_length_battery_setting_id),
        exception_length_battery_setting_id = 0;

    mce_setting_notifier_remove(exception_length_jack_in_setting_id),
        exception_length_jack_in_setting_id = 0;

    mce_setting_notifier_remove(exception_length_jack_out_setting_id),
        exception_length_jack_out_setting_id = 0;

    mce_setting_notifier_remove(exception_length_camera_setting_id),
        exception_length_camera_setting_id = 0;

    mce_setting_notifier_remove(exception_length_volume_setting_id),
        exception_length_volume_setting_id = 0;

    mce_setting_notifier_remove(exception_length_activity_setting_id),
        exception_length_activity_setting_id = 0;

    mce_setting_notifier_remove(lockscreen_anim_enabled_setting_id),
        lockscreen_anim_enabled_setting_id = 0;
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
        mce_touchscreen_gesture_enable_path =
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

static guint tklock_ui_notify_end_id = 0;
static guint tklock_ui_notify_beg_id = 0;

static void tklock_ui_send_tklock_signal(void)
{
    bool current = tklock_ui_is_enabled();

    if( tklock_ui_notified == current )
        goto EXIT;

    tklock_ui_notified = current;

    /* do lipstick specific ipc */
    if( lipstick_service_state == SERVICE_STATE_RUNNING ) {
        if( current )
            tklock_ui_open();
        else
            tklock_ui_close();
    }

    /* broadcast signal */
    tklock_dbus_send_tklock_mode(0);

EXIT:
    return;
}

static void tklock_ui_notify_rethink_wakelock(void)
{
    static bool have_lock = false;

    bool need_lock = (tklock_ui_notify_beg_id || tklock_ui_notify_end_id);

    if( have_lock == need_lock )
        goto EXIT;

    mce_log(LL_DEBUG, "ui notify wakelock: %s",
            need_lock ? "OBTAIN" : "RELEASE");

    if( (have_lock = need_lock) ) {
        wakelock_lock("mce_tklock_notify", -1);
    }
    else
        wakelock_unlock("mce_tklock_notify");

EXIT:
    return;
}

static bool tklock_ui_notify_must_be_delayed(void)
{
    bool delay = false;

    /* We do not want to send tklock changes during display power
     * off sequence as those might trigger lockscreen related
     * animations at UI side */

    if( display_state_curr == MCE_DISPLAY_POWER_DOWN ) {
        /* Powering down the display for any reason */
        delay = true;
    }
    else if( display_state_curr != display_state_next ) {
        switch( display_state_curr ) {
        case MCE_DISPLAY_LPM_ON:
            /* Making transition from lpm state. In order not
             * to confuse device lock ui, finish the display
             * state transition before acting on tklock state.
             */
            delay = true;
            break;
        default:
            break;
        }

        switch( display_state_next ) {
        case MCE_DISPLAY_OFF:
        case MCE_DISPLAY_LPM_OFF:
            /* Making transition to a blanked display state */
            delay = true;
            break;
        default:
            break;
        }
    }

    return delay;
}

static gboolean tklock_ui_notify_end_cb(gpointer data)
{
    (void) data;

    if( !tklock_ui_notify_end_id )
        goto EXIT;

    tklock_ui_notify_end_id = 0;

EXIT:

    tklock_ui_notify_rethink_wakelock();

    return FALSE;
}

static gboolean tklock_ui_notify_beg_cb(gpointer data)
{
    (void) data;

    if( !tklock_ui_notify_beg_id )
        goto EXIT;

    tklock_ui_notify_beg_id = 0;

    if( tklock_ui_notify_must_be_delayed() )
        goto EXIT;

    /* Broadcast tklock state 1st */
    tklock_ui_send_tklock_signal();

    /* Deal with possibly ending lpm state */
    tklock_ui_send_lpm_signal();

    /* Deal with redirection of tkunlock -> show device lock prompt */
    if( tklock_devicelock_want_to_unlock ) {
        if( tklock_ui_is_enabled() &&
            display_state_next == MCE_DISPLAY_ON ) {
            mce_log(LL_DEBUG, "request: show device lock query");
            tklock_ui_show_device_unlock();
        }
        else {
            mce_log(LL_WARN, "skipped: show device lock query");
        }
        tklock_devicelock_want_to_unlock = false;
    }

    /* give ui a chance to see the signal */
    if( tklock_ui_notify_end_id )
        g_source_remove(tklock_ui_notify_end_id);

    tklock_ui_notify_end_id = g_timeout_add(2000,
                                            tklock_ui_notify_end_cb,
                                            0);

EXIT:

    tklock_ui_notify_rethink_wakelock();

    return FALSE;
}

static void tklock_ui_notify_cancel(void)
{
    if( tklock_ui_notify_end_id ) {
        g_source_remove(tklock_ui_notify_end_id),
            tklock_ui_notify_end_id = 0;
    }
    if( tklock_ui_notify_beg_id ) {
        g_source_remove(tklock_ui_notify_beg_id),
            tklock_ui_notify_beg_id = 0;
    }

    tklock_ui_notify_rethink_wakelock();
}

static void tklock_ui_notify_schdule(void)
{
    if( tklock_ui_notify_end_id ) {
        g_source_remove(tklock_ui_notify_end_id),
            tklock_ui_notify_end_id = 0;
    }

    if( tklock_ui_notify_must_be_delayed() )
        goto EXIT;

    if( !tklock_ui_notify_beg_id ) {
        tklock_ui_notify_beg_id = g_idle_add(tklock_ui_notify_beg_cb, 0);
    }

EXIT:
    tklock_ui_notify_rethink_wakelock();
}

/** Timer for synchronizing tklock ui state -> submode tklock bit */
static guint    tklock_ui_sync_id = 0;

/** Callback for synchronizing tklock_ui -> submode tklock bit */
static gboolean tklock_ui_sync_cb(gpointer aptr)
{
    (void)aptr;

    tklock_ui_sync_id = 0;

    mce_log(LL_DEBUG, "tklock sync triggered");

    bool enabled = tklock_ui_is_enabled();

    if( tklock_datapipe_in_tklock_submode() != enabled )
        tklock_datapipe_set_tklock_submode(enabled);

    return G_SOURCE_REMOVE;
}

static bool tklock_ui_is_enabled(void)
{
    return tklock_ui_enabled_pvt;
}

static void tklock_ui_set_enabled(bool enable)
{
    /* See also tklock_datapipe_set_tklock_submode() */

    /* Note: As long as lipstick process is running, mce must
     *       not attempt forced tklock removal as it can lead
     *       to tklock state ringing if/when lipstick happens
     *       to require tklock to be set. */

    /* Filter request based on device state */

    /* When there is no UI to lock, allowing tklock to
     * be set can only cause problems */
    if( enable && lipstick_service_state != SERVICE_STATE_RUNNING ) {
        mce_log(LL_INFO, "deny tklock; lipstick not running");
        enable = false;
        goto EXIT;
    }

    /* If device lock is handled in lockscreen, we must not
     * allow *removing* of tklock (=move away from lockscreen)
     * while device lock is still active. */
    if( !enable && tklock_devicelock_in_lockscreen &&
        devicelock_state == DEVICELOCK_STATE_LOCKED ) {
        mce_log(LL_DEVEL, "deny tkunlock; show device lock query");
        tklock_devicelock_want_to_unlock = true;
        enable = true;
        goto EXIT;
    }

    /* Do not allow unlocking while lid sensor is enabled and covered */
    if( !enable && lid_sensor_filtered == COVER_CLOSED && !enable ) {
        mce_log(LL_WARN, "deny tkunlock; lid sensor is covered");
        enable = true;
        goto EXIT;
    }

    /* Request accepted as-is */

EXIT:
    /* Check and handle state change */
    if( tklock_ui_enabled_pvt != enable ) {
        tklock_ui_enabled_pvt = enable;
        mce_log(LL_DEBUG, "tklock_ui_enabled: %s",
                tklock_ui_enabled_pvt ? "TRUE" : "FALSE");
    }

    /* Schedule notification attempt even if there is no change,
     * so that ui side is not left thinking that a tklock request
     * it made was accepted. */
    tklock_ui_notify_schdule();

    /* Sync to submode in any case */
    if( !tklock_ui_sync_id ) {
        mce_log(LL_DEBUG, "tklock sync scheduled");
        tklock_ui_sync_id = g_idle_add(tklock_ui_sync_cb, 0);
    }
}

/** Handle reply to device lock state query
 */
static void tklock_ui_get_devicelock_cb(DBusPendingCall *pc, void *aptr)
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

    mce_log(LL_INFO, "device lock status reply: state=%s",
            devicelock_state_repr(val));
    tklock_datapipe_set_devicelock_state(val);

EXIT:
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

/** Initiate asynchronous device lock state query
 */
static void tklock_ui_get_devicelock(void)
{
    mce_log(LL_DEBUG, "query device lock status");
    dbus_send(DEVICELOCK_SERVICE,
              DEVICELOCK_REQUEST_PATH,
              DEVICELOCK_REQUEST_IF,
              "state",
              tklock_ui_get_devicelock_cb,
              DBUS_TYPE_INVALID);
}

/** Broadcast LPM UI state over D-Bus
 */
static void tklock_ui_send_lpm_signal(void)
{
    if( tklock_lpmui_state_signaled == tklock_lpmui_state_wanted )
        goto EXIT;

    tklock_lpmui_state_signaled = tklock_lpmui_state_wanted;

    bool enabled = (tklock_lpmui_state_wanted > 0);

    /* Do lipstick specific ipc 1st */
    if( lipstick_service_state == SERVICE_STATE_RUNNING ) {
        if( enabled )
            tklock_ui_enable_lpm();
        else
            tklock_ui_disable_lpm();
    }

    /* then send the signal */
    const char *sig = MCE_LPM_UI_MODE_SIG;
    const char *arg = enabled ? MCE_LPM_UI_ENABLED : MCE_LPM_UI_DISABLED;

    mce_log(LL_DEVEL, "sending dbus signal: %s %s", sig, arg);
    dbus_send(0, MCE_SIGNAL_PATH, MCE_SIGNAL_IF,  sig, 0,
              DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);

EXIT:
    return;
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

/** Tell lipstick that device unlock prompt should be shown
 */
static void tklock_ui_show_device_unlock(void)
{
    /* Re-use the signal that lipstick already uses for selecting between
     * plain lockscreen and showing the device unlock view in the context
     * of configurable power-button actions.
     *
     * The naming of the signal is a bit unfortunate, since
     * 1) it is now used for other things besides dealing with power key
     * 2) having the tkunlock redirection available means that it would
     *    not be needed at all in the power key handler ...
     */
    const char *sig = MCE_POWER_BUTTON_TRIGGER;
    const char *arg = "double-power-key";
    dbus_send(0, MCE_SIGNAL_PATH, MCE_SIGNAL_IF, sig, 0,
              DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
}

/* ========================================================================= *
 * DBUS MESSAGE HANDLERS
 * ========================================================================= */

/** Send the blanking policy state
 *
 * @param req A method call message to be replied, or
 *            NULL to broadcast a policy change signal
 */
static void
tklock_dbus_send_display_blanking_policy(DBusMessage *const req)
{
    DBusMessage *rsp = 0;

    if( req )
        rsp = dbus_new_method_reply(req);
    else
        rsp = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_BLANKING_POLICY_SIG);
    if( !rsp )
        goto EXIT;

    const char *arg = uiexception_type_to_dbus(uiexception_type);

    mce_log(LL_DEBUG, "send display blanking policy %s: %s",
            req ? "reply" : "signal", arg);

    if( !dbus_message_append_args(rsp,
                                  DBUS_TYPE_STRING, &arg,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    dbus_send_message(rsp), rsp = 0;

EXIT:
    if( rsp ) dbus_message_unref(rsp);
}

/** D-Bus callback for the get blakng policy state method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
tklock_dbus_display_blanking_policy_get_cb(DBusMessage *const msg)
{
    mce_log(LL_DEVEL, "Received blanking policy get from %s",
            mce_dbus_get_message_sender_ident(msg));

    tklock_dbus_send_display_blanking_policy(msg);

    return TRUE;
}

/** Send the keyboard slide open/closed state
 *
 * @param req A method call message to be replied, or
 *            NULL to broadcast a keypad state signal
 */
static void
tklock_dbus_send_keyboard_slide_state(DBusMessage *const req)
{
    DBusMessage *rsp = 0;

    if( req )
        rsp = dbus_new_method_reply(req);
    else
        rsp = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_SLIDING_KEYBOARD_STATE_SIG);
    if( !rsp )
        goto EXIT;

    const char *arg = MCE_SLIDING_KEYBOARD_UNDEF;

    switch( keyboard_slide_output_state ) {
    case COVER_OPEN:   arg = MCE_SLIDING_KEYBOARD_OPEN;   break;
    case COVER_CLOSED: arg = MCE_SLIDING_KEYBOARD_CLOSED; break;
    default: break;
    }

    mce_log(LL_DEBUG, "send keyboard slide state %s: %s",
            req ? "reply" : "signal", arg);

    if( !dbus_message_append_args(rsp,
                                  DBUS_TYPE_STRING, &arg,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    dbus_send_message(rsp), rsp = 0;

EXIT:
    if( rsp ) dbus_message_unref(rsp);
}

/** D-Bus callback for the get keyboard slide state method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
tklock_dbus_keyboard_slide_state_get_req_cb(DBusMessage *const msg)
{
    mce_log(LL_DEVEL, "Received keyboard slide state get request from %s",
            mce_dbus_get_message_sender_ident(msg));

    tklock_dbus_send_keyboard_slide_state(msg);

    return TRUE;
}

/** Send the keyboard available state
 *
 * @param req A method call message to be replied, or
 *            NULL to broadcast a keyboard available state signal
 */
static void
tklock_dbus_send_keyboard_available_state(DBusMessage *const req)
{
    DBusMessage *rsp = 0;

    if( req )
        rsp = dbus_new_method_reply(req);
    else
        rsp = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_HARDWARE_KEYBOARD_STATE_SIG);
    if( !rsp )
        goto EXIT;

    const char *arg = MCE_HARDWARE_KEYBOARD_UNDEF;

    switch( keyboard_available_state ) {
    case COVER_OPEN:   arg = MCE_HARDWARE_KEYBOARD_AVAILABLE;     break;
    case COVER_CLOSED: arg = MCE_HARDWARE_KEYBOARD_NOT_AVAILABLE; break;
    default: break;
    }

    mce_log(LL_DEBUG, "send keyboard available state %s: %s",
            req ? "reply" : "signal", arg);

    if( !dbus_message_append_args(rsp,
                                  DBUS_TYPE_STRING, &arg,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    dbus_send_message(rsp), rsp = 0;

EXIT:
    if( rsp ) dbus_message_unref(rsp);
}

/** D-Bus callback for the get keyboard available state method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
tklock_dbus_keyboard_available_state_get_req_cb(DBusMessage *const msg)
{
    mce_log(LL_DEVEL, "Received keyboard available state get request from %s",
            mce_dbus_get_message_sender_ident(msg));

    tklock_dbus_send_keyboard_available_state(msg);

    return TRUE;
}

/** Send the mouse available state
 *
 * @param req A method call message to be replied, or
 *            NULL to broadcast a mouse available state signal
 */
static void
tklock_dbus_send_mouse_available_state(DBusMessage *const req)
{
    DBusMessage *rsp = 0;

    if( req )
        rsp = dbus_new_method_reply(req);
    else
        rsp = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_HARDWARE_MOUSE_STATE_SIG);
    if( !rsp )
        goto EXIT;

    const char *arg = MCE_HARDWARE_MOUSE_UNDEF;

    switch( mouse_available_state ) {
    case COVER_OPEN:   arg = MCE_HARDWARE_MOUSE_AVAILABLE;     break;
    case COVER_CLOSED: arg = MCE_HARDWARE_MOUSE_NOT_AVAILABLE; break;
    default: break;
    }

    mce_log(LL_DEBUG, "send mouse available state %s: %s",
            req ? "reply" : "signal", arg);

    if( !dbus_message_append_args(rsp,
                                  DBUS_TYPE_STRING, &arg,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    dbus_send_message(rsp), rsp = 0;

EXIT:
    if( rsp )
        dbus_message_unref(rsp);
}

/** D-Bus callback for the get mouse available state method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
tklock_dbus_mouse_available_state_get_req_cb(DBusMessage *const msg)
{
    mce_log(LL_DEVEL, "Received mouse available state get request from %s",
            mce_dbus_get_message_sender_ident(msg));

    tklock_dbus_send_mouse_available_state(msg);

    return TRUE;
}

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

    /* Note: Events on D-Bus must be based on tklock ui state,
     *       not submode tklock bit. */
    const char  *mode  = (tklock_ui_is_enabled() ?
                          MCE_TK_LOCKED : MCE_TK_UNLOCKED);

    /* If method_call is set, send a reply. Otherwise, send a signal. */
    if( method_call ) {
        msg = dbus_new_method_reply(method_call);
        mce_log(LL_DEBUG, "send tklock mode reply: %s", mode);
    }
    else {
        msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_TKLOCK_MODE_SIG);
        mce_log(LL_DEVEL, "send tklock mode signal: %s", mode);
    }

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

/** Apply allow/deny policy for TKLock requests received over D-Bus
 *
 * Basically locking is always allowed, but unlocking only when
 * display already is / is making transition to powered up state.
 *
 * @param state requested state
 *
 * @returns allowed state
 */
static tklock_request_t
tklock_dbus_sanitize_requested_mode(tklock_request_t state)
{
    /* Translate toggle requests to something we can evaluate */
    if( state == TKLOCK_REQUEST_TOGGLE )
        state = tklock_ui_is_enabled() ? TKLOCK_REQUEST_OFF : TKLOCK_REQUEST_ON;

    switch( state ) {
    default:
    case TKLOCK_REQUEST_UNDEF:
    case TKLOCK_REQUEST_TOGGLE:
        break;

    case TKLOCK_REQUEST_OFF:
    case TKLOCK_REQUEST_OFF_DELAYED:
    case TKLOCK_REQUEST_OFF_PROXIMITY:
        state = TKLOCK_REQUEST_OFF;
        switch( display_state_next ) {
        case MCE_DISPLAY_ON:
        case MCE_DISPLAY_DIM:
            break;
        default:
            if( tklock_ui_is_enabled() ) {
                mce_log(LL_WARN, "tkunlock denied due to display=%s",
                        display_state_repr(display_state_next));
                state = TKLOCK_REQUEST_ON;
            }
            break;
        }
        goto EXIT;

    case TKLOCK_REQUEST_ON:
    case TKLOCK_REQUEST_ON_DIMMED:
    case TKLOCK_REQUEST_ON_PROXIMITY:
    case TKLOCK_REQUEST_ON_DELAYED:
        state = TKLOCK_REQUEST_ON;
        break;
    }
EXIT:
    return state;
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

    int state = TKLOCK_REQUEST_UNDEF;

    if (!strcmp(MCE_TK_LOCKED, mode))
        state = TKLOCK_REQUEST_ON;
    else if (!strcmp(MCE_TK_LOCKED_DIM, mode))
        state = TKLOCK_REQUEST_ON_DIMMED;
    else if (!strcmp(MCE_TK_LOCKED_DELAY, mode))
        state = TKLOCK_REQUEST_ON_DELAYED;
    else if (!strcmp(MCE_TK_UNLOCKED, mode))
        state = TKLOCK_REQUEST_OFF;
    else
        mce_log(LL_WARN, "Received an invalid tklock mode; ignoring");

    mce_log(LL_DEBUG, "mode: %s/%d", mode, state);

    if( state != TKLOCK_REQUEST_UNDEF ) {
        tklock_ui_notified = -1;
        state = tklock_dbus_sanitize_requested_mode(state);
        tklock_datapipe_tklock_request_cb(GINT_TO_POINTER(state));
    }

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

/** D-Bus callback for handling interaction expected -state changed signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean tklock_dbus_interaction_expected_cb(DBusMessage *const msg)
{
    DBusError    err = DBUS_ERROR_INIT;
    dbus_bool_t  arg = false;

    if( !msg )
        goto EXIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_BOOLEAN, &arg,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to parse interaction expected signal: %s: %s",
                err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "received interaction expected signal: state=%d", arg);
    tklock_datapipe_update_interaction_expected(arg);

EXIT:
    dbus_error_free(&err);

    return TRUE;
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

    mce_log(LL_DEVEL, "tklock callback value: %s, from %s",
            tklock_status_repr(result),
            mce_dbus_get_message_sender_ident(msg));

    tklock_request_t state = TKLOCK_REQUEST_OFF;
    switch( result ) {
    case TKLOCK_UNLOCK:
        tklock_ui_notified = -1;
        state = tklock_dbus_sanitize_requested_mode(state);
        tklock_datapipe_tklock_request_cb(GINT_TO_POINTER(state));
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

    mce_log(LL_CRUCIAL, "notification begin from %s",
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

    mce_log(LL_CRUCIAL, "notification end from %s",
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
static gboolean tklock_dbus_devicelock_changed_cb(DBusMessage *const msg)
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

    mce_log(LL_DEBUG, "received device lock signal: state=%s",
            devicelock_state_repr(val));
    tklock_datapipe_set_devicelock_state(val);

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
        .callback  = tklock_dbus_devicelock_changed_cb,
    },
    {
        .interface = "org.nemomobile.lipstick.screenlock",
        .name      = "interaction_expected",
        .rules     = "path='/screenlock'",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = tklock_dbus_interaction_expected_cb,
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
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_SLIDING_KEYBOARD_STATE_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"slide_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_HARDWARE_KEYBOARD_STATE_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"keyboard_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_HARDWARE_MOUSE_STATE_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"mouse_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_BLANKING_POLICY_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"blanking_policy\" type=\"s\"/>\n"
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
        .name      = MCE_NOTIFICATION_BEGIN_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_notification_beg_cb,
        .args      =
            "    <arg direction=\"in\" name=\"notification_name\" type=\"s\"/>\n"
            "    <arg direction=\"in\" name=\"duration_time\" type=\"i\"/>\n"
            "    <arg direction=\"in\" name=\"activity_extend_time\" type=\"i\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_NOTIFICATION_END_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_notification_end_cb,
        .args      =
            "    <arg direction=\"in\" name=\"notification_name\" type=\"s\"/>\n"
            "    <arg direction=\"in\" name=\"linger_time\" type=\"i\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_SLIDING_KEYBOARD_STATE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_keyboard_slide_state_get_req_cb,
        .args      =
            "    <arg direction=\"out\" name=\"slide_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_HARDWARE_KEYBOARD_STATE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_keyboard_available_state_get_req_cb,
        .args      =
            "    <arg direction=\"out\" name=\"keyboard_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_HARDWARE_MOUSE_STATE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_mouse_available_state_get_req_cb,
        .args      =
            "    <arg direction=\"out\" name=\"mouse_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_BLANKING_POLICY_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = tklock_dbus_display_blanking_policy_get_cb,
        .args      =
            "    <arg direction=\"out\" name=\"blanking_policy\" type=\"s\"/>\n"
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
    int64_t now = mce_lib_get_boot_tick();
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

        tklock_uiexception_begin(UIEXCEPTION_TYPE_NOTIF, 0);
        tklock_uiexception_rethink();
    }
    else {
        if( (tmo = tklock_notif_state.tn_linger - now) < 0 )
            tmo = 0;
        tklock_uiexception_end(UIEXCEPTION_TYPE_NOTIF, tmo);
        tklock_uiexception_rethink();
    }
}

static void
tklock_notif_extend_by_renew(void)
{
    int64_t now = mce_lib_get_boot_tick();

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

        int64_t now = mce_lib_get_boot_tick();
        int64_t tmo = now + linger;

        if( tklock_notif_state.tn_linger < tmo )
            tklock_notif_state.tn_linger = tmo;

        tklock_notif_update_state();
        goto EXIT;
    }

    mce_log(LL_DEBUG, "attempt to end non-existing notification");

EXIT:

    return;
}

static void
tklock_notif_reserve_slot(const char *owner, const char *name, int64_t length, int64_t renew)
{
    int64_t now = mce_lib_get_boot_tick();
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
    /* Ignore zero length notifications */
    if( length <= 0 )
        goto EXIT;

    /* cap length to [1,30] second range */
    if( length > 30000 )
        length = 30000;
    else if( length < 1000 )
        length = 1000;

    /* cap renew to [0,5] second range, negative means use default */
    if( renew > 5000 )
        renew = 5000;
    else if( renew < 0 )
        renew = exception_length_activity;

    mce_log(LL_DEBUG, "name: %s, length: %d, renew: %d",
            name, (int)length, (int)renew);
    tklock_notif_reserve_slot(owner, name, length, renew);

EXIT:
    return;
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

    /* get dynamic config, install change monitors */
    tklock_setting_init();

    tklock_autolock_init();

    /* Set initial lid_sensor_is_working_pipe value
     * before installing datapipe handlers */
    tklock_lidsensor_init();

    /* attach to internal state variables */
    tklock_datapipe_init();

    /* set up dbus message handlers */
    mce_tklock_init_dbus();

    /* Make sure lpm state gets initialized & broadcast */
    tklock_lpmui_set_state(false);

    /* Broadcast initial blanking policy */
    tklock_dbus_send_display_blanking_policy(0);

    /* Evaluate initial lid sensor state */
    tklock_lidfilter_rethink_lid_state();

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
    tklock_setting_quit();

    /* cancel all timers */
    tklock_autolock_disable();
    tklock_proxlock_disable();
    tklock_uiexception_cancel();
    tklock_dtcalib_stop();
    tklock_datapipe_proximity_uncover_cancel();
    tklock_notif_quit();
    tklock_ui_notify_cancel();

    tklock_autolock_quit();

    if( tklock_ui_sync_id ) {
        g_source_remove(tklock_ui_sync_id),
            tklock_ui_sync_id = 0;
    }

    common_on_proximity_cancel(MODULE_NAME, 0, 0);

    // FIXME: check that final state is sane

    return;
}

/** Perform display powerup under faked abnormal blanking policy
 *
 * @param to_state display state to wake up to
 */
void mce_tklock_unblank(display_state_t to_state)
{
    if( display_state_next == to_state)
        goto EXIT;

    if( !lockscreen_anim_enabled ) {
        /* Disable lockscreen animations by invoking a faked
         * abnormal display blanking policy for the duration
         * of the display power up. */
        tklock_uiexception_begin(UIEXCEPTION_TYPE_NOANIM, 0);
    }

    mce_datapipe_request_display_state(to_state);

EXIT:
    return;
}
