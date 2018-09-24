/**
 * @file mce-hbtimer.c
 *
 * Mode Control Entity - Suspend proof timer functionality
 *
 * <p>
 *
 * Copyright (C) 2014-2015 Jolla Ltd.
 *
 * <p>
 *
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

#include "mce-hbtimer.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-lib.h"

#ifdef ENABLE_WAKELOCKS
# include "libwakelock.h"
#endif

#include <sys/socket.h>

#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include <iphbd/libiphb.h>

/* ========================================================================= *
 * Types and functions
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * TIMER_METHODS
 * ------------------------------------------------------------------------- */

/** State data for mce heartbeat timers */
struct mce_hbtimer_t
{
    /** Timer name, used for debug logging purposes */
    char       *hbt_name;

    /** Trigger time, milliseconds in CLOCK_BOOTTIME base */
    int64_t     hbt_trigger;

    /** Timer callback function */
    GSourceFunc hbt_notify;

    /** Timer delay in milliseconds */
    int         hbt_period;

    /** Flag for: control within hbt_notify() */
    bool        hbt_in_notify;

    /** User data to pass to hbt_notify() */
    void       *hbt_user_data;
};

mce_hbtimer_t * mce_hbtimer_create         (const char *name, int period, GSourceFunc notify, void *user_data);
void            mce_hbtimer_delete         (mce_hbtimer_t *self);

bool            mce_hbtimer_is_active      (const mce_hbtimer_t *self);
const char     *mce_hbtimer_get_name       (const mce_hbtimer_t *self);
void            mce_hbtimer_set_period     (mce_hbtimer_t *self, int period);
void            mce_hbtimer_start          (mce_hbtimer_t *self);
void            mce_hbtimer_stop           (mce_hbtimer_t *self);

static void     mce_hbtimer_notify         (mce_hbtimer_t *self);
static void     mce_hbtimer_set_trigger    (mce_hbtimer_t *self, int64_t trigger);

/* ------------------------------------------------------------------------- *
 * GENERIC_UTILITIES
 * ------------------------------------------------------------------------- */

/** Monotonic tick value used to signify "not-set" */
#define NO_TICK INT64_MAX

static guint    mht_add_iowatch            (int fd, bool close_on_unref, GIOCondition cnd, GIOFunc io_cb, gpointer aptr);

/* ------------------------------------------------------------------------- *
 * QUEUE_MANAGEMENT
 * ------------------------------------------------------------------------- */

/** List of registered timers */
static GSList *mht_queue_timer_list = 0;

void            mht_queue_dispatch_timers  (void);
static void     mht_queue_schedule_wakeups (void);
static void     mht_queue_garbage_collect  (void);
static void     mht_queue_add_timer        (mce_hbtimer_t *self);
static void     mht_queue_remove_timer     (mce_hbtimer_t *self);
static bool     mht_queue_has_timer        (const mce_hbtimer_t *self);

/* ------------------------------------------------------------------------- *
 * GLIB_WAKEUPS
 * ------------------------------------------------------------------------- */

/** Source ID for currently active glib timer wakeup */
static guint mce_hbtimer_glib_wait_id = 0;

static gboolean mht_glib_wakeup_cb         (gpointer aptr);
static void     mht_glib_set_wakeup        (int64_t trigger, int64_t now);

/* ------------------------------------------------------------------------- *
 * IPHB_WAKEUPS
 * ------------------------------------------------------------------------- */

/** How much wakeups from suspend can be delayed [s]
 *
 * To increase changes of aligning with other iphb wakeups this
 * should be >= heartbeat period (=12 seconds) */
#define MHT_IPHB_WAKEUP_MAX_DELAY_S     12

/** Cached timestamp of last requested iphb wakeup */
static int64_t  mht_iphb_wakeup_tick = NO_TICK;

/** Source id for iphb wakeup input watch */
static guint   mht_iphb_wakeup_watch_id = 0;

static gboolean mht_iphb_wakeup_cb         (GIOChannel *chn, GIOCondition cnd, gpointer data);
static void     mht_iphb_set_wakeup        (int64_t trigger, int64_t now);

/* ------------------------------------------------------------------------- *
 * IPHB_CONNECTION
 * ------------------------------------------------------------------------- */

/** Number of times iphb connect is attempted after dsme startup */
#define MHT_CONNECTION_MAX_RETRIES    5

/** Delay between iphb connect attempts [ms] */
#define MHT_CONNECTION_RETRY_DELAY_MS 5000

/** Timer ID for: iphb connection attempts */
static guint mht_connection_timer_id = 0;

/** Number of connection attempts so far */
static gint  mht_connection_retry_no = 0;

/** Handle for iphb connection */
static iphb_t   mht_connection_handle = 0;

static bool     mht_connection_try_to_open (void);

static gboolean mht_connection_timer_cb    (gpointer aptr);
static bool     mht_connection_is_pending  (void);
static void     mht_connection_start_timer (void);
static void     mht_connection_stop_timer  (void);

static void     mht_connection_open        (void);
static void     mht_connection_close       (void);

/* ------------------------------------------------------------------------- *
 * DATAPIPE_HANDLERS
 * ------------------------------------------------------------------------- */

/** Availability of dsme; from dsme_service_state_pipe */
static service_state_t dsme_service_state = SERVICE_STATE_UNDEF;

/** Device is shutting down; assume false */
static bool shutting_down = false;

static void mht_datapipe_dsme_service_state_cb (gconstpointer data);
static void mht_datapipe_resume_detected_event_cb (gconstpointer data);
static void mht_datapipe_shutting_down_cb  (gconstpointer data);

static void mht_datapipe_init(void);
static void mht_datapipe_quit(void);

/* ------------------------------------------------------------------------- *
 * MODULE_INIT
 * ------------------------------------------------------------------------- */

/** Flag for: mce_hbtimer_init() has been called */
static bool     mce_hbtimer_initialized = false;

void            mce_hbtimer_init          (void);
void            mce_hbtimer_quit          (void);

/* ========================================================================= *
 * GENERIC_UTILITIES
 * ========================================================================= */

/** Helper for creating I/O watch for file descriptor
 */
static guint
mht_add_iowatch(int fd, bool close_on_unref,
                      GIOCondition cnd, GIOFunc io_cb, gpointer aptr)
{
    guint         wid = 0;
    GIOChannel   *chn = 0;

    if( !(chn = g_io_channel_unix_new(fd)) )
        goto cleanup;

    g_io_channel_set_close_on_unref(chn, close_on_unref);

    cnd |= G_IO_ERR | G_IO_HUP | G_IO_NVAL;

    if( !(wid = g_io_add_watch(chn, cnd, io_cb, aptr)) )
        goto cleanup;

cleanup:
    if( chn != 0 ) g_io_channel_unref(chn);

    return wid;

}

/* ========================================================================= *
 * TIMER_METHODS
 * ========================================================================= */

/** Create heatbeat timer
 *
 * @param name      timer name
 * @param period    timer delay [ms]
 * @param notify    function to be called when triggered
 * @param user_data pointer to be passed to notify function
 *
 * @return heartbeat timer object
 */
mce_hbtimer_t *
mce_hbtimer_create(const char  *name,
                   int          period,
                   GSourceFunc  notify,
                   void        *user_data)
{
    mce_hbtimer_t *self = calloc(1, sizeof *self);

    self->hbt_name      = name ? strdup(name) : 0;
    self->hbt_notify    = notify;
    self->hbt_period    = period;
    self->hbt_user_data = user_data;
    self->hbt_trigger   = NO_TICK;
    self->hbt_in_notify = false;

    mht_queue_add_timer(self);

    return self;
}

/** Delete heatbeat timer
 *
 * @param self heartbeat timer object, or NULL
 */
void
mce_hbtimer_delete(mce_hbtimer_t *self)
{
    if( !self )
        goto EXIT;

    mht_queue_remove_timer(self);
    mht_queue_schedule_wakeups();

    free(self->hbt_name),
        self->hbt_name = 0;

    free(self);

EXIT:
    return;
}

/** Predicate for: heatbeat timer has been started
 *
 * @param self heartbeat timer object, or NULL
 *
 * @return true if heartbeat timer has been started, false otherwise
 */
bool
mce_hbtimer_is_active(const mce_hbtimer_t *self)
{
    bool active = false;
    if( self )
        active = (self->hbt_trigger < NO_TICK);
    return active;
}

/** Get heatbeat timer name
 *
 * @param self heartbeat timer object, or NULL
 *
 * @return "invalid" if object is not valid,
 *         "unknown" if object has no name, or
 *         object name
 */

const char *
mce_hbtimer_get_name(const mce_hbtimer_t *self)
{
    const char *name = "invalid";
    if( self )
        name = self->hbt_name;
    return name ?: "unknown";
}

/** Set heatbeat timer period
 *
 * @param self   heartbeat timer object, or NULL
 * @param period timer delay [ms]
 */
void
mce_hbtimer_set_period(mce_hbtimer_t *self, int period)
{
    if( self )
        self->hbt_period = period;
}

/** Call heatbeat timer notification functiom
 *
 * @param self   heartbeat timer object, or NULL
 */
static void
mce_hbtimer_notify(mce_hbtimer_t *self)
{
    if( !self )
        goto EXIT;

    if( self->hbt_in_notify )
        goto EXIT;

    if( !self->hbt_notify )
        goto EXIT;

    self->hbt_in_notify = true;
    self->hbt_trigger   = NO_TICK;

    bool again = self->hbt_notify(self->hbt_user_data);

    /* Check that notify callback did not delete the timer */
    if( !mht_queue_has_timer(self) )
        goto EXIT;

    self->hbt_in_notify = false;

    if( again )
        mce_hbtimer_start(self);

EXIT:
    return;
}

/** Set heatbeat timer trigger time stamp
 *
 * @param self    heartbeat timer object, or NULL
 * @param trigger trigger time
 */

static void
mce_hbtimer_set_trigger(mce_hbtimer_t *self, int64_t trigger)
{
    if( !self )
        goto EXIT;

    if( self->hbt_trigger == trigger )
        goto EXIT;

    self->hbt_trigger = trigger;
    mht_queue_schedule_wakeups();

EXIT:
    return;
}

/** Start heatbeat timer
 *
 * @param self   heartbeat timer object, or NULL
 */
void
mce_hbtimer_start(mce_hbtimer_t *self)
{
    if( !self )
        goto EXIT;

    mce_log(LL_DEBUG, "start %s %d", mce_hbtimer_get_name(self),
            self->hbt_period);
    int64_t now = mce_lib_get_boot_tick();
    int64_t trigger = now + self->hbt_period;
    mce_hbtimer_set_trigger(self, trigger);

EXIT:
    return;
}

/** Stop heatbeat timer
 *
 * @param self   heartbeat timer object, or NULL
 */
void
mce_hbtimer_stop(mce_hbtimer_t *self)
{
    if( !self )
        goto EXIT;

    mce_log(LL_DEBUG, "stop %s", mce_hbtimer_get_name(self));
    mce_hbtimer_set_trigger(self, NO_TICK);
    mht_queue_schedule_wakeups();

EXIT:
    return;
}

/* ========================================================================= *
 * QUEUE_MANAGEMENT
 * ========================================================================= */

/** Clean up unused timer list slots
 *
 * When timers are deleted, the associated node in mht_queue_timer_list
 * is left in place with data pointer set to zero.
 *
 * This function can be used to remove such stale entries in suitable
 * point outside the dispatch iteration loop.
 */
static void
mht_queue_garbage_collect(void)
{
    GSList **tail = &mht_queue_timer_list;

    GSList *item;

    while( (item = *tail) ) {
        if( item->data ) {
            tail = &item->next;
        }
        else {
            *tail = item->next;
            item->next = 0;
            g_slist_free(item);
        }
    }
}

/** Predicate for: heartbeat timer is registered
 *
 * @param self   heartbeat timer object, or NULL
 *
 * return true if timer is registered, false otherwise
 */
static bool
mht_queue_has_timer(const mce_hbtimer_t *self)
{
    bool has_timer = false;

    if( !self )
        goto EXIT;

    has_timer = g_slist_find(mht_queue_timer_list, self) != 0;

EXIT:
    return has_timer;
}

/** Register heartbeat timer
 *
 * @param self   heartbeat timer object, or NULL
 */
static void
mht_queue_add_timer(mce_hbtimer_t *self)
{
    if( !self )
        goto EXIT;

    /* Try to find recyclable vacated timer slot */
    GSList *item = g_slist_find(mht_queue_timer_list, 0);

    if( item )
        item->data = self;
    else
        mht_queue_timer_list = g_slist_prepend(mht_queue_timer_list, self);

EXIT:
    return;
}

/** Unregister heartbeat timer
 *
 * @param self   heartbeat timer object, or NULL
 */
static void
mht_queue_remove_timer(mce_hbtimer_t *self)
{
    if( !self )
        goto EXIT;

    GSList *item = g_slist_find(mht_queue_timer_list, self);

    /* Vacate the slot by clearing the data link */
    if( item )
        item->data = 0;

EXIT:
    return;
}

/** Scan registered heartbeat timers and schdule nearest wakeup
 */
static void
mht_queue_schedule_wakeups(void)
{
    if( !mce_hbtimer_initialized )
        goto EXIT;

    int64_t trigger = NO_TICK;
    bool    compact = false;

    for( GSList *item = mht_queue_timer_list; item; item = item->next ) {
        mce_hbtimer_t *timer = item->data;

        if( !timer )
            compact = true;
        else if( trigger > timer->hbt_trigger )
            trigger = timer->hbt_trigger;
    }

    if( compact )
        mht_queue_garbage_collect();

    int64_t now = mce_lib_get_boot_tick();

    if( trigger < now )
        trigger = now;

    mht_glib_set_wakeup(trigger, now);
    mht_iphb_set_wakeup(trigger, now);

EXIT:
    return;
}

/** Scan registered heartbeat timers and notify triggered ones
 */
void
mht_queue_dispatch_timers(void)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

    if( !mce_hbtimer_initialized )
        goto EXIT;

    /* We need to be sure that actions resulting from
     * timer notifications do not cause recursive dispatching
     * to take place. */

    if( pthread_mutex_trylock(&mutex) != 0 )
        goto EXIT;

    /* Block suspend during dispatching */
#ifdef ENABLE_WAKELOCKS
    wakelock_lock("mce_hbtimer_dispatch", -1);
#endif

    int64_t now = mce_lib_get_boot_tick();

    for( GSList *item = mht_queue_timer_list; item; item = item->next ) {
        mce_hbtimer_t *timer = item->data;

        if( !timer )
            continue;

        if( timer->hbt_trigger == NO_TICK )
            continue;

        mce_log(LL_DEBUG, "%s T%+"PRId64" ms",
                mce_hbtimer_get_name(timer),
                now - timer->hbt_trigger);

        if( now < timer->hbt_trigger )
            continue;

        mce_hbtimer_notify(timer);
    }

    /* Check the next timer to trigger */
    mht_queue_schedule_wakeups();

#ifdef ENABLE_WAKELOCKS
    wakelock_unlock("mce_hbtimer_dispatch");
#endif

    pthread_mutex_unlock(&mutex);

EXIT:
    return;
}

/* ========================================================================= *
 * GLIB_WAKEUPS
 * ========================================================================= */

/** Glib timeout callback for dispatching heartbeat timers
 *
 * @param aptr (not used)
 *
 * @return FALSE to stop timeout from repeating
 */
static gboolean
mht_glib_wakeup_cb(gpointer aptr)
{
    (void) aptr;

    if( !mce_hbtimer_glib_wait_id )
        goto EXIT;

    mce_hbtimer_glib_wait_id = 0;

    mce_log(LL_DEBUG, "glib wakeup; dispatch hbtimers");
    mht_queue_dispatch_timers();

EXIT:
    return FALSE;
}

/** Reprogram glib timeout for dispatching heartbeat timers
 *
 * @param trigger when to trigger
 * @param now     current time
 */
static void
mht_glib_set_wakeup(int64_t trigger, int64_t now)
{
    static int64_t prev = NO_TICK;

    int delay = -1;

    /* The glib timers use CLOCK_MONOTONIC while we need to evaluate
     * triggering in CLOCK_BOOTTIME.
     *
     * Assume glib reprogramming is cheap enough so that we do not need
     * to try to avoid it.
     */

    if( mce_hbtimer_glib_wait_id ) {
        g_source_remove(mce_hbtimer_glib_wait_id),
            mce_hbtimer_glib_wait_id = 0;
    }
    if( trigger != NO_TICK ) {
        delay = (int)(trigger - now);
        mce_hbtimer_glib_wait_id =
            g_timeout_add(delay, mht_glib_wakeup_cb, 0);
    }

    if( prev != trigger ) {
        prev = trigger;
        mce_log(LL_DEBUG, "glib wakeup in %d ms", delay);
    }
}

/* ========================================================================= *
 * IPHB_WAKEUPS
 * ========================================================================= */

/** iphb wakeup callback for dispatching heartbeat timers
 *
 * @param chn  io channel
 * @param cnd  io condition
 * @param data (unused)
 *
 * @return TRUE to keep io watch alive, or FALSE to disable it
 */
static gboolean
mht_iphb_wakeup_cb(GIOChannel *chn, GIOCondition cnd, gpointer data)
{
    (void)data;

    gboolean keep_going = FALSE;

    if( !mht_iphb_wakeup_watch_id )
        goto cleanup_nak;

    int fd = g_io_channel_unix_get_fd(chn);

    if( fd < 0 )
        goto cleanup_nak;

    if( cnd & ~G_IO_IN )
        goto cleanup_nak;

    if( !(cnd & G_IO_IN) )
        goto cleanup_ack;

    char buf[256];

    int rc = recv(fd, buf, sizeof buf, MSG_DONTWAIT);

    if( rc == 0 ) {
        if( !shutting_down )
            mce_log(LL_ERR, "unexpected eof");
        goto cleanup_nak;
    }

    if( rc == -1 ) {
        if( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK )
            goto cleanup_ack;

        mce_log(LL_ERR, "read error: %m");
        goto cleanup_nak;
    }

    if( mht_iphb_wakeup_tick == NO_TICK )
        goto cleanup_ack;

    /* clear programmed state */
    mht_iphb_wakeup_tick = NO_TICK;

    /* notify */
    mce_log(LL_DEBUG, "iphb wakeup; dispatch hbtimers");
    mht_queue_dispatch_timers();

cleanup_ack:
    keep_going = TRUE;

cleanup_nak:

    if( !keep_going ) {
        mht_iphb_wakeup_watch_id = 0;
        mht_connection_close();
    }

    return keep_going;
}

/** Reprogram iphb timeout for dispatching heartbeat timers
 *
 * @param trigger when to trigger
 * @param now     current time
 */
static void
mht_iphb_set_wakeup(int64_t trigger, int64_t now)
{
    /* Assume: iphb timer should be stopped */
    int lo = 0;
    int hi = 0;
    int64_t tick = NO_TICK;

    if( mht_connection_handle && trigger != NO_TICK) {
        /* Calculate the iphb wakeup range to be used. */
        int64_t delay = (trigger - now + 999) / 1000;

        lo = (int)delay;
        hi = lo + MHT_IPHB_WAKEUP_MAX_DELAY_S;

        /* Calculate the next full BOOTTIME second after low bound
         * of iphb wakeup. This is used for avoiding constant iphb
         * ipc when wakeups get re-evaluated.
         *
         * There might be up to one second jitter in actual wakeup
         * vs cached time stamp, but it does not matter since:
         * 1) the wide iphb range means the wake ups from suspend
         *    are going to be off by several seconds on average
         *    anyway
         * 2) if the device does not get suspended, the higher
         *    resolution glib timeout gets triggered on time
         */

        tick = now + delay * 1000;
        tick += 999;
        tick -= tick % 1000;
    }

    if( mht_iphb_wakeup_tick != tick ) {
        mht_iphb_wakeup_tick = tick;

        if( mht_connection_handle )
            iphb_wait2(mht_connection_handle, lo, hi, 0, 1);

        mce_log(LL_DEBUG, "iphb wakeup in [%d, %d] s", lo, hi);
    }
}

/* ========================================================================= *
 * IPHB_CONNECTION
 * ========================================================================= */

/** Try to establish iphb socket connection
 *
 * Note: This is a helper function meant to be called only from
 *       mht_connection_open() and mht_connection_timer_cb()
 *       functions.
 *
 * @return true if connection is made, false otherwise
 */
static bool
mht_connection_try_to_open(void)
{
    iphb_t handle = 0;

    if( mht_connection_handle )
        goto cleanup;

    if( !(handle = iphb_open(0)) ) {
        mce_log(LL_WARN, "iphb_open: %m");
        goto cleanup;
    }

    int fd = iphb_get_fd(handle);
    if( fd == -1 ) {
        mce_log(LL_WARN, "iphb_get_fd: %m");
        goto cleanup;
    }

    /* set up io watch */
    mht_iphb_wakeup_watch_id =
        mht_add_iowatch(fd, false, G_IO_IN, mht_iphb_wakeup_cb, 0);

    if( !mht_iphb_wakeup_watch_id )
        goto cleanup;

    /* cache the handle */
    mht_connection_handle = handle, handle = 0;

    mce_log(LL_DEBUG, "iphb connected; dispatch hbtimers");
    mht_queue_dispatch_timers();

cleanup:

    if( handle ) iphb_close(handle);

    return mht_connection_handle != 0;
}

/** Callback for connect reattempt timer
 *
 * @param aptr  (unused)
 *
 * @return TRUE to keep timer repeating, or FALSE to stop it
 */
static gboolean
mht_connection_timer_cb(gpointer aptr)
{
    (void)aptr;

    if( !mht_connection_timer_id )
        goto cleanup;

    ++mht_connection_retry_no;

    if( !mht_connection_try_to_open() ) {
        if( mht_connection_retry_no < MHT_CONNECTION_MAX_RETRIES )
            goto cleanup;

        mce_log(LL_WARN, "connect failed %d times; giving up",
                mht_connection_retry_no);
    }
    else {
        mce_log(LL_DEBUG, "connected after %d retries",
                mht_connection_retry_no);
    }

    mht_connection_timer_id = 0;

cleanup:

    return mht_connection_timer_id != 0;
}

/** Start connect reattempt timer
 */
static void
mht_connection_start_timer(void)
{
    if( !mht_connection_timer_id && mce_hbtimer_initialized ) {
        mht_connection_retry_no = 0;
        mht_connection_timer_id =
            g_timeout_add(MHT_CONNECTION_RETRY_DELAY_MS,
                          mht_connection_timer_cb, 0);
    }
}

/** Cancel connect reattempt timer
 */
static void
mht_connection_stop_timer(void)
{
    if( mht_connection_timer_id ) {
        g_source_remove(mht_connection_timer_id),
            mht_connection_timer_id = 0;
    }
}

/** Predicate for: Connect reattempt timer is active
 */
static bool
mht_connection_is_pending(void)
{
    return mht_connection_timer_id != 0;
}

/** Start connecting to iphb socket
 *
 * If the connection cant be made immediately it
 * will be reattempted few times via timer callback.
 */
static void
mht_connection_open(void)
{
    if( mht_connection_is_pending() ) {
        // Retry timer already set up
    }
    else if( !mht_connection_try_to_open() ) {
        // Could not connect now - start retry timer
        mht_connection_start_timer();
    }
}

/** Close connection to iphb socket
 */
static void
mht_connection_close(void)
{
    /* stop pending connect attempts */
    mht_connection_stop_timer();

    /* remove io watch */
    if( mht_iphb_wakeup_watch_id ) {
        g_source_remove(mht_iphb_wakeup_watch_id),
            mht_iphb_wakeup_watch_id = 0;
    }

    /* close handle */
    if( mht_connection_handle ) {
        iphb_close(mht_connection_handle),
            mht_connection_handle = 0;

        mce_log(LL_DEBUG, "iphb disconnected");

        /* reset last programmed wakeup */
        mht_iphb_set_wakeup(NO_TICK, NO_TICK);
    }
}

/* ========================================================================= *
 * DATAPIPE_HANDLERS
 * ========================================================================= */

/** Resumed from suspend notification */
static void mht_datapipe_resume_detected_event_cb(gconstpointer data)
{
    (void) data;

    /* Check triggers & update glib wakeup after resume */
    mce_log(LL_DEBUG, "resumed; dispatch hbtimers");
    mht_queue_dispatch_timers();
}

/** Datapipe trigger for dsme availability
 */
static void mht_datapipe_dsme_service_state_cb(gconstpointer const data)
{
    service_state_t prev = dsme_service_state;
    dsme_service_state = GPOINTER_TO_INT(data);

    if( dsme_service_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "DSME dbus service: %s -> %s",
            service_state_repr(prev),
            service_state_repr(dsme_service_state));

    if( dsme_service_state == SERVICE_STATE_RUNNING )
        mht_connection_open();
    else
        mht_connection_close();

EXIT:
    return;
}

/** Change notifications for shutting_down
 */
static void mht_datapipe_shutting_down_cb(gconstpointer data)
{
    bool prev = shutting_down;
    shutting_down = GPOINTER_TO_INT(data);

    if( shutting_down == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "shutting_down = %d -> %d",
            prev, shutting_down);

    /* Loss of iphb connection is expected during shutdown */

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t mht_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &resume_detected_event_pipe,
        .output_cb = mht_datapipe_resume_detected_event_cb,
    },
    {
        .datapipe  = &dsme_service_state_pipe,
        .output_cb = mht_datapipe_dsme_service_state_cb,
    },
    {
        .datapipe  = &shutting_down_pipe,
        .output_cb = mht_datapipe_shutting_down_cb,
    },

    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t mht_datapipe_bindings =
{
    .module   = "mce_hbtimer",
    .handlers = mht_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void mht_datapipe_init(void)
{
    mce_datapipe_init_bindings(&mht_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void mht_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&mht_datapipe_bindings);
}

/* ========================================================================= *
 * MODULE_INIT
 * ========================================================================= */

void
mce_hbtimer_init(void)
{
    /* Connect to datapipes */
    mht_datapipe_init();

    /* Mark as initialized */
    mce_hbtimer_initialized = true;

    /* Schedule timers that have been created
     * before initialization took place */
    mht_queue_schedule_wakeups();
}

void
mce_hbtimer_quit(void)
{
    /* Mark as de-initialized */
    mce_hbtimer_initialized = false;

    /* Disconnect from datapipes */
    mht_datapipe_quit();

    /* Remove wakeups */
    mht_glib_set_wakeup(NO_TICK, NO_TICK);
    mht_iphb_set_wakeup(NO_TICK, NO_TICK);

    /* close iphb connection */
    mht_connection_close();
}
