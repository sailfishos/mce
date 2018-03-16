/**
 * @file inactivity.c
 * Inactivity module -- this implements inactivity logic for MCE
 * <p>
 * Copyright © 2007-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright © 2015      Jolla Ltd.
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

#include "inactivity.h"

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-dbus.h"
#include "../mce-dsme.h"
#include "../mce-hbtimer.h"
#include "../mce-setting.h"

#ifdef ENABLE_WAKELOCKS
# include "../libwakelock.h"
#endif

#include <string.h>

#include <mce/dbus-names.h>

#include <gmodule.h>

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** Module name */
#define MODULE_NAME             "inactivity"

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

/** Maximum amount of monitored activity callbacks */
#define ACTIVITY_CB_MAX_MONITORED       16

/** Duration of suspend blocking after sending inactivity signals */
#define MIA_KEEPALIVE_DURATION_MS 5000

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * HELPER_FUNCTIONS
 * ------------------------------------------------------------------------- */

static const char *mia_inactivity_repr     (bool inactive);
static void        mia_generate_activity   (void);
static void        mia_generate_inactivity (void);

/* ------------------------------------------------------------------------- *
 * DBUS_ACTION
 * ------------------------------------------------------------------------- */

/** D-Bus action */
typedef struct {
    /** D-Bus activity callback owner */
    gchar *owner;

    /** D-Bus service */
    gchar *service;

    /** D-Bus path */
    gchar *path;

    /** D-Bus interface */
    gchar *interface;

    /** D-Bus method name */
    gchar *method_name;
} mia_action_t;

static mia_action_t *mia_action_create (const char *owner, const char *service, const char *path, const char *interface, const char *method);
static void          mia_action_delete (mia_action_t *self);

/* ------------------------------------------------------------------------- *
 * DATAPIPE_TRACKING
 * ------------------------------------------------------------------------- */

static bool     mia_activity_allowed                 (void);
static void     mia_datapipe_inactivity_event_cb     (gconstpointer data);
static void     mia_datapipe_device_inactive_cb      (gconstpointer data);
static void     mia_datapipe_proximity_sensor_actual_cb(gconstpointer data);
static void     mia_datapipe_inactivity_delay_cb     (gconstpointer data);
static void     mia_datapipe_submode_cb              (gconstpointer data);
static void     mia_datapipe_alarm_ui_state_cb       (gconstpointer data);
static void     mia_datapipe_call_state_cb           (gconstpointer data);
static void     mia_datapipe_system_state_cb         (gconstpointer data);
static void     mia_datapipe_display_state_next_cb   (gconstpointer data);
static void     mia_datapipe_interaction_expected_cb (gconstpointer data);
static void     mia_datapipe_charger_state_cb        (gconstpointer data);
static void     mia_datapipe_init_done_cb            (gconstpointer data);
static void     mia_datapipe_osupdate_running_cb     (gconstpointer data);

static void     mia_datapipe_check_initial_state     (void);

static void     mia_datapipe_init(void);
static void     mia_datapipe_quit(void);

/* ------------------------------------------------------------------------- *
 * SUSPEND_BLOCK
 * ------------------------------------------------------------------------- */

#ifdef ENABLE_WAKELOCKS
static void      mia_keepalive_rethink (void);
static gboolean  mia_keepalive_cb      (gpointer aptr);
static void      mia_keepalive_start   (void);
static void      mia_keepalive_stop    (void);
#endif

/* ------------------------------------------------------------------------- *
 * DBUS_HANDLERS
 * ------------------------------------------------------------------------- */

static gboolean mia_dbus_activity_action_owner_cb  (DBusMessage *const sig);
static gboolean mia_dbus_add_activity_action_cb    (DBusMessage *const msg);
static gboolean mia_dbus_remove_activity_action_cb (DBusMessage *const msg);

static gboolean mia_dbus_send_inactivity_state     (DBusMessage *const method_call);
static gboolean mia_dbus_get_inactivity_state      (DBusMessage *const req);

static void     mia_dbus_init(void);
static void     mia_dbus_quit(void);

/* ------------------------------------------------------------------------- *
 * ACTIVITY_ACTIONS
 * ------------------------------------------------------------------------- */

static void     mia_activity_action_remove      (const char *owner);
static bool     mia_activity_action_add         (const char *owner, const char *service, const char *path, const char *interface, const char *method);
static void     mia_activity_action_remove_all  (void);
static void     mia_activity_action_execute_all (void);

/* ------------------------------------------------------------------------- *
 * INACTIVITY_TIMER
 * ------------------------------------------------------------------------- */

static gboolean mia_timer_cb    (gpointer data);
static void     mia_timer_start (void);
static void     mia_timer_stop  (void);

static void     mia_timer_init  (void);
static void     mia_timer_quit  (void);

/* ------------------------------------------------------------------------- *
 * SHUTDOWN_TIMER
 * ------------------------------------------------------------------------- */

static gboolean mia_shutdown_timer_cb       (gpointer aptr);
static void     mia_shutdown_timer_stop     (void);
static void     mia_shutdown_timer_start    (void);
static bool     mia_shutdown_timer_wanted   (void);
static void     mia_shutdown_timer_rethink  (void);
static void     mia_shutdown_timer_restart  (void);
static void     mia_shutdown_timer_init     (void);
static void     mia_shutdown_timer_quit     (void);

/* ------------------------------------------------------------------------- *
 * SETTING_TRACKING
 * ------------------------------------------------------------------------- */

static void     mia_setting_changed_cb      (GConfClient *gcc, guint id, GConfEntry *entry, gpointer data);
static void     mia_setting_init            (void);
static void     mia_setting_quit            (void);

/* ------------------------------------------------------------------------- *
 * MODULE_LOAD_UNLOAD
 * ------------------------------------------------------------------------- */

G_MODULE_EXPORT const gchar *g_module_check_init (GModule *module);
G_MODULE_EXPORT void         g_module_unload     (GModule *module);

/* ------------------------------------------------------------------------- *
 * STATE_DATA
 * ------------------------------------------------------------------------- */

/** List of activity callbacks */
static GSList *activity_action_list = NULL;

/** List of monitored activity requesters */
static GSList *activity_action_owners = NULL;

/** Heartbeat timer for inactivity timeout */
static mce_hbtimer_t *inactivity_timer_hnd = 0;

/** Heartbeat timer for idle shutdown */
static mce_hbtimer_t *shutdown_timer_hnd = 0;

/** Flag for: Idle shutdown already triggered */
static bool shutdown_timer_triggered = false;

/** Cached device inactivity state
 *
 * Default to inactive. Initial state is evaluated and broadcast over
 * D-Bus during mce startup - see mia_datapipe_check_initial_state().
 */
static gboolean device_inactive = TRUE;

/* Cached submode bitmask; assume in transition at startup */
static submode_t submode = MCE_SUBMODE_TRANSITION;

/** Cached alarm ui state */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_INVALID_INT32;

/** Cached call state */
static call_state_t call_state = CALL_STATE_INVALID;

/* Cached system state */
static system_state_t system_state = MCE_SYSTEM_STATE_UNDEF;

/** Cached display state */
static display_state_t display_state_next = MCE_DISPLAY_UNDEF;

/** Cached inactivity timeout delay [s] */
static gint device_inactive_delay = DEFAULT_INACTIVITY_DELAY;

/** Cached proximity sensor state */
static cover_state_t proximity_sensor_actual = COVER_UNDEF;

/** Cached Interaction expected state */
static bool interaction_expected = false;

/** Cached charger state; assume unknown */
static charger_state_t charger_state = CHARGER_STATE_UNDEF;

/** Cached init_done state; assume unknown */
static tristate_t init_done = TRISTATE_UNKNOWN;

/** Update mode is active; assume false */
static bool osupdate_running = false;

/** Setting for automatick shutdown delay after inactivity */
static gint    mia_shutdown_delay = MCE_DEFAULT_INACTIVITY_SHUTDOWN_DELAY;
static guint   mia_shutdown_delay_setting_id = 0;

/* ========================================================================= *
 * HELPER_FUNCTIONS
 * ========================================================================= */

/** Inactivity boolean to human readable string helper
 */
static const char *mia_inactivity_repr(bool inactive)
{
    return inactive ? "inactive" : "active";
}

/** Helper for attempting to switch to active state
 */
static void mia_generate_activity(void)
{
    datapipe_exec_full(&inactivity_event_pipe, GINT_TO_POINTER(FALSE),
                       USE_INDATA, CACHE_OUTDATA);
}

/** Helper for switching to inactive state
 */
static void mia_generate_inactivity(void)
{
    datapipe_exec_full(&inactivity_event_pipe, GINT_TO_POINTER(TRUE),
                       USE_INDATA, CACHE_OUTDATA);
}

/* ========================================================================= *
 * DBUS_ACTION
 * ========================================================================= */

/** Create D-Bus action object
 *
 * @param owner      Private D-Bus name of the owner of the action
 * @param service    D-Bus name of the service to send message to
 * @param path       Object path to use
 * @param interface  Inteface to use
 * @param method     Name of the method to invoke
 *
 * @return pointer to initialized D-Bus action object
 */
static mia_action_t *mia_action_create(const char *owner,
                                       const char *service,
                                       const char *path,
                                       const char *interface,
                                       const char *method)
{
    mia_action_t *self = g_malloc0(sizeof *self);

    self->owner       = g_strdup(owner);
    self->service     = g_strdup(service);
    self->path        = g_strdup(path);
    self->interface   = g_strdup(interface);
    self->method_name = g_strdup(method);

    return self;
}

/** Delete D-Bus action object
 *
 * @param self Pointer to initialized D-Bus action object, or NULL
 */
static void mia_action_delete(mia_action_t *self)
{
    if( !self )
        goto EXIT;

    g_free(self->owner);
    g_free(self->service);
    g_free(self->path);
    g_free(self->interface);
    g_free(self->method_name);
    g_free(self);

EXIT:
    return;
}

/* ========================================================================= *
 * DATAPIPE_TRACKING
 * ========================================================================= */

static bool mia_activity_allowed(void)
{
    bool allowed = false;

    /* Never filter activity if display is in dimmed state.
     *
     * Whether we have arrived to dimmed state via expected or
     * unexpected routes, the touch input is active and ui side
     * event eater will ignore only the first event. If we do
     * not allow activity (and turn on the display) we will get
     * ui interaction in odd looking dimmed state that then gets
     * abruptly ended by blanking timer.
     */
    if( display_state_next == MCE_DISPLAY_DIM )
        goto ALLOW;

    /* Activity applies only when display is on */
    if( display_state_next != MCE_DISPLAY_ON ) {
        mce_log(LL_DEBUG, "display_state_curr = %s; ignoring activity",
                display_state_repr(display_state_next));
        goto DENY;
    }

    /* Activity applies only to USER mode */
    if( system_state != MCE_SYSTEM_STATE_USER ) {
        mce_log(LL_DEBUG, "system_state = %s; ignoring activity",
                system_state_repr(system_state));
        goto DENY;
    }

    /* Normally activity does not apply when lockscreen is active */
    if( submode & MCE_SUBMODE_TKLOCK ) {

        /* Active alarm */
        switch( alarm_ui_state ) {
        case MCE_ALARM_UI_RINGING_INT32:
        case MCE_ALARM_UI_VISIBLE_INT32:
            goto ALLOW;

        default:
            break;
        }

        /* Active call */
        switch( call_state ) {
        case CALL_STATE_RINGING:
        case CALL_STATE_ACTIVE:
            goto ALLOW;

        default:
            break;
        }

        /* Expecting user interaction */
        if( interaction_expected )
            goto ALLOW;

        goto DENY;
    }

ALLOW:
    allowed = true;

DENY:
    return allowed;
}

/** React to device inactivity requests
 *
 * @param data The unfiltered inactivity state;
 *             TRUE if the device is inactive,
 *             FALSE if the device is active
 */
static void mia_datapipe_inactivity_event_cb(gconstpointer data)
{
    gboolean inactive = GPOINTER_TO_INT(data);

    mce_log(LL_DEBUG, "input: inactivity=%s",
            mia_inactivity_repr(inactive));

    if( inactive ) {
        /* Inactivity is not repeated */
        if( device_inactive )
            goto EXIT;
    }
    else {
        /* Activity might not be allowed */
        if( !mia_activity_allowed() )
            goto EXIT;
    }

    datapipe_exec_full(&device_inactive_pipe,
                       GINT_TO_POINTER(inactive),
                       USE_INDATA, CACHE_OUTDATA);
EXIT:
    return;
}

/** React to device inactivity changes
 *
 * @param data Filtered inactivity state;
 *             TRUE if the device is inactive,
 *             FALSE if the device is active
 */
static void mia_datapipe_device_inactive_cb(gconstpointer data)
{
    gboolean prev = device_inactive;
    device_inactive = GPOINTER_TO_INT(data);

    /* Actions taken on transitions only */
    if( prev != device_inactive ) {
        mce_log(LL_DEBUG, "device_inactive: %s -> %s",
                mia_inactivity_repr(prev),
                mia_inactivity_repr(device_inactive));

        mia_dbus_send_inactivity_state(NULL);

        /* React to activity */
        if( !device_inactive )
            mia_activity_action_execute_all();

        mia_shutdown_timer_rethink();
    }

    /* Restart/stop timer */
    mia_timer_start();
}

/** Generate activity from proximity sensor uncover
 *
 * @param data proximity sensor state as void pointer
 */
static void mia_datapipe_proximity_sensor_actual_cb(gconstpointer data)
{
    cover_state_t prev = proximity_sensor_actual;
    proximity_sensor_actual = GPOINTER_TO_INT(data);

    if( proximity_sensor_actual == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "proximity_sensor_actual: %s -> %s",
            proximity_state_repr(prev),
            proximity_state_repr(proximity_sensor_actual));

    /* generate activity if proximity sensor is
     * uncovered and there is a incoming call */

    if( proximity_sensor_actual == COVER_OPEN &&
        call_state == CALL_STATE_RINGING ) {
        mce_log(LL_INFO, "proximity -> uncovered, call = ringing");
        mia_generate_activity();
    }

EXIT:
        return;
}

/** React to inactivity timeout change
 *
 * @param data inactivity timeout (as void pointer)
 */
static void mia_datapipe_inactivity_delay_cb(gconstpointer data)
{
    gint prev = device_inactive_delay;
    device_inactive_delay = GPOINTER_TO_INT(data);

    /* Sanitise timeout */
    if( device_inactive_delay <= 0 )
        device_inactive_delay = 30;

    if( device_inactive_delay == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "device_inactive_delay: %d -> %d",
            prev, device_inactive_delay);

    /* Reprogram timer */
    mia_timer_start();

EXIT:
    return;
}

/** Handle submode_pipe notifications
 *
 * @param data The submode stored in a pointer
 */
static void mia_datapipe_submode_cb(gconstpointer data)
{
    submode_t prev = submode;
    submode = GPOINTER_TO_INT(data);

    if( submode == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "submode = %s",
            submode_change_repr(prev, submode));

EXIT:
    return;
}

/** Handle alarm_ui_state_pipe notifications
 *
 * @param data (not used)
 */
static void mia_datapipe_alarm_ui_state_cb(gconstpointer data)
{
    alarm_ui_state_t prev = alarm_ui_state;
    alarm_ui_state = GPOINTER_TO_INT(data);

    if( alarm_ui_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "alarm_ui_state: %s -> %s",
            alarm_state_repr(prev),
            alarm_state_repr(alarm_ui_state));

EXIT:
    return;
}

/** Handle call_state_pipe notifications
 *
 * @param data (not used)
 */
static void mia_datapipe_call_state_cb(gconstpointer data)
{
    call_state_t prev = call_state;
    call_state = GPOINTER_TO_INT(data);

    if( call_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "call_state = %s", call_state_repr(call_state));

EXIT:
    return;
}

/**
 * Handle system_state_pipe notifications
 *
 * @param data The system state stored in a pointer
 */
static void mia_datapipe_system_state_cb(gconstpointer data)
{
    system_state_t prev = system_state;
    system_state = GPOINTER_TO_INT(data);

    if( system_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "system_state: %s -> %s",
            system_state_repr(prev),
            system_state_repr(system_state));

    if( prev == MCE_SYSTEM_STATE_UNDEF )
        mia_datapipe_check_initial_state();

    mia_shutdown_timer_rethink();

EXIT:
    return;
}

/** Handle display_state_next_pipe notifications
 *
 * @param data Current display_state_t (as void pointer)
 */
static void mia_datapipe_display_state_next_cb(gconstpointer data)
{
    display_state_t prev = display_state_next;
    display_state_next = GPOINTER_TO_INT(data);

    if( display_state_next == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_next: %s -> %s",
            display_state_repr(prev),
            display_state_repr(display_state_next));

    if( prev == MCE_DISPLAY_UNDEF )
        mia_datapipe_check_initial_state();

EXIT:
    return;
}

/** Change notifications for interaction_expected_pipe
 */
static void mia_datapipe_interaction_expected_cb(gconstpointer data)
{
    bool prev = interaction_expected;
    interaction_expected = GPOINTER_TO_INT(data);

    if( prev == interaction_expected )
        goto EXIT;

    mce_log(LL_DEBUG, "interaction_expected: %d -> %d",
            prev, interaction_expected);

    /* Generate activity to restart blanking timers if interaction
     * becomes expected while lockscreen is active. */
    if( interaction_expected &&
        (submode & MCE_SUBMODE_TKLOCK) &&
        display_state_next == MCE_DISPLAY_ON ) {
        mce_log(LL_DEBUG, "interaction expected; generate activity");
        mia_generate_activity();
    }

EXIT:
    return;
}

/** Change notifications for charger_state
 */
static void mia_datapipe_charger_state_cb(gconstpointer data)
{
    charger_state_t prev = charger_state;
    charger_state = GPOINTER_TO_INT(data);

    if( charger_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "charger_state = %s -> %s",
            charger_state_repr(prev),
            charger_state_repr(charger_state));

    mia_shutdown_timer_rethink();

EXIT:
    return;
}

/** Change notifications for init_done
 */
static void mia_datapipe_init_done_cb(gconstpointer data)
{
    tristate_t prev = init_done;
    init_done = GPOINTER_TO_INT(data);

    if( init_done == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "init_done = %s -> %s",
            tristate_repr(prev),
            tristate_repr(init_done));

    mia_shutdown_timer_rethink();

EXIT:
    return;
}

/** Change notifications for osupdate_running
 */
static void mia_datapipe_osupdate_running_cb(gconstpointer data)
{
    bool prev = osupdate_running;
    osupdate_running = GPOINTER_TO_INT(data);

    if( osupdate_running == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "osupdate_running = %d -> %d", prev, osupdate_running);

    mia_shutdown_timer_rethink();

EXIT:
    return;
}

/** Handle initial state evaluation and broadcast
 */
static void mia_datapipe_check_initial_state(void)
{
    static bool done = false;

    /* This must be done only once */
    if( done )
        goto EXIT;

    /* Wait until the initial state transitions on
     * mce startup are done and the device state
     * is sufficiently known */

    if( system_state == MCE_SYSTEM_STATE_UNDEF )
        goto EXIT;

    if( display_state_next == MCE_DISPLAY_UNDEF )
        goto EXIT;

    done = true;

    /* Basically the idea is that mce restarts while the
     * display is off should leave the device in inactive
     * state, but booting up / restarting mce while the
     * display is on should yield active state.
     *
     * Once mce startup has progressed so that we known
     * the system state: Attempt to generate activity.
     *
     * The activity filtering rules should take care
     * of suppressing it in the "mce restart while
     * display is off" case.
     */

    mce_log(LL_DEBUG, "device state known");
    mia_generate_activity();

    /* Make sure the current state gets broadcast even
     * if the artificial activity gets suppressed. */

    mce_log(LL_DEBUG, "forced broadcast");
    mia_dbus_send_inactivity_state(0);

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t mia_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &inactivity_event_pipe,
        .output_cb = mia_datapipe_inactivity_event_cb,
    },
    {
        .datapipe  = &device_inactive_pipe,
        .output_cb = mia_datapipe_device_inactive_cb,
    },
    {
        .datapipe  = &proximity_sensor_actual_pipe,
        .output_cb = mia_datapipe_proximity_sensor_actual_cb,
    },
    {
        .datapipe  = &inactivity_delay_pipe,
        .output_cb = mia_datapipe_inactivity_delay_cb,
    },
    {
        .datapipe  = &submode_pipe,
        .output_cb = mia_datapipe_submode_cb,
    },
    {
        .datapipe  = &alarm_ui_state_pipe,
        .output_cb = mia_datapipe_alarm_ui_state_cb,
    },
    {
        .datapipe  = &call_state_pipe,
        .output_cb = mia_datapipe_call_state_cb,
    },
    {
        .datapipe  = &system_state_pipe,
        .output_cb = mia_datapipe_system_state_cb,
    },
    {
        .datapipe  = &display_state_next_pipe,
        .output_cb = mia_datapipe_display_state_next_cb,
    },
    {
        .datapipe  = &interaction_expected_pipe,
        .output_cb = mia_datapipe_interaction_expected_cb,
    },
    {
        .datapipe  = &charger_state_pipe,
        .output_cb = mia_datapipe_charger_state_cb,
    },
    {
        .datapipe  = &init_done_pipe,
        .output_cb = mia_datapipe_init_done_cb,
    },
    {
        .datapipe  = &osupdate_running_pipe,
        .output_cb = mia_datapipe_osupdate_running_cb,
    },
    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t mia_datapipe_bindings =
{
    .module   = "inactivity",
    .handlers = mia_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void mia_datapipe_init(void)
{
    datapipe_bindings_init(&mia_datapipe_bindings);
}

/** Remove triggers/filters from datapipes */
static void mia_datapipe_quit(void)
{
    datapipe_bindings_quit(&mia_datapipe_bindings);
}

/* ========================================================================= *
 * SUSPEND_BLOCK
 * ========================================================================= */

#ifdef ENABLE_WAKELOCKS
/** Timer ID for ending suspend blocking */
static guint mia_keepalive_id = 0;

/** Evaluate need for suspend blocking
 */
static void mia_keepalive_rethink(void)
{
    static bool have_lock = false;

    bool need_lock = (mia_keepalive_id != 0);

    if( have_lock == need_lock )
        goto EXIT;

    mce_log(LL_DEBUG, "inactivity notify wakelock: %s",
            need_lock ? "OBTAIN" : "RELEASE");

    if( (have_lock = need_lock) )
        wakelock_lock("mce_inactivity_notify", -1);
    else
        wakelock_unlock("mce_inactivity_notify");

EXIT:
    return;
}

/** Timer callback for ending suspend blocking
 */
static gboolean mia_keepalive_cb(gpointer aptr)
{
    (void)aptr;

    mia_keepalive_id = 0;
    mia_keepalive_rethink();

    return FALSE;
}

/** Start/restart temporary suspend blocking
 */
static void mia_keepalive_start(void)
{
    if( mia_keepalive_id ) {
        g_source_remove(mia_keepalive_id),
            mia_keepalive_id = 0;
    }

    mia_keepalive_id = g_timeout_add(MIA_KEEPALIVE_DURATION_MS,
                                     mia_keepalive_cb, 0);
    mia_keepalive_rethink();
}

/** Cancel suspend blocking
 */
static void mia_keepalive_stop(void)
{
    if( mia_keepalive_id ) {
        g_source_remove(mia_keepalive_id),
            mia_keepalive_id = 0;
    }
    mia_keepalive_rethink();
}
#endif /* ENABLE_WAKELOCKS */

/* ========================================================================= *
 * DBUS_HANDLERS
 * ========================================================================= */

/** D-Bus name owner changed handler for canceling activity actions
 *
 * If a process that has added activity actions drops out from the system
 * bus, the actions must be canceled.
 *
 * @param sig NameOwnerChanged D-Bus signal
 *
 * @return TRUE
 */
static gboolean mia_dbus_activity_action_owner_cb(DBusMessage *const sig)
{
    DBusError   err  = DBUS_ERROR_INIT;
    const char *name = 0;
    const char *prev = 0;
    const char *curr = 0;

    if( !dbus_message_get_args(sig, &err,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_STRING, &prev,
                               DBUS_TYPE_STRING, &curr,
                               DBUS_TYPE_INVALID)) {
        mce_log(LL_ERR, "Failed to get arguments: %s: %s",
                err.name, err.message);
        goto EXIT;
    }

    if( !*curr )
        mia_activity_action_remove(name);

EXIT:
    dbus_error_free(&err);

    return TRUE;
}

/** D-Bus callback for the add activity callback method call
 *
 * @param req The D-Bus message
 *
 * @return TRUE
 */
static gboolean mia_dbus_add_activity_action_cb(DBusMessage *const req)
{
    const char *sender    = dbus_message_get_sender(req);
    DBusError   err       = DBUS_ERROR_INIT;
    const char *service   = 0;
    const char *path      = 0;
    const char *interface = 0;
    const char *method    = 0;
    dbus_bool_t res       = false;

    if( !sender )
        goto EXIT;

    mce_log(LL_DEVEL, "Add activity callback request from %s",
            mce_dbus_get_name_owner_ident(sender));

    if( !dbus_message_get_args(req, &err,
                               DBUS_TYPE_STRING, &service,
                               DBUS_TYPE_STRING, &path,
                               DBUS_TYPE_STRING, &interface,
                               DBUS_TYPE_STRING, &method,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to get arguments: %s: %s",
                err.name, err.message);
        goto EXIT;
    }

    res = mia_activity_action_add(sender, service, path, interface, method);

EXIT:
    if( !dbus_message_get_no_reply(req) ) {
        DBusMessage *rsp = dbus_new_method_reply(req);

        if( !dbus_message_append_args(rsp,
                                      DBUS_TYPE_BOOLEAN, &res,
                                      DBUS_TYPE_INVALID) ) {
            mce_log(LL_ERR, "Failed to append reply argument");
            dbus_message_unref(rsp);
        }
        else {
            dbus_send_message(rsp);
        }
    }

    dbus_error_free(&err);

    return TRUE;
}

/** D-Bus callback for the remove activity callback method call
 *
 * @param req The D-Bus message
 *
 * @return TRUE
 */
static gboolean mia_dbus_remove_activity_action_cb(DBusMessage *const req)
{
    const char *sender = dbus_message_get_sender(req);

    if( !sender )
        goto EXIT;

    mce_log(LL_DEVEL, "Remove activity callback request from %s",
            mce_dbus_get_name_owner_ident(sender));

    mia_activity_action_remove(sender);

EXIT:

    if( !dbus_message_get_no_reply(req) ) {
        DBusMessage *reply = dbus_new_method_reply(req);
        dbus_send_message(reply);
    }

    return TRUE;
}

/** Send an inactivity status reply or signal
 *
 * @param method_call A DBusMessage to reply to;
 *                    pass NULL to send an inactivity status signal instead
 * @return TRUE
 */
static gboolean mia_dbus_send_inactivity_state(DBusMessage *const method_call)
{
    /* Make sure initial state is broadcast; -1 does not match TRUE/FALSE */
    static int last_sent = -1;

    DBusMessage *msg = NULL;

    if( method_call ) {
        /* Send reply to state query */
        msg = dbus_new_method_reply(method_call);
    }
    else if( last_sent == device_inactive ) {
        /* Do not repeat broadcasts */
        goto EXIT;
    }
    else {
        /* Broadcast state change */

#ifdef ENABLE_WAKELOCKS
        /* Block suspend for a while to give other processes
         * a chance to get and process the signal. */
        mia_keepalive_start();
#endif

        msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_INACTIVITY_SIG);
    }

    mce_log(method_call ? LL_DEBUG : LL_DEVEL,
            "Sending inactivity %s: %s",
            method_call ? "reply" : "signal",
            mia_inactivity_repr(device_inactive));

    /* Append the inactivity status */
    if( !dbus_message_append_args(msg,
                                  DBUS_TYPE_BOOLEAN, &device_inactive,
                                  DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to append argument to D-Bus message");
        goto EXIT;
    }

    /* Send the message */
    dbus_send_message(msg), msg = 0;

    if( !method_call )
        last_sent = device_inactive;

EXIT:
    if( msg )
        dbus_message_unref(msg);

    return TRUE;
}

/** D-Bus callback for the get inactivity status method call
 *
 * @param req The D-Bus message
 *
 * @return TRUE
 */
static gboolean mia_dbus_get_inactivity_state(DBusMessage *const req)
{
        mce_log(LL_DEVEL, "Received inactivity status get request from %s",
               mce_dbus_get_message_sender_ident(req));

        /* Try to send a reply that contains the current inactivity status */
        mia_dbus_send_inactivity_state(req);

        return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t mia_dbus_handlers[] =
{
    /* signals - outbound (for Introspect purposes only) */
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_INACTIVITY_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"device_inactive\" type=\"b\"/>\n"
    },
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_INACTIVITY_STATUS_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mia_dbus_get_inactivity_state,
        .args      =
            "    <arg direction=\"out\" name=\"device_inactive\" type=\"b\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_ADD_ACTIVITY_CALLBACK_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mia_dbus_add_activity_action_cb,
        .args      =
            "    <arg direction=\"in\" name=\"service_name\" type=\"s\"/>\n"
            "    <arg direction=\"in\" name=\"object_path\" type=\"s\"/>\n"
            "    <arg direction=\"in\" name=\"interface_name\" type=\"s\"/>\n"
            "    <arg direction=\"in\" name=\"method_name\" type=\"s\"/>\n"
            "    <arg direction=\"out\" name=\"added\" type=\"b\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_REMOVE_ACTIVITY_CALLBACK_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mia_dbus_remove_activity_action_cb,
        .args      =
            ""
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Add dbus handlers
 */
static void mia_dbus_init(void)
{
    mce_dbus_handler_register_array(mia_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mia_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(mia_dbus_handlers);
}

/* ========================================================================= *
 * ACTIVITY_ACTIONS
 * ========================================================================= */

/**
 * Remove an activity cb from the list of monitored processes
 * and the callback itself
 *
 * @param owner The D-Bus owner of the callback
 */
static void mia_activity_action_remove(const char *owner)
{
    /* Remove D-Bus name owner monitor */
    mce_dbus_owner_monitor_remove(owner,
                                  &activity_action_owners);

    /* Remove the activity callback itself */
    for( GSList *now = activity_action_list; now; now = now->next ) {

        mia_action_t *cb = now->data;

        if( strcmp(cb->owner, owner) )
            continue;

        activity_action_list = g_slist_remove(activity_action_list, cb);
        mia_action_delete(cb);
        break;
    };
}

static bool mia_activity_action_add(const char *owner,
                                    const char *service,
                                    const char *path,
                                    const char *interface,
                                    const char *method)
{
    bool ack = false;

    /* Add D-Bus name owner monitor */
    if( mce_dbus_owner_monitor_add(owner,
                                   mia_dbus_activity_action_owner_cb,
                                   &activity_action_owners,
                                   ACTIVITY_CB_MAX_MONITORED) == -1) {
        mce_log(LL_ERR, "Failed to add name owner monitoring for `%s'",
                owner);
        goto EXIT;
    }

    /* Add activity callback */
    mia_action_t *cb = mia_action_create(owner, service, path, interface, method);

    activity_action_list = g_slist_prepend(activity_action_list, cb);

    ack = true;

EXIT:
    return ack;
}

/** Unregister all activity callbacks
 */
static void mia_activity_action_remove_all(void)
{
    for( GSList *now = activity_action_list; now; now = now->next ) {
        mia_action_t *act = now->data;

        now->data = 0;
        mia_action_delete(act);
    }

    /* Flush action list */
    g_slist_free(activity_action_list),
        activity_action_list = 0;

    /* Remove associated name owner monitors */
    mce_dbus_owner_monitor_remove_all(&activity_action_owners);
}

/** Call all activity callbacks, then unregister them
 */
static void mia_activity_action_execute_all(void)
{
    /* Execute D-Bus actions */
    for( GSList *now = activity_action_list; now; now = now->next ) {
        mia_action_t *act = now->data;

        dbus_send(act->service, act->path, act->interface, act->method_name,
                  0, DBUS_TYPE_INVALID);
    }

    /* Then unregister them */
    mia_activity_action_remove_all();
}

/* ========================================================================= *
 * INACTIVITY_TIMER
 * ========================================================================= */

/** Timer callback to trigger inactivity
 *
 * @param data (not used)
 *
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean mia_timer_cb(gpointer data)
{
    (void)data;

    mce_log(LL_DEBUG, "inactivity timeout triggered");

    mia_generate_inactivity();

    return FALSE;
}

/** Setup inactivity timeout
 */
static void mia_timer_start(void)
{
    mia_timer_stop();

    if( device_inactive )
        goto EXIT;

    mce_log(LL_DEBUG, "inactivity timeout in %d seconds",
            device_inactive_delay);
    mce_hbtimer_set_period(inactivity_timer_hnd, device_inactive_delay * 1000);
    mce_hbtimer_start(inactivity_timer_hnd);

EXIT:
    return;
}

/** Cancel inactivity timeout
 */
static void mia_timer_stop(void)
{
    if( mce_hbtimer_is_active(inactivity_timer_hnd) ) {
        mce_log(LL_DEBUG, "inactivity timeout canceled");
        mce_hbtimer_stop(inactivity_timer_hnd);
    }
}

/** Initialize inactivity heartbeat timer
 */
static void
mia_timer_init(void)
{
    inactivity_timer_hnd = mce_hbtimer_create("inactivity-timer",
                                               device_inactive_delay * 1000,
                                               mia_timer_cb, 0);
}

/** Cleanup inactivity heartbeat timer
 */
static void
mia_timer_quit(void)
{
    mce_hbtimer_delete(inactivity_timer_hnd),
        inactivity_timer_hnd = 0;
}

/* ========================================================================= *
 * SHUTDOWN_TIMER
 * ========================================================================= */

/** Timer callback for starting inactivity shutdown
 *
 * @param aptr  (unused) User data pointer
 *
 * @return TRUE to restart timer, or FALSE to stop it
 */
static gboolean
mia_shutdown_timer_cb(gpointer aptr)
{
    (void)aptr;

    mce_log(LL_WARN, "shutdown timer triggered");

    shutdown_timer_triggered = true;
    mce_dsme_request_normal_shutdown();

    return FALSE;
}

/** Cancel inactivity shutdown timeout
 */
static void
mia_shutdown_timer_stop(void)
{
    if( !mce_hbtimer_is_active(shutdown_timer_hnd) )
        goto EXIT;

    mce_log(LL_DEBUG, "shutdown timer stopped");
    mce_hbtimer_stop(shutdown_timer_hnd);

EXIT:
    shutdown_timer_triggered = false;
    return;
}

/** Schedule inactivity shutdown timeout
 */
static void
mia_shutdown_timer_start(void)
{
    if( mce_hbtimer_is_active(shutdown_timer_hnd) )
        goto EXIT;

    if( shutdown_timer_triggered ) {
        mce_log(LL_DEBUG, "shutdown timer already triggered");
        goto EXIT;
    }

    if( mia_shutdown_delay < MCE_MINIMUM_INACTIVITY_SHUTDOWN_DELAY ) {
        mce_log(LL_DEBUG, "shutdown timer is disabled in config");
        goto EXIT;
    }

    mce_log(LL_DEBUG, "shutdown timer started (trigger in %d seconds)",
            mia_shutdown_delay);
    mce_hbtimer_set_period(shutdown_timer_hnd,
                           mia_shutdown_delay * 1000);
    mce_hbtimer_start(shutdown_timer_hnd);

EXIT:
    return;
}

/** Evaluate if conditions for inactivity shutdown has been met
 *
 * @return true if delayed shutdown should be started, false otherwise
 */
static bool
mia_shutdown_timer_wanted(void)
{
    bool want_timer = false;

    if( !device_inactive )
        goto EXIT;

    if( charger_state != CHARGER_STATE_OFF )
        goto EXIT;

    if( osupdate_running )
        goto EXIT;

    if( init_done != TRISTATE_TRUE )
        goto EXIT;

    if( system_state != MCE_SYSTEM_STATE_USER )
        goto EXIT;

    want_timer = true;

EXIT:
    return want_timer;
}

/** Schedule/cancel inactivity shutdown based on device state
 */
static void
mia_shutdown_timer_rethink(void)
{
    if( mia_shutdown_timer_wanted() )
        mia_shutdown_timer_start();
    else
        mia_shutdown_timer_stop();
}

/** Reschedule inactivity shutdown after settings changes
 */
static void
mia_shutdown_timer_restart(void)
{
    if( !shutdown_timer_triggered ) {
        mia_shutdown_timer_stop();
        mia_shutdown_timer_rethink();
    }
}

/** Initialize inactivity shutdown triggering
 */
static void
mia_shutdown_timer_init(void)
{
    shutdown_timer_hnd =
        mce_hbtimer_create("idle_shutdown",
                           mia_shutdown_delay * 1000,
                           mia_shutdown_timer_cb,
                           0);
}

/** Cleanup inactivity shutdown triggering
 */
static void
mia_shutdown_timer_quit(void)
{
    mce_hbtimer_delete(shutdown_timer_hnd),
        shutdown_timer_hnd = 0;
}

/* ========================================================================= *
 * SETTING_TRACKING
 * ========================================================================= */

/** Handle setting value changed notifications
 *
 * @param gcc   (unused) gconf client object
 * @param id    ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param aptr  (unused) User data pointer
 */
static void
mia_setting_changed_cb(GConfClient *gcc, guint id,
                       GConfEntry *entry, gpointer aptr)
{
    (void)gcc;
    (void)aptr;

    const GConfValue *gcv = gconf_entry_get_value(entry);

    if( !gcv ) {
        mce_log(LL_DEBUG, "GConf Key `%s' has been unset",
                gconf_entry_get_key(entry));
        goto EXIT;
    }

    if( id == mia_shutdown_delay_setting_id ) {
        gint prev = mia_shutdown_delay;
        mia_shutdown_delay = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "mia_shutdown_delay: %d -> %d",
                prev, mia_shutdown_delay);
        mia_shutdown_timer_restart();
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }

EXIT:
    return;
}

/** Get intial setting values and start tracking changes
 */
static void
mia_setting_init(void)
{
    mce_setting_track_int(MCE_SETTING_INACTIVITY_SHUTDOWN_DELAY,
                          &mia_shutdown_delay,
                          MCE_DEFAULT_INACTIVITY_SHUTDOWN_DELAY,
                          mia_setting_changed_cb,
                          &mia_shutdown_delay_setting_id);
}

/** Stop tracking setting changes
 */
static void
mia_setting_quit(void)
{
    mce_setting_notifier_remove(mia_shutdown_delay_setting_id),
        mia_shutdown_delay_setting_id = 0;
}

/* ========================================================================= *
 * MODULE_LOAD_UNLOAD
 * ========================================================================= */

/** Init function for the inactivity module
 *
 * @param module (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    mia_setting_init();

    mia_timer_init();
    mia_shutdown_timer_init();

    /* Append triggers/filters to datapipes */
    mia_datapipe_init();

    /* Add dbus handlers */
    mia_dbus_init();

    /* Start timers */
    mia_timer_start();

    /* The initial inactivity state gets broadcast once the system
     * and display states are known */

    return NULL;
}

/** Exit function for the inactivity module
 *
 * @todo D-Bus unregistration
 *
 * @param module (not used)
 */
void g_module_unload(GModule *module)
{
    (void)module;

    mia_setting_quit();

    /* Remove dbus handlers */
    mia_dbus_quit();

    /* Remove triggers/filters from datapipes */
    mia_datapipe_quit();

    /* Do not leave any timers active */
    mia_timer_quit();
    mia_shutdown_timer_quit();

#ifdef ENABLE_WAKELOCKS
    mia_keepalive_stop();
#endif

    /* Flush activity actions */
    mia_activity_action_remove_all();
    return;
}
