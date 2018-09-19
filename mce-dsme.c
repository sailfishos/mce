/**
 * @file mce-dsme.c
 * Interface code and logic between
 * DSME (the Device State Management Entity)
 * and MCE (the Mode Control Entity)
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright © 2015      Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Ismo Laitinen <ismo.laitinen@nokia.com>
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

#include "mce-dsme.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-dbus.h"
#include "mce-worker.h"

#include <stdlib.h>
#include <unistd.h>

#include <dsme/state.h>
#include <dsme/protocol.h>
#include <dsme/processwd.h>

/* ========================================================================= *
 * MODULE DATA
 * ========================================================================= */

/** Pointer to the dsmesock connection */
static dsmesock_connection_t *mce_dsme_socket_connection = NULL;

/** I/O watch for mce_dsme_socket_connection */
static guint mce_dsme_socket_recv_id = 0;

/** ID for delayed state transition reporting timer */
static guint mce_dsme_transition_id = 0;

/** Availability of dsme; from dsme_service_state_pipe */
static service_state_t dsme_service_state = SERVICE_STATE_UNDEF;

/** System state from dsme; fed to system_state_pipe */
static system_state_t system_state = MCE_SYSTEM_STATE_UNDEF;

/** Shutdown warning from dsme; fed to shutting_down_pipe */
static bool mce_dsme_shutting_down_flag = false;

/** Cached init_done state; assume unknown */
static tristate_t init_done = TRISTATE_UNKNOWN;

/* ========================================================================= *
 * MODULE FUNCTIONS
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * UTILITY_FUNCTIONS
 * ------------------------------------------------------------------------- */

static system_state_t mce_dsme_normalise_system_state (dsme_state_t dsmestate);

/* ------------------------------------------------------------------------- *
 * WORKER_WATCHDOG
 * ------------------------------------------------------------------------- */

static void           mce_dsme_worker_done_cb  (void *aptr, void *reply);
static void          *mce_dsme_worker_pong_cb  (void *aptr);
static void           mce_dsme_worker_ping     (void);;

/* ------------------------------------------------------------------------- *
 * PROCESS_WATCHDOG
 * ------------------------------------------------------------------------- */

static void           mce_dsme_processwd_pong (void);
static void           mce_dsme_processwd_init (void);
static void           mce_dsme_processwd_quit (void);

/* ------------------------------------------------------------------------- *
 * SYSTEM_STATE
 * ------------------------------------------------------------------------- */

static void           mce_dsme_query_system_state      (void);
void                  mce_dsme_request_powerup         (void);
void                  mce_dsme_request_reboot          (void);
void                  mce_dsme_request_normal_shutdown (void);

/* ------------------------------------------------------------------------- *
 * TRANSITION_SUBMODE
 * ------------------------------------------------------------------------- */

static gboolean       mce_dsme_transition_cb       (gpointer data);
static void           mce_dsme_transition_cancel   (void);
static void           mce_dsme_transition_schedule (void);

/* ------------------------------------------------------------------------- *
 * SHUTTING_DOWN
 * ------------------------------------------------------------------------- */

static bool           mce_dsme_is_shutting_down  (void);
static void           mce_dsme_set_shutting_down (bool shutting_down);

/* ------------------------------------------------------------------------- *
 * SOCKET_CONNECTION
 * ------------------------------------------------------------------------- */

static bool           mce_dsme_socket_send         (void *msg);
static gboolean       mce_dsme_socket_recv_cb      (GIOChannel *source, GIOCondition condition, gpointer data);
static bool           mce_dsme_socket_is_connected (void);
static bool           mce_dsme_socket_connect      (void);
static void           mce_dsme_socket_disconnect   (void);

/* ------------------------------------------------------------------------- *
 * DBUS_HANDLERS
 * ------------------------------------------------------------------------- */

static gboolean       mce_dsme_dbus_init_done_cb              (DBusMessage *const msg);
static gboolean       mce_dsme_dbus_shutdown_cb               (DBusMessage *const msg);
static gboolean       mce_dsme_dbus_thermal_shutdown_cb       (DBusMessage *const msg);
static gboolean       mce_dsme_dbus_battery_empty_shutdown_cb (DBusMessage *const msg);

static void           mce_dsme_dbus_init(void);
static void           mce_dsme_dbus_quit(void);

/* ------------------------------------------------------------------------- *
 * DATAPIPE_TRACKING
 * ------------------------------------------------------------------------- */

static void           mce_dsme_datapipe_dsme_service_state_cb (gconstpointer data);
static void           mce_dsme_datapipe_init_done_cb          (gconstpointer data);
static void           mce_dsme_datapipe_system_state_cb       (gconstpointer data);

static void           mce_dsme_datapipe_init(void);
static void           mce_dsme_datapipe_quit(void);

/* ------------------------------------------------------------------------- *
 * MODULE_INIT_EXIT
 * ------------------------------------------------------------------------- */

gboolean              mce_dsme_init(void);
void                  mce_dsme_exit(void);

/* ========================================================================= *
 * UTILITY_FUNCTIONS
 * ========================================================================= */

/** Convert system states used by dsme to the ones used in mce datapipes
 *
 * @param dsmestate System state constant used by dsme
 *
 * @return System state constant used within mce
 */
static system_state_t mce_dsme_normalise_system_state(dsme_state_t dsmestate)
{
    system_state_t state = MCE_SYSTEM_STATE_UNDEF;

    switch (dsmestate) {
    case DSME_STATE_SHUTDOWN:
        state = MCE_SYSTEM_STATE_SHUTDOWN;
        break;

    case DSME_STATE_USER:
        state = MCE_SYSTEM_STATE_USER;
        break;

    case DSME_STATE_ACTDEAD:
        state = MCE_SYSTEM_STATE_ACTDEAD;
        break;

    case DSME_STATE_REBOOT:
        state = MCE_SYSTEM_STATE_REBOOT;
        break;

    case DSME_STATE_BOOT:
        state = MCE_SYSTEM_STATE_BOOT;
        break;

    case DSME_STATE_NOT_SET:
        break;

    case DSME_STATE_TEST:
        mce_log(LL_WARN,
                "Received DSME_STATE_TEST; treating as undefined");
        break;

    case DSME_STATE_MALF:
        mce_log(LL_WARN,
                "Received DSME_STATE_MALF; treating as undefined");
        break;

    case DSME_STATE_LOCAL:
        mce_log(LL_WARN,
                "Received DSME_STATE_LOCAL; treating as undefined");
        break;

    default:
        mce_log(LL_ERR,
                "Received an unknown state from DSME; "
                "treating as undefined");
        break;
    }

    return state;
}

/* ========================================================================= *
 * WORKER_WATCHDOG
 * ========================================================================= */

/** Validation context for the jobs passed from this module */
#define MCE_DSME_WORKERWD_JOB_CONTEXT "mce-dsme"

/** Descriptive name for the dummy sanity check worker thread jobs */
#define MCE_DSME_WORKERWD_JOB_NAME    "ping"

/** Number of worker jobs scheduled */
static guint mce_dsme_worker_ping_cnt = 0;

/** Number of worker jobs executed */
static guint mce_dsme_worker_pong_cnt = 0;

/** Number of worker jobs notified */
static guint mce_dsme_worker_done_cnt = 0;

/** Flag for: worker thread issues noticed */
static bool  mce_dsme_worker_misbehaving = false;

/** Handle dummy job finished notification
 *
 * @param aptr   Ping count passed to the worker thread (as void pointer)
 * @param reply  Ping count returned from the worker thread (as void pointer)
 */
static void mce_dsme_worker_done_cb(void *aptr, void *reply)
{
    (void)reply;

    mce_dsme_worker_done_cnt = GPOINTER_TO_INT(aptr);

    /* Check if the last job scheduled matches what got executed
     * and notified as finished */

    if( mce_dsme_worker_ping_cnt != mce_dsme_worker_pong_cnt ||
        mce_dsme_worker_ping_cnt != mce_dsme_worker_done_cnt ) {
        mce_log(LL_CRIT, "worker thread is misbehaving");
        mce_dsme_worker_misbehaving = true;
    }
}

/** Dummy job to be executed by the worker thread
 *
 * @param aptr  Ping count (as void pointer)
 *
 * @return Ping count (as void pointer)
 */
static void *mce_dsme_worker_pong_cb(void *aptr)
{
    /* Note: This is executed in the worker thread context */

    mce_dsme_worker_pong_cnt = GPOINTER_TO_INT(aptr);

    /* Check if the job we got to execute is the latest one
     * scheduled from the main thread */

    if( mce_dsme_worker_ping_cnt != mce_dsme_worker_pong_cnt ) {
        mce_log(LL_CRIT, "worker thread is misbehaving");
        mce_dsme_worker_misbehaving = true;
    }

    return aptr;
}

/** Run a dummy job through worker thread to make sure it is still functioning
 */
static void mce_dsme_worker_ping(void)
{
    /* Check if previous job got executed as expected */
    if( mce_dsme_worker_ping_cnt != mce_dsme_worker_pong_cnt ||
        mce_dsme_worker_ping_cnt != mce_dsme_worker_done_cnt ) {
        mce_log(LL_CRIT, "worker thread is possibly stuck");
        mce_dsme_worker_misbehaving = true;
    }
    else if( mce_dsme_worker_misbehaving ) {
        mce_dsme_worker_misbehaving = false;
        mce_log(LL_CRIT, "worker thread is working again");
    }

    mce_dsme_worker_ping_cnt += 1;

    mce_worker_add_job(MCE_DSME_WORKERWD_JOB_CONTEXT,
                       MCE_DSME_WORKERWD_JOB_NAME,
                       mce_dsme_worker_pong_cb,
                       mce_dsme_worker_done_cb,
                       GINT_TO_POINTER(mce_dsme_worker_ping_cnt));
}

/* ========================================================================= *
 * PROCESS_WATCHDOG
 * ========================================================================= */

/**
 * Send pong message to the DSME process watchdog
 */
static void mce_dsme_processwd_pong(void)
{
    /* Set up the message */
    DSM_MSGTYPE_PROCESSWD_PONG msg =
        DSME_MSG_INIT(DSM_MSGTYPE_PROCESSWD_PONG);
    msg.pid = getpid();

    /* Send the message */
    mce_dsme_socket_send(&msg);

    /* Run worker thread sanity check */
    mce_dsme_worker_ping();

    /* Execute hearbeat actions even if ping-pong ipc failed */
    datapipe_exec_full(&heartbeat_event_pipe, GINT_TO_POINTER(0),
                       DATAPIPE_USE_INDATA, DATAPIPE_CACHE_NOTHING);
}

/**
 * Register to DSME process watchdog
 */
static void mce_dsme_processwd_init(void)
{
    /* Set up the message */
    DSM_MSGTYPE_PROCESSWD_CREATE msg =
        DSME_MSG_INIT(DSM_MSGTYPE_PROCESSWD_CREATE);
    msg.pid = getpid();

    /* Send the message */
    mce_dsme_socket_send(&msg);
}

/**
 * Unregister from DSME process watchdog
 */
static void mce_dsme_processwd_quit(void)
{
    mce_log(LL_DEBUG, "Disabling DSME process watchdog");

    /* Set up the message */
    DSM_MSGTYPE_PROCESSWD_DELETE msg =
        DSME_MSG_INIT(DSM_MSGTYPE_PROCESSWD_DELETE);
    msg.pid = getpid();

    /* Send the message */
    mce_dsme_socket_send(&msg);
}

/* ========================================================================= *
 * SYSTEM_STATE
 * ========================================================================= */

/** Send system state inquiry
 */
static void mce_dsme_query_system_state(void)
{
    /* Set up the message */
    DSM_MSGTYPE_STATE_QUERY msg = DSME_MSG_INIT(DSM_MSGTYPE_STATE_QUERY);

    /* Send the message */
    mce_dsme_socket_send(&msg);
}

/** Request powerup
 */
void mce_dsme_request_powerup(void)
{
    /* Set up the message */
    DSM_MSGTYPE_POWERUP_REQ msg = DSME_MSG_INIT(DSM_MSGTYPE_POWERUP_REQ);

    /* Send the message */
    mce_dsme_socket_send(&msg);
}

/** Request reboot
 */
void mce_dsme_request_reboot(void)
{
    if( datapipe_get_gint(osupdate_running_pipe) ) {
        mce_log(LL_WARN, "reboot blocked; os update in progress");
        goto EXIT;
    }

    /* Set up the message */
    DSM_MSGTYPE_REBOOT_REQ msg = DSME_MSG_INIT(DSM_MSGTYPE_REBOOT_REQ);

    /* Send the message */
    mce_dsme_socket_send(&msg);
EXIT:
    return;
}

/** Request normal shutdown
 */
void mce_dsme_request_normal_shutdown(void)
{
    if( datapipe_get_gint(osupdate_running_pipe) ) {
        mce_log(LL_WARN, "shutdown blocked; os update in progress");
        goto EXIT;
    }

    /* Set up the message */
    DSM_MSGTYPE_SHUTDOWN_REQ msg = DSME_MSG_INIT(DSM_MSGTYPE_SHUTDOWN_REQ);

    /* Send the message */
    mce_dsme_socket_send(&msg);
EXIT:
    return;
}

/* ========================================================================= *
 * TRANSITION_SUBMODE
 * ========================================================================= */

/** Timer callback for ending transition submode
 *
 * @param data (not used)
 *
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean mce_dsme_transition_cb(gpointer data)
{
    (void)data;

    mce_dsme_transition_id = 0;

    mce_rem_submode_int32(MCE_SUBMODE_TRANSITION);

    return FALSE;
}

/** Cancel delayed end of transition submode
 */
static void mce_dsme_transition_cancel(void)
{
    if( mce_dsme_transition_id ) {
        g_source_remove(mce_dsme_transition_id),
            mce_dsme_transition_id = 0;
    }
}

/** Schedule delayed end of transition submode
 */
static void mce_dsme_transition_schedule(void)
{
    /* Remove existing timeout */
    mce_dsme_transition_cancel();

    /* Check if we have transition to end */
    if( !(mce_get_submode_int32() & MCE_SUBMODE_TRANSITION) )
        goto EXIT;

#if TRANSITION_DELAY > 0
    /* Setup new timeout */
    mce_dsme_transition_id =
        g_timeout_add(TRANSITION_DELAY, mce_dsme_transition_cb, NULL);
#elif TRANSITION_DELAY == 0
    /* Set up idle callback */
    mce_dsme_transition_id =
        g_idle_add(mce_dsme_transition_cb, NULL);
#else
    /* Trigger immediately */
    mce_dsme_transition_cb(0);
#endif

EXIT:
    return;
}

/* ========================================================================= *
 * SHUTTING_DOWN
 * ========================================================================= */

/** Predicate for: device is shutting down
 *
 * @return true if shutdown is in progress, false otherwise
 */
static bool mce_dsme_is_shutting_down(void)
{
    return mce_dsme_shutting_down_flag;
}

/** Update device is shutting down state
 *
 * @param shutting_down true if shutdown has started, false otherwise
 */
static void mce_dsme_set_shutting_down(bool shutting_down)
{
    if( mce_dsme_shutting_down_flag == shutting_down )
        goto EXIT;

    mce_dsme_shutting_down_flag = shutting_down;

    mce_log(LL_DEVEL, "Shutdown %s",
            mce_dsme_shutting_down_flag ? "started" : "canceled");

    /* Re-evaluate dsmesock connection */
    if( !mce_dsme_shutting_down_flag )
        mce_dsme_socket_connect();

    datapipe_exec_full(&shutting_down_pipe,
                       GINT_TO_POINTER(mce_dsme_shutting_down_flag),
                       DATAPIPE_USE_INDATA, DATAPIPE_CACHE_INDATA);

EXIT:
    return;
}

/* ========================================================================= *
 * SOCKET_CONNECTION
 * ========================================================================= */

/**
 * Generic send function for dsmesock messages
 *
 * @param msg A pointer to the message to send
 */
static bool mce_dsme_socket_send(void *msg)
{
    bool res = false;

    if( !mce_dsme_socket_connection ) {
        mce_log(LL_WARN, "failed to send %s to dsme; %s",
                dsmemsg_name(msg), "not connected");
        goto EXIT;
    }

    if( dsmesock_send(mce_dsme_socket_connection, msg) == -1) {
        mce_log(LL_ERR, "failed to send %s to dsme; %m",
                dsmemsg_name(msg));
    }

    mce_log(LL_DEBUG, "%s sent to DSME", dsmemsg_name(msg));

    res = true;

EXIT:
    return res;
}

/** Callback for pending I/O from dsmesock
 *
 * @param source     (not used)
 * @param condition  I/O condition that caused the callback to be called
 * @param data       (not used)
 *
 * @return TRUE if iowatch is to be kept, or FALSE if it should be removed
 */
static gboolean mce_dsme_socket_recv_cb(GIOChannel *source,
                                        GIOCondition condition,
                                        gpointer data)
{
    gboolean keep_going = TRUE;
    dsmemsg_generic_t *msg = 0;

    DSM_MSGTYPE_STATE_CHANGE_IND *msg2;

    (void)source;
    (void)data;

    if( condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) ) {
        if( !mce_dsme_is_shutting_down() )
            mce_log(LL_CRIT, "DSME socket hangup/error");
        keep_going = FALSE;
        goto EXIT;
    }

    if( !(msg = dsmesock_receive(mce_dsme_socket_connection)) )
        goto EXIT;

    if( DSMEMSG_CAST(DSM_MSGTYPE_CLOSE, msg) ) {
        if( !mce_dsme_is_shutting_down() )
            mce_log(LL_WARN, "DSME socket closed");
        keep_going = FALSE;
    }
    else if( DSMEMSG_CAST(DSM_MSGTYPE_PROCESSWD_PING, msg) ) {
        mce_dsme_processwd_pong();
    }
    else if( (msg2 = DSMEMSG_CAST(DSM_MSGTYPE_STATE_CHANGE_IND, msg)) ) {
        system_state_t state = mce_dsme_normalise_system_state(msg2->state);
        datapipe_exec_full(&system_state_pipe,
                           GINT_TO_POINTER(state),
                           DATAPIPE_USE_INDATA, DATAPIPE_CACHE_INDATA);
    }
    else {
        mce_log(LL_DEBUG, "Unhandled %s message received from DSME",
                dsmemsg_name(msg));
    }

EXIT:
    free(msg);

    if( !keep_going ) {
        if( !mce_dsme_is_shutting_down() ) {
            mce_log(LL_WARN, "DSME i/o notifier disabled;"
                    " assuming dsme was stopped");
        }

        /* mark notifier as removed */
        mce_dsme_socket_recv_id = 0;

        /* close and wait for possible dsme restart */
        mce_dsme_socket_disconnect();
    }

    return keep_going;
}

/** Predicate for: socket connection to dsme exists
 *
 * @return true if connected, false otherwise
 */
static bool mce_dsme_socket_is_connected(void)
{
    return mce_dsme_socket_connection && mce_dsme_socket_recv_id;
}

/** Initialise dsmesock connection
 *
 * @return true on success, false on failure
 */
static bool mce_dsme_socket_connect(void)
{
    GIOChannel *iochan = NULL;

    /* No new connections during shutdown */
    if( mce_dsme_is_shutting_down() )
        goto EXIT;

    /* No new connections unless dsme dbus service is up */
    if( dsme_service_state != SERVICE_STATE_RUNNING )
        goto EXIT;

    /* Already connected ? */
    if( mce_dsme_socket_recv_id )
        goto EXIT;

    mce_log(LL_DEBUG, "Opening DSME socket");

    if( !(mce_dsme_socket_connection = dsmesock_connect()) ) {
        mce_log(LL_ERR, "Failed to open DSME socket");
        goto EXIT;
    }

    mce_log(LL_DEBUG, "Adding DSME socket notifier");

    if( !(iochan = g_io_channel_unix_new(mce_dsme_socket_connection->fd)) ) {
        mce_log(LL_ERR,"Failed to set up I/O channel for DSME socket");
        goto EXIT;
    }

    mce_dsme_socket_recv_id =
        g_io_add_watch(iochan,
                       G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                       mce_dsme_socket_recv_cb, NULL);

    /* Query the current system state; if the mainloop isn't running,
     * this will trigger an update when the mainloop starts
     */
    mce_dsme_query_system_state();

    /* Register with DSME's process watchdog */
    mce_dsme_processwd_init();

EXIT:
    if( iochan ) g_io_channel_unref(iochan);

    if( !mce_dsme_socket_recv_id )
        mce_dsme_socket_disconnect();

    return mce_dsme_socket_is_connected();
}

/** Close dsmesock connection
 */
static void mce_dsme_socket_disconnect(void)
{
    if( mce_dsme_socket_recv_id ) {
        mce_log(LL_DEBUG, "Removing DSME socket notifier");
        g_source_remove(mce_dsme_socket_recv_id);
        mce_dsme_socket_recv_id = 0;

        /* Still having had a live socket notifier means: We have
         * initiated the dsmesock disconnect and need to deactivate
         * the process watchdog before actually disconnecting. */
        mce_dsme_processwd_quit();
    }

    if( mce_dsme_socket_connection ) {
        mce_log(LL_DEBUG, "Closing DSME socket");
        dsmesock_close(mce_dsme_socket_connection);
        mce_dsme_socket_connection = 0;
    }
}

/* ========================================================================= *
 * DBUS_HANDLERS
 * ========================================================================= */

/** D-Bus callback for the init done notification signal
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean mce_dsme_dbus_init_done_cb(DBusMessage *const msg)
{
    (void)msg;

    mce_log(LL_DEVEL, "Received init done notification");

    /* Remove transition submode after brief delay */
    mce_dsme_transition_schedule();

    return TRUE;
}

/** D-Bus callback for the shutdown notification signal
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean mce_dsme_dbus_shutdown_cb(DBusMessage *const msg)
{
    (void)msg;

    mce_log(LL_WARN, "Received shutdown notification");
    mce_dsme_set_shutting_down(true);

    return TRUE;
}

/** D-Bus callback for the thermal shutdown notification signal
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
mce_dsme_dbus_thermal_shutdown_cb(DBusMessage *const msg)
{
    (void)msg;

    mce_log(LL_WARN, "Received thermal shutdown notification");
    mce_datapipe_request_display_state(MCE_DISPLAY_ON);

    return TRUE;
}

/** D-Bus callback for the battery empty shutdown notification signal
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
mce_dsme_dbus_battery_empty_shutdown_cb(DBusMessage *const msg)
{
    (void)msg;

    mce_log(LL_WARN, "Received battery empty shutdown notification");
    mce_datapipe_request_display_state(MCE_DISPLAY_ON);

    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t mce_dsme_dbus_handlers[] =
{
    /* signals */
    {
        .interface = "com.nokia.startup.signal",
        .name      = "init_done",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = mce_dsme_dbus_init_done_cb,
    },
    {
        .interface = "com.nokia.dsme.signal",
        .name      = "shutdown_ind",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = mce_dsme_dbus_shutdown_cb,
    },
    {
        .interface = "com.nokia.dsme.signal",
        .name      = "thermal_shutdown_ind",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = mce_dsme_dbus_thermal_shutdown_cb,
    },
    {
        .interface = "com.nokia.dsme.signal",
        .name      = "battery_empty_ind",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = mce_dsme_dbus_battery_empty_shutdown_cb,
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Add dbus handlers
 */
static void mce_dsme_dbus_init(void)
{
    mce_dbus_handler_register_array(mce_dsme_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mce_dsme_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(mce_dsme_dbus_handlers);
}

/* ========================================================================= *
 * DATAPIPE_TRACKING
 * ========================================================================= */

/** Datapipe trigger for dsme availability
 *
 * @param data DSME D-Bus service availability (as a void pointer)
 */
static void mce_dsme_datapipe_dsme_service_state_cb(gconstpointer const data)
{
    service_state_t prev = dsme_service_state;
    dsme_service_state = GPOINTER_TO_INT(data);

    if( dsme_service_state == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "DSME dbus service: %s -> %s",
            service_state_repr(prev),
            service_state_repr(dsme_service_state));

    /* Re-evaluate dsmesock connection */
    if( dsme_service_state == SERVICE_STATE_RUNNING )
        mce_dsme_socket_connect();

EXIT:
    return;
}

/** Change notifications for init_done
 */
static void mce_dsme_datapipe_init_done_cb(gconstpointer data)
{
    tristate_t prev = init_done;
    init_done = GPOINTER_TO_INT(data);

    if( init_done == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "init_done = %s -> %s",
            tristate_repr(prev),
            tristate_repr(init_done));

    if( init_done == TRISTATE_TRUE ) {
        /* Remove transition submode after brief delay */
        mce_dsme_transition_schedule();
    }

EXIT:
    return;
}

/** Handle system_state_pipe notifications
 *
 * Implemented as an input trigger to ensure this function gets
 * executed before output triggers from other modules/plugins.
 *
 * @param data The system state (as a void pointer)
 */
static void mce_dsme_datapipe_system_state_cb(gconstpointer data)
{
    system_state_t prev = system_state;
    system_state = GPOINTER_TO_INT(data);

    if( system_state == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "system_state: %s -> %s",
            system_state_repr(prev),
            system_state_repr(system_state));

    /* Set transition submode unless coming from MCE_SYSTEM_STATE_UNDEF */
    if( prev != MCE_SYSTEM_STATE_UNDEF )
        mce_add_submode_int32(MCE_SUBMODE_TRANSITION);

    /* Handle LED patterns */
    switch( system_state ) {
    case MCE_SYSTEM_STATE_USER:
        datapipe_exec_output_triggers(&led_pattern_activate_pipe,
                                      MCE_LED_PATTERN_DEVICE_ON,
                                      DATAPIPE_USE_INDATA);
        break;

    case MCE_SYSTEM_STATE_SHUTDOWN:
    case MCE_SYSTEM_STATE_REBOOT:
        datapipe_exec_output_triggers(&led_pattern_deactivate_pipe,
                                      MCE_LED_PATTERN_DEVICE_ON,
                                      DATAPIPE_USE_INDATA);
        break;

    default:
        break;
    }

    /* Handle shutdown flag */
    switch( system_state ) {
    case MCE_SYSTEM_STATE_ACTDEAD:
    case MCE_SYSTEM_STATE_USER:
        /* Re-entry to actdead/user also means shutdown
         * has been cancelled */
        mce_dsme_set_shutting_down(false);
        break;

    case MCE_SYSTEM_STATE_SHUTDOWN:
    case MCE_SYSTEM_STATE_REBOOT:
        mce_dsme_set_shutting_down(true);
        break;

    default:
        break;
    }

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t mce_dsme_datapipe_handlers[] =
{
    // input triggers
    {
        .datapipe  = &system_state_pipe,
        .input_cb  = mce_dsme_datapipe_system_state_cb,
    },
    // output triggers
    {
        .datapipe  = &dsme_service_state_pipe,
        .output_cb = mce_dsme_datapipe_dsme_service_state_cb,
    },
    {
        .datapipe  = &init_done_pipe,
        .output_cb = mce_dsme_datapipe_init_done_cb,
    },
    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t mce_dsme_datapipe_bindings =
{
    .module   = "mce-dsme",
    .handlers = mce_dsme_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void mce_dsme_datapipe_init(void)
{
    mce_datapipe_init_bindings(&mce_dsme_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void mce_dsme_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&mce_dsme_datapipe_bindings);
}

/* ========================================================================= *
 * MODULE_INIT_EXIT
 * ========================================================================= */

/** Init function for the mce-dsme component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_dsme_init(void)
{
    mce_worker_add_context(MCE_DSME_WORKERWD_JOB_CONTEXT);

    mce_dsme_datapipe_init();

    mce_dsme_dbus_init();

    return TRUE;
}

/** Exit function for the mce-dsme component
 */
void mce_dsme_exit(void)
{
    mce_worker_rem_context(MCE_DSME_WORKERWD_JOB_CONTEXT);

    mce_dsme_dbus_quit();

    mce_dsme_socket_disconnect();

    mce_dsme_datapipe_quit();

    /* Remove all timer sources before returning */
    mce_dsme_transition_cancel();

    return;
}
