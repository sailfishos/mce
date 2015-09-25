/**
 * @file display.c
 * Display module -- this implements display handling for MCE
 * <p>
 * Copyright © 2007-2011 Nokia Corporation and/or its subsidiary(-ies).
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

#include "display.h"

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-io.h"
#include "../mce-lib.h"
#include "../mce-fbdev.h"
#include "../mce-conf.h"
#include "../mce-gconf.h"
#include "../mce-dbus.h"
#include "../mce-sensorfw.h"
#ifdef ENABLE_HYBRIS
# include "../mce-hybris.h"
#endif

#include "../filewatcher.h"

#ifdef ENABLE_WAKELOCKS
# include "../libwakelock.h"
#endif

#include <sys/ptrace.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <pthread.h>

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include <glib/gstdio.h>
#include <gmodule.h>

#if 0 // DEBUG: make all logging from this module "critical"
# undef mce_log
# define mce_log(LEV, FMT, ARGS...) \
        mce_log_file(LL_CRIT, __FILE__, __FUNCTION__, FMT , ## ARGS)
#endif

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** Module name */
#define MODULE_NAME             "display"

/** UI side graphics fading percentage
 *
 * Controls maximum opacity of the black box rendered on top at the ui
 * when backlight dimming alone is not enough to make dimmed display
 * state visible to the user.
 */
#define MCE_FADER_MAXIMUM_OPACITY_PERCENT 50

/** Backlight fade animation duration after brightness setting changes */
#define MCE_FADER_DURATION_SETTINGS_CHANGED 600

/** Minimum duration for direct backlight fade animation */
#define MCE_FADER_DURATION_HW_MIN    0

/** Maximum duration for direct backlight fade animation */
#define MCE_FADER_DURATION_HW_MAX 5000

/** Minimum duration for compositor based ui dimming animation */
#define MCE_FADER_DURATION_UI_MIN  100

/** Maximum duration for compositor based ui dimming animation */
#define MCE_FADER_DURATION_UI_MAX 5000

/* ========================================================================= *
 * TYPEDEFS
 * ========================================================================= */

/** Display type */
typedef enum {
    /** Display type unset */
    DISPLAY_TYPE_UNSET = -1,
    /** No display available; XXX should never happen */
    DISPLAY_TYPE_NONE = 0,
    /** Generic display interface without CABC */
    DISPLAY_TYPE_GENERIC = 1,
    /** EID l4f00311 with CABC */
    DISPLAY_TYPE_L4F00311 = 2,
    /** Sony acx565akm with CABC */
    DISPLAY_TYPE_ACX565AKM = 3,
    /** Taal display */
    DISPLAY_TYPE_TAAL = 4,
    /** Himalaya display */
    DISPLAY_TYPE_HIMALAYA = 5,
    /** Generic display name */
    DISPLAY_TYPE_DISPLAY0 = 6,
    /** Generic name for ACPI-controlled displays */
    DISPLAY_TYPE_ACPI_VIDEO0 = 7
} display_type_t;

/** Inhibit type */
typedef enum {
    /** Inhibit value invalid */
    INHIBIT_INVALID = -1,
    /** No inhibit */
    INHIBIT_OFF = 0,
    /** Default value */
    DEFAULT_BLANKING_INHIBIT_MODE = INHIBIT_OFF,
    /** Inhibit blanking; always keep on if charger connected */
    INHIBIT_STAY_ON_WITH_CHARGER = 1,
    /** Inhibit blanking; always keep on or dimmed if charger connected */
    INHIBIT_STAY_DIM_WITH_CHARGER = 2,
    /** Inhibit blanking; always keep on */
    INHIBIT_STAY_ON = 3,
    /** Inhibit blanking; always keep on or dimmed */
    INHIBIT_STAY_DIM = 4,
} inhibit_t;

/**
 * CABC mapping; D-Bus API modes vs SysFS mode
 */
typedef struct {
    /** CABC mode D-Bus name */
    const gchar *const dbus;
    /** CABC mode SysFS name */
    const gchar *const sysfs;
    /** CABC mode available */
    gboolean available;
} cabc_mode_mapping_t;

/** UpdatesEnabled state for UI */
typedef enum
{
    RENDERER_ERROR    = -2,
    RENDERER_UNKNOWN  = -1,
    RENDERER_DISABLED =  0,
    RENDERER_ENABLED  =  1,
} renderer_state_t;

/** State information for frame buffer resume waiting */
typedef struct
{
    /** frame buffer suspended flag */
    bool suspended;

    /** worker thread id */
    pthread_t thread;

    /** worker thread done flag */
    bool finished;

    /** path to fb wakeup event file */
    const char *wake_path;

    /** wakeup file descriptor */
    int         wake_fd;

    /** path to fb sleep event file */
    const char *sleep_path;

    /** sleep file descriptor */
    int         sleep_fd;

    /** write end of wakeup mainloop pipe */
    int  pipe_fd;

    /** pipe reader io watch id */
    guint     pipe_id;
} waitfb_t;

/** Possible values for bootstate */
typedef enum {
    BOOTSTATE_UNKNOWN,
    BOOTSTATE_USER,
    BOOTSTATE_ACT_DEAD,
} bootstate_t;

/** Content and where to write it */
typedef struct governor_setting_t
{
    /** Path (or rather glob pattern) to file where to write */
    char *path;

    /** Data to write */
    char *data;
} governor_setting_t;

/** Display state machine states */
typedef enum
{
    STM_UNSET,
    STM_RENDERER_INIT_START,
    STM_RENDERER_WAIT_START,
    STM_ENTER_POWER_ON,
    STM_STAY_POWER_ON,
    STM_LEAVE_POWER_ON,
    STM_RENDERER_INIT_STOP,
    STM_RENDERER_WAIT_STOP,
    STM_WAIT_FADE_TO_BLACK,
    STM_WAIT_FADE_TO_TARGET,
    STM_INIT_SUSPEND,
    STM_WAIT_SUSPEND,
    STM_ENTER_POWER_OFF,
    STM_STAY_POWER_OFF,
    STM_LEAVE_POWER_OFF,
    STM_INIT_RESUME,
    STM_WAIT_RESUME,
    STM_ENTER_LOGICAL_OFF,
    STM_STAY_LOGICAL_OFF,
    STM_LEAVE_LOGICAL_OFF,
} stm_state_t;

/** Delays for display blank/unblank related debug led patterns [ms] */
enum
{
    /** How long to wait for framebuffer sleep/wake event from kernel */
    LED_DELAY_FB_SUSPEND_RESUME = 1000,

    /** How long to wait dbus method call reply from lipstick */
    LED_DELAY_UI_DISABLE_ENABLE_MINIMUM = 3000,

    /** How long to initially wait dbus method call reply from lipstick */
    LED_DELAY_UI_DISABLE_ENABLE_MAXIMUM = 15000,
};

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * MISC_UTILS
 * ------------------------------------------------------------------------- */

static inline bool         mdy_str_eq_p(const char *s1, const char *s2);
static const char         *blanking_pause_mode_repr(blanking_pause_mode_t mode);

/* ------------------------------------------------------------------------- *
 * SHUTDOWN
 * ------------------------------------------------------------------------- */

static bool                mdy_shutdown_in_progress(void);

/* ------------------------------------------------------------------------- *
 * DATAPIPE_TRACKING
 * ------------------------------------------------------------------------- */

static void                mdy_datapipe_packagekit_locked_cb(gconstpointer data);;
static void                mdy_datapipe_system_state_cb(gconstpointer data);
static void                mdy_datapipe_submode_cb(gconstpointer data);
static void                mdy_datapipe_mdy_datapipe_lid_cover_policy_cb(gconstpointer data);
static gpointer            mdy_datapipe_display_state_filter_cb(gpointer data);
static void                mdy_datapipe_display_state_cb(gconstpointer data);
static void                mdy_datapipe_display_state_next_cb(gconstpointer data);
static void                mdy_datapipe_keyboard_slide_input_cb(gconstpointer const data);
static void                mdy_datapipe_display_brightness_cb(gconstpointer data);
static void                mdy_datapipe_lpm_brightness_cb(gconstpointer data);
static void                mdy_datapipe_display_state_req_cb(gconstpointer data);
static void                mdy_datapipe_audio_route_cb(gconstpointer data);
static void                mdy_datapipe_charger_state_cb(gconstpointer data);
static void                mdy_datapipe_exception_state_cb(gconstpointer data);
static void                mdy_datapipe_alarm_ui_state_cb(gconstpointer data);
static void                mdy_datapipe_proximity_sensor_cb(gconstpointer data);
static void                mdy_datapipe_power_saving_mode_cb(gconstpointer data);
static void                mdy_datapipe_call_state_trigger_cb(gconstpointer data);
static void                mdy_datapipe_device_inactive_cb(gconstpointer data);
static void                mdy_datapipe_orientation_state_cb(gconstpointer data);
static void                mdy_datapipe_shutting_down_cb(gconstpointer aptr);

static void                mdy_datapipe_init(void);
static void                mdy_datapipe_quit(void);

/* ------------------------------------------------------------------------- *
 * FBDEV_FD
 * ------------------------------------------------------------------------- */

static void                mdy_fbdev_rethink(void);

/* ------------------------------------------------------------------------- *
 * HIGH_BRIGHTNESS_MODE
 * ------------------------------------------------------------------------- */

static void                mdy_hbm_set_level(int number);

static gboolean            mdy_hbm_timeout_cb(gpointer data);
static void                mdy_hbm_cancel_timeout(void);
static void                mdy_hbm_schedule_timeout(void);

static void                mdy_hbm_rethink(void);

/* ------------------------------------------------------------------------- *
 * BACKLIGHT_BRIGHTNESS
 * ------------------------------------------------------------------------- */

typedef enum
{
    /** No brightness fading */
    FADER_IDLE,

    /** Normal brightness fading */
    FADER_DEFAULT,

    /** Fading to MCE_DISPLAY_DIM */
    FADER_DIMMING,

    /** Fading due to ALS adjustment */
    FADER_ALS,

    /** Fading to DISPLAY_OFF */
    FADER_BLANK,

    /** Fading from DISPLAY_OFF */
    FADER_UNBLANK,

    FADER_NUMOF
} fader_type_t;

static const char *
fader_type_name(fader_type_t type)
{
    static const char * const lut[FADER_NUMOF] =
    {
        [FADER_IDLE]          = "IDLE",
        [FADER_DEFAULT]       = "DEFAULT",
        [FADER_DIMMING]       = "DIMMING",
        [FADER_ALS]           = "ALS",
        [FADER_BLANK]         = "BLANK",
        [FADER_UNBLANK]       = "UNBLANK",
    };
    return (type < FADER_NUMOF) ? lut[type] : "INVALID";
}

#ifdef ENABLE_HYBRIS
static void                mdy_brightness_set_level_hybris(int number);
#endif
static void                mdy_brightness_set_level_default(int number);
static void                mdy_brightness_set_level(int number);

static void                mdy_brightness_fade_continue_with_als(fader_type_t fader_type);
static void                mdy_brightness_force_level(int number);

static void                mdy_brightness_set_priority_boost(bool enable);

static gboolean            mdy_brightness_fade_timer_cb(gpointer data);
static void                mdy_brightness_cleanup_fade_timer(void);
static void                mdy_brightness_stop_fade_timer(void);
static void                mdy_brightness_start_fade_timer(fader_type_t type, gint step_time);
static bool                mdy_brightness_fade_is_active(void);
static bool                mdy_brightness_is_fade_allowed(fader_type_t type);

static void                mdy_brightness_set_fade_target_ex(fader_type_t type, gint new_brightness, gint transition_time);
static void                mdy_brightness_set_fade_target_default(gint new_brightness);
static void                mdy_brightness_set_fade_target_dimming(gint new_brightness);
static void                mdy_brightness_set_fade_target_als(gint new_brightness);
static void                mdy_brightness_set_fade_target_blank(void);
static void                mdy_brightness_set_fade_target_unblank(gint new_brightness);

static int                 mdy_brightness_get_dim_static(void);
static int                 mdy_brightness_get_dim_dynamic(void);
static int                 mdy_brightness_get_dim_threshold_lo(void);
static int                 mdy_brightness_get_dim_threshold_hi(void);

static void                mdy_brightness_set_on_level(gint hbm_and_level);
static void                mdy_brightness_set_dim_level(void);
static void                mdy_brightness_set_lpm_level(gint level);

static void                mdy_brightness_init(void);

/* ------------------------------------------------------------------------- *
 * UI_SIDE_DIMMING
 * ------------------------------------------------------------------------- */

static void                mdy_ui_dimming_set_level(int level);
static void                mdy_ui_dimming_rethink(void);

/* ------------------------------------------------------------------------- *
 * CONTENT_ADAPTIVE_BACKLIGHT_CONTROL
 * ------------------------------------------------------------------------- */

static void                mdy_cabc_mode_set(const gchar *const mode);

/* ------------------------------------------------------------------------- *
 * BOOTUP_LED_PATTERN
 * ------------------------------------------------------------------------- */

static void                mdy_poweron_led_rethink(void);
static gboolean            mdy_poweron_led_rethink_cb(gpointer aptr);
static void                mdy_poweron_led_rethink_cancel(void);
static void                mdy_poweron_led_rethink_schedule(void);

/* ------------------------------------------------------------------------- *
 * AUTOMATIC_BLANKING
 * ------------------------------------------------------------------------- */

/** Maximum dim timeout applicable in ACTDEAD mode [s] */
#define ACTDEAD_MAX_DIM_TIMEOUT 15

/** Maximum blank timeout applicable in ACTDEAD mode [s] */
#define ACTDEAD_MAX_OFF_TIMEOUT 3

static void                mdy_blanking_update_inactivity_timeout(void);
static guint               mdy_blanking_find_dim_timeout_index(gint dim_timeout);
static gboolean            mdy_blanking_can_blank_from_low_power_mode(void);

// display timer: ON -> DIM

static gboolean            mdy_blanking_dim_cb(gpointer data);
static void                mdy_blanking_cancel_dim(void);
static void                mdy_blanking_schedule_dim(void);

static gboolean            mdy_blanking_inhibit_broadcast_cb(gpointer aptr);
static void                mdy_blanking_inhibit_cancel_broadcast(void);
static void                mdy_blanking_inhibit_schedule_broadcast(void);

// display timer: DIM -> OFF

static gboolean            mdy_blanking_off_cb(gpointer data);
static void                mdy_blanking_cancel_off(void);
static void                mdy_blanking_schedule_off(void);

// display timer: LPM_ON -> LPM_OFF (when proximity covered)

static gboolean            mdy_blanking_lpm_off_cb(gpointer data);
static void                mdy_blanking_cancel_lpm_off(void);
static void                mdy_blanking_schedule_lpm_off(void);

// blanking pause period: inhibit automatic ON -> DIM transitions

static gboolean            mdy_blanking_pause_period_cb(gpointer data);
static void                mdy_blanking_stop_pause_period(void);
static void                mdy_blanking_start_pause_period(void);

static bool                mdy_blanking_is_paused(void);
static bool                mdy_blanking_pause_can_dim(void);
static bool                mdy_blanking_pause_is_allowed(void);

static void                mdy_blanking_add_pause_client(const gchar *name);
static gboolean            mdy_blanking_remove_pause_client(const gchar *name);
static void                mdy_blanking_remove_pause_clients(void);
static gboolean            mdy_blanking_pause_client_lost_cb(DBusMessage *const msg);

// adaptive dimming period: dimming timeouts get longer on ON->DIM->ON transitions

static gboolean            mdy_blanking_adaptive_dimming_cb(gpointer data);
static void                mdy_blanking_start_adaptive_dimming(void);
static void                mdy_blanking_stop_adaptive_dimming(void);

// display timer: all of em
static bool                mdy_blanking_inhibit_off_p(void);
static bool                mdy_blanking_inhibit_dim_p(void);
static void                mdy_blanking_rethink_timers(bool force);
static void                mdy_blanking_rethink_proximity(void);
static void                mdy_blanking_cancel_timers(void);

// after boot blank prevent
static void                mdy_blanking_rethink_afterboot_delay(void);
static gint                mdy_blanking_get_afterboot_delay(void);

/* ------------------------------------------------------------------------- *
 * DISPLAY_TYPE_PROBING
 * ------------------------------------------------------------------------- */

static int                 mdy_display_type_glob_err_cb(const char *path, int err);
static gboolean            mdy_display_type_probe_brightness(const gchar *dirpath, char **setpath, char **maxpath);

static gboolean            mdy_display_type_get_from_config(display_type_t *display_type);
static gboolean            mdy_display_type_get_from_sysfs_probe(display_type_t *display_type);
static gboolean            mdy_display_type_get_from_hybris(display_type_t *display_type);
static display_type_t      mdy_display_type_get(void);

/* ------------------------------------------------------------------------- *
 * FBDEV_SLEEP_AND_WAKEUP
 * ------------------------------------------------------------------------- */

#ifdef ENABLE_WAKELOCKS
static gboolean            mdy_waitfb_event_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr);
static void               *mdy_waitfb_thread_entry(void *aptr);
static gboolean            mdy_waitfb_thread_start(waitfb_t *self);
static void                mdy_waitfb_thread_stop(waitfb_t *self);
#endif

/* ------------------------------------------------------------------------- *
 * COMPOSITOR_IPC
 * ------------------------------------------------------------------------- */

static void                mdy_compositor_set_killer_led(bool enable);

static void                mdy_compositor_set_panic_led(renderer_state_t req);
static gboolean            mdy_compositor_panic_led_cb(gpointer aptr);
static void                mdy_compositor_cancel_panic_led(void);
static void                mdy_compositor_schedule_panic_led(renderer_state_t req);

static gboolean            mdy_compositor_kill_verify_cb(gpointer aptr);
static gboolean            mdy_compositor_kill_kill_cb(gpointer aptr);
static gboolean            mdy_compositor_kill_core_cb(gpointer aptr);

static void                mdy_compositor_schedule_killer(void);
static void                mdy_compositor_cancel_killer(void);

static void                mdy_compositor_name_owner_pid_cb(const char *name, int pid);

static bool                mdy_compositor_is_available(void);

static void                mdy_compositor_name_owner_set(const char *curr);

static void                mdy_compositor_state_req_cb(DBusPendingCall *pending, void *user_data);
static void                mdy_compositor_cancel_state_req(void);
static gboolean            mdy_compositor_start_state_req(renderer_state_t state);

/* ------------------------------------------------------------------------- *
 * CALLSTATE_CHANGES
 * ------------------------------------------------------------------------- */

enum
{
    /** Default duration for blocking suspend after call_state changes */
    CALLSTATE_CHANGE_BLOCK_SUSPEND_DEFAULT_MS = 5 * 1000,

    /** Duration for blocking suspend after call_state changes to active */
    CALLSTATE_CHANGE_BLOCK_SUSPEND_ACTIVE_MS  = 60 * 1000,
};

static gboolean mdy_callstate_end_changed_cb(gpointer aptr);
static bool     mdy_callstate_changed_recently(void);
static void     mdy_callstate_clear_changed(void);
static void     mdy_callstate_set_changed(void);

/* ------------------------------------------------------------------------- *
 * AUTOSUSPEND_POLICY
 * ------------------------------------------------------------------------- */

#ifdef ENABLE_WAKELOCKS
static int                 mdy_autosuspend_get_allowed_level(void);
static void                mdy_autosuspend_gconf_cb(GConfClient *const client, const guint id, GConfEntry *const entry, gpointer const data);
#endif

/* ------------------------------------------------------------------------- *
 * ORIENTATION_ACTIVITY
 * ------------------------------------------------------------------------- */

static void                mdy_orientation_changed_cb(int state);
static void                mdy_orientation_generate_activity(void);
static bool                mdy_orientation_sensor_wanted(void);
static void                mdy_orientation_sensor_rethink(void);

/* ------------------------------------------------------------------------- *
 * DISPLAY_STATE
 * ------------------------------------------------------------------------- */

static void                mdy_display_state_changed(void);
static void                mdy_display_state_enter(display_state_t prev_state, display_state_t next_state);
static void                mdy_display_state_leave(display_state_t prev_state, display_state_t next_state);

/* ------------------------------------------------------------------------- *
 * FRAMEBUFFER_SUSPEND_RESUME
 * ------------------------------------------------------------------------- */

/** Framebuffer suspend/resume failure led patterns */
typedef enum
{
    FBDEV_LED_OFF,
    FBDEV_LED_SUSPENDING,
    FBDEV_LED_RESUMING,
} mdy_fbsusp_led_state_t;

static void                mdy_fbsusp_led_set(mdy_fbsusp_led_state_t req);
static gboolean            mdy_fbsusp_led_timer_cb(gpointer aptr);
static void                mdy_fbsusp_led_cancel_timer(void);
static void                mdy_fbsusp_led_start_timer(mdy_fbsusp_led_state_t req);

/* ------------------------------------------------------------------------- *
 * DISPLAY_STATE_MACHINE
 * ------------------------------------------------------------------------- */

// human readable state names
static const char         *mdy_stm_state_name(stm_state_t state);

// react to systemui availability changes
static void                mdy_datapipe_compositor_available_cb(gconstpointer aptr);
static void                mdy_datapipe_lipstick_available_cb(gconstpointer aptr);

// whether to power on/off the frame buffer
static bool                mdy_stm_display_state_needs_power(display_state_t state);

// early/late suspend and resume
static bool                mdy_stm_is_early_suspend_allowed(void);
static bool                mdy_stm_is_late_suspend_allowed(void);

static void                mdy_stm_start_fb_suspend(void);
static bool                mdy_stm_is_fb_suspend_finished(void);

static void                mdy_stm_start_fb_resume(void);
static bool                mdy_stm_is_fb_resume_finished(void);

static void                mdy_stm_release_wakelock(void);
static void                mdy_stm_acquire_wakelock(void);

// display_state changing
static void                mdy_stm_push_target_change(display_state_t next_state);
static bool                mdy_stm_pull_target_change(void);
static void                mdy_stm_finish_target_change(void);

// setUpdatesEnabled() from state machine
static bool                mdy_stm_is_renderer_pending(void);
static bool                mdy_stm_is_renderer_disabled(void);
static bool                mdy_stm_is_renderer_enabled(void);

static void                mdy_stm_disable_renderer(void);
static void                mdy_stm_enable_renderer(void);

// actual state machine
static void                mdy_stm_trans(stm_state_t state);
static void                mdy_stm_step(void);
static void                mdy_stm_exec(void);

// delayed state machine execution
static gboolean            mdy_stm_rethink_cb(gpointer aptr);
static void                mdy_stm_cancel_rethink(void);
static void                mdy_stm_schedule_rethink(void);
static void                mdy_stm_force_rethink(void);

/* ------------------------------------------------------------------------- *
 * CPU_SCALING_GOVERNOR
 * ------------------------------------------------------------------------- */

static governor_setting_t *mdy_governor_get_settings(const char *tag);
static void                mdy_governor_free_settings(governor_setting_t *settings);
static bool                mdy_governor_write_data(const char *path, const char *data);
static void                mdy_governor_apply_setting(const governor_setting_t *setting);
static void                mdy_governor_set_state(int state);
static void                mdy_governor_rethink(void);
static void                mdy_governor_conf_cb(GConfClient *const client, const guint id, GConfEntry *const entry, gpointer const data);

/* ------------------------------------------------------------------------- *
 * DBUS_HANDLERS
 * ------------------------------------------------------------------------- */

static gboolean            mdy_dbus_send_blanking_pause_status(DBusMessage *const method_call);
static gboolean            mdy_dbus_handle_blanking_pause_get_req(DBusMessage *const msg);

static gboolean            mdy_dbus_send_blanking_inhibit_status(DBusMessage *const method_call);
static gboolean            mdy_dbus_handle_blanking_inhibit_get_req(DBusMessage *const msg);

static void                mdy_dbus_invalidate_display_status(void);
static gboolean            mdy_dbus_send_display_status(DBusMessage *const method_call);
static const char         *mdy_dbus_get_reason_to_block_display_on(void);

static void                mdy_dbus_handle_display_state_req(display_state_t state);
static gboolean            mdy_dbus_handle_display_on_req(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_display_dim_req(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_display_off_req(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_display_lpm_req(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_display_status_get_req(DBusMessage *const msg);

static gboolean            mdy_dbus_send_cabc_mode(DBusMessage *const method_call);
static gboolean            mdy_dbus_handle_cabc_mode_owner_lost_sig(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_cabc_mode_get_req(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_cabc_mode_set_req(DBusMessage *const msg);

static gboolean            mdy_dbus_handle_blanking_pause_start_req(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_blanking_pause_cancel_req(DBusMessage *const msg);

static gboolean            mdy_dbus_handle_desktop_started_sig(DBusMessage *const msg);

static void                mdy_dbus_init(void);
static void                mdy_dbus_quit(void);

/* ------------------------------------------------------------------------- *
 * FLAG_FILE_TRACKING
 * ------------------------------------------------------------------------- */

static gboolean            mdy_flagfiles_desktop_ready_cb(gpointer user_data);
static void                mdy_flagfiles_bootstate_cb(const char *path, const char *file, gpointer data);
static void                mdy_flagfiles_init_done_cb(const char *path, const char *file, gpointer data);
static void                mdy_flagfiles_update_mode_cb(const char *path, const char *file, gpointer data);
static void                mdy_flagfiles_start_tracking(void);
static void                mdy_flagfiles_stop_tracking(void);

/* ------------------------------------------------------------------------- *
 * GCONF_SETTINGS
 * ------------------------------------------------------------------------- */

static void                mdy_gconf_cb(GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);
static void                mdy_gconf_init(void);
static void                mdy_gconf_quit(void);
static void                mdy_gconf_sanitize_brightness_settings(void);
static void                mdy_gconf_sanitize_dim_timeouts(bool force_update);

/* ------------------------------------------------------------------------- *
 * MODULE_LOAD_UNLOAD
 * ------------------------------------------------------------------------- */

G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
G_MODULE_EXPORT void         g_module_unload(GModule *module);

/* ========================================================================= *
 * VARIABLES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * MODULE_LOAD_UNLOAD
 * ------------------------------------------------------------------------- */

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
    /** Name of the module */
    .name = MODULE_NAME,
    /** Module provides */
    .provides = provides,
    /** Module priority */
    .priority = 250
};

/* ------------------------------------------------------------------------- *
 * SHUTDOWN
 * ------------------------------------------------------------------------- */

/** Have we seen shutdown_ind signal or equivalent from dsme */
static bool mdy_shutdown_started_flag = false;

/** Start of shutdown timestamp */
static int64_t mdy_shutdown_started_tick = 0;

/** Are we already unloading the module? */
static gboolean mdy_unloading_module = FALSE;

/** Timer for waiting simulated desktop ready state */
static guint mdy_desktop_ready_id = 0;

/** Device is shutting down predicate
 */
static bool mdy_shutdown_in_progress(void)
{
    return mdy_shutdown_started_flag;
}

/* ------------------------------------------------------------------------- *
 * AUTOMATIC_BLANKING
 * ------------------------------------------------------------------------- */

/** ID for adaptive display dimming timer source */
static guint mdy_blanking_adaptive_dimming_cb_id = 0;

/** Index for the array of adaptive dimming timeout multipliers */
static guint mdy_adaptive_dimming_index = 0;

/** Display blank prevention timer */
static gint mdy_blank_prevent_timeout = BLANK_PREVENT_TIMEOUT;

/** File used to enable low power mode */
static gchar *mdy_low_power_mode_file = NULL;

/** Is display low power mode supported
 *
 * Since we now support proximity based fake lpm that does not
 * require special support from display hw / drivers, the logic
 * for mdy_low_power_mode_supported can be switched to: enabled
 * by default and disabled only in special cases.
 */
static gboolean mdy_low_power_mode_supported = TRUE;

/** Maximum number of monitored services that calls blanking pause */
#define BLANKING_PAUSE_MAX_MONITORED    5

/**
 * Index for the array of possible display dim timeouts
 */
static guint mdy_dim_timeout_index = 0;

/* ------------------------------------------------------------------------- *
 * HIGH_BRIGHTNESS_MODE
 * ------------------------------------------------------------------------- */

/** File used to set high brightness mode */
static output_state_t mdy_high_brightness_mode_output =
{
    .context = "high_brightness_mode",
    .truncate_file = TRUE,
    .close_on_exit = FALSE,
};

/** Is display high brightness mode supported */
static gboolean mdy_high_brightness_mode_supported = FALSE;

/* ------------------------------------------------------------------------- *
 * CONTENT_ADAPTIVE_BACKLIGHT_CONTROL
 * ------------------------------------------------------------------------- */

/** Is content adaptive brightness control supported */
static gboolean mdy_cabc_is_supported = FALSE;

/** File used to get the available CABC modes */
static gchar *mdy_cabc_available_modes_file = NULL;

/**
 * CABC mode (power save mode active) -- uses the SysFS mode names;
 * NULL to disable
 */
static const gchar *mdy_psm_cabc_mode = NULL;

/** CABC mode -- uses the SysFS mode names */
static const gchar *mdy_cabc_mode = DEFAULT_CABC_MODE;

/** File used to set the CABC mode */
static gchar *mdy_cabc_mode_file = NULL;

/** List of monitored CABC mode requesters */
static GSList *mdy_cabc_mode_monitor_list = NULL;

/* ------------------------------------------------------------------------- *
 * FLAG_FILE_TRACKING
 * ------------------------------------------------------------------------- */

/** Are we going to USER or ACT_DEAD */
static bootstate_t mdy_bootstate = BOOTSTATE_UNKNOWN;

/** Content change watcher for the bootstate flag file */
static filewatcher_t *mdy_bootstate_watcher = 0;

/** Is the init-done flag file present in the file system */
static gboolean mdy_init_done = FALSE;

/** Content change watcher for the init-done flag file */
static filewatcher_t *mdy_init_done_watcher = 0;

/** Is the update-mode flag file present in the file system */
static gboolean mdy_update_mode = FALSE;

/** Content change watcher for the update-mode flag file */
static filewatcher_t *mdy_update_mode_watcher = 0;

/* ------------------------------------------------------------------------- *
 * GCONF_SETTINGS
 * ------------------------------------------------------------------------- */

/** Setting for statically defined dimmed screen brightness */
static gint mdy_brightness_dim_static = DEFAULT_DISPLAY_DIM_STATIC_BRIGHTNESS;

/** GConf callback ID for mdy_brightness_dim_static */
static guint mdy_brightness_dim_static_gconf_id = 0;

/** Setting for dimmed screen brightness defined as portion of on brightness*/
static gint mdy_brightness_dim_dynamic = DEFAULT_DISPLAY_DIM_DYNAMIC_BRIGHTNESS;

/** GConf callback ID for mdy_brightness_dim_dynamic */
static guint mdy_brightness_dim_dynamic_gconf_id = 0;

/** Setting for start compositor dimming threshold */
static gint mdy_brightness_dim_compositor_lo = DEFAULT_DISPLAY_DIM_COMPOSITOR_LO;

/** GConf callback ID for mdy_brightness_dim_compositor_lo */
static guint mdy_brightness_dim_compositor_lo_gconf_id = 0;

/** Setting for use maximum compositor dimming threshold */
static gint mdy_brightness_dim_compositor_hi= DEFAULT_DISPLAY_DIM_COMPOSITOR_HI;

/** GConf callback ID for mdy_brightness_dim_compositor_hi */
static guint mdy_brightness_dim_compositor_hi_gconf_id = 0;

/** Setting for allowing dimming while blanking is paused */
static gint mdy_blanking_pause_mode = DEFAULT_BLANKING_PAUSE_MODE;

/** GConf callback ID for mdy_blanking_pause_mode */
static guint mdy_blanking_pause_mode_gconf_cb_id = 0;

/** Display blanking timeout setting */
static gint mdy_blank_timeout = DEFAULT_BLANK_TIMEOUT;

/** Display blanking timeout from lockscreen setting */
static gint mdy_blank_from_lockscreen_timeout = DEFAULT_BLANK_FROM_LOCKSCREEN_TIMEOUT;

/** Display blanking timeout from lpm-on setting */
static gint mdy_blank_from_lpm_on_timeout = DEFAULT_BLANK_FROM_LPM_ON_TIMEOUT;

/** Display blanking timeout from lpm-off setting */
static gint mdy_blank_from_lpm_off_timeout = DEFAULT_BLANK_FROM_LPM_OFF_TIMEOUT;

/** GConf callback ID for mdy_blank_timeout */
static guint mdy_blank_timeout_gconf_cb_id = 0;

/** GConf callback ID for mdy_blank_from_lockscreen_timeout */
static guint mdy_blank_from_lockscreen_timeout_gconf_cb_id = 0;

/** GConf callback ID for mdy_blank_from_lpm_on_timeout */
static guint mdy_blank_from_lpm_on_timeout_gconf_cb_id = 0;

/** GConf callback ID for mdy_blank_from_lpm_off_timeout */
static guint mdy_blank_from_lpm_off_timeout_gconf_cb_id = 0;

/** Number of brightness steps */
static gint mdy_brightness_step_count = DEFAULT_DISP_BRIGHTNESS_STEP_COUNT;

/** Size of one brightness step */
static gint mdy_brightness_step_size  = DEFAULT_DISP_BRIGHTNESS_STEP_SIZE;

/** display brightness setting; [1, mdy_brightness_step_count] */
static gint mdy_brightness_setting = DEFAULT_DISP_BRIGHTNESS;

/** timestamp of the latest brightness setting change */
static int64_t mdy_brightness_setting_change_time = 0;

/** GConf callback ID for mdy_brightness_step_count */
static guint mdy_brightness_step_count_gconf_id = 0;

/** GConf callback ID for mdy_brightness_step_size */
static guint mdy_brightness_step_size_gconf_id = 0;

/** GConf callback ID for mdy_brightness_setting */
static guint mdy_brightness_setting_gconf_id = 0;

/** GConf callback ID for auto-brightness setting */
static guint mdy_automatic_brightness_setting_gconf_id = 0;

/** PSM display brightness setting; [1, 5]
 *  or -1 when power save mode is not active
 *
 * (not in gconf, but kept close to mdy_brightness_setting)
 */
static gint mdy_psm_disp_brightness = -1;

/** Never blank display setting */
static gint mdy_disp_never_blank = DEFAULT_DISPLAY_NEVER_BLANK;

/** GConf callback ID for display never blank setting */
static guint mdy_disp_never_blank_gconf_cb_id = 0;

/** Use adaptive timeouts for dimming */
static gboolean mdy_adaptive_dimming_enabled = DEFAULT_ADAPTIVE_DIMMING_ENABLED;

/** GConf callback ID for display blanking timeout setting */
static guint mdy_adaptive_dimming_enabled_gconf_cb_id = 0;

/** Array of possible display dim timeouts */
static GSList *mdy_possible_dim_timeouts = NULL;

/** GConf callback ID for display blanking timeout setting */
static guint mdy_possible_dim_timeouts_gconf_cb_id = 0;

/** Threshold to use for adaptive timeouts for dimming in milliseconds */
static gint mdy_adaptive_dimming_threshold = DEFAULT_ADAPTIVE_DIMMING_THRESHOLD;

/** GConf callback ID for the threshold for adaptive display dimming */
static guint mdy_adaptive_dimming_threshold_gconf_cb_id = 0;

/** Display dimming timeout setting */
static gint mdy_disp_dim_timeout = DEFAULT_DIM_TIMEOUT;

/** GConf callback ID for display dimming timeout setting */
static guint mdy_disp_dim_timeout_gconf_cb_id = 0;

/** Use low power mode setting */
static gboolean mdy_use_low_power_mode = DEFAULT_USE_LOW_POWER_MODE;

/** GConf callback ID for low power mode setting */
static guint mdy_use_low_power_mode_gconf_cb_id = 0;

/** Display blanking inhibit mode */
static inhibit_t mdy_blanking_inhibit_mode = DEFAULT_BLANKING_INHIBIT_MODE;

/** Kbd slide display blanking inhibit mode */
static gint  mdy_kbd_slide_inhibit_mode       = DEFAULT_KBD_SLIDE_INHIBIT;
static guint mdy_kbd_slide_inhibit_mode_cb_id = 0;

/** GConf callback ID for display blanking inhibit mode setting */
static guint mdy_blanking_inhibit_mode_gconf_cb_id = 0;

/* ========================================================================= *
 * MISC_UTILS
 * ========================================================================= */

/** Null tolerant string equality predicate
 */
static inline bool mdy_str_eq_p(const char *s1, const char *s2)
{
    // Note: mdy_str_eq_p(NULL, NULL) -> false on purpose
    return (s1 && s2) ? !strcmp(s1, s2) : false;
}

/** Convert blanking_pause_mode_t enum to human readable string
 *
 * @param mode blanking_pause_mode_t enumeration value
 *
 * @return human readable representation of mode
 */
static const char *blanking_pause_mode_repr(blanking_pause_mode_t mode)
{
    const char *res = "unknown";

    switch( mode ) {
    case BLANKING_PAUSE_MODE_DISABLED:  res = "disabled"; break;
    case BLANKING_PAUSE_MODE_KEEP_ON:   res = "keep-on";  break;
    case BLANKING_PAUSE_MODE_ALLOW_DIM: res = "allow-dim"; break;
    default: break;
    }

    return res;
}

/* ========================================================================= *
 * DATAPIPE_TRACKING
 * ========================================================================= */

/** Cached lipstick availability; assume unknown */
static service_state_t lipstick_service_state = SERVICE_STATE_UNDEF;

/** PackageKit Locked property is set to true during sw updates */
static bool packagekit_locked = false;

/**
 * Handle packagekit_locked_pipe notifications
 *
 * @param data The locked state stored in a pointer
 */
static void mdy_datapipe_packagekit_locked_cb(gconstpointer data)
{
    bool prev = packagekit_locked;
    packagekit_locked = GPOINTER_TO_INT(data);

    if( packagekit_locked == prev )
        goto EXIT;

    /* Log by default as it might help analyzing upgrade problems */
    mce_log(LL_WARN, "packagekit_locked = %d", packagekit_locked);

    /* re-evaluate suspend policy */
    mdy_stm_schedule_rethink();

EXIT:
    return;
}

/* Cached system state */
static system_state_t system_state = MCE_STATE_UNDEF;

/**
 * Handle system_state_pipe notifications
 *
 * @param data The system state stored in a pointer
 */
static void mdy_datapipe_system_state_cb(gconstpointer data)
{
    system_state_t prev = system_state;
    system_state = GPOINTER_TO_INT(data);

    if( system_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "system_state: %s -> %s",
            system_state_repr(prev),
            system_state_repr(system_state));

    /* re-evaluate suspend policy */
    mdy_stm_schedule_rethink();

    /* Deal with ACTDEAD alarms / not in USER mode */
    mdy_fbdev_rethink();

#ifdef ENABLE_CPU_GOVERNOR
    mdy_governor_rethink();
#endif

    mdy_blanking_rethink_afterboot_delay();

EXIT:
    return;
}

/* Assume we are in mode transition when mce starts up */
static submode_t submode = MCE_TRANSITION_SUBMODE;

/**
 * Handle submode_pipe notifications
 *
 * @param data The submode stored in a pointer
 */
static void mdy_datapipe_submode_cb(gconstpointer data)
{
    submode_t prev = submode;
    submode = GPOINTER_TO_INT(data);

    if( submode == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "submode = %d", submode);

    /* Rethink dim/blank timers if tklock state changed */
    if( (prev ^ submode) & MCE_TKLOCK_SUBMODE )
        mdy_blanking_rethink_timers(false);

    submode_t old_trans = prev & MCE_TRANSITION_SUBMODE;
    submode_t new_trans = submode & MCE_TRANSITION_SUBMODE;

    if( old_trans && !new_trans ) {
        /* End of transition; stable state reached */

        // force blanking timer reprogramming
        mdy_blanking_rethink_timers(true);
    }

EXIT:
    return;
}

/** Cache Lid cover policy state; assume unknown
 */
static cover_state_t lid_cover_policy_state = COVER_UNDEF;

/** Change notifications from lid_cover_policy_pipe
 */
static void mdy_datapipe_mdy_datapipe_lid_cover_policy_cb(gconstpointer data)
{
    cover_state_t prev = lid_cover_policy_state;
    lid_cover_policy_state = GPOINTER_TO_INT(data);

    if( lid_cover_policy_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "lid_cover_policy_state = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(lid_cover_policy_state));
EXIT:
    return;
}

/* Cached current display state */
static display_state_t display_state = MCE_DISPLAY_UNDEF;

/* Cached target display state */
static display_state_t display_state_next = MCE_DISPLAY_UNDEF;

/** Filter display_state_req_pipe changes
 *
 * @param data The unfiltered display state stored in a pointer
 *
 * @return The filtered display state stored in a pointer
 */
static gpointer mdy_datapipe_display_state_filter_cb(gpointer data)
{
    display_state_t want_state = GPOINTER_TO_INT(data);
    display_state_t next_state = want_state;

    /* Handle never-blank override */
    if( mdy_disp_never_blank ) {
        next_state = MCE_DISPLAY_ON;
        goto UPDATE;
    }

    /* Handle update-mode override */
    if( mdy_update_mode ) {
        next_state = MCE_DISPLAY_ON;
        goto UPDATE;
    }

    /* Validate requested display state */
    switch( next_state ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_ON:
        break;

    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
        if( mdy_use_low_power_mode && mdy_low_power_mode_supported )
            break;

        mce_log(LL_DEBUG, "reject low power mode display request");
        next_state = MCE_DISPLAY_OFF;
        goto UPDATE;

    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
        mce_log(LL_WARN, "reject invalid display mode request");
        next_state = MCE_DISPLAY_OFF;
        goto UPDATE;
    }

    /* Allow display off / no change */
    if( next_state == MCE_DISPLAY_OFF || next_state == display_state )
        goto UPDATE;

    /* Keep existing state if display on requests are made during
     * mce/device startup and device shutdown/reboot. */
    if( system_state == MCE_STATE_UNDEF ) {
        /* But initial state = ON/OFF selection at display plugin
         * initialization must still be allowed */
        if( display_state == MCE_DISPLAY_UNDEF ) {
            if( next_state != MCE_DISPLAY_ON )
                next_state = MCE_DISPLAY_OFF;
        }
        else {
            mce_log(LL_WARN, "reject display mode request at start up");
            next_state = display_state;
        }
    }
    else if( (submode & MCE_TRANSITION_SUBMODE) &&
             ( system_state == MCE_STATE_SHUTDOWN ||
               system_state == MCE_STATE_REBOOT ) ) {
        mce_log(LL_WARN, "reject display mode request at shutdown/reboot");
        next_state = display_state;
    }

UPDATE:
    if( want_state != next_state ) {
        mce_log(LL_DEBUG, "requested: %s, granted: %s",
                display_state_repr(want_state),
                display_state_repr(next_state));
    }

    /* Note: An attempt to keep the current state can lead into this
     *       datapipe input filter returning transiend power up/down
     *       or undefined states. These must be ignored at the datapipe
     *       output handler display_state_req_pipe(). */

    return GINT_TO_POINTER(next_state);
}

/** Handle display_state_req_pipe notifications
 *
 * This is where display state transition starts
 *
 * @param data Requested display_state_t (as void pointer)
 */
static void mdy_datapipe_display_state_req_cb(gconstpointer data)
{
    display_state_t next_state = GPOINTER_TO_INT(data);
    switch( next_state ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_ON:
        /* Feed valid stable states into the state machine */
        mdy_stm_push_target_change(next_state);
        break;

    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
        /* Ignore transient or otherwise invalid display states */

        /* Any "no-change" transient state requests practically
         * have to be side effects of display state request
         * filtering - no need to make fuzz about them */
        if( next_state == display_state )
            break;

        mce_log(LL_WARN, "%s is not valid target state; ignoring",
                display_state_repr(next_state));
        break;
    }
}

/** Handle display_state_pipe notifications
 *
 * This is where display state transition ends
 *
 * @param data The display state stored in a pointer
 */
static void mdy_datapipe_display_state_cb(gconstpointer data)
{
    display_state_t prev = display_state;
    display_state = GPOINTER_TO_INT(data);

    if( display_state == prev )
        goto EXIT;

    mdy_blanking_inhibit_schedule_broadcast();

EXIT:
    return;
}

/** Handle display_state_next_pipe notifications
 *
 * This is where display state transition ends
 *
 * @param data The display state stored in a pointer
 */
static void mdy_datapipe_display_state_next_cb(gconstpointer data)
{
    display_state_t prev = display_state_next;
    display_state_next = GPOINTER_TO_INT(data);

    if( display_state_next == prev )
        goto EXIT;

    mdy_ui_dimming_rethink();

    /* Start/stop orientation sensor */
    mdy_orientation_sensor_rethink();

    mdy_blanking_rethink_afterboot_delay();

    mdy_dbus_send_display_status(0);

EXIT:
    return;
}

/** Keypad slide input state; assume closed */
static cover_state_t kbd_slide_input_state = COVER_CLOSED;

/** Change notifications from keyboard_slide_pipe
 */
static void mdy_datapipe_keyboard_slide_input_cb(gconstpointer const data)
{
    cover_state_t prev = kbd_slide_input_state;
    kbd_slide_input_state = GPOINTER_TO_INT(data);

    if( kbd_slide_input_state == COVER_UNDEF )
        kbd_slide_input_state = COVER_CLOSED;

    if( kbd_slide_input_state == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "kbd_slide_input_state = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(kbd_slide_input_state));

    /* force blanking reprogramming */
    mdy_blanking_rethink_timers(true);

EXIT:
    return;
}

/** Handle display_brightness_pipe notifications
 *
 * @note A brightness request is only sent if the value changed
 * @param data The display brightness stored in a pointer
 */
static void mdy_datapipe_display_brightness_cb(gconstpointer data)
{
    static gint curr = -1;

    gint prev = curr;
    curr = GPOINTER_TO_INT(data);

    if( curr == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "brightness: %d -> %d", prev, curr);

    mdy_brightness_set_on_level(curr);

EXIT:
    return;
}

/** Handle lpm_brightness_pipe notifications
 *
 * @note A brightness request is only sent if the value changed
 *
 * @param data The display brightness stored in a pointer
 */
static void mdy_datapipe_lpm_brightness_cb(gconstpointer data)
{
    static gint curr = -1;

    gint prev = curr;
    curr = GPOINTER_TO_INT(data);

    mce_log(LL_DEBUG, "input: %d -> %d", prev, curr);

    if( curr == prev )
        goto EXIT;

    mdy_brightness_set_lpm_level(curr);

EXIT:
    return;
}

/* Cached audio routing state */
static audio_route_t audio_route = AUDIO_ROUTE_HANDSET;

/** Handle audio_route_pipe notifications
 */
static void mdy_datapipe_audio_route_cb(gconstpointer data)
{
    audio_route_t prev = audio_route;
    audio_route = GPOINTER_TO_INT(data);

    if( audio_route == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "audio_route = %d", audio_route);
    mdy_blanking_rethink_timers(false);

EXIT:
    return;
}

/** Cached charger connection state */
static charger_state_t charger_state = CHARGER_STATE_UNDEF;

/** Handle charger_state_pipe notifications
 *
 * @param data TRUE if the charger was connected,
 *             FALSE if the charger was disconnected
 */
static void mdy_datapipe_charger_state_cb(gconstpointer data)
{
    charger_state_t prev = charger_state;
    charger_state = GPOINTER_TO_INT(data);

    if( charger_state == prev )
        goto EXIT;

    mdy_blanking_rethink_timers(false);

EXIT:
    return;
}

/** Cached exceptional ui state */
static uiexctype_t exception_state = UIEXC_NONE;

/** Handle exception_state_pipe notifications
 */
static void mdy_datapipe_exception_state_cb(gconstpointer data)
{
    uiexctype_t prev = exception_state;
    exception_state = GPOINTER_TO_INT(data);

    if( exception_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "exception_state = %d", exception_state);

    // normal on->dim->blank might not be applicable
    mdy_blanking_rethink_timers(false);

    // notification exception state blocks suspend
    mdy_stm_schedule_rethink();

EXIT:
    return;
}

/** Cached alarm ui state */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_INVALID_INT32;

/** Handle alarm_ui_state_pipe notifications
 *
 * @param data Unused
 */
static void mdy_datapipe_alarm_ui_state_cb(gconstpointer data)
{
    alarm_ui_state_t prev =  alarm_ui_state;
    alarm_ui_state = GPOINTER_TO_INT(data);

    if( alarm_ui_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "alarm_ui_state: %s -> %s",
            alarm_state_repr(prev),
            alarm_state_repr(alarm_ui_state));

    /* Act dead alarm ui does not implement compositor service.
     * Open/close fbdevice as if compositor were started/stopped
     * based on alarm ui state changes */
    mdy_fbdev_rethink();

    mdy_blanking_rethink_timers(false);

    // suspend policy
    mdy_stm_schedule_rethink();
EXIT:
    return;
}

/** Cached proximity sensor state */
static cover_state_t proximity_state = COVER_UNDEF;

/** Handle proximity_sensor_pipe notifications
 *
 * @param data Unused
 */
static void mdy_datapipe_proximity_sensor_cb(gconstpointer data)
{
    cover_state_t prev = proximity_state;
    proximity_state = GPOINTER_TO_INT(data);

    if( proximity_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "proximity_state = %s",
            proximity_state_repr(proximity_state));

    /* handle toggling between LPM_ON and LPM_OFF */
    mdy_blanking_rethink_proximity();

EXIT:
    return;
}

/** Cached power saving mode state */
static gboolean power_saving_mode = false;

/** Handle power_saving_mode_pipe notifications
 *
 * @param data Unused
 */
static void mdy_datapipe_power_saving_mode_cb(gconstpointer data)
{
    gboolean prev = power_saving_mode;
    power_saving_mode = GPOINTER_TO_INT(data);

    if( power_saving_mode == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "power_saving_mode = %d", power_saving_mode);

    if( power_saving_mode ) {
        /* Override the CABC mode and brightness setting */
        mdy_psm_cabc_mode = DEFAULT_PSM_CABC_MODE;
        mdy_psm_disp_brightness = mce_xlat_int(1,100, 1,20,
                                               mdy_brightness_setting);

        execute_datapipe(&display_brightness_pipe,
                         GINT_TO_POINTER(mdy_psm_disp_brightness),
                         USE_INDATA, CACHE_INDATA);

        execute_datapipe(&lpm_brightness_pipe,
                         GINT_TO_POINTER(mdy_psm_disp_brightness),
                         USE_INDATA, CACHE_INDATA);

        mdy_cabc_mode_set(mdy_psm_cabc_mode);
    } else {
        /* Restore the CABC mode and brightness setting */
        mdy_psm_cabc_mode = NULL;
        mdy_psm_disp_brightness = -1;

        execute_datapipe(&display_brightness_pipe,
                         GINT_TO_POINTER(mdy_brightness_setting),
                         USE_INDATA, CACHE_INDATA);

        execute_datapipe(&lpm_brightness_pipe,
                         GINT_TO_POINTER(mdy_brightness_setting),
                         USE_INDATA, CACHE_INDATA);

        mdy_cabc_mode_set(mdy_cabc_mode);
    }
EXIT:
    return;
}

/** Cached call state */
static call_state_t call_state = CALL_STATE_INVALID;

/** Handle call_state_pipe notifications
 *
 * @param data Unused
 */
static void mdy_datapipe_call_state_trigger_cb(gconstpointer data)
{
    call_state_t prev = call_state;
    call_state = GPOINTER_TO_INT(data);

    if( call_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "call_state = %s", call_state_repr(call_state));

    mdy_blanking_rethink_timers(false);

    // autosuspend policy
    mdy_callstate_set_changed();

EXIT:
    return;
}

/** Cached inactivity state */
static gboolean device_inactive = FALSE;

/** Handle device_inactive_pipe notifications
 *
 * @param data The inactivity stored in a pointer;
 *             TRUE if the device is inactive,
 *             FALSE if the device is active
 */
static void mdy_datapipe_device_inactive_cb(gconstpointer data)
{
    device_inactive = GPOINTER_TO_INT(data);

    /* while inactivity can be considered a "state",
     * activity is more like "event", i.e. it needs
     * to be handled without paying attention to
     * previous inactivity value */

    mce_log(LL_DEBUG, "device_inactive = %d", device_inactive);

    if( device_inactive )
        goto EXIT;

    /* Adjust the adaptive dimming timeouts,
     * even if we don't use them
     */
    if (mdy_blanking_adaptive_dimming_cb_id != 0) {
        if (g_slist_nth(mdy_possible_dim_timeouts,
                        mdy_dim_timeout_index +
                        mdy_adaptive_dimming_index + 1) != NULL)
            mdy_adaptive_dimming_index++;
    }

    switch( display_state ) {
    case MCE_DISPLAY_ON:
        /* Explicitly reset the display dim timer */
        mdy_blanking_rethink_timers(true);
        break;

    case MCE_DISPLAY_OFF:
        /* Activity alone will not make OFF->ON transition.
         * Except in act dead, where display is not really off
         * and thus double tap detection is not active ... */

        if( system_state != MCE_STATE_ACTDEAD )
            break;

        // fall through

    case MCE_DISPLAY_DIM:
        /* DIM->ON on device activity */
        mce_log(LL_NOTICE, "display on due to activity");
        execute_datapipe(&display_state_req_pipe,
                         GINT_TO_POINTER(MCE_DISPLAY_ON),
                         USE_INDATA, CACHE_INDATA);
        break;

    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
        break;
    }

EXIT:
    return;
}

/** Cached Orientation Sensor value */
static orientation_state_t orientation_state = MCE_ORIENTATION_UNDEFINED;

/** Handle orientation_sensor_pipe notifications
 *
 * @param data The orientation state stored in a pointer
 */
static void mdy_datapipe_orientation_state_cb(gconstpointer data)
{
    orientation_state_t prev = orientation_state;
    orientation_state = GPOINTER_TO_INT(data);

    if( orientation_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "orientation_state = %s",
            orientation_state_repr(orientation_state));

    /* Ignore sensor power up/down */
    if( prev == MCE_ORIENTATION_UNDEFINED ||
        orientation_state ==  MCE_ORIENTATION_UNDEFINED )
        goto EXIT;

    mdy_orientation_generate_activity();
EXIT:
    return;
}

/** React to shutdown-in-progress state changes
 */
static void mdy_datapipe_shutting_down_cb(gconstpointer aptr)
{
    bool prev = mdy_shutdown_started_flag;
    mdy_shutdown_started_flag = GPOINTER_TO_INT(aptr);

    if( mdy_shutdown_started_flag == prev )
        goto EXIT;

    if( mdy_shutdown_started_flag ) {
        mce_log(LL_DEBUG, "Shutdown started");

        /* Cache start of shutdown time stamp */
        mdy_shutdown_started_tick = mce_lib_get_boot_tick();
    }
    else {
        mce_log(LL_DEBUG, "Shutdown canceled");
    }

    /* Framebuffer must be kept open during shutdown */
    mdy_fbdev_rethink();

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t mdy_datapipe_handlers[] =
{
    // input triggers
    {
        .datapipe  = &display_state_pipe,
        .input_cb  = mdy_datapipe_display_state_cb,
    },
    {
        .datapipe  = &display_state_next_pipe,
        .input_cb  = mdy_datapipe_display_state_next_cb,
    },
    {
        .datapipe  = &keyboard_slide_pipe,
        .input_cb  = mdy_datapipe_keyboard_slide_input_cb,
    },
    // output triggers
    {
        .datapipe  = &display_state_req_pipe,
        .output_cb = mdy_datapipe_display_state_req_cb,
    },
    {
        .datapipe  = &display_brightness_pipe,
        .output_cb = mdy_datapipe_display_brightness_cb,
    },
    {
        .datapipe  = &lpm_brightness_pipe,
        .output_cb = mdy_datapipe_lpm_brightness_cb,
    },
    {
        .datapipe  = &charger_state_pipe,
        .output_cb = mdy_datapipe_charger_state_cb,
    },
    {
        .datapipe  = &system_state_pipe,
        .output_cb = mdy_datapipe_system_state_cb,
    },
    {
        .datapipe  = &orientation_sensor_pipe,
        .output_cb = mdy_datapipe_orientation_state_cb,
    },
    {
        .datapipe  = &submode_pipe,
        .output_cb = mdy_datapipe_submode_cb,
    },
    {
        .datapipe  = &device_inactive_pipe,
        .output_cb = mdy_datapipe_device_inactive_cb,
    },
    {
        .datapipe  = &call_state_pipe,
        .output_cb = mdy_datapipe_call_state_trigger_cb,
    },
    {
        .datapipe  = &power_saving_mode_pipe,
        .output_cb = mdy_datapipe_power_saving_mode_cb,
    },
    {
        .datapipe  = &proximity_sensor_pipe,
        .output_cb = mdy_datapipe_proximity_sensor_cb,
    },
    {
        .datapipe  = &alarm_ui_state_pipe,
        .output_cb = mdy_datapipe_alarm_ui_state_cb,
    },
    {
        .datapipe  = &exception_state_pipe,
        .output_cb = mdy_datapipe_exception_state_cb,
    },
    {
        .datapipe  = &audio_route_pipe,
        .output_cb = mdy_datapipe_audio_route_cb,
    },
    {
        .datapipe  = &packagekit_locked_pipe,
        .output_cb = mdy_datapipe_packagekit_locked_cb,
    },

    {
        .datapipe  = &compositor_available_pipe,
        .output_cb = mdy_datapipe_compositor_available_cb,
    },
    {
        .datapipe  = &lipstick_available_pipe,
        .output_cb = mdy_datapipe_lipstick_available_cb,
    },
    {
        .datapipe  = &lid_cover_policy_pipe,
        .output_cb = mdy_datapipe_mdy_datapipe_lid_cover_policy_cb,
    },
    {
        .datapipe  = &shutting_down_pipe,
        .output_cb = mdy_datapipe_shutting_down_cb,
    },

    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t mdy_datapipe_bindings =
{
    .module   = "display",
    .handlers = mdy_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void mdy_datapipe_init(void)
{
    // filters
    append_filter_to_datapipe(&display_state_req_pipe,
                              mdy_datapipe_display_state_filter_cb);

    // triggers
    datapipe_bindings_init(&mdy_datapipe_bindings);
}

/** Remove triggers/filters from datapipes */
static void mdy_datapipe_quit(void)
{
    // triggers
    datapipe_bindings_quit(&mdy_datapipe_bindings);

    // filters
    remove_filter_from_datapipe(&display_state_req_pipe,
                                mdy_datapipe_display_state_filter_cb);
}

/* ========================================================================= *
 * FBDEV_FD
 * ========================================================================= */

/** Decide whether frame buffer device should be kept open or not
 *
 * Having mce keep frame buffer device open during startup makes
 * it possible to make transition from boot logo to ui without
 * the display blanking in between.
 *
 * And similarly during shutdown/reboot the shutdown logo stays
 * visible after ui processes that drew it has been terminated.
 *
 * However we need to release the device file descriptor if ui
 * side happens to make unexpected exit, otherwise the stale
 * unresponsive ui would remain on screen.
 *
 * And act dead alarms are a special case, because there we have
 * ui that does compositor dbus inteface (act dead charging ui)
 * getting replaced with one that does not (act dead alarms ui).
 */
static void mdy_fbdev_rethink(void)
{
    // have we seen compositor since mce startup
    static bool compositor_was_available = false;

    // assume framebuffer device should be kept open
    bool can_close = false;

    // do not close if compositor is up
    if( mdy_compositor_is_available() ) {
        compositor_was_available = true;
        goto EXIT;
    }

    // do not close if compositor has not yet been up
    if( !compositor_was_available )
        goto EXIT;

    // do not close during shutdown
    if( mdy_shutdown_in_progress() )
        goto EXIT;

    if( system_state == MCE_STATE_ACTDEAD ) {
        // or when there are act dead alarms
        switch( alarm_ui_state ) {
        case MCE_ALARM_UI_RINGING_INT32:
        case MCE_ALARM_UI_VISIBLE_INT32:
            goto EXIT;
        default:
            break;
        }
    }
    else if( system_state != MCE_STATE_USER ) {
        // or we are not in USER/ACT_DEAD
        goto EXIT;
    }

    // and since the whole close + reopen is needed only to
    // clear the display when something potentially has left
    // stale ui on screen - we skip it if the display is not
    // firmly in powered up state
    if( display_state != display_state_next )
        goto EXIT;
    if( !mdy_stm_display_state_needs_power(display_state) )
        goto EXIT;

    can_close = true;

EXIT:

    if( can_close )
        mce_fbdev_close();
    else
        mce_fbdev_open();
}

/* ========================================================================= *
 * HIGH_BRIGHTNESS_MODE
 * ========================================================================= */

/** Cached high brightness mode; this is the logical value */
static gint mdy_hbm_level_wanted = 0;

/** High brightness mode; this is the last value written */
static gint mdy_hbm_level_written = -1;

/** ID for high brightness mode timer source */
static guint mdy_hbm_timeout_cb_id = 0;

/** Helper for updating high brightness state with bounds checking
 *
 * @param number high brightness mode [0 ... 2]
 */
static void mdy_hbm_set_level(int number)
{
    int minval = 0;
    int maxval = 2;

    /* Clip value to valid range */
    if( number < minval ) {
        mce_log(LL_ERR, "value=%d vs min=%d", number, minval);
        number = minval;
    }
    else if( number > maxval ) {
        mce_log(LL_ERR, "value=%d vs max=%d", number, maxval);
        number = maxval;
    }
    else
        mce_log(LL_DEBUG, "value=%d", number);

    /* Write unconditionally, but ... */
    mce_write_number_string_to_file(&mdy_high_brightness_mode_output, number);

    /* ... make a note of the last value written */
    mdy_hbm_level_written = number;
}

/**
 * Timeout callback for the high brightness mode
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean mdy_hbm_timeout_cb(gpointer data)
{
    (void)data;

    mce_log(LL_DEBUG, "HMB timer triggered");
    mdy_hbm_timeout_cb_id = 0;

    /* Disable high brightness mode */
    mdy_hbm_set_level(0);

    return FALSE;
}

/**
 * Cancel the high brightness mode timeout
 */
static void mdy_hbm_cancel_timeout(void)
{
    /* Remove the timeout source for the high brightness mode */
    if (mdy_hbm_timeout_cb_id != 0) {
        mce_log(LL_DEBUG, "HMB timer cancelled");
        g_source_remove(mdy_hbm_timeout_cb_id);
        mdy_hbm_timeout_cb_id = 0;
    }
}

/**
 * Setup the high brightness mode timeout
 */
static void mdy_hbm_schedule_timeout(void)
{
    gint timeout = DEFAULT_HBM_TIMEOUT;
    mdy_hbm_cancel_timeout();

    /* Setup new timeout */
    mce_log(LL_DEBUG, "HMB timer scheduled @ %d secs", timeout);
    mdy_hbm_timeout_cb_id = g_timeout_add_seconds(timeout,
                                                  mdy_hbm_timeout_cb, NULL);
}

/**
 * Update high brightness mode
 *
 * @param hbm_level The wanted high brightness mode level;
 *                  will be overridden if the display is dimmed/off
 *                  or if high brightness mode is not supported
 */
static void mdy_hbm_rethink(void)
{
    if (mdy_high_brightness_mode_supported == FALSE)
        goto EXIT;

    /* should not occur, but do nothing while in transition */
    if( display_state == MCE_DISPLAY_POWER_DOWN ||
        display_state == MCE_DISPLAY_POWER_UP ) {
        mce_log(LL_WARN, "hbm mode setting wile in transition");
        goto EXIT;
    }

    /* If the display is off or dimmed, disable HBM */
    if( display_state != MCE_DISPLAY_ON ) {
        if (mdy_hbm_level_written != 0) {
            mdy_hbm_set_level(0);
        }
    } else if( mdy_hbm_level_written != mdy_hbm_level_wanted ) {
        mdy_hbm_set_level(mdy_hbm_level_wanted);
    }

    /**
     * Half brightness mode should be disabled after a certain timeout
     */
    if( mdy_hbm_level_written <= 0 ) {
        mdy_hbm_cancel_timeout();
    } else if( mdy_hbm_timeout_cb_id == 0 ) {
        mdy_hbm_schedule_timeout();
    }

EXIT:
    return;
}

/* ========================================================================= *
 * BACKLIGHT_BRIGHTNESS
 * ========================================================================= */

/** Maximum display brightness, hw specific */
static gint mdy_brightness_level_maximum = DEFAULT_MAXIMUM_DISPLAY_BRIGHTNESS;

/** File used to get maximum display brightness */
static gchar *mdy_brightness_level_maximum_path = NULL;

/** Cached brightness, last value written; [0, mdy_brightness_level_maximum] */
static gint mdy_brightness_level_cached = -1;

/** Brightness, when display is not off; [0, mdy_brightness_level_maximum] */
static gint mdy_brightness_level_display_on = 1;

/** Dim brightness; [0, mdy_brightness_level_maximum] */
static gint mdy_brightness_level_display_dim = 1;

/** LPM brightness; [0, mdy_brightness_level_maximum] */
static gint mdy_brightness_level_display_lpm = 1;

/** Brightness to use on display wakeup; [0, mdy_brightness_level_maximum] */
static int mdy_brightness_level_display_resume = 1;

/** File used to set display brightness */
static output_state_t mdy_brightness_level_output =
{
    .path = NULL,
    .context = "brightness",
    .truncate_file = TRUE,
    .close_on_exit = FALSE,
};

/** Hook for setting brightness
 *
 * Note: For use from mdy_brightness_set_level() only!
 *
 * @param number brightness value; after bounds checking
 */
static void (*mdy_brightness_set_level_hook)(int number) = mdy_brightness_set_level_default;

/** Is hardware driven display fading supported */
static gboolean mdy_brightness_hw_fading_is_supported = FALSE;

/** File used to set hw display fading */
static output_state_t mdy_brightness_hw_fading_output =
{
    .path = NULL,
    .context = "hw_fading",
    .truncate_file = TRUE,
    .close_on_exit = TRUE,
};

/** Brightness fade timeout callback ID */
static guint mdy_brightness_fade_timer_id = 0;

/** Type of ongoing brightness fade */
static fader_type_t mdy_brightness_fade_type = FADER_IDLE;

/** Monotonic time stamp for brightness fade start time */
static int64_t mdy_brightness_fade_start_time = 0;

/** Monotonic time stamp for brightness fade end time */
static int64_t mdy_brightness_fade_end_time = 0;

/** Brightness level at the start of brightness fade */
static int     mdy_brightness_fade_start_level = 0;

/** Brightness level at the end of brightness fade */
static int     mdy_brightness_fade_end_level = 0;

/** Default brightness fade length during display state transitions [ms] */
static gint mdy_brightness_fade_duration_def_ms = DEFAULT_BRIGHTNESS_FADE_DEFAULT_MS;

/** GConf change notification id for mdy_brightness_fade_duration_def_ms */
static guint mdy_brightness_fade_duration_def_ms_gconf_cb_id = 0;

/** Brightness fade length during display dimming [ms] */
static gint mdy_brightness_fade_duration_dim_ms = DEFAULT_BRIGHTNESS_FADE_DIMMING_MS;

/** GConf change notification id for mdy_brightness_fade_duration_dim_ms */
static guint mdy_brightness_fade_duration_dim_ms_gconf_cb_id = 0;

/** Brightness fade length during ALS tuning [ms] */
static gint mdy_brightness_fade_duration_als_ms = DEFAULT_BRIGHTNESS_FADE_ALS_MS;

/** GConf change notification id for mdy_brightness_fade_duration_als_ms */
static guint mdy_brightness_fade_duration_als_ms_gconf_cb_id = 0;

/** Brightness fade length during display power down [ms]
 *
 * Note: Fade-to-black delays display power off and thus should be
 *       kept short enough not to cause irritation (due to increased
 *       response time to power key press).
 */
static gint mdy_brightness_fade_duration_blank_ms = DEFAULT_BRIGHTNESS_FADE_BLANK_MS;

/** GConf change notification id for mdy_brightness_fade_duration_blank_ms */
static guint mdy_brightness_fade_duration_blank_ms_gconf_cb_id = 0;

/** Brightness fade length during display power up [ms]
 *
 * The fade in starts after frame buffer has been powered up.
 * However the brightness is acted on only after UI side draws
 * something on screen. Shortly after that there usually is a lot
 * of cpu activity and longer fade durations will stutter.
 *
 * 90 = 55 (fb wakeup -> 1st draw) + 35 (1st draw -> cpu load)
 *
 * Basically we end up seeing the brighter end of the fade in.
 */
static gint mdy_brightness_fade_duration_unblank_ms = DEFAULT_BRIGHTNESS_FADE_UNBLANK_MS;

/** GConf change notification id for mdy_brightness_fade_duration_unblank_ms */
static guint mdy_brightness_fade_duration_unblank_ms_gconf_cb_id = 0;

/** Use of orientation sensor enabled */
static gboolean mdy_orientation_sensor_enabled = DEFAULT_ORIENTATION_SENSOR_ENABLED;

/** GConf change notification id for mdy_orientation_sensor_enabled */
static guint mdy_orientation_sensor_enabled_gconf_cb_id = 0;

/** Flipover gesture detection enabled */
static gboolean mdy_flipover_gesture_enabled = DEFAULT_FLIPOVER_GESTURE_ENABLED;

/** GConf change notification id for mdy_flipover_gesture_enabled */
static guint mdy_flipover_gesture_enabled_gconf_cb_id = 0;

/** Orientation change generates activity enabled */
static gboolean mdy_orientation_change_is_activity = DEFAULT_ORIENTATION_CHANGE_IS_ACTIVITY;

/** GConf change notification id for mdy_orientation_change_is_activity */
static guint mdy_orientation_change_is_activity_gconf_cb_id = 0;

/** Set display brightness via sysfs write */
static void mdy_brightness_set_level_default(int number)
{
    mce_write_number_string_to_file(&mdy_brightness_level_output, number);
}

#ifdef ENABLE_HYBRIS
/** Set display brightness via libhybris */
static void mdy_brightness_set_level_hybris(int number)
{
    mce_hybris_backlight_set_brightness(number);
}
#endif

/** Helper for updating backlight brightness with bounds checking
 *
 * @param number brightness in 0 to mdy_brightness_level_maximum range
 */
static void mdy_brightness_set_level(int number)
{
    int minval = 0;
    int maxval = mdy_brightness_level_maximum;

    /* If we manage to get out of hw bounds values from depths
     * of pipelines and state machines we could end up with
     * black screen without easy way out -> clip to valid range */
    if( number < minval ) {
        mce_log(LL_ERR, "value=%d vs min=%d", number, minval);
        number = minval;
    }
    else if( number > maxval ) {
        mce_log(LL_ERR, "value=%d vs max=%d", number, maxval);
        number = maxval;
    }
    else
        mce_log(LL_DEBUG, "value=%d", number);

    if( mdy_brightness_level_cached != number ) {
        mdy_brightness_level_cached = number;
        mdy_brightness_set_level_hook(number);
    }

    // TODO: we might want to power off fb at zero brightness
    //       and power it up at non-zero brightness???
}

/** Helper for boosting mce scheduling priority during brightness fading
 *
 * Any scheduling hiccups during backlight brightness tuning are really
 * visible. To make it less likely to occur, this function is used to
 * move mce process to SCHED_FIFO while fade timer is active.
 *
 * Note: Currently this is the only place where mce needs to tweak
 * the scheduling parameters. If that ever changes, a better interface
 * needs to be built.
 */
static void mdy_brightness_set_priority_boost(bool enable)
{
    /* Initialize cached state to default scheduler */
    static int normal_scheduler = SCHED_OTHER;
    static int normal_priority  = 0;

    /* Initially boosted priority is not in use */
    static bool is_enabled = false;

    if( is_enabled == enable )
        goto EXIT;

    int scheduler = SCHED_OTHER;

    struct sched_param param;
    memset(&param, 0, sizeof param);

    if( enable ) {
        /* Cache current scheduling parameters */
        if( (scheduler = sched_getscheduler(0)) == -1 )
            mce_log(LL_WARN, "sched_getscheduler: %m");
        else if( sched_getparam(0, &param) == -1 )
            mce_log(LL_WARN, "sched_getparam: %m");
        else {
            normal_scheduler = scheduler;
            normal_priority = param.sched_priority;
        }

        /* Switch to medium priority fifo scheduling */
        scheduler = SCHED_FIFO;
        param.sched_priority = (sched_get_priority_min(scheduler) +
                                sched_get_priority_max(scheduler)) / 2;
    }
    else {
        /* Switch back to cached scheduling parameters */
        scheduler = normal_scheduler;
        param.sched_priority = normal_priority;
    }

    mce_log(LL_DEBUG, "sched=%d, prio=%d", scheduler, param.sched_priority);

    if( sched_setscheduler(0, scheduler, &param) == -1 ) {
        mce_log(LL_WARN, "can't %s high priority mode: %m",
                enable ? "enter" : "leave");
    }

    /* The logical change is made even if we fail to actually change
     * the scheduling parameters */
    is_enabled = enable;

EXIT:
    return;
}

/** Helper for cancelling brightness fade and forcing a brightness level
 *
 * @param number brightness in 0 to mdy_brightness_level_maximum range
 */
static void mdy_brightness_force_level(int number)
{
    mce_log(LL_DEBUG, "brightness from %d to %d",
            mdy_brightness_level_cached, number);

    mdy_brightness_stop_fade_timer();

    mdy_brightness_fade_start_level =
        mdy_brightness_fade_end_level = number;

    mdy_brightness_fade_start_time =
        mdy_brightness_fade_end_time = mce_lib_get_boot_tick();

    mdy_brightness_set_level(number);
}

/** Helper for re-evaluating need for als-tuning
 *
 * Ongoing higher priority fade animations can block als tuning
 * from taking place. This function is used to re-evaluates the
 * need for als tuning once the blocking transitions finish.
 *
 * @param fade_type type of fade animation that was just finished
 */
static void mdy_brightness_fade_continue_with_als(fader_type_t fader_type)
{
    /* Only applicable after ending fade animations that
     * temporarily block als transitions - keep this in
     * sync with logic in mdy_brightness_is_fade_allowed().
     */
    switch( fader_type ) {
    case FADER_DEFAULT:
    case FADER_DIMMING:
        break;
    default:
        goto EXIT;
    }

    /* The display state must be stable too */
    if( display_state_next != display_state )
        goto EXIT;

    /* Target level depends on the display state */
    int level = mdy_brightness_fade_end_level;

    switch( display_state ) {
    case MCE_DISPLAY_LPM_ON:
        level = mdy_brightness_level_display_lpm;
        break;

    case MCE_DISPLAY_DIM:
        level = mdy_brightness_level_display_dim;
        break;

    case MCE_DISPLAY_ON:
        level = mdy_brightness_level_display_on;
        break;

    default:
        goto EXIT;
    }

    /* Apply if change is needed */
    if( level != mdy_brightness_fade_end_level ) {
        mce_log(LL_DEBUG, "continue with als tuning");
        mdy_brightness_set_fade_target_als(level);
    }

EXIT:
    return;
}

/**
 * Timeout callback for the brightness fade
 *
 * @param data Unused
 * @return Returns TRUE to repeat, until the cached brightness has reached
 *         the destination value; when this happens, FALSE is returned
 */
static gboolean mdy_brightness_fade_timer_cb(gpointer data)
{
    (void)data;

    gboolean keep_going = FALSE;

    if( !mdy_brightness_fade_timer_id )
        goto EXIT;

    /* Assume end of transition brightness is to be used */
    int lev = mdy_brightness_fade_end_level;

    /* Get current time */
    int64_t now = mce_lib_get_boot_tick();

    if( mdy_brightness_fade_start_time <= now &&
        now < mdy_brightness_fade_end_time ) {
        /* Linear interpolation */
        int weight_end = (int)(now - mdy_brightness_fade_start_time);
        int weight_beg = (int)(mdy_brightness_fade_end_time - now);
        int weight_tot = weight_end + weight_beg;

        lev = (weight_end * mdy_brightness_fade_end_level +
               weight_beg * mdy_brightness_fade_start_level +
               weight_tot/2) / weight_tot;

        keep_going = TRUE;
    }

    mdy_brightness_set_level(lev);

    /* Cleanup if finished */
    if( !keep_going ) {
        /* Cache fade type that just finished */
        fader_type_t fader_type = mdy_brightness_fade_type;

        /* Reset fader state */
        mdy_brightness_fade_timer_id = 0;
        mdy_brightness_cleanup_fade_timer();
        mce_log(LL_DEBUG, "fader finished");

        /* Check if we need to continue with als tuning */
        mdy_brightness_fade_continue_with_als(fader_type);
    }

EXIT:
    return keep_going;
}

/** Helper function for cleaning up brightness fade timer
 *
 * Common fader timer cancellation logic
 *
 * NOTE: For use from mdy_brightness_fade_timer_cb() and
 * mdy_brightness_stop_fade_timer() functions only.
 */
static void mdy_brightness_cleanup_fade_timer(void)
{
    /* Remove timer source */
    if( mdy_brightness_fade_timer_id )
        g_source_remove(mdy_brightness_fade_timer_id),
        mdy_brightness_fade_timer_id = 0;

    /* Clear ongoing fade type */
    mdy_brightness_fade_type = FADER_IDLE;

    /* Unblock display off transition */
    mdy_stm_schedule_rethink();

    /* Cancel scheduling priority boost */
    mdy_brightness_set_priority_boost(false);
}

/**
 * Cancel the brightness fade timeout
 */
static void mdy_brightness_stop_fade_timer(void)
{
    /* Cleanup if timer is active */
    if( mdy_brightness_fade_timer_id )
        mdy_brightness_cleanup_fade_timer();
}

/**
 * Setup the brightness fade timeout
 *
 * @param step_time The time between each brightness step
 */
static void mdy_brightness_start_fade_timer(fader_type_t type,
                                            gint step_time)
{
    if( !mdy_brightness_fade_timer_id ) {
        mce_log(LL_DEBUG, "fader started");
        mdy_brightness_set_priority_boost(true);
    }
    else {
        mce_log(LL_DEBUG, "fader restarted");
        g_source_remove(mdy_brightness_fade_timer_id),
            mdy_brightness_fade_timer_id = 0;
    }

    /* Setup new timeout */
    mdy_brightness_fade_timer_id =
        g_timeout_add(step_time, mdy_brightness_fade_timer_cb, NULL);

    /* Set ongoing fade type */
    mdy_brightness_fade_type = type;
}

static bool mdy_brightness_fade_is_active(void)
{
    return mdy_brightness_fade_timer_id != 0;
}

/** Check if starting brightness fade of given type is allowed
 *
 * @param type fade type to start
 *
 * @return true if the fading can start, false otherwise
 */
static bool
mdy_brightness_is_fade_allowed(fader_type_t type)
{
    bool allowed = true;

    switch( mdy_brightness_fade_type ) {
    default:
    case FADER_IDLE:
    case FADER_ALS:
        break;

    case FADER_DEFAULT:
    case FADER_DIMMING:
        /* Deny als tuning during display state transitions.
         *
         * The idea here is that fades associated with display
         * state transitions are kept as uniform as possible
         * even if lightning conditions change during the
         * fade.
         *
         * After these blocking fade animations are finished,
         * mdy_brightness_fade_continue_with_als() is used
         * to re-evaluate the need for possible als tuning
         * again.
         */
        if( type == FADER_ALS )
            allowed = false;
        break;

    case FADER_BLANK:
        /* ongoing fade to black can't be cancelled */
        allowed = false;
        break;

    case FADER_UNBLANK:
        /* only unblank target level can be changed */
        if( type != FADER_UNBLANK )
            allowed = false;
        break;
    }

    return allowed;
}

/**
 * Update brightness fade
 *
 * Will fade from current value to new value
 *
 * @param new_brightness The new brightness to fade to
 */
static void mdy_brightness_set_fade_target_ex(fader_type_t type,
                                              gint new_brightness,
                                              gint transition_time)
{
    /* While something like 20-40 ms would suffice for most cases
     * using smaller 4 ms value allows us to make few steps during
     * the short time window we have available during unblanking. */
    const int delay_min = 4;

    /* Negative transition time: constant velocity change [%/s] */
    if( transition_time < 0 ) {
        int d = abs(new_brightness - mdy_brightness_level_cached);
        // velocity: percent/sec -> steps/sec
        int v = mce_xlat_int(1, 100,
                             1, mdy_brightness_level_maximum,
                             -transition_time);
        if( v <= 0 )
            transition_time = MCE_FADER_DURATION_HW_MAX;
        else
            transition_time = (1000 * d + v/2) / v;
    }

    /* Keep transition time in sane range */
    transition_time = mce_clip_int(MCE_FADER_DURATION_HW_MIN,
                                   MCE_FADER_DURATION_HW_MAX,
                                   transition_time);

    mce_log(LL_DEBUG, "type %s fade from %d to %d in %d ms",
            fader_type_name(type),
            mdy_brightness_level_cached,
            new_brightness, transition_time);

    if( !mdy_brightness_is_fade_allowed(type) ) {
        mce_log(LL_DEBUG, "ignoring fade=%s; ongoing fade=%s",
                fader_type_name(type),
                fader_type_name(mdy_brightness_fade_type));
        goto EXIT;
    }

    /* If we're already at the target level, stop any
     * ongoing fading activity */
    if( mdy_brightness_level_cached == new_brightness ) {
        mdy_brightness_stop_fade_timer();
        goto EXIT;
    }

    /* Small enough changes are made immediately instead of
     * using fading timer */
    if( abs(mdy_brightness_level_cached - new_brightness) <= 1 ) {
        mce_log(LL_DEBUG, "small change; not using fader");
        mdy_brightness_force_level(new_brightness);
        goto EXIT;
    }

    /* Calculate fading time window */
    int64_t beg = mce_lib_get_boot_tick();
    int64_t end = beg + transition_time;

    /* If an ongoing fading has the same target level and it
     * will finish before the new one would, use it */
    if( mdy_brightness_fade_is_active() &&
        mdy_brightness_fade_end_level == new_brightness &&
        mdy_brightness_fade_end_time <= end )
        goto EXIT;

    /* Move fading start point to current time */
    mdy_brightness_fade_start_time = beg;

    if( mdy_brightness_fade_end_time <= beg ) {
        /* Previous fading has ended -> set fading end point */
        mdy_brightness_fade_end_time = end;
    }
    else if( mdy_brightness_fade_end_time > end ) {
        /* Current fading would end later -> adjust end point */
        mdy_brightness_fade_end_time = end;
    }

    /* Set up fade start and end brightness levels */
    mdy_brightness_fade_start_level = mdy_brightness_level_cached;
    mdy_brightness_fade_end_level   = new_brightness;

    /* If the - possibly adjusted - transition time is so short that
     * only couple of adjustments would be made, do an immediate
     * level set instead of fading */
    transition_time = (int)(mdy_brightness_fade_end_time -
                            mdy_brightness_fade_start_time);

    if( transition_time < delay_min * 3 ) {
        mce_log(LL_DEBUG, "short transition; not using fader");
        mdy_brightness_force_level(new_brightness);
        goto EXIT;
    }

    /* Calculate desired brightness change velocity. */
    int steps = abs(mdy_brightness_fade_end_level -
                    mdy_brightness_fade_start_level);
    int delay = transition_time / steps; // NB steps != 0

    /* Reject insane timer wakeup frequencies. The fade timer
     * utilizes timestamp based interpolation, so the delay
     * does not need to be exactly as planned above.
     */
    if( delay < delay_min )
        delay = delay_min;

    mdy_brightness_start_fade_timer(type, delay);

EXIT:
    return;
}

/** Start brightness fading associated with display state change
 */
static void mdy_brightness_set_fade_target_default(gint new_brightness)
{
    mdy_brightness_set_fade_target_ex(FADER_DEFAULT,
                                      new_brightness,
                                      mdy_brightness_fade_duration_def_ms);
}

/** Start brightness fading after powering up the display
 */
static void mdy_brightness_set_fade_target_unblank(gint new_brightness)
{
    mdy_brightness_set_fade_target_ex(FADER_UNBLANK,
                                      new_brightness,
                                      mdy_brightness_fade_duration_unblank_ms);
}

/** Start fade to black before powering off the display
 */
static void mdy_brightness_set_fade_target_blank(void)
{
    if( call_state == CALL_STATE_ACTIVE ) {
        /* Unlike the other brightness fadings, the fade-to-black blocks
         * the display state machine and thus delays the whole display
         * power off sequence.
         *
         * Thus it must not be used during active call to avoid stray
         * touch input from ear/chin when proximity blanking is in use.
         */
        mdy_brightness_force_level(0);
        goto EXIT;
    }

    mdy_brightness_set_fade_target_ex(FADER_BLANK,
                                      0,
                                      mdy_brightness_fade_duration_blank_ms);
EXIT:
    return;
}

/** Start brightness fading associated with display dimmed state
 */
static void mdy_brightness_set_fade_target_dimming(gint new_brightness)
{
    mdy_brightness_set_fade_target_ex(FADER_DIMMING,
                                      new_brightness,
                                      mdy_brightness_fade_duration_dim_ms);
}

/** Flag for: Automatic ALS based brightness tuning is allowed */
bool mdy_brightness_als_fade_allowed = false;

/** Start brightness fading due to ALS / brightness setting change
 */
static void mdy_brightness_set_fade_target_als(gint new_brightness)
{
    /* Update wake up brightness level in case we got als data
     * before unblank fading has been started */
    mce_log(LL_DEBUG, "resume level: %d -> %d",
            mdy_brightness_level_display_resume,
            new_brightness);
    mdy_brightness_level_display_resume = new_brightness;

    /* If currently unblanking, just adjust the target level */
    if( mdy_brightness_fade_type == FADER_UNBLANK ) {
        mdy_brightness_set_fade_target_unblank(new_brightness);
        mce_log(LL_DEBUG, "skip als fade; adjust unblank target");
        goto EXIT;
    }

    /* Check if main display state machine is blocking ALS tuning */
    if( !mdy_brightness_als_fade_allowed ) {
        mce_log(LL_DEBUG, "skip als fade; not allowed");
        goto EXIT;
    }

    /* Assume configured fade duration is used */
    int dur = mdy_brightness_fade_duration_als_ms;

    /* To make effects of changing the brightness settings
     * more clear, override constant time / long als fade durations
     * that happen immediately after relevant settings changes. */
    if( dur < 0 || dur > MCE_FADER_DURATION_SETTINGS_CHANGED ) {
        int64_t now = mce_lib_get_boot_tick();
        int64_t end = (mdy_brightness_setting_change_time +
                       MCE_FADER_DURATION_SETTINGS_CHANGED);
        if( now <= end )
            dur = MCE_FADER_DURATION_SETTINGS_CHANGED;
    }

    /* Start als brightness fade */
    mdy_brightness_set_fade_target_ex(FADER_ALS, new_brightness, dur);

EXIT:
    return;
}

/** Get static display brightness setting in hw units
 */
static int mdy_brightness_get_dim_static(void)
{
    // N % of hw maximum
    return mce_xlat_int(1, 100,
                        1, mdy_brightness_level_maximum,
                        mdy_brightness_dim_static);
}

/** Get dynamic display brightness setting in hw units
 */
static int mdy_brightness_get_dim_dynamic(void)
{
    // N % of display on brightness
    return mce_xlat_int(1, 100,
                        1, mdy_brightness_level_display_on,
                        mdy_brightness_dim_dynamic);
}

/** Get start of compositor dimming threshold in hw units
 */
static int mdy_brightness_get_dim_threshold_lo(void)
{
    // N % of hw maximum
    return mce_xlat_int(1, 100,
                        1, mdy_brightness_level_maximum,
                        mdy_brightness_dim_compositor_lo);
}

/** Get maximal compositor dimming threshold in hw units
 */
static int mdy_brightness_get_dim_threshold_hi(void)
{
    // N % of hw maximum
    return mce_xlat_int(1, 100,
                        1, mdy_brightness_level_maximum,
                        mdy_brightness_dim_compositor_hi);
}

/** Map value in one range to another using linear interpolation
 *
 * Assumes src_lo < src_hi, if this is not the case
 * low boundary of the target range is returned.
 *
 * If provided value is outside the source range, output
 * is capped to the given target range.
 *
 * @param src_lo  low boundary of the source range
 * @param src_hi  high boundary of the source range
 * @param dst_lo  low boundary of the target range
 * @param dst_hi  high boundary of the target range
 * @param val     value in [src_lo, src_hi] range
 *
 * @return val mapped to [dst_lo, dst_hi] range
 */
static inline int
xlat(int src_lo, int src_hi, int dst_lo, int dst_hi, int val)
{
    if( src_lo > src_hi )
        return dst_lo;

    if( val <= src_lo )
        return dst_lo;

    if( val >= src_hi )
        return dst_hi;

    int range = src_hi - src_lo;

    val = (val - src_lo) * dst_hi + (src_hi - val) * dst_lo;
    val += range/2;
    val /= range;

    return val;
}

static void mdy_brightness_set_dim_level(void)
{
    /* Update backlight level to use in dimmed state */
    int brightness = mdy_brightness_get_dim_static();
    int dynamic    = mdy_brightness_get_dim_dynamic();

    if( brightness > dynamic )
        brightness = dynamic;

    if( mdy_brightness_level_display_dim != brightness ) {
        mce_log(LL_DEBUG, "brightness.dim: %d -> %d",
                mdy_brightness_level_display_dim, brightness);
        mdy_brightness_level_display_dim = brightness;
    }

    /* Check if compositor side fading needs to be used */
    int difference = (mdy_brightness_level_display_on -
                      mdy_brightness_level_display_dim);

    /* Difference level where minimal compositor fading starts */
    int threshold_lo  = mdy_brightness_get_dim_threshold_lo();

    /* Difference level where maximal compositor fading is reached */
    int threshold_hi  = mdy_brightness_get_dim_threshold_hi();

    /* If fading start is set beyond the point where maximal fading
     * is reached, use on/off control at high threshold point */
    if( threshold_lo <= threshold_hi )
        threshold_lo = threshold_hi + 1;

    int compositor_fade_level = xlat(threshold_hi, threshold_lo,
                                     MCE_FADER_MAXIMUM_OPACITY_PERCENT, 0,
                                     difference);

    /* Note: The pattern can be activated anytime, it will get
     *       effective only when display is in dimmed state
     *
     * FIXME: When ui side dimming is working, the led pattern
     *        hack should be removed altogether.
     */
    execute_datapipe_output_triggers(compositor_fade_level > 0 ?
                                     &led_pattern_activate_pipe :
                                     &led_pattern_deactivate_pipe,
                                     "PatternDisplayDimmed",
                                     USE_INDATA);

    /* Update ui side fader opacity value */
    mdy_ui_dimming_set_level(compositor_fade_level);
}

static void mdy_brightness_set_lpm_level(gint level)
{
    /* Map from: 1-100% to: 1-hw_max */
    int brightness = mce_xlat_int(1, 100,
                                  1, mdy_brightness_level_maximum,
                                  level);

    mce_log(LL_DEBUG, "mdy_brightness_level_display_lpm: %d -> %d",
            mdy_brightness_level_display_lpm, brightness);

    mdy_brightness_level_display_lpm = brightness;

    /* Take updated values in use - based on non-transitional
     * display state we are in or transitioning to */
    switch( display_state_next ) {
    case MCE_DISPLAY_LPM_ON:
        mdy_brightness_set_fade_target_als(mdy_brightness_level_display_lpm);
        break;

    default:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_POWER_DOWN:
    case MCE_DISPLAY_POWER_UP:
        break;
    }

    return;
}

static void mdy_brightness_set_on_level(gint hbm_and_level)
{
    gint new_brightness = (hbm_and_level >> 0) & 0xff;
    gint new_hbm_level  = (hbm_and_level >> 8) & 0xff;

    mce_log(LL_INFO, "hbm_level=%d, brightness=%d",
            new_hbm_level, new_brightness);

    /* If the pipe is choked, ignore the value */
    if (new_brightness == 0)
        goto EXIT;

    /* This is always necessary,
     * since 100% + HBM is not the same as 100% without HBM
     */
    mdy_hbm_level_wanted = new_hbm_level;
    mdy_hbm_rethink();

    /* Adjust the value, since it's a percentage value, and filter out
     * the high brightness setting
     */
    new_brightness = (mdy_brightness_level_maximum * new_brightness) / 100;

    /* The value we have here is for non-dimmed screen only */
    if( mdy_brightness_level_display_on != new_brightness ) {
        mce_log(LL_DEBUG, "brightness.on: %d -> %d",
                mdy_brightness_level_display_on, new_brightness);
        mdy_brightness_level_display_on = new_brightness;
    }

    /* Re-evaluate dim brightness too */
    mdy_brightness_set_dim_level();

    /* Note: The lpm brightness is handled separately */

    /* Take updated values in use - based on non-transitional
     * display state we are in or transitioning to */
    switch( display_state_next ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
        break;

    case MCE_DISPLAY_DIM:
        mdy_brightness_set_fade_target_als(mdy_brightness_level_display_dim);
        break;

    default:
    case MCE_DISPLAY_POWER_DOWN:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_UNDEF:
        break;

    case MCE_DISPLAY_ON:
        mdy_brightness_set_fade_target_als(mdy_brightness_level_display_on);
        break;
    }

EXIT:
    return;
}

/* ========================================================================= *
 * UI_SIDE_DIMMING
 * ========================================================================= */

/** Signal to send when ui side fader opacity changes */
#define MCE_FADER_OPACITY_SIG "fader_opacity_ind"

/** Compositor side fade to black opacity level percentage
 *
 * Used when backlight brightness alone can't produce visible dimmed state */
static int mdy_ui_dimming_level = 0;

/** Update mdy_ui_dimming_level state */
static void mdy_ui_dimming_set_level(int level)
{
    mdy_ui_dimming_level = level;
    mdy_ui_dimming_rethink();
}

/** Re-evaluate target opacity for ui side dimming
 *
 * Should be called when:
 * 1. on/dimmed brightness changes
 * 2. display state transition starts
 * 3. display state transition is finished
 */
static void mdy_ui_dimming_rethink(void)
{
    /* Initialize previous value to invalid state so that initial
     * evaluation on mce startup forces signal to be sent */
    static dbus_int32_t dimming_prev = -1;

    /* This gets a bit hairy because we do not want to restart the
     * ui side fade animation once it has started and heading to
     * the correct level -> on display power up we want to make
     * only one guess when/if the fading target changes and how
     * fast the change should happen.
     *
     * The triggers for calling this function are:
     * 1) display state transition starts
     * 2) als tuning changes mdy_ui_dimming_level
     *
     * When (1) happens, both display_state and display_state_next
     * hold stable states.
     *
     * If (2) happens during display power up/down, the
     * display_state variable can hold transitient
     * MCE_DISPLAY_POWER_UP/DOWN states.
     */

    /* Assume that ui side dimming should not occur */
    dbus_int32_t dimming_curr = 0;

    if( display_state == MCE_DISPLAY_POWER_DOWN ||
        display_state_next == MCE_DISPLAY_OFF   ||
        display_state_next == MCE_DISPLAY_LPM_OFF ) {
        /* At or entering powered off state -> keep current state */
        if( dimming_prev >= 0 )
            dimming_curr = dimming_prev;
    }
    else if( display_state_next == MCE_DISPLAY_DIM ) {
        /* At or entering dimmed state -> use if needed */
        dimming_curr = mdy_ui_dimming_level;
    }

    /* Skip the rest if the target level does not change */
    if( dimming_prev == dimming_curr )
        goto EXIT;

    dimming_prev = dimming_curr;

    /* Assume the change is due to ALS tuning */
    dbus_int32_t duration = mdy_brightness_fade_duration_als_ms;

    if( display_state == MCE_DISPLAY_POWER_UP ) {
        /* Leaving powered off state -> use unblank duration */
        duration = mdy_brightness_fade_duration_unblank_ms;
    }
    else if( display_state == MCE_DISPLAY_POWER_DOWN ) {
        /* Entering powered off state -> use blank duration */
        duration = mdy_brightness_fade_duration_blank_ms;
    }
    else if( display_state != display_state_next ) {
        /* Ongoing display state transition that does not need
         * or has not yet entered transient state */
        if( display_state == MCE_DISPLAY_OFF ||
            display_state == MCE_DISPLAY_LPM_OFF ) {
            /* Leaving powered off state -> use unblank duration */
            duration = mdy_brightness_fade_duration_unblank_ms;
        }
        else if( display_state_next == MCE_DISPLAY_OFF ||
                 display_state_next == MCE_DISPLAY_LPM_OFF ) {
            /* Entering powered off state -> use blank duration */
            duration = mdy_brightness_fade_duration_blank_ms;
        }
        else if( display_state_next == MCE_DISPLAY_DIM ) {
            /* Entering dimmed state -> use dimming duration */
            duration = mdy_brightness_fade_duration_dim_ms;
        }
        else {
            /* Use default state transition duration */
            duration = mdy_brightness_fade_duration_def_ms;
        }
    }

    /* Keep transition time in sane range. Also takes care
     * that negative values used to signify constant velocity
     * change do not get passed to compositor side dimming. */
    duration = mce_clip_int(MCE_FADER_DURATION_UI_MIN,
                            MCE_FADER_DURATION_UI_MAX,
                            duration);

    mce_log(LL_DEVEL, "sending dbus signal: %s %d %d",
            MCE_FADER_OPACITY_SIG, dimming_curr, duration);

    dbus_send(0, MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
              MCE_FADER_OPACITY_SIG, 0,
              DBUS_TYPE_INT32, &dimming_curr,
              DBUS_TYPE_INT32, &duration,
              DBUS_TYPE_INVALID);

EXIT:
    return;
}

/* ========================================================================= *
 * CONTENT_ADAPTIVE_BACKLIGHT_CONTROL
 * ========================================================================= */

/**
 * CABC mappings; D-Bus API modes vs SysFS mode
 */
cabc_mode_mapping_t mdy_cabc_mode_mapping[] =
{
    {
        .dbus = MCE_CABC_MODE_OFF,
        .sysfs = CABC_MODE_OFF,
        .available = FALSE
    },
    {
        .dbus = MCE_CABC_MODE_UI,
        .sysfs = CABC_MODE_UI,
        .available = FALSE
    },
    {
        .dbus = MCE_CABC_MODE_STILL_IMAGE,
        .sysfs = CABC_MODE_STILL_IMAGE,
        .available = FALSE
    },
    {
        .dbus = MCE_CABC_MODE_MOVING_IMAGE,
        .sysfs = CABC_MODE_MOVING_IMAGE,
        .available = FALSE
    },
    {
        .dbus = NULL,
        .sysfs = NULL,
        .available = FALSE
    }
};

/**
 * Set CABC mode
 *
 * @param mode The CABC mode to set
 */
static void mdy_cabc_mode_set(const gchar *const mode)
{
    static gboolean available_modes_scanned = FALSE;
    const gchar *tmp = NULL;
    gint i;

    if ((mdy_cabc_is_supported == FALSE) || (mdy_cabc_available_modes_file == NULL))
        goto EXIT;

    /* Update the list of available modes against the list we support */
    if (available_modes_scanned == FALSE) {
        gchar *available_modes = NULL;

        available_modes_scanned = TRUE;

        if (mce_read_string_from_file(mdy_cabc_available_modes_file,
                                      &available_modes) == FALSE)
            goto EXIT;

        for (i = 0; (tmp = mdy_cabc_mode_mapping[i].sysfs) != NULL; i++) {
            if (strstr_delim(available_modes, tmp, " ") != NULL)
                mdy_cabc_mode_mapping[i].available = TRUE;
        }

        g_free(available_modes);
    }

    /* If the requested mode is supported, use it */
    for (i = 0; (tmp = mdy_cabc_mode_mapping[i].sysfs) != NULL; i++) {
        if (mdy_cabc_mode_mapping[i].available == FALSE)
            continue;

        if (!strcmp(tmp, mode)) {
            mce_write_string_to_file(mdy_cabc_mode_file, tmp);

            /* Don't overwrite the regular CABC mode with the
             * power save mode CABC mode
             */
            if (mdy_psm_cabc_mode == NULL)
                mdy_cabc_mode = tmp;

            break;
        }
    }

EXIT:
    return;
}

/* ========================================================================= *
 * BOOTUP_LED_PATTERN
 * ========================================================================= */

/** Re-evaluate whether we want POWER_ON led pattern or not
 */
static void mdy_poweron_led_rethink(void)
{
    bool want_led = (!mdy_init_done && mdy_bootstate == BOOTSTATE_USER);

    mce_log(LL_DEBUG, "%s MCE_LED_PATTERN_POWER_ON",
            want_led ? "activate" : "deactivate");

    execute_datapipe_output_triggers(want_led ?
                                     &led_pattern_activate_pipe :
                                     &led_pattern_deactivate_pipe,
                                     MCE_LED_PATTERN_POWER_ON,
                                     USE_INDATA);
}

/** Timer id for delayed POWER_ON led state evaluation */
static guint mdy_poweron_led_rethink_id = 0;

/** Timer callback for delayed POWER_ON led state evaluation
 */
static gboolean mdy_poweron_led_rethink_cb(gpointer aptr)
{
    (void)aptr;

    if( mdy_poweron_led_rethink_id ) {
        mdy_poweron_led_rethink_id = 0;
        mdy_poweron_led_rethink();
    }
    return FALSE;
}

/** Cancel delayed POWER_ON led state evaluation
 */
static void mdy_poweron_led_rethink_cancel(void)
{
    if( mdy_poweron_led_rethink_id )
        g_source_remove(mdy_poweron_led_rethink_id),
        mdy_poweron_led_rethink_id = 0;
}

/** Schedule delayed POWER_ON led state evaluation
 */
static void mdy_poweron_led_rethink_schedule(void)
{
    if( !mdy_poweron_led_rethink_id )
        mdy_poweron_led_rethink_id = g_idle_add(mdy_poweron_led_rethink_cb, 0);
}

/* ========================================================================= *
 * AUTOMATIC_BLANKING
 * ========================================================================= */

/** Re-calculate inactivity timeout
 *
 * This function should be called whenever the variables used
 * in the calculation are changed.
 */
static void mdy_blanking_update_inactivity_timeout(void)
{
    /* Inactivity should be signaled around the time when the display
     * should have dimmed and blanked - even if the actual blanking is
     * blocked by blanking pause and/or blanking inhibit mode. */

    int inactivity_timeout = mdy_disp_dim_timeout + mdy_blank_timeout;

    mce_log(LL_DEBUG, "inactivity_timeout = %d", inactivity_timeout);

    execute_datapipe(&inactivity_timeout_pipe,
                     GINT_TO_POINTER(inactivity_timeout),
                     USE_INDATA, CACHE_INDATA);
}

/**
 * Find the dim timeout index from a dim timeout
 *
 * If list of possible dim timeouts is not available, zero is returned.
 *
 * If the given dim_timeout is larger than the largest entry int the
 * possible timeouts list, the index to the largest entry is returned.
 *
 * Otherwise the index to the first entry that is greater or equal
 * to specified dim_timeout is returned.
 *
 * @param dim_timeout The dim timeout to find the index for
 *
 * @return The closest dim timeout index
 */
static guint mdy_blanking_find_dim_timeout_index(gint dim_timeout)
{
    guint   res  = 0;
    GSList *iter = mdy_possible_dim_timeouts;

    if( !iter )
        goto EXIT;

    for( ;; ) {
        gint allowed_timeout = GPOINTER_TO_INT(iter->data);

        if( dim_timeout <= allowed_timeout )
            break;

        if( !(iter = iter->next) )
            break;

        ++res;
    }

EXIT:
    return res;
}

/**
 * Check whether changing from LPM to blank can be done
 *
 * @return TRUE if blanking is possible, FALSE otherwise
 */
static gboolean mdy_blanking_can_blank_from_low_power_mode(void)
{
    // allow if LPM is not supposed to be used anyway
    if( !mdy_use_low_power_mode )
        return TRUE;

    // always allow in MALF
    if( submode & MCE_MALF_SUBMODE )
        return TRUE;

    // always allow during active call
    if( call_state == CALL_STATE_RINGING || call_state == CALL_STATE_ACTIVE )
        return TRUE;

#if 0
    // for reference, old logic: allow after proximity tklock set
    if( submode & MCE_PROXIMITY_TKLOCK_SUBMODE )
        return TRUE;
#else
    // TODO: we need proximity locking back in, for now just allow it
    //       when tklocked
    if( submode & MCE_TKLOCK_SUBMODE )
        return TRUE;
#endif

    return FALSE;
}

// TIMER: ON -> DIM

/** Display dimming timeout callback ID */
static guint mdy_blanking_dim_cb_id = 0;

/**
 * Timeout callback for display dimming
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean mdy_blanking_dim_cb(gpointer data)
{
    (void)data;

    mce_log(LL_DEBUG, "DIM timer triggered");

    display_state_t display = MCE_DISPLAY_DIM;

    mdy_blanking_dim_cb_id = 0;

    mdy_blanking_inhibit_schedule_broadcast();

    /* If device is in MALF state skip dimming since systemui
     * isn't working yet */
    if( submode & MCE_MALF_SUBMODE )
        display = MCE_DISPLAY_OFF;

    execute_datapipe(&display_state_req_pipe,
                     GINT_TO_POINTER(display),
                     USE_INDATA, CACHE_INDATA);

    return FALSE;
}

/**
 * Cancel display dimming timeout
 */
static void mdy_blanking_cancel_dim(void)
{
        /* Remove the timeout source for display dimming */
    if (mdy_blanking_dim_cb_id != 0) {
        mce_log(LL_DEBUG, "DIM timer canceled");
        g_source_remove(mdy_blanking_dim_cb_id), mdy_blanking_dim_cb_id = 0;

        mdy_blanking_inhibit_schedule_broadcast();
    }
}

/**
 * Setup dim timeout
 */
static void mdy_blanking_schedule_dim(void)
{
    gint dim_timeout = mdy_blanking_get_afterboot_delay();

    if( dim_timeout < mdy_disp_dim_timeout )
        dim_timeout = mdy_disp_dim_timeout;

    mdy_blanking_cancel_dim();

    if( mdy_adaptive_dimming_enabled ) {
        gpointer *tmp = g_slist_nth_data(mdy_possible_dim_timeouts,
                                         mdy_dim_timeout_index +
                                         mdy_adaptive_dimming_index);

        gint adaptive_timeout = GPOINTER_TO_INT(tmp);

        if( dim_timeout < adaptive_timeout )
            dim_timeout = adaptive_timeout;
    }

    /* In act dead mode blanking timeouts are capped */
    if( system_state == MCE_STATE_ACTDEAD ) {
        if( dim_timeout > ACTDEAD_MAX_DIM_TIMEOUT )
            dim_timeout = ACTDEAD_MAX_DIM_TIMEOUT;
    }

    mce_log(LL_DEBUG, "DIM timer scheduled @ %d secs", dim_timeout);

    /* Setup new timeout */
    mdy_blanking_dim_cb_id = g_timeout_add_seconds(dim_timeout,
                                                   mdy_blanking_dim_cb, NULL);

    mdy_blanking_inhibit_schedule_broadcast();

    return;
}

/** Idle callback id for delayed blanking inhibit signaling */
static guint mdy_blanking_inhibit_broadcast_id = 0;

/** Idle callback for delayed blanking inhibit signaling
 *
 * @param aptr (unused)
 *
 * @return FALSE to stop idle callback from repeating
 */
static gboolean mdy_blanking_inhibit_broadcast_cb(gpointer aptr)
{
    (void)aptr;

    if( !mdy_blanking_inhibit_broadcast_id )
        goto EXIT;

    mdy_blanking_inhibit_broadcast_id = 0;

    mdy_dbus_send_blanking_inhibit_status(0);

EXIT:
    return FALSE;
}

/** Schedule blanking inhibit signaling
 *
 * Idle callback is used to delay the sending of the D-Bus
 * signal so that timer reprogramming etc temporary changes
 * do not cause swarm of signals to be broadcast.
 */
static void mdy_blanking_inhibit_schedule_broadcast(void)
{
    if( mdy_blanking_inhibit_broadcast_id )
        goto EXIT;

    mdy_blanking_inhibit_broadcast_id =
        g_idle_add(mdy_blanking_inhibit_broadcast_cb, 0);

EXIT:
    return;
}

/** Cancel pending delayed blanking inhibit signaling
 */
static void mdy_blanking_inhibit_cancel_broadcast(void)
{
    if( !mdy_blanking_inhibit_broadcast_id )
        goto EXIT;

    g_source_remove(mdy_blanking_inhibit_broadcast_id),
        mdy_blanking_inhibit_broadcast_id = 0;

EXIT:
    return;
}

// TIMER: DIM -> OFF

/** Display blanking timeout callback ID */
static guint mdy_blanking_off_cb_id = 0;

/**
 * Timeout callback for display blanking
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean mdy_blanking_off_cb(gpointer data)
{
    (void)data;

    if( !mdy_blanking_off_cb_id )
        goto EXIT;

    mce_log(LL_DEBUG, "BLANK timer triggered");

    mdy_blanking_off_cb_id = 0;

    mdy_blanking_inhibit_schedule_broadcast();

    /* Default to: display off */
    display_state_t next_state = MCE_DISPLAY_OFF;

    /* Use lpm on, if starting from on/dim and tklock is already set */
    switch( display_state ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        if( submode & MCE_TKLOCK_SUBMODE )
            next_state = MCE_DISPLAY_LPM_ON;
        break;
    default:
        break;
    }

    execute_datapipe(&display_state_req_pipe,
                     GINT_TO_POINTER(next_state),
                     USE_INDATA, CACHE_INDATA);

    /* Remove wakelock unless the timer got re-programmed */
    if( !mdy_blanking_off_cb_id  )
        wakelock_unlock("mce_lpm_off");
EXIT:

    return FALSE;
}

/**
 * Cancel the display blanking timeout
 */
static void mdy_blanking_cancel_off(void)
{
    /* Remove the timeout source for display blanking */
    if( mdy_blanking_off_cb_id != 0 ) {
        mce_log(LL_DEBUG, "BLANK timer cancelled");
        g_source_remove(mdy_blanking_off_cb_id);
        mdy_blanking_off_cb_id = 0;

        mdy_blanking_inhibit_schedule_broadcast();

        /* unlock on cancellation */
        wakelock_unlock("mce_lpm_off");
    }
}

/**
 * Setup blank timeout
 *
 * This needs to use a wakelock so that the device will not
 * suspend when LPM_OFF -> OFF transition is scheduled.
 */
static void mdy_blanking_schedule_off(void)
{
    gint timeout = mdy_blank_timeout;

    if( display_state == MCE_DISPLAY_LPM_OFF )
        timeout = mdy_blank_from_lpm_off_timeout;
    else if( submode & MCE_TKLOCK_SUBMODE ) {
        /* In case UI boots up to lockscreen, we need to
         * apply additional after-boot delay also to
         * blanking timer. */
        timeout = mdy_blanking_get_afterboot_delay();

        if( timeout < mdy_blank_from_lockscreen_timeout )
            timeout = mdy_blank_from_lockscreen_timeout;
    }

    /* In act dead mode blanking timeouts are capped */
    if( system_state == MCE_STATE_ACTDEAD ) {
        if( timeout > ACTDEAD_MAX_OFF_TIMEOUT )
            timeout = ACTDEAD_MAX_OFF_TIMEOUT;
    }

    /* Blanking pause can optionally stay in dimmed state */
    if( display_state == MCE_DISPLAY_DIM &&
        mdy_blanking_is_paused() &&
        mdy_blanking_pause_can_dim() ) {
        mdy_blanking_cancel_off();
        goto EXIT;
    }

    if( mdy_blanking_off_cb_id ) {
        g_source_remove(mdy_blanking_off_cb_id);
        mce_log(LL_DEBUG, "BLANK timer rescheduled @ %d secs", timeout);
    }
    else {
        wakelock_lock("mce_lpm_off", -1);
        mce_log(LL_DEBUG, "BLANK timer scheduled @ %d secs", timeout);
    }

    /* Use idle callback for zero timeout */
    if( timeout > 0 )
        mdy_blanking_off_cb_id = g_timeout_add(timeout * 1000,
                                               mdy_blanking_off_cb, 0);
    else
        mdy_blanking_off_cb_id = g_idle_add(mdy_blanking_off_cb, 0);

    mdy_blanking_inhibit_schedule_broadcast();

EXIT:
    return;
}

// TIMER: LPM_ON -> LPM_OFF

/** Low power mode proximity blank timeout callback ID */
static guint mdy_blanking_lpm_off_cb_id = 0;

/**
 * Timeout callback for low power mode proximity blank
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean mdy_blanking_lpm_off_cb(gpointer data)
{
    (void)data;

    mce_log(LL_DEBUG, "LPM-BLANK timer triggered");

    mdy_blanking_lpm_off_cb_id = 0;

    execute_datapipe(&display_state_req_pipe,
                     GINT_TO_POINTER(MCE_DISPLAY_LPM_OFF),
                     USE_INDATA, CACHE_INDATA);

    return FALSE;
}

/**
 * Cancel the low power mode proximity blank timeout
 */
static void mdy_blanking_cancel_lpm_off(void)
{
    /* Remove the timeout source for low power mode */
    if (mdy_blanking_lpm_off_cb_id != 0) {
        mce_log(LL_DEBUG, "LPM-BLANK timer cancelled");
        g_source_remove(mdy_blanking_lpm_off_cb_id);
        mdy_blanking_lpm_off_cb_id = 0;
    }
}

/**
 * Setup low power mode proximity blank timeout if supported
 */
static void mdy_blanking_schedule_lpm_off(void)
{
    gint timeout = mdy_blank_from_lpm_on_timeout;

    mdy_blanking_cancel_lpm_off();

    /* Setup new timeout */
    mce_log(LL_DEBUG, "LPM-BLANK timer scheduled @ %d secs", timeout);
    mdy_blanking_lpm_off_cb_id =
        g_timeout_add_seconds(timeout,
                              mdy_blanking_lpm_off_cb, NULL);
    return;
}

// PERIOD: BLANKING PAUSE

/** ID for display blank prevention timer source */
static guint mdy_blanking_pause_period_cb_id = 0;

/**
 * Timeout callback for display blanking pause
 *
 * @param data Unused
 *
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean mdy_blanking_pause_period_cb(gpointer data)
{
    (void)data;

    if( mdy_blanking_pause_period_cb_id ) {
        mce_log(LL_DEVEL, "BLANKING PAUSE timeout");
        mdy_blanking_pause_period_cb_id = 0;
        mdy_blanking_remove_pause_clients();

        mdy_dbus_send_blanking_pause_status(0);
    }

    return FALSE;
}

/**
 * Cancel blank prevention timeout
 */
static void mdy_blanking_stop_pause_period(void)
{
    if( mdy_blanking_pause_period_cb_id ) {
        mce_log(LL_DEVEL, "BLANKING PAUSE cancelled");
        g_source_remove(mdy_blanking_pause_period_cb_id),
            mdy_blanking_pause_period_cb_id = 0;

        mdy_dbus_send_blanking_pause_status(0);
    }
}

/**
 * Prevent screen blanking for display_timeout seconds
 */
static void mdy_blanking_start_pause_period(void)
{
    /* Cancel existing timeout */
    if( mdy_blanking_pause_period_cb_id )
        g_source_remove(mdy_blanking_pause_period_cb_id);

    /* Setup new timeout */
    mdy_blanking_pause_period_cb_id =
        g_timeout_add_seconds(mdy_blank_prevent_timeout,
                              mdy_blanking_pause_period_cb, NULL);

    mce_log(LL_DEBUG, "BLANKING PAUSE started; period = %d",
            mdy_blank_prevent_timeout);

    mdy_dbus_send_blanking_pause_status(0);
}

/** List of monitored blanking pause clients */
static GSList *mdy_blanking_pause_clients = NULL;

/** Blanking pause is active predicate
 *
 * @returns true if there are active clients, false otherwise
 */
static bool mdy_blanking_is_paused(void)
{
    return mdy_blanking_pause_period_cb_id != 0;
    //return mdy_blanking_pause_clients != 0;
}

/** Dimming allowed while blanking is paused predicate
 *
 * returns true if dimming is allowed, false otherwise
 */
static bool mdy_blanking_pause_can_dim(void)
{
    return mdy_blanking_pause_mode == BLANKING_PAUSE_MODE_ALLOW_DIM;
}

/** Blanking pause is allowed predicate
 *
 * returns true if blanking pause is allowed, false otherwise
 */
static bool mdy_blanking_pause_is_allowed(void)
{
    return mdy_blanking_pause_mode != BLANKING_PAUSE_MODE_DISABLED;
}

/** Add blanking pause client
 *
 * @param name The private the D-Bus name of the client
 */
static void mdy_blanking_add_pause_client(const gchar *name)
{
    gssize rc = -1;

    if( !name )
        goto EXIT;

    // check if the feature is disabled
    if( !mdy_blanking_pause_is_allowed() ) {
        mce_log(LL_DEBUG, "blanking pause request from`%s ignored';"
                " feature is disabled", name);
        goto EXIT;
    }

    // display must be on
    switch( display_state ) {
    case MCE_DISPLAY_ON:
        // always allowed
        break;

    case MCE_DISPLAY_DIM:
        // optionally allowed
        if( mdy_blanking_pause_can_dim() )
            break;
        // fall through

    default:
        mce_log(LL_WARN, "blanking pause request from`%s ignored';"
                " display not on", name);
        goto EXIT;
    }

    // and tklock off
    if( submode & MCE_TKLOCK_SUBMODE ) {
        mce_log(LL_WARN, "blanking pause request from`%s ignored';"
                " tklock on", name);
        goto EXIT;
    }

    rc = mce_dbus_owner_monitor_add(name,
                                    mdy_blanking_pause_client_lost_cb,
                                    &mdy_blanking_pause_clients,
                                    BLANKING_PAUSE_MAX_MONITORED);
    if( rc < 0 ) {
        mce_log(LL_WARN, "Failed to add name owner monitor for `%s'", name);
        goto EXIT;
    }

    mdy_blanking_start_pause_period();
    mdy_blanking_rethink_timers(true);

EXIT:
    return;
}

/** Remove blanking pause client
 *
 * @param name The private the D-Bus name of the client
 *
 * @return TRUE on success, FALSE if name is NULL
 */
static gboolean mdy_blanking_remove_pause_client(const gchar *name)
{
    gssize rc = -1;

    if( !name )
        goto EXIT;

    rc = mce_dbus_owner_monitor_remove(name, &mdy_blanking_pause_clients);

    if( rc < 0 ) {
        // name was not monitored
        goto EXIT;
    }

    if( rc == 0 ) {
        /* no names left, remove the timeout */
        mdy_blanking_stop_pause_period();
        mdy_blanking_rethink_timers(true);
    }

EXIT:
    return (rc != -1);
}

/** Remove all clients, stop blanking pause */
static void mdy_blanking_remove_pause_clients(void)
{
    /* If there are clients to remove or blanking pause timer to stop,
     * we need to re-evaluate need for dimming timer before returning */
    bool rethink = (mdy_blanking_pause_clients || mdy_blanking_is_paused());

    /* Remove all name monitors for the blanking pause requester */
    mce_dbus_owner_monitor_remove_all(&mdy_blanking_pause_clients);

    /* Stop blank prevent timer */
    mdy_blanking_stop_pause_period();

    if( rethink )
        mdy_blanking_rethink_timers(true);
}

/** Handle blanking pause clients dropping from dbus
 *
 * D-Bus callback used for monitoring the process that requested
 * blanking prevention; if that process exits, immediately
 * cancel the blanking timeout and resume normal operation
 *
 * @param msg The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_blanking_pause_client_lost_cb(DBusMessage *const msg)
{
    gboolean    status     = FALSE;
    const char *dbus_name  = 0;
    const char *prev_owner = 0;
    const char *curr_owner = 0;
    DBusError   error      = DBUS_ERROR_INIT;

    if( !dbus_message_get_args(msg, &error,
                               DBUS_TYPE_STRING, &dbus_name,
                               DBUS_TYPE_STRING, &prev_owner,
                               DBUS_TYPE_STRING, &curr_owner,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to get argument from %s.%s; %s",
                "org.freedesktop.DBus", "NameOwnerChanged",
                error.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "blanking pause client %s lost", dbus_name);

    mdy_blanking_remove_pause_client(dbus_name);
    status = TRUE;

EXIT:
    dbus_error_free(&error);
    return status;
}

// PERIOD: ADAPTIVE DIMMING

/**
 * Timeout callback for adaptive dimming timeout
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean mdy_blanking_adaptive_dimming_cb(gpointer data)
{
    (void)data;

    mdy_blanking_adaptive_dimming_cb_id = 0;
    mdy_adaptive_dimming_index = 0;

    return FALSE;
}

/**
 * Cancel the adaptive dimming timeout
 */
static void mdy_blanking_stop_adaptive_dimming(void)
{
    /* Remove the timeout source for adaptive dimming */
    if (mdy_blanking_adaptive_dimming_cb_id != 0) {
        g_source_remove(mdy_blanking_adaptive_dimming_cb_id);
        mdy_blanking_adaptive_dimming_cb_id = 0;
    }
}

/**
 * Setup adaptive dimming timeout
 */
static void mdy_blanking_start_adaptive_dimming(void)
{
    mdy_blanking_stop_adaptive_dimming();

    if (mdy_adaptive_dimming_enabled == FALSE)
        goto EXIT;

    /* Setup new timeout */
    mdy_blanking_adaptive_dimming_cb_id =
        g_timeout_add(mdy_adaptive_dimming_threshold,
                      mdy_blanking_adaptive_dimming_cb, NULL);

EXIT:
    return;
}

// AUTOMATIC BLANKING STATE MACHINE

/** Check if blanking inhibit mode denies turning display off
 *
 * @return true if automatic blanking should not happen, false otherwise
 */
static bool mdy_blanking_inhibit_off_p(void)
{
    bool inhibit = false;

    /* Blanking inhibit is explicitly ignored in act dead */
    switch( system_state ) {
    case MCE_STATE_ACTDEAD:
        goto EXIT;

    default:
        break;
    }

    /* Evaluate charger related blanking inhibit policy */
    switch( mdy_blanking_inhibit_mode ) {
    case INHIBIT_STAY_DIM:
        inhibit = true;
        break;

    case INHIBIT_STAY_DIM_WITH_CHARGER:
        if( charger_state == CHARGER_STATE_ON )
            inhibit = true;
        break;

    default:
        break;
    }

    /* Evaluate kbd slide related blanking inhibit policy */
    switch( mdy_kbd_slide_inhibit_mode ) {
    case KBD_SLIDE_INHIBIT_STAY_DIM_WHEN_OPEN:
        if( kbd_slide_input_state == COVER_OPEN )
            inhibit = true;
        break;

    default:
        break;
    }

EXIT:
    return inhibit;
}

/** Check if blanking inhibit mode denies dimming display
 *
 * @return true if automatic dimming should not happen, false otherwise
 */
static bool mdy_blanking_inhibit_dim_p(void)
{
    bool inhibit = false;

    /* Blanking inhibit is explicitly ignored in act dead */
    switch( system_state ) {
    case MCE_STATE_ACTDEAD:
        goto EXIT;

    default:
        break;
    }

    /* Evaluate charger related blanking inhibit policy */
    switch( mdy_blanking_inhibit_mode ) {
    case INHIBIT_STAY_ON:
        inhibit = true;
        break;

    case INHIBIT_STAY_ON_WITH_CHARGER:
        if( charger_state == CHARGER_STATE_ON )
            inhibit = true;
        break;

    default:
        break;
    }

    /* Evaluate kbd slide related blanking inhibit policy */
    switch( mdy_kbd_slide_inhibit_mode ) {
    case KBD_SLIDE_INHIBIT_STAY_ON_WHEN_OPEN:
        if( kbd_slide_input_state == COVER_OPEN )
            inhibit = true;
        break;

    default:
        break;
    }

EXIT:
    return inhibit;
}

/** Reprogram blanking timers
 */
static void mdy_blanking_rethink_timers(bool force)
{
    // TRIGGERS:
    // submode           <- mdy_datapipe_submode_cb()
    // display_state     <- mdy_display_state_changed()
    // audio_route       <- mdy_datapipe_audio_route_cb()
    // charger_state     <- mdy_datapipe_charger_state_cb()
    // exception_state   <- mdy_datapipe_exception_state_cb()
    // call_state        <- mdy_datapipe_call_state_trigger_cb()
    //
    // INPUTS:
    // proximity_state   <- mdy_datapipe_proximity_sensor_cb()
    // mdy_blanking_inhibit_mode <- mdy_gconf_cb()
    // mdy_blanking_is_paused()

    static display_state_t prev_display_state = MCE_DISPLAY_UNDEF;

    static cover_state_t prev_proximity_state = COVER_UNDEF;

    static uiexctype_t prev_exception_state = UIEXC_NONE;

    static call_state_t prev_call_state = CALL_STATE_NONE;

    static charger_state_t prev_charger_state = CHARGER_STATE_UNDEF;

    static audio_route_t prev_audio_route = AUDIO_ROUTE_HANDSET;

    static submode_t prev_tklock_mode = 0;
    submode_t tklock_mode = submode & MCE_TKLOCK_SUBMODE;

    if( prev_tklock_mode != tklock_mode )
        force = true;

    if( prev_audio_route != audio_route )
        force = true;

    if( prev_charger_state != charger_state )
        force = true;

    if( prev_exception_state != exception_state )
        force = true;

    if( prev_call_state != call_state )
        force = true;

    if( prev_proximity_state != proximity_state )
        force = true;

    if( prev_display_state != display_state ) {
        force = true;

        /* Stop blanking pause period, unless toggling between
         * ON and DIM states while dimming during blanking
         * pause is allowed */

        if( (prev_display_state == MCE_DISPLAY_ON ||
             prev_display_state == MCE_DISPLAY_DIM) &&
            (display_state == MCE_DISPLAY_ON ||
             display_state == MCE_DISPLAY_DIM) &&
            mdy_blanking_is_paused() &&
            mdy_blanking_pause_can_dim() ) {
            // keep existing blanking pause timer alive
        }
        else {
            // stop blanking pause period
            mdy_blanking_stop_pause_period();
        }

        // handle adaptive blanking states
        switch( display_state ) {
        default:
        case MCE_DISPLAY_UNDEF:
        case MCE_DISPLAY_OFF:
        case MCE_DISPLAY_LPM_OFF:
        case MCE_DISPLAY_LPM_ON:
        case MCE_DISPLAY_POWER_UP:
        case MCE_DISPLAY_POWER_DOWN:
            mdy_blanking_stop_adaptive_dimming();
            mdy_adaptive_dimming_index = 0;
            break;

        case MCE_DISPLAY_DIM:
            mdy_blanking_start_adaptive_dimming();
            break;

        case MCE_DISPLAY_ON:
            mdy_blanking_stop_adaptive_dimming();
            break;
        }
    }

    mce_log(LL_DEBUG, "update %s", force ? "YES" : "NO");

    if( !force )
        goto EXIT;

    mdy_blanking_cancel_dim();
    mdy_blanking_cancel_off();
    mdy_blanking_cancel_lpm_off();

    if( exception_state & ~UIEXC_CALL ) {
        /* exceptional ui states other than
         * call ui -> no dim/blank timers */
        goto EXIT;
    }

    switch( display_state ) {
    case MCE_DISPLAY_OFF:
        break;

    case MCE_DISPLAY_LPM_OFF:
        mdy_blanking_schedule_off();
        break;

    case MCE_DISPLAY_LPM_ON:
        mdy_blanking_schedule_lpm_off();
        break;

    case MCE_DISPLAY_DIM:
        if( mdy_update_mode )
            break;
        if( mdy_blanking_inhibit_off_p() )
            break;
        mdy_blanking_schedule_off();
        break;

    case MCE_DISPLAY_ON:
        if( mdy_update_mode )
            break;
        if( exception_state & ~UIEXC_CALL ) {
            break;
        }

        if( mdy_blanking_inhibit_dim_p() )
            break;

        if( exception_state & UIEXC_CALL ) {
            // do not dim-blank when handling incoming call
            if( call_state == CALL_STATE_RINGING )
                break;

            // no dim-blank timers with handset audio
            // ... while proximity covered
            if( audio_route == AUDIO_ROUTE_HANDSET  &&
                proximity_state == COVER_CLOSED )
                break;
            // dim-blank timers used with speaker/headset
            mdy_blanking_schedule_dim();
            break;
        }

        if( tklock_mode ) {
            mdy_blanking_schedule_off();
            break;
        }

        if( mdy_blanking_is_paused() && !mdy_blanking_pause_can_dim() )
            break;

        mdy_blanking_schedule_dim();
        break;

    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
        break;
    }

EXIT:
    prev_display_state = display_state;
    prev_proximity_state = proximity_state;
    prev_exception_state = exception_state;
    prev_call_state = call_state;
    prev_charger_state = charger_state;
    prev_audio_route = audio_route;
    prev_tklock_mode = tklock_mode;

    return;
}

/** Reprogram blanking timers on proximity triggers
 */
static void mdy_blanking_rethink_proximity(void)
{
    switch( display_state ) {
    case MCE_DISPLAY_LPM_ON:
        if( proximity_state == COVER_CLOSED )
            execute_datapipe(&display_state_req_pipe,
                             GINT_TO_POINTER(MCE_DISPLAY_LPM_OFF),
                             USE_INDATA, CACHE_INDATA);
        else
            mdy_blanking_schedule_lpm_off();
        break;

    case MCE_DISPLAY_LPM_OFF:
        if( proximity_state == COVER_OPEN &&
            lid_cover_policy_state != COVER_CLOSED )
            execute_datapipe(&display_state_req_pipe,
                             GINT_TO_POINTER(MCE_DISPLAY_LPM_ON),
                             USE_INDATA, CACHE_INDATA);
        else
            mdy_blanking_schedule_off();
        break;

    default:
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
        break;
    }
}

/** Cancel all timers that are display state specific
 */
static void mdy_blanking_cancel_timers(void)
{
    mdy_blanking_cancel_dim();
    mdy_blanking_cancel_off();
    mdy_blanking_cancel_lpm_off();

    //mdy_blanking_stop_pause_period();
    //mdy_hbm_cancel_timeout();
    mdy_brightness_stop_fade_timer();
    //mdy_blanking_stop_adaptive_dimming();
}

/** End of after bootup autoblank prevent; initially not active */
static int64_t mdy_blanking_afterboot_limit = 0;

/** Get delay until end of after boot blank prevent
 *
 * @return seconds to end of after boot blank prevent, or
 *         zero if time limit has been already passed
 */
static gint mdy_blanking_get_afterboot_delay(void)
{
    gint delay = 0;
    if( mdy_blanking_afterboot_limit ) {
        int64_t now = mce_lib_get_boot_tick();
        int64_t tmo = mdy_blanking_afterboot_limit - now;
        if( tmo > 0 )
            delay = (gint)tmo;
    }
    return (delay + 999) / 1000;
}

/** Evaluate need for longer after-boot blanking delay
 */
static void mdy_blanking_rethink_afterboot_delay(void)
{
    int64_t want_limit = 0;

    /* Bootup has not yet finished */
    if( mdy_init_done )
        goto DONE;

    /* We are booting to USER mode */
    if( mdy_bootstate != BOOTSTATE_USER )
        goto DONE;

    if( system_state != MCE_STATE_USER )
        goto DONE;

    /* Lipstick has started */
    if( lipstick_service_state != SERVICE_STATE_RUNNING )
        goto DONE;

    /* Display is/soon will be powered on */
    if( display_state_next != MCE_DISPLAY_ON )
        goto DONE;

    /* And limit has not yet been set */
    if( mdy_blanking_afterboot_limit )
        goto EXIT;

    /* Set up Use longer after-boot dim timeout */
    want_limit = (mce_lib_get_boot_tick() +
                  AFTERBOOT_BLANKING_TIMEOUT * 1000);

DONE:

    if( mdy_blanking_afterboot_limit == want_limit )
        goto EXIT;

    /* Enable long delay when needed, but disable only after
     * display leaves powered on state */
    if( want_limit || display_state_next != MCE_DISPLAY_ON ) {
        mce_log(LL_DEBUG, "after boot blank prevent %s",
                want_limit ? "activated" : "deactivated");

        mdy_blanking_afterboot_limit = want_limit;

        /* If dim/blank timer is running, reprogram it */
        if( mdy_blanking_dim_cb_id )
            mdy_blanking_schedule_dim();
        else if( mdy_blanking_off_cb_id )
            mdy_blanking_schedule_off();
    }

EXIT:
    return;
}

/* ========================================================================= *
 * DISPLAY_TYPE_PROBING
 * ========================================================================= */

/** Callback function for logging errors within glob()
 *
 * @param path path to file/dir where error occurred
 * @param err  errno that occurred
 *
 * @return 0 (= do not stop glob)
 */
static int mdy_display_type_glob_err_cb(const char *path, int err)
{
    mce_log(LL_WARN, "%s: glob: %s", path, g_strerror(err));
    return 0;
}

/** Check if sysfs directory contains brightness and max_brightness entries
 *
 * @param sysfs directory to probe
 * @param setpath place to store path to brightness file
 * @param maxpath place to store path to max_brightness file
 * @return TRUE if brightness and max_brightness files were found,
 *         FALSE otherwise
 */
static gboolean mdy_display_type_probe_brightness(const gchar *dirpath,
                                        char **setpath, char **maxpath)
{
    gboolean  res = FALSE;

    gchar *set = g_strdup_printf("%s/brightness", dirpath);
    gchar *max = g_strdup_printf("%s/max_brightness", dirpath);

    if( set && max && !g_access(set, W_OK) && !g_access(max, R_OK) ) {
        *setpath = set, set = 0;
        *maxpath = max, max = 0;
        res = TRUE;
    }

    g_free(set);
    g_free(max);

    return res;
}

/** Get the display type from MCE_CONF_DISPLAY_GROUP config group
 *
 * @param display_type where to store the selected display type
 *
 * @return TRUE if valid configuration was found, FALSE otherwise
 */

static gboolean mdy_display_type_get_from_config(display_type_t *display_type)
{
    gboolean   res = FALSE;
    gchar     *set = 0;
    gchar     *max = 0;

    gchar    **vdir = 0;
    gchar    **vset = 0;
    gchar    **vmax = 0;
    gsize      nset = 0;
    gsize      nmax = 0;

    /* First check if we have a configured brightness directory
     * that a) exists and b) contains both brightness and
     * max_brightness files */

    vdir = mce_conf_get_string_list(MCE_CONF_DISPLAY_GROUP,
                                    MCE_CONF_BACKLIGHT_DIRECTORY, 0);
    if( vdir ) {
        for( size_t i = 0; vdir[i]; ++i ) {
            if( !*vdir[i] || g_access(vdir[i], F_OK) )
                continue;

            if( mdy_display_type_probe_brightness(vdir[i], &set, &max) )
                goto EXIT;
        }
    }

    /* Then check if we can find mathes from possible brightness and
     * max_brightness file lists */

    vset = mce_conf_get_string_list(MCE_CONF_DISPLAY_GROUP,
                                    MCE_CONF_BACKLIGHT_PATH, &nset);

    vmax = mce_conf_get_string_list(MCE_CONF_DISPLAY_GROUP,
                                    MCE_CONF_MAX_BACKLIGHT_PATH, &nmax);

    if( nset != nmax ) {
        mce_log(LL_WARN, "%s and %s do not have the same amount of "
                "configuration entries",
                MCE_CONF_BACKLIGHT_PATH, MCE_CONF_MAX_BACKLIGHT_PATH);
    }

    if( nset > nmax )
        nset = nmax;

    for( gsize i = 0; i < nset; ++i ) {
        if( *vset[i] == 0 || g_access(vset[i], W_OK) != 0 )
            continue;

        if( *vmax[i] == 0 || g_access(vmax[i], R_OK) != 0 )
            continue;

        set = g_strdup(vset[i]);
        max = g_strdup(vmax[i]);
        break;
    }

EXIT:
    /* Have we found both brightness and max_brightness files? */
    if( set && max ) {
        mce_log(LL_NOTICE, "applying DISPLAY_TYPE_GENERIC from config file");
        mce_log(LL_NOTICE, "brightness path = %s", set);
        mce_log(LL_NOTICE, "max_brightness path = %s", max);

        mdy_brightness_level_output.path  = set, set = 0;
        mdy_brightness_level_maximum_path = max, max = 0;

        mdy_cabc_mode_file            = 0;
        mdy_cabc_available_modes_file = 0;
        mdy_cabc_is_supported         = 0;

        *display_type = DISPLAY_TYPE_GENERIC;
        res = TRUE;
    }

    g_free(max);
    g_free(set);

    g_strfreev(vmax);
    g_strfreev(vset);
    g_strfreev(vdir);

    return res;
}

/** Get the display type by looking up from sysfs
 *
 * @param display_type where to store the selected display type
 *
 * @return TRUE if valid configuration was found, FALSE otherwise
 */
static gboolean mdy_display_type_get_from_sysfs_probe(display_type_t *display_type)
{
    static const char pattern[] = "/sys/class/backlight/*";

    static const char * const lut[] = {
        /* this seems to be some kind of "Android standard" path */
        "/sys/class/leds/lcd-backlight",
        NULL
    };

    gboolean   res = FALSE;
    gchar     *set = 0;
    gchar     *max = 0;

    glob_t    gb;

    memset(&gb, 0, sizeof gb);

    /* Assume: Any match from fixed list will be true positive.
     * Check them before possibly ambiguous backlight class entries. */
    for( size_t i = 0; lut[i]; ++i ) {
        if( mdy_display_type_probe_brightness(lut[i], &set, &max) )
            goto EXIT;
    }

    if( glob(pattern, 0, mdy_display_type_glob_err_cb, &gb) != 0 ) {
        mce_log(LL_WARN, "no backlight devices found");
        goto EXIT;
    }

    if( gb.gl_pathc > 1 )
        mce_log(LL_WARN, "several backlight devices present, "
                "choosing the first usable one");

    for( size_t i = 0; i < gb.gl_pathc; ++i ) {
        const char *path = gb.gl_pathv[i];

        if( mdy_display_type_probe_brightness(path, &set, &max) )
            goto EXIT;
    }

EXIT:
    /* Have we found both brightness and max_brightness files? */
    if( set && max ) {
        mce_log(LL_NOTICE, "applying DISPLAY_TYPE_GENERIC from sysfs probe");
        mce_log(LL_NOTICE, "brightness path = %s", set);
        mce_log(LL_NOTICE, "max_brightness path = %s", max);

        mdy_brightness_level_output.path = set, set = 0;
        mdy_brightness_level_maximum_path    = max, max = 0;

        mdy_cabc_mode_file            = 0;
        mdy_cabc_available_modes_file = 0;
        mdy_cabc_is_supported            = 0;

        *display_type = DISPLAY_TYPE_GENERIC;
        res = TRUE;
    }

    g_free(max);
    g_free(set);

    globfree(&gb);

    return res;
}

static gboolean mdy_display_type_get_from_hybris(display_type_t *display_type)
{
#ifdef ENABLE_HYBRIS
    gboolean   res = FALSE;
    if( !mce_hybris_backlight_init() ) {
        mce_log(LL_DEBUG, "libhybris brightness controls not available");
        goto EXIT;
    }

    mce_log(LL_NOTICE, "using libhybris for display brightness control");
    mdy_brightness_set_level_hook = mdy_brightness_set_level_hybris;
    mdy_brightness_level_maximum = 255;
    *display_type = DISPLAY_TYPE_GENERIC;

    res = TRUE;
EXIT:
    return res;
#else
    (void)display_type;
    return FALSE;
#endif
}

/**
 * Get the display type
 *
 * @return The display type
 */
static display_type_t mdy_display_type_get(void)
{
    static display_type_t display_type = DISPLAY_TYPE_UNSET;

    /* If we have the display type already, return it */
    if (display_type != DISPLAY_TYPE_UNSET)
        goto EXIT;

    if( mdy_display_type_get_from_config(&display_type) ) {
        // nop
    }
    else if (g_access(DISPLAY_BACKLIGHT_PATH DISPLAY_ACX565AKM, W_OK) == 0) {
        display_type = DISPLAY_TYPE_ACX565AKM;

        mdy_brightness_level_output.path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_ACX565AKM, DISPLAY_CABC_BRIGHTNESS_FILE, NULL);
        mdy_brightness_level_maximum_path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_ACX565AKM, DISPLAY_CABC_MAX_BRIGHTNESS_FILE, NULL);
        mdy_cabc_mode_file = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_ACX565AKM, DISPLAY_CABC_MODE_FILE, NULL);
        mdy_cabc_available_modes_file = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_ACX565AKM, DISPLAY_CABC_AVAILABLE_MODES_FILE, NULL);

        mdy_cabc_is_supported =
            (g_access(mdy_cabc_mode_file, W_OK) == 0);
    }
    else if (g_access(DISPLAY_BACKLIGHT_PATH DISPLAY_L4F00311, W_OK) == 0) {
        display_type = DISPLAY_TYPE_L4F00311;

        mdy_brightness_level_output.path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_L4F00311, DISPLAY_CABC_BRIGHTNESS_FILE, NULL);
        mdy_brightness_level_maximum_path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_L4F00311, DISPLAY_CABC_MAX_BRIGHTNESS_FILE, NULL);
        mdy_cabc_mode_file = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_L4F00311, DISPLAY_CABC_MODE_FILE, NULL);
        mdy_cabc_available_modes_file = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_L4F00311, DISPLAY_CABC_AVAILABLE_MODES_FILE, NULL);

        mdy_cabc_is_supported =
            (g_access(mdy_cabc_mode_file, W_OK) == 0);
    }
    else if (g_access(DISPLAY_BACKLIGHT_PATH DISPLAY_TAAL, W_OK) == 0) {
        display_type = DISPLAY_TYPE_TAAL;

        mdy_brightness_level_output.path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_TAAL, DISPLAY_CABC_BRIGHTNESS_FILE, NULL);
        mdy_brightness_level_maximum_path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_TAAL, DISPLAY_CABC_MAX_BRIGHTNESS_FILE, NULL);

        mdy_cabc_mode_file = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_TAAL, "/device", DISPLAY_CABC_MODE_FILE, NULL);
        mdy_cabc_available_modes_file = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_TAAL, "/device", DISPLAY_CABC_AVAILABLE_MODES_FILE, NULL);

        mdy_cabc_is_supported =
            (g_access(mdy_cabc_mode_file, W_OK) == 0);
    }
    else if (g_access(DISPLAY_BACKLIGHT_PATH DISPLAY_HIMALAYA, W_OK) == 0) {
        display_type = DISPLAY_TYPE_HIMALAYA;

        mdy_brightness_level_output.path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_HIMALAYA, DISPLAY_CABC_BRIGHTNESS_FILE, NULL);
        mdy_brightness_level_maximum_path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_HIMALAYA, DISPLAY_CABC_MAX_BRIGHTNESS_FILE, NULL);

        mdy_cabc_mode_file = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_HIMALAYA, "/device", DISPLAY_CABC_MODE_FILE, NULL);
        mdy_cabc_available_modes_file = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_HIMALAYA, "/device", DISPLAY_CABC_AVAILABLE_MODES_FILE, NULL);

        mdy_cabc_is_supported =
            (g_access(mdy_cabc_mode_file, W_OK) == 0);
    }
    else if (g_access(DISPLAY_BACKLIGHT_PATH DISPLAY_DISPLAY0, W_OK) == 0) {
        display_type = DISPLAY_TYPE_DISPLAY0;

        mdy_brightness_level_output.path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_DISPLAY0, DISPLAY_CABC_BRIGHTNESS_FILE, NULL);
        mdy_brightness_level_maximum_path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_DISPLAY0, DISPLAY_CABC_MAX_BRIGHTNESS_FILE, NULL);

        mdy_cabc_mode_file = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_DISPLAY0, "/device", DISPLAY_CABC_MODE_FILE, NULL);
        mdy_cabc_available_modes_file = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_DISPLAY0, "/device", DISPLAY_CABC_AVAILABLE_MODES_FILE, NULL);
        mdy_brightness_hw_fading_output.path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_DISPLAY0, DISPLAY_DEVICE_PATH, DISPLAY_HW_DIMMING_FILE, NULL);
        mdy_high_brightness_mode_output.path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_DISPLAY0, DISPLAY_DEVICE_PATH, DISPLAY_HBM_FILE, NULL);
        mdy_low_power_mode_file = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_DISPLAY0, DISPLAY_DEVICE_PATH, DISPLAY_LPM_FILE, NULL);

        mdy_cabc_is_supported =
            (g_access(mdy_cabc_mode_file, W_OK) == 0);
        mdy_brightness_hw_fading_is_supported =
            (g_access(mdy_brightness_hw_fading_output.path, W_OK) == 0);
        mdy_high_brightness_mode_supported =
            (g_access(mdy_high_brightness_mode_output.path, W_OK) == 0);
        mdy_low_power_mode_supported =
            (g_access(mdy_low_power_mode_file, W_OK) == 0);

        /* Enable hardware fading if supported */
        if (mdy_brightness_hw_fading_is_supported == TRUE)
            (void)mce_write_number_string_to_file(&mdy_brightness_hw_fading_output, 1);
    }
    else if (g_access(DISPLAY_BACKLIGHT_PATH DISPLAY_ACPI_VIDEO0, W_OK) == 0) {
        display_type = DISPLAY_TYPE_ACPI_VIDEO0;

        mdy_brightness_level_output.path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_ACPI_VIDEO0, DISPLAY_CABC_BRIGHTNESS_FILE, NULL);
        mdy_brightness_level_maximum_path = g_strconcat(DISPLAY_BACKLIGHT_PATH, DISPLAY_ACPI_VIDEO0, DISPLAY_CABC_MAX_BRIGHTNESS_FILE, NULL);
    }
    else if (g_access(DISPLAY_GENERIC_PATH, W_OK) == 0) {
        display_type = DISPLAY_TYPE_GENERIC;

        mdy_brightness_level_output.path = g_strconcat(DISPLAY_GENERIC_PATH, DISPLAY_GENERIC_BRIGHTNESS_FILE, NULL);
        mdy_brightness_level_maximum_path = g_strconcat(DISPLAY_GENERIC_PATH, DISPLAY_GENERIC_MAX_BRIGHTNESS_FILE, NULL);
    }
    else if( mdy_display_type_get_from_sysfs_probe(&display_type) ) {
        // nop
    }
    else if( mdy_display_type_get_from_hybris(&display_type) ) {
        /* nop */
    }
    else {
        display_type = DISPLAY_TYPE_NONE;
    }

    errno = 0;

    mce_log(LL_DEBUG, "Display type: %d", display_type);

EXIT:
    return display_type;
}

/* ========================================================================= *
 * FBDEV_SLEEP_AND_WAKEUP
 * ========================================================================= */

/** Input watch callback for frame buffer resume waiting
 *
 * Gets triggered when worker thread writes to pipe
 *
 * @param chn  (not used)
 * @param cnd  (not used)
 * @param aptr state data (as void pointer)
 *
 * @return FALSE (to disable the input watch)
 */
#ifdef ENABLE_WAKELOCKS
static gboolean mdy_waitfb_event_cb(GIOChannel *chn,
                                    GIOCondition cnd,
                                    gpointer aptr)
{
    (void)chn;

    waitfb_t *self = aptr;
    gboolean  keep = FALSE;

    if( !self->pipe_id )
        goto EXIT;

    if( cnd & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) ) {
        goto EXIT;
    }

    char tmp[64];
    int fd = g_io_channel_unix_get_fd(chn);
    int rc = read(fd, tmp, sizeof tmp);

    if( rc == -1 ) {
        if( errno == EINTR || errno == EAGAIN )
            keep = TRUE;
        else
            mce_log(LL_ERR, "read events: %m");
        goto EXIT;
    }
    if( rc == 0 ) {
        mce_log(LL_ERR, "read events: EOF");
        goto EXIT;
    }

    keep = TRUE;
    self->suspended = (tmp[rc-1] == 'S');
    mce_log(LL_NOTICE, "read:%d, suspended:%d", rc, self->suspended);
    mdy_stm_schedule_rethink();

EXIT:
    if( !keep && self->pipe_id ) {
        self->pipe_id = 0;
        mce_log(LL_CRIT, "stopping io watch");
        mdy_waitfb_thread_stop(self);
    }
    return keep;
}
#endif /* ENABLE_WAKELOCKS */

/** Wait for fb sleep/wakeup thread
 *
 * Alternates between waiting for fb wakeup and sleep.
 * Signals mainloop about the changes via a pipe.
 *
 * @param aptr state data (as void pointer)
 *
 * @return 0
 */
#ifdef ENABLE_WAKELOCKS
static void *mdy_waitfb_thread_entry(void *aptr)
{
    waitfb_t *self = aptr;

    /* allow quick and dirty cancellation */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, 0);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0);

    /* block in sysfs read */
    char tmp[32];
    int rc;
    for( ;; ) {
        // FIXME: check if mce_log() is thread safe

        //fprintf(stderr, "READ <<<< %s\n", self->wake_path);

        /* wait for fb wakeup */
        self->wake_fd = TEMP_FAILURE_RETRY(open(self->wake_path, O_RDONLY));
        if( self->wake_fd == -1 ) {
            fprintf(stderr, "%s: open: %m", self->wake_path);
            break;
        }
        rc = TEMP_FAILURE_RETRY(read(self->wake_fd, tmp, sizeof tmp));
        if( rc == -1 ) {
            fprintf(stderr, "%s: %m", self->wake_path);
            break;
        }
        TEMP_FAILURE_RETRY(close(self->wake_fd)), self->wake_fd = -1;

        /* send "woke up" to mainloop */
        TEMP_FAILURE_RETRY(write(self->pipe_fd, "W", 1));

        //fprintf(stderr, "READ <<<< %s\n", self->sleep_path);

        /* wait for fb sleep */
        self->sleep_fd = TEMP_FAILURE_RETRY(open(self->sleep_path, O_RDONLY));
        if( self->sleep_fd == -1 ) {
            fprintf(stderr, "%s: open: %m", self->sleep_path);
            break;
        }
        rc = TEMP_FAILURE_RETRY(read(self->sleep_fd, tmp, sizeof tmp));
        if( rc == -1 ) {
            fprintf(stderr, "%s: %m", self->sleep_path);
            break;
        }
        TEMP_FAILURE_RETRY(close(self->sleep_fd)), self->sleep_fd = -1;

        /* send "sleeping" to mainloop */
        TEMP_FAILURE_RETRY(write(self->pipe_fd, "S", 1));
    }

    /* mark thread done and exit */
    self->finished = true;
    return 0;
}
#endif /* ENABLE_WAKELOCKS */

/** Start delayed display state change broadcast
 *
 * @param self state data
 *
 * @return TRUE if waiting was initiated succesfully, FALSE otherwise
 */
#ifdef ENABLE_WAKELOCKS
static gboolean mdy_waitfb_thread_start(waitfb_t *self)
{
    gboolean    res    = FALSE;

    GIOChannel *chn    = 0;
    int         pfd[2] = {-1, -1};

    mdy_waitfb_thread_stop(self);

    if( access(self->wake_path, F_OK) == -1 ||
        access(self->sleep_path, F_OK) == -1 )
        goto EXIT;

    if( pipe2(pfd, O_CLOEXEC) == -1 ) {
        mce_log(LL_ERR, "pipe: %m");
        goto EXIT;
    }

    self->pipe_fd = pfd[1], pfd[1] = -1;

    if( !(chn = g_io_channel_unix_new(pfd[0])) ) {
        goto EXIT;
    }
    self->pipe_id = g_io_add_watch(chn,
                                   G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                                   mdy_waitfb_event_cb, self);
    if( !self->pipe_id ) {
        goto EXIT;
    }
    g_io_channel_set_close_on_unref(chn, TRUE), pfd[0] = -1;

    self->finished = false;

    if( pthread_create(&self->thread, 0, mdy_waitfb_thread_entry, self) ) {
        mce_log(LL_ERR, "failed to create waitfb thread");
        goto EXIT;
    }

    res = TRUE;

EXIT:
    if( chn != 0 ) g_io_channel_unref(chn);
    if( pfd[1] != -1 ) close(pfd[1]);
    if( pfd[0] != -1 ) close(pfd[0]);

    /* all or nothing */
    if( !res ) mdy_waitfb_thread_stop(self);
    return res;
}
#endif /* ENABLE_WAKELOCKS */

/** Release all dynamic resources related to fb resume waiting
 *
 * @param self state data
 */
#ifdef ENABLE_WAKELOCKS
static void mdy_waitfb_thread_stop(waitfb_t *self)
{
    /* cancel worker thread */
    if( self->thread && !self->finished ) {
        mce_log(LL_DEBUG, "stopping waitfb thread");
        if( pthread_cancel(self->thread) != 0 ) {
            mce_log(LL_ERR, "failed to stop waitfb thread");
        }
        else {
            void *status = 0;
            pthread_join(self->thread, &status);
            mce_log(LL_DEBUG, "thread stopped, status = %p", status);
        }
    }
    self->thread  = 0;

    /* remove pipe input io watch */
    if( self->pipe_id ) {
        mce_log(LL_DEBUG, "remove pipe input watch");
        g_source_remove(self->pipe_id), self->pipe_id = 0;
    }

    /* close pipe output fd */
    if( self->pipe_fd != -1) {
        mce_log(LL_DEBUG, "close pipe write fd");
        close(self->pipe_fd), self->pipe_fd = -1;
    }

    /* close sysfs input fds */
    if( self->sleep_fd != -1 ) {
        mce_log(LL_DEBUG, "close %s", self->sleep_path);
        close(self->sleep_fd), self->sleep_fd = -1;
    }
    if( self->wake_fd != -1 ) {
        mce_log(LL_DEBUG, "close %s", self->wake_path);
        close(self->wake_fd), self->wake_fd = -1;
    }
}
#endif /* ENABLE_WAKELOCKS */

/** State information for wait for fb resume thread */
static waitfb_t mdy_waitfb_data =
{
    .suspended  = false,
    .thread     = 0,
    .finished   = false,
#if 0
    // debug kernel delay handling via fifos
    .wake_path  = "/tmp/wait_for_fb_wake",
    .sleep_path = "/tmp/wait_for_fb_sleep",
#else
    // real sysfs paths
    .wake_path  = "/sys/power/wait_for_fb_wake",
    .sleep_path = "/sys/power/wait_for_fb_sleep",
#endif
    .wake_fd    = -1,
    .sleep_fd   = -1,
    .pipe_fd    = -1,
    .pipe_id    = 0,
};

/* ========================================================================= *
 * COMPOSITOR_IPC
 * ========================================================================= */

/** Owner of compositor dbus name */
static gchar *mdy_compositor_priv_name = 0;

/** PID to kill when compositor does not react to setUpdatesEnabled() ipc  */
static int mdy_compositor_pid = -1;

/** Delay [s] from setUpdatesEnabled() to attempting compositor core dump */
static gint mdy_compositor_core_delay = DEFAULT_LIPSTICK_CORE_DELAY;

/** GConf callback ID for mdy_compositor_core_delay setting */
static guint mdy_compositor_core_delay_gconf_cb_id = 0;

/* Delay [s] from attempting compositor core dump to killing compositor */
static gint mdy_compositor_kill_delay = 25;

/* Delay [s] for verifying whether compositor did exit after kill attempt */
static gint mdy_compositor_verify_delay = 5;

/** Currently active compositor killing timer id */
static guint mdy_compositor_kill_id  = 0;

/** Currently active setUpdatesEnabled() method call */
static DBusPendingCall *mdy_compositor_state_req_pc = 0;

/** Timeout to use for setUpdatesEnabled method calls [ms]; -1 = use default */
static int mdy_compositor_ipc_timeout = 2 * 60 * 1000; /* 2 minutes */

/** UI side rendering state; no suspend unless RENDERER_DISABLED */
static renderer_state_t mdy_compositor_ui_state = RENDERER_UNKNOWN;

/** Enable/Disable compositor killing led pattern
 *
 * @param enable true to start the led, false to stop it
 */
static void mdy_compositor_set_killer_led(bool enable)
{
    static bool enabled = false;

    if( enabled == enable )
        goto EXIT;

    enabled = enable;
    execute_datapipe_output_triggers(enabled ?
                                     &led_pattern_activate_pipe :
                                     &led_pattern_deactivate_pipe,
                                     "PatternKillingLipstick",
                                     USE_INDATA);
EXIT:
    return;
}

/** Enabled/Disable setUpdatesEnabled failure led patterns
 */
static void mdy_compositor_set_panic_led(renderer_state_t req)
{
    bool blanking = false;
    bool unblanking = false;

    switch( req ) {
    case RENDERER_DISABLED:
        blanking = true;
        mce_log(LL_DEVEL, "start alert led pattern for: failed ui stop");
        break;
    case RENDERER_ENABLED:
        unblanking = true;
        mce_log(LL_DEVEL, "start alert led pattern for: failed ui start");
        break;
    default:
        break;
    }

    execute_datapipe_output_triggers(blanking ?
                                     &led_pattern_activate_pipe :
                                     &led_pattern_deactivate_pipe,
                                     "PatternDisplayBlankFailed",
                                     USE_INDATA);

    execute_datapipe_output_triggers(unblanking ?
                                     &led_pattern_activate_pipe :
                                     &led_pattern_deactivate_pipe,
                                     "PatternDisplayUnblankFailed",
                                     USE_INDATA);
}

/** Timer id for setUpdatesEnabled is taking too long */
static guint mdy_renderer_led_timer_id = 0;

/** Timer callback for setUpdatesEnabled is taking too long
 */
static gboolean mdy_compositor_panic_led_cb(gpointer aptr)
{
    renderer_state_t req = GPOINTER_TO_INT(aptr);

    if( !mdy_renderer_led_timer_id )
        goto EXIT;

    mce_log(LL_DEBUG, "compositor panic led timer triggered");

    mdy_renderer_led_timer_id = 0;
    mdy_compositor_set_panic_led(req);

EXIT:
    return FALSE;
}

/* Cancel setUpdatesEnabled is taking too long timer
 */
static void mdy_compositor_cancel_panic_led(void)
{
    mdy_compositor_set_panic_led(RENDERER_UNKNOWN);

    if( mdy_renderer_led_timer_id != 0 ) {
        mce_log(LL_DEBUG, "compositor panic led timer cancelled");
        g_source_remove(mdy_renderer_led_timer_id),
            mdy_renderer_led_timer_id = 0;
    }
}

/* Schedule setUpdatesEnabled is taking too long timer
 */
static void mdy_compositor_schedule_panic_led(renderer_state_t req)
{
    /* During bootup it is more or less expected that compositor is
     * unable to answer immediately. So we initially allow longer
     * delay and bring it down gradually to target level. */
    static int delay = LED_DELAY_UI_DISABLE_ENABLE_MAXIMUM;

    mdy_compositor_set_panic_led(RENDERER_UNKNOWN);

    if( mdy_renderer_led_timer_id != 0 )
        g_source_remove(mdy_renderer_led_timer_id);

    mdy_renderer_led_timer_id = g_timeout_add(delay,
                                              mdy_compositor_panic_led_cb,
                                              GINT_TO_POINTER(req));

    mce_log(LL_DEBUG, "compositor panic led timer sheduled @ %d ms", delay);

    delay = delay * 3 / 4;
    if( delay < LED_DELAY_UI_DISABLE_ENABLE_MINIMUM )
        delay = LED_DELAY_UI_DISABLE_ENABLE_MINIMUM;
}

/** Timer for verifying that compositor has exited after kill signal
 *
 * @param aptr Process identifier as void pointer
 *
 * @return FALSE to stop the timer from repeating
 */
static gboolean mdy_compositor_kill_verify_cb(gpointer aptr)
{
    int pid = GPOINTER_TO_INT(aptr);

    if( !mdy_compositor_kill_id )
        goto EXIT;

    mdy_compositor_kill_id = 0;

    if( kill(pid, 0) == -1 && errno == ESRCH )
        goto EXIT;

    mce_log(LL_ERR, "compositor is not responsive and killing it failed");

EXIT:
    /* Stop the led pattern even if we can't kill compositor process */
    mdy_compositor_set_killer_led(false);

    return FALSE;
}

/** Timer for killing compositor in case core dump attempt did not make it exit
 *
 * @param aptr Process identifier as void pointer
 *
 * @return FALSE to stop the timer from repeating
 */
static gboolean mdy_compositor_kill_kill_cb(gpointer aptr)
{
    int pid = GPOINTER_TO_INT(aptr);

    if( !mdy_compositor_kill_id )
        goto EXIT;

    mdy_compositor_kill_id = 0;

    /* In the unlikely event that asynchronous pid query is not finished
     * at the kill timeout, abandon the quest */
    if( pid == -1 ) {
        if( (pid = mdy_compositor_pid) == -1 ) {
            mce_log(LL_WARN, "pid of compositor not know yet; can't kill it");
            goto EXIT;
        }
    }

    /* If compositor is already gone after core dump attempt, no further
     * actions are needed */
    if( kill(pid, 0) == -1 && errno == ESRCH )
        goto EXIT;

    mce_log(LL_WARN, "compositor is not responsive; attempting to kill it");

    /* Send SIGKILL to compositor; if that succeeded, verify after brief
     * delay if the process is really gone */

    if( kill(pid, SIGKILL) == -1 ) {
        mce_log(LL_ERR, "failed to SIGKILL compositor: %m");
    }
    else {
        mdy_compositor_kill_id =
            g_timeout_add(1000 * mdy_compositor_verify_delay,
                          mdy_compositor_kill_verify_cb,
                          GINT_TO_POINTER(pid));
    }

EXIT:
    /* Keep led pattern active if verify timer was scheduled */
    mdy_compositor_set_killer_led(mdy_compositor_kill_id != 0);

    return FALSE;
}

/** Timer for dumping compositor core if setUpdatesEnabled() goes without reply
 *
 * @param aptr Process identifier as void pointer
 *
 * @return FALSE to stop the timer from repeating
 */
static gboolean mdy_compositor_kill_core_cb(gpointer aptr)
{
    int pid = GPOINTER_TO_INT(aptr);

    if( !mdy_compositor_kill_id )
        goto EXIT;

    mdy_compositor_kill_id = 0;

    mce_log(LL_WARN, "compositor is not responsive; attempting to core dump it");

    /* In the unlikely event that asynchronous pid query is not finished
     * at the core dump timeout, wait a while longer and just kill it */
    if( pid == -1 ) {
        if( (pid = mdy_compositor_pid) == -1 ) {
            mce_log(LL_WARN, "pid of compositor not know yet; skip core dump");
            goto SKIP;
        }
    }

    /* We do not want to kill compositor if debugger is attached to it.
     * Since there can be only one attacher at one time, we can use dummy
     * attach + detach cycle to determine debugger presence. */
    if( ptrace(PTRACE_ATTACH, pid, 0, 0) == -1 ) {
        mce_log(LL_WARN, "could not attach to compositor: %m");
        mce_log(LL_WARN, "assuming debugger is attached; skip killing");
        goto EXIT;
    }

    if( ptrace(PTRACE_DETACH, pid, 0,0) == -1 ) {
        mce_log(LL_WARN, "could not detach from compositor: %m");
    }

    /* We need to send some signal that a) leads to core dump b) is not
     * handled "nicely" by compositor. SIGXCPU fits that description and
     * is also c) somewhat relevant "CPU time limit exceeded" d) easily
     * distinguishable from other "normal" crash reports. */

    if( kill(pid, SIGXCPU) == -1 ) {
        mce_log(LL_ERR, "failed to SIGXCPU compositor: %m");
        goto EXIT;
    }

    /* Just in case compositor process was stopped, make it continue - and
     * hopefully dump a core. */

    if( kill(pid, SIGCONT) == -1 )
        mce_log(LL_ERR, "failed to SIGCONT compositor: %m");

SKIP:

    /* Allow some time for core dump to take place, then just kill it */
    mdy_compositor_kill_id = g_timeout_add(1000 * mdy_compositor_kill_delay,
                                           mdy_compositor_kill_kill_cb,
                                           GINT_TO_POINTER(pid));
EXIT:

    /* Start led pattern active if kill timer was scheduled */
    mdy_compositor_set_killer_led(mdy_compositor_kill_id != 0);

    return FALSE;
}

/** Shedule compositor core dump + kill
 *
 * This should be called when initiating asynchronous setUpdatesEnabled()
 * D-Bus method call.
 */
static void mdy_compositor_schedule_killer(void)
{
    /* The compositor killing is not used unless we have "devel" flavor
     * mce, or normal mce running in verbose mode */
    if( !mce_log_p(LL_DEVEL) )
        goto EXIT;

    /* Setting the core dump delay to zero disables killing too. */
    if( mdy_compositor_core_delay <= 0 )
        goto EXIT;

    /* Note: Initially we might not yet know the compositor PID. But once
     *       it gets known, the kill timer chain will lock in to it.
     *       If compositor name owner changes, the timer chain is cancelled
     *       and pid reset again. This should make sure we can do the
     *       killing even if the async pid query does not finish before
     *       we need to make the 1st setUpdatesEnabled() ipc and we do not
     *       kill freshly restarted compositor because the previous instance
     *       got stuck. */

    if( !mdy_compositor_kill_id ) {
        mce_log(LL_DEBUG, "scheduled compositor killing");
        mdy_compositor_kill_id =
            g_timeout_add(1000 * mdy_compositor_core_delay,
                          mdy_compositor_kill_core_cb,
                          GINT_TO_POINTER(mdy_compositor_pid));
    }

EXIT:
    return;
}

/** Cancel any pending compositor killing timers
 *
 * This should be called when non-error reply is received for
 * setUpdatesEnabled() D-Bus method call.
 */
static void mdy_compositor_cancel_killer(void)
{
    if( mdy_compositor_kill_id ) {
        g_source_remove(mdy_compositor_kill_id),
            mdy_compositor_kill_id = 0;
        mce_log(LL_DEBUG, "cancelled compositor killing");
    }

    /* In any case stop the led pattern */
    mdy_compositor_set_killer_led(false);
}

static void mdy_compositor_name_owner_pid_cb(const char *name, int pid)
{
    if( mdy_str_eq_p(mdy_compositor_priv_name, name) )
        mdy_compositor_pid = pid;
}

static bool mdy_compositor_is_available(void)
{
    return mdy_compositor_priv_name != 0;
}

static void mdy_compositor_name_owner_set(const char *curr)
{
    bool has_owner = (curr && *curr);

    mce_log(LL_DEVEL, "compositor is %s on system bus",
            has_owner ? curr : "N/A");

    /* first clear existing data, timers, etc */
    g_free(mdy_compositor_priv_name), mdy_compositor_priv_name = 0;
    mdy_compositor_pid = -1;
    mdy_compositor_cancel_killer();

    /* then cache dbus name and start pid query */
    if( has_owner ) {
        mdy_compositor_priv_name = g_strdup(curr);
        mce_dbus_get_pid_async(curr, mdy_compositor_name_owner_pid_cb);
    }
}

/** Handle replies to org.nemomobile.compositor.setUpdatesEnabled() calls
 *
 * @param pending   asynchronous dbus call handle
 * @param user_data enable/disable state as void pointer
 */
static void mdy_compositor_state_req_cb(DBusPendingCall *pending,
                                      void *user_data)
{
    DBusMessage *rsp = 0;
    DBusError    err = DBUS_ERROR_INIT;

    /* The user_data pointer is used for storing the renderer
     * state associated with the async method call sent to
     * compositor. The reply message is just an acknowledgement
     * from ui that it got the value and thus has no content. */
    renderer_state_t state = GPOINTER_TO_INT(user_data);

    mce_log(LL_NOTICE, "%s(%s) - method reply",
            COMPOSITOR_SET_UPDATES_ENABLED,
            state ? "ENABLE" : "DISABLE");

    if( mdy_compositor_state_req_pc != pending )
        goto cleanup;

    mdy_compositor_cancel_panic_led();

    mdy_compositor_state_req_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pending)) )
        goto cleanup;

    if( dbus_set_error_from_message(&err, rsp) ) {
        /* Mark down that the request failed; we can't
         * enter suspend without UI side being in the
         * loop or we'll risk spectacular crashes */
        mce_log(LL_WARN, "%s: %s", err.name, err.message);
        mdy_compositor_ui_state = RENDERER_ERROR;
    }
    else {
        mdy_compositor_ui_state = state;
        mdy_compositor_cancel_killer();
    }

    mce_log(LL_NOTICE, "RENDERER state=%d", mdy_compositor_ui_state);

    mdy_stm_schedule_rethink();

cleanup:
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

/** Cancel pending org.nemomobile.compositor.setUpdatesEnabled() call
 *
 * This is just bookkeeping for mce side, the method call message
 * has already been sent - we just are no longer interested in
 * seeing the return message.
 */
static void mdy_compositor_cancel_state_req(void)
{
    mdy_compositor_cancel_panic_led();

    if( !mdy_compositor_state_req_pc )
        return;

    mce_log(LL_NOTICE, "RENDERER STATE REQUEST CANCELLED");

    dbus_pending_call_cancel(mdy_compositor_state_req_pc),
        mdy_compositor_state_req_pc = 0;
}

/** Enable/Disable ui updates via dbus ipc with compositor
 *
 * Used at transitions to/from display=off
 *
 * Gives time for compositor to finish rendering activities
 * before putting frame buffer to sleep via early/late suspend,
 * and telling when rendering is allowed again.
 *
 * @param enabled TRUE for enabling updates, FALSE for disbling updates
 *
 * @return TRUE if asynchronous method call was succesfully sent,
 *         FALSE otherwise
 */
static gboolean mdy_compositor_start_state_req(renderer_state_t state)
{
    gboolean         res = FALSE;
    DBusConnection  *bus = 0;
    DBusMessage     *req = 0;
    dbus_bool_t      dta = (state == RENDERER_ENABLED);

    mdy_compositor_cancel_state_req();

    mce_log(LL_NOTICE, "%s(%s) - method call",
            COMPOSITOR_SET_UPDATES_ENABLED,
            state ? "ENABLE" : "DISABLE");

    /* Mark the state at compositor side as unknown until we get
     * either ack or error reply */
    mdy_compositor_ui_state = RENDERER_UNKNOWN;

    if( !(bus = dbus_connection_get()) )
        goto EXIT;

    req = dbus_message_new_method_call(COMPOSITOR_SERVICE,
                                       COMPOSITOR_PATH,
                                       COMPOSITOR_IFACE,
                                       COMPOSITOR_SET_UPDATES_ENABLED);
    if( !req )
        goto EXIT;

    if( !dbus_message_append_args(req,
                                  DBUS_TYPE_BOOLEAN, &dta,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    if( !dbus_connection_send_with_reply(bus, req,
                                         &mdy_compositor_state_req_pc,
                                         mdy_compositor_ipc_timeout) )
        goto EXIT;

    if( !mdy_compositor_state_req_pc )
        goto EXIT;

    if( !dbus_pending_call_set_notify(mdy_compositor_state_req_pc,
                                      mdy_compositor_state_req_cb,
                                      GINT_TO_POINTER(state), 0) )
        goto EXIT;

    /* Success */
    res = TRUE;

    /* If we do not get reply in a short while, start led pattern */
    mdy_compositor_schedule_panic_led(state);

    /* And after waiting a bit longer, assume that compositor is
     * process stuck and kill it */
    mdy_compositor_schedule_killer();

EXIT:
    if( mdy_compositor_state_req_pc  ) {
        dbus_pending_call_unref(mdy_compositor_state_req_pc);
        // Note: The pending call notification holds the final
        //       reference. It gets released at cancellation
        //       or return from notification callback
    }
    if( req ) dbus_message_unref(req);
    if( bus ) dbus_connection_unref(bus);

    return res;
}

/* ========================================================================= *
 * CALLSTATE_CHANGES
 * ========================================================================= */

/** Timer id for ending call state was recently changed condition */
static guint mdy_callstate_end_changed_id = 0;

/** Timer callback for ending call state was recently changed condition */
static gboolean mdy_callstate_end_changed_cb(gpointer aptr)
{
    (void)aptr;

    if( !mdy_callstate_end_changed_id )
        goto EXIT;

    mdy_callstate_end_changed_id = 0;

    mce_log(LL_DEBUG, "suspend blocking/call state change: ended");

    // autosuspend policy
    mdy_stm_schedule_rethink();

EXIT:
    return FALSE;
}

/** Predicate function for call state was recently changed
 */
static bool mdy_callstate_changed_recently(void)
{
    return mdy_callstate_end_changed_id != 0;
}

/** Cancel call state was recently changed condition
 */
static void mdy_callstate_clear_changed(void)
{
    if( mdy_callstate_end_changed_id ) {
        mce_log(LL_DEBUG, "suspend blocking/call state change: canceled");

        g_source_remove(mdy_callstate_end_changed_id),
            mdy_callstate_end_changed_id = 0;

        // autosuspend policy
        mdy_stm_schedule_rethink();
    }
}

/** Start call state was recently changed condition
 *
 * How long the condition is kept depends on the call_state.
 */
static void mdy_callstate_set_changed(void)
{
    int delay = CALLSTATE_CHANGE_BLOCK_SUSPEND_DEFAULT_MS;

    if( call_state == CALL_STATE_ACTIVE )
        delay = CALLSTATE_CHANGE_BLOCK_SUSPEND_ACTIVE_MS;

    if( mdy_callstate_end_changed_id )
        g_source_remove(mdy_callstate_end_changed_id);
    else
        mce_log(LL_DEBUG, "suspend blocking/call state change: started");

    mdy_callstate_end_changed_id =
        g_timeout_add(delay, mdy_callstate_end_changed_cb, 0);

    // autosuspend policy
    mdy_stm_schedule_rethink();
}

/* ========================================================================= *
 * AUTOSUSPEND_POLICY
 * ========================================================================= */

#ifdef ENABLE_WAKELOCKS
/** Automatic suspend policy modes */
enum
{
    /** Always stay in on-mode */
    SUSPEND_POLICY_DISABLED    = 0,

    /** Normal transitions between on, early suspend, and late suspend */
    SUSPEND_POLICY_ENABLED     = 1,

    /** Allow on and early suspend, but never enter late suspend */
    SUSPEND_POLICY_EARLY_ONLY  = 2,

    /** Default mode to use if no configuration exists */
    SUSPEND_POLICY_DEFAULT     = SUSPEND_POLICY_ENABLED,
};

enum
{
    SUSPEND_LEVEL_ON,          // suspend not allowed
    SUSPEND_LEVEL_EARLY,       // early suspend allowed
    SUSPEND_LEVEL_LATE,        // early and late suspend allowed
};

/** Automatic suspend policy */
static gint mdy_suspend_policy = SUSPEND_POLICY_DEFAULT;

/** GConf callback ID for automatic suspend policy changes */
static guint mdy_suspend_policy_id = 0;

/** Check if suspend policy allows suspending
 *
 * @return 0 - suspending not allowed
 *         1 - early suspend allowed
 *         2 - late suspend allowed
 */
static int mdy_autosuspend_get_allowed_level(void)
{
    bool block_late  = false;
    bool block_early = false;

    /* no late suspend when incoming / active call */
    switch( call_state ) {
    case CALL_STATE_RINGING:
        block_late = true;
        break;
    default:
    case CALL_STATE_ACTIVE:
        break;
    }

    /* no late suspend immediately after call_state change */
    if( mdy_callstate_changed_recently() )
        block_late = true;

    /* no late suspend when alarm on screen */
    switch( alarm_ui_state ) {
    case MCE_ALARM_UI_RINGING_INT32:
    case MCE_ALARM_UI_VISIBLE_INT32:
        block_late = true;
        break;
    default:
        break;
    }

    /* Exceptional situations without separate state
     * management block late suspend */
    if( exception_state & (UIEXC_NOTIF|UIEXC_LINGER) )
        block_late = true;

    /* no late suspend in ACTDEAD etc */
    if( system_state != MCE_STATE_USER )
        block_late = true;

    /* no late suspend during bootup */
    if( mdy_desktop_ready_id || !mdy_init_done )
        block_late = true;

    /* no late suspend during shutdown */
    if( mdy_shutdown_in_progress() )
        block_late = true;

    /* no late suspend while PackageKit is in Locked state */
    if( packagekit_locked )
        block_late = true;

    /* no more suspend at module unload */
    if( mdy_unloading_module )
        block_early = true;

    /* no suspend during update mode */
    if( mdy_update_mode )
        block_early = true;

    /* do not suspend while ui side might still be drawing */
    if( mdy_compositor_ui_state != RENDERER_DISABLED )
        block_early = true;

    /* adjust based on gconf setting */
    switch( mdy_suspend_policy ) {
    case SUSPEND_POLICY_DISABLED:
        block_early = true;
        break;

    case SUSPEND_POLICY_EARLY_ONLY:
        block_late = true;
        break;

    default:
    case SUSPEND_POLICY_ENABLED:
        break;
    }

    if( block_early )
        return SUSPEND_LEVEL_ON;

    if( block_late )
        return SUSPEND_LEVEL_EARLY;

    return SUSPEND_LEVEL_LATE;
}

/** Callback for handling changes to autosuspend policy configuration
 *
 * @param client (not used)
 * @param id     (not used)
 * @param entry  GConf entry that changed
 * @param data   (not used)
 */
static void mdy_autosuspend_gconf_cb(GConfClient *const client, const guint id,
                                     GConfEntry *const entry, gpointer const data)
{
    (void)client; (void)id; (void)data;

    gint policy = SUSPEND_POLICY_ENABLED;
    const GConfValue *value = 0;

    if( entry && (value = gconf_entry_get_value(entry)) ) {
        if( value->type == GCONF_VALUE_INT )
            policy = gconf_value_get_int(value);
    }
    if( mdy_suspend_policy != policy ) {
        mce_log(LL_NOTICE, "suspend policy change: %d -> %d",
                mdy_suspend_policy, policy);
        mdy_suspend_policy = policy;
        mdy_stm_schedule_rethink();
    }
}
#endif /* ENABLE_WAKELOCKS */

/* ========================================================================= *
 * ORIENTATION_ACTIVITY
 * ========================================================================= */

/** Callback for handling orientation change notifications
 *
 * @param state orientation sensor state
 */
static void mdy_orientation_changed_cb(int state)
{
    execute_datapipe(&orientation_sensor_pipe,
                     GINT_TO_POINTER(state),
                     USE_INDATA, CACHE_INDATA);
}

/** Generate user activity from orientation sensor input
 */
static void mdy_orientation_generate_activity(void)
{
    /* The feature must be enabled */
    if( !mdy_orientation_change_is_activity )
        goto EXIT;

    /* Display must be on/dimmed */
    switch( display_state ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        break;

    default:
        goto EXIT;
    }

    mce_log(LL_DEBUG, "orientation change; generate activity");
    execute_datapipe(&device_inactive_pipe,
                     GINT_TO_POINTER(FALSE),
                     USE_INDATA, CACHE_INDATA);

EXIT:
    return;
}

/** Check if orientation sensor should be enabled
 */
static bool mdy_orientation_sensor_wanted(void)
{
    bool wanted = false;

    /* Skip if master toggle is disabled */
    if( !mdy_orientation_sensor_enabled )
        goto EXIT;

    /* Enable orientation sensor in ON|DIM */

    /* Start the orientation sensor already when powering up
     * to ON|DIM|LPM_ON states -> we should have a valid sensor
     * state before the display transition finishes.
     */
    switch( display_state_next ) {
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_UP:
        break;

    default:
        goto EXIT;
    }

    /* But only if orientation sensor features are enabled */
    wanted  = (mdy_flipover_gesture_enabled ||
               mdy_orientation_change_is_activity);

EXIT:
    return wanted;
}

/** Start/stop orientation sensor based on display state
 */
static void mdy_orientation_sensor_rethink(void)
{
    static bool was_enabled = false;

    bool enable = mdy_orientation_sensor_wanted();

    if( was_enabled == enable )
        goto EXIT;

    was_enabled = enable;

    mce_log(LL_DEBUG, "%s orientation sensor", enable ? "enable" : "disable");

    if( enable ) {
        /* Add notification before enabling to get initial guess */
        mce_sensorfw_orient_set_notify(mdy_orientation_changed_cb);
        mce_sensorfw_orient_enable();
    }
    else {
        /* Remove notification after disabling to get final state */
        mce_sensorfw_orient_disable();
        mce_sensorfw_orient_set_notify(0);
    }

EXIT:
    return;

}

/* ========================================================================= *
 * DISPLAY_STATE
 * ========================================================================= */

/* React to new display state (via display state datapipe)
 */
static void mdy_display_state_changed(void)
{
    /* Disable blanking pause if display != ON */
    switch( display_state ) {
    case MCE_DISPLAY_ON:
        break;

    case MCE_DISPLAY_DIM:
        if( mdy_blanking_is_paused() && mdy_blanking_pause_can_dim() )
            break;
        // fall through

    default:
        mdy_blanking_remove_pause_clients();
        break;
    }

    /* Program dim/blank timers */
    mdy_blanking_rethink_timers(false);

    /* Enable/disable high brightness mode */
    mdy_hbm_rethink();

    /* Restart brightness fading in case automatic brightness tuning has
     * changed the target levels during the display state transition.
     * Should turn in to big nop if there are no changes.
     */

    switch( display_state ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
        /* Blanking or already blanked -> set zero brightness */
        mdy_brightness_force_level(0);
        break;

    case MCE_DISPLAY_LPM_ON:
        mdy_brightness_set_fade_target_default(mdy_brightness_level_display_lpm);
        break;

    case MCE_DISPLAY_DIM:
        mdy_brightness_set_fade_target_dimming(mdy_brightness_level_display_dim);
        break;

    case MCE_DISPLAY_ON:
        mdy_brightness_set_fade_target_default(mdy_brightness_level_display_on);
        break;

    case MCE_DISPLAY_UNDEF:
        break;

    default:
    case MCE_DISPLAY_POWER_DOWN:
    case MCE_DISPLAY_POWER_UP:
        // these should never show up here
        mce_abort();
        break;
    }

    /* This will send the correct state
     * since the pipe contains the new value
     */
    mdy_dbus_send_display_status(0);
}

/** Handle end of display state transition
 *
 * After the state machine has finished display state
 * tranistion, it gets broadcast to display_state_pipe
 * via this function.
 *
 * Actions for this will be executed in display_state_pipe
 * output trigger mdy_display_state_changed().
 *
 * @param prev_state    previous display state
 * @param display_state state transferred to
 */
static void mdy_display_state_enter(display_state_t prev_state,
                                    display_state_t next_state)
{
    mce_log(LL_INFO, "END %s -> %s transition",
            display_state_repr(prev_state),
            display_state_repr(next_state));

    /* Allow ALS brightness tuning after entering powered on state */
    if( mdy_stm_display_state_needs_power(next_state) ) {
        mce_log(LL_DEBUG, "allow als fade");
        mdy_brightness_als_fade_allowed = true;
    }

    /* Restore display_state_pipe to valid value */
    display_state_pipe.cached_data = GINT_TO_POINTER(next_state);

    /* Run display state change triggers */
    mce_log(LL_DEVEL, "current display state = %s",
            display_state_repr(next_state));
    execute_datapipe(&display_state_pipe,
                     GINT_TO_POINTER(next_state),
                     USE_INDATA, CACHE_INDATA);

    /* Deal with new stable display state */
    mdy_display_state_changed();
}

/** Handle start of display state transition
 *
 * @param prev_state    display state before transition
 * @param display_state target state to transfer to
 */
static void mdy_display_state_leave(display_state_t prev_state,
                                    display_state_t next_state)
{
    mce_log(LL_INFO, "BEG %s -> %s transition",
            display_state_repr(prev_state),
            display_state_repr(next_state));

    /* Cancel display state specific timers that we do not want to
     * trigger while waiting for frame buffer suspend/resume. */
    mdy_blanking_cancel_timers();

    bool have_power = mdy_stm_display_state_needs_power(prev_state);
    bool need_power = mdy_stm_display_state_needs_power(next_state);

    /* Deny ALS brightness when heading to powered off state */
    if( !need_power ) {
        mce_log(LL_DEBUG, "deny als fade");
        mdy_brightness_als_fade_allowed = false;
    }

    /* Update display brightness that should be used the next time
     * the display is powered up. Start fader already here if the
     * display is already powered up (otherwise will be started
     * after fb power up at STM_WAIT_RESUME / STM_LEAVE_LOGICAL_OFF).
     */
    switch( next_state ) {
    case MCE_DISPLAY_ON:
        mdy_brightness_level_display_resume = mdy_brightness_level_display_on;
        if( have_power )
            mdy_brightness_set_fade_target_default(mdy_brightness_level_display_resume);
        break;

    case MCE_DISPLAY_DIM:
        mdy_brightness_level_display_resume = mdy_brightness_level_display_dim;
        if( have_power )
            mdy_brightness_set_fade_target_dimming(mdy_brightness_level_display_resume);
        break;

    case MCE_DISPLAY_LPM_ON:
        mdy_brightness_level_display_resume = mdy_brightness_level_display_lpm;
        if( have_power )
            mdy_brightness_set_fade_target_default(mdy_brightness_level_display_resume);
        break;

    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
        mdy_brightness_level_display_resume = 0;
        mdy_brightness_set_fade_target_blank();
        break;

    case MCE_DISPLAY_UNDEF:
        break;

    default:
    case MCE_DISPLAY_POWER_DOWN:
    case MCE_DISPLAY_POWER_UP:
        // these should never show up here
        mce_abort();
        break;
    }

    /* Broadcast the final target of this transition; note that this
     * happens while display_state_pipe still holds the previous
     * (non-transitional) state */
    mce_log(LL_NOTICE, "target display state = %s",
            display_state_repr(next_state));
    execute_datapipe(&display_state_next_pipe,
                     GINT_TO_POINTER(next_state),
                     USE_INDATA, CACHE_INDATA);

    /* Invalidate display_state_pipe when making transitions
     * that need to wait for external parties */
    if( have_power != need_power ) {
        display_state_t state =
            need_power ? MCE_DISPLAY_POWER_UP : MCE_DISPLAY_POWER_DOWN;

        mce_log(LL_DEVEL, "current display state = %s",
                display_state_repr(state));
        display_state_pipe.cached_data = GINT_TO_POINTER(state);
        execute_datapipe(&display_state_pipe,
                         display_state_pipe.cached_data,
                         USE_INDATA, CACHE_INDATA);
    }
}

/* ========================================================================= *
 * FRAMEBUFFER_SUSPEND_RESUME
 * ========================================================================= */

/** Framebuffer suspend/resume failure led patterns
 */
static void mdy_fbsusp_led_set(mdy_fbsusp_led_state_t req)
{
    bool blanking = false;
    bool unblanking = false;

    switch( req ) {
    case FBDEV_LED_SUSPENDING:
        blanking = true;
        mce_log(LL_DEVEL, "start alert led pattern for: failed fb suspend");
        break;
    case FBDEV_LED_RESUMING:
        unblanking = true;
        mce_log(LL_DEVEL, "start alert led pattern for: failed fb resume");
        break;
    default:
        break;
    }

    execute_datapipe_output_triggers(blanking ?
                                     &led_pattern_activate_pipe :
                                     &led_pattern_deactivate_pipe,
                                     "PatternDisplaySuspendFailed",
                                     USE_INDATA);

    execute_datapipe_output_triggers(unblanking ?
                                     &led_pattern_activate_pipe :
                                     &led_pattern_deactivate_pipe,
                                     "PatternDisplayResumeFailed",
                                     USE_INDATA);
}

/** Timer id for fbdev suspend/resume is taking too long */
static guint mdy_fbsusp_led_timer_id = 0;

/** Timer callback for fbdev suspend/resume is taking too long
 */
static gboolean mdy_fbsusp_led_timer_cb(gpointer aptr)
{
    mdy_fbsusp_led_state_t req = GPOINTER_TO_INT(aptr);

    if( !mdy_fbsusp_led_timer_id )
        goto EXIT;

    mce_log(LL_DEBUG, "fbdev led timer triggered");

    mdy_fbsusp_led_timer_id = 0;
    mdy_fbsusp_led_set(req);

EXIT:
    return FALSE;
}

/* Cancel fbdev suspend/resume is taking too long timer
 */
static void mdy_fbsusp_led_cancel_timer(void)
{
    mdy_fbsusp_led_set(FBDEV_LED_OFF);

    if( mdy_fbsusp_led_timer_id != 0 ) {
        mce_log(LL_DEBUG, "fbdev led timer cancelled");
        g_source_remove(mdy_fbsusp_led_timer_id),
            mdy_fbsusp_led_timer_id = 0;
    }
}

/* Schedule fbdev suspend/resume is taking too long timer
 */
static void mdy_fbsusp_led_start_timer(mdy_fbsusp_led_state_t req)
{
    mdy_fbsusp_led_set(FBDEV_LED_OFF);

    int delay = LED_DELAY_FB_SUSPEND_RESUME;

    if( mdy_fbsusp_led_timer_id != 0 )
        g_source_remove(mdy_fbsusp_led_timer_id);

    mdy_fbsusp_led_timer_id = g_timeout_add(delay,
                                           mdy_fbsusp_led_timer_cb,
                                           GINT_TO_POINTER(req));

    mce_log(LL_DEBUG, "fbdev led timer sheduled @ %d ms", delay);
}

/* ========================================================================= *
 * DISPLAY_STATE_MACHINE
 * ========================================================================= */

/** A setUpdatesEnabled(true) call needs to be made when possible */
static bool mdy_stm_enable_rendering_needed = true;

/** Display state we are currently in */
static display_state_t mdy_stm_curr = MCE_DISPLAY_UNDEF;

/** Display state we are currently changing to */
static display_state_t mdy_stm_next = MCE_DISPLAY_UNDEF;

/** Display state that has been reqyested */
static display_state_t mdy_stm_want = MCE_DISPLAY_UNDEF;

/** Display state machine state */
static stm_state_t mdy_stm_dstate = STM_UNSET;

/** Display state / suspend policy wakelock held */
static bool mdy_stm_acquire_wakelockd = false;

/** Display state machine state to human readable string
 */
static const char *mdy_stm_state_name(stm_state_t state)
{
    const char *name = "UNKNOWN";

#define DO(tag) case STM_##tag: name = #tag; break
    switch( state ) {
        DO(UNSET);
        DO(RENDERER_INIT_START);
        DO(RENDERER_WAIT_START);
        DO(ENTER_POWER_ON);
        DO(STAY_POWER_ON);
        DO(LEAVE_POWER_ON);
        DO(RENDERER_INIT_STOP);
        DO(RENDERER_WAIT_STOP);
        DO(WAIT_FADE_TO_BLACK);
        DO(WAIT_FADE_TO_TARGET);
        DO(INIT_SUSPEND);
        DO(WAIT_SUSPEND);
        DO(ENTER_POWER_OFF);
        DO(STAY_POWER_OFF);
        DO(LEAVE_POWER_OFF);
        DO(INIT_RESUME);
        DO(WAIT_RESUME);
        DO(ENTER_LOGICAL_OFF);
        DO(STAY_LOGICAL_OFF);
        DO(LEAVE_LOGICAL_OFF);
    default: break;
    }
#undef DO
    return name;
}

/** react to compositor availability changes
 */
static void mdy_datapipe_compositor_available_cb(gconstpointer aptr)
{
    static service_state_t service = SERVICE_STATE_UNDEF;

    service_state_t prev = service;
    service = GPOINTER_TO_INT(aptr);

    if( service == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "compositor_available = %s -> %s",
            service_state_repr(prev),
            service_state_repr(service));

    /* The private name of the owner is needed when/if
     * the compositor gets unresponsive and needs to
     * be killed and restarted */

    const char *curr = 0;

    if( service == SERVICE_STATE_RUNNING )
        curr = mce_dbus_nameowner_get(COMPOSITOR_SERVICE);

    mdy_compositor_name_owner_set(curr);

    /* If compositor drops from systembus in USER/ACTDEAD mode while
     * we are not shutting down, assume we are dealing with lipstick
     * crash / act-dead-charging stop and power cycling the frame
     buffer is needed to clear zombie ui off the screen. */
    mdy_fbdev_rethink();

    /* set setUpdatesEnabled(true) needs to be called flag */
    mdy_stm_enable_rendering_needed = true;

    /* a) Lipstick assumes that updates are allowed when
     *    it starts up. Try to arrange that it is so.
     *
     * b) Without lipstick in place we must not suspend
     *    because there is nobody to communicate the
     *    updating is allowed
     *
     * Turning the display on at lipstick runstate change
     * deals with both (a) and (b)
     *
     * Exception: When mce restarts while lipstick is
     *            running, we need to keep the existing
     *            display state.
     */
    if( prev != SERVICE_STATE_UNDEF )
        mdy_stm_push_target_change(MCE_DISPLAY_ON);

EXIT:
    return;
}

/** react to systemui availability changes
 */
static void mdy_datapipe_lipstick_available_cb(gconstpointer aptr)
{
    service_state_t prev = lipstick_service_state;
    lipstick_service_state = GPOINTER_TO_INT(aptr);

    if( lipstick_service_state == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "lipstick_available = %s -> %s",
            service_state_repr(prev),
            service_state_repr(lipstick_service_state));

    mdy_blanking_rethink_afterboot_delay();

EXIT:
    return;
}

/** Predicate for choosing between STM_STAY_POWER_ON|OFF
 */
static bool mdy_stm_display_state_needs_power(display_state_t state)
{
    bool power_on = true;

    switch( state ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_LPM_ON:
        break;

    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_UNDEF:
        power_on = false;
        break;

    default:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
        mce_abort();
    }

    return power_on;
}

/** Predicate for: policy allows early suspend
 */
static bool mdy_stm_is_early_suspend_allowed(void)
{
#ifdef ENABLE_WAKELOCKS
    bool res = (mdy_autosuspend_get_allowed_level() >= SUSPEND_LEVEL_EARLY);
    mce_log(LL_INFO, "res=%s", res ? "true" : "false");
    return res;
#else
    // "early suspend" in state machine transforms in to
    // fb power control via ioctl without ENABLE_WAKELOCKS
    return true;
#endif
}

/** Predicate for: policy allows late suspend
 */
static bool mdy_stm_is_late_suspend_allowed(void)
{
#ifdef ENABLE_WAKELOCKS
    bool res = (mdy_autosuspend_get_allowed_level() >= SUSPEND_LEVEL_LATE);
    mce_log(LL_INFO, "res=%s", res ? "true" : "false");
    return res;
#else
    return false;
#endif
}

/** Start frame buffer suspend
 */
static void mdy_stm_start_fb_suspend(void)
{
    mdy_fbsusp_led_start_timer(FBDEV_LED_SUSPENDING);

#ifdef ENABLE_WAKELOCKS
    mce_log(LL_NOTICE, "suspending");
    if( mdy_waitfb_data.thread )
        wakelock_allow_suspend();
    else
        mdy_waitfb_data.suspended = true, mce_fbdev_set_power(false);
#else
    mce_log(LL_NOTICE, "power off frame buffer");
    mdy_waitfb_data.suspended = true, mce_fbdev_set_power(false);
#endif
}

/** Start frame buffer resume
 */
static void mdy_stm_start_fb_resume(void)
{
    mdy_fbsusp_led_start_timer(FBDEV_LED_RESUMING);

#ifdef ENABLE_WAKELOCKS
    mce_log(LL_NOTICE, "resuming");
    if( mdy_waitfb_data.thread )
        wakelock_block_suspend();
    else
        mdy_waitfb_data.suspended = false, mce_fbdev_set_power(true);
#else
    mce_log(LL_NOTICE, "power off frame buffer");
    mdy_waitfb_data.suspended = false, mce_fbdev_set_power(true);
#endif
}

/** Predicate for: frame buffer is powered off
 */
static bool mdy_stm_is_fb_suspend_finished(void)
{
    bool res = mdy_waitfb_data.suspended;

    if( res )
        mdy_fbsusp_led_cancel_timer();

    mce_log(LL_INFO, "res=%s", res ? "true" : "false");
    return res;
}

/** Predicate for: frame buffer is powered on
 */
static bool mdy_stm_is_fb_resume_finished(void)
{
    bool res = !mdy_waitfb_data.suspended;

    if( res )
        mdy_fbsusp_led_cancel_timer();

    mce_log(LL_INFO, "res=%s", res ? "true" : "false");
    return res;
}

/** Release display wakelock to allow late suspend
 */
static void mdy_stm_release_wakelock(void)
{
    if( mdy_stm_acquire_wakelockd ) {
        mdy_stm_acquire_wakelockd = false;
#ifdef ENABLE_WAKELOCKS
        mce_log(LL_INFO, "wakelock released");
        //wakelock_unlock("mce_display_on");
        wakelock_lock("mce_display_on",
                      1000 * 1000 * 1000);
#endif
    }
}

/** Acquire display wakelock to block late suspend
 */
static void mdy_stm_acquire_wakelock(void)
{
    if( !mdy_stm_acquire_wakelockd ) {
        mdy_stm_acquire_wakelockd = true;
#ifdef ENABLE_WAKELOCKS
        wakelock_lock("mce_display_on", -1);
        mce_log(LL_INFO, "wakelock acquired");
#endif
    }
}

/** Helper for making state transitions
 */
static void mdy_stm_trans(stm_state_t state)
{
    if( mdy_stm_dstate != state ) {
        mce_log(LL_INFO, "STM: %s -> %s",
                mdy_stm_state_name(mdy_stm_dstate),
                mdy_stm_state_name(state));
        mdy_stm_dstate = state;
    }
}

/** Push new change from pipeline to state machine
 */
static void mdy_stm_push_target_change(display_state_t next_state)
{
    if( mdy_stm_want != next_state ) {
        mdy_stm_want = next_state;
        /* Try to initiate state transitions immediately
         * to make the in-transition transient states
         * visible to code that polls the display state
         * instead of using output triggers */
        mdy_stm_force_rethink();
    }
}

/** Pull new change from within the state machine
 */
static bool mdy_stm_pull_target_change(void)
{
    // already in transition?
    if( mdy_stm_curr != mdy_stm_next )
        return true;

    // new transition requested?
    if( mdy_stm_want == MCE_DISPLAY_UNDEF )
        return false;

    mdy_stm_next = mdy_stm_want, mdy_stm_want = MCE_DISPLAY_UNDEF;

    // transition to new state requested?
    if( mdy_stm_curr == mdy_stm_next ) {
        /* No-change requests will be ignored, but we still
         * need to check if forced display state indication
         * signal needs to be sent. */
        mdy_dbus_send_display_status(0);
        return false;
    }

    // do pre-transition actions
    mdy_display_state_leave(mdy_stm_curr, mdy_stm_next);
    return true;
}

/** Finish current change from within the state machine
 */
static void mdy_stm_finish_target_change(void)
{
    // do post-transition actions
    display_state_t prev = mdy_stm_curr;
    mdy_stm_curr = mdy_stm_next;
    mdy_display_state_enter(prev, mdy_stm_curr);
}

/** Predicate for setUpdatesEnabled() ipc not finished yet
 */
static bool mdy_stm_is_renderer_pending(void)
{
    return mdy_compositor_ui_state == RENDERER_UNKNOWN;
}

/** Predicate for setUpdatesEnabled(false) ipc finished
 */
static bool mdy_stm_is_renderer_disabled(void)
{
    return mdy_compositor_ui_state == RENDERER_DISABLED;
}

/** Predicate for setUpdatesEnabled(true) ipc finished
 */
static bool mdy_stm_is_renderer_enabled(void)
{
    return mdy_compositor_ui_state == RENDERER_ENABLED;
}

/* Start setUpdatesEnabled(false) ipc with systemui
 */
static void mdy_stm_disable_renderer(void)
{
    if( mdy_compositor_ui_state != RENDERER_DISABLED ||
        mdy_stm_enable_rendering_needed ) {
        mce_log(LL_NOTICE, "stopping renderer");
        mdy_compositor_start_state_req(RENDERER_DISABLED);

        /* clear setUpdatesEnabled(true) needs to be called flag */
        mdy_stm_enable_rendering_needed = false;
    }
    else {
        mce_log(LL_NOTICE, "renderer already disabled");
    }
}

/* Start setUpdatesEnabled(true) ipc with systemui
 */
static void mdy_stm_enable_renderer(void)
{
    if( mdy_compositor_ui_state != RENDERER_ENABLED ||
        mdy_stm_enable_rendering_needed ) {
        mce_log(LL_NOTICE, "starting renderer");
        mdy_compositor_start_state_req(RENDERER_ENABLED);
        /* clear setUpdatesEnabled(true) needs to be called flag */
        mdy_stm_enable_rendering_needed = false;
    }
    else {
        mce_log(LL_NOTICE, "renderer already enabled");
    }
}

/** Execute one state machine step
 *
 * The state transition flow implemented by this function is
 * described in graphviz dot language in display.dot.
 *
 * Any changes to state transition logic should be made to
 * display.dot too.
 *
 * As an example: generate PNG image by executing
 *   dot -Tpng display.dot -o display.png
 */
static void mdy_stm_step(void)
{
    switch( mdy_stm_dstate ) {
    default:
    case STM_UNSET:
        mdy_stm_acquire_wakelock();
        if( !mdy_stm_pull_target_change() )
            break;

        if( mdy_stm_display_state_needs_power(mdy_stm_next) )
            mdy_stm_trans(STM_RENDERER_INIT_START);
        else
            mdy_stm_trans(STM_RENDERER_INIT_STOP);
        break;

    case STM_RENDERER_INIT_START:
        if( !mdy_compositor_is_available() ) {
            mdy_stm_trans(STM_WAIT_FADE_TO_TARGET);
        }
        else {
            mdy_stm_enable_renderer();
            mdy_stm_trans(STM_RENDERER_WAIT_START);
        }
        break;

    case STM_RENDERER_WAIT_START:
        if( mdy_stm_is_renderer_pending() )
            break;
        if( mdy_stm_is_renderer_enabled() ) {
            mdy_stm_trans(STM_WAIT_FADE_TO_TARGET);
            break;
        }
        /* If compositor is not responsive, we must keep trying
         * until we get a reply - or compositor dies and drops
         * from system bus */
        mce_log(LL_CRIT, "ui start failed, retrying");
        mdy_stm_trans(STM_RENDERER_INIT_START);
        break;

    case STM_WAIT_FADE_TO_TARGET:
        /* If the display is already powered up and normal ui is
         * visible, the transition must not be blocked by ongoing
         * brightness fade. Otherwise the user input processing
         * would get misinterpreted. */
        if( mdy_stm_curr == MCE_DISPLAY_ON ||
            mdy_stm_curr == MCE_DISPLAY_DIM ) {
            mdy_stm_trans(STM_ENTER_POWER_ON);
            break;
        }

        /* When using sw fader, we need to wait until it is finished.
         * Otherwise the avalanche of activity resulting from the
         * display=on signal will starve mce of cpu and the brightness
         * transition gets really jumpy. */
        if( mdy_brightness_fade_is_active() )
            break;
        mdy_stm_trans(STM_ENTER_POWER_ON);
        break;

    case STM_ENTER_POWER_ON:
        mdy_stm_finish_target_change();
        mdy_stm_trans(STM_STAY_POWER_ON);
        break;

    case STM_STAY_POWER_ON:
        if( mdy_stm_enable_rendering_needed && mdy_compositor_is_available() ) {
            mce_log(LL_NOTICE, "handling compositor startup");
            mdy_stm_trans(STM_LEAVE_POWER_ON);
            break;
        }
        if( mdy_stm_pull_target_change() )
            mdy_stm_trans(STM_LEAVE_POWER_ON);
        break;

    case STM_LEAVE_POWER_ON:
        if( mdy_stm_display_state_needs_power(mdy_stm_next) )
            mdy_stm_trans(STM_RENDERER_INIT_START);
        else
            mdy_stm_trans(STM_WAIT_FADE_TO_BLACK);
        break;

    case STM_WAIT_FADE_TO_BLACK:
        if( mdy_brightness_fade_is_active() )
            break;
        mdy_stm_trans(STM_RENDERER_INIT_STOP);
        break;

    case STM_RENDERER_INIT_STOP:
        if( !mdy_compositor_is_available() ) {
            mce_log(LL_WARN, "no compositor; going to logical off");
            mdy_stm_trans(STM_ENTER_LOGICAL_OFF);
        }
        else {
            mdy_stm_disable_renderer();
            mdy_stm_trans(STM_RENDERER_WAIT_STOP);
        }
        break;

    case STM_RENDERER_WAIT_STOP:
        if( mdy_stm_is_renderer_pending() )
            break;
        if( mdy_stm_is_renderer_disabled() ) {
            mdy_stm_trans(STM_INIT_SUSPEND);
            break;
        }
        /* If compositor is not responsive, we must keep trying
         * until we get a reply - or compositor dies and drops
         * from system bus */
        mce_log(LL_CRIT, "ui stop failed, retrying");
        mdy_stm_trans(STM_RENDERER_INIT_STOP);
        break;

    case STM_INIT_SUSPEND:
        if( mdy_stm_is_early_suspend_allowed() ) {
            mdy_stm_start_fb_suspend();
            mdy_stm_trans(STM_WAIT_SUSPEND);
        }
        else {
            mdy_stm_trans(STM_ENTER_LOGICAL_OFF);
        }
        break;

    case STM_WAIT_SUSPEND:
        if( !mdy_stm_is_fb_suspend_finished() )
            break;
        mdy_stm_trans(STM_ENTER_POWER_OFF);
        break;

    case STM_ENTER_POWER_OFF:

        mdy_stm_finish_target_change();
        mdy_stm_trans(STM_STAY_POWER_OFF);
        break;

    case STM_STAY_POWER_OFF:
        if( mdy_stm_pull_target_change() ) {
            mdy_stm_trans(STM_LEAVE_POWER_OFF);
            break;
        }

        if( !mdy_stm_is_early_suspend_allowed() ) {
            mdy_stm_trans(STM_LEAVE_POWER_OFF);
            break;
        }

        /* FIXME: Need separate states for stopping/starting
         *        sensors during suspend/resume */

        if( mdy_stm_is_late_suspend_allowed() ) {
            mce_sensorfw_suspend();
            mdy_stm_release_wakelock();
        }
        else {
            mdy_stm_acquire_wakelock();
            mce_sensorfw_resume();
        }
        break;

    case STM_LEAVE_POWER_OFF:
        mdy_stm_acquire_wakelock();
        mce_sensorfw_resume();
        if( mdy_stm_display_state_needs_power(mdy_stm_next) )
            mdy_stm_trans(STM_INIT_RESUME);
        else if( !mdy_stm_is_early_suspend_allowed() )
            mdy_stm_trans(STM_INIT_RESUME);
        else
            mdy_stm_trans(STM_ENTER_POWER_OFF);
        break;

    case STM_INIT_RESUME:
        mdy_stm_start_fb_resume();
        mdy_stm_trans(STM_WAIT_RESUME);
        break;

    case STM_WAIT_RESUME:
        if( !mdy_stm_is_fb_resume_finished() )
            break;
        if( mdy_stm_display_state_needs_power(mdy_stm_next) ) {
            /* We must have non-zero brightness in place when ui draws
             * for the 1st time or the brightness changes will not happen
             * until ui draws again ... */
            if( mdy_brightness_level_cached <= 0 )
                mdy_brightness_force_level(1);

            mdy_brightness_set_fade_target_unblank(mdy_brightness_level_display_resume);
            mdy_stm_trans(STM_RENDERER_INIT_START);
        }
        else
            mdy_stm_trans(STM_ENTER_LOGICAL_OFF);
        break;

    case STM_ENTER_LOGICAL_OFF:
        mdy_stm_finish_target_change();
        mdy_stm_trans(STM_STAY_LOGICAL_OFF);
        break;

    case STM_STAY_LOGICAL_OFF:
        if( mdy_stm_pull_target_change() ) {
            mdy_stm_trans(STM_LEAVE_LOGICAL_OFF);
            break;
        }

        if( !mdy_compositor_is_available() )
            break;

        if( mdy_stm_enable_rendering_needed ) {
            mdy_stm_trans(STM_LEAVE_LOGICAL_OFF);
            break;
        }

        if( mdy_stm_is_early_suspend_allowed() ) {
            mdy_stm_trans(STM_LEAVE_LOGICAL_OFF);
            break;
        }
        break;

    case STM_LEAVE_LOGICAL_OFF:
        if( mdy_stm_display_state_needs_power(mdy_stm_next) ) {
            mdy_brightness_set_fade_target_unblank(mdy_brightness_level_display_resume);
            mdy_stm_trans(STM_RENDERER_INIT_START);
            break;
        }

        if( mdy_stm_enable_rendering_needed ) {
            mdy_stm_trans(STM_RENDERER_INIT_STOP);
            break;
        }

        mdy_stm_trans(STM_INIT_SUSPEND);
        break;
    }
}

/** Simple re-entrancy counter for mdy_stm_exec() */
static int mdy_stm_in_exec = 0;

/** Execute state machine steps until wait state is hit
 */
static void mdy_stm_exec(void)
{
    if( ++mdy_stm_in_exec != 1 )
        goto EXIT;

    stm_state_t prev;
    mce_log(LL_INFO, "ENTER @ %s", mdy_stm_state_name(mdy_stm_dstate));
    do {
        prev = mdy_stm_dstate;
        mdy_stm_step();
    } while( mdy_stm_dstate != prev );
    mce_log(LL_INFO, "LEAVE @ %s", mdy_stm_state_name(mdy_stm_dstate));

EXIT:
    --mdy_stm_in_exec;
}

/** Timer id for state machine execution */
static guint mdy_stm_rethink_id = 0;

/** Timer callback for state machine execution
 */
static gboolean mdy_stm_rethink_cb(gpointer aptr)
{
    (void)aptr; // not used

    if( mdy_stm_rethink_id ) {
        /* clear pending rethink */
        mdy_stm_rethink_id = 0;

        /* run the state machine */
        mdy_stm_exec();

        /* remove wakelock if not re-scheduled */
#ifdef ENABLE_WAKELOCKS
        if( !mdy_stm_rethink_id )
            wakelock_unlock("mce_display_stm");
#endif
    }
    return FALSE;
}

/** Cancel state machine execution timer
 */
static void mdy_stm_cancel_rethink(void)
{
    if( mdy_stm_rethink_id ) {
        g_source_remove(mdy_stm_rethink_id), mdy_stm_rethink_id = 0;
        mce_log(LL_INFO, "cancelled");

#ifdef ENABLE_WAKELOCKS
        wakelock_unlock("mce_display_stm");
#endif
    }
}

/** Schedule state machine execution timer
 */
static void mdy_stm_schedule_rethink(void)
{
    if( !mdy_stm_rethink_id ) {
#ifdef ENABLE_WAKELOCKS
        wakelock_lock("mce_display_stm", -1);
#endif

        mce_log(LL_INFO, "scheduled");
        mdy_stm_rethink_id = g_idle_add(mdy_stm_rethink_cb, 0);
    }
}

/** Force immediate state machine execution
 */
static void mdy_stm_force_rethink(void)
{
    /* Datapipe actions from within mdy_stm_exec() can
     * cause feedback in the form of additional display
     * state requests. These are expected and ok as long
     * as they do not cause re-entry to mdy_stm_exec(). */
    if( mdy_stm_in_exec )
        goto EXIT;

#ifdef ENABLE_WAKELOCKS
    if( !mdy_stm_rethink_id )
        wakelock_lock("mce_display_stm", -1);
#endif

    if( mdy_stm_rethink_id )
        g_source_remove(mdy_stm_rethink_id), mdy_stm_rethink_id = 0;

    mdy_stm_exec();

#ifdef ENABLE_WAKELOCKS
    if( !mdy_stm_rethink_id )
        wakelock_unlock("mce_display_stm");
#endif

EXIT:
  return;
}

/* ========================================================================= *
 * CPU_SCALING_GOVERNOR
 * ========================================================================= */

#ifdef ENABLE_CPU_GOVERNOR
/** CPU scaling governor override; not enabled by default */
static gint mdy_governor_conf = GOVERNOR_UNSET;

/** GConf callback ID for cpu scaling governor changes */
static guint mdy_governor_conf_id = 0;

/** GOVERNOR_DEFAULT CPU scaling governor settings */
static governor_setting_t *mdy_governor_default = 0;

/** GOVERNOR_INTERACTIVE CPU scaling governor settings */
static governor_setting_t *mdy_governor_interactive = 0;

/** Limit number of files that can be modified via settings */
#define GOVERNOR_MAX_SETTINGS 32

/** Obtain arrays of settings from mce ini-files
 *
 * Use mdy_governor_free_settings() to release data returned from this
 * function.
 *
 * If CPU scaling governor is not defined in mce INI-files, an
 * empty (=no-op) array of settings is returned.
 *
 * @param tag Name of CPU scaling governor state
 *
 * @return array of settings
 */
static governor_setting_t *mdy_governor_get_settings(const char *tag)
{
    governor_setting_t *res = 0;
    size_t              have = 0;
    size_t              used = 0;

    char sec[128], key[128], *path, *data;

    snprintf(sec, sizeof sec, "CPUScalingGovernor%s", tag);

    if( !mce_conf_has_group(sec) ) {
        mce_log(LL_INFO, "Not configured: %s", sec);
        goto EXIT;
    }

    for( size_t i = 0; ; ++i ) {
        snprintf(key, sizeof key, "path%zd", i+1);
        path = mce_conf_get_string(sec, key, 0);
        if( !path || !*path )
            break;

        if( i >= GOVERNOR_MAX_SETTINGS ) {
            mce_log(LL_WARN, "rejecting excess settings;"
                    " starting from: [%s] %s", sec, key);
            break;
        }

        snprintf(key, sizeof key, "data%zd", i+1);
        data = mce_conf_get_string(sec, key, 0);
        if( !data )
            break;

        if( used == have ) {
            have += 8;
            res = realloc(res, have * sizeof *res);
        }

        res[used].path = strdup(path);
        res[used].data = strdup(data);
        ++used;
        mce_log(LL_DEBUG, "%s[%zd]: echo > %s %s",
                sec, used, path, data);
    }

    if( used == 0 ) {
        mce_log(LL_WARN, "No items defined for: %s", sec);
    }

EXIT:
    have = used + 1;
    res = realloc(res, have * sizeof *res);

    res[used].path = 0;
    res[used].data = 0;

    return res;
}

/** Release settings array obtained with mdy_governor_get_settings()
 *
 * @param settings array of settings, or NULL
 */
static void mdy_governor_free_settings(governor_setting_t *settings)
{
    if( settings ) {
        for( size_t i = 0; settings[i].path; ++i ) {
            free(settings[i].path);
            free(settings[i].data);
        }
        free(settings);
    }
}

/** Write string to an already existing sysfs file
 *
 * Since the path originates from configuration data we make
 * some checking in order not to write to an obviously bogus
 * destination, namely:
 * 1) the path must start with /sys/devices/system/cpu/
 * 2) the opened file must have the same device id as /sys
 *
 * @param path file to write to
 * @param data text to write
 *
 * @returns true on success, false on failure
 */
static bool mdy_governor_write_data(const char *path, const char *data)
{
    static const char subtree[] = "/sys/devices/system/cpu/";

    bool  res  = false;
    int   todo = strlen(data);
    int   done = 0;
    int   fd   = -1;
    char *dest = 0;

    struct stat st_sys, st_dest;

    /* get canonicalised absolute path */
    if( !(dest = realpath(path, 0)) ) {
        mce_log(LL_WARN, "%s: failed to resolve real path: %m", path);
        goto cleanup;
    }

    /* check that the destination has more or less expected path */
    if( strncmp(dest, subtree, sizeof subtree - 1) ) {
        mce_log(LL_WARN, "%s: not under %s", dest, subtree);
        goto cleanup;
    }

    /* NB: no O_CREAT & co, the file must already exist */
    if( (fd = TEMP_FAILURE_RETRY(open(dest, O_WRONLY))) == -1 ) {
        mce_log(LL_WARN, "%s: failed to open for writing: %m", dest);
        goto cleanup;
    }

    /* check that the file we managed to open actually resides in sysfs */
    if( stat("/sys", &st_sys) == -1 ) {
        mce_log(LL_WARN, "%s: failed to stat: %m", "/sys");
        goto cleanup;
    }
    if( fstat(fd, &st_dest) == -1 ) {
        mce_log(LL_WARN, "%s: failed to stat: %m", dest);
        goto cleanup;
    }
    if( st_sys.st_dev != st_dest.st_dev ) {
        mce_log(LL_WARN, "%s: not in sysfs", dest);
        goto cleanup;
    }

    /* write the content */
    errno = 0, done = TEMP_FAILURE_RETRY(write(fd, data, todo));

    if( done != todo ) {
        mce_log(LL_WARN, "%s: wrote %d of %d bytes: %m",
                dest, done, todo);
        goto cleanup;
    }

    res = true;

cleanup:

    if( fd != -1 ) TEMP_FAILURE_RETRY(close(fd));
    free(dest);

    return res;
}

/** Write cpu scaling governor parameter to sysfs
 *
 * @param setting Content and where to write it
 */
static void mdy_governor_apply_setting(const governor_setting_t *setting)
{
    glob_t gb;

    memset(&gb, 0, sizeof gb);

    switch( glob(setting->path, 0, 0, &gb) )
    {
    case 0:
        // success
        break;

    case GLOB_NOMATCH:
        mce_log(LL_WARN, "%s: no matches found", setting->path);
        goto cleanup;

    case GLOB_NOSPACE:
    case GLOB_ABORTED:
    default:
            mce_log(LL_ERR, "%s: glob() failed", setting->path);
        goto cleanup;
    }

    for( size_t i = 0; i < gb.gl_pathc; ++i ) {
        if( mdy_governor_write_data(gb.gl_pathv[i], setting->data) ) {
            mce_log(LL_DEBUG, "wrote \"%s\" to: %s",
                    setting->data,  gb.gl_pathv[i]);
        }
    }

cleanup:
    globfree(&gb);
}

/** Switch cpu scaling governor state
 *
 * @param state GOVERNOR_DEFAULT, GOVERNOR_DEFAULT, ...
 */
static void mdy_governor_set_state(int state)
{
    const governor_setting_t *settings = 0;

    switch( state )
    {
    case GOVERNOR_DEFAULT:
        settings = mdy_governor_default;
        break;
    case GOVERNOR_INTERACTIVE:
        settings = mdy_governor_interactive;
        break;

    default: break;
    }

    if( !settings ) {
        mce_log(LL_WARN, "governor state=%d has no mapping", state);
    }
    else {
        for( ; settings->path; ++settings ) {
            mdy_governor_apply_setting(settings);
        }
    }
}

/** Evaluate and apply CPU scaling governor policy */
static void mdy_governor_rethink(void)
{
    static int governor_have = GOVERNOR_UNSET;

    /* By default we want to use "interactive"
     * cpu scaling governor, except ... */
    int governor_want = GOVERNOR_INTERACTIVE;

    /* Use default when in transitional states */
    if( system_state != MCE_STATE_USER &&
        system_state != MCE_STATE_ACTDEAD ) {
        governor_want = GOVERNOR_DEFAULT;
    }

    /* Use default during bootup */
    if( mdy_desktop_ready_id || !mdy_init_done ) {
        governor_want = GOVERNOR_DEFAULT;
    }

    /* Use default during shutdown */
    if( mdy_shutdown_in_progress()  ) {
        governor_want = GOVERNOR_DEFAULT;
    }

    /* Restore default on unload / mce exit */
    if( mdy_unloading_module ) {
        governor_want = GOVERNOR_DEFAULT;
    }

    /* Config override has been set */
    if( mdy_governor_conf != GOVERNOR_UNSET ) {
        governor_want = mdy_governor_conf;
    }

    /* Apply new policy state */
    if( governor_have != governor_want ) {
        mce_log(LL_NOTICE, "state: %d -> %d",
                governor_have,  governor_want);
        mdy_governor_set_state(governor_want);
        governor_have = governor_want;
    }
}

/** Callback for handling changes to cpu scaling governor configuration
 *
 * @param client (not used)
 * @param id     (not used)
 * @param entry  GConf entry that changed
 * @param data   (not used)
 */
static void mdy_governor_conf_cb(GConfClient *const client, const guint id,
                             GConfEntry *const entry, gpointer const data)
{
    (void)client; (void)id; (void)data;

    gint policy = GOVERNOR_UNSET;
    const GConfValue *value = 0;

    if( entry && (value = gconf_entry_get_value(entry)) ) {
        if( value->type == GCONF_VALUE_INT )
            policy = gconf_value_get_int(value);
    }
    if( mdy_governor_conf != policy ) {
        mce_log(LL_NOTICE, "cpu scaling governor change: %d -> %d",
                mdy_governor_conf, policy);
        mdy_governor_conf = policy;
        mdy_governor_rethink();
    }
}
#endif /* ENABLE_CPU_GOVERNOR */

/* ========================================================================= *
 * DBUS_HANDLERS
 * ========================================================================= */

/** Send a blanking pause status reply or signal
 *
 * @param method_call A DBusMessage to reply to; or NULL to send signal
 *
 * @return TRUE
 */
static gboolean mdy_dbus_send_blanking_pause_status(DBusMessage *const method_call)
{
    static int   prev = -1;
    bool         curr = mdy_blanking_is_paused();
    const char  *data = (curr ?
                         MCE_PREVENT_BLANK_ACTIVE_STRING :
                         MCE_PREVENT_BLANK_INACTIVE_STRING);
    DBusMessage *msg  = 0;

    if( method_call ) {
        msg = dbus_new_method_reply(method_call);
        mce_log(LL_DEBUG, "Sending blanking pause reply: %s", data);
    }
    else {
        if( prev == curr )
            goto EXIT;

        prev = curr;
        msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_PREVENT_BLANK_SIG);
        mce_log(LL_DEVEL, "Sending blanking pause signal: %s", data);
    }

    if( !dbus_message_append_args(msg,
                                  DBUS_TYPE_STRING, &data,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    dbus_send_message(msg), msg = 0;

EXIT:
    if( msg )
        dbus_message_unref(msg);

    return TRUE;
}

/** D-Bus callback for the get blanking pause status method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean mdy_dbus_handle_blanking_pause_get_req(DBusMessage *const msg)
{
    mce_log(LL_DEVEL, "Received blanking pause status get request from %s",
            mce_dbus_get_message_sender_ident(msg));

    mdy_dbus_send_blanking_pause_status(msg);

    return TRUE;
}

/** Send a blanking inhibit status reply or signal
 *
 * @param method_call A DBusMessage to reply to; or NULL to send signal
 *
 * @return TRUE
 */
static gboolean mdy_dbus_send_blanking_inhibit_status(DBusMessage *const method_call)
{
    static int   prev = -1;
    bool         curr = false;

    /* Having blanking pause active counts as inhibit active too */
    if( mdy_blanking_is_paused() )
        curr = true;

    /* In display on/dim state we should have either dimming or
     * blanking timer active. If that is not the case, some form of
     * blanking inhibit is active. This should catch things like
     * stay-on inhibit modes, update mode, never-blank mode, etc
     */
    if( display_state == MCE_DISPLAY_ON ||
        display_state == MCE_DISPLAY_DIM ) {
        if( !mdy_blanking_off_cb_id && !mdy_blanking_dim_cb_id )
            curr = true;
    }

    /* The stay-dim inhibit modes do not prevent dimming, so those need
     * to be taken into account separately.
     */
    if( display_state == MCE_DISPLAY_ON && mdy_blanking_dim_cb_id ) {
        if( mdy_blanking_inhibit_dim_p() )
            curr = true;
    }

    DBusMessage *msg  = 0;
    const char  *data = (curr ?
                         MCE_INHIBIT_BLANK_ACTIVE_STRING :
                         MCE_INHIBIT_BLANK_INACTIVE_STRING);

    if( method_call ) {
        msg = dbus_new_method_reply(method_call);
        mce_log(LL_DEBUG, "Sending blanking inhibit reply: %s", data);
    }
    else {
        if( prev == curr )
            goto EXIT;

        prev = curr;
        msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_BLANKING_INHIBIT_SIG);
        mce_log(LL_DEVEL, "Sending blanking inhibit signal: %s", data);
    }

    if( !dbus_message_append_args(msg,
                                  DBUS_TYPE_STRING, &data,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    dbus_send_message(msg), msg = 0;

EXIT:
    if( msg )
        dbus_message_unref(msg);

    return TRUE;
}

/** D-Bus callback for the get blanking inhibit status method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean mdy_dbus_handle_blanking_inhibit_get_req(DBusMessage *const msg)
{
    mce_log(LL_DEVEL, "Received blanking inhibit status get request from %s",
            mce_dbus_get_message_sender_ident(msg));

    mdy_dbus_send_blanking_inhibit_status(msg);

    return TRUE;
}

/** Latest display state indication that was broadcast over D-Bus */
static const gchar *mdy_dbus_last_display_state = "";

/** Clear lastest display state sent to force re-broadcasting
 */
static void mdy_dbus_invalidate_display_status(void)
{
    mdy_dbus_last_display_state = "";
}

/**
 * Send a display status reply or signal
 *
 * @param method_call A DBusMessage to reply to;
 *                    pass NULL to send a display status signal instead
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_send_display_status(DBusMessage *const method_call)
{
    gboolean     status = FALSE;
    DBusMessage *msg    = NULL;
    const gchar *state  = MCE_DISPLAY_OFF_STRING;

    /* Evaluate display state name to send */
    switch( display_state_next ) {
    default:
        break;

    case MCE_DISPLAY_DIM:
        state = MCE_DISPLAY_DIM_STRING;
        break;

    case MCE_DISPLAY_ON:
        state = MCE_DISPLAY_ON_STRING;
        break;
    }

    if( !method_call ) {
        /* Signal fully powered on states when the transition has
         * finished, other states when transition starts.
         *
         * The intent is that ui sees display state change to off
         * before setUpdatesEnabled(false) ipc is made and it stays
         * off until reply to setUpdatesEnabled(true) is received.
         */
        switch( display_state_next ) {
        case MCE_DISPLAY_ON:
        case MCE_DISPLAY_DIM:
            if( display_state != display_state_next )
                goto EXIT;
            break;

        default:
            break;
        }

        if( !strcmp(mdy_dbus_last_display_state, state))
            goto EXIT;
        mdy_dbus_last_display_state = state;
        mce_log(LL_NOTICE, "Sending display status signal: %s", state);
    }
    else
        mce_log(LL_DEBUG, "Sending display status reply: %s", state);

    /* If method_call is set, send a reply,
     * otherwise, send a signal
     */
    if (method_call != NULL) {
        msg = dbus_new_method_reply(method_call);
    } else {
        /* display_status_ind */
        msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_DISPLAY_SIG);
    }

    /* Append the display status */
    if (dbus_message_append_args(msg,
                                 DBUS_TYPE_STRING, &state,
                                 DBUS_TYPE_INVALID) == FALSE) {
        mce_log(LL_ERR, "Failed to append %sargument to D-Bus message "
                "for %s.%s",
                method_call ? "reply " : "",
                method_call ? MCE_REQUEST_IF :
                MCE_SIGNAL_IF,
                method_call ? MCE_DISPLAY_STATUS_GET :
                MCE_DISPLAY_SIG);
        dbus_message_unref(msg);
        goto EXIT;
    }

    /* Send the message */
    status = dbus_send_message(msg);

EXIT:
    return status;
}

/** Helper for deciding if external display on/dim request are allowed
 *
 * There are state machines handling display on/off during
 * calls and alarms. We do not want external requests to interfere
 * with them.
 *
 * @return reason to block, or NULL if allowed
 */
static const char *mdy_dbus_get_reason_to_block_display_on(void)
{
    const char *reason = 0;

    /* display off? */
    switch( display_state ) {
    default:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
    case MCE_DISPLAY_UNDEF:
        break;

    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_ON:
        // it is already powered on, nothing to block
        goto EXIT;
    }

    /* system state must be USER or ACT DEAD */
    switch( system_state ) {
    case MCE_STATE_USER:
    case MCE_STATE_ACTDEAD:
        break;
    default:
        reason = "system_state != USER|ACTDEAD";
        goto EXIT;
    }

    /* active calls? */
    switch( call_state ) {
    case CALL_STATE_RINGING:
    case CALL_STATE_ACTIVE:
        reason = "call ringing|active";
        goto EXIT;
    default:
        break;
    }

    /* active alarms? */
    switch( alarm_ui_state ) {
    case MCE_ALARM_UI_RINGING_INT32:
    case MCE_ALARM_UI_VISIBLE_INT32:
        reason = "active alarm";
        goto EXIT;
    default:
        break;
    }

    /* lid closed? */
    if( lid_cover_policy_state == COVER_CLOSED ) {
        reason = "lid closed";
        goto EXIT;
    }

    /* proximity covered? */
    if( proximity_state == COVER_CLOSED ) {
        reason = "proximity covered";
        goto EXIT;
    }
EXIT:

    return reason;
}

/** Helper for handling display state requests coming over D-Bus
 *
 * @param state  Requested display state
 */
static void mdy_dbus_handle_display_state_req(display_state_t state)
{
    /* When dealing with display state requests coming over D-Bus,
     * we need to make sure an indication signal is sent even if
     * the request gets ignored due to never-blank mode or something
     * similar -> reset the last indication sent cache */
    mdy_dbus_invalidate_display_status();

    execute_datapipe(&display_state_req_pipe,
                     GINT_TO_POINTER(state),
                     USE_INDATA, CACHE_INDATA);
}

/**
 * D-Bus callback for the display on method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean mdy_dbus_handle_display_on_req(DBusMessage *const msg)
{
    display_state_t  request = datapipe_get_gint(display_state_next_pipe);
    const char      *reason  = mdy_dbus_get_reason_to_block_display_on();

    if( reason ) {
        mce_log(LL_WARN, "display ON request from %s denied: %s",
                mce_dbus_get_message_sender_ident(msg), reason);
    }
    else {
        mce_log(LL_DEVEL,"display ON request from %s",
                mce_dbus_get_message_sender_ident(msg));
        request = MCE_DISPLAY_ON;
    }

    if( !dbus_message_get_no_reply(msg) )
        dbus_send_message(dbus_new_method_reply(msg));

    mdy_dbus_handle_display_state_req(request);

    return TRUE;
}

/**
 * D-Bus callback for the display dim method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean mdy_dbus_handle_display_dim_req(DBusMessage *const msg)
{
    display_state_t  request = datapipe_get_gint(display_state_next_pipe);
    const char      *reason  = mdy_dbus_get_reason_to_block_display_on();

    if( reason ) {
        mce_log(LL_WARN, "display DIM request from %s denied: %s",
                mce_dbus_get_message_sender_ident(msg), reason);
    }
    else {
        mce_log(LL_DEVEL,"display DIM request from %s",
                mce_dbus_get_message_sender_ident(msg));
        request = MCE_DISPLAY_DIM;
    }

    if( !dbus_message_get_no_reply(msg) )
        dbus_send_message(dbus_new_method_reply(msg));

    mdy_dbus_handle_display_state_req(request);

    return TRUE;
}

/** Override mode for display off requests made over D-Bus */
static gint mdy_dbus_display_off_override = DEFAULT_DISPLAY_OFF_OVERRIDE;

/** GConf notifier id for mdy_dbus_display_off_override */
static guint mdy_dbus_display_off_override_gconf_cb_id = 0;

/**
 * D-Bus callback for the display off method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean mdy_dbus_handle_display_off_req(DBusMessage *const msg)
{
    if( mdy_dbus_display_off_override == DISPLAY_OFF_OVERRIDE_USE_LPM )
        return mdy_dbus_handle_display_lpm_req(msg);

    mce_log(LL_DEVEL, "display off request from %s",
            mce_dbus_get_message_sender_ident(msg));

    if( !dbus_message_get_no_reply(msg) )
        dbus_send_message(dbus_new_method_reply(msg));

    execute_datapipe(&tk_lock_pipe,
                     GINT_TO_POINTER(LOCK_ON),
                     USE_INDATA, CACHE_INDATA);

    mdy_dbus_handle_display_state_req(MCE_DISPLAY_OFF);

    return TRUE;
}

/** D-Bus callback for the display lpm method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean mdy_dbus_handle_display_lpm_req(DBusMessage *const msg)
{
    display_state_t  current = datapipe_get_gint(display_state_next_pipe);
    display_state_t  request = MCE_DISPLAY_OFF;
    bool             lock_ui = true;
    const char      *reason  = 0;

    mce_log(LL_DEVEL, "display lpm request from %s",
            mce_dbus_get_message_sender_ident(msg));

    /* Ignore lpm requests if there are active calls / alarms */
    if( exception_state & (UIEXC_CALL | UIEXC_ALARM) ) {
        reason  = "call or alarm active";
        request = current;
        lock_ui = false;
        goto EXIT;
    }

    /* If there is any reason to block display on/dim request,
     * it applies for lpm requests too */
    if( (reason = mdy_dbus_get_reason_to_block_display_on()) )
        goto EXIT;

    /* But UI side is allowed only to blank via lpm */
    switch( current ) {
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_ON:
        request = MCE_DISPLAY_LPM_ON;
        break;

    default:
        reason = "display is off";
        break;
    }

EXIT:
    if( !dbus_message_get_no_reply(msg) )
        dbus_send_message(dbus_new_method_reply(msg));

    /* Warn if lpm request can't be applied */
    if( reason ) {
        mce_log(LL_WARN, "display LPM request from %s denied: %s",
                mce_dbus_get_message_sender_ident(msg), reason);
    }

    if( lock_ui ) {
        execute_datapipe(&tk_lock_pipe,
                         GINT_TO_POINTER(LOCK_ON),
                         USE_INDATA, CACHE_INDATA);
    }

    mdy_dbus_handle_display_state_req(request);

    return TRUE;
}

/**
 * D-Bus callback for the get display status method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_handle_display_status_get_req(DBusMessage *const msg)
{
    gboolean status = FALSE;

    mce_log(LL_DEVEL, "Received display status get request from %s",
            mce_dbus_get_message_sender_ident(msg));

    /* Try to send a reply that contains the current display status */
    if (mdy_dbus_send_display_status(msg) == FALSE)
        goto EXIT;

    status = TRUE;

EXIT:
    return status;
}

/**
 * Send a CABC status reply
 *
 * @param method_call A DBusMessage to reply to
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_send_cabc_mode(DBusMessage *const method_call)
{
    const gchar *dbus_cabc_mode = NULL;
    DBusMessage *msg = NULL;
    gboolean status = FALSE;
    gint i;

    for (i = 0; mdy_cabc_mode_mapping[i].sysfs != NULL; i++) {
        if (!strcmp(mdy_cabc_mode_mapping[i].sysfs, mdy_cabc_mode)) {
            dbus_cabc_mode = mdy_cabc_mode_mapping[i].dbus;
            break;
        }
    }

    if (dbus_cabc_mode == NULL)
        dbus_cabc_mode = MCE_CABC_MODE_OFF;

    mce_log(LL_DEBUG,"Sending CABC mode: %s", dbus_cabc_mode);

    msg = dbus_new_method_reply(method_call);

    /* Append the CABC mode */
    if (dbus_message_append_args(msg,
                                 DBUS_TYPE_STRING, &dbus_cabc_mode,
                                 DBUS_TYPE_INVALID) == FALSE) {
        mce_log(LL_ERR, "Failed to append reply argument to D-Bus message "
                "for %s.%s",
                MCE_REQUEST_IF, MCE_CABC_MODE_GET);
        dbus_message_unref(msg);
        goto EXIT;
    }

    /* Send the message */
    status = dbus_send_message(msg);

EXIT:
    return status;
}

/**
 * D-Bus callback used for monitoring the process that requested
 * CABC mode change; if that process exits, immediately
 * restore the CABC mode to the default
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_handle_cabc_mode_owner_lost_sig(DBusMessage *const msg)
{
    gboolean status = FALSE;
    const gchar *old_name;
    const gchar *new_name;
    const gchar *service;
    DBusError error;

    /* Register error channel */
    dbus_error_init(&error);

    /* Extract result */
    if (dbus_message_get_args(msg, &error,
                              DBUS_TYPE_STRING, &service,
                              DBUS_TYPE_STRING, &old_name,
                              DBUS_TYPE_STRING, &new_name,
                              DBUS_TYPE_INVALID) == FALSE) {
        mce_log(LL_ERR, "Failed to get argument from %s.%s; %s",
                "org.freedesktop.DBus", "NameOwnerChanged",
                error.message);
        dbus_error_free(&error);
        goto EXIT;
    }

    /* Remove the name monitor for the CABC mode */
    mce_dbus_owner_monitor_remove_all(&mdy_cabc_mode_monitor_list);
    mdy_cabc_mode_set(DEFAULT_CABC_MODE);

    status = TRUE;

EXIT:
    return status;
}

/**
 * D-Bus callback for the get CABC mode method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_handle_cabc_mode_get_req(DBusMessage *const msg)
{
    gboolean status = FALSE;

    mce_log(LL_DEVEL, "Received CABC mode get request from %s",
            mce_dbus_get_message_sender_ident(msg));

    /* Try to send a reply that contains the current CABC mode */
    if (mdy_dbus_send_cabc_mode(msg) == FALSE)
        goto EXIT;

    status = TRUE;

EXIT:
    return status;
}

/**
 * D-Bus callback for the set CABC mode method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_handle_cabc_mode_set_req(DBusMessage *const msg)
{
    dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
    const gchar *sender = dbus_message_get_sender(msg);
    const gchar *sysfs_cabc_mode = NULL;
    const gchar *dbus_cabc_mode = NULL;
    gboolean status = FALSE;
    gint i;
    DBusError error;

    /* Register error channel */
    dbus_error_init(&error);

    if (sender == NULL) {
        mce_log(LL_ERR, "invalid set CABC mode request (NULL sender)");
        goto EXIT;
    }

    mce_log(LL_DEVEL, "Received set CABC mode request from %s",
            mce_dbus_get_name_owner_ident(sender));

    /* Extract result */
    if (dbus_message_get_args(msg, &error,
                              DBUS_TYPE_STRING, &dbus_cabc_mode,
                              DBUS_TYPE_INVALID) == FALSE) {
        mce_log(LL_ERR, "Failed to get argument from %s.%s; %s",
                MCE_REQUEST_IF, MCE_CABC_MODE_REQ,
                error.message);
        dbus_error_free(&error);
        goto EXIT;
    }

    for (i = 0; mdy_cabc_mode_mapping[i].dbus != NULL; i++) {
        if (!strcmp(mdy_cabc_mode_mapping[i].dbus, dbus_cabc_mode)) {
            sysfs_cabc_mode = mdy_cabc_mode_mapping[i].sysfs;
        }
    }

    /* Use the default if the requested mode was invalid */
    if (sysfs_cabc_mode == NULL) {
        mce_log(LL_WARN, "Invalid CABC mode requested; using %s",
                DEFAULT_CABC_MODE);
        sysfs_cabc_mode = DEFAULT_CABC_MODE;
    }

    mdy_cabc_mode_set(sysfs_cabc_mode);

    /* We only ever monitor one owner; latest wins */
    mce_dbus_owner_monitor_remove_all(&mdy_cabc_mode_monitor_list);

    if (mce_dbus_owner_monitor_add(sender,
                                   mdy_dbus_handle_cabc_mode_owner_lost_sig,
                                   &mdy_cabc_mode_monitor_list,
                                   1) == -1) {
        mce_log(LL_INFO, "Failed to add name owner monitoring for `%s'",
                mce_dbus_get_name_owner_ident(sender));
    }

    /* If reply is wanted, send the current CABC mode */
    if (no_reply == FALSE) {
        DBusMessage *reply = dbus_new_method_reply(msg);

        for (i = 0; mdy_cabc_mode_mapping[i].sysfs != NULL; i++) {
            if (!strcmp(sysfs_cabc_mode, mdy_cabc_mode_mapping[i].sysfs)) {
                // XXX: error handling!
                dbus_message_append_args(reply, DBUS_TYPE_STRING, &mdy_cabc_mode_mapping[i].dbus, DBUS_TYPE_INVALID);
                break;
            }
        }

        status = dbus_send_message(reply);
    } else {
        status = TRUE;
    }

EXIT:
    return status;
}

/**
 * D-Bus callback for display blanking prevent request method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_handle_blanking_pause_start_req(DBusMessage *const msg)
{
    gboolean     status   = FALSE;
    dbus_bool_t  no_reply = dbus_message_get_no_reply(msg);
    const gchar *sender   = dbus_message_get_sender(msg);

    if( !sender ) {
        mce_log(LL_ERR, "invalid blanking pause request (NULL sender)");
        goto EXIT;
    }

    mce_log(LL_DEVEL, "blanking pause request from %s",
            mce_dbus_get_name_owner_ident(sender));

    mdy_blanking_add_pause_client(sender);

    if( no_reply )
        status = TRUE;
    else {
        DBusMessage *reply = dbus_new_method_reply(msg);
        status = dbus_send_message(reply), reply = 0;
    }

EXIT:
    return status;
}

/**
 * D-Bus callback for display cancel blanking prevent request method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_handle_blanking_pause_cancel_req(DBusMessage *const msg)
{
    gboolean     status   = FALSE;
    dbus_bool_t  no_reply = dbus_message_get_no_reply(msg);
    const gchar *sender   = dbus_message_get_sender(msg);

    if( !sender ) {
        mce_log(LL_ERR, "invalid cancel blanking pause request (NULL sender)");
        goto EXIT;
    }

    mce_log(LL_DEVEL, "cancel blanking pause request from %s",
            mce_dbus_get_name_owner_ident(sender));

    mdy_blanking_remove_pause_client(sender);

    if( no_reply)
        status = TRUE;
    else {
        DBusMessage *reply = dbus_new_method_reply(msg);
        status = dbus_send_message(reply), reply = 0;
    }

EXIT:
    return status;
}

/**
 * D-Bus callback for the desktop startup notification signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_handle_desktop_started_sig(DBusMessage *const msg)
{
    gboolean status = FALSE;

    (void)msg;

    mce_log(LL_NOTICE, "Received desktop startup notification");

    mce_log(LL_DEBUG, "deactivate MCE_LED_PATTERN_POWER_ON");
    execute_datapipe_output_triggers(&led_pattern_deactivate_pipe,
                                     MCE_LED_PATTERN_POWER_ON, USE_INDATA);

    mce_rem_submode_int32(MCE_BOOTUP_SUBMODE);

    mce_rem_submode_int32(MCE_MALF_SUBMODE);
    if (g_access(MCE_MALF_FILENAME, F_OK) == 0) {
        g_remove(MCE_MALF_FILENAME);
    }

    /* Reprogram blanking timers */
    mdy_blanking_rethink_timers(true);

    status = TRUE;

    return status;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t mdy_dbus_handlers[] =
{
    /* signals - outbound (for Introspect purposes only) */
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_PREVENT_BLANK_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"blanking_pause\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_BLANKING_INHIBIT_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"blanking_inhibit\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_DISPLAY_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"display_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_FADER_OPACITY_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"fader_opacity_percent\" type=\"i\"/>\n"
            "    <arg name=\"transition_length\" type=\"i\"/>\n"
    },
    /* signals */
    {
        .interface = "com.nokia.startup.signal",
        .name      = "desktop_visible",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = mdy_dbus_handle_desktop_started_sig,
    },
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_PREVENT_BLANK_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mdy_dbus_handle_blanking_pause_get_req,
        .args      =
            "    <arg direction=\"out\" name=\"blanking_pause\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_BLANKING_INHIBIT_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mdy_dbus_handle_blanking_inhibit_get_req,
        .args      =
            "    <arg direction=\"out\" name=\"blanking_inhibit\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_DISPLAY_STATUS_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mdy_dbus_handle_display_status_get_req,
        .args      =
            "    <arg direction=\"out\" name=\"display_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_CABC_MODE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mdy_dbus_handle_cabc_mode_get_req,
        .args      =
            "    <arg direction=\"out\" name=\"cabc_mode\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_DISPLAY_ON_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mdy_dbus_handle_display_on_req,
        .args      =
            ""
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_DISPLAY_DIM_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mdy_dbus_handle_display_dim_req,
        .args      =
            ""
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_DISPLAY_OFF_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mdy_dbus_handle_display_off_req,
        .args      =
            ""
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_DISPLAY_LPM_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mdy_dbus_handle_display_lpm_req,
        .args      =
            ""
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_PREVENT_BLANK_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mdy_dbus_handle_blanking_pause_start_req,
        .args      =
            ""
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_CANCEL_PREVENT_BLANK_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mdy_dbus_handle_blanking_pause_cancel_req,
        .args      =
            ""
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_CABC_MODE_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mdy_dbus_handle_cabc_mode_set_req,
        .args      =
            "    <arg direction=\"in\" name=\"requested_cabc_mode\" type=\"s\"/>\n"
            "    <arg direction=\"out\" name=\"activated_cabc_mode\" type=\"s\"/>\n"
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Install dbus message handlers
 */
static void mdy_dbus_init(void)
{
    mce_dbus_handler_register_array(mdy_dbus_handlers);
}

/** Remove dbus message handlers
 */
static void mdy_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(mdy_dbus_handlers);
}

/* ========================================================================= *
 * FLAG_FILE_TRACKING
 * ========================================================================= */

/** Simulated "desktop ready" via uptime based timer
 */
static gboolean mdy_flagfiles_desktop_ready_cb(gpointer user_data)
{
    (void)user_data;
    if( mdy_desktop_ready_id ) {
        mdy_desktop_ready_id = 0;
        mce_log(LL_NOTICE, "desktop ready delay ended");
        mdy_stm_schedule_rethink();
#ifdef ENABLE_CPU_GOVERNOR
        mdy_governor_rethink();
#endif
    }
    return FALSE;
}

/** Content of init-done flag file has changed
 *
 * @param path directory where flag file is
 * @param file name of the flag file
 * @param data (not used)
 */
static void mdy_flagfiles_init_done_cb(const char *path,
                                 const char *file,
                                 gpointer data)
{
    (void)data;

    char full[256];
    snprintf(full, sizeof full, "%s/%s", path, file);

    gboolean flag = access(full, F_OK) ? FALSE : TRUE;

    if( mdy_init_done != flag ) {
        mdy_init_done = flag;
        mce_log(LL_NOTICE, "init_done flag file present: %s",
                mdy_init_done ? "true" : "false");
        mdy_stm_schedule_rethink();
#ifdef ENABLE_CPU_GOVERNOR
        mdy_governor_rethink();
#endif
        mdy_poweron_led_rethink();
        mdy_blanking_rethink_afterboot_delay();
    }
}

/** Content of update-mode flag file has changed
 *
 * @param path directory where flag file is
 * @param file name of the flag file
 * @param data (not used)
 */
static void mdy_flagfiles_update_mode_cb(const char *path,
                                         const char *file,
                                         gpointer data)
{
    (void)data;

    char full[256];
    snprintf(full, sizeof full, "%s/%s", path, file);

    gboolean flag = access(full, F_OK) ? FALSE : TRUE;

    if( mdy_update_mode != flag ) {
        mdy_update_mode = flag;

        /* Log by default as it might help analyzing upgrade problems */
        mce_log(LL_WARN, "update_mode flag file present: %s",
                mdy_update_mode ? "true" : "false");

        if( mdy_update_mode ) {
            /* Issue display on request when update mode starts */
            execute_datapipe(&display_state_req_pipe,
                             GINT_TO_POINTER(MCE_DISPLAY_ON),
                             USE_INDATA, CACHE_INDATA);
        }

        /* suspend policy is affected by update mode */
        mdy_stm_schedule_rethink();

        /* blanking timers need to be started or stopped */
        mdy_blanking_rethink_timers(true);

        /* broadcast change within mce */
        execute_datapipe(&update_mode_pipe,
                         GINT_TO_POINTER(mdy_update_mode),
                         USE_INDATA, CACHE_INDATA);
    }
}

/** Content of bootstate flag file has changed
 *
 * @param path directory where flag file is
 * @param file name of the flag file
 * @param data (not used)
 */
static void mdy_flagfiles_bootstate_cb(const char *path,
                                     const char *file,
                                     gpointer data)
{
    (void)data;

    int fd = -1;

    char full[256];
    char buff[256];
    int rc;

    snprintf(full, sizeof full, "%s/%s", path, file);

    /* default to unknown */
    mdy_bootstate = BOOTSTATE_UNKNOWN;

    if( (fd = open(full, O_RDONLY)) == -1 ) {
        if( errno != ENOENT )
            mce_log(LL_WARN, "%s: %m", full);
        goto EXIT;
    }

    if( (rc = read(fd, buff, sizeof buff - 1)) == -1 ) {
        mce_log(LL_WARN, "%s: %m", full);
        goto EXIT;
    }

    buff[rc] = 0;
    buff[strcspn(buff, "\r\n")] = 0;

    mce_log(LL_NOTICE, "bootstate flag file content: %s", buff);

    /* for now we only need to differentiate USER and not USER */
    if( !strcmp(buff, "BOOTSTATE=USER") )
        mdy_bootstate = BOOTSTATE_USER;
    else
        mdy_bootstate = BOOTSTATE_ACT_DEAD;
EXIT:
    if( fd != -1 ) close(fd);

    mdy_poweron_led_rethink();
    mdy_blanking_rethink_afterboot_delay();
}

/** Start tracking of init_done and bootstate flag files
 */
static void mdy_flagfiles_start_tracking(void)
{
    static const char update_dir[] = "/tmp";
    static const char update_flag[] = "os-update-running";

    static const char flag_dir[]  = "/run/systemd/boot-status";
    static const char flag_init[] = "init-done";
    static const char flag_boot[] = "bootstate";

    time_t uptime = 0;  // uptime in seconds
    time_t ready  = 60; // desktop ready at
    time_t delay  = 10; // default wait time

    /* if the update directory exits, track flag file presense */
    if( access(update_dir, F_OK) == 0 ) {
        mdy_update_mode_watcher = filewatcher_create(update_dir, update_flag,
                                                     mdy_flagfiles_update_mode_cb,
                                                     0, 0);
    }

    /* if the status directory exists, wait for flag file to appear */
    if( access(flag_dir, F_OK) == 0 ) {
        mdy_init_done_watcher = filewatcher_create(flag_dir, flag_init,
                                               mdy_flagfiles_init_done_cb,
                                               0, 0);
        mdy_bootstate_watcher = filewatcher_create(flag_dir, flag_boot,
                                               mdy_flagfiles_bootstate_cb,
                                               0, 0);
    }

    /* or fall back to waiting for uptime to reach some minimum value */
    if( !mdy_init_done_watcher ) {
        struct timespec ts;

        /* Assume that monotonic clock == uptime */
        if( clock_gettime(CLOCK_MONOTONIC, &ts) == 0 )
            uptime = ts.tv_sec;

        if( uptime + delay < ready )
            delay = ready - uptime;

        /* do not wait for the init-done flag file */
        mdy_init_done = TRUE;
    }

    mce_log(LL_NOTICE, "suspend delay %d seconds", (int)delay);
    mdy_desktop_ready_id = g_timeout_add_seconds(delay, mdy_flagfiles_desktop_ready_cb, 0);

    if( mdy_init_done_watcher ) {
        /* evaluate the initial state of init-done flag file */
        filewatcher_force_trigger(mdy_init_done_watcher);
    }

    if( mdy_bootstate_watcher ) {
        /* evaluate the initial state of bootstate flag file */
        filewatcher_force_trigger(mdy_bootstate_watcher);
    }
    else {
        /* or assume ACT_DEAD & co are not supported */
        mdy_bootstate = BOOTSTATE_USER;
    }

    if( mdy_update_mode_watcher ) {
        /* evaluate the initial state of update-mode flag file */
        filewatcher_force_trigger(mdy_update_mode_watcher);
    }
}

/** Stop tracking of init_done state
 */
static void mdy_flagfiles_stop_tracking(void)
{
    filewatcher_delete(mdy_update_mode_watcher), mdy_update_mode_watcher = 0;

    filewatcher_delete(mdy_init_done_watcher), mdy_init_done_watcher = 0;

    filewatcher_delete(mdy_bootstate_watcher), mdy_bootstate_watcher = 0;

    if( mdy_desktop_ready_id ) {
        g_source_remove(mdy_desktop_ready_id);
        mdy_desktop_ready_id = 0;
    }
}

/* ========================================================================= *
 * GCONF_SETTINGS
 * ========================================================================= */

/**
 * GConf callback for display related settings
 *
 * @param gcc Unused
 * @param id Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data Unused
 */
static void mdy_gconf_cb(GConfClient *const gcc, const guint id,
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

    if( id == mdy_brightness_setting_gconf_id ) {
        gint val = gconf_value_get_int(gcv);
        if( mdy_brightness_setting != val ) {
            mce_log(LL_NOTICE, "mdy_brightness_setting: %d -> %d",
                    mdy_brightness_setting, val);
            mdy_brightness_setting = val;

            /* Save timestamp of the setting change */
            mdy_brightness_setting_change_time = mce_lib_get_boot_tick();

            mdy_gconf_sanitize_brightness_settings();
        }
    }
    else if( id == mdy_brightness_dim_static_gconf_id ) {
        gint val = gconf_value_get_int(gcv);
        if( mdy_brightness_dim_static != val ) {
            mce_log(LL_NOTICE, "mdy_brightness_dim_static: %d -> %d",
                    mdy_brightness_dim_static, val);
            mdy_brightness_dim_static = val;

            mdy_gconf_sanitize_brightness_settings();
            mdy_brightness_set_dim_level();
        }
    }
    else if( id == mdy_brightness_dim_dynamic_gconf_id ) {
        gint val = gconf_value_get_int(gcv);
        if( mdy_brightness_dim_dynamic != val ) {
            mce_log(LL_NOTICE, "mdy_brightness_dim_dynamic: %d -> %d",
                    mdy_brightness_dim_dynamic, val);
            mdy_brightness_dim_dynamic = val;

            mdy_gconf_sanitize_brightness_settings();
            mdy_brightness_set_dim_level();
        }
    }
    else if( id == mdy_brightness_dim_compositor_lo_gconf_id ) {
        gint val = gconf_value_get_int(gcv);
        if( mdy_brightness_dim_compositor_lo != val ) {
            mce_log(LL_NOTICE, "mdy_brightness_dim_compositor_lo: %d -> %d",
                    mdy_brightness_dim_compositor_lo, val);
            mdy_brightness_dim_compositor_lo = val;

            mdy_gconf_sanitize_brightness_settings();
            mdy_brightness_set_dim_level();
        }
    }
    else if( id == mdy_brightness_dim_compositor_hi_gconf_id ) {
        gint val = gconf_value_get_int(gcv);
        if( mdy_brightness_dim_compositor_hi != val ) {
            mce_log(LL_NOTICE, "mdy_brightness_dim_compositor_hi: %d -> %d",
                    mdy_brightness_dim_compositor_hi, val);
            mdy_brightness_dim_compositor_hi = val;

            mdy_gconf_sanitize_brightness_settings();
            mdy_brightness_set_dim_level();
        }
    }
    else if( id == mdy_automatic_brightness_setting_gconf_id ) {
        /* Save timestamp of the setting change */
        mdy_brightness_setting_change_time = mce_lib_get_boot_tick();
    }
    else if( id == mdy_brightness_step_size_gconf_id ) {
        // NOTE: This is not supposed to be changed at runtime
        gint val = gconf_value_get_int(gcv);
        if( mdy_brightness_step_size != val ) {
            mce_log(LL_WARN, "mdy_brightness_step_size: %d -> %d",
                    mdy_brightness_step_size, val);
            mdy_brightness_step_size = val;
            mdy_gconf_sanitize_brightness_settings();
        }
    }
    else if( id == mdy_brightness_step_count_gconf_id ) {
        // NOTE: This is not supposed to be changed at runtime
        gint val = gconf_value_get_int(gcv);
        if( mdy_brightness_step_count != val ) {
            mce_log(LL_WARN, "mdy_brightness_step_count: %d -> %d",
                    mdy_brightness_step_count, val);
            mdy_brightness_step_count = val;
            mdy_gconf_sanitize_brightness_settings();
        }
    }
    else if (id == mdy_blank_timeout_gconf_cb_id) {
        mdy_blank_timeout = gconf_value_get_int(gcv);
        mdy_blanking_update_inactivity_timeout();

        /* Reprogram blanking timers */
        mdy_blanking_rethink_timers(true);
    }
    else if( id == mdy_blank_from_lockscreen_timeout_gconf_cb_id )
    {
        mdy_blank_from_lockscreen_timeout = gconf_value_get_int(gcv);

        /* Reprogram blanking timers */
        mdy_blanking_rethink_timers(true);
    }
    else if( id == mdy_blank_from_lpm_on_timeout_gconf_cb_id )
    {
        mdy_blank_from_lpm_on_timeout = gconf_value_get_int(gcv);

        /* Reprogram blanking timers */
        mdy_blanking_rethink_timers(true);
    }
    else if( id == mdy_blank_from_lpm_on_timeout_gconf_cb_id )
    {
        mdy_blank_from_lpm_on_timeout = gconf_value_get_int(gcv);

        /* Reprogram blanking timers */
        mdy_blanking_rethink_timers(true);
    }
    else if (id == mdy_use_low_power_mode_gconf_cb_id) {
        mdy_use_low_power_mode = gconf_value_get_bool(gcv);

        if (((display_state == MCE_DISPLAY_LPM_OFF) ||
             (display_state == MCE_DISPLAY_LPM_ON)) &&
            ((mdy_low_power_mode_supported == FALSE) ||
                (mdy_use_low_power_mode == FALSE) ||
                (mdy_blanking_can_blank_from_low_power_mode() == TRUE))) {
            execute_datapipe(&display_state_req_pipe,
                             GINT_TO_POINTER(MCE_DISPLAY_OFF),
                             USE_INDATA, CACHE_INDATA);
        }
        else if ((display_state == MCE_DISPLAY_OFF) &&
                 (mdy_use_low_power_mode == TRUE) &&
                 (mdy_blanking_can_blank_from_low_power_mode() == FALSE) &&
                 (mdy_low_power_mode_supported == TRUE)) {
            execute_datapipe(&display_state_req_pipe,
                             GINT_TO_POINTER(MCE_DISPLAY_LPM_ON),
                             USE_INDATA, CACHE_INDATA);
        }
    }
    else if (id == mdy_adaptive_dimming_enabled_gconf_cb_id) {
        mdy_adaptive_dimming_enabled = gconf_value_get_bool(gcv);
        mdy_blanking_stop_adaptive_dimming();
    }
    else if (id == mdy_adaptive_dimming_threshold_gconf_cb_id) {
        mdy_adaptive_dimming_threshold = gconf_value_get_int(gcv);
        mdy_blanking_stop_adaptive_dimming();
    }
    else if (id == mdy_possible_dim_timeouts_gconf_cb_id )
    {
        mdy_gconf_sanitize_dim_timeouts(true);

        /* Reprogram blanking timers */
        mdy_blanking_rethink_timers(true);
    }
    else if (id == mdy_disp_dim_timeout_gconf_cb_id) {
        mdy_disp_dim_timeout = gconf_value_get_int(gcv);
        mdy_gconf_sanitize_dim_timeouts(false);

        /* Reprogram blanking timers */
        mdy_blanking_rethink_timers(true);
    }
    else if (id == mdy_blanking_inhibit_mode_gconf_cb_id) {
        mdy_blanking_inhibit_mode = gconf_value_get_int(gcv);

        /* force blanking reprogramming */
        mdy_blanking_rethink_timers(true);
    }
    else if (id == mdy_kbd_slide_inhibit_mode_cb_id) {
        mdy_kbd_slide_inhibit_mode = gconf_value_get_int(gcv);
        /* force blanking reprogramming */
        mdy_blanking_rethink_timers(true);
    }
    else if( id == mdy_disp_never_blank_gconf_cb_id ) {
        mdy_disp_never_blank = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "never_blank = %d", mdy_disp_never_blank);
    }
    else if( id == mdy_compositor_core_delay_gconf_cb_id ) {
        mdy_compositor_core_delay = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "compositor kill delay = %d",
                mdy_compositor_core_delay);
    }
    else if( id == mdy_brightness_fade_duration_def_ms_gconf_cb_id ) {
        mdy_brightness_fade_duration_def_ms = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "fade duration / def = %d",
                mdy_brightness_fade_duration_def_ms);
    }
    else if( id == mdy_brightness_fade_duration_dim_ms_gconf_cb_id ) {
        mdy_brightness_fade_duration_dim_ms = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "fade duration / dim = %d",
                mdy_brightness_fade_duration_dim_ms);
    }
    else if( id == mdy_brightness_fade_duration_als_ms_gconf_cb_id ) {
        mdy_brightness_fade_duration_als_ms = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "fade duration / als = %d",
                mdy_brightness_fade_duration_als_ms);
    }
    else if( id == mdy_brightness_fade_duration_blank_ms_gconf_cb_id ) {
        mdy_brightness_fade_duration_blank_ms = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "fade duration / blank = %d",
                mdy_brightness_fade_duration_blank_ms);
    }
    else if( id == mdy_brightness_fade_duration_unblank_ms_gconf_cb_id ) {
        mdy_brightness_fade_duration_unblank_ms = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "fade duration / unblank = %d",
                mdy_brightness_fade_duration_unblank_ms);
    }
    else if( id == mdy_dbus_display_off_override_gconf_cb_id ) {
        mdy_dbus_display_off_override = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "display off override = %d",
                mdy_dbus_display_off_override);
    }

    else if( id == mdy_blanking_pause_mode_gconf_cb_id ) {
        gint old = mdy_blanking_pause_mode;
        mdy_blanking_pause_mode = gconf_value_get_int(gcv);

        mce_log(LL_NOTICE, "blanking pause mode = %s",
                blanking_pause_mode_repr(mdy_blanking_pause_mode));

        if( mdy_blanking_pause_mode == old ) {
            /* nop */
        }
        else if( mdy_blanking_pause_is_allowed() ) {
            /* Reprogram dim timer as needed when toggling between
             * keep-on and allow-dimming modes.
             *
             * Note that re-enabling after disable means that active
             * client side renew sessions are out of sync and hiccups
             * can occur (i.e. display can blank once after enabling).
             */
            mdy_blanking_rethink_timers(true);
        }
        else {
            /* Flush any active sessions there might be and reprogram
             * display off timer as needed. */
            mdy_blanking_remove_pause_clients();
        }
    }
    else if (id == mdy_orientation_sensor_enabled_gconf_cb_id) {
        mdy_orientation_sensor_enabled = gconf_value_get_bool(gcv);
        mdy_orientation_sensor_rethink();
    }
    else if (id == mdy_flipover_gesture_enabled_gconf_cb_id) {
        mdy_flipover_gesture_enabled = gconf_value_get_bool(gcv);
        mdy_orientation_sensor_rethink();
    }
    else if (id == mdy_orientation_change_is_activity_gconf_cb_id) {
        mdy_orientation_change_is_activity = gconf_value_get_bool(gcv);
        mdy_orientation_sensor_rethink();
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }

EXIT:
    return;
}

static void mdy_gconf_sanitize_brightness_settings(void)
{
    /* During settings reset operation all affected settings are
     * first then change notifications are emitted one by one.
     *
     * This means the locally cached values do not get updated
     * simultaneously via notification callbacks. But we can
     * explicitly update all three brightness related settings
     * which allows us to see all three values change at once
     * already when the first notification is received (and the
     * possible notifications for other two values do not cause
     * any further activity). */

    mce_gconf_get_int(MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_COUNT,
                      &mdy_brightness_step_count);
    mce_gconf_get_int(MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_SIZE,
                      &mdy_brightness_step_size);
    mce_gconf_get_int(MCE_GCONF_DISPLAY_BRIGHTNESS,
                      &mdy_brightness_setting);

    /* Migrate configuration ranges */
    if( mdy_brightness_step_count == 5 && mdy_brightness_step_size == 1 ) {
        /* Legacy 5 step control -> convert to percentage */
        mdy_brightness_step_count = 100;
        mdy_brightness_step_size  = 1;
        mdy_brightness_setting    = 20 * mdy_brightness_setting;
    }
    else if( mdy_brightness_step_count != 100 || mdy_brightness_step_size != 1 ) {
        /* Unsupported config -> force to 60 percent */
        mdy_brightness_step_count = 100;
        mdy_brightness_step_size  = 1;
        mdy_brightness_setting    = 60;
    }

    /* Clip brightness to supported range */
    if( mdy_brightness_setting > 100 )
        mdy_brightness_setting = 100;
    else if( mdy_brightness_setting < 1 )
        mdy_brightness_setting = 1;

    /* Clip dimmed brightness settings to supported range */
    mdy_brightness_dim_static =
        mce_clip_int(1, 100, mdy_brightness_dim_static);

    mdy_brightness_dim_dynamic =
        mce_clip_int(1, 100, mdy_brightness_dim_dynamic);

    mdy_brightness_dim_compositor_lo =
        mce_clip_int(0, 100, mdy_brightness_dim_compositor_lo);

    mdy_brightness_dim_compositor_hi =
        mce_clip_int(0, 100, mdy_brightness_dim_compositor_hi);

    /* Update config; signals will be emitted and config notifiers
     * called - mdy_gconf_cb() must ignore no-change notifications
     * to avoid recursive sanitation. */
    mce_gconf_set_int(MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_SIZE,
                      mdy_brightness_step_size);
    mce_gconf_set_int(MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_COUNT,
                      mdy_brightness_step_count);
    mce_gconf_set_int(MCE_GCONF_DISPLAY_BRIGHTNESS,
                      mdy_brightness_setting);

    mce_log(LL_DEBUG, "mdy_brightness_setting=%d", mdy_brightness_setting);

    mce_gconf_set_int(MCE_GCONF_DISPLAY_DIM_STATIC_BRIGHTNESS,
                      mdy_brightness_dim_static);
    mce_gconf_set_int(MCE_GCONF_DISPLAY_DIM_DYNAMIC_BRIGHTNESS,
                      mdy_brightness_dim_dynamic);
    mce_gconf_set_int(MCE_GCONF_DISPLAY_DIM_COMPOSITOR_LO,
                      mdy_brightness_dim_compositor_lo);
    mce_gconf_set_int(MCE_GCONF_DISPLAY_DIM_COMPOSITOR_HI,
                      mdy_brightness_dim_compositor_hi);

    mce_log(LL_DEBUG, "mdy_brightness_dim_static=%d",
            mdy_brightness_dim_static);
    mce_log(LL_DEBUG, "mdy_brightness_dim_dynamic=%d",
            mdy_brightness_dim_dynamic);
    mce_log(LL_DEBUG, "mdy_brightness_dim_compositor_lo=%d",
            mdy_brightness_dim_compositor_lo);
    mce_log(LL_DEBUG, "mdy_brightness_dim_compositor_hi=%d",
            mdy_brightness_dim_compositor_hi);

    /* Then execute through the brightness pipe too; this will update
     * the mdy_brightness_level_display_on & mdy_brightness_level_display_dim
     * values. */
    execute_datapipe(&display_brightness_pipe,
                     GINT_TO_POINTER(mdy_brightness_setting),
                     USE_INDATA, CACHE_INDATA);

    mce_log(LL_DEBUG, "mdy_brightness_level_display_on = %d",
            mdy_brightness_level_display_on);
    mce_log(LL_DEBUG, "mdy_brightness_level_display_dim = %d",
            mdy_brightness_level_display_dim);

    /* And drive the display brightness setting value through lpm datapipe
     * too. This will update the mdy_brightness_level_display_lpm value. */
    execute_datapipe(&lpm_brightness_pipe,
                     GINT_TO_POINTER(mdy_brightness_setting),
                     USE_INDATA, CACHE_INDATA);
}

static void mdy_gconf_sanitize_dim_timeouts(bool force_update)
{
    /* If asked to, flush existing list of allowed timeouts */
    if( force_update && mdy_possible_dim_timeouts ) {
        g_slist_free(mdy_possible_dim_timeouts),
            mdy_possible_dim_timeouts = 0;
    }

    /* Make sure we have a list of allowed timeouts */
    if( !mdy_possible_dim_timeouts ) {
        mce_gconf_get_int_list(MCE_GCONF_DISPLAY_DIM_TIMEOUT_LIST,
                               &mdy_possible_dim_timeouts);
    }

    if( !mdy_possible_dim_timeouts ) {
        static const int def[] = { DEFAULT_DISPLAY_DIM_TIMEOUT_LIST };

        GSList *tmp = 0;
        for( size_t i = 0; i < G_N_ELEMENTS(def); ++i )
            tmp = g_slist_prepend(tmp, GINT_TO_POINTER(def[i]));
        mdy_possible_dim_timeouts = g_slist_reverse(tmp);
    }

    /* Find the closest match in the list of valid dim timeouts */
    mdy_dim_timeout_index = mdy_blanking_find_dim_timeout_index(mdy_disp_dim_timeout);

    /* Reset adaptive dimming state */
    mdy_adaptive_dimming_index = 0;

    /* Update inactivity timeout */
    mdy_blanking_update_inactivity_timeout();
}

/** Get initial gconf valus and start tracking changes
 */
static void mdy_gconf_init(void)
{
    /* Display brightness settings */

    mce_gconf_track_int(MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_COUNT,
                        &mdy_brightness_step_count,
                        DEFAULT_DISP_BRIGHTNESS_STEP_COUNT,
                        mdy_gconf_cb,
                        &mdy_brightness_step_count_gconf_id);

    mce_gconf_track_int(MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_SIZE,
                        &mdy_brightness_step_size,
                        DEFAULT_DISP_BRIGHTNESS_STEP_SIZE,
                        mdy_gconf_cb,
                        &mdy_brightness_step_size_gconf_id);

    mce_gconf_track_int(MCE_GCONF_DISPLAY_BRIGHTNESS,
                        &mdy_brightness_setting,
                        DEFAULT_DISP_BRIGHTNESS,
                        mdy_gconf_cb,
                        &mdy_brightness_setting_gconf_id);

    mce_gconf_track_int(MCE_GCONF_DISPLAY_DIM_STATIC_BRIGHTNESS,
                        &mdy_brightness_dim_static,
                        DEFAULT_DISPLAY_DIM_STATIC_BRIGHTNESS,
                        mdy_gconf_cb,
                        &mdy_brightness_dim_static_gconf_id);

    mce_gconf_track_int(MCE_GCONF_DISPLAY_DIM_DYNAMIC_BRIGHTNESS,
                        &mdy_brightness_dim_dynamic,
                        DEFAULT_DISPLAY_DIM_DYNAMIC_BRIGHTNESS,
                        mdy_gconf_cb,
                        &mdy_brightness_dim_dynamic_gconf_id);

    mce_gconf_track_int(MCE_GCONF_DISPLAY_DIM_COMPOSITOR_LO,
                        &mdy_brightness_dim_compositor_lo,
                        DEFAULT_DISPLAY_DIM_COMPOSITOR_LO,
                        mdy_gconf_cb,
                        &mdy_brightness_dim_compositor_lo_gconf_id);

    mce_gconf_track_int(MCE_GCONF_DISPLAY_DIM_COMPOSITOR_HI,
                        &mdy_brightness_dim_compositor_hi,
                        DEFAULT_DISPLAY_DIM_COMPOSITOR_HI,
                        mdy_gconf_cb,
                        &mdy_brightness_dim_compositor_hi_gconf_id);

    /* Note: We're only interested in auto-brightness change
     *       notifications. The value itself is handled in
     *       filter-brightness-als plugin. */
    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_DISPLAY_ALS_ENABLED,
                           mdy_gconf_cb,
                           &mdy_automatic_brightness_setting_gconf_id);

    /* Migrate ranges, update hw dim/on brightness levels */
    mdy_gconf_sanitize_brightness_settings();

    /* Display blank timeout */
    mce_gconf_track_int(MCE_GCONF_DISPLAY_BLANK_TIMEOUT,
                        &mdy_blank_timeout,
                        DEFAULT_BLANK_TIMEOUT,
                        mdy_gconf_cb,
                        &mdy_blank_timeout_gconf_cb_id);

    /* Display blank from lockscreen timeout */
    mce_gconf_track_int(MCE_GCONF_DISPLAY_BLANK_FROM_LOCKSCREEN_TIMEOUT,
                        &mdy_blank_from_lockscreen_timeout,
                        DEFAULT_BLANK_FROM_LOCKSCREEN_TIMEOUT,
                        mdy_gconf_cb,
                        &mdy_blank_from_lockscreen_timeout_gconf_cb_id);

    /* Display blank from lpm on timeout */
    mce_gconf_track_int(MCE_GCONF_DISPLAY_BLANK_FROM_LPM_ON_TIMEOUT,
                        &mdy_blank_from_lpm_on_timeout,
                        DEFAULT_BLANK_FROM_LPM_ON_TIMEOUT,
                        mdy_gconf_cb,
                        &mdy_blank_from_lpm_on_timeout_gconf_cb_id);

    /* Display blank from lpm off timeout */
    mce_gconf_track_int(MCE_GCONF_DISPLAY_BLANK_FROM_LPM_OFF_TIMEOUT,
                        &mdy_blank_from_lpm_off_timeout,
                        DEFAULT_BLANK_FROM_LPM_OFF_TIMEOUT,
                        mdy_gconf_cb,
                        &mdy_blank_from_lpm_off_timeout_gconf_cb_id);

    /* Never blank toggle */
    mce_gconf_track_int(MCE_GCONF_DISPLAY_NEVER_BLANK,
                        &mdy_disp_never_blank,
                        DEFAULT_DISPLAY_NEVER_BLANK,
                        mdy_gconf_cb,
                        &mdy_disp_never_blank_gconf_cb_id);

    /* Use adaptive display dim timeout toggle */
    mce_gconf_track_bool(MCE_GCONF_DISPLAY_ADAPTIVE_DIMMING,
                         &mdy_adaptive_dimming_enabled,
                         DEFAULT_ADAPTIVE_DIMMING_ENABLED,
                         mdy_gconf_cb,
                         &mdy_adaptive_dimming_enabled_gconf_cb_id);

    /* Adaptive display dimming threshold timer */
    mce_gconf_track_int(MCE_GCONF_DISPLAY_ADAPTIVE_DIM_THRESHOLD,
                        &mdy_adaptive_dimming_threshold,
                        DEFAULT_ADAPTIVE_DIMMING_THRESHOLD,
                        mdy_gconf_cb,
                        &mdy_adaptive_dimming_threshold_gconf_cb_id);

    /* Display dim timer */
    mce_gconf_track_int(MCE_GCONF_DISPLAY_DIM_TIMEOUT,
                        &mdy_disp_dim_timeout,
                        DEFAULT_DIM_TIMEOUT,
                        mdy_gconf_cb,
                        &mdy_disp_dim_timeout_gconf_cb_id);

    /* Possible dim timeouts */
    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_DISPLAY_DIM_TIMEOUT_LIST,
                           mdy_gconf_cb,
                           &mdy_possible_dim_timeouts_gconf_cb_id);

    /* After all blanking and dimming settings are fetched, we
     * need to sanitize them and calculate inactivity timeout */
    mdy_gconf_sanitize_dim_timeouts(true);

    /* Use low power mode toggle */
    mce_gconf_track_bool(MCE_GCONF_USE_LOW_POWER_MODE,
                         &mdy_use_low_power_mode,
                         DEFAULT_USE_LOW_POWER_MODE,
                         mdy_gconf_cb,
                         &mdy_use_low_power_mode_gconf_cb_id);

    /* Blanking inhibit modes */
    mce_gconf_track_int(MCE_GCONF_BLANKING_INHIBIT_MODE,
                        &mdy_blanking_inhibit_mode,
                        DEFAULT_BLANKING_INHIBIT_MODE,
                        mdy_gconf_cb,
                        &mdy_blanking_inhibit_mode_gconf_cb_id);

    mce_gconf_track_int(MCE_GCONF_KBD_SLIDE_INHIBIT,
                        &mdy_kbd_slide_inhibit_mode,
                        DEFAULT_KBD_SLIDE_INHIBIT,
                        mdy_gconf_cb,
                        &mdy_kbd_slide_inhibit_mode_cb_id);

    /* Delay for killing unresponsive compositor */
    mce_gconf_track_int(MCE_GCONF_LIPSTICK_CORE_DELAY,
                        &mdy_compositor_core_delay,
                        DEFAULT_LIPSTICK_CORE_DELAY,
                        mdy_gconf_cb,
                        &mdy_compositor_core_delay_gconf_cb_id);

    /* Brightness fade length: default */
    mce_gconf_track_int(MCE_GCONF_BRIGHTNESS_FADE_DEFAULT_MS,
                        &mdy_brightness_fade_duration_def_ms,
                        DEFAULT_BRIGHTNESS_FADE_DEFAULT_MS,
                        mdy_gconf_cb,
                        &mdy_brightness_fade_duration_def_ms_gconf_cb_id);

    /* Brightness fade length: dim */
    mce_gconf_track_int(MCE_GCONF_BRIGHTNESS_FADE_DIMMING_MS,
                        &mdy_brightness_fade_duration_dim_ms,
                        DEFAULT_BRIGHTNESS_FADE_DIMMING_MS,
                        mdy_gconf_cb,
                        &mdy_brightness_fade_duration_dim_ms_gconf_cb_id);

    /* Brightness fade length: als */
    mce_gconf_track_int(MCE_GCONF_BRIGHTNESS_FADE_ALS_MS,
                        &mdy_brightness_fade_duration_als_ms,
                        DEFAULT_BRIGHTNESS_FADE_ALS_MS,
                        mdy_gconf_cb,
                        &mdy_brightness_fade_duration_als_ms_gconf_cb_id);

    /* Brightness fade length: blank */
    mce_gconf_track_int(MCE_GCONF_BRIGHTNESS_FADE_BLANK_MS,
                        &mdy_brightness_fade_duration_blank_ms,
                        DEFAULT_BRIGHTNESS_FADE_BLANK_MS,
                        mdy_gconf_cb,
                        &mdy_brightness_fade_duration_blank_ms_gconf_cb_id);

    /* Brightness fade length: unblank */
    mce_gconf_track_int(MCE_GCONF_BRIGHTNESS_FADE_UNBLANK_MS,
                        &mdy_brightness_fade_duration_unblank_ms,
                        DEFAULT_BRIGHTNESS_FADE_UNBLANK_MS,
                        mdy_gconf_cb,
                        &mdy_brightness_fade_duration_unblank_ms_gconf_cb_id);

    /* Override mode for display off requests made over D-Bus */
    mce_gconf_track_int(MCE_GCONF_DISPLAY_OFF_OVERRIDE,
                        &mdy_dbus_display_off_override,
                        DEFAULT_DISPLAY_OFF_OVERRIDE,
                        mdy_gconf_cb,
                        &mdy_dbus_display_off_override_gconf_cb_id);

    /* Use orientation sensor */
    mce_gconf_track_bool(MCE_GCONF_ORIENTATION_SENSOR_ENABLED,
                         &mdy_orientation_sensor_enabled,
                         DEFAULT_ORIENTATION_SENSOR_ENABLED,
                         mdy_gconf_cb,
                         &mdy_orientation_sensor_enabled_gconf_cb_id);

    mce_gconf_track_bool(MCE_GCONF_FLIPOVER_GESTURE_ENABLED,
                         &mdy_flipover_gesture_enabled,
                         DEFAULT_FLIPOVER_GESTURE_ENABLED,
                         mdy_gconf_cb,
                         &mdy_flipover_gesture_enabled_gconf_cb_id);

    mce_gconf_track_bool(MCE_GCONF_ORIENTATION_CHANGE_IS_ACTIVITY,
                         &mdy_orientation_change_is_activity,
                         DEFAULT_ORIENTATION_CHANGE_IS_ACTIVITY,
                         mdy_gconf_cb,
                         &mdy_orientation_change_is_activity_gconf_cb_id);

    /* Blanking pause mode */
    mce_gconf_track_int(MCE_GCONF_DISPLAY_BLANKING_PAUSE_MODE,
                        &mdy_blanking_pause_mode,
                        DEFAULT_BLANKING_PAUSE_MODE,
                        mdy_gconf_cb,
                        &mdy_blanking_pause_mode_gconf_cb_id);
}

static void mdy_gconf_quit(void)
{
    /* Remove config change notifiers */

    mce_gconf_notifier_remove(mdy_brightness_step_count_gconf_id),
        mdy_brightness_step_count_gconf_id = 0;

    mce_gconf_notifier_remove(mdy_brightness_step_size_gconf_id),
        mdy_brightness_step_size_gconf_id = 0;

    mce_gconf_notifier_remove(mdy_brightness_setting_gconf_id),
        mdy_brightness_setting_gconf_id = 0;

    mce_gconf_notifier_remove(mdy_brightness_dim_static_gconf_id),
        mdy_brightness_dim_static_gconf_id = 0;

    mce_gconf_notifier_remove(mdy_brightness_dim_dynamic_gconf_id),
        mdy_brightness_dim_dynamic_gconf_id = 0;

    mce_gconf_notifier_remove(mdy_brightness_dim_compositor_lo_gconf_id),
        mdy_brightness_dim_compositor_lo_gconf_id = 0;

    mce_gconf_notifier_remove(mdy_brightness_dim_compositor_hi_gconf_id),
        mdy_brightness_dim_compositor_hi_gconf_id = 0;

    mce_gconf_notifier_remove(mdy_automatic_brightness_setting_gconf_id),
        mdy_automatic_brightness_setting_gconf_id = 0;

    mce_gconf_notifier_remove(mdy_blank_timeout_gconf_cb_id),
        mdy_blank_timeout_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_blank_from_lockscreen_timeout_gconf_cb_id),
        mdy_blank_from_lockscreen_timeout_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_blank_from_lpm_on_timeout_gconf_cb_id),
        mdy_blank_from_lpm_on_timeout_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_blank_from_lpm_off_timeout_gconf_cb_id),
        mdy_blank_from_lpm_off_timeout_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_disp_never_blank_gconf_cb_id),
        mdy_disp_never_blank_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_adaptive_dimming_enabled_gconf_cb_id),
        mdy_adaptive_dimming_enabled_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_adaptive_dimming_threshold_gconf_cb_id),
        mdy_adaptive_dimming_threshold_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_disp_dim_timeout_gconf_cb_id),
        mdy_disp_dim_timeout_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_possible_dim_timeouts_gconf_cb_id),
        mdy_possible_dim_timeouts_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_use_low_power_mode_gconf_cb_id),
        mdy_use_low_power_mode_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_blanking_inhibit_mode_gconf_cb_id),
        mdy_blanking_inhibit_mode_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_kbd_slide_inhibit_mode_cb_id),
        mdy_kbd_slide_inhibit_mode_cb_id = 0;

    mce_gconf_notifier_remove(mdy_compositor_core_delay_gconf_cb_id),
        mdy_compositor_core_delay_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_brightness_fade_duration_def_ms_gconf_cb_id),
        mdy_brightness_fade_duration_def_ms_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_brightness_fade_duration_dim_ms_gconf_cb_id),
        mdy_brightness_fade_duration_dim_ms_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_brightness_fade_duration_als_ms_gconf_cb_id),
        mdy_brightness_fade_duration_als_ms_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_brightness_fade_duration_blank_ms_gconf_cb_id),
        mdy_brightness_fade_duration_blank_ms_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_brightness_fade_duration_unblank_ms_gconf_cb_id),
        mdy_brightness_fade_duration_unblank_ms_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_dbus_display_off_override_gconf_cb_id),
        mdy_dbus_display_off_override_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_orientation_sensor_enabled_gconf_cb_id),
        mdy_orientation_sensor_enabled_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_flipover_gesture_enabled_gconf_cb_id),
        mdy_flipover_gesture_enabled_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_orientation_change_is_activity_gconf_cb_id),
        mdy_orientation_change_is_activity_gconf_cb_id = 0;

    mce_gconf_notifier_remove(mdy_blanking_pause_mode_gconf_cb_id),
        mdy_blanking_pause_mode_gconf_cb_id = 0;

    /* Free dynamic data obtained from config */

    g_slist_free(mdy_possible_dim_timeouts), mdy_possible_dim_timeouts = 0;
}

/* ========================================================================= *
 * MODULE_LOAD_UNLOAD
 * ========================================================================= */

/** Probe maximum and current backlight brightness from sysfs
 */
static void mdy_brightness_init(void)
{
    gulong tmp = 0;

    /* If possible, obtain maximum brightness level */
    if( !mdy_brightness_level_maximum_path ) {
        mce_log(LL_NOTICE, "No path for maximum brightness file; "
                "defaulting to %d",
                mdy_brightness_level_maximum);
    }
    else if( !mce_read_number_string_from_file(mdy_brightness_level_maximum_path,
                                               &tmp, NULL, FALSE, TRUE) ) {
        mce_log(LL_ERR, "Could not read the maximum brightness from %s; "
                "defaulting to %d",
                mdy_brightness_level_maximum_path, mdy_brightness_level_maximum);
    }
    else
        mdy_brightness_level_maximum = (gint)tmp;

    mce_log(LL_DEBUG, "max_brightness = %d", mdy_brightness_level_maximum);

    /* If we can read the current hw brightness level, update the
     * cached brightness so we can do soft transitions from the
     * initial state */
    if( mdy_brightness_level_output.path &&
        mce_read_number_string_from_file(mdy_brightness_level_output.path,
                                              &tmp, NULL, FALSE, TRUE) ) {
        mdy_brightness_level_cached = (gint)tmp;
    }
    mce_log(LL_DEBUG, "mdy_brightness_level_cached=%d",
            mdy_brightness_level_cached);

    /* On some devices there are multiple ways to control backlight
     * brightness. We use only one, but after bootup it might contain
     * a value that does not match the reality.
     *
     * The likely scenario is something like:
     *   lcd-backlight/brightness = 255 (incorrect)
     *   wled/brightness          =  64 (correct)
     *
     * Which leads - if using manual/100% brightness - mce not to
     * update the brightness because it already is supposed to
     * be at 255.
     *
     * Using "reported_by_kernel minus one" as mce cached value
     * would make mce to update the sysfs value later on, but then
     * the kernel can ignore it because it sees no change.
     *
     * But by writing the off-by-one value to sysfs:
     * a) we're still close to the reported value in case it happened
     *    to be correct (after mce restart)
     * b) the kernel side sees at least one brightness change even if
     *    the brightness setting evaluation would lead to the same
     *    value that was originally reported
     */
    if( mdy_brightness_level_cached > 0 )
        mdy_brightness_force_level(mdy_brightness_level_cached - 1);
}

/**
 * Init function for the display handling module
 *
 * @param module Unused
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *g_module_check_init(GModule *module)
{
    const gchar *failure = 0;

    gboolean display_is_on = TRUE;

    (void)module;

    /* Initialise the display type and the relevant paths */
    (void)mdy_display_type_get();

#ifdef ENABLE_CPU_GOVERNOR
    /* Get CPU scaling governor settings from INI-files */
    mdy_governor_default = mdy_governor_get_settings("Default");
    mdy_governor_interactive = mdy_governor_get_settings("Interactive");

    /* Get cpu scaling governor configuration & track changes */
    mce_gconf_track_int(MCE_GCONF_CPU_SCALING_GOVERNOR,
                        &mdy_governor_conf,
                        GOVERNOR_UNSET,
                        mdy_governor_conf_cb,
                        &mdy_governor_conf_id);

    /* Evaluate initial state */
    mdy_governor_rethink();
#endif

#ifdef ENABLE_WAKELOCKS
    /* Get autosuspend policy configuration & track changes */
    mce_gconf_track_int(MCE_GCONF_USE_AUTOSUSPEND,
                        &mdy_suspend_policy,
                        SUSPEND_POLICY_DEFAULT,
                        mdy_autosuspend_gconf_cb,
                        &mdy_suspend_policy_id);

    /* Evaluate initial state */
    mdy_stm_schedule_rethink();
#endif

    /* Start waiting for init_done state */
    mdy_flagfiles_start_tracking();

    /* Append triggers/filters to datapipes */
    mdy_datapipe_init();

    /* Install dbus message handlers */
    mdy_dbus_init();

    /* Probe maximum and current backlight brightness from sysfs */
    mdy_brightness_init();

    /* Get initial gconf valus and start tracking changes */
    mdy_gconf_init();

    mdy_cabc_mode_set(DEFAULT_CABC_MODE);

    /* if we have brightness control file and initial brightness
     * is zero -> start from display off */
    if( mdy_brightness_level_output.path &&
        mdy_brightness_level_cached <= 0 )
        display_is_on = FALSE;

    /* Note: Transition to MCE_DISPLAY_OFF can be made already
     * here, but the MCE_DISPLAY_ON state is blocked until mCE
     * gets notification from DSME */
    mce_log(LL_INFO, "initial display mode = %s",
            display_is_on ? "ON" : "OFF");
    execute_datapipe(&display_state_req_pipe,
                     GINT_TO_POINTER(display_is_on ?
                                     MCE_DISPLAY_ON :
                                     MCE_DISPLAY_OFF),
                     USE_INDATA, CACHE_INDATA);

    /* Start the framebuffer sleep/wakeup thread */
#ifdef ENABLE_WAKELOCKS
    mdy_waitfb_thread_start(&mdy_waitfb_data);
#endif

    /* Re-evaluate the power on LED state from idle callback
     * i.e. when the led plugin is loaded and operational */
    mdy_poweron_led_rethink_schedule();

    /* Evaluate initial orientation sensor enable state */
    mdy_orientation_sensor_rethink();

    /* Send initial blanking pause & inhibit states */
    mdy_dbus_send_blanking_pause_status(0);
    mdy_dbus_send_blanking_inhibit_status(0);

    return failure;
}

/**
 * Exit function for the display handling module
 *
 * @todo D-Bus unregistration
 *
 * @param module Unused
 */
void g_module_unload(GModule *module)
{
    (void)module;

    /* Mark down that we are unloading */
    mdy_unloading_module = TRUE;

    /* Kill the framebuffer sleep/wakeup thread */
#ifdef ENABLE_WAKELOCKS
    mdy_waitfb_thread_stop(&mdy_waitfb_data);
#endif

    /* Remove dbus message handlers */
    mdy_dbus_quit();

    /* Stop tracking gconf changes */
    mdy_gconf_quit();

    /* Stop waiting for init_done state */
    mdy_flagfiles_stop_tracking();

#ifdef ENABLE_WAKELOCKS
    /* Remove suspend policy change notifier */
    mce_gconf_notifier_remove(mdy_suspend_policy_id),
        mdy_suspend_policy_id = 0;
#endif

#ifdef ENABLE_CPU_GOVERNOR
    /* Remove cpu scaling governor change notifier */
    mce_gconf_notifier_remove(mdy_governor_conf_id),
        mdy_governor_conf_id = 0;

    /* Switch back to defaults */
    mdy_governor_rethink();

    /* Release CPU scaling governor settings from INI-files */
    mdy_governor_free_settings(mdy_governor_default),
        mdy_governor_default = 0;
    mdy_governor_free_settings(mdy_governor_interactive),
        mdy_governor_interactive = 0;
#endif

    /* Remove triggers/filters from datapipes */
    mdy_datapipe_quit();

    /* Close files */
    mce_close_output(&mdy_brightness_level_output);
    mce_close_output(&mdy_high_brightness_mode_output);

    /* Free strings */
    g_free((void*)mdy_brightness_level_output.path);
    g_free(mdy_brightness_level_maximum_path);
    g_free(mdy_cabc_mode_file);
    g_free(mdy_cabc_available_modes_file);
    g_free((void*)mdy_brightness_hw_fading_output.path);
    g_free((void*)mdy_high_brightness_mode_output.path);
    g_free(mdy_low_power_mode_file);

    /* Remove all timer sources */
    mdy_blanking_stop_pause_period();
    mdy_brightness_stop_fade_timer();
    mdy_blanking_cancel_dim();
    mdy_blanking_stop_adaptive_dimming();
    mdy_blanking_cancel_off();
    mdy_compositor_cancel_killer();
    mdy_callstate_clear_changed();
    mdy_blanking_inhibit_cancel_broadcast();

    /* Cancel active asynchronous dbus method calls to avoid
     * callback functions with stale adresses getting invoked */
    mdy_compositor_cancel_state_req();

    /* Cancel pending state machine updates */
    mdy_stm_cancel_rethink();

    mdy_poweron_led_rethink_cancel();

    /* Remove callbacks on module unload */
    mce_sensorfw_orient_set_notify(0);

    g_free(mdy_compositor_priv_name), mdy_compositor_priv_name = 0;

    /* If we are shutting down/rebooting and we have fbdev
     * open, create a detached child process to hold on to
     * it so that display does not power off after mce & ui
     * side have been terminated */
    if( mdy_shutdown_in_progress() && mce_fbdev_is_open() ) {
        /* Calculate when to release fbdev file descriptor
         *
         * use shutdown_started + 6.0 s
         */

        int delay_ms = (int)(mdy_shutdown_started_tick
                             + 6000
                             - mce_lib_get_boot_tick());
        mce_fbdev_linger_after_exit(delay_ms);
    }

    return;
}
