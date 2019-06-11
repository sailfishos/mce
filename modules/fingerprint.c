/**
 * @file fingerprint.c
 *
 * Fingerprint daemon tracking module for the Mode Control Entity
 * <p>
 * Copyright (c) 2015-2019 Jolla Ltd.
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
#include "../mce-lib.h"
#include "../mce-log.h"
#include "../mce-dbus.h"
#include "../mce-setting.h"
#include "../evdev.h"

#include <linux/input.h>

#include <gmodule.h>

/* ========================================================================= *
 * Types
 * ========================================================================= */

/** Return values for requests made to fingerprint daemon
 *
 * Keep fpreply_repr() in sync with any changes made here.
 */
typedef enum fpreply_t
{
    /** Operation successfully started */
    FPREPLY_STARTED                  = 0,

    /** Unspecified (low level) failure */
    FPREPLY_FAILED                   = 1,

    /** Abort() while already idle */
    FPREPLY_ALREADY_IDLE             = 2,

    /** Abort/Enroll/Identify() while busy */
    FPREPLY_ALREADY_BUSY             = 3,

    /** Not allowed  */
    FPREPLY_DENIED                   = 4,

    /** Enroll() key that already exists */
    FPREPLY_KEY_ALREADY_EXISTS       = 5,

    /** Remove() key that does not exist */
    FPREPLY_KEY_DOES_NOT_EXIST       = 6,

    /** Identify() without having any keys */
    FPREPLY_NO_KEYS_AVAILABLE        = 7,

    /** Null or otherwise illegal key name */
    FPREPLY_KEY_IS_INVALID           = 8,

} fpreply_t;

/** Resulting events from accepted fingerprint daemon requests
 *
 * Keep fpresult_repr() in sync with any changes made here.
 */
typedef enum fpresult_t
{
    FPRESULT_ABORTED,
    FPRESULT_FAILED,
    FPRESULT_IDENTIFIED,
    FPRESULT_VERIFIED,
} fpresult_t;

/** Fingerprint daemon ipc operation state
 *
 * Keep fpopstate_repr() in sync with any changes made here.
 */
typedef enum fpopstate_t
{
    /** Initial state */
    FPOPSTATE_INITIALIZE,

    /** Wait until operation is required and fpd is idle */
    FPOPSTATE_WAITING,

    /** Send asynchronous dbus method call and wait for reply */
    FPOPSTATE_REQUEST,

    /** Wait for operation results / errors / cancellation */
    FPOPSTATE_PENDING,

    /* Operation was successfully finished */
    FPOPSTATE_SUCCESS,

    /* Operation failed */
    FPOPSTATE_FAILURE,

    /* Send asynchronous abort dbus method call and wait for reply */
    FPOPSTATE_ABORT,

    /** Wait for fpd to make transition to idle state */
    FPOPSTATE_ABORTING,

    /** Opearation was aborted */
    FPOPSTATE_ABORTED,

    /** Delay in between operation retry attempts */
    FPOPSTATE_THROTTLING,

    /** Number of possible states */
    FPOPSTATE_NUMOF
} fpopstate_t;

/** State machine for performing ipc operations with fingerprint daemon
 *
 * The happy path for making request to fingerprint daemon over dbus is:
 *
 * 1. Wait for daemon to be idle
 * 2. Request start of operation
 * 3. Wait for operation started acknowledgement
 * 4. Wait for operation result
 *
 * To facilitate overlapping use by multiple clients, all clients must
 * expect requests to be denied (while busy with requests from other
 * clients), daemon dropping out of system bus and coming back up, and
 * illogical seeming state transitions and be prepared to retry until
 * succeeding.
 *
 * The fpoperation_t structure contains bookkeeping data for generic
 * state machine that can be used to perform any fingerprint daemon
 * request by providing suitable hooks.
 */
typedef struct fpoperation_t fpoperation_t;

struct fpoperation_t
{
    /** State machine name */
    const char      *fpo_name;

    /** Current state */
    fpopstate_t      fpo_state;

    /** Expected/tracked fpstate
     *
     * Used for detecting situations where we're obviously out of
     * sync with what is going on at the fingerprint daemon side.
     */
    fpstate_t        fpo_fpstate;

    /** Pending async D-Bus method call
     *
     * This is either the operation this state machine is expected
     * to perform, or abort used for canceling successfully started
     * request.
     */
    DBusPendingCall *fpo_pending;

    /** Pending timeout
     *
     * Used for throttling consecutive operations, so that we allow
     * time for system state changes affecting the use of this state
     * machine to occur.
     */
    guint            fpo_timer;

    /** Hook for entering a state */
    void (*fpo_enter_cb)(fpoperation_t *self);

    /** Hook for leaving a state */
    void (*fpo_leave_cb)(fpoperation_t *self);

    /** Hook for evaluating staying in a state */
    void (*fpo_eval_cb) (fpoperation_t *self);

    /** Hook for handling operation result events */
    void (*fpo_result_cb)(fpoperation_t *self, fpresult_t event);
};

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * FPREPLY
 * ------------------------------------------------------------------------- */

static const char  *fpreply_repr(fpreply_t val);

/* ------------------------------------------------------------------------- *
 * FPRESULT
 * ------------------------------------------------------------------------- */

static const char  *fpresult_repr(fpresult_t event);

/* ------------------------------------------------------------------------- *
 * FPOPSTATE
 * ------------------------------------------------------------------------- */

static const char  *fpopstate_repr(fpopstate_t state);

/* ------------------------------------------------------------------------- *
 * FPOPERATION
 * ------------------------------------------------------------------------- */

static const char   *fpoperation_name               (const fpoperation_t *self);
static fpopstate_t   fpoperation_state              (const fpoperation_t *self);
static void          fpoperation_enter              (fpoperation_t *self);
static void          fpoperation_leave              (fpoperation_t *self);
static bool          fpoperation_eval_overrides     (fpoperation_t *self);
static void          fpoperation_eval               (fpoperation_t *self);
static void          fpoperation_result             (fpoperation_t *self, fpresult_t event);
static void          fpoperation_trans              (fpoperation_t *self, fpopstate_t state);
static fpstate_t     fpoperation_get_fpstate        (const fpoperation_t *self);
static void          fpoperation_set_fpstate        (fpoperation_t *self, fpstate_t state);
static void          fpoperation_cancel_timout      (fpoperation_t *self);
static bool          fpoperation_detach_timout      (fpoperation_t *self);
static void          fpoperation_attach_timeout     (fpoperation_t *self, int delay, GSourceFunc cb);
static gboolean      fpoperation_trigger_fpwakeup_cb(gpointer aptr);
static gboolean      fpoperation_throttling_ended_cb(gpointer aptr);
static void          fpoperation_cancel_pending_call(fpoperation_t *self);
static bool          fpoperation_detach_pending_call(fpoperation_t *self, DBusPendingCall *pc);
static void          fpoperation_attach_pending_call(fpoperation_t *self, DBusPendingCall *pc);
static void          fpoperation_identify_reply_cb  (DBusPendingCall *pc, void *aptr);
static void          fpoperation_start_identify     (fpoperation_t *self);
static void          fpoperation_abort_reply_cb     (DBusPendingCall *pc, void *aptr);
static void          fpoperation_start_abort        (fpoperation_t *self);

/* ------------------------------------------------------------------------- *
 * FPIDENTIFY
 * ------------------------------------------------------------------------- */

static void  fpidentify_enter_cb (fpoperation_t *self);
static void  fpidentify_leave_cb (fpoperation_t *self);
static void  fpidentify_eval_cb  (fpoperation_t *self);
static void  fpidentify_result_cb(fpoperation_t *self, fpresult_t event);

/* ------------------------------------------------------------------------- *
 * FINGERPRINT_DATA
 * ------------------------------------------------------------------------- */

static gpointer  fingerprint_data_create   (const char *name);
static void      fingerprint_data_detete_cb(gpointer aptr);
static void      fingerprint_data_flush    (void);
static void      fingerprint_data_remove   (const char *name);
static void      fingerprint_data_add      (const char *name);
static bool      fingerprint_data_exists   (void);
static void      fingerprint_data_init     (void);
static void      fingerprint_data_quit     (void);

/* ------------------------------------------------------------------------- *
 * FINGERPRINT_LED_SCANNING
 * ------------------------------------------------------------------------- */

static void  fingerprint_led_scanning_activate(bool activate);

/* ------------------------------------------------------------------------- *
 * FINGERPRINT_LED_ACQUIRED
 * ------------------------------------------------------------------------- */

static void      fingerprint_led_acquired_activate(bool activate);
static gboolean  fingerprint_led_acquired_timer_cb(gpointer aptr);
static void      fingerprint_led_acquired_trigger (void);
static void      fingerprint_led_acquired_cancel  (void);

/* ------------------------------------------------------------------------- *
 * FINGERPRINT_DATAPIPE
 * ------------------------------------------------------------------------- */

static void  fingerprint_datapipe_set_fpstate                (fpstate_t state);
static bool  fingerprint_datapipe_evaluate_enroll_in_progress(void);
static void  fingerprint_datapipe_update_enroll_in_progress  (void);
static void  fingerprint_datapipe_generate_activity          (void);
static void  fingerprint_datapipe_fpd_service_state_cb       (gconstpointer data);
static void  fingerprint_datapipe_system_state_cb            (gconstpointer data);
static void  fingerprint_datapipe_devicelock_state_cb        (gconstpointer data);
static void  fingerprint_datapipe_submode_cb                 (gconstpointer data);
static void  fingerprint_datapipe_display_state_next_cb      (gconstpointer data);
static void  fingerprint_datapipe_interaction_expected_cb    (gconstpointer data);
static void  fingerprint_datapipe_topmost_window_pid_cb      (gconstpointer data);
static void  fingerprint_datapipe_proximity_sensor_actual_cb (gconstpointer data);
static void  fingerprint_datapipe_lid_sensor_filtered_cb     (gconstpointer data);
static void  fingerprint_datapipe_keypress_event_cb          (gconstpointer const data);
static void  fingerprint_datapipe_init                       (void);
static void  fingerprint_datapipe_quit                       (void);

/* ------------------------------------------------------------------------- *
 * FINGERPRINT_SETTING
 * ------------------------------------------------------------------------- */

static void  fingerprint_setting_cb  (GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);
static void  fingerprint_setting_init(void);
static void  fingerprint_setting_quit(void);

/* ------------------------------------------------------------------------- *
 * FINGERPRINT_DBUS
 * ------------------------------------------------------------------------- */

static gboolean  fingerprint_dbus_fpstate_changed_cb  (DBusMessage *const msg);
static gboolean  fingerprint_dbus_fpacquired_info_cb  (DBusMessage *const msg);
static gboolean  fingerprint_dbus_fpadded_cb          (DBusMessage *const msg);
static gboolean  fingerprint_dbus_fpremoved_cb        (DBusMessage *const msg);
static gboolean  fingerprint_dbus_fpidentified_cb     (DBusMessage *const msg);
static gboolean  fingerprint_dbus_fpaborted_cb        (DBusMessage *const msg);
static gboolean  fingerprint_dbus_fpfailed_cb         (DBusMessage *const msg);
static gboolean  fingerprint_dbus_fpverified_cb       (DBusMessage *const msg);
static gboolean  fingerprint_dbus_fperror_cb          (DBusMessage *const msg);
static gboolean  fingerprint_dbus_fpprogress_cb       (DBusMessage *const msg);
static void      fingerprint_dbus_init                (void);
static void      fingerprint_dbus_quit                (void);
static void      fingerprint_dbus_fpstate_query_cb    (DBusPendingCall *pc, void *aptr);
static void      fingerprint_dbus_fpstate_query_cancel(void);
static void      fingerprint_dbus_fpstate_query_start (void);
static void      fingerprint_dbus_fpdata_query_cb     (DBusPendingCall *pc, void *aptr);
static void      fingerprint_dbus_fpdata_query_cancel (void);
static void      fingerprint_dbus_fpdata_query_start  (void);

/* ------------------------------------------------------------------------- *
 * FPWAKEUP
 * ------------------------------------------------------------------------- */

static bool      fpwakeup_is_allowed        (void);
static void      fpwakeup_set_allowed       (bool allowed);
static gboolean  fpwakeup_allow_cb          (gpointer aptr);
static void      fpwakeup_cancel_allow      (void);
static void      fpwakeup_schedule_allow    (void);
static bool      fpwakeup_evaluate_allowed  (void);
static void      fpwakeup_update_allowed    (void);
static void      fpwakeup_rethink_now       (void);
static gboolean  fpwakeup_rethink_cb        (gpointer aptr);
static void      fpwakeup_schedule_rethink  (void);
static void      fpwakeup_cancel_rethink    (void);
static void      fpwakeup_propagate_fpstate (void);
static void      fpwakeup_propagate_fpresult(fpresult_t event);
static void      fpwakeup_propagate_eval    (void);
static bool      fpwakeup_set_primed        (bool prime);
static void      fpwakeup_trigger           (void);

/* ------------------------------------------------------------------------- *
 * G_MODULE
 * ------------------------------------------------------------------------- */

const gchar  *g_module_check_init(GModule *module);
void          g_module_unload    (GModule *module);

/* ========================================================================= *
 * FINGERPRINT_DATAPIPE
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

/** Interaction expected; assume false */
static bool interaction_expected = false;

/** Cached PID of process owning the topmost window on UI */
static int topmost_window_pid = -1;

/** Cached proximity sensor state */
static cover_state_t proximity_sensor_actual = COVER_UNDEF;

/** Lid cover policy state; assume unknown */
static cover_state_t lid_sensor_filtered = COVER_UNDEF;

/** Cached power key pressed down state */
static bool powerkey_pressed = false;

/* ========================================================================= *
 * FINGERPRINT_SETTINGS
 * ========================================================================= */

/** Fingerprint wakeup enable mode */
static gint  fingerprint_wakeup_mode = MCE_DEFAULT_FPWAKEUP_MODE;
static guint fingerprint_wakeup_mode_setting_id = 0;

static gint  fingerprint_allow_delay = MCE_DEFAULT_FPWAKEUP_ALLOW_DELAY;
static guint fingerprint_allow_delay_setting_id = 0;

static gint  fingerprint_trigger_delay = MCE_DEFAULT_FPWAKEUP_TRIGGER_DELAY;
static guint fingerprint_trigger_delay_setting_id = 0;

static gint  fingerprint_throttle_delay = MCE_DEFAULT_FPWAKEUP_THROTTLE_DELAY;
static guint fingerprint_throttle_delay_setting_id = 0;

/* ========================================================================= *
 * MANAGED_STATES
 * ========================================================================= */

/** Tracked fpd operational state; assume unknown */
static fpstate_t fpstate = FPSTATE_UNSET;

/** Tracked fingerprint enroll status; assume not in progress */
static bool enroll_in_progress = false;

/** State machine data for handling fpd requests */
static fpoperation_t fpoperation_lut[] =
{
    {
        .fpo_name      = "identify_stm",
        .fpo_state     = FPOPSTATE_INITIALIZE,
        .fpo_fpstate   = FPSTATE_UNSET,
        .fpo_pending   = 0,
        .fpo_timer     = 0,
        .fpo_enter_cb  = fpidentify_enter_cb,
        .fpo_leave_cb  = fpidentify_leave_cb,
        .fpo_eval_cb   = fpidentify_eval_cb,
        .fpo_result_cb = fpidentify_result_cb,
    },
};

/* ========================================================================= *
 * FPREPLY
 * ========================================================================= */

static const char *
fpreply_repr(fpreply_t val)
{
    const char *repr = "FPREPLY_UNKNOWN";

#define REPR_VAL(NAME) case NAME: repr = #NAME; break
    switch( val ) {
        REPR_VAL(FPREPLY_STARTED);
        REPR_VAL(FPREPLY_FAILED);
        REPR_VAL(FPREPLY_ALREADY_IDLE);
        REPR_VAL(FPREPLY_ALREADY_BUSY);
        REPR_VAL(FPREPLY_DENIED);
        REPR_VAL(FPREPLY_KEY_ALREADY_EXISTS);
        REPR_VAL(FPREPLY_KEY_DOES_NOT_EXIST);
        REPR_VAL(FPREPLY_NO_KEYS_AVAILABLE);
        REPR_VAL(FPREPLY_KEY_IS_INVALID);
    default: break;
    }
#undef REPR_VAL

    return repr;
}

/* ========================================================================= *
 * FPRESULT
 * ========================================================================= */

static const char *
fpresult_repr(fpresult_t event)
{
    static const char * const fpresult_lut[] =
    {
        [FPRESULT_ABORTED]      = "FPRESULT_ABORTED",
        [FPRESULT_FAILED]       = "FPRESULT_FAILED",
        [FPRESULT_IDENTIFIED]   = "FPRESULT_IDENTIFIED",
        [FPRESULT_VERIFIED]     = "FPRESULT_VERIFIED",
    };
    return fpresult_lut[event];
}

/* ========================================================================= *
 * FPOPSTATE
 * ========================================================================= */

static const char *
fpopstate_repr(fpopstate_t state)
{
    static const char * const fpopstate_lut[FPOPSTATE_NUMOF] = {
        [FPOPSTATE_INITIALIZE]  = "FPOPSTATE_INITIALIZE",
        [FPOPSTATE_WAITING]     = "FPOPSTATE_WAITING",
        [FPOPSTATE_REQUEST]     = "FPOPSTATE_REQUEST",
        [FPOPSTATE_PENDING]     = "FPOPSTATE_PENDING",
        [FPOPSTATE_SUCCESS]     = "FPOPSTATE_SUCCESS",
        [FPOPSTATE_FAILURE]     = "FPOPSTATE_FAILURE",
        [FPOPSTATE_ABORT]       = "FPOPSTATE_ABORT",
        [FPOPSTATE_ABORTING]    = "FPOPSTATE_ABORTING",
        [FPOPSTATE_ABORTED]     = "FPOPSTATE_ABORTED",
        [FPOPSTATE_THROTTLING]  = "FPOPSTATE_THROTTLING",
    };
    return fpopstate_lut[state];
}

/* ========================================================================= *
 * FPOPERATION
 * ========================================================================= */

/** Accessor for operation name
 */
static const char *
fpoperation_name(const fpoperation_t *self)
{
    return self->fpo_name ?: "unnamed";
}

/** Accessor for operation state
 */
static fpopstate_t
fpoperation_state(const fpoperation_t *self)
{
    return self->fpo_state;
}

/** Handle tasks after entering to a state
 */
static void
fpoperation_enter(fpoperation_t *self)
{
    if( self->fpo_enter_cb )
        self->fpo_enter_cb(self);

}

/** Handle tasks after leaving a state
 */
static void
fpoperation_leave(fpoperation_t *self)
{
    if( self->fpo_leave_cb )
        self->fpo_leave_cb(self);
}

/** Handle evaluation of generic rules
 */
static bool
fpoperation_eval_overrides(fpoperation_t *self)
{
    bool overridden = false;

    /* If fingerprint daemon is not on system bus, cancel any
     * ongoing async activity via transition to aborted state.
     */
    if( fpstate == FPSTATE_UNSET ) {
        switch( fpoperation_state(self) ) {
        case FPOPSTATE_INITIALIZE:
        case FPOPSTATE_WAITING:
            /* Nothing initiated -> NOP */
            break;
        case FPOPSTATE_REQUEST:
        case FPOPSTATE_PENDING:
        case FPOPSTATE_SUCCESS:
        case FPOPSTATE_FAILURE:
        case FPOPSTATE_ABORT:
        case FPOPSTATE_ABORTING:
            fpoperation_trans(self, FPOPSTATE_ABORTED);
            overridden = true;
            break;
        default:
        case FPOPSTATE_ABORTED:
        case FPOPSTATE_THROTTLING:
            /* No pending ipc -> NOP */
            break;
        }
    }

    return overridden;
}

/** Evaluate whether current state is still valid
 */
static void
fpoperation_eval(fpoperation_t *self)
{
    if( !fpoperation_eval_overrides(self) ) {
        if( self->fpo_eval_cb )
            self->fpo_eval_cb(self);
    }
}

/** Handle operation result events
 */
static void
fpoperation_result(fpoperation_t *self, fpresult_t event)
{
    mce_log(LL_DEBUG, "%s @ %s: got event %s",
            fpoperation_name(self),
            fpopstate_repr(fpoperation_state(self)),
            fpresult_repr(event));

    if( self->fpo_result_cb )
        self->fpo_result_cb(self, event);
}

/** Handle state transition
 */
static void
fpoperation_trans(fpoperation_t *self, fpopstate_t state)
{
    if( self->fpo_state != state ) {
        mce_log(LL_DEBUG, "%s @ %s: transition to %s",
                fpoperation_name(self),
                fpopstate_repr(self->fpo_state),
                fpopstate_repr(state));

        fpoperation_leave(self);
        self->fpo_state = state;
        fpoperation_enter(self);

        fpwakeup_schedule_rethink();
    }
}

/** Accessor for cached fpd state
 */
static fpstate_t
fpoperation_get_fpstate(const fpoperation_t *self)
{
    return self->fpo_fpstate;
}

/** Set cached fpd state
 */
static void
fpoperation_set_fpstate(fpoperation_t *self, fpstate_t state)
{
    fpstate_t prev = self->fpo_fpstate;
    self->fpo_fpstate = state;

    if( prev != self->fpo_fpstate ) {
        mce_log(LL_DEBUG, "%s @ %s: fpstate: %s -> %s",
                fpoperation_name(self),
                fpopstate_repr(fpoperation_state(self)),
                fpstate_repr(prev),
                fpstate_repr(self->fpo_fpstate));
    }
}

/** Cancel timer
 */
static void
fpoperation_cancel_timout(fpoperation_t *self)
{
    if( self->fpo_timer ) {
        g_source_remove(self->fpo_timer),
            self->fpo_timer = 0;
    }
}

/** Remove timer id from bookkeeping data
 */
static bool
fpoperation_detach_timout(fpoperation_t *self)
{
    bool detached = false;
    if( self->fpo_timer ) {
        self->fpo_timer = 0;
        detached = true;
    }
    return detached;
}

/** Attach timer id to bookkeeping data
 */
static void
fpoperation_attach_timeout(fpoperation_t *self, int delay, GSourceFunc cb)
{
    fpoperation_cancel_timout(self);
    self->fpo_timer = mce_wakelocked_timeout_add(delay, cb, self);
}

/** Timer callback for triggering fpwakeup
 */
static gboolean
fpoperation_trigger_fpwakeup_cb(gpointer aptr)
{
    fpoperation_t *self = aptr;

    if( !fpoperation_detach_timout(self) )
        goto EXIT;

    fpwakeup_trigger();
    fpoperation_trans(self, FPOPSTATE_THROTTLING);

EXIT:
    return G_SOURCE_REMOVE;
}

/** Timer callback for exiting FPOPSTATE_THROTTLING state
 */
static gboolean
fpoperation_throttling_ended_cb(gpointer aptr)
{
    fpoperation_t *self = aptr;

    if( !fpoperation_detach_timout(self) )
        goto EXIT;

    fpoperation_trans(self, FPOPSTATE_WAITING);

EXIT:
    return G_SOURCE_REMOVE;
}

/** Cancel pending async dbus method call
 */
static void
fpoperation_cancel_pending_call(fpoperation_t *self)
{
    if( self->fpo_pending ) {
                dbus_pending_call_cancel(self->fpo_pending);
        dbus_pending_call_unref(self->fpo_pending),
            self->fpo_pending = 0;
    }
}

/** Detach pending async dbus method call from bookkeeping data
 */
static bool
fpoperation_detach_pending_call(fpoperation_t *self, DBusPendingCall *pc)
{
    bool detached = false;

    if( pc != 0 && self->fpo_pending == pc ) {
                //dbus_pending_call_unref(self->fpo_pending),
        self->fpo_pending = 0;
        detached = true;
    }
    return detached;
}

/** Attach pending async dbus method call to bookkeeping data
 */
static void
fpoperation_attach_pending_call(fpoperation_t *self, DBusPendingCall *pc)
{
    fpoperation_cancel_pending_call(self);
    self->fpo_pending = pc;
}

/** Callback for handling reply to FINGERPRINT1_DBUS_REQ_IDENTIFY calls
 */
static void
fpoperation_identify_reply_cb(DBusPendingCall *pc, void *aptr)
{
    fpoperation_t *self = aptr;

    DBusMessage *rsp   = 0;
    DBusError    err   = DBUS_ERROR_INIT;
    dbus_int32_t res   = 0;

    if( !fpoperation_detach_pending_call(self, pc) )
        goto EXIT;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
        mce_log(LL_WARN, "no reply");
        goto EXIT;
    }

    if( dbus_set_error_from_message(&err, rsp) ||
        !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_INT32, &res,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "identify reply: %s", fpreply_repr(res));
    switch( res ) {
    case FPREPLY_STARTED:
        fpoperation_trans(self, FPOPSTATE_PENDING);
        break;
    default:
        fpoperation_trans(self, FPOPSTATE_FAILURE);
        break;
    }

EXIT:
    if( rsp ) dbus_message_unref(rsp);

    dbus_error_free(&err);

    dbus_pending_call_unref(pc);

    return;
}

/** Initiate async FINGERPRINT1_DBUS_REQ_IDENTIFY method call
 */
static void
fpoperation_start_identify(fpoperation_t *self)
{
    DBusPendingCall *pc = 0;

    dbus_send_ex(FINGERPRINT1_DBUS_SERVICE,
                 FINGERPRINT1_DBUS_ROOT_OBJECT,
                 FINGERPRINT1_DBUS_INTERFACE,
                 FINGERPRINT1_DBUS_REQ_IDENTIFY,
                 fpoperation_identify_reply_cb,
                 self, 0,
                 &pc,
                 DBUS_TYPE_INVALID);
    fpoperation_attach_pending_call(self, pc);
}

/** Callback for handling reply to FINGERPRINT1_DBUS_REQ_ABORT calls
 */
static void
fpoperation_abort_reply_cb(DBusPendingCall *pc, void *aptr)
{
    fpoperation_t *self = aptr;

    DBusMessage *rsp   = 0;
    DBusError    err   = DBUS_ERROR_INIT;
    dbus_int32_t res   = 0;

    if( !fpoperation_detach_pending_call(self, pc) )
        goto EXIT;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
        mce_log(LL_WARN, "no reply");
        goto EXIT;
    }

    if( dbus_set_error_from_message(&err, rsp) ||
        !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_INT32, &res,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "abort reply: %s", fpreply_repr(res));
    switch( res ) {
    case FPREPLY_STARTED:
        fpoperation_trans(self, FPOPSTATE_ABORTING);
        break;
    case FPREPLY_ALREADY_IDLE:
        fpoperation_trans(self, FPOPSTATE_ABORTED);
        break;
    default:
        fpoperation_trans(self, FPOPSTATE_FAILURE);
        break;
    }

EXIT:
    if( rsp ) dbus_message_unref(rsp);

    dbus_error_free(&err);

    dbus_pending_call_unref(pc);

    return;
}

/** Initiate async FINGERPRINT1_DBUS_REQ_ABORT method call
 */
static void
fpoperation_start_abort(fpoperation_t *self)
{
    DBusPendingCall *pc = 0;

    dbus_send_ex(FINGERPRINT1_DBUS_SERVICE,
                 FINGERPRINT1_DBUS_ROOT_OBJECT,
                 FINGERPRINT1_DBUS_INTERFACE,
                 FINGERPRINT1_DBUS_REQ_ABORT,
                 fpoperation_abort_reply_cb,
                 self, 0,
                 &pc,
                 DBUS_TYPE_INVALID);
    fpoperation_attach_pending_call(self, pc);
}

/* ========================================================================= *
 * FPIDENTIFY
 * ========================================================================= */

/** Identify operation - Hook for entering a state
 */
static void
fpidentify_enter_cb(fpoperation_t *self)
{
    switch( fpoperation_state(self) ) {
    case FPOPSTATE_INITIALIZE:
        break;
    case FPOPSTATE_WAITING:
        break;
    case FPOPSTATE_REQUEST:
        fpoperation_start_identify(self);
        break;
    case FPOPSTATE_PENDING:
        fpoperation_set_fpstate(self, FPSTATE_IDENTIFYING);
        break;
    case FPOPSTATE_SUCCESS:
        /* We have identified fingerprint. Delay execution of fp wakeup
         * briefly to see if some higher priority event such as power key
         * press happens in close proximity, */
        if( fpwakeup_set_primed(true) )
            mce_log(LL_DEBUG, "fp wakeup primed");
        fpoperation_attach_timeout(self, fingerprint_trigger_delay,
                                   fpoperation_trigger_fpwakeup_cb);
        break;
    case FPOPSTATE_FAILURE:
        break;
    case FPOPSTATE_ABORT:
        fpoperation_start_abort(self);
        break;
    case FPOPSTATE_ABORTING:
        fpoperation_set_fpstate(self, FPSTATE_ABORTING);
        break;
    case FPOPSTATE_ABORTED:
        break;
    case FPOPSTATE_THROTTLING:
        fpoperation_attach_timeout(self, fingerprint_throttle_delay,
                                   fpoperation_throttling_ended_cb);
        break;
    default:
        break;
    }
}

/** Identify operation - Hook for leaving a state
 */
static void
fpidentify_leave_cb(fpoperation_t *self)
{

    switch( fpoperation_state(self) ) {
    case FPOPSTATE_INITIALIZE:
        break;
    case FPOPSTATE_WAITING:
        break;
    case FPOPSTATE_REQUEST:
        fpoperation_cancel_pending_call(self);
        break;
    case FPOPSTATE_PENDING:
        break;
    case FPOPSTATE_SUCCESS:
        break;
    case FPOPSTATE_FAILURE:
        break;
    case FPOPSTATE_ABORT:
        break;
    case FPOPSTATE_ABORTING:
        break;
    case FPOPSTATE_ABORTED:
        break;
    case FPOPSTATE_THROTTLING:
        fpoperation_cancel_timout(self);
        break;
    default:
        break;
    }
}

/** Identify operation - Hook for evaluating a state
 */
static void
fpidentify_eval_cb(fpoperation_t *self)
{
    switch( fpoperation_state(self) ) {
    case FPOPSTATE_INITIALIZE:
        fpoperation_trans(self, FPOPSTATE_WAITING);
        break;
    case FPOPSTATE_WAITING:
        if( !fpwakeup_is_allowed() )
            break;
        if( fpstate != FPSTATE_IDLE )
            break;
        fpoperation_trans(self, FPOPSTATE_REQUEST);
        break;

    case FPOPSTATE_REQUEST:
        break;
    case FPOPSTATE_PENDING:
        if( !fpwakeup_is_allowed() ) {
            fpoperation_trans(self, FPOPSTATE_ABORT);
        }
        else if( fpoperation_get_fpstate(self) != FPSTATE_IDENTIFYING ) {
            fpoperation_trans(self, FPOPSTATE_FAILURE);
        }
        break;
    case FPOPSTATE_ABORT:
        break;
    case FPOPSTATE_ABORTING:
        switch( fpoperation_get_fpstate(self) ) {
        case FPSTATE_ABORTING:
            break;
        case FPSTATE_IDLE:
            fpoperation_trans(self, FPOPSTATE_ABORTED);
            break;
        default:
            fpoperation_trans(self, FPOPSTATE_FAILURE);
            break;
        }
        break;
    case FPOPSTATE_SUCCESS:
        break;
    case FPOPSTATE_FAILURE:
    case FPOPSTATE_ABORTED:
        fpoperation_trans(self, FPOPSTATE_THROTTLING);
        break;
    case FPOPSTATE_THROTTLING:
        break;
    default:
        break;
    }
}

/** Identify operation - Hook for handling result events
 */
static void
fpidentify_result_cb(fpoperation_t *self, fpresult_t event)
{
    switch( fpoperation_state(self) ) {
    case FPOPSTATE_INITIALIZE:
        break;
    case FPOPSTATE_WAITING:
        break;
    case FPOPSTATE_REQUEST:
        break;
    case FPOPSTATE_PENDING:
        switch( event ) {
        case FPRESULT_IDENTIFIED:
            fpoperation_trans(self, FPOPSTATE_SUCCESS);
            break;
        case FPRESULT_FAILED:
            fpoperation_trans(self, FPOPSTATE_FAILURE);
            break;
        case FPRESULT_ABORTED:
            fpoperation_trans(self, FPOPSTATE_ABORTED);
            break;
        default:
            break;
        }
        break;
    case FPOPSTATE_ABORT:
        break;
    case FPOPSTATE_ABORTING:
        switch( event ) {
        case FPRESULT_ABORTED:
            fpoperation_trans(self, FPOPSTATE_ABORTED);
            break;
        default:
            break;
        }
        break;
    case FPOPSTATE_SUCCESS:
    case FPOPSTATE_FAILURE:
    case FPOPSTATE_ABORTED:
    case FPOPSTATE_THROTTLING:
        break;
    default:
        break;
    }
}

/* ========================================================================= *
 * FINGERPRINT_DATA
 * ========================================================================= */

/** Hash table for tracking fingerprint template names known to fpd
 */
static GHashTable *fingerprint_data_lut = 0;

/** Allocate fingerprint template names
 *
 * Gives debug visibility to template names that get added
 */
static gpointer
fingerprint_data_create(const char *name)
{
    mce_log(LL_DEBUG, "fingerprint '%s' added", name);
    return g_strdup(name);
}

/** Callback for releasing fingerprint template names
 *
 * Gives debug visibility to template names that get dropped
 */
static void
fingerprint_data_detete_cb(gpointer aptr)
{
    mce_log(LL_DEBUG, "fingerprint '%s' removed", (char *)aptr);
    g_free(aptr);
}

/** Flush all cached fingerprint template names
 */
static void
fingerprint_data_flush(void)
{
    if( !fingerprint_data_lut )
        goto EXIT;

    if( g_hash_table_size(fingerprint_data_lut) > 0 ) {
        g_hash_table_remove_all(fingerprint_data_lut);
        fpwakeup_schedule_rethink();
    }

EXIT:
    return;
}

/** Remove a fingerprint template name from cache
 */
static void
fingerprint_data_remove(const char *name)
{
    if( !fingerprint_data_lut )
        goto EXIT;

    if( g_hash_table_remove(fingerprint_data_lut, name) )
        fpwakeup_schedule_rethink();

EXIT:
    return;
}

/** Add fingerprint template name to cache
 */
static void
fingerprint_data_add(const char *name)
{
    if( !fingerprint_data_lut )
        goto EXIT;

    if( g_hash_table_lookup(fingerprint_data_lut, name))
        goto EXIT;

    g_hash_table_insert(fingerprint_data_lut,
                        fingerprint_data_create(name),
                        GINT_TO_POINTER(1));
    fpwakeup_schedule_rethink();

EXIT:
    return;
}

/** Predicate for: There are registered fingerprints
 */
static bool
fingerprint_data_exists(void)
{
    guint count = 0;

    if( !fingerprint_data_lut )
        goto EXIT;

    count = g_hash_table_size(fingerprint_data_lut);

EXIT:
    return count > 0;
}

/** Initialize fingerprint template name cache
 */
static void
fingerprint_data_init(void)
{
    if( !fingerprint_data_lut ) {
        mce_log(LL_DEBUG, "fingerprint data init");
        fingerprint_data_lut = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     fingerprint_data_detete_cb, 0);
    }
}

/** Cleanup fingerprint template name cache
 */
static void
fingerprint_data_quit(void)
{
    if( fingerprint_data_lut ) {
        mce_log(LL_DEBUG, "fingerprint data cleanup");
        g_hash_table_unref(fingerprint_data_lut),
            fingerprint_data_lut = 0;
    }
}

/* ========================================================================= *
 * FINGERPRINT_LED_SCANNING
 * ========================================================================= */

/** Control led pattern for indicating fingerprint scanner status
 *
 * @param activate true to activate led, false to deactivate
 */
static void
fingerprint_led_scanning_activate(bool activate)
{
    static bool activated = false;
    if( activated != activate ) {
        datapipe_exec_full((activated = activate) ?
                           &led_pattern_activate_pipe :
                           &led_pattern_deactivate_pipe,
                           MCE_LED_PATTERN_SCANNING_FINGERPRINT);
    }
}

/* ========================================================================= *
 * FINGERPRINT_LED_ACQUIRED
 * ========================================================================= */

/** Control led pattern for indicating fingerprint acquisition events
 *
 * @param activate true to activate led, false to deactivate
 */
static void
fingerprint_led_acquired_activate(bool activate)
{
    static bool activated = false;
    if( activated != activate ) {
        datapipe_exec_full((activated = activate) ?
                           &led_pattern_activate_pipe :
                           &led_pattern_deactivate_pipe,
                           MCE_LED_PATTERN_FINGERPRINT_ACQUIRED);
    }
}

/** Timer id for: Stop fingerprint acquisition event led */
static guint fingerprint_led_acquired_timer_id = 0;

/** Timer callback for: Stop fingerprint acquisition event led */
static gboolean
fingerprint_led_acquired_timer_cb(gpointer aptr)
{
    (void)aptr;
    fingerprint_led_acquired_timer_id = 0;
    fingerprint_led_acquired_activate(false);
    return FALSE;
}

/** Briefly activate fingerprint acquisition event led
 */
static void
fingerprint_led_acquired_trigger(void)
{
    if( fingerprint_led_acquired_timer_id )
        g_source_remove(fingerprint_led_acquired_timer_id);
    fingerprint_led_acquired_timer_id =
        mce_wakelocked_timeout_add(200, fingerprint_led_acquired_timer_cb, 0);
    fingerprint_led_acquired_activate(true);
}

/** Dctivate fingerprint acquisition event led
 */
static void
fingerprint_led_acquired_cancel(void)
{
    if( fingerprint_led_acquired_timer_id ) {
        g_source_remove(fingerprint_led_acquired_timer_id),
            fingerprint_led_acquired_timer_id = 0;
    }
    fingerprint_led_acquired_activate(false);
}

/* ========================================================================= *
 * FINGERPRINT_DATAPIPE
 * ========================================================================= */

/** Update fpstate_pipe content
 *
 * @param state  fingerprint operation state reported by fpd
 */
static void
fingerprint_datapipe_set_fpstate(fpstate_t state)
{
    fpstate_t prev = fpstate;
    fpstate = state;

    if( fpstate == prev )
        goto EXIT;

    mce_log(LL_NOTICE, "fpstate: %s -> %s",
            fpstate_repr(prev),
            fpstate_repr(fpstate));

    datapipe_exec_full(&fpstate_pipe, GINT_TO_POINTER(fpstate));

    switch( fpstate ) {
    case FPSTATE_ENROLLING:
    case FPSTATE_IDENTIFYING:
    case FPSTATE_VERIFYING:
        fingerprint_led_scanning_activate(true);
        break;
    default:
        fingerprint_led_scanning_activate(false);
        break;
    }

    fingerprint_datapipe_update_enroll_in_progress();
    fpwakeup_propagate_fpstate();

    fpwakeup_schedule_rethink();

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
fingerprint_datapipe_evaluate_enroll_in_progress(void)
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
fingerprint_datapipe_update_enroll_in_progress(void)
{
    bool prev = enroll_in_progress;
    enroll_in_progress = fingerprint_datapipe_evaluate_enroll_in_progress();

    if( enroll_in_progress == prev )
        goto EXIT;

    mce_log(LL_NOTICE, "enroll_in_progress: %s -> %s",
            prev ? "true" : "false",
            enroll_in_progress ? "true" : "false");

    datapipe_exec_full(&enroll_in_progress_pipe,
                       GINT_TO_POINTER(enroll_in_progress));
EXIT:
    return;
}

/** Generate user activity to reset blanking timers
 */
static void
fingerprint_datapipe_generate_activity(void)
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
    mce_datapipe_generate_activity();

EXIT:
    return;
}

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
        fingerprint_dbus_fpdata_query_start();
    }
    else {
        fingerprint_dbus_fpdata_query_cancel();
        fingerprint_dbus_fpstate_query_cancel();
        fingerprint_datapipe_set_fpstate(FPSTATE_UNSET);
        fingerprint_data_flush();
    }

    fpwakeup_schedule_rethink();

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

    fingerprint_datapipe_update_enroll_in_progress();

    fpwakeup_schedule_rethink();

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

    fingerprint_datapipe_update_enroll_in_progress();

    fpwakeup_schedule_rethink();

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

    fingerprint_datapipe_update_enroll_in_progress();

    fpwakeup_schedule_rethink();

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

    fingerprint_datapipe_update_enroll_in_progress();

    fpwakeup_schedule_rethink();

EXIT:
    return;

}

/** Change notifications for interaction_expected_pipe
 */
static void
fingerprint_datapipe_interaction_expected_cb(gconstpointer data)
{
    bool prev = interaction_expected;
    interaction_expected = GPOINTER_TO_INT(data);

    if( prev == interaction_expected )
        goto EXIT;

    mce_log(LL_DEBUG, "interaction_expected: %d -> %d",
            prev, interaction_expected);

    fpwakeup_schedule_rethink();

EXIT:
    return;
}

/** Change notifications for topmost_window_pid_pipe
 */
static void
fingerprint_datapipe_topmost_window_pid_cb(gconstpointer data)
{
    int prev = topmost_window_pid;
    topmost_window_pid = GPOINTER_TO_INT(data);

    if( prev == topmost_window_pid )
        goto EXIT;

    mce_log(LL_DEBUG, "topmost_window_pid: %d -> %d",
            prev, topmost_window_pid);

    fpwakeup_schedule_rethink();

EXIT:
    return;
}

/** Change notifications for proximity_sensor_actual
 */
static void
fingerprint_datapipe_proximity_sensor_actual_cb(gconstpointer data)
{
    cover_state_t prev = proximity_sensor_actual;
    proximity_sensor_actual = GPOINTER_TO_INT(data);

    if( proximity_sensor_actual == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "proximity_sensor_actual = %s -> %s",
            proximity_state_repr(prev),
            proximity_state_repr(proximity_sensor_actual));

    fpwakeup_schedule_rethink();

EXIT:
    return;
}

/** Change notifications from lid_sensor_filtered_pipe
 */
static void
fingerprint_datapipe_lid_sensor_filtered_cb(gconstpointer data)
{
    cover_state_t prev = lid_sensor_filtered;
    lid_sensor_filtered = GPOINTER_TO_INT(data);

    if( lid_sensor_filtered == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "lid_sensor_filtered = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(lid_sensor_filtered));

    fpwakeup_schedule_rethink();

EXIT:
    return;
}

/** Datapipe trigger for power key events
 *
 * @param data A pointer to the input_event struct
 */
static void
fingerprint_datapipe_keypress_event_cb(gconstpointer const data)
{
    const struct input_event * const *evp;
    const struct input_event *ev;

    if( !(evp = data) )
        goto EXIT;

    if( !(ev = *evp) )
        goto EXIT;

    /* For example in Sony Xperia X fingerprint scanner is located
     * on the power key. This creates interesting situations as
     * we can also get fingerprint identification while user intents
     * to just press the power key...
     */
    if( ev->type == EV_KEY && ev->code == KEY_POWER ) {
        /* Unprime on power key event of any kind. This effectively
         * cancels fingerprint wakeup that has been detected just
         * before power key press / release.
         */
        if( fpwakeup_set_primed(false) )
            mce_log(LL_WARN, "powerkey event; fp wakeup unprimed");

        /* Denying fpwakeups via policy when power key is pressed
         * down should inhibit fingerprint wakeups in those cases
         * where we see the powerkey press before getting fingerprint
         * identified.
         */
        bool pressed = (ev->value != 0);
        if( powerkey_pressed != pressed ) {
            mce_log(LL_DEBUG, "powerkey_pressed: %d -> %d",
                    powerkey_pressed, pressed);
            powerkey_pressed = pressed;
            fpwakeup_schedule_rethink();
        }
    }
EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t fingerprint_datapipe_handlers[] =
{
    // input triggers
    {
        .datapipe = &keypress_event_pipe,
        .input_cb = fingerprint_datapipe_keypress_event_cb,
    },
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
    {
        .datapipe  = &interaction_expected_pipe,
        .output_cb = fingerprint_datapipe_interaction_expected_cb,
    },
    {
        .datapipe  = &topmost_window_pid_pipe,
        .output_cb = fingerprint_datapipe_topmost_window_pid_cb,
    },
    {
        .datapipe  = &proximity_sensor_actual_pipe,
        .output_cb = fingerprint_datapipe_proximity_sensor_actual_cb,
    },
    {
        .datapipe  = &lid_sensor_filtered_pipe,
        .output_cb = fingerprint_datapipe_lid_sensor_filtered_cb,
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
    mce_datapipe_init_bindings(&fingerprint_datapipe_bindings);
}

/** Remove triggers/filters from datapipes */
static void
fingerprint_datapipe_quit(void)
{
    // triggers
    mce_datapipe_quit_bindings(&fingerprint_datapipe_bindings);
}

/* ========================================================================= *
 * FINGERPRINT_SETTINGS
 * ========================================================================= */

/** Setting changed callback
 *
 * @param gcc   Unused
 * @param id    Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data  Unused
 */
static void
fingerprint_setting_cb(GConfClient *const gcc, const guint id,
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

    if( id == fingerprint_wakeup_mode_setting_id ) {
        gint old = fingerprint_wakeup_mode;
        fingerprint_wakeup_mode = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "fingerprint_wakeup_mode: %d -> %d",
                old, fingerprint_wakeup_mode);
        fpwakeup_schedule_rethink();
    }
    else if( id == fingerprint_trigger_delay_setting_id ) {
        gint old = fingerprint_trigger_delay;
        fingerprint_trigger_delay = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "fingerprint_trigger_delay: %d -> %d",
                old, fingerprint_trigger_delay);
        /* Takes effect on the next identify */
    }
    else if( id == fingerprint_throttle_delay_setting_id ) {
        gint old = fingerprint_throttle_delay;
        fingerprint_throttle_delay = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "fingerprint_throttle_delay: %d -> %d",
                old, fingerprint_throttle_delay);
        /* Takes effect after the next ipc attempt */
    }
    else if( id == fingerprint_allow_delay_setting_id ) {
        gint old = fingerprint_allow_delay;
        fingerprint_allow_delay = gconf_value_get_int(gcv);
        mce_log(LL_NOTICE, "fingerprint_allow_delay: %d -> %d",
                old, fingerprint_allow_delay);
        /* Takes effect on the next policy change */
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
fingerprint_setting_init(void)
{
    mce_setting_track_int(MCE_SETTING_FPWAKEUP_MODE,
                          &fingerprint_wakeup_mode,
                          MCE_DEFAULT_FPWAKEUP_MODE,
                          fingerprint_setting_cb,
                          &fingerprint_wakeup_mode_setting_id);

    mce_setting_track_int(MCE_SETTING_FPWAKEUP_ALLOW_DELAY,
                          &fingerprint_allow_delay,
                          MCE_DEFAULT_FPWAKEUP_ALLOW_DELAY,
                          fingerprint_setting_cb,
                          &fingerprint_allow_delay_setting_id);

    mce_setting_track_int(MCE_SETTING_FPWAKEUP_TRIGGER_DELAY,
                          &fingerprint_trigger_delay,
                          MCE_DEFAULT_FPWAKEUP_TRIGGER_DELAY,
                          fingerprint_setting_cb,
                          &fingerprint_trigger_delay_setting_id);

    mce_setting_track_int(MCE_SETTING_FPWAKEUP_THROTTLE_DELAY,
                          &fingerprint_throttle_delay,
                          MCE_DEFAULT_FPWAKEUP_THROTTLE_DELAY,
                          fingerprint_setting_cb,
                          &fingerprint_throttle_delay_setting_id);
}

/** Stop tracking setting changes
 */
static void
fingerprint_setting_quit(void)
{
    mce_setting_notifier_remove(fingerprint_wakeup_mode_setting_id),
        fingerprint_wakeup_mode_setting_id = 0;
    mce_setting_notifier_remove(fingerprint_allow_delay_setting_id),
        fingerprint_allow_delay_setting_id = 0;
    mce_setting_notifier_remove(fingerprint_trigger_delay_setting_id),
        fingerprint_trigger_delay_setting_id = 0;
    mce_setting_notifier_remove(fingerprint_throttle_delay_setting_id),
        fingerprint_throttle_delay_setting_id = 0;
}

/* ========================================================================= *
 * FINGERPRINT_DBUS
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

    fingerprint_datapipe_set_fpstate(fpstate_parse(state));

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
        fingerprint_datapipe_generate_activity();
        break;
    default:
        break;
    }

    fingerprint_led_acquired_trigger();

EXIT:
    dbus_error_free(&err);

    return TRUE;
}

/** Handle fpd fingerprint added signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
fingerprint_dbus_fpadded_cb(DBusMessage *const msg)
{
    const char *name = 0;
    DBusError   err  = DBUS_ERROR_INIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "parse error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "fpadded: %s", name);
    fingerprint_data_add(name);

EXIT:
    dbus_error_free(&err);

    return TRUE;
}

/** Handle fpd fingerprint removed signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
fingerprint_dbus_fpremoved_cb(DBusMessage *const msg)
{
    const char *name = 0;
    DBusError   err  = DBUS_ERROR_INIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "parse error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "fpremoved: %s", name);
    fingerprint_data_remove(name);

EXIT:
    dbus_error_free(&err);

    return TRUE;
}

/** Handle fpd fingerprint identify succeeded signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
fingerprint_dbus_fpidentified_cb(DBusMessage *const msg)
{
    const char *name = 0;
    DBusError   err  = DBUS_ERROR_INIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "parse error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "fpidentified: %s", name);

    fpwakeup_propagate_fpresult(FPRESULT_IDENTIFIED);

EXIT:
    dbus_error_free(&err);

    return TRUE;
}

/** Handle fpd fingerprint operation aborted signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
fingerprint_dbus_fpaborted_cb(DBusMessage *const msg)
{
    (void)msg;

    mce_log(LL_DEBUG, "fpaborted");

    fpwakeup_propagate_fpresult(FPRESULT_ABORTED);

    return TRUE;
}

/** Handle fpd fingerprint operation failed signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
fingerprint_dbus_fpfailed_cb(DBusMessage *const msg)
{
    (void)msg;

    mce_log(LL_DEBUG, "fpfailed");

    fpwakeup_propagate_fpresult(FPRESULT_FAILED);

    return TRUE;
}

/** Handle fpd fingerprint verify operation succeeded signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
fingerprint_dbus_fpverified_cb(DBusMessage *const msg)
{
    (void)msg;

    mce_log(LL_DEBUG, "fpverified");

    fpwakeup_propagate_fpresult(FPRESULT_VERIFIED);

    return TRUE;
}

/** Handle fpd fingerprint acquisition error signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
fingerprint_dbus_fperror_cb(DBusMessage *const msg)
{
    const char *name = 0;
    DBusError   err  = DBUS_ERROR_INIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "parse error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "fperror: %s", name);

EXIT:
    dbus_error_free(&err);

    return TRUE;
}

/** Handle fpd fingerprint enroll progress signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
fingerprint_dbus_fpprogress_cb(DBusMessage *const msg)
{
    dbus_int32_t percent = 0;
    DBusError   err  = DBUS_ERROR_INIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_INT32, &percent,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "parse error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "fpprogress: %d%%", percent);

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
    {
        .interface = FINGERPRINT1_DBUS_INTERFACE,
        .name      = FINGERPRINT1_DBUS_SIG_ERROR_INFO,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = fingerprint_dbus_fperror_cb,
    },
    {
        .interface = FINGERPRINT1_DBUS_INTERFACE,
        .name      = FINGERPRINT1_DBUS_SIG_ADDED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = fingerprint_dbus_fpadded_cb,
    },
    {
        .interface = FINGERPRINT1_DBUS_INTERFACE,
        .name      = FINGERPRINT1_DBUS_SIG_REMOVED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = fingerprint_dbus_fpremoved_cb,
    },

    {
        .interface = FINGERPRINT1_DBUS_INTERFACE,
        .name      = FINGERPRINT1_DBUS_SIG_IDENTIFIED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = fingerprint_dbus_fpidentified_cb,
    },
    {
        .interface = FINGERPRINT1_DBUS_INTERFACE,
        .name      = FINGERPRINT1_DBUS_SIG_ABORTED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = fingerprint_dbus_fpaborted_cb,
    },
    {
        .interface = FINGERPRINT1_DBUS_INTERFACE,
        .name      = FINGERPRINT1_DBUS_SIG_FAILED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = fingerprint_dbus_fpfailed_cb,
    },

    {
        .interface = FINGERPRINT1_DBUS_INTERFACE,
        .name      = FINGERPRINT1_DBUS_SIG_VERIFIED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = fingerprint_dbus_fpverified_cb,
    },
    {
        .interface = FINGERPRINT1_DBUS_INTERFACE,
        .name      = FINGERPRINT1_DBUS_SIG_ENROLL_PROGRESS,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = fingerprint_dbus_fpprogress_cb,
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

/* ------------------------------------------------------------------------- *
 * FINGERPRINT1_DBUS_REQ_GET_STATE
 * ------------------------------------------------------------------------- */

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

    fingerprint_datapipe_set_fpstate(fpstate_parse(state));

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

/* ------------------------------------------------------------------------- *
 * FINGERPRINT1_DBUS_REQ_GET_ALL
 * ------------------------------------------------------------------------- */

static DBusPendingCall *fingerprint_dbus_fpdata_query_pc = 0;

/** Handle reply to async fpdata query
 *
 * @param pc    pending call handle
 * @param aptr  (unused) user data pointer
 */
static void
fingerprint_dbus_fpdata_query_cb(DBusPendingCall *pc, void *aptr)
{
    (void)aptr;

    DBusMessage *rsp   = 0;
    DBusError    err   = DBUS_ERROR_INIT;
    char       **arr   = 0;
    int          len   = 0;

    if( pc != fingerprint_dbus_fpdata_query_pc )
        goto EXIT;

    fingerprint_dbus_fpdata_query_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
        mce_log(LL_WARN, "no reply");
        goto EXIT;
    }

    if( dbus_set_error_from_message(&err, rsp) ||
        !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &arr, &len,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    for( int i = 0; i < len; ++i )
        fingerprint_data_add(arr[i]);

EXIT:
    if( rsp ) dbus_message_unref(rsp);

    dbus_error_free(&err);

    dbus_pending_call_unref(pc);

    return;
}

/** Cancel pending async fpdata query
 */
static void
fingerprint_dbus_fpdata_query_cancel(void)
{
    if( fingerprint_dbus_fpdata_query_pc ) {
        dbus_pending_call_cancel(fingerprint_dbus_fpdata_query_pc);
        dbus_pending_call_unref(fingerprint_dbus_fpdata_query_pc);
        fingerprint_dbus_fpdata_query_pc = 0;
    }
}

/** Initiate async query to find out current fpdata
 */
static void
fingerprint_dbus_fpdata_query_start(void)
{
    fingerprint_dbus_fpdata_query_cancel();

    dbus_send_ex(FINGERPRINT1_DBUS_SERVICE,
                 FINGERPRINT1_DBUS_ROOT_OBJECT,
                 FINGERPRINT1_DBUS_INTERFACE,
                 FINGERPRINT1_DBUS_REQ_GET_ALL,
                 fingerprint_dbus_fpdata_query_cb, 0, 0,
                 &fingerprint_dbus_fpdata_query_pc,
                 DBUS_TYPE_INVALID);
}

/* ========================================================================= *
 * FPWAKEUP
 * ========================================================================= */

/** Policy state: Using fpd for fingerprint wakeups is allowed
 */
static bool fpwakeup_allowed_state = false;

/** Predicate for: Using fpd for fingerprint wakeups is allowed
 */
static bool
fpwakeup_is_allowed(void)
{
    return fpwakeup_allowed_state;
}

/** Allow/deny using fpd for fingerprint wakeups
 */
static void
fpwakeup_set_allowed(bool allowed)
{
    fpwakeup_cancel_allow();

    if( fpwakeup_allowed_state != allowed ) {
        fpwakeup_allowed_state = allowed;
        mce_log(LL_DEBUG, "fingerprint_wakeup = %s",
                fpwakeup_allowed_state ? "allowed" : "denied");

        fpwakeup_schedule_rethink();
    }
}

/** Timer for: Adding hysteresis to allowing fingerprint wakeups
 */
static guint fpwakeup_allow_id = 0;

/** Timer callback for allowing fingerprint wakeups
 */
static gboolean
fpwakeup_allow_cb(gpointer aptr)
{
    (void)aptr;

    if( !fpwakeup_allow_id )
        goto EXIT;

    fpwakeup_allow_id = 0;
    fpwakeup_set_allowed(true);

EXIT:
    return G_SOURCE_REMOVE;
}

/** Cancel delayed fingerprint wakeup allowing
 */
static void
fpwakeup_cancel_allow(void)
{
    if( fpwakeup_allow_id ) {
        g_source_remove(fpwakeup_allow_id),
            fpwakeup_allow_id = 0;
    }
}

/** Allow fingerprint wakeups after slight delay
 *
 * The purpose is to add hysteresis to denied -> allowed policy
 * decisions, so that short living state transitions do not cause
 * unwanted fpd ipc activity.
 */
static void
fpwakeup_schedule_allow(void)
{
    if( !fpwakeup_allow_id ) {
        fpwakeup_allow_id = mce_wakelocked_timeout_add(fingerprint_allow_delay,
                                                       fpwakeup_allow_cb, 0);
    }
}

/** Evaluate whether system state allows fingerprint wakeups
 */
static bool
fpwakeup_evaluate_allowed(void)
{
    bool allowed = false;

    /* Must be running in USER mode */
    if( system_state != MCE_SYSTEM_STATE_USER )
        goto EXIT;

    /* Fingerprint daemon must be running */
    if( fpd_service_state != SERVICE_STATE_RUNNING )
        goto EXIT;

    /* Need to have fingerprints registered */
    if( !fingerprint_data_exists() )
        goto EXIT;

    /* Check fpwakeup policy */
    switch( fingerprint_wakeup_mode ) {
    default:
    case FPWAKEUP_ENABLE_NEVER:
        goto EXIT;
    case FPWAKEUP_ENABLE_ALWAYS:
        /* Lid must not be closed */
        if( lid_sensor_filtered == COVER_CLOSED )
            goto EXIT;
        /* Proximity sensor state: don't care */
        break;
    case FPWAKEUP_ENABLE_NO_PROXIMITY:
        /* Lid must not be closed */
        if( lid_sensor_filtered == COVER_CLOSED )
            goto EXIT;
        /* Proximity sensor must not be covered or unknown */
        if( proximity_sensor_actual != COVER_OPEN )
            goto EXIT;
        break;
    }

    /* Power key must not be pressed down */
    if( powerkey_pressed )
        goto EXIT;

    switch( display_state_next ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
        /* Devicelock ui disables auth in truly powered
         * off display states -> mce can step in*/
        break;
    case MCE_DISPLAY_LPM_ON:
        /* Devicelock ui handles unlocking in lpm */
        if( devicelock_state != DEVICELOCK_STATE_UNLOCKED )
            goto EXIT;
        break;

    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        /* Devicelock ui handles unlocking on/dimmed */
        if( devicelock_state != DEVICELOCK_STATE_UNLOCKED )
            goto EXIT;

        /* Nothing to do if lockscreen is deactivated */
        if( !(submode & MCE_SUBMODE_TKLOCK) )
            goto EXIT;

        /* Nothing to do when interacting with lockscreen */
        if( interaction_expected )
            goto EXIT;

        /* Nothing to do when some app is on top of lockscreen */
        if( topmost_window_pid != -1 )
            goto EXIT;
        break;

    default:
        goto EXIT;
    }

    /* MCE can use fingerprint scanner as kind of power key */
    allowed = true;
EXIT:
    return allowed;
}

/** Update fingerprint wakeups allowed policy state
 */
static void
fpwakeup_update_allowed(void)
{
    bool allowed = fpwakeup_evaluate_allowed();

    if( !allowed ) {
        fpwakeup_set_allowed(false);
    }
    else if( !fpwakeup_allowed_state ) {
        fpwakeup_schedule_allow();
    }
}

/** Re-evaluate everything related to fingerprint wakeups
 */
static void
fpwakeup_rethink_now(void)
{
    fpwakeup_cancel_rethink();
    fpwakeup_update_allowed();
    fpwakeup_propagate_eval();

    return;
}

/** Idle timer for: Re-evaluating fingerprint wakeup
 */
static guint fpwakeup_rethink_id = 0;

/** Idle callback for: Re-evaluating fingerprint wakeup
 */
static gboolean
fpwakeup_rethink_cb(gpointer aptr)
{
    (void)aptr;

    if( !fpwakeup_rethink_id )
        goto EXIT;

    fpwakeup_rethink_id = 0;
    fpwakeup_rethink_now();

EXIT:
    return G_SOURCE_REMOVE;
}

/** Schedule re-evaluation of fingerprint wakeup policy and state
 *
 * Fingerprint wakeup policy depends on multitude of state variables
 * and datapipes. Some of these are interconnected, and can cause
 * a flurry of triggers. To avoid excessive re-evaluation, an idle
 * callback is used to compress N triggers into just one evaluation.
 */
static void
fpwakeup_schedule_rethink(void)
{
    if( !fpwakeup_rethink_id ) {
        fpwakeup_rethink_id = mce_wakelocked_idle_add(fpwakeup_rethink_cb, 0);
    }
}

/** Cancel re-evaluation of fingerprint wakeup policy and state
 */
static void
fpwakeup_cancel_rethink(void)
{
    if( fpwakeup_rethink_id ) {
        g_source_remove(fpwakeup_rethink_id),
            fpwakeup_rethink_id = 0;
    }
}

/** Propagate fingerprint daemon state changes to operation state machines
 */
static void
fpwakeup_propagate_fpstate(void)
{
    for( size_t i = 0; i < G_N_ELEMENTS(fpoperation_lut); ++i )
        fpoperation_set_fpstate(&fpoperation_lut[i], fpstate);
}

/** Propagate fingerprint daemon result events to operation state machines
 */
static void
fpwakeup_propagate_fpresult(fpresult_t event)
{
    for( size_t i = 0; i < G_N_ELEMENTS(fpoperation_lut); ++i )
        fpoperation_result(&fpoperation_lut[i], event);
}

/** Propagate state re-evaluation to operation state machines
 */
static void
fpwakeup_propagate_eval(void)
{
    for( size_t i = 0; i < G_N_ELEMENTS(fpoperation_lut); ++i )
        fpoperation_eval(&fpoperation_lut[i]);
}

static bool fpwakeup_primed = false;

static bool
fpwakeup_set_primed(bool prime)
{
    bool changed = false;
    if( fpwakeup_primed != prime ) {
        fpwakeup_primed = prime;
        changed = true;
    }
    return changed;
}

/** Execute display wakeup
 */
static void
fpwakeup_trigger(void)
{
    if( !fpwakeup_set_primed(false) ) {
        /* Other overlapping inputs, such as power key press,
         * have taken priority over fingerprint wakeup.
         */
        mce_log(LL_WARN, "fingerprint wakeup; explicitly ignored");
    }
    else if( !fpwakeup_is_allowed() ) {
        /* Policy state changed somewhere in between requesting
         * fingerprint identification and getting the result.
         */
        mce_log(LL_WARN, "fingerprint wakeup; ignored due to policy");
    }
    else {
        mce_log(LL_CRUCIAL, "fingerprint wakeup triggered");

        /* (Mis)use haptic feedback associated with device unlocking */
        datapipe_exec_full(&ngfd_event_request_pipe,
                           "unlock_device");

        /* Make sure we unblank / exit from lpm */
        mce_datapipe_request_display_state(MCE_DISPLAY_ON);

        /* Exit from lockscreen */
        mce_datapipe_request_tklock(TKLOCK_REQUEST_OFF);

        /* Deactivate type=6 led patterns (e.g. sms/email notifications)
         * by signaling "true user activity" via synthetized gesture
         * input event. (The event type ought not matter, but using
         * double tap event is somewhat logical and does not cause side
         * effects in the few places where the event type is actually
         * checked.)
         */
        const struct input_event ev = {
            .type  = EV_MSC,
            .code  = MSC_GESTURE,
            .value = GESTURE_DOUBLETAP | GESTURE_SYNTHESIZED,
        };
        datapipe_exec_full(&user_activity_event_pipe, &ev);
    }
}

/* ========================================================================= *
 * G_MODULE
 * ========================================================================= */

/** Init function for the fpd tracking module
 *
 * @param module (unused) module handle
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *
g_module_check_init(GModule *module)
{
    (void)module;

    fingerprint_data_init();
    fingerprint_setting_init();
    fingerprint_datapipe_init();
    fingerprint_dbus_init();

    return NULL;
}

/** Exit function for the fpd tracking module
 *
 * @param module (unused) module handle
 */
G_MODULE_EXPORT void
g_module_unload(GModule *module)
{
    (void)module;

    fingerprint_data_quit();
    fingerprint_setting_quit();

    fingerprint_dbus_quit();
    fingerprint_datapipe_quit();
    fingerprint_dbus_fpstate_query_cancel();
    fingerprint_dbus_fpdata_query_cancel();
    fpwakeup_cancel_rethink();
    fpwakeup_cancel_allow();

    fingerprint_led_scanning_activate(false);
    fingerprint_led_acquired_cancel();
    return;
}
