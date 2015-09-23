/**
 * @file mce-wakelock.c
 * Wakelock multiplexing code for the Mode Control Entity
 * <p>
 * Copyright (C) 2015 Jolla Ltd.
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

#include "mce-wakelock.h"
#include "mce-log.h"

#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <glib.h>

/* ========================================================================= *
 * FUNCTIONS & DATA
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * SYSFS_API
 * ------------------------------------------------------------------------- */

/** Path to kernel wakelock obtain sysfs file */
static const char mwl_sysfs_lock_path[] = "/sys/power/wake_lock";

/** Path to kernel wakelock release sysfs file */
static const char mwl_sysfs_unlock_path[] = "/sys/power/wake_unlock";

static bool mwl_sysfs_write(const char *path, const char *data, int size);

/* ------------------------------------------------------------------------- *
 * RAWLOCK_API
 * ------------------------------------------------------------------------- */

/** Name of the multiplexed "real" wakelock */
static const char mwl_rawlock_name[] = "mce_mux";

/** Flag for: "real" wakelock is held */
static bool mce_rawlock_locked = false;

static bool mwl_rawlock_supported (void);
static bool mwl_rawlock_lock      (void);
static bool mwl_rawlock_unlock    (void);
static void mwl_rawlock_set       (bool lock);

/* ------------------------------------------------------------------------- *
 * mwl_wakelock_t
 * ------------------------------------------------------------------------- */

/** Virtual wakelock object */
typedef struct mwl_wakelock_t
{
    /** Name of the virtual wakelock */
    gchar *wl_name;

    /** Release timer id */
    guint  wl_timer_id;
} mwl_wakelock_t;

static gboolean        mwl_wakelock_timer_cb    (gpointer aptr);
static void            mwl_wakelock_start_timer (mwl_wakelock_t *self, int delay_ms);
static void            mwl_wakelock_stop_timer  (mwl_wakelock_t *self);

static mwl_wakelock_t *mwl_wakelock_create      (const char *name);
static void            mwl_wakelock_delete      (mwl_wakelock_t *self);
static void            mwl_wakelock_delete_cb   (void *self);

/* ------------------------------------------------------------------------- *
 * MODULE_API
 * ------------------------------------------------------------------------- */

/** Flag for: wakelock module is ready for use */
static bool mce_wakelock_ready  = false;

/** Lookup table for tracked wakelock objects */
static GHashTable *mce_wakelock_lut = 0; // [name] -> mwl_wakelock_t *

static mwl_wakelock_t *mce_wakelock_add_entry    (const char *name);
static void            mce_wakelock_rem_entry    (const char *name);
static bool            mce_wakelock_have_entries (void);

void                   mce_wakelock_obtain       (const char *name, int duration_ms);
void                   mce_wakelock_release      (const char *name);

void                   mce_wakelock_init         (void);
void                   mce_wakelock_quit         (void);
void                   mce_wakelock_abort        (void);

/* ========================================================================= *
 * SYSFS_API
 * ========================================================================= */

/** Helper for writing to sysfs files
 */
static bool
mwl_sysfs_write(const char *path, const char *data, int size)
{
    bool res = false;
    int  fd  = -1;

    if( !path || !data || size <= 0 )
        goto cleanup;

    if( (fd = open(path, O_WRONLY)) == -1 )
        goto cleanup;

    if( write(fd, data, size) == -1 )
        goto cleanup;

    res = true;

cleanup:
    if( fd != -1 ) close(fd);

    return res;
}

/* ========================================================================= *
 * RAWLOCK_API
 * ========================================================================= */

/** Predicate for: wakelock sysfs control files exist
 */
static bool
mwl_rawlock_supported(void)
{
    return (access(mwl_sysfs_lock_path, W_OK) == 0 &&
            access(mwl_sysfs_unlock_path, W_OK) == 0);
}

/** Async signal safe wakelock obtain
 */
static bool
mwl_rawlock_lock(void)
{
    return mwl_sysfs_write(mwl_sysfs_lock_path, mwl_rawlock_name,
                           sizeof mwl_rawlock_name - 1);
}

/** Async signal safe wakelock release
 */
static bool
mwl_rawlock_unlock(void)
{
    return mwl_sysfs_write(mwl_sysfs_unlock_path, mwl_rawlock_name,
                           sizeof mwl_rawlock_name - 1);
}

/** Set wakelock state
 *
 * @param lock  true to obtain real wakelock, false to release
 */
static void
mwl_rawlock_set(bool lock)
{
    if( mce_rawlock_locked == lock )
        goto EXIT;

    mce_log(LL_DEBUG, "wakelock %s", lock ? "obtain" : "release");

    errno = 0;

    if( (mce_rawlock_locked = lock) ) {
        if( !mwl_rawlock_lock() )
            mce_log(LL_ERR, "failed to obtain wakelock: %m");
    }
    else {
        if( !mwl_rawlock_unlock() )
            mce_log(LL_ERR, "failed to release wakelock: %m");
    }

EXIT:
    return;
}

/* ========================================================================= *
 * mwl_wakelock_t
 * ========================================================================= */

/** Timer callback for releasing virtual wakelock
 *
 * @param aptr wakelock object pointer
 */
static gboolean
mwl_wakelock_timer_cb(gpointer aptr)
{
    mwl_wakelock_t *self = aptr;

    if( !self )
        goto EXIT;

    if( !self->wl_timer_id )
        goto EXIT;

    self->wl_timer_id = 0;

    mce_wakelock_release(self->wl_name);

EXIT:
    return FALSE;
}

/** Start virtual wakelock object release timer
 *
 * @param self      wakelock object pointer, or NULL
 * @param delay_ms  delay before releasing wakelock
 */
static void
mwl_wakelock_start_timer(mwl_wakelock_t *self, int delay_ms)
{
    if( !self )
        goto EXIT;

    if( self->wl_timer_id ) {
        g_source_remove(self->wl_timer_id),
            self->wl_timer_id = 0;
    }

    if( delay_ms < 0 )
        goto EXIT;

    if( delay_ms > 0 ) {
        self->wl_timer_id = g_timeout_add(delay_ms,
                                          mwl_wakelock_timer_cb,
                                          self);
    }
    else {
        self->wl_timer_id = g_idle_add(mwl_wakelock_timer_cb, self);
    }

EXIT:
    return;
}

/** Stop virtual wakelock object release timer
 *
 * @param self wakelock object pointer, or NULL
 */
static void
mwl_wakelock_stop_timer(mwl_wakelock_t *self)
{
    if( !self )
        goto EXIT;

    if( self->wl_timer_id ) {
        g_source_remove(self->wl_timer_id),
            self->wl_timer_id = 0;
    }

EXIT:
    return;
}

/** Create virtual wakelock object
 *
 * @param name Name of wakelock object to create
 *
 * @return wakelock object pointer
 */
static mwl_wakelock_t *
mwl_wakelock_create(const char *name)
{
    mwl_wakelock_t *self = g_malloc0(sizeof *self);

    self->wl_name     = g_strdup(name);
    self->wl_timer_id = 0;

    mce_log(LL_DEBUG, "wakelock %s obtain (mux)", self->wl_name);

    return self;
}

/** Delete virtual wakelock object
 *
 * @param self wakelock object pointer, or NULL
 */
static void
mwl_wakelock_delete(mwl_wakelock_t *self)
{
    if( !self )
        goto EXIT;

    mwl_wakelock_stop_timer(self);

    mce_log(LL_DEBUG, "wakelock %s release (mux)", self->wl_name);

    g_free(self->wl_name);
    g_free(self);

EXIT:
    return;
}

/** GDestroyNotify compatible delete callback
 *
 * @param aptr wakelock object pointer (as void pointer), or NULL
 */
static void
mwl_wakelock_delete_cb(void *aptr)
{
    mwl_wakelock_delete(aptr);
}

/* ========================================================================= *
 * MODULE_API
 * ========================================================================= */

/** Lookup or create a wakelock object by name
 *
 * @param name  Name of the virtual wakelock
 *
 * @return object pointer, or NULL on errors
 */
static mwl_wakelock_t *
mce_wakelock_add_entry(const char *name)
{
    mwl_wakelock_t *self = 0;

    if( !mce_wakelock_lut )
        goto EXIT;

    if( !(self = g_hash_table_lookup(mce_wakelock_lut, name)) ) {
        self = mwl_wakelock_create(name);
        g_hash_table_replace(mce_wakelock_lut, g_strdup(name), self);
    }

EXIT:
    return self;
}

/** Remove a wakelock object by name
 *
 * @param name  Name of the virtual wakelock
 */
static void
mce_wakelock_rem_entry(const char *name)
{
    if( !mce_wakelock_lut )
        goto EXIT;

    g_hash_table_remove(mce_wakelock_lut, name);

EXIT:
    return;
}

/** Predicate for: have virtual wakelocks
 *
 * @return true if there are active virtual wakelocks, false otherwise
 */
static bool
mce_wakelock_have_entries(void)
{
    bool have_entries = false;

    if( !mce_wakelock_lut )
        goto EXIT;

    if( g_hash_table_size(mce_wakelock_lut) > 0 )
        have_entries = true;

EXIT:
    return have_entries;
}

/** Obtain virtual wakelock
 *
 * @param name  Name of the virtual wakelock
 */
void
mce_wakelock_obtain(const char *name, int duration_ms)
{
    if( !mce_wakelock_ready )
        goto EXIT;

    /* Add entry & start release timer */
    mwl_wakelock_start_timer(mce_wakelock_add_entry(name),
                             duration_ms);

    /* Re-evaluate need for real wakelock */
    mwl_rawlock_set(mce_wakelock_have_entries());

EXIT:
    return;
}

/** Release virtual wakelock
 *
 * @param name  Name of the virtual wakelock
 */
void
mce_wakelock_release(const char *name)
{
    if( !mce_wakelock_ready )
        goto EXIT;

    /* Remove entry */
    mce_wakelock_rem_entry(name);

    /* Re-evaluate need for real wakelock */
    mwl_rawlock_set(mce_wakelock_have_entries());

EXIT:
    return;
}

/** Initialize mce wakelock subsystem
 */
void
mce_wakelock_init(void)
{
    /* Leave disabled if sysfs control files do not exist */
    if( !mwl_rawlock_supported() )
        goto EXIT;

    /* Setup entry look up table */
    if( !mce_wakelock_lut )
        mce_wakelock_lut = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, mwl_wakelock_delete_cb);

    /* In case previous mce instance managed to exit without
     * clearing wakelocks: unlock without error checking */
    mwl_rawlock_unlock();

    /* allow locking */
    mce_wakelock_ready = true;

EXIT:
    mce_log(LL_DEBUG, "wakelock usage %s",
            mce_wakelock_ready ? "enabled" : "disabled");

    return;
}

/** Cleanup mce wakelock subsystem
 */
void
mce_wakelock_quit(void)
{
    /* Deny further locking */
    mce_wakelock_ready = false;

    /* Flush entry look up table */
    if( mce_wakelock_lut )
        g_hash_table_unref(mce_wakelock_lut), mce_wakelock_lut = 0;

    /* If there were active internal wakelocks,
     * remove the real kernel wakelock too */
    mwl_rawlock_set(false);
}

/** Async signal safe wakelock cleanup
 */
void
mce_wakelock_abort(void)
{
    /* Deny further locking */
    mce_wakelock_ready = false;

    /* Uncondition unlock, using syscalls only */
    mwl_rawlock_unlock();
}
