/**
 * @file fingerprint.c
 *
 * Fingerprint daemon tracking module for the Mode Control Entity
 * <p>
 * Copyright (c) 2015-2018 Jolla Ltd.
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

#include <gmodule.h>

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * LED_CONTROL
 * ------------------------------------------------------------------------- */

static void     fingerprint_scanning_led_activate  (bool activate);

static void     fingerprint_acquired_led_activate  (bool enable);
static gboolean fingerprint_acquired_led_timer_cb  (gpointer aptr);
static void     fingerprint_acquired_led_trigger   (void);
static void     fingerprint_acquired_led_cancel    (void);

/* ------------------------------------------------------------------------- *
 * STATE_MANAGEMENT
 * ------------------------------------------------------------------------- */

static void     fingerprint_update_fpstate                (fpstate_t state);
static void     fingerprint_update_enroll_in_progress     (void);
static void     fingerprint_generate_activity             (void);

/* ------------------------------------------------------------------------- *
 * DATAPIPE_HANDLERS
 * ------------------------------------------------------------------------- */

static void     fingerprint_datapipe_fpd_service_state_cb (gconstpointer data);
static void     fingerprint_datapipe_system_state_cb      (gconstpointer data);
static void     fingerprint_datapipe_devicelock_state_cb  (gconstpointer data);
static void     fingerprint_datapipe_submode_cb           (gconstpointer data);
static void     fingerprint_datapipe_display_state_next_cb(gconstpointer data);
static void     fingerprint_datapipe_init                 (void);
static void     fingerprint_datapipe_quit                 (void);

/* ------------------------------------------------------------------------- *
 * DBUS_HANDLERS
 * ------------------------------------------------------------------------- */

static gboolean fingerprint_dbus_fpstate_changed_cb       (DBusMessage *const msg);
static gboolean fingerprint_dbus_fpacquired_info_cb       (DBusMessage *const msg);
static void     fingerprint_dbus_init                     (void);
static void     fingerprint_dbus_quit                     (void);

/* ------------------------------------------------------------------------- *
 * DBUS_IPC
 * ------------------------------------------------------------------------- */

static void     fingerprint_dbus_fpstate_query_cb         (DBusPendingCall *pc, void *aptr);
static void     fingerprint_dbus_fpstate_query_cancel     (void);
static void     fingerprint_dbus_fpstate_query_start      (void);

/* ------------------------------------------------------------------------- *
 * MODULE_LOAD_UNLOAD
 * ------------------------------------------------------------------------- */

G_MODULE_EXPORT const gchar *g_module_check_init (GModule *module);
G_MODULE_EXPORT void         g_module_unload     (GModule *module);

/* ========================================================================= *
 * TRACKED_STATES
 * ========================================================================= */

/** Cached fpd service availability; assume unknown */
static service_state_t fpd_service_state = SERVICE_STATE_UNDEF;

/** Cached system_state; assume unknown */
static system_state_t system_state = MCE_SYSTEM_STATE_UNDEF;

/** Cached devicelock_state ; assume unknown */
static devicelock_state_t devicelock_state = DEVICELOCK_STATE_UNDEFINED;

/** Cached submode ; assume invalid */
static submode_t submode = MCE_SUBMODE_INVALID;

/** Cached target display_state; assume unknown */
static display_state_t display_state_next = MCE_DISPLAY_UNDEF;

/* ========================================================================= *
 * MANAGED_STATES
 * ========================================================================= */

/** Tracked fpd operational state; assume unknown */
static fpstate_t fpstate = FPSTATE_UNSET;

/** Tracked fingerprint enroll status; assume not in progress */
static bool enroll_in_progress = false;

/* ========================================================================= *
 * LED_CONTROL
 * ========================================================================= */

/** Control led pattern for indicating fingerprint scanner status
 *
 * @param activate true to activate led, false to deactivate
 */
static void
fingerprint_scanning_led_activate(bool activate)
{
    static bool activated = false;
    if( activated != activate ) {
        datapipe_exec_output_triggers((activated = activate) ?
                                      &led_pattern_activate_pipe :
                                      &led_pattern_deactivate_pipe,
                                      MCE_LED_PATTERN_SCANNING_FINGERPRINT,
                                      USE_INDATA);
    }
}

/** Control led pattern for indicating fingerprint acquisition events
 *
 * @param activate true to activate led, false to deactivate
 */
static void
fingerprint_acquired_led_activate(bool activate)
{
    static bool activated = false;
    if( activated != activate ) {
        datapipe_exec_output_triggers((activated = activate) ?
                                      &led_pattern_activate_pipe :
                                      &led_pattern_deactivate_pipe,
                                      MCE_LED_PATTERN_FINGERPRINT_ACQUIRED,
                                      USE_INDATA);
    }
}

/** Timer id for: Stop fingerprint acquisition event led */
static guint fingerprint_acquired_led_timer_id = 0;

/** Timer callback for: Stop fingerprint acquisition event led */
static gboolean
fingerprint_acquired_led_timer_cb(gpointer aptr)
{
    (void)aptr;
    fingerprint_acquired_led_timer_id = 0;
    fingerprint_acquired_led_activate(false);
    return FALSE;
}

/** Briefly activate fingerprint acquisition event led
 */
static void
fingerprint_acquired_led_trigger(void)
{
    if( fingerprint_acquired_led_timer_id )
        g_source_remove(fingerprint_acquired_led_timer_id);
    fingerprint_acquired_led_timer_id =
        g_timeout_add(200, fingerprint_acquired_led_timer_cb, 0);
    fingerprint_acquired_led_activate(true);
}

/** Dctivate fingerprint acquisition event led
 */
static void
fingerprint_acquired_led_cancel(void)
{
    if( fingerprint_acquired_led_timer_id ) {
        g_source_remove(fingerprint_acquired_led_timer_id),
            fingerprint_acquired_led_timer_id = 0;
    }
    fingerprint_acquired_led_activate(false);
}

/* ========================================================================= *
 * STATE_MANAGEMENT
 * ========================================================================= */

/** Update fpstate_pipe content
 *
 * @param state  fingerprint operation state reported by fpd
 */
static void
fingerprint_update_fpstate(fpstate_t state)
{
    fpstate_t prev = fpstate;
    fpstate = state;

    if( fpstate == prev )
        goto EXIT;

    mce_log(LL_NOTICE, "fpstate: %s -> %s",
            fpstate_repr(prev),
            fpstate_repr(fpstate));

    datapipe_exec_full(&fpstate_pipe, GINT_TO_POINTER(fpstate),
                       USE_INDATA, CACHE_INDATA);

    switch( fpstate ) {
    case FPSTATE_ENROLLING:
    case FPSTATE_IDENTIFYING:
    case FPSTATE_VERIFYING:
        fingerprint_scanning_led_activate(true);
        break;
    default:
        fingerprint_scanning_led_activate(false);
        break;
    }

    fingerprint_update_enroll_in_progress();

EXIT:
    return;
}

/** Evaluate value for enroll_in_progress_pipe
 *
 * Enrolling a fingerprint needs to block display blanking.
 *
 * To avoid hiccups / false negatives we try to be relatively
 * sure that system state is such that settings ui at least
 * in theory can be handing enroll operation on screen.
 *
 * Require that:
 * - fingerprint daemon is in enrolling state
 * - display is already on
 * - lockscreen is not active
 * - device is unlocked
 * - we are in user mode
 *
 * @return true if fp enroll is in progress, false otherwise
 */
static bool
fingerprint_evaluate_enroll_in_progress(void)
{
    bool in_progress = false;

    if( fpstate != FPSTATE_ENROLLING )
        goto EXIT;

    if( display_state_next != MCE_DISPLAY_ON &&
        display_state_next != MCE_DISPLAY_DIM )
        goto EXIT;

    if( submode & MCE_SUBMODE_TKLOCK )
        goto EXIT;

    if( devicelock_state != DEVICELOCK_STATE_UNLOCKED )
        goto EXIT;

    if( system_state != MCE_SYSTEM_STATE_USER )
        goto EXIT;

    in_progress = true;

EXIT:
    return in_progress;
}

/** Update enroll_in_progress_pipe content
 */
static void
fingerprint_update_enroll_in_progress(void)
{
    bool prev = enroll_in_progress;
    enroll_in_progress = fingerprint_evaluate_enroll_in_progress();

    if( enroll_in_progress == prev )
        goto EXIT;

    mce_log(LL_NOTICE, "enroll_in_progress: %s -> %s",
            prev ? "true" : "false",
            enroll_in_progress ? "true" : "false");

    datapipe_exec_full(&enroll_in_progress_pipe,
                       GINT_TO_POINTER(enroll_in_progress),
                       USE_INDATA, CACHE_INDATA);
EXIT:
    return;
}

/** Generate user activity to reset blanking timers
 */
static void
fingerprint_generate_activity(void)
{
    /* Display must be in powered on state */
    switch( display_state_next ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        break;
    default:
        goto EXIT;
    }

    mce_log(LL_DEBUG, "generating activity from fingerprint sensor");
    datapipe_exec_full(&inactivity_event_pipe,
                       GINT_TO_POINTER(FALSE),
                       USE_INDATA, CACHE_OUTDATA);

EXIT:
    return;
}

/* ========================================================================= *
 * DATAPIPE_HANDLERS
 * ========================================================================= */

/** Notification callback for fpd_service_state_pipe
 *
 * @param data  service_state_t value as void pointer
 */
static void
fingerprint_datapipe_fpd_service_state_cb(gconstpointer data)
{
    service_state_t prev = fpd_service_state;
    fpd_service_state = GPOINTER_TO_INT(data);

    if( fpd_service_state == prev )
        goto EXIT;

    mce_log(LL_NOTICE, "fpd_service_state = %s -> %s",
            service_state_repr(prev),
            service_state_repr(fpd_service_state));

    if( fpd_service_state == SERVICE_STATE_RUNNING ) {
        fingerprint_dbus_fpstate_query_start();
    }
    else {
        fingerprint_dbus_fpstate_query_cancel();
        fingerprint_update_fpstate(FPSTATE_UNSET);
    }

EXIT:
    return;
}

/** Notification callback for system_state_pipe
 *
 * @param data  system_state_t value as void pointer
 */
static void
fingerprint_datapipe_system_state_cb(gconstpointer data)
{
    system_state_t prev = system_state;
    system_state = GPOINTER_TO_INT(data);

    if( prev == system_state )
        goto EXIT;

    mce_log(LL_DEBUG, "system_state: %s -> %s",
            system_state_repr(prev),
            system_state_repr(system_state));

    fingerprint_update_enroll_in_progress();

EXIT:
    return;
}

/** Notification callback for devicelock_state_pipe
 *
 * @param data  devicelock_state_t value as void pointer
 */
static void
fingerprint_datapipe_devicelock_state_cb(gconstpointer data)
{
    devicelock_state_t prev = devicelock_state;
    devicelock_state = GPOINTER_TO_INT(data);

    if( devicelock_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "devicelock_state = %s -> %s",
            devicelock_state_repr(prev),
            devicelock_state_repr(devicelock_state));

    fingerprint_update_enroll_in_progress();

EXIT:
    return;
}

/** Notification callback for submode_pipe
 *
 * @param data  submode_t value as void pointer
 */
static void
fingerprint_datapipe_submode_cb(gconstpointer data)
{
    submode_t prev = submode;
    submode = GPOINTER_TO_INT(data);

    if( submode == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "submode = %s", submode_change_repr(prev, submode));

    fingerprint_update_enroll_in_progress();

EXIT:
    return;
}

/** Notification callback for display_state_next_pipe
 *
 * @param data  display_state_t value as void pointer
 */
static void
fingerprint_datapipe_display_state_next_cb(gconstpointer data)
{
    display_state_t prev = display_state_next;
    display_state_next = GPOINTER_TO_INT(data);

    if( display_state_next == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_next = %s -> %s",
            display_state_repr(prev),
            display_state_repr(display_state_next));

    fingerprint_update_enroll_in_progress();

EXIT:
    return;

}

/** Array of datapipe handlers */
static datapipe_handler_t fingerprint_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &fpd_service_state_pipe,
        .output_cb = fingerprint_datapipe_fpd_service_state_cb,
    },
    {
        .datapipe  = &system_state_pipe,
        .output_cb = fingerprint_datapipe_system_state_cb,
    },
    {
        .datapipe  = &devicelock_state_pipe,
        .output_cb = fingerprint_datapipe_devicelock_state_cb,
    },
    {
        .datapipe  = &submode_pipe,
        .output_cb = fingerprint_datapipe_submode_cb,
    },
    {
        .datapipe  = &display_state_next_pipe,
        .output_cb = fingerprint_datapipe_display_state_next_cb,
    },
    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t fingerprint_datapipe_bindings =
{
    .module   = "fingerprint",
    .handlers = fingerprint_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void
fingerprint_datapipe_init(void)
{
    // triggers
    datapipe_bindings_init(&fingerprint_datapipe_bindings);
}

/** Remove triggers/filters from datapipes */
static void
fingerprint_datapipe_quit(void)
{
    // triggers
    datapipe_bindings_quit(&fingerprint_datapipe_bindings);
}

/* ========================================================================= *
 * DBUS_HANDLERS
 * ========================================================================= */

/** Handle fpd operation state change signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
fingerprint_dbus_fpstate_changed_cb(DBusMessage *const msg)
{
    const char *state = 0;
    DBusError   err   = DBUS_ERROR_INIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &state,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "parse error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    fingerprint_update_fpstate(fpstate_parse(state));

EXIT:
    dbus_error_free(&err);

    return TRUE;
}

/** Handle fpd acquisition info signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */

static gboolean
fingerprint_dbus_fpacquired_info_cb(DBusMessage *const msg)
{
    const char *info = 0;
    DBusError   err  = DBUS_ERROR_INIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &info,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "parse error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "fpacquired: %s", info);

    /* Fingerprint aquisition info notifications during
     * enroll, identify and verify operations must delay
     * display blanking.
     */

    switch( fpstate ) {
    case FPSTATE_ENROLLING:
    case FPSTATE_IDENTIFYING:
    case FPSTATE_VERIFYING:
        fingerprint_generate_activity();
        break;
    default:
        break;
    }

    fingerprint_acquired_led_trigger();

EXIT:
    dbus_error_free(&err);

    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t fingerprint_dbus_handlers[] =
{
    /* signals */
    {
        .interface = FINGERPRINT1_DBUS_INTERFACE,
        .name      = FINGERPRINT1_DBUS_SIG_STATE_CHANGED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = fingerprint_dbus_fpstate_changed_cb,
    },
    {
        .interface = FINGERPRINT1_DBUS_INTERFACE,
        .name      = FINGERPRINT1_DBUS_SIG_ACQUISITION_INFO,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = fingerprint_dbus_fpacquired_info_cb,
    },

    /* sentinel */
    {
        .interface = 0
    }
};

/** Install dbus message handlers
 */
static void
fingerprint_dbus_init(void)
{
    mce_dbus_handler_register_array(fingerprint_dbus_handlers);
}

/** Remove dbus message handlers
 */
static void
fingerprint_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(fingerprint_dbus_handlers);
}

/* ========================================================================= *
 * DBUS_IPC
 * ========================================================================= */

static DBusPendingCall *fingerprint_dbus_fpstate_query_pc = 0;

/** Handle reply to async fpstate query
 *
 * @param pc    pending call handle
 * @param aptr  (unused) user data pointer
 */
static void
fingerprint_dbus_fpstate_query_cb(DBusPendingCall *pc, void *aptr)
{
    (void)aptr;

    DBusMessage *rsp   = 0;
    DBusError    err   = DBUS_ERROR_INIT;
    const char  *state = 0;

    if( pc != fingerprint_dbus_fpstate_query_pc )
        goto EXIT;

    fingerprint_dbus_fpstate_query_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
        mce_log(LL_WARN, "no reply");
        goto EXIT;
    }

    if( dbus_set_error_from_message(&err, rsp) ||
        !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_STRING, &state,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    fingerprint_update_fpstate(fpstate_parse(state));

EXIT:
    if( rsp ) dbus_message_unref(rsp);

    dbus_error_free(&err);

    dbus_pending_call_unref(pc);

    return;
}

/** Cancel pending async fpstate query
 */
static void
fingerprint_dbus_fpstate_query_cancel(void)
{
    if( fingerprint_dbus_fpstate_query_pc ) {
        dbus_pending_call_cancel(fingerprint_dbus_fpstate_query_pc);
        dbus_pending_call_unref(fingerprint_dbus_fpstate_query_pc);
        fingerprint_dbus_fpstate_query_pc = 0;
    }
}

/** Initiate async query to find out current fpstate
 */
static void
fingerprint_dbus_fpstate_query_start(void)
{
    fingerprint_dbus_fpstate_query_cancel();

    dbus_send_ex(FINGERPRINT1_DBUS_SERVICE,
                 FINGERPRINT1_DBUS_ROOT_OBJECT,
                 FINGERPRINT1_DBUS_INTERFACE,
                 FINGERPRINT1_DBUS_REQ_GET_STATE,
                 fingerprint_dbus_fpstate_query_cb, 0, 0,
                 &fingerprint_dbus_fpstate_query_pc,
                 DBUS_TYPE_INVALID);
}

/* ========================================================================= *
 * MODULE_LOAD_UNLOAD
 * ========================================================================= */

/** Init function for the fpd tracking module
 *
 * @param module (unused) module handle
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *
g_module_check_init(GModule *module)
{
    (void)module;

    fingerprint_datapipe_init();
    fingerprint_dbus_init();

    return NULL;
}

/** Exit function for the fpd tracking module
 *
 * @param module (unused) module handle
 */
void
g_module_unload(GModule *module)
{
    (void)module;

    fingerprint_dbus_quit();
    fingerprint_datapipe_quit();
    fingerprint_dbus_fpstate_query_cancel();

    fingerprint_scanning_led_activate(false);
    fingerprint_acquired_led_cancel();
    return;
}
