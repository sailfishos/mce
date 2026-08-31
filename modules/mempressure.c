/**
 * @file mempressure.c
 * Memory use tracking and notification plugin for the Mode Control Entity
 * <p>
 * Copyright (c) 2014 - 2021 Jolla Ltd.
 * Copyright (c) 2026 Jolla Mobile Ltd
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

#include "memnotify.h"

#include "../mce.h"
#include "../mce-lib.h"
#include "../mce-log.h"
#include "../mce-setting.h"

#include <sys/eventfd.h>

#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>

#include <gmodule.h>

/* Paths to relevant cgroup data/control files */
#define CGROUP_MEMORY_DIRECTORY "/sys/fs/cgroup/memory"
#define CGROUP_DATA_PATH        CGROUP_MEMORY_DIRECTORY "/memory.usage_in_bytes"
#define CGROUP_STAT_PATH        CGROUP_MEMORY_DIRECTORY "/memory.stat"
#define CGROUP_CTRL_PATH        CGROUP_MEMORY_DIRECTORY "/cgroup.event_control"

/* RAM page size in bytes
 *
 * Configuration is defined in terms of (memnotify style) page counts.
 *
 * Conversion to/from byte counts used in cgroups interface are done via:
 * - mempressure_bytes_to_pages()
 * - mempressure_pages_to_bytes()
 */
#define PAGE_SIZE ((unsigned long)sysconf(_SC_PAGESIZE))

/* ========================================================================= *
 * Types
 * ========================================================================= */

/** Structure for holding for /dev/mempressure compatible limit data */
typedef struct
{
    /** Estimate of number of non-discardable RAM pages */
    gint        mnl_used;
} mempressure_limit_t;

/** Structure for holding values parsed from cgroup sysfs files */
typedef struct {
    /* - - - - - - - - - - - - - - - - - - - *
     * Values from memory.usage_in_bytes
     * - - - - - - - - - - - - - - - - - - - */

    uint64_t mu_usage_in_bytes;

    /* - - - - - - - - - - - - - - - - - - - *
     * Values from memory.stat
     * - - - - - - - - - - - - - - - - - - - */

    /* anon rss */

    uint64_t mu_active_anon;          /* active_anon (when available) */
    uint64_t mu_inactive_anon;        /* inactive_anon (when available) */
    uint64_t mu_rss;                  /* anon rss (active_anon + inactive_anon) */

    /* file backed */
    uint64_t mu_active_file;          /* active_file (when available) */
    uint64_t mu_inactive_file;        /* inactive_file (when available) */
    uint64_t mu_cache;                /* file cache (active_file + inactive_file) */

    /* slab */

    uint64_t mu_slab_reclaimable;     /* slab_reclaimable (when available) */
    uint64_t mu_slab_unreclaimable;   /* slab_unreclaimable (when available) */
    uint64_t mu_slab;                 /* slab (when available) */

    /* swap */
    uint64_t mu_swap;                 /* swap usage if available */

    /* special */

    uint64_t mu_unevictable;          /* unevictable (mlocked) */

    /* - - - - - - - - - - - - - - - - - - - *
     * Derived values
     * - - - - - - - - - - - - - - - - - - - */

    uint64_t mu_anon;                 /* active_anon + inactive_anon OR rss */
    uint64_t mu_file;                 /* active_file + inactive_file OR cache */

    uint64_t mu_reclaimable;          /* file + slab_reclaimable */
    uint64_t mu_non_reclaimable;      /* anon + slab_unreclaimable + unevictable */

    /* - - - - - - - - - - - - - - - - - - - *
     * Weighted evaluation score
     * - - - - - - - - - - - - - - - - - - - */

    uint64_t mu_score;
} memcg_usage_t;

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * UTILITY
 * ------------------------------------------------------------------------- */

static int      mempressure_bytes_to_pages(uint64_t bytes);
static uint64_t mempressure_pages_to_bytes(int pages);
static guint    mempressure_iowatch_add   (int fd, bool close_on_unref, GIOCondition cnd, GIOFunc io_cb, gpointer aptr);

/* ------------------------------------------------------------------------- *
 * MEMCG_USAGE
 * ------------------------------------------------------------------------- */

static void     memcg_usage_update      (memcg_usage_t *self, const memcg_usage_t *that);
static bool     memcg_usage_parse_uint64(const char *str, uint64_t *pval);
static bool     memcg_usage_parse_data  (memcg_usage_t *self, char *mem_bytes, char *mem_stat);
static uint64_t memcg_usage_get_score   (const memcg_usage_t *self);

/* ------------------------------------------------------------------------- *
 * MEMPRESSURE_DATAPIPE
 * ------------------------------------------------------------------------- */

static void mempressure_datapipe_display_state_curr_cb(gconstpointer data);
static void mempressure_datapipe_heartbeat_event_cb   (gconstpointer data);
static void mempressure_datapipe_init                 (void);
static void mempressure_datapipe_quit                 (void);

/* ------------------------------------------------------------------------- *
 * MEMPRESSURE_LIMIT
 * ------------------------------------------------------------------------- */

static void mempressure_limit_clear   (mempressure_limit_t *self);
static bool mempressure_limit_is_valid(const mempressure_limit_t *self);
static int  mempressure_limit_repr    (const mempressure_limit_t *self, char *data, size_t size);
static void mempressure_limit_set     (mempressure_limit_t *self, gint page_count);
static bool mempressure_limit_exceeded(const mempressure_limit_t *self, const mempressure_limit_t *status);

/* ------------------------------------------------------------------------- *
 * MEMPRESSURE_STATUS
 * ------------------------------------------------------------------------- */

static memnotify_level_t mempressure_status_evaluate_level(void);
static bool              mempressure_status_update_level  (void);
static void              mempressure_status_show_triggers (void);

/* ------------------------------------------------------------------------- *
 * MEMPRESSURE_CGROUP
 * ------------------------------------------------------------------------- */

static bool     mempressure_cgroup_is_available     (void);
static gboolean mempressure_cgroup_event_cb         (GIOChannel *chn, GIOCondition cnd, gpointer aptr);
static void     mempressure_cgroup_quit             (void);
static bool     mempressure_cgroup_init             (void);
static void     mempressure_cgroup_update_thresholds(void);
static bool     mempressure_cgroup_read_data             (const char *path, int fd, char *buff, size_t size);
static bool     mempressure_cgroup_update_status    (void);

/* ------------------------------------------------------------------------- *
 * MEMPRESSURE_PERIODIC_POLL
 * ------------------------------------------------------------------------- */

static gboolean mempressure_periodic_poll_timer_cb     (gpointer data);
static void     mempressure_periodic_poll_stop         (void);
static void     mempressure_periodic_poll_start        (void);
static gboolean mempressure_periodic_poll_rethink_cb   (gpointer data);
static void     mempressure_periodic_poll_rethink_later(void);
static void     mempressure_periodic_poll_init         (void);
static void     mempressure_periodic_poll_quit         (void);

/* ------------------------------------------------------------------------- *
 * MEMPRESSURE_SETTING
 * ------------------------------------------------------------------------- */

static void mempressure_setting_cb  (GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);
static void mempressure_setting_init(void);
static void mempressure_setting_quit(void);

/* ------------------------------------------------------------------------- *
 * MEMPRESSURE_PLUGIN
 * ------------------------------------------------------------------------- */

static void mempressure_plugin_quit(void);
static bool mempressure_plugin_init(void);

/* ------------------------------------------------------------------------- *
 * G_MODULE
 * ------------------------------------------------------------------------- */

const gchar *g_module_check_init(GModule *module);
void         g_module_unload    (GModule *module);

/* ========================================================================= *
 * UTILITY
 * ========================================================================= */

static inline double
mempressure_bytes_to_megabytes(uint64_t byte_count)
{
    return byte_count * 1e-6;
}

/* Convert kernel reported byte count to page count used in configuration
 */
static int
mempressure_bytes_to_pages(uint64_t bytes)
{
    return (int)(bytes / PAGE_SIZE);
}

/* Convert configuration page count to bytes for use in kernel interface
 */
static uint64_t
mempressure_pages_to_bytes(int pages)
{
    if( pages < 0 )
        pages = 0;
    return PAGE_SIZE * (uint64_t)pages;
}

/** Add a glib I/O notification for a file descriptor
 */
static guint
mempressure_iowatch_add(int fd, bool close_on_unref,
                        GIOCondition cnd, GIOFunc io_cb, gpointer aptr)
{
    guint         wid = 0;
    GIOChannel   *chn = 0;

    if( !(chn = g_io_channel_unix_new(fd)) ) {
        goto EXIT;
    }

    g_io_channel_set_close_on_unref(chn, close_on_unref);

    cnd |= G_IO_ERR | G_IO_HUP | G_IO_NVAL;

    if( !(wid = g_io_add_watch(chn, cnd, io_cb, aptr)) )
        goto EXIT;

EXIT:

    if( chn )
        g_io_channel_unref(chn);

    return wid;
}

/* ========================================================================= *
 * MEMCG_USAGE
 * ========================================================================= */

static void
memcg_usage_update(memcg_usage_t *self, const memcg_usage_t *that)
{
#define update_silent(memb) do {\
    self->memb = that->memb;\
} while( false )

#define update_verbose(memb) do {\
    if( self->memb != that->memb ) {\
        mce_log(LL_DEBUG, "memcg_usage.%s: %.1f -> %.1f (%+.1f MB)", #memb,\
                mempressure_bytes_to_megabytes(self->memb),\
                mempressure_bytes_to_megabytes(that->memb),\
                that->memb * 1e-6 - self->memb * 1e-6);\
        self->memb = that->memb;\
    }\
} while( false )

    update_silent(mu_active_anon);
    update_silent(mu_inactive_anon);
    update_silent(mu_rss);
    update_silent(mu_active_file);
    update_silent(mu_inactive_file);
    update_silent(mu_cache);
    update_silent(mu_slab_reclaimable);
    update_silent(mu_slab_unreclaimable);
    update_silent(mu_slab);
    update_silent(mu_swap);
    update_silent(mu_unevictable);
    update_silent(mu_anon);
    update_silent(mu_file);
    update_silent(mu_reclaimable);
    update_silent(mu_non_reclaimable);
    update_verbose(mu_score);
    update_verbose(mu_usage_in_bytes);

#undef update_verbose
#undef update_silent
}

static bool
memcg_usage_parse_uint64(const char *str, uint64_t *pval)
{
    bool parsed = false;

    if( str ) {
        char     *end = NULL;
        uint64_t  val = strtoull(str, &end, 0);
        if( end > str && *end == 0 )
            *pval = val, parsed = true;
        else
            mce_log(LL_WARN, "not a uint64 number: %s", str);
    }

    return parsed;
}

static bool
memcg_usage_parse_data(memcg_usage_t *self, char *mem_bytes, char *mem_stat)
{
    bool ret = false;

    /* Parse memory.usage_in_bytes content */
    char *pos = mem_bytes;
    char *row = mce_slice_token(pos, &pos, "\n");
    if( !memcg_usage_parse_uint64(row, &self->mu_usage_in_bytes) ) {
        mce_log(LL_ERR, "%s: failed to parse content", CGROUP_DATA_PATH);
        goto EXIT;
    }

    /* Parse memory.stat content */

    for( pos = mem_stat; *pos; ) {
        row = mce_slice_token(pos, &pos, "\n");

        char *key = mce_slice_token(row, &row, NULL);
        char *str = mce_slice_token(row, &row, NULL);

        uint64_t val = 0;
        if( !memcg_usage_parse_uint64(str, &val) ) {
            mce_log(LL_ERR, "%s: failed to parse field: %s = %s", CGROUP_STAT_PATH, key, str);
            goto EXIT;
        }

        else if( !strcmp(key, "total_active_anon") )
            self->mu_active_anon = val;
        else if( !strcmp(key, "total_inactive_anon") )
            self->mu_inactive_anon = val;
        else if( !strcmp(key, "total_rss") )
            self->mu_rss = val;

        else if( !strcmp(key, "total_active_file") )
            self->mu_active_file = val;
        else if( !strcmp(key, "total_inactive_file") )
            self->mu_inactive_file = val;
        else if( !strcmp(key, "total_cache") )
            self->mu_cache = val;

        else if( !strcmp(key, "total_slab_reclaimable") )
            self->mu_slab_reclaimable = val;
        else if( !strcmp(key, "total_slab_unreclaimable") )
            self->mu_slab_unreclaimable = val;
        else if( !strcmp(key, "total_slab") )
            self->mu_slab = val;

        else if( !strcmp(key, "total_unevictable") )
            self->mu_unevictable = val;
        else if( !strcmp(key, "total_swap") )
            self->mu_swap = val;
    }

    /* Calculate derived values */

    self->mu_anon = (self->mu_active_anon + self->mu_inactive_anon) ?: self->mu_rss;
    self->mu_file = (self->mu_active_file + self->mu_inactive_file) ?: self->mu_cache;

    /* Note: We might have total "slab" and "slab_reclaimable" only, in which case
     * calculate missing "slab_unreclaimable". */
    if( !self->mu_slab_unreclaimable && self->mu_slab )
        self->mu_slab_unreclaimable = self->mu_slab - self->mu_slab_reclaimable;

    self->mu_reclaimable        = self->mu_file + self->mu_slab_reclaimable;
    self->mu_non_reclaimable    = self->mu_anon + self->mu_slab_unreclaimable;

    /* Note: "unevictable" overlaps with "unreclaimable" but we have no way of
     * knowning how much -> use maximum of these two values */
    if( self->mu_non_reclaimable < self->mu_unevictable )
        self->mu_non_reclaimable = self->mu_unevictable;

    /* Calculate memory pressure score */

    double reclaimable_weight = 0.05; // FIXME: these should be settings
    double swap_weight        = 0.20;

    self->mu_score = self->mu_non_reclaimable;

    if( reclaimable_weight > 0.0 )
        self->mu_score += (uint64_t)(self->mu_reclaimable * reclaimable_weight);

    if( swap_weight > 0.0 )
        self->mu_score += (uint64_t)(self->mu_swap * swap_weight);

    ret = true;

EXIT:
    return ret;
}

static uint64_t
memcg_usage_get_score(const memcg_usage_t *self)
{
    return self ? self->mu_score : 0;
}

/* ========================================================================= *
 * MEMPRESSURE_DATAPIPE
 * ========================================================================= */

static display_state_t display_state_curr = MCE_DISPLAY_UNDEF;

/** Handle display state change
 *
 * @param data The display state stored in a pointer
 */
static void
mempressure_datapipe_display_state_curr_cb(gconstpointer data)
{
    display_state_t prev = display_state_curr;

    display_state_curr = GPOINTER_TO_INT(data);

    if( prev == display_state_curr )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_curr: %s -> %s",
            display_state_repr(prev),
            display_state_repr(display_state_curr));

    mempressure_periodic_poll_rethink_later();
EXIT:
    return;
}

/** Change notifications for heartbeat_event_pipe
 *
 * @param data (unused dummy parameter)
 */
static void
mempressure_datapipe_heartbeat_event_cb(gconstpointer data)
{
    (void)data;

    mce_log(LL_DEBUG, "ENTER - refresh on heartbeat");

    if( mempressure_cgroup_update_status() )
        mempressure_status_update_level();

    mce_log(LL_DEBUG, "LEAVE - refresh on heartbeat");
}

/** Array of datapipe handlers */
static datapipe_handler_t mempressure_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &heartbeat_event_pipe,
        .output_cb = mempressure_datapipe_heartbeat_event_cb,
    },
    {
        .datapipe  = &display_state_curr_pipe,
        .output_cb = mempressure_datapipe_display_state_curr_cb,
    },
    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t mempressure_datapipe_bindings =
{
    .module   = "mempressure",
    .handlers = mempressure_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void
mempressure_datapipe_init(void)
{
    mce_datapipe_init_bindings(&mempressure_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void
mempressure_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&mempressure_datapipe_bindings);
}

/* ========================================================================= *
 * MEMPRESSURE_LIMIT
 * ========================================================================= */

/** Reset limit object values
 */
static void
mempressure_limit_clear(mempressure_limit_t *self)
{
    mempressure_limit_set(self, 0);
}

/** Limit validity predicate
 */
static bool
mempressure_limit_is_valid(const mempressure_limit_t *self)
{
    return self->mnl_used > 0;
}

/** Convert limit object values to /dev/mempressure compatible ascii form
 */
static int
mempressure_limit_repr(const mempressure_limit_t *self, char *data, size_t size)
{
    int res = snprintf(data, size, "used %d", self->mnl_used);
    return res;
}

/** Set limit object value
 */
static void
mempressure_limit_set(mempressure_limit_t *self, gint page_count)
{
    if( page_count < 0 )
        page_count = 0;

    if( self->mnl_used != page_count ) {
        mce_log(LL_DEBUG, "mnl_used: %d -> %d", self->mnl_used, page_count);
        self->mnl_used = page_count;
    }
}

/** Check if limit object values are exceeded by given state data
 */
static bool
mempressure_limit_exceeded(const mempressure_limit_t *self,
                           const mempressure_limit_t *status)
{
    return (mempressure_limit_is_valid(self) &&
            self->mnl_used <= status->mnl_used);
}

/* ========================================================================= *
 * MEMPRESSURE_STATUS
 * ========================================================================= */

/** Configuration limits for normal/warning/critical levels */
static mempressure_limit_t mempressure_limit[] =
{
    [MEMNOTIFY_LEVEL_NORMAL] = {
        .mnl_used   = 0,
    },
    [MEMNOTIFY_LEVEL_WARNING] = {
        // values come from config - disabled by default
        .mnl_used   = 0,
    },
    [MEMNOTIFY_LEVEL_CRITICAL] = {
        // values come from config - disabled by default
        .mnl_used   = 0,
    },
};

/** Cached status read from kernel device */
static mempressure_limit_t mempressure_status =
{
    .mnl_used   = 0,
};

/** Cached memory use level */
static memnotify_level_t mempressure_level = MEMNOTIFY_LEVEL_UNKNOWN;

/** Check current memory status against triggering levels
 */
static memnotify_level_t
mempressure_status_evaluate_level(void)
{
    memnotify_level_t res = MEMNOTIFY_LEVEL_UNKNOWN;

    if( mempressure_limit_is_valid(&mempressure_status) ) {
        res = MEMNOTIFY_LEVEL_NORMAL;

        for( memnotify_level_t lev = MEMNOTIFY_LEVEL_NORMAL + 1;
             lev < G_N_ELEMENTS(mempressure_limit); ++lev ) {
            if( mempressure_limit_exceeded(mempressure_limit + lev, &mempressure_status) )
                res = lev;
        }
    }

    return res;
}

/** Re-evaluate memory use level and broadcast changes via datapipe
 */
static bool
mempressure_status_update_level(void)
{
    memnotify_level_t prev = mempressure_level;

    mempressure_level = mempressure_status_evaluate_level();

    if( mempressure_level == prev )
        goto EXIT;

    mce_log(LL_WARN, "mempressure_level: %s -> %s",
            memnotify_level_repr(prev),
            memnotify_level_repr(mempressure_level));

    datapipe_exec_full(&memnotify_level_pipe,
                       GINT_TO_POINTER(mempressure_level));

    mempressure_periodic_poll_rethink_later();

EXIT:

    return mempressure_level != MEMNOTIFY_LEVEL_UNKNOWN;
}

/** Log current memory level configuration for debugging purposes
 */
static void
mempressure_status_show_triggers(void)
{
    if( mce_log_p(LL_DEBUG) ) {
        for( size_t i = 0; i < G_N_ELEMENTS(mempressure_limit); ++i ) {
            char tmp[256];
            mempressure_limit_repr(mempressure_limit + i, tmp, sizeof tmp);
            mce_log(LL_DEBUG, "%s: %s", memnotify_level_repr(i), tmp);
        }
    }
}

/* ========================================================================= *
 * MEMPRESSURE_CGROUP
 * ========================================================================= */

/** File descriptor for CGROUP_DATA_PATH */
static int   mempressure_cgroup_data_fd  = -1;

/** File descriptor for CGROUP_STAT_PATH */
static int   mempressure_cgroup_stat_fd  = -1;

/** File descriptor for CGROUP_CTRL_PATH */
static int   mempressure_cgroup_ctrl_fd  = -1;

/** Eventfd for receiving notifications about threshold crossings */
static int   mempressure_cgroup_event_fd = -1;

/** I/O watch for mempressure_cgroup_event_fd */
static guint mempressure_cgroup_event_id = 0;

/** Probe if the required cgroup sysfs files are present
 */
static bool
mempressure_cgroup_is_available(void)
{
    return (access(CGROUP_DATA_PATH, R_OK) == 0 &&
            access(CGROUP_STAT_PATH, R_OK) == 0 &&
            access(CGROUP_CTRL_PATH, W_OK) == 0);
}

/** Input watch callback for cgroup memory threshold crossings
 */
static gboolean
mempressure_cgroup_event_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
{
    (void) chn;
    (void) aptr;

    mce_log(LL_DEBUG, "ENTER - refresh on event");

    gboolean ret = G_SOURCE_REMOVE;

    if( !mempressure_cgroup_event_id )
        goto EXIT;

    if( mempressure_cgroup_event_fd == -1 )
        goto EXIT;

    if( mempressure_cgroup_data_fd == -1 )
        goto EXIT;

    if( cnd & ~G_IO_IN ) {
        mce_log(LL_ERR, "unexpected input watch condition");
        goto EXIT;
    }

    uint64_t count = 0;

    ssize_t rc = read(mempressure_cgroup_event_fd, &count, sizeof count);

    if( rc == 0 ) {
        mce_log(LL_ERR, "eventfd eof");
        goto EXIT;
    }

    if( rc == -1 ) {
        if( errno == EINTR || errno == EAGAIN )
            ret = G_SOURCE_CONTINUE;
        else
            mce_log(LL_ERR, "eventfd error: %m");
        goto EXIT;
    }

    if( mempressure_cgroup_update_status() )
        ret = G_SOURCE_CONTINUE;

    /* Update level anyway -> if we disable iowatch due to
     * read/parse errors, the level gets reset to 'unknown'.
     */
    mempressure_status_update_level();

EXIT:

    if( ret == G_SOURCE_REMOVE && mempressure_cgroup_event_id ) {
        mempressure_cgroup_event_id = 0;
        mce_log(LL_CRIT, "disabling eventfd iowatch");
    }

    mce_log(LL_DEBUG, "LEAVE - refresh on event");

    return ret;
}

/** Stop cgroup memory tracking
 */
static void
mempressure_cgroup_quit(void)
{
    if( mempressure_cgroup_event_id ) {
        mce_log(LL_DEBUG, "remove eventfd iowatch");
        g_source_remove(mempressure_cgroup_event_id),
            mempressure_cgroup_event_id = 0;
    }

    if( mempressure_cgroup_event_fd != -1 ) {
        mce_log(LL_DEBUG, "close eventfd");
        close(mempressure_cgroup_event_fd),
            mempressure_cgroup_event_fd = -1;
    }

    if( mempressure_cgroup_ctrl_fd != -1 ) {
        mce_log(LL_DEBUG, "close %s", CGROUP_CTRL_PATH);
        close(mempressure_cgroup_ctrl_fd),
            mempressure_cgroup_ctrl_fd = -1;
    }

    if( mempressure_cgroup_data_fd != -1 ) {
        mce_log(LL_DEBUG, "close %s", CGROUP_DATA_PATH);
        close(mempressure_cgroup_data_fd),
            mempressure_cgroup_data_fd = -1;
    }

    if( mempressure_cgroup_stat_fd != -1 ) {
        mce_log(LL_DEBUG, "close %s", CGROUP_STAT_PATH);
        close(mempressure_cgroup_stat_fd),
            mempressure_cgroup_stat_fd = -1;
    }
}

/** Start cgroup memory tracking
 */
static bool
mempressure_cgroup_init(void)
{
    bool res = false;

    /* Check threshold configuration */
    for( int i = MEMNOTIFY_LEVEL_WARNING; i <= MEMNOTIFY_LEVEL_CRITICAL; ++i ) {
        if( !mempressure_limit_is_valid(&mempressure_limit[i]) ) {
            mce_log(LL_WARN, "mempressure '%s' threshold is not defined",
                    memnotify_level_repr(i));
            goto EXIT;
        }
    }

    /* Get file descriptors */
    mce_log(LL_DEBUG, "create eventfd");
    if( (mempressure_cgroup_event_fd = eventfd(0, 0)) == -1 ) {
        mce_log(LL_ERR, "create eventfd: %m");
        goto EXIT;
    }

    mce_log(LL_DEBUG, "open %s", CGROUP_DATA_PATH);
    if( (mempressure_cgroup_data_fd = open(CGROUP_DATA_PATH, O_RDONLY)) == -1 ) {
        mce_log(LL_ERR, "%s: open: %m", CGROUP_DATA_PATH);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "open %s", CGROUP_STAT_PATH);
    if( (mempressure_cgroup_stat_fd = open(CGROUP_STAT_PATH, O_RDONLY)) == -1 ) {
        mce_log(LL_ERR, "%s: open: %m", CGROUP_STAT_PATH);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "open %s", CGROUP_CTRL_PATH);
    if( (mempressure_cgroup_ctrl_fd = open(CGROUP_CTRL_PATH, O_WRONLY)) == -1 ) {
        mce_log(LL_ERR, "%s: open: %m", CGROUP_CTRL_PATH);
        goto EXIT;
    }

    /* Program notification limits */
    const int pages = mempressure_limit[MEMNOTIFY_LEVEL_CRITICAL].mnl_used;
    for( int i = 10; i <= 300; i += 10 ) {
        uint64_t bytes = mempressure_pages_to_bytes(pages) * i / 100;

        mce_log(LL_DEBUG, "TRIGGER(%d) = %g MB", i, bytes * 1e-6);

        char data[64];
        snprintf(data, sizeof data, "%d %d %" PRIu64 "\n",
                 mempressure_cgroup_event_fd, mempressure_cgroup_data_fd, bytes);
        if( write(mempressure_cgroup_ctrl_fd, data, strlen(data)) == -1 ) {
            mce_log(LL_ERR, "%s: write: %m", CGROUP_CTRL_PATH);
            goto EXIT;
        }
    }

    /* Control fd is not needed after threshold setup is done */
    mce_log(LL_DEBUG, "close %s", CGROUP_CTRL_PATH);
    close(mempressure_cgroup_ctrl_fd),
        mempressure_cgroup_ctrl_fd = -1;

    /* Setup notification iowatch */
    mce_log(LL_DEBUG, "add eventfd iowatch");
    mempressure_cgroup_event_id =
        mempressure_iowatch_add(mempressure_cgroup_event_fd, false, G_IO_IN,
                                mempressure_cgroup_event_cb, NULL);
    if( !mempressure_cgroup_event_id ) {
        mce_log(LL_ERR, "failed to add eventfd iowatch");
        goto EXIT;
    }

    /* Evaluate and publish current state */
    if( !mempressure_cgroup_update_status() )
        goto EXIT;

    if( !mempressure_status_update_level() )
        goto EXIT;

    /* Initialization was successfully completed and we have broadcast
     * a valid pressure level on memnotify_level_pipe - which should
     * act as a signal for further to-be-loaded alternate memory pressure
     * plugins to remain inactive.
     */

    res = true;

EXIT:

    // all or nothing
    if( !res )
        mempressure_cgroup_quit();

    return res;
}

/** Set kernel side triggering levels and update current status
 */
static void
mempressure_cgroup_update_thresholds(void)
{
    /* TODO: Is there some way to remove trigger thresholds?
     *
     * Meanwhile do a full reinitialization to get rid of old thresholds.
     */
    mempressure_cgroup_quit();
    mempressure_cgroup_init();
}

static bool
mempressure_cgroup_read_data(const char *path, int fd, char *buff, size_t size)
{
    bool ret = false;

    if( fd == -1 ) {
        mce_log(LL_ERR, "%s: file not opened", path);
        goto EXIT;
    }

    if( TEMP_FAILURE_RETRY(lseek(fd, 0, SEEK_SET)) == -1 ) {
        mce_log(LL_ERR, "%s: failed to rewind: %m", path);
        goto EXIT;
    }

    ssize_t rc = TEMP_FAILURE_RETRY(read(fd, buff, size));
    if( rc == -1 )
        mce_log(LL_ERR, "%s: failed to read: %m", path);
    else if( rc == 0 )
        mce_log(LL_ERR, "%s: unexpected eof", path);
    else if( (size_t)rc < size )
        buff[rc] = 0, ret = true;
    else
        mce_log(LL_ERR, "%s: buffer too small", path);

EXIT:
    return ret;
}

/** Read current memory use status from kernel side
 */
static bool
mempressure_cgroup_update_status(void)
{
    bool res = false;

    char mem_bytes[32];
    if( !mempressure_cgroup_read_data(CGROUP_DATA_PATH, mempressure_cgroup_data_fd,
                                      mem_bytes, sizeof mem_bytes) )
        goto EXIT;

    char mem_stat[1024];
    if( !mempressure_cgroup_read_data(CGROUP_STAT_PATH, mempressure_cgroup_stat_fd,
                                      mem_stat, sizeof mem_stat) )
        goto EXIT;

    memcg_usage_t current = {};
    if( !memcg_usage_parse_data(&current, mem_bytes, mem_stat) )
        goto EXIT;

    // Track changes for debugging purposes
    static memcg_usage_t previous = {};
    memcg_usage_update(&previous, &current);

    uint64_t byte_count = memcg_usage_get_score(&current);
    gint     page_count = mempressure_bytes_to_pages(byte_count);
    mempressure_limit_set(&mempressure_status, page_count);

    res = true;

EXIT:
    if( !res )
        mempressure_limit_clear(&mempressure_status);

    return res;
}

/* ========================================================================= *
 * MEMPRESSURE_PERIODIC_POLL
 * ========================================================================= */

/** Timer ID for: periodic memory pressure polling */
static guint  mempressure_periodic_poll_timer_id = 0;

/** Idle ID for: re-evaluate need for timer based polling */
static guint  mempressure_periodic_poll_rethink_id = 0;

static gboolean
mempressure_periodic_poll_timer_cb(gpointer data)
{
    (void)data;

    mce_log(LL_DEBUG, "ENTER - refresh on timer");

    if( mempressure_cgroup_update_status() )
        mempressure_status_update_level();

    mce_log(LL_DEBUG, "LEAVE - refresh on timer");

    return G_SOURCE_CONTINUE;
}

static void
mempressure_periodic_poll_stop(void)
{
    if( mempressure_periodic_poll_timer_id ) {
        mce_log(LL_DEBUG, "stop timer");
        g_source_remove(mempressure_periodic_poll_timer_id), mempressure_periodic_poll_timer_id = 0;
    }
}

static void
mempressure_periodic_poll_start(void)
{
    if( !mempressure_periodic_poll_timer_id ) {
        mce_log(LL_DEBUG, "start timer");
        mempressure_periodic_poll_timer_id = g_timeout_add(1000, mempressure_periodic_poll_timer_cb, NULL);
    }
}

static gboolean
mempressure_periodic_poll_rethink_cb(gpointer data)
{
    (void)data;

    mempressure_periodic_poll_rethink_id = 0;
    mce_log(LL_DEBUG, "rethink timer");

    bool want_timer = false;

    /* Use timer when we have display on and elevated pressure
     */

    switch( mempressure_level ) {
    case MEMNOTIFY_LEVEL_WARNING:
    case MEMNOTIFY_LEVEL_CRITICAL:
        if( display_state_curr == MCE_DISPLAY_ON )
            want_timer = true;
        break;
    default:
        break;
    }

    if( want_timer )
        mempressure_periodic_poll_start();
    else
        mempressure_periodic_poll_stop();

    return G_SOURCE_REMOVE;
}

static void
mempressure_periodic_poll_rethink_later(void)
{
    if( !mempressure_periodic_poll_rethink_id ) {
        mce_log(LL_DEBUG, "schedule timer rethink");
        mempressure_periodic_poll_rethink_id = g_idle_add(mempressure_periodic_poll_rethink_cb, NULL);
    }
}

static void
mempressure_periodic_poll_init(void)
{
    mempressure_periodic_poll_rethink_later();
}

static void
mempressure_periodic_poll_quit(void)
{
    if( mempressure_periodic_poll_rethink_id )
        g_source_remove(mempressure_periodic_poll_rethink_id), mempressure_periodic_poll_rethink_id = 0;

    mempressure_periodic_poll_stop();
}

/* ========================================================================= *
 * MEMPRESSURE_SETTING
 * ========================================================================= */

/** GConf notification id for mempressure.warning.used level */
static guint mempressure_setting_warning_used_id = 0;

/** GConf notification id for mempressure.critical.used level */
static guint mempressure_setting_critical_used_id = 0;

/** GConf callback for mempressure related settings
 *
 * @param gcc    (not used)
 * @param id     Connection ID from gconf_client_notify_add()
 * @param entry  The modified GConf entry
 * @param data   (not used)
 */
static void
mempressure_setting_cb(GConfClient *const gcc, const guint id,
                       GConfEntry *const entry, gpointer const data)
{
    const GConfValue *gcv = gconf_entry_get_value(entry);

    (void)gcc;
    (void)data;

    if( !gcv ) {
        mce_log(LL_WARN, "GConf Key `%s' has been unset",
                gconf_entry_get_key(entry));
    }
    else if( id == mempressure_setting_warning_used_id ) {
        gint old = mempressure_limit[MEMNOTIFY_LEVEL_WARNING].mnl_used;
        gint val = gconf_value_get_int(gcv);
        if( old != val ) {
            mce_log(LL_DEBUG, "mempressure.warning.used: %d -> %d", old, val);
            mempressure_limit[MEMNOTIFY_LEVEL_WARNING].mnl_used = val;
            mempressure_cgroup_update_thresholds();
        }
    }
    else if( id == mempressure_setting_critical_used_id ) {
        gint old = mempressure_limit[MEMNOTIFY_LEVEL_CRITICAL].mnl_used;
        gint val = gconf_value_get_int(gcv);
        if( old != val ) {
            mce_log(LL_DEBUG, "mempressure.critical.used: %d -> %d", old, val);
            mempressure_limit[MEMNOTIFY_LEVEL_CRITICAL].mnl_used = val;
            mempressure_cgroup_update_thresholds();
        }
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }

    return;
}

/** Get initial setting values and start tracking changes
 */
static void
mempressure_setting_init(void)
{
    /* mempressure.warning.used level */
    mce_setting_notifier_add(MCE_SETTING_MEMNOTIFY_WARNING_PATH,
                             MCE_SETTING_MEMNOTIFY_WARNING_USED,
                             mempressure_setting_cb,
                             &mempressure_setting_warning_used_id);

    mce_setting_get_int(MCE_SETTING_MEMNOTIFY_WARNING_USED,
                        &mempressure_limit[MEMNOTIFY_LEVEL_WARNING].mnl_used);

    /* mempressure.critical.used level */
    mce_setting_notifier_add(MCE_SETTING_MEMNOTIFY_CRITICAL_PATH,
                             MCE_SETTING_MEMNOTIFY_CRITICAL_USED,
                             mempressure_setting_cb,
                             &mempressure_setting_critical_used_id);

    mce_setting_get_int(MCE_SETTING_MEMNOTIFY_CRITICAL_USED,
                        &mempressure_limit[MEMNOTIFY_LEVEL_CRITICAL].mnl_used);

    mempressure_status_show_triggers();
}

/** Stop tracking setting changes
 */
static void
mempressure_setting_quit(void)
{
    mce_setting_notifier_remove(mempressure_setting_warning_used_id),
        mempressure_setting_warning_used_id = 0;

    mce_setting_notifier_remove(mempressure_setting_critical_used_id),
        mempressure_setting_critical_used_id = 0;
}

/* ========================================================================= *
 * MEMPRESSURE_PLUGIN
 * ========================================================================= */

static void
mempressure_plugin_quit(void)
{
    mempressure_datapipe_quit();
    mempressure_cgroup_quit();
    mempressure_setting_quit();
    mempressure_periodic_poll_quit();
}

static bool
mempressure_plugin_init(void)
{
    bool success = false;

    mempressure_setting_init();

    if( !mempressure_cgroup_init() )
        goto EXIT;

    mempressure_datapipe_init();
    mempressure_periodic_poll_init();

    success = true;

EXIT:
    /* All or nothing */
    if( !success )
        mempressure_plugin_quit();

    return success;
}

/* ========================================================================= *
 * G_MODULE
 * ========================================================================= */

/** Init function for the mempressure plugin
 *
 * @param module  (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *
g_module_check_init(GModule *module)
{
    (void)module;

    /* Check if some memory pressure plugin has already taken over */
    memnotify_level_t level = datapipe_get_gint(memnotify_level_pipe);
    if( level != MEMNOTIFY_LEVEL_UNKNOWN ) {
        mce_log(LL_DEBUG, "level already set to %s; mempressure disabled",
                memnotify_level_repr(level));
        goto EXIT;
    }

    /* Check if required sysfs files are present */
    if( !mempressure_cgroup_is_available() ) {
        mce_log(LL_WARN, "mempressure cgroup interface not available");
        goto EXIT;
    }

    /* Initialize */
    if( !mempressure_plugin_init() ) {
        mce_log(LL_WARN, "mempressure plugin init failed");
        goto EXIT;
    }

    mce_log(LL_NOTICE, "mempressure plugin active");

EXIT:

    return NULL;
}

/** Exit function for the mempressure plugin
 *
 * @param module  (not used)
 */
G_MODULE_EXPORT void
g_module_unload(GModule *module)
{
    (void)module;

    mce_log(LL_DEBUG, "unloading mempressure plugin");

    mempressure_plugin_quit();

    return;
}
