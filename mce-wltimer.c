/**
 * @file mce-wltimer.c
 *
 * Mode Control Entity - Timers that block suspend until triggered
 *
 * <p>
 *
 * Copyright (c) 2015 - 2023 Jolla Ltd.
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

#include "mce-wltimer.h"

#include "mce-log.h"
#include "mce-wakelock.h"

#include <stdlib.h>
#include <string.h>

/* ========================================================================= *
 * Types and functions
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * TIMER_METHODS
 * ------------------------------------------------------------------------- */

/** State data for mce wakelock timers */
struct mce_wltimer_t
{
    /** Timer name, used as wakelock name too */
    char       *wlt_name;

    /** Timer delay in milliseconds */
    int         wlt_period;

    /** Underlying glib timeout id */
    guint       wlt_timer_id;

    /** Timer callback function */
    GSourceFunc wlt_notify;

    /** User data to pass to wlt_notify() */
    void       *wlt_user_data;

    /** Currently handling notify */
    bool        wlt_triggered;

    /** Timer start while in notify */
    bool        wlt_started;

    /** Timer stop while in notify */
    bool        wlt_stopped;
};

mce_wltimer_t * mce_wltimer_create         (const char *name, int period, GSourceFunc notify, void *user_data);
void            mce_wltimer_delete         (mce_wltimer_t *self);

static void     mce_wltimer_eval_wakelock  (const mce_wltimer_t *self);

bool            mce_wltimer_is_active      (const mce_wltimer_t *self);
const char     *mce_wltimer_get_name       (const mce_wltimer_t *self);
void            mce_wltimer_set_period     (mce_wltimer_t *self, int period);
void            mce_wltimer_start          (mce_wltimer_t *self);
void            mce_wltimer_stop           (mce_wltimer_t *self);

static gboolean mce_wltimer_gate_cb        (gpointer aptr);

/* ------------------------------------------------------------------------- *
 * QUEUE_MANAGEMENT
 * ------------------------------------------------------------------------- */

/** Idle callback id for delayed garbage collect */
static guint mwt_queue_compact_id = 0;

/** List of registered timers */
static GSList *mwt_queue_timer_list = 0;

static void     mwt_queue_compact          (void);
static gboolean mwt_queue_compact_cb       (gpointer aptr);
static void     mwt_queue_schedule_compact (void);
static void     mwt_queue_cancel_compact   (void);

static void     mwt_queue_add_timer        (mce_wltimer_t *self);
static void     mwt_queue_remove_timer     (mce_wltimer_t *self);
static bool     mwt_queue_has_timer        (const mce_wltimer_t *self);

/* ------------------------------------------------------------------------- *
 * MODULE_INIT
 * ------------------------------------------------------------------------- */

/** Flag for: timers can be started */
static bool     mce_wltimer_ready = true;

void            mce_wltimer_init          (void);
void            mce_wltimer_quit          (void);

/* ========================================================================= *
 * TIMER_METHODS
 * ========================================================================= */

/** Create wakelock timer
 *
 * @param name      timer name
 * @param period    timer delay [ms]
 * @param notify    function to be called when triggered
 * @param user_data pointer to be passed to notify function
 *
 * @return wakelock timer object
 */
mce_wltimer_t *
mce_wltimer_create(const char  *name,
                   int          period,
                   GSourceFunc  notify,
                   void        *user_data)
{
    mce_wltimer_t *self = calloc(1, sizeof *self);

    self->wlt_name      = name ? strdup(name) : 0;
    self->wlt_period    = period;
    self->wlt_timer_id  = 0;
    self->wlt_notify    = notify;
    self->wlt_user_data = user_data;
    self->wlt_triggered = false;
    self->wlt_started   = false;
    self->wlt_stopped   = false;

    mwt_queue_add_timer(self);

    return self;
}

/** Delete wakelock timer
 *
 * @param self wakelock timer object, or NULL
 */
void
mce_wltimer_delete(mce_wltimer_t *self)
{
    if( !self )
        goto EXIT;

    if( self->wlt_triggered )
        mce_log(LL_DEBUG, "%s: timer delete while in notify",
                mce_wltimer_get_name(self));

    /* Clear the behaviour modifying flags so that the timer id
     * and wakelock does get released at mce_wltimer_stop().
     *
     * Note that mwt_queue_remove_timer() invalidates
     * timer object so that mce_wltimer_gate_cb() knows
     * not to touch it anymore when user callback returns.
     */
    self->wlt_triggered = false;
    self->wlt_started   = false;
    self->wlt_stopped   = false;

    mce_wltimer_stop(self);
    mwt_queue_remove_timer(self);

    free(self->wlt_name),
        self->wlt_name = 0;

    free(self);

EXIT:
    return;
}

/** Evaluate need for wakelock
 *
 * @param self   wakelock timer object, or NULL
 */
static void
mce_wltimer_eval_wakelock(const mce_wltimer_t *self)
{
    if( !self )
        goto EXIT;

    if( !self->wlt_name)
        goto EXIT;

    if( self->wlt_timer_id )
        mce_wakelock_obtain(self->wlt_name, -1);
    else
        mce_wakelock_release(self->wlt_name);

EXIT:
    return;
}

/** Predicate for: wakelock timer has been started
 *
 * @param self wakelock timer object, or NULL
 *
 * @return true if wakelock timer has been started, false otherwise
 */
bool
mce_wltimer_is_active(const mce_wltimer_t *self)
{
    bool active = false;
    if( self && self->wlt_timer_id ) {
        active = !(self->wlt_triggered && self->wlt_stopped);
    }
    return active;
}

/** Get wakelock timer name
 *
 * @param self wakelock timer object, or NULL
 *
 * @return "invalid" if object is not valid,
 *         "unknown" if object has no name, or
 *         object name
 */

const char *
mce_wltimer_get_name(const mce_wltimer_t *self)
{
    const char *name = "invalid";
    if( self )
        name = self->wlt_name;
    return name ?: "unknown";
}

/** Set wakelock timer period
 *
 * @param self   wakelock timer object, or NULL
 * @param period timer delay [ms]
 */
void
mce_wltimer_set_period(mce_wltimer_t *self, int period)
{
    if( self )
        self->wlt_period = period;
}

/** Call wakelock timer notification functiom
 *
 * @param aptr   wakelock timer object (as void pointer)
 */
static gboolean
mce_wltimer_gate_cb(gpointer aptr)
{
    gboolean repeat = FALSE;

    mce_wltimer_t *self = aptr;

    if( !self->wlt_timer_id )
        goto EXIT;

    mce_log(LL_DEBUG, "trigger %s %d", mce_wltimer_get_name(self),
            self->wlt_period);

    if( self->wlt_notify ) {
        self->wlt_triggered = true;

        bool res = self->wlt_notify(self->wlt_user_data);

        if( !mwt_queue_has_timer(self) ) {
            /* The notify callback managed to delete the timer
             * object, invalidate the pointer */
            self = 0;
        }
        else {
            if( self->wlt_started ) {
                mce_log(LL_DEBUG, "%s: timer was started while in notify",
                        mce_wltimer_get_name(self));
                repeat = true;
            }
            else if( self->wlt_stopped ) {
                mce_log(LL_DEBUG, "%s: timer was stopped while in notify",
                        mce_wltimer_get_name(self));
                repeat = false;
            }
            else {
                /* Repeat/stop according to the callback return value */
                repeat = res;
            }
            self->wlt_started   = false;
            self->wlt_stopped   = false;
            self->wlt_triggered = false;
        }
    }

EXIT:

    if( self ) {
        if( !repeat &&  self->wlt_timer_id )
            self->wlt_timer_id = 0;
        mce_wltimer_eval_wakelock(self);
    }

    return repeat;
}

/** Start wakelock timer
 *
 * @param self   wakelock timer object, or NULL
 */
void
mce_wltimer_start(mce_wltimer_t *self)
{
    if( !self )
        goto EXIT;

    if( self->wlt_triggered ) {
        /* In notify - Keep alive after callback returns */
        mce_log(LL_DEBUG, "%s: timer start while in notify",
                mce_wltimer_get_name(self));
        self->wlt_started = true;
        self->wlt_stopped = false;
        goto EXIT;
    }

    if( !mce_wltimer_ready )
        goto EXIT;

    if( self->wlt_period < 0 )
        goto EXIT;

    if( self->wlt_timer_id )
        goto EXIT;

    mce_log(LL_DEBUG, "start %s %d", mce_wltimer_get_name(self),
            self->wlt_period);

    if( self->wlt_period > 0 ) {
        self->wlt_timer_id = g_timeout_add(self->wlt_period,
                                           mce_wltimer_gate_cb,
                                           self);
    }
    else {
        self->wlt_timer_id = g_idle_add(mce_wltimer_gate_cb,
                                        self);
    }

EXIT:
    mce_wltimer_eval_wakelock(self);
    return;
}

/** Stop wakelock timer
 *
 * @param self   wakelock timer object, or NULL
 */
void
mce_wltimer_stop(mce_wltimer_t *self)
{
    if( !self )
        goto EXIT;

    if( self->wlt_triggered ) {
        /* In notify - Stop after callback returns */
        mce_log(LL_DEBUG, "%s: timer stop while in notify",
                mce_wltimer_get_name(self));
        self->wlt_started = false;
        self->wlt_stopped = true;
        goto EXIT;
    }

    if( !self->wlt_timer_id )
        goto EXIT;

    mce_log(LL_DEBUG, "stop %s", mce_wltimer_get_name(self));

    g_source_remove(self->wlt_timer_id),
        self->wlt_timer_id = 0;

EXIT:
    mce_wltimer_eval_wakelock(self);

    return;
}

/* ========================================================================= *
 * QUEUE_MANAGEMENT
 * ========================================================================= */

/** Clean up unused timer list slots
 *
 * When timers are deleted, the associated node in mwt_queue_timer_list
 * is left in place with data pointer set to zero.
 *
 * This function can be used to remove such stale entries in suitable
 * point outside the dispatch iteration loop.
 */
static void
mwt_queue_compact(void)
{
    mwt_queue_cancel_compact();

    GSList **tail = &mwt_queue_timer_list;

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

/** Idle callback function for delayed garbage collect
 *
 * @param aptr user data pointer (unused)
 *
 * @return FALSE, to stop repeats
 */
static gboolean
mwt_queue_compact_cb(gpointer aptr)
{
    (void)aptr;

    if( !mwt_queue_compact_id )
        goto EXIT;

    mwt_queue_compact_id = 0;

    mwt_queue_compact();

EXIT:
    return FALSE;
}

/** Schedule delayed garbage collection
 */
static void mwt_queue_schedule_compact(void)
{
    if( mwt_queue_compact_id )
        goto EXIT;

    mwt_queue_compact_id = g_idle_add(mwt_queue_compact_cb, 0);

EXIT:
    return;
}

/** Cancel delayed garbage collection
 */
static void mwt_queue_cancel_compact(void)
{
    if( !mwt_queue_compact_id )
        goto EXIT;

    g_source_remove(mwt_queue_compact_id),
        mwt_queue_compact_id = 0;

EXIT:
    return;
}

/** Predicate for: wakelock timer is registered
 *
 * @param self   wakelock timer object, or NULL
 *
 * return true if timer is registered, false otherwise
 */
static bool
mwt_queue_has_timer(const mce_wltimer_t *self)
{
    bool has_timer = false;

    if( !self )
        goto EXIT;

    has_timer = g_slist_find(mwt_queue_timer_list, self) != 0;

EXIT:
    return has_timer;
}

/** Register wakelock timer
 *
 * @param self   wakelock timer object, or NULL
 */
static void
mwt_queue_add_timer(mce_wltimer_t *self)
{
    if( !self )
        goto EXIT;

    /* Try to find recyclable vacated timer slot */
    GSList *item = g_slist_find(mwt_queue_timer_list, 0);

    if( item )
        item->data = self;
    else
        mwt_queue_timer_list = g_slist_prepend(mwt_queue_timer_list, self);

EXIT:
    return;
}

/** Unregister wakelock timer
 *
 * @param self   wakelock timer object, or NULL
 */
static void
mwt_queue_remove_timer(mce_wltimer_t *self)
{
    if( !self )
        goto EXIT;

    GSList *item = g_slist_find(mwt_queue_timer_list, self);

    /* Vacate the slot by clearing the data link */
    if( item )
        item->data = 0;

    /* Schedule removal of the link it self */
    mwt_queue_schedule_compact();

EXIT:
    return;
}

/* ========================================================================= *
 * MODULE_INIT
 * ========================================================================= */

void
mce_wltimer_init(void)
{
    /* nop */
}

void
mce_wltimer_quit(void)
{
    mce_log(LL_DEBUG, "deny suspend block timers");

    /* Deny starting of timers */
    mce_wltimer_ready = false;

    /* Disable left-behind timer objects */
    for( GSList *item = mwt_queue_timer_list; item; item = item->next ) {
        mce_wltimer_t *timer = item->data;
        if( !timer )
            continue;

        /* Note: What we have here is effectively a resource leak
         *       somewhere else. But all that can be done is to make
         *       sure we do not leave behind active timeouts that
         *       might then trigger callbacks in unexpected manner.
         */

        mce_log(LL_WARN, "timer '%s' exists at deinit",
               mce_wltimer_get_name(timer));

        mce_wltimer_stop(timer);
        item->data = 0;
    }

    /* Flush timer list */
    mwt_queue_compact();

}
