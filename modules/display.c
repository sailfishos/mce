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
#include <glib.h>
#include <gmodule.h>
#include <glib/gstdio.h>

#include <sys/time.h>
#include <pthread.h>
#include <stdlib.h>
#include <glob.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/fb.h>

#include <sys/ioctl.h>
#include <sys/ptrace.h>

#include <mce/mode-names.h>

#include "mce.h"
#include "display.h"
#include "mce-io.h"
#include "mce-lib.h"
#include "mce-log.h"
#include "mce-conf.h"
#include "mce-dbus.h"
#include "mce-gconf.h"
#include "mce-sensorfw.h"
#include "datapipe.h"

#include "../filewatcher.h"

#ifdef ENABLE_WAKELOCKS
# include "../libwakelock.h"
#endif

#ifdef ENABLE_HYBRIS
# include "../mce-hybris.h"
#endif

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

/** Define demo mode DBUS method */
#define MCE_DBUS_DEMO_MODE_REQ  "display_set_demo_mode"

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

/** Brightness change policies */
typedef enum {
    /** Policy not set */
    BRIGHTNESS_CHANGE_POLICY_INVALID = MCE_INVALID_TRANSLATION,
    /** Brightness changes instantly */
    BRIGHTNESS_CHANGE_DIRECT = 0,
    /** Fade with fixed step time */
    BRIGHTNESS_CHANGE_STEP_TIME = 1,
    /** Fade time independent of number of steps faded */
    BRIGHTNESS_CHANGE_CONSTANT_TIME = 2,
    /** Default setting when brightness increases */
    DEFAULT_BRIGHTNESS_INCREASE_POLICY = BRIGHTNESS_CHANGE_CONSTANT_TIME,
    /** Default setting when brightness decreases */
    DEFAULT_BRIGHTNESS_DECREASE_POLICY = BRIGHTNESS_CHANGE_CONSTANT_TIME
} brightness_change_policy_t;

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
    LED_DELAY_UI_DISABLE_ENABLE = 1500,
};

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * DATAPIPE_TRACKING
 * ------------------------------------------------------------------------- */

static void                mdy_datapipe_system_state_cb(gconstpointer data);
static void                mdy_datapipe_submode_cb(gconstpointer data);
static gpointer            mdy_datapipe_display_state_filter_cb(gpointer data);
static void                mdy_datapipe_display_state_cb(gconstpointer data);
static void                mdy_datapipe_display_brightness_cb(gconstpointer data);
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

static void                mdy_datapipe_init(void);
static void                mdy_datapipe_quit(void);

/* ------------------------------------------------------------------------- *
 * FBDEV_POWER_STATE
 * ------------------------------------------------------------------------- */

#ifdef ENABLE_HYBRIS
static void                mdy_fbdev_set_power_hybris(int value);
static void                mdy_fbdev_set_power_dummy(int value);
#endif
static void                mdy_fbdev_set_power_default(int value);
static void                mdy_fbdev_set_power(int value);

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

#ifdef ENABLE_HYBRIS
static void                mdy_brightness_set_level_hybris(int number);
#endif
static void                mdy_brightness_set_level_default(int number);
static void                mdy_brightness_set_level(int number);
static void                mdy_brightness_force_level(int number);

static gboolean            mdy_brightness_fade_timer_cb(gpointer data);
static void                mdy_brightness_stop_fade_timer(void);
static void                mdy_brightness_start_fade_timer(gint step_time);
static void                mdy_brightness_set_fade_target(gint new_brightness);
static void                mdy_brightness_set_on_level(gint hbm_and_level);

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

static guint               mdy_blanking_find_dim_timeout_index(gint dim_timeout);
static gboolean            mdy_blanking_can_blank_from_low_power_mode(void);

// display timer: ON -> DIM

static gboolean            mdy_blanking_dim_cb(gpointer data);
static void                mdy_blanking_cancel_dim(void);
static void                mdy_blanking_schedule_dim(void);

// display timer: DIM -> OFF

static gboolean            mdy_blanking_off_cb(gpointer data);
static void                mdy_blanking_cancel_off(void);
static void                mdy_blanking_schedule_off(void);

// display timer: DIM -> LPM_ON

static gboolean            mdy_blanking_lpm_on_cb(gpointer data);
static void                mdy_blanking_cancel_lpm_on(void);
static void                mdy_blanking_schedule_lpm_on(void);

// display timer: LPM_ON -> LPM_OFF (when proximity covered)

static gboolean            mdy_blanking_lpm_off_cb(gpointer data);
static void                mdy_blanking_cancel_lpm_off(void);
static void                mdy_blanking_schedule_lpm_off(void);

// blanking pause period: inhibit automatic ON -> DIM transitions

static gboolean            mdy_blanking_pause_period_cb(gpointer data);
static void                mdy_blanking_stop_pause_period(void);
static void                mdy_blanking_start_pause_period(void);

static bool                mdy_blanking_is_paused(void);

static void                mdy_blanking_add_pause_client(const gchar *name);
static gboolean            mdy_blanking_remove_pause_client(const gchar *name);
static void                mdy_blanking_remove_pause_clients(void);
static gboolean            mdy_blanking_pause_client_lost_cb(DBusMessage *const msg);

// adaptive dimming period: dimming timeouts get longer on ON->DIM->ON transitions

static gboolean            mdy_blanking_adaptive_dimming_cb(gpointer data);
static void                mdy_blanking_start_adaptive_dimming(void);
static void                mdy_blanking_stop_adaptive_dimming(void);

// display timer: all of em
static void                mdy_blanking_rethink_timers(bool force);
static void                mdy_blanking_rethink_proximity(void);
static void                mdy_blanking_cancel_timers(void);

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
 * LIPSTICK_KILLER
 * ------------------------------------------------------------------------- */

static void                mdy_lipstick_killer_enable_led(bool enable);
static gboolean            mdy_lipstick_killer_verify_cb(gpointer aptr);
static gboolean            mdy_lipstick_killer_kill_cb(gpointer aptr);
static gboolean            mdy_lipstick_killer_core_cb(gpointer aptr);
static void                mdy_lipstick_killer_schedule(void);
static void                mdy_lipstick_killer_cancel(void);

/* ------------------------------------------------------------------------- *
 * RENDERING_ENABLE_DISABLE
 * ------------------------------------------------------------------------- */

static void                mdy_renderer_led_set(renderer_state_t req);
static gboolean            mdy_renderer_led_timer_cb(gpointer aptr);
static void                mdy_renderer_led_cancel_timer(void);
static void                mdy_renderer_led_start_timer(renderer_state_t req);

static void                mdy_renderer_set_state_cb(DBusPendingCall *pending, void *user_data);
static void                mdy_renderer_cancel_state_set(void);
static gboolean            mdy_renderer_set_state_req(renderer_state_t state);

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

/* ------------------------------------------------------------------------- *
 * DISPLAY_STATE
 * ------------------------------------------------------------------------- */

static void                mdy_display_state_enter_post(void);
static void                mdy_display_state_enter_pre(display_state_t prev_state, display_state_t next_state);
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
static const char         *mdy_display_state_name(display_state_t state);

// react to systemui availability changes
static void mdy_stm_lipstick_name_owner_changed(const char *name, const char *prev, const char *curr);

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
static bool                mdy_stm_is_target_changing(void);
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
 * DBUS_NAME_OWNER_TRACKING
 * ------------------------------------------------------------------------- */

static void                mdy_nameowner_changed(const char *name, const char *prev, const char *curr);
static DBusHandlerResult   mdy_nameowner_filter_cb(DBusConnection *con, DBusMessage *msg, void *user_data);
static void                mdy_nameowner_query_rsp(DBusPendingCall *pending, void *user_data);
static void                mdy_nameowner_query_req(const char *name);
static char               *mdy_nameowner_watch(const char *name);
static void                mdy_nameowner_unwatch(char *rule);
static void                mdy_nameowner_init(void);
static void                mdy_nameowner_quit(void);

/* ------------------------------------------------------------------------- *
 * DBUS_HANDLERS
 * ------------------------------------------------------------------------- */

static gboolean            mdy_dbus_send_display_status(DBusMessage *const method_call);
static const char         *mdy_dbus_get_reason_to_block_display_on(void);

static gboolean            mdy_dbus_handle_display_on_req(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_display_dim_req(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_display_off_req(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_display_status_get_req(DBusMessage *const msg);

static gboolean            mdy_dbus_send_cabc_mode(DBusMessage *const method_call);
static gboolean            mdy_dbus_handle_cabc_mode_owner_lost_sig(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_cabc_mode_get_req(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_cabc_mode_set_req(DBusMessage *const msg);

static gboolean            mdy_dbus_handle_blanking_pause_start_req(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_blanking_pause_cancel_req(DBusMessage *const msg);

static gboolean            mdy_dbus_handle_set_demo_mode_req(DBusMessage *const msg);

static gboolean            mdy_dbus_handle_desktop_started_sig(DBusMessage *const msg);
static gboolean            mdy_dbus_handle_shutdown_started_sig(DBusMessage *const msg);

static void                mdy_dbus_init(void);
static void                mdy_dbus_quit(void);

/* ------------------------------------------------------------------------- *
 * FLAG_FILE_TRACKING
 * ------------------------------------------------------------------------- */

static gboolean            mdy_flagfiles_desktop_ready_cb(gpointer user_data);
static void                mdy_flagfiles_bootstate_cb(const char *path, const char *file, gpointer data);
static void                mdy_flagfiles_init_done_cb(const char *path, const char *file, gpointer data);
static void                mdy_flagfiles_start_tracking(void);
static void                mdy_flagfiles_stop_tracking(void);

/* ------------------------------------------------------------------------- *
 * GCONF_SETTINGS
 * ------------------------------------------------------------------------- */

static void                mdy_gconf_cb(GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);
static void                mdy_gconf_init(void);
static void                mdy_gconf_quit(void);
static void                mdy_gconf_sanitize_brightness_settings(void);

/* ------------------------------------------------------------------------- *
 * INIFILE_SETTINGS
 * ------------------------------------------------------------------------- */

static void                mdy_config_init(void);

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
 * MISCELLANEOUS
 * ------------------------------------------------------------------------- */

/** Have we seen shutdown_ind signal from dsme */
static gboolean mdy_shutdown_started = FALSE;

/** Are we already unloading the module? */
static gboolean mdy_unloading_module = FALSE;

/** Timer for waiting simulated desktop ready state */
static guint mdy_desktop_ready_id = 0;

/* ------------------------------------------------------------------------- *
 * AUTOMATIC_BLANKING
 * ------------------------------------------------------------------------- */

/** ID for adaptive display dimming timer source */
static guint mdy_blanking_adaptive_dimming_cb_id = 0;

/** Index for the array of adaptive dimming timeout multipliers */
static guint mdy_adaptive_dimming_index = 0;

/** Display blank timeout setting when low power mode is supported */
static gint mdy_disp_lpm_off_timeout = DEFAULT_LPM_BLANK_TIMEOUT;

/** Display low power mode timeout setting */
static gint mdy_disp_lpm_on_timeout = DEFAULT_BLANK_TIMEOUT;

/** Display blank prevention timer */
static gint mdy_blank_prevent_timeout = BLANK_PREVENT_TIMEOUT;

/** Bootup dim additional timeout */
static gint mdy_additional_bootup_dim_timeout = 0;

/** File used to enable low power mode */
static gchar *mdy_low_power_mode_file = NULL;

/** Is display low power mode supported */
static gboolean mdy_low_power_mode_supported = FALSE;

/** Mapping of brightness change integer <-> policy string */
static const mce_translation_t mdy_brightness_change_policy_translation[] = {
    {
        .number = BRIGHTNESS_CHANGE_DIRECT,
        .string = "direct",
    },
    {
        .number = BRIGHTNESS_CHANGE_STEP_TIME,
        .string = "steptime",
    },
    {
        .number = BRIGHTNESS_CHANGE_CONSTANT_TIME,
        .string = "constanttime",
    },
    { /* MCE_INVALID_TRANSLATION marks the end of this array */
        .number = MCE_INVALID_TRANSLATION,
        .string = NULL
    }
};

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

/* ------------------------------------------------------------------------- *
 * GCONF_SETTINGS
 * ------------------------------------------------------------------------- */

/** Display blanking timeout setting */
static gint mdy_disp_blank_timeout = DEFAULT_BLANK_TIMEOUT;

/** GConf callback ID for mdy_disp_blank_timeout */
static guint mdy_disp_blank_timeout_gconf_cb_id = 0;

/** Number of brightness steps */
static gint mdy_brightness_step_count = DEFAULT_DISP_BRIGHTNESS_STEP_COUNT;

/** Size of one brightness step */
static gint mdy_brightness_step_size  = DEFAULT_DISP_BRIGHTNESS_STEP_SIZE;

/** display brightness setting; [1, mdy_brightness_step_count] */
static gint mdy_brightness_setting = DEFAULT_DISP_BRIGHTNESS;

/** GConf callback ID for mdy_brightness_step_count */
static guint mdy_brightness_step_count_gconf_id = 0;

/** GConf callback ID for mdy_brightness_step_size */
static guint mdy_brightness_step_size_gconf_id = 0;

/** GConf callback ID for mdy_brightness_setting */
static guint mdy_brightness_setting_gconf_id = 0;

/** PSM display brightness setting; [1, 5]
 *  or -1 when power save mode is not active
 *
 * (not in gconf, but kept close to mdy_brightness_setting)
 */
static gint mdy_psm_disp_brightness = -1;

/** Never blank display setting */
static gint mdy_disp_never_blank = 0;

/** GConf callback ID for display never blank setting */
static guint mdy_disp_never_blank_gconf_cb_id = 0;

/** Use adaptive timeouts for dimming */
static gboolean mdy_adaptive_dimming_enabled = DEFAULT_ADAPTIVE_DIMMING_ENABLED;

/** GConf callback ID for display blanking timeout setting */
static guint mdy_adaptive_dimming_enabled_gconf_cb_id = 0;

/** Array of possible display dim timeouts */
static GSList *mdy_possible_dim_timeouts = NULL;

/** Threshold to use for adaptive timeouts for dimming in milliseconds */
static gint mdy_adaptive_dimming_threshold = DEFAULT_ADAPTIVE_DIMMING_THRESHOLD;

/** GConf callback ID for the threshold for adaptive display dimming */
static guint mdy_adaptive_dimming_threshold_gconf_cb_id = 0;

/** Display dimming timeout setting */
static gint mdy_disp_dim_timeout = DEFAULT_DIM_TIMEOUT;

/** GConf callback ID for display dimming timeout setting */
static guint mdy_disp_dim_timeout_gconf_cb_id = 0;

/** Use low power mode setting */
static gboolean mdy_use_low_power_mode = FALSE;

/** GConf callback ID for low power mode setting */
static guint mdy_use_low_power_mode_gconf_cb_id = 0;

/** Display blanking inhibit mode */
static inhibit_t mdy_blanking_inhibit_mode = DEFAULT_BLANKING_INHIBIT_MODE;

/** GConf callback ID for display blanking inhibit mode setting */
static guint mdy_blanking_inhibit_mode_gconf_cb_id = 0;

/* ------------------------------------------------------------------------- *
 * INIFILE_SETTINGS
 * ------------------------------------------------------------------------- */

/** Brightness increase policy */
static brightness_change_policy_t mdy_brightness_increase_policy =
                                        DEFAULT_BRIGHTNESS_INCREASE_POLICY;

/** Brightness decrease policy */
static brightness_change_policy_t mdy_brightness_decrease_policy =
                                        DEFAULT_BRIGHTNESS_DECREASE_POLICY;

/** Brightness increase step-time */
static gint mdy_brightness_increase_step_time =
                DEFAULT_BRIGHTNESS_INCREASE_STEP_TIME;

/** Brightness decrease step-time */
static gint mdy_brightness_decrease_step_time =
                DEFAULT_BRIGHTNESS_DECREASE_STEP_TIME;

/** Brightness increase constant time */
static gint mdy_brightness_increase_constant_time =
                DEFAULT_BRIGHTNESS_INCREASE_CONSTANT_TIME;

/** Brightness decrease constant time */
static gint mdy_brightness_decrease_constant_time =
                DEFAULT_BRIGHTNESS_DECREASE_CONSTANT_TIME;

/* ========================================================================= *
 * DATAPIPE_TRACKING
 * ========================================================================= */

/** PackageKit Locked property is set to true */
static bool packagekit_locked = false;

/**
 * Handle packagekit_locked_pipe notifications
 *
 * @param data The locked state stored in a pointer
 */
static void mdy_packagekit_locked_cb(gconstpointer data)
{
    bool prev = packagekit_locked;
    packagekit_locked = GPOINTER_TO_INT(data);

    if( packagekit_locked == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "packagekit_locked = %d", packagekit_locked);

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

    mce_log(LL_DEBUG, "system_state = %d", system_state);

    switch( system_state ) {
    case MCE_STATE_ACTDEAD:
    case MCE_STATE_USER:
        execute_datapipe(&display_state_req_pipe,
                         GINT_TO_POINTER(MCE_DISPLAY_ON),
                         USE_INDATA, CACHE_INDATA);
        break;

    case MCE_STATE_SHUTDOWN:
    case MCE_STATE_REBOOT:
    case MCE_STATE_UNDEF:
    default:
            break;
    }

    /* Clear shutting down flag on re-entry to USER state */
    if( system_state == MCE_STATE_USER && mdy_shutdown_started ) {
        mdy_shutdown_started = FALSE;
        mce_log(LL_NOTICE, "Shutdown canceled");
    }

    /* re-evaluate suspend policy */
    mdy_stm_schedule_rethink();

#ifdef ENABLE_CPU_GOVERNOR
    mdy_governor_rethink();
#endif

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
        switch( system_state ) {
        case MCE_STATE_USER:
        case MCE_STATE_ACTDEAD:
            mdy_additional_bootup_dim_timeout = 0;
            break;
        default:
            break;
        }
        // force blanking timer reprogramming
        mdy_blanking_rethink_timers(true);
    }

EXIT:
    return;
}

/* Cached display state */
static display_state_t display_state = MCE_DISPLAY_UNDEF;

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

        mce_log(LL_WARN, "reject low power mode display request");
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
        mce_log(LL_DEBUG, "reject display mode request at start up");
        next_state = display_state;
    }
    else if( (submode & MCE_TRANSITION_SUBMODE) &&
             ( system_state == MCE_STATE_SHUTDOWN ||
               system_state == MCE_STATE_REBOOT ) ) {
        mce_log(LL_WARN, "reject display mode request at shutdown/reboot");
        next_state = display_state;
    }

UPDATE:
    if( want_state != next_state ) {
        mce_log(LL_WARN, "requested: %s, granted: %s",
                mdy_display_state_name(want_state),
                mdy_display_state_name(next_state));
    }
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
    mdy_stm_push_target_change(next_state);
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

    mce_log(LL_DEVEL, "display state = %s",
            mdy_display_state_name(display_state));

    mdy_display_state_enter_post();

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

    mdy_brightness_set_on_level(curr);

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
static gboolean charger_connected = FALSE;

/** Handle charger_state_pipe notifications
 *
 * @param data TRUE if the charger was connected,
 *             FALSE if the charger was disconnected
 */
static void mdy_datapipe_charger_state_cb(gconstpointer data)
{
    gboolean prev = charger_connected;
    charger_connected = GPOINTER_TO_INT(data);

    if( charger_connected == prev )
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

    mce_log(LL_DEBUG, "alarm_ui_state = %d", alarm_ui_state);

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

    mce_log(LL_DEBUG, "proximity_state = %d", proximity_state);

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
        mdy_cabc_mode_set(mdy_psm_cabc_mode);
    } else {
        /* Restore the CABC mode and brightness setting */
        mdy_psm_cabc_mode = NULL;
        mdy_psm_disp_brightness = -1;

        execute_datapipe(&display_brightness_pipe,
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

    mce_log(LL_DEBUG, "call_state = %d", call_state);

    mdy_blanking_rethink_timers(false);

    // autosuspend policy
    mdy_stm_schedule_rethink();

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

    mce_log(LL_DEBUG, "orientation_state = %d", orientation_state);

    mdy_orientation_generate_activity();
EXIT:
    return;
}

/** Append triggers/filters to datapipes
 */
static void mdy_datapipe_init(void)
{
    // filters
    append_filter_to_datapipe(&display_state_req_pipe,
                              mdy_datapipe_display_state_filter_cb);

    // triggers
    append_output_trigger_to_datapipe(&display_state_req_pipe,
                                      mdy_datapipe_display_state_req_cb);
    append_output_trigger_to_datapipe(&display_state_pipe,
                                      mdy_datapipe_display_state_cb);
    append_output_trigger_to_datapipe(&display_brightness_pipe,
                                      mdy_datapipe_display_brightness_cb);

    append_output_trigger_to_datapipe(&charger_state_pipe,
                                      mdy_datapipe_charger_state_cb);
    append_output_trigger_to_datapipe(&system_state_pipe,
                                      mdy_datapipe_system_state_cb);
    append_output_trigger_to_datapipe(&orientation_sensor_pipe,
                                      mdy_datapipe_orientation_state_cb);
    append_output_trigger_to_datapipe(&submode_pipe,
                                      mdy_datapipe_submode_cb);
    append_output_trigger_to_datapipe(&device_inactive_pipe,
                                      mdy_datapipe_device_inactive_cb);
    append_output_trigger_to_datapipe(&call_state_pipe,
                                      mdy_datapipe_call_state_trigger_cb);
    append_output_trigger_to_datapipe(&power_saving_mode_pipe,
                                      mdy_datapipe_power_saving_mode_cb);
    append_output_trigger_to_datapipe(&proximity_sensor_pipe,
                                      mdy_datapipe_proximity_sensor_cb);
    append_output_trigger_to_datapipe(&alarm_ui_state_pipe,
                                      mdy_datapipe_alarm_ui_state_cb);
    append_output_trigger_to_datapipe(&exception_state_pipe,
                                      mdy_datapipe_exception_state_cb);
    append_output_trigger_to_datapipe(&audio_route_pipe,
                                      mdy_datapipe_audio_route_cb);
    append_output_trigger_to_datapipe(&packagekit_locked_pipe,
                                      mdy_packagekit_locked_cb);
}

/** Remove triggers/filters from datapipes */
static void mdy_datapipe_quit(void)
{
    // triggers

    remove_output_trigger_from_datapipe(&packagekit_locked_pipe,
                                        mdy_packagekit_locked_cb);
    remove_output_trigger_from_datapipe(&alarm_ui_state_pipe,
                                        mdy_datapipe_alarm_ui_state_cb);
    remove_output_trigger_from_datapipe(&proximity_sensor_pipe,
                                        mdy_datapipe_proximity_sensor_cb);
    remove_output_trigger_from_datapipe(&power_saving_mode_pipe,
                                        mdy_datapipe_power_saving_mode_cb);
    remove_output_trigger_from_datapipe(&call_state_pipe,
                                        mdy_datapipe_call_state_trigger_cb);
    remove_output_trigger_from_datapipe(&device_inactive_pipe,
                                        mdy_datapipe_device_inactive_cb);
    remove_output_trigger_from_datapipe(&submode_pipe,
                                        mdy_datapipe_submode_cb);
    remove_output_trigger_from_datapipe(&orientation_sensor_pipe,
                                        mdy_datapipe_orientation_state_cb);
    remove_output_trigger_from_datapipe(&system_state_pipe,
                                        mdy_datapipe_system_state_cb);
    remove_output_trigger_from_datapipe(&charger_state_pipe,
                                        mdy_datapipe_charger_state_cb);
    remove_output_trigger_from_datapipe(&exception_state_pipe,
                                        mdy_datapipe_exception_state_cb);
    remove_output_trigger_from_datapipe(&audio_route_pipe,
                                        mdy_datapipe_audio_route_cb);
    remove_output_trigger_from_datapipe(&display_brightness_pipe,
                                        mdy_datapipe_display_brightness_cb);
    remove_output_trigger_from_datapipe(&display_state_pipe,
                                        mdy_datapipe_display_state_cb);
    remove_output_trigger_from_datapipe(&display_state_req_pipe,
                                        mdy_datapipe_display_state_req_cb);

    // filters

    remove_filter_from_datapipe(&display_state_req_pipe,
                                mdy_datapipe_display_state_filter_cb);
}

/* ========================================================================= *
 * FBDEV_POWER_STATE
 * ========================================================================= */

/** Hook for setting the frame buffer power state
 *
 * For use from mdy_fbdev_set_power() only
 *
 * @param value The ioctl value to pass to the backlight
 */
static void (*mdy_fbdev_set_power_hook)(int value) = 0;

#ifdef ENABLE_HYBRIS
/** Libhybris backend for mdy_fbdev_set_power()
 *
 * @param value FB_BLANK_POWERDOWN or FB_BLANK_UNBLANK
 */
static void mdy_fbdev_set_power_hybris(int value)
{
    static int old_value = -1;

    if( old_value == value )
        goto EXIT;

    switch( value ) {
    case FB_BLANK_POWERDOWN:
        mce_hybris_framebuffer_set_power(false);
        break;

    case FB_BLANK_UNBLANK:
        mce_hybris_framebuffer_set_power(true);
        break;

    default:
        mce_log(LL_WARN, "ignoring unknown ioctl value %d", value);
        break;
    }

    mce_log(LL_DEBUG, "value %d -> %d", old_value, value);
    old_value = value;

EXIT:
    return;
}
/** Dummy backend for mdy_fbdev_set_power()
 *
 * Used in cases where mce should not touch frame buffer
 * power state.
 *
 * @param value (not used)
 * @return TRUE for faked success
 */
static void mdy_fbdev_set_power_dummy(int value)
{
    (void)value;
}
#endif /* ENABLE_HYBRIS */

/** FBIOBLANK backend for mdy_fbdev_set_power()
 *
 * @param value The ioctl value to pass to the backlight
 * @return TRUE on success, FALSE on failure
 */
static void mdy_fbdev_set_power_default(int value)
{
    static int fd        = -1;
    static int old_value = FB_BLANK_UNBLANK;

    if( fd == -1 ) {
        if( (fd = open(FB_DEVICE, O_RDWR)) == -1 ) {
            mce_log(LL_ERR, "Failed to open `%s'; %m", FB_DEVICE);
            goto EXIT;
        }

        old_value = !value; /* force ioctl() */
    }

    if( old_value == value )
        goto EXIT;

    if( ioctl(fd, FBIOBLANK, value) == -1 ) {
        mce_log(LL_ERR, "%s: ioctl(FBIOBLANK,%d): %m", FB_DEVICE, value);
        close(fd), fd = -1;
        goto EXIT;
    }

    old_value = value;

EXIT:
    return;
}

/** Set the frame buffer power state
 *
 * @param value The ioctl value to pass to the backlight
 * @return TRUE on success, FALSE on failure
 */
static void mdy_fbdev_set_power(int value)
{
    if( mdy_fbdev_set_power_hook )
        mdy_fbdev_set_power_hook(value);
    else
        mce_log(LL_ERR, "value = %d before initializing hook", value);
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

/** Target brightness; [0, mdy_brightness_level_maximum] */
static gint mdy_brightness_level_target = -1;

/** Brightness, when display is not off; [0, mdy_brightness_level_maximum] */
static gint mdy_brightness_level_display_on = -1;

/** Dim brightness; [0, mdy_brightness_level_maximum] */
static gint mdy_brightness_level_display_dim = -1;

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
static guint mdy_brightness_fade_timer_cb_id = 0;

/** Fadeout step length */
static gint mdy_brightness_fade_steplength = 2;

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

    mdy_brightness_set_level_hook(number);

    // TODO: we might want to power off fb at zero brightness
    //       and power it up at non-zero brightness???
}

/** Helper for cancelling brightness fade and forcing a brightness level
 *
 * @param number brightness in 0 to mdy_brightness_level_maximum range
 */
static void mdy_brightness_force_level(int number)
{
    mdy_brightness_stop_fade_timer();
    mdy_brightness_level_cached = number;
    mdy_brightness_level_target = number;
    mdy_brightness_set_level(number);
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
    gboolean retval = TRUE;

    (void)data;

    if ((mdy_brightness_level_cached == -1) ||
        (ABS(mdy_brightness_level_cached -
             mdy_brightness_level_target) < mdy_brightness_fade_steplength)) {
        mdy_brightness_level_cached = mdy_brightness_level_target;
        retval = FALSE;
    } else if (mdy_brightness_level_target > mdy_brightness_level_cached) {
        mdy_brightness_level_cached += mdy_brightness_fade_steplength;
    } else {
        mdy_brightness_level_cached -= mdy_brightness_fade_steplength;
    }

    mdy_brightness_set_level(mdy_brightness_level_cached);

    if (retval == FALSE)
        mdy_brightness_fade_timer_cb_id = 0;

    return retval;
}

/**
 * Cancel the brightness fade timeout
 */
static void mdy_brightness_stop_fade_timer(void)
{
    /* Remove the timeout source for the display brightness fade */
    if (mdy_brightness_fade_timer_cb_id != 0) {
        g_source_remove(mdy_brightness_fade_timer_cb_id);
        mdy_brightness_fade_timer_cb_id = 0;
    }
}

/**
 * Setup the brightness fade timeout
 *
 * @param step_time The time between each brightness step
 */
static void mdy_brightness_start_fade_timer(gint step_time)
{
    mdy_brightness_stop_fade_timer();

    /* Setup new timeout */
    mdy_brightness_fade_timer_cb_id =
        g_timeout_add(step_time, mdy_brightness_fade_timer_cb, NULL);
}

/**
 * Update brightness fade
 *
 * Will fade from current value to new value
 *
 * @param new_brightness The new brightness to fade to
 */
static void mdy_brightness_set_fade_target(gint new_brightness)
{
    gboolean increase = (new_brightness >= mdy_brightness_level_cached);
    gint step_time = 10;

    /* This should never happen, but just in case */
    if (mdy_brightness_level_cached == new_brightness)
        goto EXIT;

    /* If we have support for HW-fading, or if we're using the direct
     * brightness change policy, don't bother with any of this
     */
    if ((mdy_brightness_hw_fading_is_supported == TRUE) ||
        ((mdy_brightness_increase_policy == BRIGHTNESS_CHANGE_DIRECT) &&
            (increase == TRUE)) ||
        ((mdy_brightness_decrease_policy == BRIGHTNESS_CHANGE_DIRECT) &&
            (increase == FALSE))) {
        mdy_brightness_force_level(new_brightness);
        goto EXIT;
    }

    /* If we're already fading towards the right brightness,
     * don't change anything
     */
    if (mdy_brightness_level_target == new_brightness)
        goto EXIT;

    mdy_brightness_level_target = new_brightness;

    if (increase == TRUE) {
        if (mdy_brightness_increase_policy == BRIGHTNESS_CHANGE_STEP_TIME)
            step_time = mdy_brightness_increase_step_time;
        else {
            step_time = mdy_brightness_increase_constant_time /
                (new_brightness - mdy_brightness_level_cached);
        }
    } else {
        if (mdy_brightness_decrease_policy == BRIGHTNESS_CHANGE_STEP_TIME)
            step_time = mdy_brightness_decrease_step_time;
        else {
            step_time = mdy_brightness_decrease_constant_time /
                (mdy_brightness_level_cached - new_brightness);
        }
    }

    /* Special case */
    if (step_time == 5) {
        step_time = 2;
        mdy_brightness_fade_steplength = 2;
    } else {
        mdy_brightness_fade_steplength = 1;
    }

    mdy_brightness_start_fade_timer(step_time);

EXIT:
    return;
}

static void mdy_brightness_set_dim_level(void)
{
    /* default is: X percent of maximum */
    int new_brightness = (mdy_brightness_level_maximum *
                          DEFAULT_DIM_BRIGHTNESS) / 100;

    /* or, at maximum half of DISPLAY_ON level */
    if( new_brightness > mdy_brightness_level_display_on / 2 )
        new_brightness = mdy_brightness_level_display_on / 2;

    /* but do not allow zero value */
    if( new_brightness < 1 )
        new_brightness = 1;

    /* The value we have here is for non-dimmed screen only */
    if( mdy_brightness_level_display_dim != new_brightness ) {
        mce_log(LL_DEBUG, "brightness.dim: %d -> %d",
                mdy_brightness_level_display_dim, new_brightness);
        mdy_brightness_level_display_dim = new_brightness;
    }

    int delta = (mdy_brightness_level_display_on -
                 mdy_brightness_level_display_dim);
    int limit = mdy_brightness_level_maximum * 10 / 100;

    execute_datapipe_output_triggers(delta < limit ?
                                     &led_pattern_activate_pipe :
                                     &led_pattern_deactivate_pipe,
                                     "PatternDisplayDimmed",
                                     USE_INDATA);
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

    /* If we're just rehashing the same brightness value, don't bother */
    if ((new_brightness == mdy_brightness_level_cached) &&
        (mdy_brightness_level_cached != -1))
        goto EXIT;

    /* The value we have here is for non-dimmed screen only */
    if( mdy_brightness_level_display_on != new_brightness ) {
        mce_log(LL_DEBUG, "brightness.on: %d -> %d",
                mdy_brightness_level_display_on, new_brightness);
        mdy_brightness_level_display_on = new_brightness;
    }

    /* Re-evaluate dim brightness too */
    mdy_brightness_set_dim_level();

    /* Re-evaluate lpm brightness too */
    // TODO: ALS config & sensor input processing

    /* Take updated values in use */
    switch( display_state ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
        break;

    case MCE_DISPLAY_LPM_ON:
        mdy_brightness_set_fade_target(mdy_brightness_level_display_lpm);
        break;

    case MCE_DISPLAY_DIM:
        mdy_brightness_set_fade_target(mdy_brightness_level_display_dim);
        break;

    default:
    case MCE_DISPLAY_POWER_DOWN:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_UNDEF:
        break;

    case MCE_DISPLAY_ON:
        mdy_brightness_set_fade_target(mdy_brightness_level_display_on);
        break;
    }

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

        if( dim_timeout <= allowed_timeout) )
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
    }
}

/**
 * Setup dim timeout
 */
static void mdy_blanking_schedule_dim(void)
{
    gint dim_timeout = mdy_disp_dim_timeout + mdy_additional_bootup_dim_timeout;

    mdy_blanking_cancel_dim();

    if( mdy_adaptive_dimming_enabled ) {
        gpointer *tmp = g_slist_nth_data(mdy_possible_dim_timeouts,
                                         mdy_dim_timeout_index +
                                         mdy_adaptive_dimming_index);

        if (tmp != NULL)
            dim_timeout = GPOINTER_TO_INT(tmp) +
            mdy_additional_bootup_dim_timeout;
    }

    mce_log(LL_DEBUG, "DIM timer scheduled @ %d secs", dim_timeout);

    /* Setup new timeout */
    mdy_blanking_dim_cb_id = g_timeout_add_seconds(dim_timeout,
                                                  mdy_blanking_dim_cb, NULL);
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

    execute_datapipe(&display_state_req_pipe,
                     GINT_TO_POINTER(MCE_DISPLAY_OFF),
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
    gint timeout = mdy_disp_blank_timeout;

    if( display_state == MCE_DISPLAY_LPM_OFF )
        timeout = mdy_disp_lpm_off_timeout;

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

    return;
}

// TIMER: DIM -> LPM_ON

/** Low power mode timeout callback ID */
static guint mdy_blanking_lpm_on_cb_id = 0;

/**
 * Timeout callback for low power mode
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean mdy_blanking_lpm_on_cb(gpointer data)
{
    (void)data;

    mdy_blanking_lpm_on_cb_id = 0;

    mce_log(LL_DEBUG, "LPM timer triggered");

    execute_datapipe(&display_state_req_pipe,
                     GINT_TO_POINTER(MCE_DISPLAY_LPM_ON),
                     USE_INDATA, CACHE_INDATA);

    return FALSE;
}

/**
 * Cancel the low power mode timeout
 */
static void mdy_blanking_cancel_lpm_on(void)
{
    /* Remove the timeout source for low power mode */
    if (mdy_blanking_lpm_on_cb_id != 0) {
        mce_log(LL_DEBUG, "LPM timer cancelled");
        g_source_remove(mdy_blanking_lpm_on_cb_id);
        mdy_blanking_lpm_on_cb_id = 0;
    }
}

/**
 * Setup low power mode timeout if supported
 */
static void mdy_blanking_schedule_lpm_on(void)
{
    mdy_blanking_cancel_lpm_on();

    if( mdy_use_low_power_mode && mdy_low_power_mode_supported ) {
        /* Setup new timeout */
        mce_log(LL_DEBUG, "LPM timer scheduled @ %d secs",
                mdy_disp_lpm_on_timeout);
        mdy_blanking_lpm_on_cb_id =
            g_timeout_add_seconds(mdy_disp_lpm_on_timeout,
                                  mdy_blanking_lpm_on_cb, NULL);
    } else {
        mdy_blanking_schedule_off();
    }
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
    gint timeout = DEFAULT_LPM_PROXIMITY_BLANK_TIMEOUT;

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

/** Add blanking pause client
 *
 * @param name The private the D-Bus name of the client
 */
static void mdy_blanking_add_pause_client(const gchar *name)
{
    gssize rc = -1;

    if( !name )
        goto EXIT;

    // display must be on
    if( display_state != MCE_DISPLAY_ON ) {
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
    /* Remove all name monitors for the blanking pause requester */
    mce_dbus_owner_monitor_remove_all(&mdy_blanking_pause_clients);

    if( mdy_blanking_is_paused() ) {
        /* Stop blank prevent timer */
        mdy_blanking_stop_pause_period();
        mdy_blanking_rethink_timers(true);
    }
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

/** Reprogram blanking timers
 */
static void mdy_blanking_rethink_timers(bool force)
{
    // TRIGGERS:
    // submode           <- mdy_datapipe_submode_cb()
    // display_state     <- mdy_display_state_enter_post()
    // audio_route       <- mdy_datapipe_audio_route_cb()
    // charger_connected <- mdy_datapipe_charger_state_cb()
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

    static gboolean prev_charger_connected = false;

    static audio_route_t prev_audio_route = AUDIO_ROUTE_HANDSET;

    static submode_t prev_tklock_mode = 0;
    submode_t tklock_mode = submode & MCE_TKLOCK_SUBMODE;

    if( prev_tklock_mode != tklock_mode )
        force = true;

    if( prev_audio_route != audio_route )
        force = true;

    if( prev_charger_connected != charger_connected )
        force = true;

    if( prev_exception_state != exception_state )
        force = true;

    if( prev_call_state != call_state )
        force = true;

    if( prev_proximity_state != proximity_state )
        force = true;

    if( prev_display_state != display_state ) {
        force = true;

        // always stop blanking pause period
        mdy_blanking_stop_pause_period();

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
    mdy_blanking_cancel_lpm_on();
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
        if( mdy_blanking_inhibit_mode == INHIBIT_STAY_DIM )
            break;
        if( charger_connected &&
            mdy_blanking_inhibit_mode == INHIBIT_STAY_DIM_WITH_CHARGER )
            break;
        mdy_blanking_schedule_off();
        break;

    case MCE_DISPLAY_ON:
        if( exception_state & ~UIEXC_CALL ) {
            break;
        }

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

        if( mdy_blanking_inhibit_mode == INHIBIT_STAY_ON )
            break;

        if( charger_connected &&
            mdy_blanking_inhibit_mode == INHIBIT_STAY_ON_WITH_CHARGER )
            break;

        if( tklock_mode ) {
            mdy_blanking_schedule_off();
            break;
        }

        if( mdy_blanking_is_paused() )
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
    prev_charger_connected = charger_connected;
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
        if( proximity_state == COVER_OPEN )
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
    mdy_blanking_cancel_lpm_on();
    mdy_blanking_cancel_lpm_off();

    //mdy_blanking_stop_pause_period();
    //mdy_hbm_cancel_timeout();
    mdy_brightness_stop_fade_timer();
    //mdy_blanking_stop_adaptive_dimming();
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

/** Get the display type from [modules/display] config group
 *
 * @param display_type where to store the selected display type
 *
 * @return TRUE if valid configuration was found, FALSE otherwise
 */

static gboolean mdy_display_type_get_from_config(display_type_t *display_type)
{
    static const gchar group[] = "modules/display";

    gboolean   res = FALSE;
    gchar     *set = 0;
    gchar     *max = 0;

    gchar    **vdir = 0;
    gchar    **vset = 0;
    gchar    **vmax = 0;

    /* First check if we have a configured brightness directory
     * that a) exists and b) contains both brightness and
     * max_brightness files */
    if( (vdir = mce_conf_get_string_list(group, "brightness_dir", 0)) ) {
        for( size_t i = 0; vdir[i]; ++i ) {
            if( !*vdir[i] || g_access(vdir[i], F_OK) )
                continue;

            if( mdy_display_type_probe_brightness(vdir[i], &set, &max) )
                goto EXIT;
        }
    }

    /* Then check if we can find mathes from possible brightness and
     * max_brightness file lists */
    if( !(vset = mce_conf_get_string_list(group, "brightness", 0)) )
        goto EXIT;

    if( !(vmax = mce_conf_get_string_list(group, "max_brightness", 0)) )
        goto EXIT;

    for( size_t i = 0; vset[i]; ++i ) {
        if( *vset[i] && !g_access(vset[i], W_OK) ) {
            set = g_strdup(vset[i]);
            break;
        }
    }

    for( size_t i = 0; vmax[i]; ++i ) {
        if( *vmax[i] && !g_access(vmax[i], R_OK) ) {
            max = g_strdup(vmax[i]);
            break;
        }
    }

EXIT:
    /* Have we found both brightness and max_brightness files? */
    if( set && max ) {
        mce_log(LL_NOTICE, "applying DISPLAY_TYPE_GENERIC from config file");
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

    if( !mce_hybris_framebuffer_init() ) {
        mce_log(LL_NOTICE, "libhybris fb power controls not available; using dummy");
        mdy_fbdev_set_power_hook = mdy_fbdev_set_power_dummy;
    }
    else {
        mce_log(LL_NOTICE, "using libhybris for fb power control");
        mdy_fbdev_set_power_hook = mdy_fbdev_set_power_hybris;
    }

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

    if( mdy_display_type_get_from_hybris(&display_type) ) {
        /* allow proximity based lpm ui */
        mdy_low_power_mode_supported = TRUE;
    }
    else if( mdy_display_type_get_from_config(&display_type) ) {
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
    else {
        display_type = DISPLAY_TYPE_NONE;
    }

    errno = 0;

    mce_log(LL_DEBUG, "Display type: %d", display_type);

    /* Default to using ioctl() for frame buffer power control */
    if( !mdy_fbdev_set_power_hook )
        mdy_fbdev_set_power_hook = mdy_fbdev_set_power_default;
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
 * LIPSTICK_KILLER
 * ========================================================================= */

/** Delay [s] from setUpdatesEnabled() to attempting lipstick core dump */
static gint mdy_lipstick_killer_core_delay = 30;

/** GConf callback ID for mdy_lipstick_killer_core_delay setting */
static guint mdy_lipstick_killer_core_delay_gconf_cb_id = 0;

/* Delay [s] from attempting lipstick core dump to killing lipstick */
static gint mdy_lipstick_killer_kill_delay = 25;

/* Delay [s] for verifying whether lipstick did exit after kill attempt */
static gint mdy_lipstick_killer_verify_delay = 5;

/** Owner of lipstick dbus name */
static gchar *mdy_lipstick_killer_name = 0;

/** PID to kill when lipstick does not react to setUpdatesEnabled() ipc  */
static int mdy_lipstick_killer_pid = -1;

/** Currently active lipstick killer timer id */
static guint mdy_lipstick_killer_id  = 0;

/** Enable/Disable lipstick killer led pattern
 *
 * @param enable true to start the led, false to stop it
 */
static void mdy_lipstick_killer_enable_led(bool enable)
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

/** Timer for verifying that lipstick has exited after kill signal
 *
 * @param aptr Process identifier as void pointer
 *
 * @return FALSE to stop the timer from repeating
 */
static gboolean mdy_lipstick_killer_verify_cb(gpointer aptr)
{
    int pid = GPOINTER_TO_INT(aptr);

    if( !mdy_lipstick_killer_id )
        goto EXIT;

    mdy_lipstick_killer_id = 0;

    if( kill(pid, 0) == -1 && errno == ESRCH )
        goto EXIT;

    mce_log(LL_ERR, "lipstick is not responsive and killing it failed");

EXIT:
    /* Stop the led pattern even if we can't kill lipstick process */
    mdy_lipstick_killer_enable_led(false);

    return FALSE;
}

/** Timer for killing lipstick in case core dump attempt did not make it exit
 *
 * @param aptr Process identifier as void pointer
 *
 * @return FALSE to stop the timer from repeating
 */
static gboolean mdy_lipstick_killer_kill_cb(gpointer aptr)
{
    int pid = GPOINTER_TO_INT(aptr);

    if( !mdy_lipstick_killer_id )
        goto EXIT;

    mdy_lipstick_killer_id = 0;

    /* In the unlikely event that asynchronous pid query is not finished
     * at the kill timeout, abandon the quest */
    if( pid == -1 ) {
        if( (pid = mdy_lipstick_killer_pid) == -1 ) {
            mce_log(LL_WARN, "pid of lipstick not know yet; can't kill it");
            goto EXIT;
        }
    }

    /* If lipstick is already gone after core dump attempt, no further
     * actions are needed */
    if( kill(pid, 0) == -1 && errno == ESRCH )
        goto EXIT;

    mce_log(LL_WARN, "lipstick is not responsive; attempting to kill it");

    /* Send SIGKILL to lipstick; if that succeeded, verify after brief
     * delay if the process is really gone */

    if( kill(pid, SIGKILL) == -1 ) {
        mce_log(LL_ERR, "failed to SIGKILL lipstick: %m");
    }
    else {
        mdy_lipstick_killer_id =
            g_timeout_add(1000 * mdy_lipstick_killer_verify_delay,
                          mdy_lipstick_killer_verify_cb,
                          GINT_TO_POINTER(pid));
    }

EXIT:
    /* Keep led pattern active if verify timer was scheduled */
    mdy_lipstick_killer_enable_led(mdy_lipstick_killer_id != 0);

    return FALSE;
}

/** Timer for dumping lipstick core if setUpdatesEnabled() goes without reply
 *
 * @param aptr Process identifier as void pointer
 *
 * @return FALSE to stop the timer from repeating
 */
static gboolean mdy_lipstick_killer_core_cb(gpointer aptr)
{
    int pid = GPOINTER_TO_INT(aptr);

    if( !mdy_lipstick_killer_id )
        goto EXIT;

    mdy_lipstick_killer_id = 0;

    mce_log(LL_WARN, "lipstick is not responsive; attempting to core dump it");

    /* In the unlikely event that asynchronous pid query is not finished
     * at the core dump timeout, wait a while longer and just kill it */
    if( pid == -1 ) {
        if( (pid = mdy_lipstick_killer_pid) == -1 ) {
            mce_log(LL_WARN, "pid of lipstick not know yet; skip core dump");
            goto SKIP;
        }
    }

    /* We do not want to kill lipstick if debugger is attached to it.
     * Since there can be only one attacher at one time, we can use dummy
     * attach + detach cycle to determine debugger presence. */
    if( ptrace(PTRACE_ATTACH, pid, 0, 0) == -1 ) {
        mce_log(LL_WARN, "could not attach to lipstick: %m");
        mce_log(LL_WARN, "assuming debugger is attached; skip killing");
        goto EXIT;
    }

    if( ptrace(PTRACE_DETACH, pid, 0,0) == -1 ) {
        mce_log(LL_WARN, "could not detach from lipstick: %m");
    }

    /* We need to send some signal that a) leads to core dump b) is not
     * handled "nicely" by lipstick. SIGXCPU fits that description and
     * is also c) somewhat relevant "CPU time limit exceeded" d) easily
     * distinguishable from other "normal" crash reports. */

    if( kill(pid, SIGXCPU) == -1 ) {
        mce_log(LL_ERR, "failed to SIGXCPU lipstick: %m");
        goto EXIT;
    }

    /* Just in case lipstick process was stopped, make it continue - and
     * hopefully dump a core. */

    if( kill(pid, SIGCONT) == -1 )
        mce_log(LL_ERR, "failed to SIGCONT lipstick: %m");

SKIP:

    /* Allow some time for core dump to take place, then just kill it */
    mdy_lipstick_killer_id = g_timeout_add(1000 * mdy_lipstick_killer_kill_delay,
                                           mdy_lipstick_killer_kill_cb,
                                           GINT_TO_POINTER(pid));
EXIT:

    /* Start led pattern active if kill timer was scheduled */
    mdy_lipstick_killer_enable_led(mdy_lipstick_killer_id != 0);

    return FALSE;
}

/** Shedule lipstick core dump + kill
 *
 * This should be called when initiating asynchronous setUpdatesEnabled()
 * D-Bus method call.
 */
static void mdy_lipstick_killer_schedule(void)
{
    /* The lipstick killer is not used unless we have "devel" flavor
     * mce, or normal mce running in verbose mode */
    if( !mce_log_p(LL_DEVEL) )
        goto EXIT;

    /* Setting the core dump delay to zero disables killing too. */
    if( mdy_lipstick_killer_core_delay <= 0 )
        goto EXIT;

    /* Note: Initially we might not yet know the lipstick PID. But once
     *       it gets known, the kill timer chain will lock in to it.
     *       If lipstick name owner changes, the timer chain is cancelled
     *       and pid reset again. This should make sure we can do the
     *       killing even if the async pid query does not finish before
     *       we need to make the 1st setUpdatesEnabled() ipc and we do not
     *       kill freshly restarted lipstick because the previous instance
     *       got stuck. */

    if( !mdy_lipstick_killer_id ) {
        mce_log(LL_DEBUG, "scheduled lipstick killer");
        mdy_lipstick_killer_id =
            g_timeout_add(1000 * mdy_lipstick_killer_core_delay,
                          mdy_lipstick_killer_core_cb,
                          GINT_TO_POINTER(mdy_lipstick_killer_pid));
    }

EXIT:
    return;
}

/** Cancel any pending lipstick killing timers
 *
 * This should be called when non-error reply is received for
 * setUpdatesEnabled() D-Bus method call.
 */
static void mdy_lipstick_killer_cancel(void)
{
    if( mdy_lipstick_killer_id ) {
        g_source_remove(mdy_lipstick_killer_id),
            mdy_lipstick_killer_id = 0;
        mce_log(LL_DEBUG, "cancelled lipstick killer");
    }

    /* In any case stop the led pattern */
    mdy_lipstick_killer_enable_led(false);
}

/* ========================================================================= *
 * RENDERING_ENABLE_DISABLE
 * ========================================================================= */

// TODO: These should come from lipstick devel package
#define RENDERER_SERVICE  "org.nemomobile.lipstick"
#define RENDERER_PATH     "/"
#define RENDERER_IFACE    "org.nemomobile.lipstick"
#define RENDERER_SET_UPDATES_ENABLED "setUpdatesEnabled"

/** Timeout to use for setUpdatesEnabled method calls [ms]; -1 = use default */
static int mdy_renderer_ipc_timeout = 2 * 60 * 1000; /* 2 minutes */

/** UI side rendering state; no suspend unless RENDERER_DISABLED */
static renderer_state_t mdy_renderer_ui_state = RENDERER_UNKNOWN;

/** Currently active setUpdatesEnabled() method call */
static DBusPendingCall *mdy_renderer_set_state_pc = 0;

/** Enabled/Disable setUpdatesEnabled failure led patterns
 */
static void mdy_renderer_led_set(renderer_state_t req)
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
static gboolean mdy_renderer_led_timer_cb(gpointer aptr)
{
    renderer_state_t req = GPOINTER_TO_INT(aptr);

    if( !mdy_renderer_led_timer_id )
        goto EXIT;

    mce_log(LL_DEBUG, "renderer led timer triggered");

    mdy_renderer_led_timer_id = 0;
    mdy_renderer_led_set(req);

EXIT:
    return FALSE;
}

/* Cancel setUpdatesEnabled is taking too long timer
 */
static void mdy_renderer_led_cancel_timer(void)
{
    mdy_renderer_led_set(RENDERER_UNKNOWN);

    if( mdy_renderer_led_timer_id != 0 ) {
        mce_log(LL_DEBUG, "renderer led timer cancelled");
        g_source_remove(mdy_renderer_led_timer_id),
            mdy_renderer_led_timer_id = 0;
    }
}

/* Schedule setUpdatesEnabled is taking too long timer
 */
static void mdy_renderer_led_start_timer(renderer_state_t req)
{
    /* During bootup it is more or less expected that lipstick is
     * unable to answer immediately. So we initially allow longer
     * delay and bring it down gradually to target level. */
    static int delay = LED_DELAY_UI_DISABLE_ENABLE * 10;

    mdy_renderer_led_set(RENDERER_UNKNOWN);

    if( mdy_renderer_led_timer_id != 0 )
        g_source_remove(mdy_renderer_led_timer_id);

    mdy_renderer_led_timer_id = g_timeout_add(delay,
                                              mdy_renderer_led_timer_cb,
                                              GINT_TO_POINTER(req));

    mce_log(LL_DEBUG, "renderer led timer sheduled @ %d ms", delay);

    delay = delay * 3 / 4;
    if( delay < LED_DELAY_UI_DISABLE_ENABLE )
        delay = LED_DELAY_UI_DISABLE_ENABLE;
}

/** Handle replies to org.nemomobile.lipstick.setUpdatesEnabled() calls
 *
 * @param pending   asynchronous dbus call handle
 * @param user_data enable/disable state as void pointer
 */
static void mdy_renderer_set_state_cb(DBusPendingCall *pending,
                                      void *user_data)
{
    DBusMessage *rsp = 0;
    DBusError    err = DBUS_ERROR_INIT;

    /* The user_data pointer is used for storing the renderer
     * state associated with the async method call sent to
     * lipstick. The reply message is just an acknowledgement
     * from ui that it got the value and thus has no content. */
    renderer_state_t state = GPOINTER_TO_INT(user_data);

    mce_log(LL_NOTICE, "%s(%s) - method reply",
            RENDERER_SET_UPDATES_ENABLED,
            state ? "ENABLE" : "DISABLE");

    if( mdy_renderer_set_state_pc != pending )
        goto cleanup;

    mdy_renderer_led_cancel_timer();

    mdy_renderer_set_state_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pending)) )
        goto cleanup;

    if( dbus_set_error_from_message(&err, rsp) ) {
        /* Mark down that the request failed; we can't
         * enter suspend without UI side being in the
         * loop or we'll risk spectacular crashes */
        mce_log(LL_WARN, "%s: %s", err.name, err.message);
        mdy_renderer_ui_state = RENDERER_ERROR;
    }
    else {
        mdy_renderer_ui_state = state;
        mdy_lipstick_killer_cancel();
    }

    mce_log(LL_NOTICE, "RENDERER state=%d", mdy_renderer_ui_state);

    mdy_stm_schedule_rethink();

cleanup:
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

/** Cancel pending org.nemomobile.lipstick.setUpdatesEnabled() call
 *
 * This is just bookkeeping for mce side, the method call message
 * has already been sent - we just are no longer interested in
 * seeing the return message.
 */
static void mdy_renderer_cancel_state_set(void)
{
    mdy_renderer_led_cancel_timer();

    if( !mdy_renderer_set_state_pc )
        return;

    mce_log(LL_NOTICE, "RENDERER STATE REQUEST CANCELLED");

    dbus_pending_call_cancel(mdy_renderer_set_state_pc),
        mdy_renderer_set_state_pc = 0;
}

/** Enable/Disable ui updates via dbus ipc with lipstick
 *
 * Used at transitions to/from display=off
 *
 * Gives time for lipstick to finish rendering activities
 * before putting frame buffer to sleep via early/late suspend,
 * and telling when rendering is allowed again.
 *
 * @param enabled TRUE for enabling updates, FALSE for disbling updates
 *
 * @return TRUE if asynchronous method call was succesfully sent,
 *         FALSE otherwise
 */
static gboolean mdy_renderer_set_state_req(renderer_state_t state)
{
    gboolean         res = FALSE;
    DBusConnection  *bus = 0;
    DBusMessage     *req = 0;
    dbus_bool_t      dta = (state == RENDERER_ENABLED);

    mdy_renderer_cancel_state_set();

    mce_log(LL_NOTICE, "%s(%s) - method call",
            RENDERER_SET_UPDATES_ENABLED,
            state ? "ENABLE" : "DISABLE");

    /* Mark the state at lipstick side as unknown until we get
     * either ack or error reply */
    mdy_renderer_ui_state = RENDERER_UNKNOWN;

    if( !(bus = dbus_connection_get()) )
        goto EXIT;

    req = dbus_message_new_method_call(RENDERER_SERVICE,
                                       RENDERER_PATH,
                                       RENDERER_IFACE,
                                       RENDERER_SET_UPDATES_ENABLED);
    if( !req )
        goto EXIT;

    if( !dbus_message_append_args(req,
                                  DBUS_TYPE_BOOLEAN, &dta,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    if( !dbus_connection_send_with_reply(bus, req,
                                         &mdy_renderer_set_state_pc,
                                         mdy_renderer_ipc_timeout) )
        goto EXIT;

    if( !mdy_renderer_set_state_pc )
        goto EXIT;

    if( !dbus_pending_call_set_notify(mdy_renderer_set_state_pc,
                                      mdy_renderer_set_state_cb,
                                      GINT_TO_POINTER(state), 0) )
        goto EXIT;

    /* Success */
    res = TRUE;

    /* If we do not get reply in a short while, start led pattern */
    mdy_renderer_led_start_timer(state);

    /* And after waiting a bit longer, assume that lipstick i
     * process stuck and kill it */
    mdy_lipstick_killer_schedule();

EXIT:
    if( mdy_renderer_set_state_pc  ) {
        dbus_pending_call_unref(mdy_renderer_set_state_pc);
        // Note: The pending call notification holds the final
        //       reference. It gets released at cancellation
        //       or return from notification callback
    }
    if( req ) dbus_message_unref(req);
    if( bus ) dbus_connection_unref(bus);

    return res;
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
    if( mdy_shutdown_started )
        block_late = true;

    /* no late suspend while PackageKit is in Locked state */
    if( packagekit_locked )
        block_late = true;

    /* no more suspend at module unload */
    if( mdy_unloading_module )
        block_early = true;

    /* do not suspend while ui side might still be drawing */
    if( mdy_renderer_ui_state != RENDERER_DISABLED )
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
    /* Generate activity if the display is on/dim */
    switch( display_state ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        mce_log(LL_DEBUG, "orientation change; generate activity");
        execute_datapipe(&device_inactive_pipe,
                         GINT_TO_POINTER(FALSE),
                         USE_INDATA, CACHE_INDATA);
        break;

    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
        break;
    }
}

/* ========================================================================= *
 * DISPLAY_STATE
 * ========================================================================= */

/** Start/stop orientation sensor based on display state
 */
static void mdy_orientation_sensor_rethink(void)
{
    /* Enable orientation sensor in ON|DIM */

    /* Start the orientation sensor already when powering up
     * to ON|DIM states -> we have valid sensor state about the
     * same time as display transition finishes.
     *
     * FIXME: This needs to be revisited when LPM display states
     *         are taken in use.
     */
    switch( display_state ) {
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_POWER_UP:
        mce_sensorfw_orient_set_notify(mdy_orientation_changed_cb);
        mce_sensorfw_orient_enable();
        break;

    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_POWER_DOWN:
        mce_sensorfw_orient_disable();
        mce_sensorfw_orient_set_notify(0);
        break;
    }
}

/* React to new display state (via display state datapipe)
 */
static void mdy_display_state_enter_post(void)
{
    /* Disable blanking pause if display != ON */
    if( display_state != MCE_DISPLAY_ON )
        mdy_blanking_remove_pause_clients();

    /* Program dim/blank timers */
    mdy_blanking_rethink_timers(false);

    mdy_hbm_rethink();
    mdy_orientation_sensor_rethink();

    switch( display_state ) {
    case MCE_DISPLAY_POWER_DOWN:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
        /* Blanking or already blanked -> set zero brightness */
        mdy_brightness_force_level(0);
        break;

    case MCE_DISPLAY_POWER_UP:
        /* Unblanking; brightness depends on the next state */
        mdy_brightness_force_level(mdy_brightness_level_display_resume);
        break;

    case MCE_DISPLAY_LPM_ON:
        /* LPM UI active; use lpm brightness */
        mdy_brightness_force_level(mdy_brightness_level_display_lpm);
        break;

    case MCE_DISPLAY_DIM:
        if( mdy_brightness_level_cached <= mdy_brightness_level_display_lpm ) {
            /* If we unblank, switch on display immediately */
            mdy_brightness_force_level(mdy_brightness_level_display_dim);
        } else {
            /* Gradually fade in/out to target level */
            mdy_brightness_set_fade_target(mdy_brightness_level_display_dim);
        }
        break;

    case MCE_DISPLAY_ON:
        if( mdy_brightness_level_cached <= mdy_brightness_level_display_lpm ) {
            /* If we unblank, switch on display immediately */
            mdy_brightness_force_level(mdy_brightness_level_display_on);
        } else {
            /* Gradually fade in/out to target level */
            mdy_brightness_set_fade_target(mdy_brightness_level_display_on);
        }
        break;

    default:
    case MCE_DISPLAY_UNDEF:
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
 * output trigger mdy_display_state_enter_post().
 *
 * @param prev_state    previous display state
 * @param display_state state transferred to
 */
static void mdy_display_state_enter_pre(display_state_t prev_state,
                                        display_state_t next_state)
{
    mce_log(LL_INFO, "END %s -> %s transition",
            mdy_display_state_name(prev_state),
            mdy_display_state_name(next_state));

    /* Restore display_state_pipe to valid value */
    display_state_pipe.cached_data = GINT_TO_POINTER(next_state);

    switch( next_state ) {
    case MCE_DISPLAY_ON:
        mdy_brightness_level_display_resume = mdy_brightness_level_display_on;
        break;

    case MCE_DISPLAY_DIM:
        mdy_brightness_level_display_resume = mdy_brightness_level_display_dim;
        break;

    case MCE_DISPLAY_LPM_ON:
        mdy_brightness_level_display_resume = mdy_brightness_level_display_lpm;
        break;

    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_POWER_UP:
    case MCE_DISPLAY_POWER_DOWN:
        mdy_brightness_level_display_resume = 1;
        break;
    }

    /* Run display state change triggers */
    execute_datapipe(&display_state_pipe,
                     GINT_TO_POINTER(next_state),
                     USE_INDATA, CACHE_INDATA);
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
            mdy_display_state_name(prev_state),
            mdy_display_state_name(next_state));

    /* Cancel display state specific timers that we do not want to
     * trigger while waiting for frame buffer suspend/resume. */
    mdy_blanking_cancel_timers();

    /* Broadcast the final target of this transition; note that this
     * happens while display_state_pipe still holds the previous
     * (non-transitional) state */
    execute_datapipe(&display_state_next_pipe,
                     GINT_TO_POINTER(next_state),
                     USE_INDATA, CACHE_INDATA);

    /* Invalidate display_state_pipe when making transitions
     * that need to wait for external parties */
    if( next_state == MCE_DISPLAY_OFF ) {
        display_state_pipe.cached_data = GINT_TO_POINTER(MCE_DISPLAY_POWER_DOWN);
        /* Run display state change triggers */
        execute_datapipe(&display_state_pipe,
                         display_state_pipe.cached_data,
                         USE_INDATA, CACHE_INDATA);

    }
    else if( prev_state == MCE_DISPLAY_OFF ) {
        display_state_pipe.cached_data = GINT_TO_POINTER(MCE_DISPLAY_POWER_UP);
        /* Run display state change triggers */
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

/** Display state to human readable string
 */
static const char *mdy_display_state_name(display_state_t state)
{
    const char *name = "UNKNOWN";

#define DO(tag) case MCE_DISPLAY_##tag: name = #tag; break;
    switch( state ) {
        DO(UNDEF)
        DO(OFF)
        DO(LPM_OFF)
        DO(LPM_ON)
        DO(DIM)
        DO(ON)
        DO(POWER_UP)
        DO(POWER_DOWN)
    default: break;
    }
#undef DO
    return name;
}

/** Does "org.nemomobile.lipstick" have owner on system bus */
static bool mdy_stm_lipstick_on_dbus = false;

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

static void mdy_stm_lipstick_name_owner_pid(const char *name, int pid)
{
    if( mdy_lipstick_killer_name && !strcmp(mdy_lipstick_killer_name, name) )
        mdy_lipstick_killer_pid = pid;
}

/** react to systemui availability changes
 */
static void mdy_stm_lipstick_name_owner_changed(const char *name,
                                                const char *prev,
                                                const char *curr)
{
    (void)name;
    (void)prev;

    bool has_owner = (curr && *curr);

    if( mdy_stm_lipstick_on_dbus != has_owner ) {
        /* set setUpdatesEnabled(true) needs to be called flag */
        mdy_stm_enable_rendering_needed = true;

        /* update lipstick runing state */
        mdy_stm_lipstick_on_dbus = has_owner;
        mce_log(LL_WARN, "lipstick %s system bus",
                has_owner ? "is on" : "dropped from");

        /* a) Lipstick assumes that updates are allowed when
         *    it starts up. Try to arrange that it is so.
         * b) Without lipstick in place we must not suspend
         *    because there is nobody to communicate the
         *    updating is allowed
         *
         * Turning the display on at lipstick runstate change
         * deals with both (a) and (b) */
        mdy_stm_push_target_change(MCE_DISPLAY_ON);
    }

    g_free(mdy_lipstick_killer_name), mdy_lipstick_killer_name = 0;
    mdy_lipstick_killer_pid = -1;
    mdy_lipstick_killer_cancel();

    if( curr && *curr ) {
        mdy_lipstick_killer_name = g_strdup(curr);
        mce_dbus_get_pid_async(curr, mdy_stm_lipstick_name_owner_pid);
    }

    execute_datapipe(&lipstick_available_pipe,
                     GINT_TO_POINTER(has_owner),
                     USE_INDATA, CACHE_INDATA);
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
        mdy_waitfb_data.suspended = true, mdy_fbdev_set_power(FB_BLANK_POWERDOWN);
#else
    mce_log(LL_NOTICE, "power off frame buffer");
    mdy_waitfb_data.suspended = true, mdy_fbdev_set_power(FB_BLANK_POWERDOWN);
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
        mdy_waitfb_data.suspended = false, mdy_fbdev_set_power(FB_BLANK_UNBLANK);
#else
    mce_log(LL_NOTICE, "power off frame buffer");
    mdy_waitfb_data.suspended = false, mdy_fbdev_set_power(FB_BLANK_UNBLANK);
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

/** Predicate for display state change in progress
 */
static bool mdy_stm_is_target_changing(void)
{
    return mdy_stm_curr != mdy_stm_next;
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
    if( mdy_stm_curr == mdy_stm_next )
        return false;

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
    mdy_display_state_enter_pre(prev, mdy_stm_curr);
}

/** Predicate for setUpdatesEnabled() ipc not finished yet
 */
static bool mdy_stm_is_renderer_pending(void)
{
    return mdy_renderer_ui_state == RENDERER_UNKNOWN;
}

/** Predicate for setUpdatesEnabled(false) ipc finished
 */
static bool mdy_stm_is_renderer_disabled(void)
{
    return mdy_renderer_ui_state == RENDERER_DISABLED;
}

/** Predicate for setUpdatesEnabled(true) ipc finished
 */
static bool mdy_stm_is_renderer_enabled(void)
{
    return mdy_renderer_ui_state == RENDERER_ENABLED;
}

/* Start setUpdatesEnabled(false) ipc with systemui
 */
static void mdy_stm_disable_renderer(void)
{
    if( mdy_renderer_ui_state != RENDERER_DISABLED ) {
        mce_log(LL_NOTICE, "stopping renderer");
        mdy_renderer_set_state_req(RENDERER_DISABLED);
    }
}

/* Start setUpdatesEnabled(true) ipc with systemui
 */
static void mdy_stm_enable_renderer(void)
{
    if( !mdy_stm_lipstick_on_dbus ) {
        mdy_renderer_ui_state = RENDERER_ENABLED;
        mce_log(LL_NOTICE, "starting renderer - skipped");
    }
    else if( mdy_renderer_ui_state != RENDERER_ENABLED ||
             mdy_stm_enable_rendering_needed ) {
        mce_log(LL_NOTICE, "starting renderer");
        mdy_renderer_set_state_req(RENDERER_ENABLED);
        /* clear setUpdatesEnabled(true) needs to be called flag */
        mdy_stm_enable_rendering_needed = false;
    }
    else {
        mce_log(LL_NOTICE, "renderer already enabled");
    }
}

/** Execute one state machine step
 */
static void mdy_stm_step(void)
{
    switch( mdy_stm_dstate ) {
    case STM_UNSET:
    default:
            mdy_stm_acquire_wakelock();
        if( mdy_stm_display_state_needs_power(mdy_stm_want) )
            mdy_stm_trans(STM_RENDERER_INIT_START);
        break;

    case STM_RENDERER_INIT_START:
        if( !mdy_stm_lipstick_on_dbus ) {
            mdy_stm_trans(STM_ENTER_POWER_ON);
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
            mdy_stm_trans(STM_ENTER_POWER_ON);
            break;
        }
        /* If lipstick is not responsive, we must keep trying
         * until we get a reply - or lipstick dies and drops
         * from system bus */
        mce_log(LL_CRIT, "ui start failed, retrying");
        mdy_stm_trans(STM_RENDERER_INIT_START);
        break;

    case STM_ENTER_POWER_ON:
        mdy_stm_finish_target_change();
        mdy_stm_trans(STM_STAY_POWER_ON);
        break;

    case STM_STAY_POWER_ON:
        if( mdy_stm_enable_rendering_needed && mdy_stm_lipstick_on_dbus ) {
            mce_log(LL_NOTICE, "handling lipstick startup");
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
            mdy_stm_trans(STM_RENDERER_INIT_STOP);
        break;

    case STM_RENDERER_INIT_STOP:
        if( !mdy_stm_lipstick_on_dbus ) {
            mce_log(LL_WARN, "no lipstick; going to logical off");
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
        /* If lipstick is not responsive, we must keep trying
         * until we get a reply - or lipstick dies and drops
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
        if( mdy_stm_display_state_needs_power(mdy_stm_next) )
            mdy_stm_trans(STM_RENDERER_INIT_START);
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

        if( !mdy_stm_lipstick_on_dbus )
            break;

        if( mdy_stm_is_early_suspend_allowed() ) {
            mdy_stm_trans(STM_LEAVE_LOGICAL_OFF);
            break;
        }

        if( mdy_stm_enable_rendering_needed ) {
            mdy_stm_trans(STM_RENDERER_INIT_STOP);
            break;
        }

        break;

    case STM_LEAVE_LOGICAL_OFF:
        if( mdy_stm_is_target_changing() )
            mdy_stm_trans(STM_RENDERER_INIT_START);
        else
            mdy_stm_trans(STM_INIT_SUSPEND);
        break;
    }
}

/** Execute state machine steps until wait state is hit
 */
static void mdy_stm_exec(void)
{
    stm_state_t prev;
    mce_log(LL_INFO, "ENTER @ %s", mdy_stm_state_name(mdy_stm_dstate));
    do {
        prev = mdy_stm_dstate;
        mdy_stm_step();
    } while( mdy_stm_dstate != prev );
    mce_log(LL_INFO, "LEAVE @ %s", mdy_stm_state_name(mdy_stm_dstate));
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
        mce_log(LL_NOTICE, "Not configured: %s", sec);
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
    if( mdy_shutdown_started  ) {
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
 * DBUS_NAME_OWNER_TRACKING
 * ========================================================================= */

/** Format string for constructing name owner lost match rules */
static const char mdy_nameowner_rule_fmt[] =
"type='signal'"
",interface='"DBUS_INTERFACE_DBUS"'"
",member='NameOwnerChanged'"
",arg0='%s'"
;

/** D-Bus connection */
static DBusConnection *mdy_nameowner_bus = 0;

/** Lookup table of D-Bus names to watch */
static struct
{
    const char *name;
    char       *rule;
    void     (*notify)(const char *name, const char *prev, const char *curr);
} mdy_nameowner_lut[] =
{
    {
        .name = RENDERER_SERVICE,
        .notify = mdy_stm_lipstick_name_owner_changed,
    },
    {
        .name = 0,
    }
};

/** Call NameOwner changed callback from mdy_nameowner_lut
 *
 * @param name D-Bus name that changed owner
 * @param prev D-Bus name of the previous owner
 * @param curr D-Bus name of the current owner
 */
static void
mdy_nameowner_changed(const char *name, const char *prev, const char *curr)
{
    for( int i = 0; mdy_nameowner_lut[i].name; ++i ) {
        if( !strcmp(mdy_nameowner_lut[i].name, name) )
            mdy_nameowner_lut[i].notify(name, prev, curr);
    }
}

/** Call back for handling asynchronous client verification via GetNameOwner
 *
 * @param pending   control structure for asynchronous d-bus methdod call
 * @param user_data dbus_name of the client as void poiter
 */
static
void
mdy_nameowner_query_rsp(DBusPendingCall *pending, void *user_data)
{
    const char  *name   = user_data;
    const char  *owner  = 0;
    DBusMessage *rsp    = 0;
    DBusError    err    = DBUS_ERROR_INIT;

    if( !(rsp = dbus_pending_call_steal_reply(pending)) )
        goto EXIT;

    if( dbus_set_error_from_message(&err, rsp) ||
        !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_STRING, &owner,
                               DBUS_TYPE_INVALID) )
    {
        if( strcmp(err.name, DBUS_ERROR_NAME_HAS_NO_OWNER) ) {
            mce_log(LL_WARN, "%s: %s", err.name, err.message);
        }
    }

    mdy_nameowner_changed(name, "", owner ?: "");

EXIT:
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

/** Verify that a client exists via an asynchronous GetNameOwner method call
 *
 * @param name the dbus name who's owner we wish to know
 */
static
void
mdy_nameowner_query_req(const char *name)
{
    DBusMessage     *req = 0;
    DBusPendingCall *pc  = 0;
    char            *key = 0;

    req = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
                                       DBUS_PATH_DBUS,
                                       DBUS_INTERFACE_DBUS,
                                       "GetNameOwner");
    dbus_message_append_args(req,
                             DBUS_TYPE_STRING, &name,
                             DBUS_TYPE_INVALID);

    if( !dbus_connection_send_with_reply(mdy_nameowner_bus, req, &pc, -1) )
        goto EXIT;

    if( !pc )
        goto EXIT;

    key = strdup(name);

    if( !dbus_pending_call_set_notify(pc, mdy_nameowner_query_rsp, key, free) )
        goto EXIT;

    // key string is owned by pending call
    key = 0;

EXIT:
    free(key);

    if( pc  ) dbus_pending_call_unref(pc);
    if( req ) dbus_message_unref(req);
}

/** D-Bus message filter for handling NameOwnerChanged signals
 *
 * @param con       (not used)
 * @param msg       message to be acted upon
 * @param user_data (not used)
 *
 * @return DBUS_HANDLER_RESULT_NOT_YET_HANDLED (other filters see the msg too)
 */
static
DBusHandlerResult
mdy_nameowner_filter_cb(DBusConnection *con, DBusMessage *msg, void *user_data)
{
    (void)user_data;
    (void)con;

    DBusHandlerResult res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *name = 0;
    const char *prev = 0;
    const char *curr = 0;

    DBusError err = DBUS_ERROR_INIT;

    if( !dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS,
                                "NameOwnerChanged") )
        goto EXIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_STRING, &prev,
                               DBUS_TYPE_STRING, &curr,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    mdy_nameowner_changed(name, prev, curr);
EXIT:
    dbus_error_free(&err);
    return res;
}

/** Create a match rule and add it to D-Bus daemon side
 *
 * Use mdy_nameowner_unwatch() to cancel.
 *
 * @param name D-Bus name that changed owner
 *
 * @return rule that was sent to the D-Bus daemon
 */
static char *
mdy_nameowner_watch(const char *name)
{
    char *rule = g_strdup_printf(mdy_nameowner_rule_fmt, name);
    dbus_bus_add_match(mdy_nameowner_bus, rule, 0);
    return rule;
}

/** Remove a match rule from D-Bus daemon side and free it
 *
 * @param rule obtained from mdy_nameowner_watch()
 */
static void mdy_nameowner_unwatch(char *rule)
{
    if( rule ) {
        dbus_bus_remove_match(mdy_nameowner_bus, rule, 0);
        g_free(rule);
    }
}

/** Start D-Bus name owner tracking
 */
static void
mdy_nameowner_init(void)
{
    /* Get D-Bus system bus connection */
    if( !(mdy_nameowner_bus = dbus_connection_get()) ) {
        goto EXIT;
    }

    dbus_connection_add_filter(mdy_nameowner_bus,
                               mdy_nameowner_filter_cb, 0, 0);

    for( int i = 0; mdy_nameowner_lut[i].name; ++i ) {
        mdy_nameowner_lut[i].rule = mdy_nameowner_watch(mdy_nameowner_lut[i].name);
        mdy_nameowner_query_req(mdy_nameowner_lut[i].name);
    }
EXIT:
    return;
}

/** Stop D-Bus name owner tracking
 */

static void
mdy_nameowner_quit(void)
{
    if( !mdy_nameowner_bus )
        goto EXIT;

    /* remove filter callback */
    dbus_connection_remove_filter(mdy_nameowner_bus,
                                  mdy_nameowner_filter_cb, 0);

    /* remove name owner matches */
    for( int i = 0; mdy_nameowner_lut[i].name; ++i ) {
        mdy_nameowner_unwatch(mdy_nameowner_lut[i].rule),
            mdy_nameowner_lut[i].rule = 0;
    }

    // TODO: we should keep track of async name owner calls
    //       and cancel them at this point

    dbus_connection_unref(mdy_nameowner_bus), mdy_nameowner_bus = 0;

EXIT:
    return;

}

/* ========================================================================= *
 * DBUS_HANDLERS
 * ========================================================================= */

/**
 * Send a display status reply or signal
 *
 * @param method_call A DBusMessage to reply to;
 *                    pass NULL to send a display status signal instead
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_send_display_status(DBusMessage *const method_call)
{
    static const gchar *prev_state = "";
    DBusMessage *msg = NULL;
    const gchar *state = NULL;
    gboolean status = FALSE;

    switch( display_state ) {
    case MCE_DISPLAY_POWER_DOWN:
    case MCE_DISPLAY_POWER_UP:
        if( !method_call ) {
            /* Looks like something in the UI does not survive
             * getting display off signal before setUpdatesEnabled()
             * method call... send it afterwards as before*/
            goto EXIT;
        }
        // fall through
    default:
    case MCE_DISPLAY_UNDEF:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
        state = MCE_DISPLAY_OFF_STRING;
        break;

    case MCE_DISPLAY_DIM:
        state = MCE_DISPLAY_DIM_STRING;
        break;

    case MCE_DISPLAY_ON:
        state = MCE_DISPLAY_ON_STRING;
        break;
    }

    if( !method_call ) {
        if( !strcmp(prev_state, state))
            goto EXIT;
        prev_state = state;
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

    /* proximity covered? */
    if( proximity_state == COVER_CLOSED ) {
        reason = "proximity covered";
        goto EXIT;
    }
EXIT:

    return reason;
}

/**
 * D-Bus callback for the display on method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_handle_display_on_req(DBusMessage *const msg)
{
    dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
    gboolean status = FALSE;

    const char *reason = mdy_dbus_get_reason_to_block_display_on();

    if( reason ) {
        mce_log(LL_WARN, "display ON request from %s denied: %s",
                mce_dbus_get_message_sender_ident(msg), reason);
    }
    else {
        mce_log(LL_DEVEL,"display ON request from %s",
                mce_dbus_get_message_sender_ident(msg));
        execute_datapipe(&display_state_req_pipe,
                         GINT_TO_POINTER(MCE_DISPLAY_ON),
                         USE_INDATA, CACHE_INDATA);
    }

    if (no_reply == FALSE) {
        DBusMessage *reply = dbus_new_method_reply(msg);

        status = dbus_send_message(reply);
    } else {
        status = TRUE;
    }

    return status;
}

/**
 * D-Bus callback for the display dim method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_handle_display_dim_req(DBusMessage *const msg)
{
    dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
    gboolean status = FALSE;

    const char *reason = mdy_dbus_get_reason_to_block_display_on();

    if( reason ) {
        mce_log(LL_WARN, "display DIM request from %s denied: %s",
                mce_dbus_get_message_sender_ident(msg), reason);
    }
    else {
        mce_log(LL_DEVEL,"display DIM request from %s",
                mce_dbus_get_message_sender_ident(msg));
        execute_datapipe(&display_state_req_pipe,
                         GINT_TO_POINTER(MCE_DISPLAY_DIM),
                         USE_INDATA, CACHE_INDATA);
    }

    if (no_reply == FALSE) {
        DBusMessage *reply = dbus_new_method_reply(msg);

        status = dbus_send_message(reply);
    } else {
        status = TRUE;
    }

    return status;
}

/**
 * D-Bus callback for the display off method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_handle_display_off_req(DBusMessage *const msg)
{
    dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
    gboolean status = FALSE;

    mce_log(LL_DEVEL, "display off request from %s",
            mce_dbus_get_message_sender_ident(msg));

    execute_datapipe(&tk_lock_pipe,
                     GINT_TO_POINTER(LOCK_ON),
                     USE_INDATA, CACHE_INDATA);
    execute_datapipe(&display_state_req_pipe,
                     GINT_TO_POINTER(MCE_DISPLAY_OFF),
                     USE_INDATA, CACHE_INDATA);

    if (no_reply == FALSE) {
        DBusMessage *reply = dbus_new_method_reply(msg);

        status = dbus_send_message(reply);
    } else {
        status = TRUE;
    }

    return status;
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
 * D-Bus callback to switch demo mode on or off
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_handle_set_demo_mode_req(DBusMessage *const msg)
{
    gboolean status = FALSE;
    DBusError error;
    DBusMessage *reply = NULL;
    char *use = 0;

    // FIXME: this is defunct code and should be removed

    mce_log(LL_DEVEL, "Recieved demo mode change request from %s",
            mce_dbus_get_message_sender_ident(msg));

    dbus_error_init(&error);

    if(!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &use, DBUS_TYPE_INVALID))
    {
        dbus_error_free(&error);
        goto EXIT;
    }

    if(!strcmp(use, "on"))
    {
        mdy_blanking_inhibit_mode = INHIBIT_STAY_ON;

        /* unblank screen */
        execute_datapipe(&display_state_req_pipe,
                         GINT_TO_POINTER(MCE_DISPLAY_ON),
                         USE_INDATA, CACHE_INDATA);

        /* turn off tklock */
        execute_datapipe(&tk_lock_pipe,
                         GINT_TO_POINTER(LOCK_OFF_DELAYED),
                         USE_INDATA, CACHE_INDATA);

        mdy_blanking_rethink_timers(true);
    }
    else
    {
        mdy_blanking_inhibit_mode = DEFAULT_BLANKING_INHIBIT_MODE;
        mdy_blanking_rethink_timers(true);
    }

    if((reply = dbus_message_new_method_return(msg)))
        if(dbus_message_append_args (reply, DBUS_TYPE_STRING, &use, DBUS_TYPE_INVALID) == FALSE)
        {
            dbus_message_unref(reply);
            goto EXIT;
        }

    status = dbus_send_message(reply) ;

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

    mce_log(LL_DEBUG, "Received desktop startup notification");

    mce_log(LL_DEBUG, "deactivate MCE_LED_PATTERN_POWER_ON");
    execute_datapipe_output_triggers(&led_pattern_deactivate_pipe,
                                     MCE_LED_PATTERN_POWER_ON, USE_INDATA);

    mce_rem_submode_int32(MCE_BOOTUP_SUBMODE);

    mce_rem_submode_int32(MCE_MALF_SUBMODE);
    if (g_access(MCE_MALF_FILENAME, F_OK) == 0) {
        g_remove(MCE_MALF_FILENAME);
    }

    /* Restore normal inactivity timeout */
    execute_datapipe(&inactivity_timeout_pipe,
                     GINT_TO_POINTER(mdy_disp_dim_timeout +
                                     mdy_disp_blank_timeout),
                     USE_INDATA, CACHE_INDATA);

    /* Remove the additional timeout */
    mdy_additional_bootup_dim_timeout = 0;

    /* Reprogram blanking timers */
    mdy_blanking_rethink_timers(true);

    status = TRUE;

    return status;
}

/** Common code for thermal, battery empty and normal shutdown handling
 */
static void mdy_dbus_handle_shutdown_started(void)
{
    /* mark that we're shutting down */
    mdy_shutdown_started = TRUE;

    /* re-evaluate suspend policy */
    mdy_stm_schedule_rethink();

#ifdef ENABLE_CPU_GOVERNOR
    mdy_governor_rethink();
#endif
}

/** D-Bus callback for the shutdown notification signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean mdy_dbus_handle_shutdown_started_sig(DBusMessage *const msg)
{
    (void)msg;

    mce_log(LL_WARN, "Received shutdown notification");
    mdy_dbus_handle_shutdown_started();

    return TRUE;
}

/** D-Bus callback for the thermal shutdown notification signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean
mdy_dbus_handle_thermal_shutdown_started_sig(DBusMessage *const msg)
{
    (void)msg;

    mce_log(LL_WARN, "Received thermal shutdown notification");
    mdy_dbus_handle_shutdown_started();

    return TRUE;
}

/** D-Bus callback for the battery empty shutdown notification signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean
mdy_dbus_handle_battery_empty_shutdown_started_sig(DBusMessage *const msg)
{
    (void)msg;

    mce_log(LL_WARN, "Received battery empty shutdown notification");
    mdy_dbus_handle_shutdown_started();

    return TRUE;
}

/** Install dbus message handlers
 */
static void mdy_dbus_init(void)
{
    /* get_display_status */
    mce_dbus_handler_add(MCE_REQUEST_IF,
                         MCE_DISPLAY_STATUS_GET,
                         NULL,
                         DBUS_MESSAGE_TYPE_METHOD_CALL,
                         mdy_dbus_handle_display_status_get_req);

    /* get_cabc_mode */
    mce_dbus_handler_add(MCE_REQUEST_IF,
                         MCE_CABC_MODE_GET,
                         NULL,
                         DBUS_MESSAGE_TYPE_METHOD_CALL,
                         mdy_dbus_handle_cabc_mode_get_req);

    /* req_display_state_on */
    mce_dbus_handler_add(MCE_REQUEST_IF,
                         MCE_DISPLAY_ON_REQ,
                         NULL,
                         DBUS_MESSAGE_TYPE_METHOD_CALL,
                         mdy_dbus_handle_display_on_req);

    /* req_display_state_dim */
    mce_dbus_handler_add(MCE_REQUEST_IF,
                         MCE_DISPLAY_DIM_REQ,
                         NULL,
                         DBUS_MESSAGE_TYPE_METHOD_CALL,
                         mdy_dbus_handle_display_dim_req);

    /* req_display_state_off */
    mce_dbus_handler_add(MCE_REQUEST_IF,
                         MCE_DISPLAY_OFF_REQ,
                         NULL,
                         DBUS_MESSAGE_TYPE_METHOD_CALL,
                         mdy_dbus_handle_display_off_req);

    /* req_display_blanking_pause */
    mce_dbus_handler_add(MCE_REQUEST_IF,
                         MCE_PREVENT_BLANK_REQ,
                         NULL,
                         DBUS_MESSAGE_TYPE_METHOD_CALL,
                         mdy_dbus_handle_blanking_pause_start_req);

    /* req_display_cancel_blanking_pause */
    mce_dbus_handler_add(MCE_REQUEST_IF,
                         MCE_CANCEL_PREVENT_BLANK_REQ,
                         NULL,
                         DBUS_MESSAGE_TYPE_METHOD_CALL,
                         mdy_dbus_handle_blanking_pause_cancel_req);

    /* req_cabc_mode */
    mce_dbus_handler_add(MCE_REQUEST_IF,
                         MCE_CABC_MODE_REQ,
                         NULL,
                         DBUS_MESSAGE_TYPE_METHOD_CALL,
                         mdy_dbus_handle_cabc_mode_set_req);

    /* Desktop readiness signal */
    mce_dbus_handler_add("com.nokia.startup.signal",
                         "desktop_visible",
                         NULL,
                         DBUS_MESSAGE_TYPE_SIGNAL,
                         mdy_dbus_handle_desktop_started_sig);

    /* System shutdown signal */
    mce_dbus_handler_add("com.nokia.dsme.signal",
                         "shutdown_ind",
                         NULL,
                         DBUS_MESSAGE_TYPE_SIGNAL,
                         mdy_dbus_handle_shutdown_started_sig);

    /* Thermal shutdown signal */
    mce_dbus_handler_add("com.nokia.dsme.signal",
                         "thermal_shutdown_ind",
                         NULL,
                         DBUS_MESSAGE_TYPE_SIGNAL,
                         mdy_dbus_handle_thermal_shutdown_started_sig);

    /* Battery empty shutdown signal */
    mce_dbus_handler_add("com.nokia.dsme.signal",
                         "battery_empty_ind",
                         NULL,
                         DBUS_MESSAGE_TYPE_SIGNAL,
                         mdy_dbus_handle_battery_empty_shutdown_started_sig);

    /* Turning demo mode on/off */
    mce_dbus_handler_add(MCE_REQUEST_IF,
                         MCE_DBUS_DEMO_MODE_REQ,
                         NULL,
                         DBUS_MESSAGE_TYPE_METHOD_CALL,
                         mdy_dbus_handle_set_demo_mode_req);
}

/** Remove dbus message handlers
 */
static void mdy_dbus_quit(void)
{
    // FIXME: actually remove dbus handlers
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
        mce_log(LL_NOTICE, "mdy_init_done -> %s",
                mdy_init_done ? "true" : "false");
        mdy_stm_schedule_rethink();
#ifdef ENABLE_CPU_GOVERNOR
        mdy_governor_rethink();
#endif
        mdy_poweron_led_rethink();
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

    /* for now we only need to differentiate USER and not USER */
    if( !strcmp(buff, "BOOTSTATE=USER") )
        mdy_bootstate = BOOTSTATE_USER;
    else
        mdy_bootstate = BOOTSTATE_ACT_DEAD;
EXIT:
    if( fd != -1 ) close(fd);

    mdy_poweron_led_rethink();
}

/** Start tracking of init_done and bootstate flag files
 */
static void mdy_flagfiles_start_tracking(void)
{
    static const char flag_dir[]  = "/run/systemd/boot-status";
    static const char flag_init[] = "init-done";
    static const char flag_boot[] = "bootstate";

    time_t uptime = 0;  // uptime in seconds
    time_t ready  = 60; // desktop ready at
    time_t delay  = 10; // default wait time

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
}

/** Stop tracking of init_done state
 */
static void mdy_flagfiles_stop_tracking(void)
{
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
            mdy_gconf_sanitize_brightness_settings();
        }
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
    else if (id == mdy_disp_blank_timeout_gconf_cb_id) {
        mdy_disp_blank_timeout = gconf_value_get_int(gcv);
        mdy_disp_lpm_on_timeout = mdy_disp_blank_timeout;

        /* Reprogram blanking timers */
        mdy_blanking_rethink_timers(true);

        /* Update inactivity timeout */
        execute_datapipe(&inactivity_timeout_pipe,
                         GINT_TO_POINTER(mdy_disp_dim_timeout +
                                         mdy_disp_blank_timeout),
                         USE_INDATA, CACHE_INDATA);
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
    else if (id == mdy_disp_dim_timeout_gconf_cb_id) {
        mdy_disp_dim_timeout = gconf_value_get_int(gcv);

        /* Find the closest match in the list of valid dim timeouts */
        mdy_dim_timeout_index = mdy_blanking_find_dim_timeout_index(mdy_disp_dim_timeout);
        mdy_adaptive_dimming_index = 0;

        /* Reprogram blanking timers */
        mdy_blanking_rethink_timers(true);

        /* Update inactivity timeout */
        execute_datapipe(&inactivity_timeout_pipe,
                         GINT_TO_POINTER(mdy_disp_dim_timeout +
                                         mdy_disp_blank_timeout),
                         USE_INDATA, CACHE_INDATA);
    }
    else if (id == mdy_blanking_inhibit_mode_gconf_cb_id) {
        mdy_blanking_inhibit_mode = gconf_value_get_int(gcv);

        /* force blanking reprogramming */
        mdy_blanking_rethink_timers(true);
    }
    else if( id == mdy_disp_never_blank_gconf_cb_id ) {
        mdy_disp_never_blank = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "never_blank = %d", mdy_disp_never_blank);
    }
    else if( id == mdy_lipstick_killer_core_delay_gconf_cb_id ) {
        mdy_lipstick_killer_core_delay = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "lipstick kill delay = %d", mdy_lipstick_killer_core_delay);
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }

EXIT:
    return;
}

static void mdy_gconf_sanitize_brightness_settings(void)
{
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

    /* Update config; signals will be emitted and config notifiers
     * called - mdy_gconf_cb() must ignore no-change notifications
     * to avoid recursive sanitation. */
    mce_gconf_set_int(MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_SIZE_PATH,
                      mdy_brightness_step_size);
    mce_gconf_set_int(MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_COUNT_PATH,
                      mdy_brightness_step_count);
    mce_gconf_set_int(MCE_GCONF_DISPLAY_BRIGHTNESS_PATH,
                      mdy_brightness_setting);

    mce_log(LL_DEBUG, "mdy_brightness_setting=%d", mdy_brightness_setting);

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
}

/** Get initial gconf valus and start tracking changes
 */
static void mdy_gconf_init(void)
{
    gulong tmp = 0;

    /* Display brightness settings */

    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_COUNT_PATH,
                           mdy_gconf_cb,
                           &mdy_brightness_step_count_gconf_id);
    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_SIZE_PATH,
                           mdy_gconf_cb,
                           &mdy_brightness_step_size_gconf_id);
    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_DISPLAY_BRIGHTNESS_PATH,
                           mdy_gconf_cb,
                           &mdy_brightness_setting_gconf_id);

    mce_gconf_get_int(MCE_GCONF_DISPLAY_BRIGHTNESS_PATH,
                      &mdy_brightness_setting);
    mce_gconf_get_int(MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_SIZE_PATH,
                      &mdy_brightness_step_size);
    mce_gconf_get_int(MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_COUNT_PATH,
                      &mdy_brightness_step_count);

    /* Migrate ranges, update hw dim/on brightness levels */
    mdy_gconf_sanitize_brightness_settings();

    /* If we can read the current hw brightness level, update the
     * cached brightness so we can do soft transitions from the
     * initial state */
    if( mdy_brightness_level_output.path &&
        mce_read_number_string_from_file(mdy_brightness_level_output.path,
                                              &tmp, NULL, FALSE, TRUE) ) {
        mdy_brightness_level_cached = tmp;
    }
    mce_log(LL_DEBUG, "mdy_brightness_level_cached=%d",
            mdy_brightness_level_cached);

    /* Display blank
     * Since we've set a default, error handling is unnecessary */
    mce_gconf_get_int(MCE_GCONF_DISPLAY_BLANK_TIMEOUT_PATH,
                      &mdy_disp_blank_timeout);

    mdy_disp_lpm_on_timeout = mdy_disp_blank_timeout;

    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_DISPLAY_BLANK_TIMEOUT_PATH,
                           mdy_gconf_cb,
                           &mdy_disp_blank_timeout_gconf_cb_id);

    /* Never blank
     * Since we've set a default, error handling is unnecessary */
    mce_gconf_get_int(MCE_GCONF_DISPLAY_NEVER_BLANK_PATH,
                      &mdy_disp_never_blank);
    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_DISPLAY_NEVER_BLANK_PATH,
                           mdy_gconf_cb,
                           &mdy_disp_never_blank_gconf_cb_id);

    /* Use adaptive display dim timeout
     * Since we've set a default, error handling is unnecessary */
    mce_gconf_get_bool(MCE_GCONF_DISPLAY_ADAPTIVE_DIMMING_PATH,
                       &mdy_adaptive_dimming_enabled);

    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_DISPLAY_ADAPTIVE_DIMMING_PATH,
                           mdy_gconf_cb,
                           &mdy_adaptive_dimming_enabled_gconf_cb_id);

    /* Possible dim timeouts */
    if( !mce_gconf_get_int_list(MCE_GCONF_DISPLAY_DIM_TIMEOUT_LIST_PATH,
                                &mdy_possible_dim_timeouts) ) {
        mce_log(LL_WARN, "no dim timeouts defined");
        // FIXME: use some built-in defaults
    }

    /* Adaptive display dimming threshold
     * Since we've set a default, error handling is unnecessary */
    mce_gconf_get_int(MCE_GCONF_DISPLAY_ADAPTIVE_DIM_THRESHOLD_PATH,
                      &mdy_adaptive_dimming_threshold);

    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_DISPLAY_ADAPTIVE_DIM_THRESHOLD_PATH,
                           mdy_gconf_cb,
                           &mdy_adaptive_dimming_threshold_gconf_cb_id);

    /* Display dim
     * Since we've set a default, error handling is unnecessary */
    mce_gconf_get_int(MCE_GCONF_DISPLAY_DIM_TIMEOUT_PATH,
                      &mdy_disp_dim_timeout);

    mdy_dim_timeout_index = mdy_blanking_find_dim_timeout_index(mdy_disp_dim_timeout);
    mdy_adaptive_dimming_index = 0;

    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_DISPLAY_DIM_TIMEOUT_PATH,
                           mdy_gconf_cb,
                           &mdy_disp_dim_timeout_gconf_cb_id);

    /* Update inactivity timeout */
    execute_datapipe(&inactivity_timeout_pipe,
                     GINT_TO_POINTER(mdy_disp_dim_timeout +
                                     mdy_disp_blank_timeout +
                                     mdy_additional_bootup_dim_timeout),
                     USE_INDATA, CACHE_INDATA);

    /* Use low power mode?
     * Since we've set a default, error handling is unnecessary */
    mce_gconf_get_bool(MCE_GCONF_USE_LOW_POWER_MODE_PATH,
                       &mdy_use_low_power_mode);

    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_USE_LOW_POWER_MODE_PATH,
                           mdy_gconf_cb,
                           &mdy_use_low_power_mode_gconf_cb_id);

    /* Don't blank on charger
     * Since we've set a default, error handling is unnecessary */
    mce_gconf_get_int(MCE_GCONF_BLANKING_INHIBIT_MODE_PATH,
                      &mdy_blanking_inhibit_mode);

    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_BLANKING_INHIBIT_MODE_PATH,
                           mdy_gconf_cb,
                           &mdy_blanking_inhibit_mode_gconf_cb_id);

    /* Delay for killing unresponsive lipstick */
    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_LIPSTICK_CORE_DELAY_PATH,
                           mdy_gconf_cb,
                           &mdy_lipstick_killer_core_delay_gconf_cb_id);
    mce_gconf_get_int(MCE_GCONF_LIPSTICK_CORE_DELAY_PATH,
                      &mdy_lipstick_killer_core_delay);
}

static void mdy_gconf_quit(void)
{
    // FIXME: actually remove change notifiers

    g_slist_free(mdy_possible_dim_timeouts), mdy_possible_dim_timeouts = 0;
}

/* ========================================================================= *
 * INIFILE_SETTINGS
 * ========================================================================= */

/** Fetch configuration values from mce.ini files
 */
static void mdy_config_init(void)
{
    gchar *str = NULL;

    /* brightness increase policy */
    str = mce_conf_get_string(MCE_CONF_DISPLAY_GROUP,
                              MCE_CONF_BRIGHTNESS_INCREASE_POLICY,
                              "");
    mdy_brightness_increase_policy =
        mce_translate_string_to_int_with_default(mdy_brightness_change_policy_translation,
                                                 str, DEFAULT_BRIGHTNESS_INCREASE_POLICY);
    g_free(str);

    /* brightness decrease policy */
    str = mce_conf_get_string(MCE_CONF_DISPLAY_GROUP,
                              MCE_CONF_BRIGHTNESS_DECREASE_POLICY,
                              "");
    mdy_brightness_decrease_policy =
        mce_translate_string_to_int_with_default(mdy_brightness_change_policy_translation,
                                                 str, DEFAULT_BRIGHTNESS_DECREASE_POLICY);
    g_free(str);

    /* brightness increase step time */
    mdy_brightness_increase_step_time =
        mce_conf_get_int(MCE_CONF_DISPLAY_GROUP,
                         MCE_CONF_STEP_TIME_INCREASE,
                         DEFAULT_BRIGHTNESS_INCREASE_STEP_TIME);

    /* brightness decrease step time */
    mdy_brightness_decrease_step_time =
        mce_conf_get_int(MCE_CONF_DISPLAY_GROUP,
                         MCE_CONF_STEP_TIME_DECREASE,
                         DEFAULT_BRIGHTNESS_DECREASE_STEP_TIME);

    /* brightness increase constant time */
    mdy_brightness_increase_constant_time =
        mce_conf_get_int(MCE_CONF_DISPLAY_GROUP,
                         MCE_CONF_CONSTANT_TIME_INCREASE,
                         DEFAULT_BRIGHTNESS_INCREASE_CONSTANT_TIME);

    /* brightness decrease constant time */
    mdy_brightness_decrease_constant_time =
        mce_conf_get_int(MCE_CONF_DISPLAY_GROUP,
                         MCE_CONF_CONSTANT_TIME_DECREASE,
                         DEFAULT_BRIGHTNESS_DECREASE_CONSTANT_TIME);
}

/* ========================================================================= *
 * MODULE_LOAD_UNLOAD
 * ========================================================================= */

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
    submode_t submode_fixme = mce_get_submode_int32();
    gulong tmp = 0;

    (void)module;

    /* Start dbus name tracking */
    mdy_nameowner_init();

    /* Initialise the display type and the relevant paths */
    (void)mdy_display_type_get();

#ifdef ENABLE_CPU_GOVERNOR
    /* Get CPU scaling governor settings from INI-files */
    mdy_governor_default = mdy_governor_get_settings("Default");
    mdy_governor_interactive = mdy_governor_get_settings("Interactive");

    /* Get cpu scaling governor configuration & track changes */
    mce_gconf_get_int(MCE_GCONF_CPU_SCALING_GOVERNOR_PATH,
                      &mdy_governor_conf);
    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_CPU_SCALING_GOVERNOR_PATH,
                           mdy_governor_conf_cb,
                           &mdy_governor_conf_id);

    /* Evaluate initial state */
    mdy_governor_rethink();
#endif

#ifdef ENABLE_WAKELOCKS
    /* Get autosuspend policy configuration & track changes */
    mce_gconf_get_int(MCE_GCONF_USE_AUTOSUSPEND_PATH,
                      &mdy_suspend_policy);
    mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
                           MCE_GCONF_USE_AUTOSUSPEND_PATH,
                           mdy_autosuspend_gconf_cb,
                           &mdy_suspend_policy_id);

    /* Evaluate initial state */
    mdy_stm_schedule_rethink();
#endif
    /* Start waiting for init_done state */
    mdy_flagfiles_start_tracking();

    if ((submode_fixme & MCE_TRANSITION_SUBMODE) != 0) {
        /* Disable bootup submode. It causes tklock problems if we don't */
        /* receive desktop_startup dbus notification */
        //mce_add_submode_int32(MCE_BOOTUP_SUBMODE);
        mdy_additional_bootup_dim_timeout = BOOTUP_DIM_ADDITIONAL_TIMEOUT;
    }
    else {
        mdy_additional_bootup_dim_timeout = 0;
    }

    /* Append triggers/filters to datapipes */
    mdy_datapipe_init();

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
        mdy_brightness_level_maximum = tmp;
    mce_log(LL_INFO, "max_brightness = %d", mdy_brightness_level_maximum);

    mdy_brightness_set_dim_level();
    mce_log(LL_INFO, "mdy_brightness_level_display_dim = %d", mdy_brightness_level_display_dim);

    mdy_cabc_mode_set(DEFAULT_CABC_MODE);

    /* Install dbus message handlers */
    mdy_dbus_init();

    /* Get initial gconf valus and start tracking changes */
    mdy_gconf_init();

    /* Fetch configuration values from mce.ini files */
    mdy_config_init();

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
    if( mdy_suspend_policy_id ) {
        mce_gconf_notifier_remove(GINT_TO_POINTER(mdy_suspend_policy_id), 0);
        mdy_suspend_policy_id = 0;
    }
#endif

#ifdef ENABLE_CPU_GOVERNOR
    /* Remove cpu scaling governor change notifier */
    if( mdy_governor_conf_id ) {
        mce_gconf_notifier_remove(GINT_TO_POINTER(mdy_governor_conf_id), 0);
        mdy_governor_conf_id = 0;
    }

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
    mdy_lipstick_killer_cancel();

    /* Cancel active asynchronous dbus method calls to avoid
     * callback functions with stale adresses getting invoked */
    mdy_renderer_cancel_state_set();

    /* Cancel pending state machine updates */
    mdy_stm_cancel_rethink();

    mdy_nameowner_quit();

    mdy_poweron_led_rethink_cancel();

    /* Remove callbacks on module unload */
    mce_sensorfw_orient_set_notify(0);

    g_free(mdy_lipstick_killer_name), mdy_lipstick_killer_name = 0;

    return;
}
