/**
 * @file powerkey.c
 * Power key logic for the Mode Control Entity
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright © 2014-2015 Jolla Ltd.
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

#include "powerkey.h"
#include "tklock.h"
#include "evdev.h"

#include "mce-log.h"
#include "mce-lib.h"
#include "mce-setting.h"
#include "mce-dbus.h"
#include "mce-dsme.h"

#include "modules/doubletap.h"

#include "systemui/dbus-names.h"

#ifdef ENABLE_WAKELOCKS
# include "libwakelock.h"
#endif

#include <linux/input.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include <libngf/ngf.h>

/* ========================================================================= *
 * OVERVIEW
 *
 * There is a predefined set of actions. Of these two are dbus actions
 * that by default make mce broadcast dbus signals, but can be configured
 * to make any dbus method call with optional string argument.
 *
 * Any combination of these actions can be bound to:
 * - single power key press
 * - double power key press
 * - long power key press
 *
 * The selected actions are executed in a fixed order and actions that
 * are common to both single and double press are executed immediately
 * after powerkey is released. This allows double press configuration
 * to extend what would be done with single press without causing
 * delays for single press handling.
 *
 * Separate combinations are used depending on whether the
 * display is on or off during the 1st power key press.
 *
 * The build-in defaults are as follows
 *
 * From display off:
 * - single press - turns display on
 * - double press - turns display on and hides lockscreen (but not device lock)
 * - long press   - does nothing
 *
 * From display on:
 * - single press - turns display off and activates locksreen
 * - double press - turns display off, activates locksreen and locks device
 * - long press   - initiates shutdown (if lockscreen is not active)
 *
 * Effectively this is just as before, except for the double press
 * actions to apply device lock / wake up to home screen.
 * ========================================================================= */

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * MISC_UTIL
 * ------------------------------------------------------------------------- */

/* Null tolerant string equality predicate
 *
 * @param s1 string
 * @param s2 string
 *
 * @return true if both s1 and s2 are null or same string, false otherwise
 */
static inline bool eq(const char *s1, const char *s2)
{
    return (s1 && s2) ? !strcmp(s1, s2) : (s1 == s2);
}

/** String is NULL or empty predicate
 */
static inline bool empty(const char *s)
{
    return s == 0 || *s == 0;
}

static char   *pwrkey_get_token(char **ppos);
static bool    pwrkey_create_flagfile(const char *path);
static bool    pwrkey_delete_flagfile(const char *path);

/* ------------------------------------------------------------------------- *
 * PS_OVERRIDE
 *
 * Provides escape from stuck proximity sensor.
 * ------------------------------------------------------------------------- */

/** [setting] Power key press count for proximity sensor override */
static gint  pwrkey_ps_override_count = MCE_DEFAULT_POWERKEY_PS_OVERRIDE_COUNT;
static guint pwrkey_ps_override_count_setting_id = 0;

/** [setting] Maximum time between power key presses for proximity sensor override */
static gint  pwrkey_ps_override_timeout = MCE_DEFAULT_POWERKEY_PS_OVERRIDE_TIMEOUT;
static guint pwrkey_ps_override_timeout_setting_id = 0;

static void  pwrkey_ps_override_evaluate(void);

/* ------------------------------------------------------------------------- *
 * ACTION_EXEC
 *
 * Individual actions that can be taken.
 * ------------------------------------------------------------------------- */

static gint  pwrkey_action_blank_mode = MCE_DEFAULT_POWERKEY_BLANKING_MODE;
static guint pwrkey_action_blank_mode_setting_id = 0;

static void  pwrkey_action_vibrate  (void);
static void  pwrkey_action_shutdown (void);
static void  pwrkey_action_tklock   (void);
static void  pwrkey_action_blank    (void);
static void  pwrkey_action_unblank  (void);
static void  pwrkey_action_tkunlock (void);
static void  pwrkey_action_devlock  (void);
static void  pwrkey_action_dbus1    (void);
static void  pwrkey_action_dbus2    (void);
static void  pwrkey_action_dbus3    (void);
static void  pwrkey_action_dbus4    (void);
static void  pwrkey_action_dbus5    (void);
static void  pwrkey_action_dbus6    (void);
static void  pwrkey_action_dbus7    (void);
static void  pwrkey_action_dbus8    (void);
static void  pwrkey_action_dbus9    (void);
static void  pwrkey_action_dbus10   (void);
static void  pwrkey_action_nop      (void);

/* ------------------------------------------------------------------------- *
 * ACTION_SETS
 *
 * Handle sets of individual actions.
 * ------------------------------------------------------------------------- */

typedef struct
{
    const char *name;
    void      (*func)(void);
} pwrkey_bitconf_t;

static void     pwrkey_mask_execute    (uint32_t mask);
static uint32_t pwrkey_mask_from_name  (const char *name);
static uint32_t pwrkey_mask_from_names (const char *names);
static gchar   *pwrkey_mask_to_names   (uint32_t mask);

/* ------------------------------------------------------------------------- *
 * GESTURE_FILTERING
 * ------------------------------------------------------------------------- */

/** Touchscreen gesture (doubletap etc) enable mode */
static gint  pwrkey_gestures_enable_mode = MCE_DEFAULT_DOUBLETAP_MODE;
static guint pwrkey_gestures_enable_mode_cb_id = 0;

static bool  pwrkey_gestures_allowed(bool synthesized);

/* ------------------------------------------------------------------------- *
 * ACTION_TRIGGERING
 * ------------------------------------------------------------------------- */

typedef struct
{
    /** Actions common to single and double press */
    uint32_t mask_common;

    /** Actions for single press */
    uint32_t mask_single;

    /** Actions for double press */
    uint32_t mask_double;

    /** Actions for long press */
    uint32_t mask_long;
} pwrkey_actions_t;

/** Actions when power key is pressed while display is on */
static pwrkey_actions_t pwrkey_actions_from_display_on  = { 0, 0, 0, 0 };

/** Actions when power key is pressed while display is off */
static pwrkey_actions_t pwrkey_actions_from_display_off = { 0, 0, 0, 0 };

/** Actions on touch screen gestures */
static pwrkey_actions_t pwrkey_actions_from_gesture[POWERKEY_ACTIONS_GESTURE_COUNT] = {};

/** Currently selected power key actions; default to turning display on */
static pwrkey_actions_t *pwrkey_actions_now =
    &pwrkey_actions_from_display_off;

static gchar *pwrkey_actions_single_on             = 0;
static guint  pwrkey_actions_single_on_setting_id  = 0;

static gchar *pwrkey_actions_double_on             = 0;
static guint  pwrkey_actions_double_on_setting_id  = 0;

static gchar *pwrkey_actions_long_on               = 0;
static guint  pwrkey_actions_long_on_setting_id    = 0;

static gchar *pwrkey_actions_single_off            = 0;
static guint  pwrkey_actions_single_off_setting_id = 0;

static gchar *pwrkey_actions_double_off            = 0;
static guint  pwrkey_actions_double_off_setting_id = 0;

static gchar *pwrkey_actions_long_off              = 0;
static guint  pwrkey_actions_long_off_setting_id   = 0;

/** Array of setting keys for configurable touchscreen gestures */
static const char * const pwrkey_actions_gesture_key[POWERKEY_ACTIONS_GESTURE_COUNT] =
{
    MCE_SETTING_POWERKEY_ACTIONS_GESTURE0,
    MCE_SETTING_POWERKEY_ACTIONS_GESTURE1,
    MCE_SETTING_POWERKEY_ACTIONS_GESTURE2,
    MCE_SETTING_POWERKEY_ACTIONS_GESTURE3,
    MCE_SETTING_POWERKEY_ACTIONS_GESTURE4,
    MCE_SETTING_POWERKEY_ACTIONS_GESTURE5,
    MCE_SETTING_POWERKEY_ACTIONS_GESTURE6,
    MCE_SETTING_POWERKEY_ACTIONS_GESTURE7,
    MCE_SETTING_POWERKEY_ACTIONS_GESTURE8,
    MCE_SETTING_POWERKEY_ACTIONS_GESTURE9,
    MCE_SETTING_POWERKEY_ACTIONS_GESTURE10,
};

/** Array of default values for configurable touchscreen gestures */
static const char * const pwrkey_actions_gesture_val[POWERKEY_ACTIONS_GESTURE_COUNT] =
{
    MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE0,
    MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE1,
    MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE2,
    MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE3,
    MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE4,
    MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE5,
    MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE6,
    MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE7,
    MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE8,
    MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE9,
    MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE10,
};

/** Array of current values for configurable touchscreen gestures */
static gchar *pwrkey_actions_gesture           [POWERKEY_ACTIONS_GESTURE_COUNT] = {};
static guint  pwrkey_actions_gesture_setting_id[POWERKEY_ACTIONS_GESTURE_COUNT] = {};

static void pwrkey_actions_parse           (pwrkey_actions_t *self, const char *names_single, const char *names_double, const char *names_long);

static void pwrkey_actions_do_gesture      (size_t gesture);
static void pwrkey_actions_do_common       (void);
static void pwrkey_actions_do_single_press (void);
static void pwrkey_actions_do_double_press (void);
static void pwrkey_actions_do_long_press   (void);
static bool pwrkey_actions_update          (const pwrkey_actions_t *self, gchar **names_single, gchar **names_double, gchar **names_long);

static bool pwrkey_actions_use_double_press(void);

static void pwrkey_actions_select          (bool display_is_on);

/* ------------------------------------------------------------------------- *
 * LONG_PRESS_TIMEOUT
 *
 * timer for telling apart short and long power key presses
 * ------------------------------------------------------------------------- */

static gint     pwrkey_long_press_delay = MCE_DEFAULT_POWERKEY_LONG_PRESS_DELAY;
static guint    pwrkey_long_press_delay_setting_id = 0;

static guint    pwrkey_long_press_timer_id = 0;

static gboolean pwrkey_long_press_timer_cb      (gpointer aptr);
static void     pwrkey_long_press_timer_start   (void);
static bool     pwrkey_long_press_timer_pending (void);
static bool     pwrkey_long_press_timer_cancel  (void);

/* ------------------------------------------------------------------------- *
 * DOUBLE_PRESS_TIMEOUT
 *
 * timer for telling apart single and double power key presses
 * ------------------------------------------------------------------------- */

static gint     pwrkey_double_press_delay = MCE_DEFAULT_POWERKEY_DOUBLE_PRESS_DELAY;
static guint    pwrkey_double_press_delay_setting_id = 0;

static guint    pwrkey_double_press_timer_id = 0;

static gboolean pwrkey_double_press_timer_cb(gpointer aptr);
static bool     pwrkey_double_press_timer_pending(void);
static bool     pwrkey_double_press_timer_cancel(void);
static void     pwrkey_double_press_timer_start(void);

/* ------------------------------------------------------------------------- *
 * NGFD_GLUE
 * ------------------------------------------------------------------------- */

static const char *xngf_state_repr    (NgfEventState state);
static void        xngf_status_cb     (NgfClient *client, uint32_t event_id, NgfEventState state, void *userdata);
static bool        xngf_create_client (void);
static void        xngf_delete_client (void);
static void        xngf_play_event    (const char *event_name);
static void        xngf_init          (void);
static void        xngf_quit          (void);

/* ------------------------------------------------------------------------- *
 * DBUS_ACTIONS
 *
 * emitting dbus signal from mce / making dbus method call to some service
 * ------------------------------------------------------------------------- */

/** Flag file for: Possibly dangerous dbus action in progress
 *
 * Used for resetting dbus action config if it causes mce to crash.
 *
 * Using tmpfs is problematic from permissions point of view, but we do
 * not want to cause flash wear by this either.
 */
static const char pwrkey_dbus_action_flag[]  =
    "/tmp/mce-powerkey-dbus-action.flag";

typedef struct
{
    const char *setting_key;
    const char *setting_def;
    gchar      *setting_val;
    guint       setting_id;

    char *destination;
    char *object;
    char *interface;
    char *member;
    char *argument;
} pwrkey_dbus_action_t;

static pwrkey_dbus_action_t pwrkey_dbus_action[POWEKEY_DBUS_ACTION_COUNT] =
{
    {
        .setting_key = MCE_SETTING_POWERKEY_DBUS_ACTION1,
        .setting_def = MCE_DEFAULT_POWERKEY_DBUS_ACTION1,
        .setting_val = 0,
        .setting_id  = 0,
    },
    {
        .setting_key = MCE_SETTING_POWERKEY_DBUS_ACTION2,
        .setting_def = MCE_DEFAULT_POWERKEY_DBUS_ACTION2,
        .setting_val = 0,
        .setting_id  = 0,
    },
    {
        .setting_key = MCE_SETTING_POWERKEY_DBUS_ACTION3,
        .setting_def = MCE_DEFAULT_POWERKEY_DBUS_ACTION3,
        .setting_val = 0,
        .setting_id  = 0,
    },
    {
        .setting_key = MCE_SETTING_POWERKEY_DBUS_ACTION4,
        .setting_def = MCE_DEFAULT_POWERKEY_DBUS_ACTION4,
        .setting_val = 0,
        .setting_id  = 0,
    },
    {
        .setting_key = MCE_SETTING_POWERKEY_DBUS_ACTION5,
        .setting_def = MCE_DEFAULT_POWERKEY_DBUS_ACTION5,
        .setting_val = 0,
        .setting_id  = 0,
    },
    {
        .setting_key = MCE_SETTING_POWERKEY_DBUS_ACTION6,
        .setting_def = MCE_DEFAULT_POWERKEY_DBUS_ACTION6,
        .setting_val = 0,
        .setting_id  = 0,
    },
    {
        .setting_key = MCE_SETTING_POWERKEY_DBUS_ACTION7,
        .setting_def = MCE_DEFAULT_POWERKEY_DBUS_ACTION7,
        .setting_val = 0,
        .setting_id  = 0,
    },
    {
        .setting_key = MCE_SETTING_POWERKEY_DBUS_ACTION8,
        .setting_def = MCE_DEFAULT_POWERKEY_DBUS_ACTION8,
        .setting_val = 0,
        .setting_id  = 0,
    },
    {
        .setting_key = MCE_SETTING_POWERKEY_DBUS_ACTION9,
        .setting_def = MCE_DEFAULT_POWERKEY_DBUS_ACTION9,
        .setting_val = 0,
        .setting_id  = 0,
    },
    {
        .setting_key = MCE_SETTING_POWERKEY_DBUS_ACTION10,
        .setting_def = MCE_DEFAULT_POWERKEY_DBUS_ACTION10,
        .setting_val = 0,
        .setting_id  = 0,
    },
};

static void   pwrkey_dbus_action_clear(pwrkey_dbus_action_t *self);
static void   pwrkey_dbus_action_reset(pwrkey_dbus_action_t *self);
static bool   pwrkey_dbus_action_is_methodcall(const pwrkey_dbus_action_t *self);
static bool   pwrkey_dbus_action_is_signal(const pwrkey_dbus_action_t *self);
static void   pwrkey_dbus_action_parse(pwrkey_dbus_action_t *self);
static gchar *pwrkey_dbus_action_to_string(const pwrkey_dbus_action_t *self);
static void   pwrkey_dbus_action_sanitize(pwrkey_dbus_action_t *self);
static void   pwrkey_dbus_action_configure(size_t action_id, bool force_reset);
static void   pwrkey_dbus_action_execute(size_t index);

/* ------------------------------------------------------------------------- *
 * POWER_KEY_STATE_MACHINE
 *
 * main logic for tracking power key presses and associated timers
 *
 * state transition graph can be generated from "powerkey.dot" file
 * ------------------------------------------------------------------------- */

/** Diplay state when power key was pressed */
static display_state_t pwrkey_stm_display_state = MCE_DISPLAY_UNDEF;

/** [setting] Power key press enable mode */
static gint  pwrkey_stm_enable_mode = MCE_DEFAULT_POWERKEY_MODE;
static guint pwrkey_stm_enable_mode_setting_id = 0;

static void pwrkey_stm_long_press_timeout   (void);
static void pwrkey_stm_double_press_timeout (void);
static void pwrkey_stm_powerkey_pressed     (void);
static void pwrkey_stm_powerkey_released    (void);

static bool pwrkey_stm_ignore_action        (void);
static bool pwrkey_stm_pending_timers       (void);

static void pwrkey_stm_rethink_wakelock     (void);

static void pwrkey_stm_store_initial_state  (void);
static void pwrkey_stm_terminate            (void);

/* ------------------------------------------------------------------------- *
 * HOME_KEY_STATE_MACHINE
 * ------------------------------------------------------------------------- */

typedef enum
{
    HOMEKEY_STM_WAIT_PRESS   = 0,
    HOMEKEY_STM_WAIT_UNBLANK = 1,
    HOMEKEY_STM_SEND_SIGNAL  = 2,
    HOMEKEY_STM_WAIT_RELEASE = 3,
} homekey_stm_t;

static const char *homekey_stm_repr        (homekey_stm_t state);
static void        homekey_stm_set_state   (homekey_stm_t state);
static bool        homekey_stm_exec_step   (void);
static void        homekey_stm_eval_state  (void);
static void        homekey_stm_set_pressed (bool pressed);

/* ------------------------------------------------------------------------- *
 * DBUS_IPC
 *
 * handling incoming and outgoing dbus messages
 * ------------------------------------------------------------------------- */

static void     pwrkey_dbus_send_signal(const char *sig, const char *arg);

static gboolean pwrkey_dbus_trigger_event_cb(DBusMessage *const req);
static gboolean pwrkey_dbus_ignore_incoming_call_cb(DBusMessage *const req);

static void     pwrkey_dbus_init(void);
static void     pwrkey_dbus_quit(void);

/* ------------------------------------------------------------------------- *
 * DYNAMIC_SETTINGS
 *
 * tracking powerkey related runtime changeable settings
 * ------------------------------------------------------------------------- */

static gint     pwrkey_setting_sanitize_id = 0;

static void     pwrkey_setting_sanitize_action_masks(void);
static void     pwrkey_setting_sanitize_dbus_actions(void);

static gboolean pwrkey_setting_sanitize_cb     (gpointer aptr);
static void     pwrkey_setting_sanitize_now    (void);
static void     pwrkey_setting_sanitize_later  (void);
static void     pwrkey_setting_sanitize_cancel (void);

static bool     pwrkey_setting_handle_gesture  (const GConfValue *gcv, guint id);
static void     pwrkey_setting_cb              (GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);

static void     pwrkey_setting_init            (void);
static void     pwrkey_setting_quit            (void);

/* ------------------------------------------------------------------------- *
 * DATAPIPE_HANDLING
 *
 * reacting to state changes / input from other mce modules
 * ------------------------------------------------------------------------- */

static void pwrkey_datapipes_keypress_event_cb(gconstpointer const data);
static void pwrkey_datapipe_ngfd_service_state_cb(gconstpointer data);
static void pwrkey_datapipe_system_state_cb(gconstpointer data);
static void pwrkey_datapipe_display_state_curr_cb(gconstpointer data);
static void pwrkey_datapipe_display_state_next_cb(gconstpointer data);
static void pwrkey_datapipe_lid_sensor_filtered_cb(gconstpointer data);
static void pwrkey_datapipe_proximity_sensor_actual_cb(gconstpointer data);
static void pwrkey_datapipe_call_state_cb(gconstpointer data);
static void pwrkey_datapipe_alarm_ui_state_cb(gconstpointer data);

static void pwrkey_datapipes_init(void);
static void pwrkey_datapipes_quit(void);

/* ------------------------------------------------------------------------- *
 * MODULE_INTEFACE
 * ------------------------------------------------------------------------- */

gboolean mce_powerkey_init(void);
void mce_powerkey_exit(void);

/* ========================================================================= *
 * MISC_UTIL
 * ========================================================================= */

/** Parse element from comma separated string list
 */
static char *
pwrkey_get_token(char **ppos)
{
    char *pos = *ppos;
    char *beg = pos;

    if( !pos )
        goto cleanup;

    for( ; *pos; ++pos ) {
        if( *pos != ',' )
            continue;
        *pos++ = 0;
        break;
    }

cleanup:
    return *ppos = pos, beg;
}

/** Create an empty flag file
 *
 * @param path Path to the file to create
 *
 * @return true if file was created, false otherwise
 */
static bool pwrkey_create_flagfile(const char *path)
{
    bool created = false;

    int fd = open(path, O_WRONLY|O_CREAT|O_EXCL|O_TRUNC, 0666);
    if( fd != -1 ) {
        close(fd);
        created = true;
    }

    return created;
}

/** Delete a flag file
 *
 * @param path Path to the file to create
 *
 * @return true if file was removed, false otherwise
 */

static bool pwrkey_delete_flagfile(const char *path)
{
    bool deleted = false;

    if( unlink(path) == 0 ) {
        deleted = true;
    }

    return deleted;
}

/* ========================================================================= *
 * DATAPIPE_HANDLING
 * ========================================================================= */

/** System state; is undefined at bootup, can't assume anything */
static system_state_t system_state = MCE_SYSTEM_STATE_UNDEF;

/** Current display state; undefined initially, can't assume anything */
static display_state_t display_state_curr = MCE_DISPLAY_UNDEF;

/** Next Display state; undefined initially, can't assume anything */
static display_state_t display_state_next = MCE_DISPLAY_UNDEF;

/** Lid cover policy state; assume unknown */
static cover_state_t lid_sensor_filtered = COVER_UNDEF;

/** Actual proximity state; assume not covered */
static cover_state_t proximity_sensor_actual = COVER_OPEN;

/** NGFD availability */
static service_state_t ngfd_service_state = SERVICE_STATE_UNDEF;

/** Cached alarm ui state */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_OFF_INT32;

/** Cached call state */
static call_state_t call_state = CALL_STATE_NONE;

/** devicelock dbus name is reserved; assume unknown */
static service_state_t devicelock_service_state = SERVICE_STATE_UNDEF;

/** Cached ongoing fingerprint enroll; assume not */
static bool enroll_in_progress = false;

/** Use powerkey for blanking during incoming calls */
static bool pwrkey_ignore_incoming_call = false;

/* ========================================================================= *
 * PS_OVERRIDE
 * ========================================================================= */

/** Provide an emergency way out from stuck proximity sensor
 *
 * If the proximity sensor is dirty/faulty and stuck to "covered"
 * state, it can leave the phone in a state where it is impossible
 * to do anything about incoming call, ringing alarm.
 *
 * To offer somekind of remedy for the situation, this function
 * allows user to force proximity sensor to "uncovered" state
 * by rapidly pressing power button several times.
 */
static void
pwrkey_ps_override_evaluate(void)
{
    static int64_t t_last  = 0;
    static gint    count   = 0;

    /* If the feature is disabled, just reset the counter */
    if( pwrkey_ps_override_count   <= 0 ||
        pwrkey_ps_override_timeout <= 0 ) {
        t_last = 0, count = 0;
        goto EXIT;
    }

    /* If neither sensor is not covered, just reset the counter */
    if( proximity_sensor_actual != COVER_CLOSED &&
        lid_sensor_filtered != COVER_CLOSED ) {
        t_last = 0, count = 0;
        goto EXIT;
    }

    int64_t t_now = mce_lib_get_boot_tick();

    /* If the previous power key press was too far in
     * the past, start counting from zero again */

    if( t_now > t_last + pwrkey_ps_override_timeout ) {
        mce_log(LL_DEBUG, "ps override count restarted");
        count = 0;
    }

    t_last = t_now;

    /* If configured number of power key presses within the time
     * limits has been reached, force proximity sensor state to
     * "uncovered".
     *
     * This should allow touch input ungrabbing and turning
     * display on during incoming call / alarm.
     *
     * If sensor gets unstuck and new proximity readings are
     * received, this override will be automatically undone.
     */

    if( ++count != pwrkey_ps_override_count ) {
        mce_log(LL_DEBUG, "ps override count = %d", count);
        goto EXIT;
    }

    if( proximity_sensor_actual == COVER_CLOSED ) {
        mce_log(LL_CRIT, "assuming stuck proximity sensor;"
                " faking uncover event");

        /* Force cached proximity state to "open" */
        datapipe_exec_full(&proximity_sensor_actual_pipe,
                           GINT_TO_POINTER(COVER_OPEN),
                           USE_INDATA, CACHE_INDATA);
    }

    if( lid_sensor_filtered == COVER_CLOSED ) {
        mce_log(LL_CRIT, "assuming stuck lid sensor;"
                " resetting validation data");

        /* Reset lid sensor validation data */
        datapipe_exec_full(&lid_sensor_is_working_pipe,
                           GINT_TO_POINTER(false),
                           USE_INDATA, CACHE_INDATA);
    }

    t_last = 0, count = 0;

EXIT:

    return;
}

/* ========================================================================= *
 * ACTION_EXEC
 * ========================================================================= */

static void
pwrkey_action_vibrate(void)
{
    mce_log(LL_DEBUG, "Requesting vibrate");
    xngf_play_event("pwrkey");
}

static void
pwrkey_action_shutdown(void)
{
    submode_t submode = mce_get_submode_int32();

    /* Do not shutdown if the tklock is active */
    if( submode & MCE_SUBMODE_TKLOCK )
        goto EXIT;

    mce_log(LL_DEVEL, "Requesting shutdown");
    mce_dsme_request_normal_shutdown();

EXIT:
    return;
}

static void
pwrkey_action_tklock(void)
{
    tklock_request_t request = TKLOCK_REQUEST_ON;
    mce_datapipe_request_tklock(request);
}

static void
pwrkey_action_tkunlock(void)
{
    /* Only unlock if we are in/entering fully powered on display state */
    switch( display_state_next ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        break;

    default:
        goto EXIT;
    }

    tklock_request_t request = TKLOCK_REQUEST_OFF;

    /* Even if powerkey actions are allowed to work while proximity
     * sensor is covered, we must not deactivatie the lockscreen */
    if( proximity_sensor_actual != COVER_OPEN ) {
        mce_log(LL_DEBUG, "Proximity sensor %s; rejecting tklock=%s",
                proximity_state_repr(proximity_sensor_actual),
                tklock_request_repr(request));
        goto EXIT;
    }

    mce_datapipe_request_tklock(request);
EXIT:
    return;
}

static void
pwrkey_action_blank(void)
{
    display_state_t request = MCE_DISPLAY_OFF;

    switch( pwrkey_action_blank_mode ) {
    case PWRKEY_BLANK_TO_LPM:
        request = MCE_DISPLAY_LPM_ON;
        break;

    case PWRKEY_BLANK_TO_OFF:
    default:
            break;
    }

    mce_log(LL_DEBUG, "Requesting display=%s",
            display_state_repr(request));
    mce_datapipe_request_display_state(request);
}

static void
pwrkey_action_unblank(void)
{
    /* Special case: Even if incoming call is beeing ignored, do not
     * allow unblanking via power key while proximity sensor is covered.
     *
     * Silencing ringing via pressing the power key through fabric
     * of a pocket easily leads to several power key presses getting
     * emitted and we do not want the display to get activated by such
     * activity.
     */
    if( call_state == CALL_STATE_RINGING ) {
        if( !pwrkey_ignore_incoming_call ) {
            mce_log(LL_DEVEL, "skip unblank; incoming call not ignored");
            goto EXIT;
        }

        if( proximity_sensor_actual != COVER_OPEN ) {
            mce_log(LL_DEVEL, "skip unblank; proximity covered/unknown");
            goto EXIT;
        }
    }

    display_state_t request = MCE_DISPLAY_ON;
    mce_log(LL_DEBUG, "Requesting display=%s",
            display_state_repr(request));
    mce_tklock_unblank(request);

EXIT:
    return;
}

static void
pwrkey_action_devlock(void)
{
    static const char service[]   = DEVICELOCK_SERVICE;
    static const char object[]    = DEVICELOCK_REQUEST_PATH;
    static const char interface[] = DEVICELOCK_REQUEST_IF;
    static const char method[]    = "setState";
    dbus_int32_t      request     = DEVICELOCK_STATE_LOCKED;

    if( devicelock_service_state != SERVICE_STATE_RUNNING ) {
        mce_log(LL_WARN, "devicelock service state is %s; skip %s request",
                service_state_repr(devicelock_service_state),
                devicelock_state_repr(request));
        goto EXIT;
    }

    mce_log(LL_DEBUG, "Requesting devicelock=%s",
            devicelock_state_repr(request));

    dbus_send(service, object, interface, method, 0,
              DBUS_TYPE_INT32, &request,
              DBUS_TYPE_INVALID);
EXIT:
    return;
}

static void
pwrkey_action_dbus1(void)
{
    pwrkey_dbus_action_execute(0);
}

static void
pwrkey_action_dbus2(void)
{
    pwrkey_dbus_action_execute(1);
}

static void
pwrkey_action_dbus3(void)
{
    pwrkey_dbus_action_execute(2);
}

static void
pwrkey_action_dbus4(void)
{
    pwrkey_dbus_action_execute(3);
}

static void
pwrkey_action_dbus5(void)
{
    pwrkey_dbus_action_execute(4);
}

static void
pwrkey_action_dbus6(void)
{
    pwrkey_dbus_action_execute(5);
}

static void
pwrkey_action_dbus7(void)
{
    pwrkey_dbus_action_execute(6);
}

static void
pwrkey_action_dbus8(void)
{
    pwrkey_dbus_action_execute(7);
}

static void
pwrkey_action_dbus9(void)
{
    pwrkey_dbus_action_execute(8);
}

static void
pwrkey_action_dbus10(void)
{
    pwrkey_dbus_action_execute(9);
}

static void
pwrkey_action_nop(void)
{
    /* Do nothing */
}

/* ========================================================================= *
 * ACTION_SETS
 * ========================================================================= */

/** Config string to callback function mapping
 *
 * The configured actions are executed in order defined by this array.
 *
 * This is needed for determining actions that common to both single and
 * double press handling.
 */
static const pwrkey_bitconf_t pwrkey_action_lut[] =
{
    // Direction: ON->OFF
    {
        .name = "blank",
        .func = pwrkey_action_blank,
    },
    {
        .name = "tklock",
        .func = pwrkey_action_tklock,
    },
    {
        .name = "devlock",
        .func = pwrkey_action_devlock,
    },
    {
        .name = "shutdown",
        .func = pwrkey_action_shutdown,
    },
    {
        .name = "vibrate",
        .func = pwrkey_action_vibrate,
    },

    // Direction: OFF->ON
    {
        .name = "unblank",
        .func = pwrkey_action_unblank,
    },
    {
        .name = "tkunlock",
        .func = pwrkey_action_tkunlock,
    },

    // D-Bus actions
    {
        .name = "dbus1",
        .func = pwrkey_action_dbus1,
    },
    {
        .name = "dbus2",
        .func = pwrkey_action_dbus2,
    },
    {
        .name = "dbus3",
        .func = pwrkey_action_dbus3,
    },
    {
        .name = "dbus4",
        .func = pwrkey_action_dbus4,
    },
    {
        .name = "dbus5",
        .func = pwrkey_action_dbus5,
    },
    {
        .name = "dbus6",
        .func = pwrkey_action_dbus6,
    },
    {
        .name = "dbus7",
        .func = pwrkey_action_dbus7,
    },
    {
        .name = "dbus8",
        .func = pwrkey_action_dbus8,
    },
    {
        .name = "dbus9",
        .func = pwrkey_action_dbus9,
    },
    {
        .name = "dbus10",
        .func = pwrkey_action_dbus10,
    },

    // Low priority placeholder/dummy action
    {
        .name = "nop",
        .func = pwrkey_action_nop,
    },
};

static void
pwrkey_mask_execute(uint32_t mask)
{
    for( size_t i = 0; i < G_N_ELEMENTS(pwrkey_action_lut); ++i ) {
        if( mask & (1u << i) ) {
            mce_log(LL_DEBUG, "* exec(%s)", pwrkey_action_lut[i].name);
            pwrkey_action_lut[i].func();
        }
    }
}

static uint32_t
pwrkey_mask_from_name(const char *name)
{
    uint32_t mask = 0;
    for( size_t i = 0; i < G_N_ELEMENTS(pwrkey_action_lut); ++i ) {
        if( strcmp(pwrkey_action_lut[i].name, name) )
            continue;
        mask |= 1u << i;
        break;
    }
    return mask;
}

static uint32_t
pwrkey_mask_from_names(const char *names)
{
    uint32_t  mask = 0;
    char     *work = 0;
    char     *pos;
    char     *end;

    if( !names )
        goto EXIT;

    if( !(work = strdup(names)) )
        goto EXIT;

    for( pos = work; pos; pos = end ) {
        if( (end = strchr(pos, ',')) )
            *end++ = 0;
        mask |= pwrkey_mask_from_name(pos);
    }

EXIT:
    free(work);

    return mask;
}

static gchar *
pwrkey_mask_to_names(uint32_t mask)
{
    char tmp[256];
    char *pos = tmp;
    char *end = tmp + sizeof tmp - 1;

    auto void add(const char *str)
    {
        while( pos < end && *str )
            *pos++ = *str++;
    };

    for( size_t i = 0; i < G_N_ELEMENTS(pwrkey_action_lut); ++i ) {
        if( mask & (1u << i) ) {
            if( pos > tmp )
                add(",");
            add(pwrkey_action_lut[i].name);
        }
    }
    *pos = 0;

    return g_strdup(tmp);
}

/* ------------------------------------------------------------------------- *
 * GESTURE_FILTERING
 * ------------------------------------------------------------------------- */

/** Predicate for: touchscreen gesture actions are allowed
 */
static bool
pwrkey_gestures_allowed(bool synthesized)
{
    bool allowed = false;

    /* Check enable setting */
    switch( pwrkey_gestures_enable_mode ) {
    case DBLTAP_ENABLE_ALWAYS:
        break;

    case DBLTAP_ENABLE_NEVER:
        if( !synthesized ) {
            mce_log(LL_DEVEL, "[gesture] ignored due to setting=never");
            goto EXIT;
        }
        /* Synthesized events (e.g. double tap from lpm) are implicitly
         * subjected to proximity rules.
         *
         * Fall through */

    default:
    case DBLTAP_ENABLE_NO_PROXIMITY:
        if( lid_sensor_filtered == COVER_CLOSED ) {
            mce_log(LL_DEVEL, "[gesture] ignored due to lid=closed");
            goto EXIT;
        }
        if( proximity_sensor_actual == COVER_CLOSED ) {
            mce_log(LL_DEVEL, "[gesture] ignored due to proximity");
            goto EXIT;
        }
        break;
    }

    switch( system_state ) {
    case MCE_SYSTEM_STATE_USER:
    case MCE_SYSTEM_STATE_ACTDEAD:
      break;
    default:
      mce_log(LL_DEVEL, "[gesture] ignored due to system state");
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
        mce_log(LL_DEVEL, "[gesture] ignored due to display state");
        goto EXIT;
    }

    allowed = true;

EXIT:

    return allowed;
}

/* ========================================================================= *
 * ACTION_TRIGGERING
 * ========================================================================= */

static void
pwrkey_actions_do_gesture(size_t gesture)
{
    /* Check settings, proximity sensor state, etc */
    if( !pwrkey_gestures_allowed(gesture & GESTURE_SYNTHESIZED) )
        goto EXIT;

    /* Remove modifier bits */
    gesture &= ~GESTURE_SYNTHESIZED;

    /* Treat unconfigurable gestures as doubletaps */
    if( gesture >= POWERKEY_ACTIONS_GESTURE_COUNT )
        gesture = GESTURE_DOUBLETAP;

    pwrkey_mask_execute(pwrkey_actions_from_gesture[gesture].mask_single);

EXIT:
    return;
}

static void
pwrkey_actions_do_common(void)
{
    pwrkey_mask_execute(pwrkey_actions_now->mask_common);
}

static void
pwrkey_actions_do_single_press(void)
{
    pwrkey_mask_execute(pwrkey_actions_now->mask_single);
}

static bool
pwrkey_actions_use_double_press(void)
{
    return pwrkey_actions_now->mask_double != 0;
}

static void
pwrkey_actions_do_double_press(void)
{
    pwrkey_mask_execute(pwrkey_actions_now->mask_double);
}

static void
pwrkey_actions_do_long_press(void)
{
    /* The action configuration applies only in the USER mode */

    switch( system_state ) {
    case MCE_SYSTEM_STATE_SHUTDOWN:
    case MCE_SYSTEM_STATE_REBOOT:
        /* Ignore if we're already shutting down/rebooting */
        break;

    case MCE_SYSTEM_STATE_ACTDEAD:
        /* Activate power on led pattern and power up to user mode*/
        mce_log(LL_DEBUG, "activate MCE_LED_PATTERN_POWER_ON");
        datapipe_exec_output_triggers(&led_pattern_activate_pipe,
                                      MCE_LED_PATTERN_POWER_ON,
                                      USE_INDATA);
        mce_dsme_request_powerup();
        break;

    case MCE_SYSTEM_STATE_USER:
        /* Apply configured actions */
        pwrkey_mask_execute(pwrkey_actions_now->mask_long);
        break;

    default:
        /* Default to powering off */
        mce_log(LL_WARN, "Requesting shutdown from state: %s",
                system_state_repr(system_state));
        mce_dsme_request_normal_shutdown();
        break;
    }
}

static bool
pwrkey_actions_update(const pwrkey_actions_t *self,
                      gchar **names_single,
                      gchar **names_double,
                      gchar **names_long)
{
    bool changed = false;

    auto void update(gchar **prev, gchar *curr)
    {
        if( prev && !eq(*prev, curr) )
            changed = true, g_free(*prev), *prev = curr, curr = 0;
        g_free(curr);
    }

    update(names_single,
           pwrkey_mask_to_names(self->mask_single | self->mask_common));

    update(names_double,
           pwrkey_mask_to_names(self->mask_double | self->mask_common));

    update(names_long,
           pwrkey_mask_to_names(self->mask_long));

    return changed;
}

static void
pwrkey_actions_parse(pwrkey_actions_t *self,
                     const char *names_single,
                     const char *names_double,
                     const char *names_long)
{
    /* Parse from configuration strings */
    self->mask_common = 0;
    self->mask_single = pwrkey_mask_from_names(names_single);
    self->mask_double = pwrkey_mask_from_names(names_double);
    self->mask_long   = pwrkey_mask_from_names(names_long);

    /* Separate leading actions that are common to both
     * single and double press */
    uint32_t diff = self->mask_single ^ self->mask_double;
    uint32_t mask = (diff - 1) & ~diff;
    uint32_t comm = self->mask_single & self->mask_double & mask;

    self->mask_common |=  comm;
    self->mask_single &= ~comm;
    self->mask_double &= ~comm;
}

static void pwrkey_actions_select(bool display_is_on)
{
    if( display_is_on )
        pwrkey_actions_now = &pwrkey_actions_from_display_on;
    else
        pwrkey_actions_now = &pwrkey_actions_from_display_off;
}

/* ========================================================================= *
 * LONG_PRESS_TIMEOUT
 * ========================================================================= */

static gboolean pwrkey_long_press_timer_cb(gpointer aptr)
{
    (void) aptr;

    if( !pwrkey_long_press_timer_id )
        goto EXIT;

    pwrkey_long_press_timer_id = 0;

    pwrkey_stm_long_press_timeout();

    pwrkey_stm_rethink_wakelock();

EXIT:

    return FALSE;
}

static bool
pwrkey_long_press_timer_pending(void)
{
    return pwrkey_long_press_timer_id != 0;
}

static bool
pwrkey_long_press_timer_cancel(void)
{
    bool canceled = false;
    if( pwrkey_long_press_timer_id ) {
        g_source_remove(pwrkey_long_press_timer_id),
            pwrkey_long_press_timer_id = 0;
        canceled = true;
    }
    return canceled;
}

static void
pwrkey_long_press_timer_start(void)
{
    pwrkey_long_press_timer_cancel();

    pwrkey_long_press_timer_id = g_timeout_add(pwrkey_long_press_delay,
                                               pwrkey_long_press_timer_cb, 0);
}

/* ========================================================================= *
 * DOUBLE_PRESS_TIMEOUT
 * ========================================================================= */

static gboolean pwrkey_double_press_timer_cb(gpointer aptr)
{
    (void) aptr;

    if( !pwrkey_double_press_timer_id )
        goto EXIT;

    pwrkey_double_press_timer_id = 0;

    pwrkey_stm_double_press_timeout();

    pwrkey_stm_rethink_wakelock();

EXIT:

    return FALSE;
}

static bool
pwrkey_double_press_timer_pending(void)
{
    return pwrkey_double_press_timer_id != 0;
}

static bool
pwrkey_double_press_timer_cancel(void)
{
    bool canceled = false;
    if( pwrkey_double_press_timer_id ) {
        g_source_remove(pwrkey_double_press_timer_id),
            pwrkey_double_press_timer_id = 0;
        canceled = true;
    }
    return canceled;
}

static void
pwrkey_double_press_timer_start(void)
{
    pwrkey_double_press_timer_cancel();

    pwrkey_double_press_timer_id = g_timeout_add(pwrkey_double_press_delay,
                                                 pwrkey_double_press_timer_cb, 0);
}

/* ========================================================================= *
 * DBUS_ACTIONS
 * ========================================================================= */

static void
pwrkey_dbus_action_clear(pwrkey_dbus_action_t *self)
{
    free(self->destination), self->destination = 0;
    free(self->object),      self->object      = 0;
    free(self->interface),   self->interface   = 0;
    free(self->member),      self->member      = 0;
    free(self->argument),    self->argument    = 0;
}

static void
pwrkey_dbus_action_reset(pwrkey_dbus_action_t *self)
{
    pwrkey_dbus_action_clear(self);

    /* Builtin default is always just a signal arg, no parsing required */
    self->argument = strdup(self->setting_def);
}

static bool
pwrkey_dbus_action_is_methodcall(const pwrkey_dbus_action_t *self)
{
    bool valid = false;

    if( empty(self->destination) ||
        empty(self->object)      ||
        empty(self->interface)   ||
        empty(self->member) ) {
        goto cleanup;
    }

    if( !dbus_validate_bus_name(self->destination, 0) ||
        !dbus_validate_path(self->object, 0)          ||
        !dbus_validate_interface(self->interface, 0)  ||
        !dbus_validate_member(self->member, 0) ) {
        goto cleanup;
    }

    if( !empty(self->argument) && !dbus_validate_utf8(self->argument, 0) )
        goto cleanup;

    valid = true;

cleanup:

    return valid;
}

static bool
pwrkey_dbus_action_is_signal(const pwrkey_dbus_action_t *self)
{
    bool valid = false;

    // must have an argument
    if( empty(self->argument) )
        goto cleanup;

    // ... and only the argument
    if( !empty(self->destination) ||
        !empty(self->object)      ||
        !empty(self->interface)   ||
        !empty(self->member) ) {
        goto cleanup;
    }

    // which needs to be valid utf8 string
    if( !dbus_validate_utf8(self->argument, 0) )
        goto cleanup;

    valid = true;

cleanup:

    return valid;
}

static gchar *
pwrkey_dbus_action_to_string(const pwrkey_dbus_action_t *self)
{
    gchar *res = 0;

    if( pwrkey_dbus_action_is_signal(self) ) {
        res = g_strdup(self->argument);
    }
    else if( pwrkey_dbus_action_is_methodcall(self) ) {
        res = g_strdup_printf("%s,%s,%s,%s,%s",
                              self->destination ?: "",
                              self->object      ?: "",
                              self->interface   ?: "",
                              self->member      ?: "",
                              self->argument    ?: "");
    }

    return res;
}

static void
pwrkey_dbus_action_parse(pwrkey_dbus_action_t *self)
{
    char *tmp = 0;
    char *pos = 0;
    char *arg = 0;

    pwrkey_dbus_action_clear(self);

    if( empty(self->setting_val) )
        goto cleanup;

    pos = tmp = strdup(self->setting_val);
    arg = pwrkey_get_token(&pos);

    if( *arg && !*pos ) {
        self->argument    = strdup(arg);
    }
    else {
        self->destination = strdup(arg);
        self->object      = strdup(pwrkey_get_token(&pos));
        self->interface   = strdup(pwrkey_get_token(&pos));
        self->member      = strdup(pwrkey_get_token(&pos));
        self->argument    = strdup(pwrkey_get_token(&pos));
    }

cleanup:
    free(tmp);
}

static void
pwrkey_dbus_action_sanitize(pwrkey_dbus_action_t *self)
{
    if( !pwrkey_dbus_action_is_methodcall(self) &&
        !pwrkey_dbus_action_is_signal(self) ) {
        pwrkey_dbus_action_reset(self);
    }
}

static void
pwrkey_dbus_action_configure(size_t action_id, bool force_reset)
{
    gchar *use = 0;

    if( action_id >= POWEKEY_DBUS_ACTION_COUNT )
        goto cleanup;

    pwrkey_dbus_action_t *action = pwrkey_dbus_action + action_id;

    if( force_reset ) {
        pwrkey_dbus_action_reset(action);
    }
    else {
        pwrkey_dbus_action_parse(action);
        pwrkey_dbus_action_sanitize(action);
    }

    use = pwrkey_dbus_action_to_string(action);

    if( !eq(action->setting_val, use) ) {
        /* Change locally cached value */
        g_free(action->setting_val), action->setting_val = use, use = 0;

        /* Flush change to settings */
        mce_setting_set_string(action->setting_key, action->setting_val);
    }

cleanup:

    g_free(use);
}

static void
pwrkey_dbus_action_execute(size_t action_id)
{
    bool flag_created = false;

    if( action_id >= POWEKEY_DBUS_ACTION_COUNT )
        goto cleanup;

    mce_log(LL_DEBUG, "Executing dbus action %zd", action_id);

    const pwrkey_dbus_action_t *action = pwrkey_dbus_action + action_id;

    /* We're potentially creating dbus messages using user specified
     * parameters. Since libdbus will abort the process rather than
     * returning some error code -> have a flag file around while
     * doing the hazardous ipc operations -> if abort occurs, the
     * flag file is left behind -> mce will reset dbus action config
     * back to default on restart */

    if( !(flag_created = pwrkey_create_flagfile(pwrkey_dbus_action_flag)) ) {
        mce_log(LL_CRIT, "%s: could not create flagfile: %m",
                pwrkey_dbus_action_flag);
        goto cleanup;
    }

    if( pwrkey_dbus_action_is_signal(action) ) {
        pwrkey_dbus_send_signal(MCE_POWER_BUTTON_TRIGGER,
                                action->argument);
        goto cleanup;
    }

    if( !pwrkey_dbus_action_is_methodcall(action) ) {
        mce_log(LL_WARN, "dbus%zd action does not have valid configuration",
                action_id + 1);
        goto cleanup;
    }

    if( empty(action->argument) ) {
        dbus_send(action->destination,
                  action->object,
                  action->interface,
                  action->member,
                  0,
                  DBUS_TYPE_INVALID);
    }
    else {
        dbus_send(action->destination,
                  action->object,
                  action->interface,
                  action->member,
                  0,
                  DBUS_TYPE_STRING, &action->argument,
                  DBUS_TYPE_INVALID);
    }

cleanup:

    if( flag_created && !pwrkey_delete_flagfile(pwrkey_dbus_action_flag) ) {
        mce_log(LL_CRIT, "%s: could not delete flagfile: %m",
                pwrkey_dbus_action_flag);
    }

    return;
}

/* ========================================================================= *
 * POWER_KEY_STATE_MACHINE
 * ========================================================================= */

/** Check if we need to hold a wakelock for power key handling
 *
 * Wakelock is held if there are pending timers.
 */
static void
pwrkey_stm_rethink_wakelock(void)
{
#ifdef ENABLE_WAKELOCKS
    static bool have_lock = false;

    bool want_lock = pwrkey_stm_pending_timers();;

    if( have_lock == want_lock )
        goto EXIT;

    if( (have_lock = want_lock) ) {
        wakelock_lock("mce_pwrkey_stm", -1);
        mce_log(LL_DEBUG, "acquire wakelock");
    }
    else {
        mce_log(LL_DEBUG, "release wakelock");
        wakelock_unlock("mce_pwrkey_stm");
    }
EXIT:
    return;
#endif
}

static bool
pwrkey_stm_pending_timers(void)
{
    return (pwrkey_long_press_timer_pending() ||
            pwrkey_double_press_timer_pending());
}

static void
pwrkey_stm_terminate(void)
{
    /* Cancel timers */
    pwrkey_double_press_timer_cancel();
    pwrkey_long_press_timer_cancel();

    /* Release wakelock */
    pwrkey_stm_rethink_wakelock();
}

static void pwrkey_stm_long_press_timeout(void)
{
    // execute long press
    pwrkey_actions_do_long_press();
}

static void pwrkey_stm_double_press_timeout(void)
{
    // execute single press
    pwrkey_actions_do_single_press();
}

static void pwrkey_stm_powerkey_pressed(void)
{
    if( pwrkey_double_press_timer_cancel() ) {
        /* Pressed while we were waiting for double press */
        pwrkey_actions_do_double_press();
    }
    else if( !pwrkey_long_press_timer_pending() ) {
        /* Pressed while there are no timers active */

        /* Store display state we started from */
        pwrkey_stm_store_initial_state();

        /* Start short vs long press detection timer */
        if( !pwrkey_stm_ignore_action() ) {
            pwrkey_long_press_timer_start();
        }
    }
}

static void pwrkey_stm_powerkey_released(void)
{
    if( pwrkey_long_press_timer_cancel() ) {
        /* Released while we were waiting for long press */

        /* Always do actions that are common to both short and
         * double press */
        pwrkey_actions_do_common();

        if( pwrkey_actions_use_double_press() ) {
            /* There is config for double press -> wait a while
             * to see if it is double press */
            pwrkey_double_press_timer_start();
        }
        else {
            /* There is no config for double press -> just do
             * actions for single press without further delays */
            pwrkey_actions_do_single_press();
        }
    }
}

static void pwrkey_stm_store_initial_state(void)
{
    /* Cache display state */

    pwrkey_stm_display_state = display_state_curr;

    /* MCE_DISPLAY_OFF requests must be queued only
     * from fully powered up display states.
     * Otherwise we create a situation where multiple
     * power key presses done while the display is off
     * or powering up will bounce back to display off
     * once initial the off->on transition finishes */

    bool display_is_on = false;

    switch( pwrkey_stm_display_state ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        display_is_on = true;
        break;

    default:
        break;
    }

    pwrkey_actions_select(display_is_on);
}

/** Should power key action be ignored predicate
 */
static bool
pwrkey_stm_ignore_action(void)
{
    /* Assume that power key action should not be ignored */
    bool ignore_powerkey = false;

    /* If alarm dialog is up, power key is used for snoozing */
    switch( alarm_ui_state ) {
    case MCE_ALARM_UI_VISIBLE_INT32:
    case MCE_ALARM_UI_RINGING_INT32:
        mce_log(LL_DEVEL, "[powerkey] ignored due to active alarm");
        ignore_powerkey = true;
        pwrkey_dbus_send_signal(MCE_ALARM_UI_FEEDBACK_SIG, MCE_FEEDBACK_EVENT_POWERKEY);
        break;

    default:
    case MCE_ALARM_UI_OFF_INT32:
    case MCE_ALARM_UI_INVALID_INT32:
        // dontcare
        break;
    }

    /* During incoming call power key is used to silence ringing */
    switch( call_state ) {
    case CALL_STATE_RINGING:
        if( pwrkey_ignore_incoming_call ) {
            /* Call ui has signaled mce that the incoming call has
             * been ignored -> powerkey can be used for display
             * control even if there is incoming call. */
            break;
        }
        mce_log(LL_DEVEL, "[powerkey] ignored due to incoming call");
        ignore_powerkey = true;
        pwrkey_dbus_send_signal(MCE_CALL_UI_FEEDBACK_SIG, MCE_FEEDBACK_EVENT_POWERKEY);
        break;

    default:
    case CALL_STATE_INVALID:
    case CALL_STATE_NONE:
    case CALL_STATE_ACTIVE:
    case CALL_STATE_SERVICE:
        // dontcare
        break;
    }

    /* If user is enrolling a fingerprint, do not blank with powerkey */
    if( enroll_in_progress ) {
        /* We only want to block actions that would blank / lock the
         * device i.e. what would happen from display on/dimmed state.
         */
        switch( display_state_next ) {
        case MCE_DISPLAY_ON:
        case MCE_DISPLAY_DIM:
            mce_log(LL_DEVEL, "[powerkey] ignored due to fingerprint enroll");
            ignore_powerkey = true;
            break;
        default:
            break;
        }
    }

    /* Skip rest if already desided to ignore */
    if( ignore_powerkey )
        goto EXIT;

    /* Proximity sensor state vs power key press handling mode */
    switch( pwrkey_stm_enable_mode ) {
    case PWRKEY_ENABLE_NEVER:
        mce_log(LL_DEVEL, "[powerkey] ignored due to setting=never");
        ignore_powerkey = true;
        goto EXIT;

    case PWRKEY_ENABLE_ALWAYS:
        break;

    case PWRKEY_ENABLE_NO_PROXIMITY2:
        /* do not ignore if display is on */
        if( pwrkey_stm_display_state == MCE_DISPLAY_ON  ||
            pwrkey_stm_display_state == MCE_DISPLAY_DIM ||
            pwrkey_stm_display_state == MCE_DISPLAY_LPM_ON ) {
            break;
        }
        /* fall through */
    default:
    case PWRKEY_ENABLE_NO_PROXIMITY:
        if( lid_sensor_filtered == COVER_CLOSED ) {
            mce_log(LL_DEVEL, "[powerkey] ignored due to lid");
            ignore_powerkey = true;
            goto EXIT;
        }

        if( proximity_sensor_actual == COVER_CLOSED ) {
            mce_log(LL_DEVEL, "[powerkey] ignored due to proximity");
            ignore_powerkey = true;
            goto EXIT;
        }
        break;
    }

EXIT:
    return ignore_powerkey;
}

/* ========================================================================= *
 * HOME_KEY_STATE_MACHINE
 * ========================================================================= */

/** Convert homekey_stm_t enum to human readable string
 *
 * @param state homekey_stm_t enumeration value
 *
 * @return human readable representation of state
 */
static const char *
homekey_stm_repr(homekey_stm_t state)
{
    const char *repr = "HOMEKEY_STM_UNKNOWN";

    switch( state ) {
    case HOMEKEY_STM_WAIT_PRESS:   repr = "HOMEKEY_STM_WAIT_PRESS";   break;
    case HOMEKEY_STM_WAIT_UNBLANK: repr = "HOMEKEY_STM_WAIT_UNBLANK"; break;
    case HOMEKEY_STM_SEND_SIGNAL:  repr = "HOMEKEY_STM_SEND_SIGNAL";  break;
    case HOMEKEY_STM_WAIT_RELEASE: repr = "HOMEKEY_STM_WAIT_RELEASE"; break;
    default:
        break;
    }

    return repr;
}

/** Current state of home key handling state machine */
static homekey_stm_t homekey_stm_state = HOMEKEY_STM_WAIT_PRESS;

/** Cached home key is pressed down state */
static bool homekey_stm_pressed = false;

/** Set current state of home key handling state machine
 *
 * Perform any actions that are related to leaving current and/or
 * entering the new state.
 *
 * @param state homekey_stm_t enumeration value
 */
static void
homekey_stm_set_state(homekey_stm_t state)
{
    if( homekey_stm_state == state )
        goto EXIT;

    mce_log(LL_DEBUG, "state: %s -> %s",
            homekey_stm_repr(homekey_stm_state),
            homekey_stm_repr(state));

    /* Handle entering new state */

    switch( (homekey_stm_state = state) ) {
    case HOMEKEY_STM_WAIT_PRESS:
        break;

    case HOMEKEY_STM_WAIT_UNBLANK:
        /* Check if policy allows display unblanking */
        if( proximity_sensor_actual != COVER_OPEN ) {
            mce_log(LL_DEBUG, "Proximity sensor %s; skip unblank",
                    proximity_state_repr(proximity_sensor_actual));
            break;
        }

        /* Initiate display power up */
        mce_log(LL_DEBUG, "request %s",
                display_state_repr(MCE_DISPLAY_ON));
        mce_datapipe_request_display_state(MCE_DISPLAY_ON);
        break;

    case HOMEKEY_STM_SEND_SIGNAL:
        /* Inform compositor that it should perform home key actions */
        pwrkey_dbus_send_signal(MCE_POWER_BUTTON_TRIGGER, "home-key");
        break;

    case HOMEKEY_STM_WAIT_RELEASE:
        break;

    default:
        break;
    }

EXIT:
    return;
}

/** Perform one home key handling state machine transition
 *
 * @return true if state transition took place, false otherwise
 */
static bool
homekey_stm_exec_step(void)
{
    homekey_stm_t prev = homekey_stm_state;

    switch( homekey_stm_state ) {
    default:
    case HOMEKEY_STM_WAIT_PRESS:
        if( homekey_stm_pressed )
            homekey_stm_set_state(HOMEKEY_STM_WAIT_UNBLANK);
        break;

    case HOMEKEY_STM_WAIT_UNBLANK:
        if( display_state_next != MCE_DISPLAY_ON )
            homekey_stm_set_state(HOMEKEY_STM_WAIT_RELEASE);
        else if( display_state_curr == MCE_DISPLAY_ON )
            homekey_stm_set_state(HOMEKEY_STM_SEND_SIGNAL);
        break;

    case HOMEKEY_STM_SEND_SIGNAL:
        homekey_stm_set_state(HOMEKEY_STM_WAIT_RELEASE);
        break;

    case HOMEKEY_STM_WAIT_RELEASE:
        if( !homekey_stm_pressed )
            homekey_stm_set_state(HOMEKEY_STM_WAIT_PRESS);
        break;
    }

    return homekey_stm_state != prev;
}

/** Update current state of home key handling state machine
 *
 * Repeatedly executes transitions until stable state is reached.
 */
static void
homekey_stm_eval_state(void)
{
    while( homekey_stm_exec_step() )
        ;
}

/** Set home key pressed down state and update state machine
 */
static void
homekey_stm_set_pressed(bool pressed)
{
    if( homekey_stm_pressed == pressed )
        goto EXIT;

    homekey_stm_pressed = pressed;
    homekey_stm_eval_state();

EXIT:
    return;
}

/* ========================================================================= *
 * DBUS_IPC
 * ========================================================================= */

/** Helper for sending powerkey feedback dbus signal
 *
 * @param sig name of the signal to send
 */
static void
pwrkey_dbus_send_signal(const char *sig, const char *arg)
{
    mce_log(LL_DEVEL, "sending dbus signal: %s %s", sig, arg);
    dbus_send(0, MCE_SIGNAL_PATH, MCE_SIGNAL_IF,  sig, 0,
              DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
}

/**
 * D-Bus callback for powerkey event triggering
 *
 * @param msg D-Bus message
 *
 * @return TRUE
 */
static gboolean pwrkey_dbus_trigger_event_cb(DBusMessage *const req)
{
    DBusMessage   *rsp = 0;
    dbus_uint32_t  act = 0;

    DBusMessageIter iter;

    mce_log(LL_DEVEL, "[power] button trigger request from %s",
            mce_dbus_get_message_sender_ident(req));

    if( !dbus_message_iter_init(req, &iter) )
        goto EXIT;

    switch( dbus_message_iter_get_arg_type(&iter) ) {
    case DBUS_TYPE_BOOLEAN:
        {
            dbus_bool_t tmp = 0;
            dbus_message_iter_get_basic(&iter, &tmp);
            act = tmp ? 1 : 0;
        }
        break;

    case DBUS_TYPE_UINT32:
        dbus_message_iter_get_basic(&iter, &act);
        break;

    default:
        mce_log(LL_ERR,	"Argument passed to %s.%s has incorrect type",
                MCE_REQUEST_IF, MCE_TRIGGER_POWERKEY_EVENT_REQ);
        goto EXIT;
    }

    if( act > 2 ) {
        mce_log(LL_ERR, "Incorrect powerkey event passed to %s.%s; "
                "ignoring request",
                MCE_REQUEST_IF, MCE_TRIGGER_POWERKEY_EVENT_REQ);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "[power] button event trigger value: %d", act);

    /* Terminate state machine actions for real power key */
    pwrkey_stm_terminate();

    /* Choose actions based on display state */
    pwrkey_stm_store_initial_state();

    if( pwrkey_stm_ignore_action() )
        goto EXIT;

    switch (act) {
    default:
    case MCE_POWERKEY_EVENT_SHORT_PRESS:
        /* short press */
        pwrkey_actions_do_common();
        pwrkey_actions_do_single_press();
        break;

    case MCE_POWERKEY_EVENT_LONG_PRESS:
        /* long press */
        pwrkey_actions_do_long_press();
        break;

    case MCE_POWERKEY_EVENT_DOUBLE_PRESS:
        /* double press */
        pwrkey_actions_do_common();
        pwrkey_actions_do_double_press();
        break;
    }

EXIT:
    if( !dbus_message_get_no_reply(req) ) {
        /* Need to send reply, create a dummy one if we do
         * not have already existing error reply */
        if( !rsp )
            rsp = dbus_new_method_reply(req);
        dbus_send_message(rsp), rsp = 0;
    }

    if( rsp )
        dbus_message_unref(rsp);

    pwrkey_stm_rethink_wakelock();

    return TRUE;
}

/** D-Bus callback for ignoring incoming call
 *
 * @param req D-Bus method call message
 *
 * @return TRUE
 */
static gboolean pwrkey_dbus_ignore_incoming_call_cb(DBusMessage *const req)
{

    mce_log(LL_DEVEL, "ignore incoming call from %s",
            mce_dbus_get_message_sender_ident(req));

    if( call_state == CALL_STATE_RINGING ) {
        mce_log(LL_DEBUG, "start ignoring incoming calls");

        /* Update powerkey module specific toggle */
        pwrkey_ignore_incoming_call = true;

        /* Make also callstate plugin ignore incoming calls. This
         * should lead to call_state changing from RINGING to ACTIVE
         * or NONE depending on whether there are other calls or not. */
        datapipe_exec_full(&ignore_incoming_call_event_pipe, GINT_TO_POINTER(true),
                           USE_INDATA, DONT_CACHE_INDATA);
    }

    if( !dbus_message_get_no_reply(req) ) {
      DBusMessage *rsp = dbus_new_method_reply(req);
      dbus_send_message(rsp), rsp = 0;
    }

    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t pwrkey_dbus_handlers[] =
{
    /* signals - outbound (for Introspect purposes only) */
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_ALARM_UI_FEEDBACK_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"event\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_CALL_UI_FEEDBACK_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"event\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_POWER_BUTTON_TRIGGER,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"event\" type=\"s\"/>\n"
    },
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_TRIGGER_POWERKEY_EVENT_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = pwrkey_dbus_trigger_event_cb,
        .args      =
            "    <arg direction=\"in\" name=\"action\" type=\"u\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_IGNORE_INCOMING_CALL_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = pwrkey_dbus_ignore_incoming_call_cb,
        .args      = 0
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Add dbus handlers
 */
static void
pwrkey_dbus_init(void)
{
    mce_dbus_handler_register_array(pwrkey_dbus_handlers);
}

/** Remove dbus handlers
 */
static void
pwrkey_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(pwrkey_dbus_handlers);
}

/* ========================================================================= *
 * DYNAMIC_SETTINGS
 * ========================================================================= */

static void
pwrkey_setting_sanitize_action_masks(void)
{
    /* parse settings -> bitmasks */
    pwrkey_actions_parse(&pwrkey_actions_from_display_on,
                         pwrkey_actions_single_on,
                         pwrkey_actions_double_on,
                         pwrkey_actions_long_on);

    pwrkey_actions_parse(&pwrkey_actions_from_display_off,
                         pwrkey_actions_single_off,
                         pwrkey_actions_double_off,
                         pwrkey_actions_long_off);

    /* bitmasks -> setting strings */
    bool on_changed =
        pwrkey_actions_update(&pwrkey_actions_from_display_on,
                              &pwrkey_actions_single_on,
                              &pwrkey_actions_double_on,
                              &pwrkey_actions_long_on);

    bool off_changed =
        pwrkey_actions_update(&pwrkey_actions_from_display_off,
                              &pwrkey_actions_single_off,
                              &pwrkey_actions_double_off,
                              &pwrkey_actions_long_off);

    /* send notifications if something changed */
    if( on_changed ) {
        mce_setting_set_string(MCE_SETTING_POWERKEY_ACTIONS_SINGLE_ON,
                               pwrkey_actions_single_on);

        mce_setting_set_string(MCE_SETTING_POWERKEY_ACTIONS_DOUBLE_ON,
                               pwrkey_actions_double_on);

        mce_setting_set_string(MCE_SETTING_POWERKEY_ACTIONS_LONG_ON,
                               pwrkey_actions_long_on);
    }

    if( off_changed ) {
        mce_setting_set_string(MCE_SETTING_POWERKEY_ACTIONS_SINGLE_OFF,
                               pwrkey_actions_single_off);

        mce_setting_set_string(MCE_SETTING_POWERKEY_ACTIONS_DOUBLE_OFF,
                               pwrkey_actions_double_off);

        mce_setting_set_string(MCE_SETTING_POWERKEY_ACTIONS_LONG_OFF,
                               pwrkey_actions_long_off);
    }

    for( size_t i = 0; i < POWERKEY_ACTIONS_GESTURE_COUNT; ++i ) {
        pwrkey_actions_parse(&pwrkey_actions_from_gesture[i],
                             pwrkey_actions_gesture[i], 0, 0);
        bool gesture_changed =
            pwrkey_actions_update(&pwrkey_actions_from_gesture[i],
                                  &pwrkey_actions_gesture[i], 0, 0);
        if( gesture_changed ) {
            mce_setting_set_string(pwrkey_actions_gesture_key[i],
                                   pwrkey_actions_gesture[i]);
        }
    }
}

static void
pwrkey_setting_sanitize_dbus_actions(void)
{
    /* The custom dbus action settings can cause mce to
     * get aborted by dbus_message_new_xxx().
     *
     * Assume having the flag file present means that
     * mce is restarting after abort and reset the dbus
     * action config back to defaults to avoid repeating
     * the abort.
     */
    bool force_reset = pwrkey_delete_flagfile(pwrkey_dbus_action_flag);

    if( force_reset ) {
        mce_log(LL_CRIT, "%s: flagfile was present; resetting"
                "dbus action config", pwrkey_dbus_action_flag);
    }

    for( size_t action_id = 0; action_id < POWEKEY_DBUS_ACTION_COUNT; ++action_id )
        pwrkey_dbus_action_configure(action_id, force_reset);
}

static void
pwrkey_setting_sanitize_now(void)
{
    pwrkey_setting_sanitize_action_masks();
    pwrkey_setting_sanitize_dbus_actions();
}

static gboolean pwrkey_setting_sanitize_cb(gpointer aptr)
{
    (void)aptr;

    if( !pwrkey_setting_sanitize_id )
        goto EXIT;

    pwrkey_setting_sanitize_id = 0;

    pwrkey_setting_sanitize_now();

EXIT:
    return FALSE;
}

static void pwrkey_setting_sanitize_later(void)
{
    if( !pwrkey_setting_sanitize_id )
        pwrkey_setting_sanitize_id = g_idle_add(pwrkey_setting_sanitize_cb, 0);
}

static void pwrkey_setting_sanitize_cancel(void)
{
    if( pwrkey_setting_sanitize_id ) {
        g_source_remove(pwrkey_setting_sanitize_id),
            pwrkey_setting_sanitize_id = 0;
    }
}

static bool
pwrkey_setting_handle_gesture(const GConfValue *gcv, guint id)
{
    bool handled = false;

    for( size_t i = 0; i < POWERKEY_ACTIONS_GESTURE_COUNT; ++i ) {
        if( pwrkey_actions_gesture_setting_id[i] != id )
            continue;

        const char *val = gconf_value_get_string(gcv);
        if( !eq(pwrkey_actions_gesture[i], val) ) {
            mce_log(LL_NOTICE, "pwrkey_actions_gesture[%zu]: '%s' -> '%s'",
                    i, pwrkey_actions_gesture[i], val);
            g_free(pwrkey_actions_gesture[i]);
            pwrkey_actions_gesture[i] = g_strdup(val);
            pwrkey_setting_sanitize_later();
        }

        handled = true;
        break;
    }

    return handled;
}

/** GConf callback for powerkey related settings
 *
 * @param gcc    (not used)
 * @param id     Connection ID from gconf_client_notify_add()
 * @param entry  The modified GConf entry
 * @param data   (not used)
 */
static void
pwrkey_setting_cb(GConfClient *const gcc, const guint id,
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

    if( id == pwrkey_stm_enable_mode_setting_id ) {
        gint old = pwrkey_stm_enable_mode;
        pwrkey_stm_enable_mode = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "pwrkey_stm_enable_mode: %d -> %d",
                old, pwrkey_stm_enable_mode);
    }
    else if( id == pwrkey_action_blank_mode_setting_id ) {
        gint old = pwrkey_action_blank_mode;
        pwrkey_action_blank_mode = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "pwrkey_action_blank_mode: %d -> %d",
                old, pwrkey_action_blank_mode);
    }
    else if( id == pwrkey_ps_override_count_setting_id ) {
        gint old = pwrkey_ps_override_count;
        pwrkey_ps_override_count = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "pwrkey_ps_override_count: %d -> %d",
                old, pwrkey_ps_override_count);
    }
    else if( id == pwrkey_ps_override_timeout_setting_id ) {
        gint old = pwrkey_ps_override_timeout;
        pwrkey_ps_override_timeout = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "pwrkey_ps_override_timeout: %d -> %d",
                old, pwrkey_ps_override_timeout);
    }
    else if( id == pwrkey_long_press_delay_setting_id ) {
        gint old = pwrkey_long_press_delay;
        pwrkey_long_press_delay = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "pwrkey_long_press_delay: %d -> %d",
                old, pwrkey_long_press_delay);
    }
    else if( id == pwrkey_double_press_delay_setting_id ) {
        gint old = pwrkey_double_press_delay;
        pwrkey_double_press_delay = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "pwrkey_double_press_delay: %d -> %d",
                old, pwrkey_double_press_delay);
    }
    else if( id == pwrkey_actions_single_on_setting_id ) {
        const char *val = gconf_value_get_string(gcv);
        if( !eq(pwrkey_actions_single_on, val) ) {
            mce_log(LL_NOTICE, "pwrkey_actions_single_on: '%s' -> '%s'",
                    pwrkey_actions_single_on, val);
            g_free(pwrkey_actions_single_on);
            pwrkey_actions_single_on = g_strdup(val);
            pwrkey_setting_sanitize_later();
        }
    }
    else if( id == pwrkey_actions_double_on_setting_id ) {
        const char *val = gconf_value_get_string(gcv);
        if( !eq(pwrkey_actions_double_on, val) ) {
            mce_log(LL_NOTICE, "pwrkey_actions_double_on: '%s' -> '%s'",
                    pwrkey_actions_double_on, val);
            g_free(pwrkey_actions_double_on);
            pwrkey_actions_double_on = g_strdup(val);
            pwrkey_setting_sanitize_later();
        }
    }
    else if( id == pwrkey_actions_long_on_setting_id ) {
        const char *val = gconf_value_get_string(gcv);
        if( !eq(pwrkey_actions_long_on, val) ) {
            mce_log(LL_NOTICE, "pwrkey_actions_long_on: '%s' -> '%s'",
                    pwrkey_actions_long_on, val);
            g_free(pwrkey_actions_long_on);
            pwrkey_actions_long_on = g_strdup(val);
            pwrkey_setting_sanitize_later();
        }
    }
    else if( id == pwrkey_actions_single_off_setting_id ) {
        const char *val = gconf_value_get_string(gcv);
        if( !eq(pwrkey_actions_single_off, val) ) {
            mce_log(LL_NOTICE, "pwrkey_actions_single_off: '%s' -> '%s'",
                    pwrkey_actions_single_off, val);
            g_free(pwrkey_actions_single_off);
            pwrkey_actions_single_off = g_strdup(val);
            pwrkey_setting_sanitize_later();
        }
    }
    else if( id == pwrkey_actions_double_off_setting_id ) {
        const char *val = gconf_value_get_string(gcv);
        if( !eq(pwrkey_actions_double_off, val) ) {
            mce_log(LL_NOTICE, "pwrkey_actions_double_off: '%s' -> '%s'",
                    pwrkey_actions_double_off, val);
            g_free(pwrkey_actions_double_off);
            pwrkey_actions_double_off = g_strdup(val);
            pwrkey_setting_sanitize_later();
        }
    }
    else if( id == pwrkey_actions_long_off_setting_id ) {
        const char *val = gconf_value_get_string(gcv);
        if( !eq(pwrkey_actions_long_off, val) ) {
            mce_log(LL_NOTICE, "pwrkey_actions_long_off: '%s' -> '%s'",
                    pwrkey_actions_long_off, val);
            g_free(pwrkey_actions_long_off);
            pwrkey_actions_long_off = g_strdup(val);
            pwrkey_setting_sanitize_later();
        }
    }
    else if( id == pwrkey_gestures_enable_mode_cb_id ) {
        gint old = pwrkey_gestures_enable_mode;
        pwrkey_gestures_enable_mode = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "pwrkey_gestures_enable_mode: %d -> %d",
                old, pwrkey_gestures_enable_mode);
    }
    else if( pwrkey_setting_handle_gesture(gcv, id) ) {
        // nop
    }
    else {
        for( size_t action_id = 0; ; ++action_id ) {
            if( action_id >= POWEKEY_DBUS_ACTION_COUNT ) {
                mce_log(LL_WARN, "Spurious GConf value received; confused!");
                goto EXIT;
            }

            pwrkey_dbus_action_t *action = pwrkey_dbus_action + action_id;

            if( id != action->setting_id )
                continue;

            const char *val = gconf_value_get_string(gcv);

            if( eq(action->setting_val, val) )
                break;

            mce_log(LL_NOTICE, "pwrkey_dbus_action%zd_val: '%s' -> '%s'",
                    action_id, action->setting_val, val);

            g_free(action->setting_val), action->setting_val = g_strdup(val);
            pwrkey_setting_sanitize_later();
            break;
        }
    }

EXIT:

    return;
}

/** Get setting values and start tracking changes
 */
static void
pwrkey_setting_init(void)
{
    /* Power key press handling mode */
    mce_setting_track_int(MCE_SETTING_POWERKEY_MODE,
                          &pwrkey_stm_enable_mode,
                          MCE_DEFAULT_POWERKEY_MODE,
                          pwrkey_setting_cb,
                          &pwrkey_stm_enable_mode_setting_id);

    /* Power key display blanking mode */
    mce_setting_track_int(MCE_SETTING_POWERKEY_BLANKING_MODE,
                          &pwrkey_action_blank_mode,
                          MCE_DEFAULT_POWERKEY_BLANKING_MODE,
                          pwrkey_setting_cb,
                          &pwrkey_action_blank_mode_setting_id);

    /* Power key press count for proximity sensor override */
    mce_setting_track_int(MCE_SETTING_POWERKEY_PS_OVERRIDE_COUNT,
                          &pwrkey_ps_override_count,
                          MCE_DEFAULT_POWERKEY_PS_OVERRIDE_COUNT,
                          pwrkey_setting_cb,
                          &pwrkey_ps_override_count_setting_id);

    /* Maximum time between power key presses for ps override */
    mce_setting_track_int(MCE_SETTING_POWERKEY_PS_OVERRIDE_TIMEOUT,
                          &pwrkey_ps_override_timeout,
                          MCE_DEFAULT_POWERKEY_PS_OVERRIDE_TIMEOUT,
                          pwrkey_setting_cb,
                          &pwrkey_ps_override_timeout_setting_id);

    /* Delay for waiting long press */
    mce_setting_track_int(MCE_SETTING_POWERKEY_LONG_PRESS_DELAY,
                          &pwrkey_long_press_delay,
                          MCE_DEFAULT_POWERKEY_LONG_PRESS_DELAY,
                          pwrkey_setting_cb,
                          &pwrkey_long_press_delay_setting_id);

    /* Delay for waiting double press */
    mce_setting_track_int(MCE_SETTING_POWERKEY_DOUBLE_PRESS_DELAY,
                          &pwrkey_double_press_delay,
                          MCE_DEFAULT_POWERKEY_DOUBLE_PRESS_DELAY,
                          pwrkey_setting_cb,
                          &pwrkey_double_press_delay_setting_id);

    /* Action sets */

    mce_setting_track_string(MCE_SETTING_POWERKEY_ACTIONS_SINGLE_ON,
                             &pwrkey_actions_single_on,
                             MCE_DEFAULT_POWERKEY_ACTIONS_SINGLE_ON,
                             pwrkey_setting_cb,
                             &pwrkey_actions_single_on_setting_id);

    mce_setting_track_string(MCE_SETTING_POWERKEY_ACTIONS_DOUBLE_ON,
                             &pwrkey_actions_double_on,
                             MCE_DEFAULT_POWERKEY_ACTIONS_DOUBLE_ON,
                             pwrkey_setting_cb,
                             &pwrkey_actions_double_on_setting_id);

    mce_setting_track_string(MCE_SETTING_POWERKEY_ACTIONS_LONG_ON,
                             &pwrkey_actions_long_on,
                             MCE_DEFAULT_POWERKEY_ACTIONS_LONG_ON,
                             pwrkey_setting_cb,
                             &pwrkey_actions_long_on_setting_id);

    mce_setting_track_string(MCE_SETTING_POWERKEY_ACTIONS_SINGLE_OFF,
                             &pwrkey_actions_single_off,
                             MCE_DEFAULT_POWERKEY_ACTIONS_SINGLE_OFF,
                             pwrkey_setting_cb,
                             &pwrkey_actions_single_off_setting_id);

    mce_setting_track_string(MCE_SETTING_POWERKEY_ACTIONS_DOUBLE_OFF,
                             &pwrkey_actions_double_off,
                             MCE_DEFAULT_POWERKEY_ACTIONS_DOUBLE_OFF,
                             pwrkey_setting_cb,
                             &pwrkey_actions_double_off_setting_id);

    mce_setting_track_string(MCE_SETTING_POWERKEY_ACTIONS_LONG_OFF,
                             &pwrkey_actions_long_off,
                             MCE_DEFAULT_POWERKEY_ACTIONS_LONG_OFF,
                             pwrkey_setting_cb,
                             &pwrkey_actions_long_off_setting_id);

    mce_setting_track_int(MCE_SETTING_DOUBLETAP_MODE,
                          &pwrkey_gestures_enable_mode,
                          MCE_DEFAULT_DOUBLETAP_MODE,
                          pwrkey_setting_cb,
                          &pwrkey_gestures_enable_mode_cb_id);

    for( size_t i = 0; i < POWERKEY_ACTIONS_GESTURE_COUNT; ++i ) {
        mce_setting_track_string(pwrkey_actions_gesture_key[i],
                                 &pwrkey_actions_gesture[i],
                                 pwrkey_actions_gesture_val[i],
                                 pwrkey_setting_cb,
                                 &pwrkey_actions_gesture_setting_id[i]);
    }

    /* D-Bus actions */

    for( size_t action_id = 0; action_id < POWEKEY_DBUS_ACTION_COUNT; ++action_id ) {
        pwrkey_dbus_action_t *action = pwrkey_dbus_action + action_id;

        mce_setting_track_string(action->setting_key,
                                 &action->setting_val,
                                 action->setting_def,
                                 pwrkey_setting_cb,
                                 &action->setting_id);
    }

    /* Apply sanity checks */

    pwrkey_setting_sanitize_now();
}

/** Stop tracking setting changes
 */
static void
pwrkey_setting_quit(void)
{
    /* Power key press handling mode */
    mce_setting_notifier_remove(pwrkey_stm_enable_mode_setting_id),
        pwrkey_stm_enable_mode_setting_id = 0;

    /* Power key press blanking mode */
    mce_setting_notifier_remove(pwrkey_action_blank_mode_setting_id),
        pwrkey_action_blank_mode_setting_id = 0;

    /* Power key press blanking mode */
    mce_setting_notifier_remove(pwrkey_ps_override_count_setting_id),
        pwrkey_ps_override_count_setting_id = 0;

    /* Power key press blanking mode */
    mce_setting_notifier_remove(pwrkey_ps_override_timeout_setting_id),
        pwrkey_ps_override_timeout_setting_id = 0;

    /* Action sets */

    mce_setting_notifier_remove(pwrkey_actions_single_on_setting_id),
        pwrkey_actions_single_on_setting_id = 0;

    mce_setting_notifier_remove(pwrkey_actions_double_on_setting_id),
        pwrkey_actions_double_on_setting_id = 0;

    mce_setting_notifier_remove(pwrkey_actions_long_on_setting_id),
        pwrkey_actions_long_on_setting_id = 0;

    mce_setting_notifier_remove(pwrkey_actions_single_off_setting_id),
        pwrkey_actions_single_off_setting_id = 0;

    mce_setting_notifier_remove(pwrkey_actions_double_off_setting_id),
        pwrkey_actions_double_off_setting_id = 0;

    mce_setting_notifier_remove(pwrkey_actions_long_off_setting_id),
        pwrkey_actions_long_off_setting_id = 0;

    mce_setting_notifier_remove(pwrkey_gestures_enable_mode_cb_id),
        pwrkey_gestures_enable_mode_cb_id = 0;

    for( size_t i = 0; i < POWERKEY_ACTIONS_GESTURE_COUNT; ++i ) {
        mce_setting_notifier_remove(pwrkey_actions_gesture_setting_id[i]),
            pwrkey_actions_gesture_setting_id[i] = 0;
    }

    g_free(pwrkey_actions_single_on),
        pwrkey_actions_single_on = 0;

    g_free(pwrkey_actions_double_on),
        pwrkey_actions_double_on = 0;

    g_free(pwrkey_actions_long_on),
        pwrkey_actions_long_on = 0;

    g_free(pwrkey_actions_single_off),
        pwrkey_actions_single_off = 0;

    g_free(pwrkey_actions_double_off),
        pwrkey_actions_double_off = 0;

    g_free(pwrkey_actions_long_off),
        pwrkey_actions_long_off = 0;;

    for( size_t i = 0; i < POWERKEY_ACTIONS_GESTURE_COUNT; ++i ) {
        g_free(pwrkey_actions_gesture[i]),
            pwrkey_actions_gesture[i] = 0;;
    }

    /* Cancel pending delayed setting sanitizing */
    pwrkey_setting_sanitize_cancel();

    /* D-Bus actions */

    for( size_t action_id = 0; action_id < POWEKEY_DBUS_ACTION_COUNT; ++action_id ) {
        pwrkey_dbus_action_t *action = pwrkey_dbus_action + action_id;

        mce_setting_notifier_remove(action->setting_id),
            action->setting_id = 0;

        g_free(action->setting_val),
            action->setting_val = 0;;

    }
}

/* ========================================================================= *
 * DATAPIPE_HANDLING
 * ========================================================================= */

/** Change notifications for system_state
 */
static void pwrkey_datapipe_system_state_cb(gconstpointer data)
{
    system_state_t prev = system_state;
    system_state = GPOINTER_TO_INT(data);

    if( prev == system_state )
        goto EXIT;

    mce_log(LL_DEBUG, "system_state: %s -> %s",
            system_state_repr(prev),
            system_state_repr(system_state));

EXIT:
    return;
}

/** Handle display state change notifications
 *
 * @param data display state (as void pointer)
 */
static void
pwrkey_datapipe_display_state_curr_cb(gconstpointer data)
{
    display_state_t prev = display_state_curr;
    display_state_curr = GPOINTER_TO_INT(data);

    if( display_state_curr == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_curr = %s -> %s",
            display_state_repr(prev),
            display_state_repr(display_state_curr));

    homekey_stm_eval_state();

EXIT:
    return;
}

/** Pre-change notifications for display_state_curr
 */
static void pwrkey_datapipe_display_state_next_cb(gconstpointer data)
{
    display_state_t prev = display_state_next;
    display_state_next = GPOINTER_TO_INT(data);

    if( prev == display_state_next )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_next = %s -> %s",
            display_state_repr(prev),
            display_state_repr(display_state_next));

    homekey_stm_eval_state();

EXIT:
    return;

}

/** Change notifications from lid_sensor_filtered_pipe
 */
static void pwrkey_datapipe_lid_sensor_filtered_cb(gconstpointer data)
{
    cover_state_t prev = lid_sensor_filtered;
    lid_sensor_filtered = GPOINTER_TO_INT(data);

    if( lid_sensor_filtered == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "lid_sensor_filtered = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(lid_sensor_filtered));

EXIT:
    return;
}

/** Change notifications for proximity_sensor_actual
 */
static void pwrkey_datapipe_proximity_sensor_actual_cb(gconstpointer data)
{
    cover_state_t prev = proximity_sensor_actual;
    proximity_sensor_actual = GPOINTER_TO_INT(data);

    if( proximity_sensor_actual == COVER_UNDEF )
        proximity_sensor_actual = COVER_OPEN;

    if( proximity_sensor_actual == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "proximity_sensor_actual = %s -> %s",
            proximity_state_repr(prev),
            proximity_state_repr(proximity_sensor_actual));

EXIT:
    return;
}

/** Handle ngfd_service_state notifications
 *
 * @param data service availability (as void pointer)
 */
static void
pwrkey_datapipe_ngfd_service_state_cb(gconstpointer data)
{
    service_state_t prev = ngfd_service_state;
    ngfd_service_state = GPOINTER_TO_INT(data);

    if( ngfd_service_state == prev )
        goto EXIT;

    mce_log(LL_NOTICE, "ngfd_service_state = %s -> %s",
            service_state_repr(prev),
            service_state_repr(ngfd_service_state));

    if( ngfd_service_state != SERVICE_STATE_RUNNING )
        xngf_delete_client();

EXIT:
    return;
}

/**
 * Datapipe trigger for the [power] key
 *
 * @param data A pointer to the input_event struct
 */
static void
pwrkey_datapipes_keypress_event_cb(gconstpointer const data)
{
    /* Faulty/aged physical power key buttons can generate
     * bursts of press and release events that are then
     * interpreted as double presses. To avoid this we
     * ignore power key presses that occur so soon after
     * previous release that they are unlikely to be
     * caused by human activity. */

    /* Minimum delay between power key release and press. */
    static const int64_t press_delay = 50;

    /* Time limit for accepting the next power key press */
    static int64_t press_limit = 0;

    const struct input_event * const *evp;
    const struct input_event *ev;

    if( !(evp = data) )
        goto EXIT;

    if( !(ev = *evp) )
        goto EXIT;

    switch( ev->type ) {
    case EV_KEY:
        switch( ev->code ) {
        case KEY_POWER:
            if( ev->value == 1 ) {
                if( mce_lib_get_boot_tick() < press_limit ) {
                    /* Too soon after the previous powerkey
                     * release -> assume faulty hw sending
                     * bursts of presses */
                    mce_log(LL_CRUCIAL, "powerkey press ignored");
                }
                else {
                    mce_log(LL_CRUCIAL, "powerkey pressed");
                    /* Detect repeated power key pressing while
                     * proximity sensor is covered; assume it means
                     * the sensor is stuck and user wants to be able
                     * to turn on the display regardless of the sensor
                     * state */
                    pwrkey_ps_override_evaluate();

                    /* Power key pressed */
                    pwrkey_stm_powerkey_pressed();

                    /* Some devices report both power key press and release
                     * already when the physical button is pressed down.
                     * Other devices wait for physical release before
                     * reporting key release. And in some devices it depends
                     * on whether the device is suspended or not.
                     *
                     * To normalize behavior in default configuration (i.e.
                     * begin display power up already on power key press
                     * without waiting for user to lift finger off the button):
                     * Synthetize key release, if no actions are bound to long
                     * power key press from display off state. */
                    if( pwrkey_stm_display_state == MCE_DISPLAY_OFF ) {
                        if( !pwrkey_actions_from_display_off.mask_long ) {
                            mce_log(LL_DEBUG, "powerkey release simulated");
                            pwrkey_stm_powerkey_released();
                        }
                    }
                }
            }
            else if( ev->value == 0 ) {
                mce_log(LL_CRUCIAL, "powerkey released");
                /* Power key released */
                pwrkey_stm_powerkey_released();

                /* Adjust time limit for accepting the next power
                 * key press */
                press_limit = mce_lib_get_boot_tick() + press_delay;
            }

            pwrkey_stm_rethink_wakelock();
            break;

        case KEY_HOME:
            if( ev->value == 1 ) {
                mce_log(LL_CRUCIAL, "homekey pressed");
                homekey_stm_set_pressed(true);
            }
            else if( ev->value == 0 ) {
                mce_log(LL_CRUCIAL, "homekey released");
                homekey_stm_set_pressed(false);
            }
            break;

        default:
            break;
        }
        break;

    case EV_MSC:
        if( ev->code == MSC_GESTURE ) {
            mce_log(LL_CRUCIAL, "gesture(%d)", ev->value);
            pwrkey_actions_do_gesture(ev->value);
        }
        break;

    default:
        break;
    }

EXIT:
    return;
}

/** Handle call state change notifications
 *
 * @param data call state (as void pointer)
 */
static void
pwrkey_datapipe_call_state_cb(gconstpointer data)
{
    call_state_t prev = call_state;
    call_state = GPOINTER_TO_INT(data);

    if( call_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "call_state = %s -> %s",
            call_state_repr(prev),
            call_state_repr(call_state));

    if( pwrkey_ignore_incoming_call ) {
        mce_log(LL_DEBUG, "stop ignoring incoming calls");
        pwrkey_ignore_incoming_call = false;
    }

EXIT:
    return;
}

/** Handle alarm ui state change notifications
 *
 * @param data alarm ui state (as void pointer)
 */
static void
pwrkey_datapipe_alarm_ui_state_cb(gconstpointer data)
{
    alarm_ui_state_t prev = alarm_ui_state;
    alarm_ui_state = GPOINTER_TO_INT(data);

    if( alarm_ui_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "alarm_ui_state = %s -> %s",
            alarm_state_repr(prev),
            alarm_state_repr(alarm_ui_state));

EXIT:
    return;
}

/** Change notifications for devicelock_service_state
 */
static void pwrkey_datapipe_devicelock_service_state_cb(gconstpointer data)
{
    service_state_t prev = devicelock_service_state;
    devicelock_service_state = GPOINTER_TO_INT(data);

    if( devicelock_service_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "devicelock_service_state = %s -> %s",
            service_state_repr(prev),
            service_state_repr(devicelock_service_state));

    /* no immediate action, but see pwrkey_action_devlock() */

EXIT:
    return;
}

/** Change notifications for enroll_in_progress
 */
static void pwrkey_datapipe_enroll_in_progress_cb(gconstpointer data)
{
    fpstate_t prev = enroll_in_progress;
    enroll_in_progress = GPOINTER_TO_INT(data);

    if( enroll_in_progress == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "enroll_in_progress = %s -> %s",
            prev ? "true" : "false",
            enroll_in_progress ? "true" : "false");

    /* no immediate action, but see pwrkey_stm_ignore_action() */

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t pwrkey_datapipe_handlers[] =
{
    // input triggers
    {
        .datapipe = &keypress_event_pipe,
        .input_cb = pwrkey_datapipes_keypress_event_cb,
    },
    // output triggers
    {
        .datapipe  = &ngfd_service_state_pipe,
        .output_cb = pwrkey_datapipe_ngfd_service_state_cb,
    },
    {
        .datapipe  = &system_state_pipe,
        .output_cb = pwrkey_datapipe_system_state_cb,
    },
    {
        .datapipe  = &display_state_curr_pipe,
        .output_cb = pwrkey_datapipe_display_state_curr_cb,
    },
    {
        .datapipe  = &display_state_next_pipe,
        .output_cb = pwrkey_datapipe_display_state_next_cb,
    },
    {
        .datapipe  = &lid_sensor_filtered_pipe,
        .output_cb = pwrkey_datapipe_lid_sensor_filtered_cb,
    },
    {
        .datapipe  = &proximity_sensor_actual_pipe,
        .output_cb = pwrkey_datapipe_proximity_sensor_actual_cb,
    },
    {
        .datapipe  = &alarm_ui_state_pipe,
        .output_cb = pwrkey_datapipe_alarm_ui_state_cb,
    },
    {
        .datapipe  = &call_state_pipe,
        .output_cb = pwrkey_datapipe_call_state_cb,
    },
    {
        .datapipe  = &devicelock_service_state_pipe,
        .output_cb = pwrkey_datapipe_devicelock_service_state_cb,
    },
    {
        .datapipe  = &enroll_in_progress_pipe,
        .output_cb = pwrkey_datapipe_enroll_in_progress_cb,
    },

    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t pwrkey_datapipe_bindings =
{
    .module   = "powerkey",
    .handlers = pwrkey_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void
pwrkey_datapipes_init(void)
{
    datapipe_bindings_init(&pwrkey_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void
pwrkey_datapipes_quit(void)
{
    datapipe_bindings_quit(&pwrkey_datapipe_bindings);
}

/* ========================================================================= *
 * NGFD_GLUE
 * ========================================================================= */

static NgfClient       *ngf_client_hnd = 0;
static DBusConnection  *ngf_dbus_con   = 0;
static uint32_t         ngf_event_id   = 0;

static const char *
xngf_state_repr(NgfEventState state)
{
    const char *repr = "unknown";

    switch( state ) {
    case NGF_EVENT_FAILED:    repr = "failed";    break;
    case NGF_EVENT_COMPLETED: repr = "completed"; break;
    case NGF_EVENT_PLAYING:   repr = "playing";   break;
    case NGF_EVENT_PAUSED:    repr = "paused";    break;
    default: break;
    }

    return repr;
}
static void
xngf_status_cb(NgfClient *client, uint32_t event_id, NgfEventState state, void *userdata)
{
    (void) client;
    (void) userdata;

    mce_log(LL_DEBUG, "%s(%d)", xngf_state_repr(state), event_id);

    switch( state ) {
    default:
    case NGF_EVENT_PLAYING:
    case NGF_EVENT_PAUSED:
        break;

    case NGF_EVENT_COMPLETED:
        ngf_event_id = 0;
        break;

    case NGF_EVENT_FAILED:
        mce_log(LL_ERR, "Failed to play id %d", event_id);
        ngf_event_id = 0;
        break;

    }
}

static bool
xngf_create_client(void)
{
    if( !ngf_dbus_con ) {
        mce_log(LL_WARN, "can't use ngfd - no dbus connection");
        goto EXIT;
    }

    if( ngfd_service_state != SERVICE_STATE_RUNNING ) {
        mce_log(LL_WARN, "can't use ngfd - service not running");
        goto EXIT;
    }

    if( ngf_client_hnd )
        goto EXIT;

    ngf_client_hnd = ngf_client_create(NGF_TRANSPORT_DBUS, ngf_dbus_con);
    if( !ngf_client_hnd ) {
        mce_log(LL_WARN, "can't use ngfd - failed to create client");
        goto EXIT;
    }

    ngf_client_set_callback(ngf_client_hnd, xngf_status_cb, NULL);

    mce_log(LL_DEBUG, "ngfd client created");

EXIT:
    return ngf_client_hnd != 0;
}

static void
xngf_delete_client(void)
{
    if( ngf_client_hnd ) {
        ngf_client_destroy(ngf_client_hnd), ngf_client_hnd = 0;
        mce_log(LL_DEBUG, "ngfd client deleted");
    }

    ngf_event_id = 0;
}

static void
xngf_play_event(const char *event_name)
{
    if( ngf_event_id ) {
        mce_log(LL_WARN, "previous event not finished yet");
        goto EXIT;
    }

    if( !xngf_create_client() )
        goto EXIT;

    ngf_event_id = ngf_client_play_event (ngf_client_hnd, event_name, NULL);

    mce_log(LL_DEBUG, "event=%s, id=%d", event_name, ngf_event_id);

EXIT:
    return;
}

static void
xngf_init(void)
{
    ngf_dbus_con = dbus_connection_get();
}

static void
xngf_quit(void)
{
    xngf_delete_client();

    if (ngf_dbus_con)
        dbus_connection_unref(ngf_dbus_con), ngf_dbus_con = 0;
}

/* ========================================================================= *
 * MODULE_INTEFACE
 * ========================================================================= */

/**
 * Init function for the powerkey component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_powerkey_init(void)
{
    pwrkey_datapipes_init();

    pwrkey_dbus_init();

    pwrkey_setting_init();

    xngf_init();

    return TRUE;
}

/**
 * Exit function for the powerkey component
 *
 * @todo D-Bus unregistration
 */
void mce_powerkey_exit(void)
{
    xngf_quit();

    pwrkey_dbus_quit();

    pwrkey_setting_quit();

    pwrkey_datapipes_quit();

    /* Remove all timer sources & release wakelock */
    pwrkey_stm_terminate();

    return;
}
