/**
 * @file mempressure.c
 * Memory use tracking and notification plugin for the Mode Control Entity
 * <p>
 * Copyright (c) 2014 - 2021 Jolla Ltd.
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
#include "../mce-log.h"
#include "../mce-setting.h"

#include <sys/eventfd.h>

#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

#include <gmodule.h>
#include <glib/gmain.h>

/* Paths to relevant cgroup data/control files */
#define CGROUP_MEMORY_DIRECTORY "/sys/fs/cgroup/memory"
#define CGROUP_DATA_PATH        CGROUP_MEMORY_DIRECTORY "/memory.usage_in_bytes"
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
 * MEMPRESSURE_LIMIT
 * ------------------------------------------------------------------------- */

static void mempressure_limit_clear   (mempressure_limit_t *self);
static bool mempressure_limit_is_valid(const mempressure_limit_t *self);
static int  mempressure_limit_repr    (const mempressure_limit_t *self, char *data, size_t size);
static bool mempressure_limit_parse   (mempressure_limit_t *self, const char *data);
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
static bool     mempressure_cgroup_update_status    (void);

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

/* Convert kernel reported byte count to page count used in configuration
 */
static int mempressure_bytes_to_pages(uint64_t bytes)
{
    return (int)(bytes / PAGE_SIZE);
}

/* Convert configuration page count to bytes for use in kernel interface
 */
static uint64_t mempressure_pages_to_bytes(int pages)
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

    if( chn != 0 ) g_io_channel_unref(chn);

    return wid;
}

/* ========================================================================= *
 * MEMPRESSURE_LIMIT
 * ========================================================================= */

/** Reset limit object values
 */
static void
mempressure_limit_clear(mempressure_limit_t *self)
{
    self->mnl_used = 0;
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

/** Parse limit object from /sys/fs/cgroup/memory/memory.usage_in_bytes format
 */
static bool
mempressure_limit_parse(mempressure_limit_t *self, const char *data)
{
    char     *end = 0;
    uint64_t  val = strtoull(data, &end, 10);
    bool      res = end > data && *end == 0;

    if( !res )
        mce_log(LL_ERR, "parse error: '%s' is not a number", data);
    else
        self->mnl_used = mempressure_bytes_to_pages(val);

    return res;
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
            if( mempressure_limit_exceeded(mempressure_limit+lev, &mempressure_status) )
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
            mempressure_limit_repr(mempressure_limit+i, tmp, sizeof tmp);
            mce_log(LL_DEBUG, "%s: %s", memnotify_level_repr(i), tmp);
        }
    }
}

/* ========================================================================= *
 * MEMPRESSURE_CGROUP
 * ========================================================================= */

/** File descriptor for CGROUP_DATA_PATH */
static int   mempressure_cgroup_data_fd  = -1;

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
            access(CGROUP_CTRL_PATH, W_OK) == 0);
}

/** Input watch callback for cgroup memory threshold crossings
 */
static gboolean
mempressure_cgroup_event_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
{
    (void) chn;
    (void) aptr;

    gboolean ret = G_SOURCE_REMOVE;

    if( !mempressure_cgroup_event_id )
        goto EXIT;

    if( mempressure_cgroup_event_fd == -1 )
        goto EXIT;

    if( mempressure_cgroup_data_fd == -1 )
        goto EXIT;

    mce_log(LL_DEBUG, "eventfd iowatch notify");

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
}

static int   mempressure_psi_warning_fd = -1;
static int   mempressure_psi_critical_fd = -1;
static guint mempressure_psi_warn_event_id = 0;
static guint mempressure_psi_crit_event_id = 0;
static guint mempressure_psi_warn_timeout = 0;
static guint mempressure_psi_crit_timeout = 0;

#define PSI_MEMORY_PATH "/proc/pressure/memory"

static void
mempressure_psi_update_level(void)
{
    memnotify_level_t prev = mempressure_level;

    if ( mempressure_psi_crit_timeout != 0 ) {
        mempressure_level = MEMNOTIFY_LEVEL_CRITICAL;
    } else if ( mempressure_psi_warn_timeout != 0 ) {
        mempressure_level = MEMNOTIFY_LEVEL_WARNING;
    } else {
        mempressure_level = MEMNOTIFY_LEVEL_NORMAL;
    }
    if ( prev != mempressure_level ) {
        mce_log(LL_WARN, "mempressure_level: %s -> %s",
                memnotify_level_repr(prev),
                memnotify_level_repr(mempressure_level));

        datapipe_exec_full(&memnotify_level_pipe,
                           GINT_TO_POINTER(mempressure_level));
    }
}

static gboolean
mempressure_psi_timeout_cb(gpointer user_data)
{
    if( user_data == NULL ) {
        mce_log(LL_ERR, "null timeout argument");
        goto EXIT;
    }

    int fd = *((int*)user_data);
    if (fd == mempressure_psi_warning_fd) {
        mce_log(LL_INFO, "PSI warning event timeout");
        mempressure_psi_warn_timeout = 0;
    } else if (fd == mempressure_psi_critical_fd) {
        mce_log(LL_INFO, "PSI critical event timeout");
        mempressure_psi_crit_timeout = 0;
    } else {
        mce_log(LL_CRIT, "unknown fd in timeout callback");
        goto EXIT;
    }

    mempressure_psi_update_level();

EXIT:
    return G_SOURCE_REMOVE;
}

/** Input watch callback for PSI events
 */
static gboolean
mempressure_psi_event_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
{
    (void)chn;
    (void)aptr;

    gboolean ret = G_SOURCE_REMOVE;

    if( cnd & ~G_IO_PRI ) {
        mce_log(LL_ERR, "unexpected input watch condition");
        goto EXIT;
    }
    if( aptr == NULL ) {
        mce_log(LL_ERR, "null watch argument");
        goto EXIT;
    }

    int fd = *((int*)aptr);

    if (fd != mempressure_psi_warning_fd && fd != mempressure_psi_critical_fd) {
        mce_log(LL_CRIT, "unknown fd in iowatch callback");
        goto EXIT;
    }

    if (fd == mempressure_psi_warning_fd) {
        mce_log(LL_INFO, "warning PSI event");
        if (mempressure_psi_warn_timeout != 0) {
            g_source_remove(mempressure_psi_warn_timeout);
        }
        mempressure_psi_warn_timeout = g_timeout_add(2000, mempressure_psi_timeout_cb, aptr);
    } else {
        assert(fd == mempressure_psi_critical_fd);
        mce_log(LL_INFO, "critical PSI event");
        if (mempressure_psi_crit_timeout != 0) {
            g_source_remove(mempressure_psi_crit_timeout);
        }
        mempressure_psi_crit_timeout = g_timeout_add(2000, mempressure_psi_timeout_cb, aptr);
    }
    mempressure_psi_update_level();
    ret = G_SOURCE_CONTINUE;

EXIT:

    if( ret == G_SOURCE_REMOVE &&
        (mempressure_psi_warn_event_id || mempressure_psi_crit_event_id) ){
        mempressure_psi_warn_event_id = 0;
        mempressure_psi_crit_event_id = 0;
        mce_log(LL_CRIT, "disabling eventfd iowatch");
    }

    return ret;
}

static void
mempressure_psi_quit(void)
{
    if ( mempressure_psi_warn_timeout != 0 ) {
        g_source_remove(mempressure_psi_warn_timeout);
    }
    if ( mempressure_psi_crit_timeout != 0 ) {
        g_source_remove(mempressure_psi_crit_timeout);
    }
    if( mempressure_psi_warn_event_id ) {
        mce_log(LL_DEBUG, "remove warnong eventfd iowatch");
        g_source_remove(mempressure_psi_warn_event_id),
            mempressure_psi_warn_event_id = 0;
    }
    if( mempressure_psi_crit_event_id ) {
        mce_log(LL_DEBUG, "remove critical eventfd iowatch");
        g_source_remove(mempressure_psi_crit_event_id),
            mempressure_psi_crit_event_id = 0;
    }
    if ( mempressure_psi_warning_fd ) {
        if ( close( mempressure_psi_warning_fd ) < 0) {
            mce_log(LL_ERR, "close failed");
        }
        mempressure_psi_warning_fd = 0;
    }
    if ( mempressure_psi_critical_fd ) {
        if ( close( mempressure_psi_critical_fd ) < 0) {
            mce_log(LL_ERR, "close failed");
        }
        mempressure_psi_critical_fd = 0;
    }
}

static bool
mempressure_psi_init(void)
{
    bool res = false;

    /* Get file descriptors */
    mce_log(LL_DEBUG, "open %s for warning threshold", PSI_MEMORY_PATH);
    if( (mempressure_psi_warning_fd = open(PSI_MEMORY_PATH, O_RDWR | O_NONBLOCK)) == -1 ) {
        mce_log(LL_ERR, "%s: open: %m", PSI_MEMORY_PATH);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "open %s for critical threshold", PSI_MEMORY_PATH);
    if( (mempressure_psi_critical_fd = open(PSI_MEMORY_PATH, O_RDWR | O_NONBLOCK)) == -1 ) {
        mce_log(LL_ERR, "%s: open: %m", PSI_MEMORY_PATH);
        goto EXIT;
    }

    // setup thresholds
    // TODO: configurable
    const char warning_threshold[] = "some 100000 1000000"; // 100 ms stall (some process) in one-second window
    const char critical_threshold[] = "full 150000 1000000"; // 150 ms stall (all processes) in one-second window

    if (write(mempressure_psi_warning_fd, warning_threshold, strlen(warning_threshold) + 1) < 0) {
        mce_log(LL_ERR, "%s: write: %m", PSI_MEMORY_PATH);
        goto EXIT;
    }

    if (write(mempressure_psi_critical_fd, critical_threshold, strlen(critical_threshold) + 1) < 0) {
        mce_log(LL_ERR, "%s: write: %m", PSI_MEMORY_PATH);
        goto EXIT;
    }

    /* Setup notification iowatch */
    mce_log(LL_DEBUG, "add warning fd iowatch");
    mempressure_psi_warn_event_id =
    mempressure_iowatch_add(mempressure_psi_warning_fd, false, G_IO_PRI,
                            mempressure_psi_event_cb, &mempressure_psi_warning_fd);
    if( !mempressure_psi_warn_event_id ) {
        mce_log(LL_ERR, "failed to add warning fd iowatch");
        goto EXIT;
    }

    mce_log(LL_DEBUG, "add critical fd iowatch");
    mempressure_psi_crit_event_id =
    mempressure_iowatch_add(mempressure_psi_critical_fd, false, G_IO_PRI,
                            mempressure_psi_event_cb, &mempressure_psi_critical_fd);
    if( !mempressure_psi_crit_event_id ) {
        mce_log(LL_ERR, "failed to add critical fd iowatch");
        goto EXIT;
    }

    res = true;
    mempressure_level = MEMNOTIFY_LEVEL_NORMAL;

EXIT:

    // all or nothing
    if( !res )
        mempressure_psi_quit();

    return res;
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

    mce_log(LL_DEBUG, "open %s", CGROUP_CTRL_PATH);
    if( (mempressure_cgroup_ctrl_fd = open(CGROUP_CTRL_PATH, O_WRONLY)) == -1 ) {
        mce_log(LL_ERR, "%s: open: %m", CGROUP_CTRL_PATH);
        goto EXIT;
    }

    /* Program notification limits */
    for( int i = MEMNOTIFY_LEVEL_WARNING; i <= MEMNOTIFY_LEVEL_CRITICAL; ++i ) {
        int      pages = mempressure_limit[i].mnl_used;
        uint64_t bytes = mempressure_pages_to_bytes(pages);

        mce_log(LL_DEBUG, "mempressure %s threshold %" PRIu64 "",
                memnotify_level_repr(i), bytes);

        char     data[256];
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
     * act as a signal for furthre to be loaded alternate memory pressure
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

/** Read current memory use status from kernel side
 */
static bool
mempressure_cgroup_update_status(void)
{
    bool res = false;

    if( mempressure_cgroup_data_fd == -1 ) {
        mce_log(LL_ERR, "data file not opened");
        goto EXIT;
    }

    if( lseek(mempressure_cgroup_data_fd, 0, SEEK_SET) == -1 ) {
        mce_log(LL_ERR, "failed to rewind data file: %m");
        goto EXIT;
    }

    errno = 0;

    char tmp[256];
    int done = read(mempressure_cgroup_data_fd, tmp, sizeof tmp - 1);
    if( done <= 0 ) {
        mce_log(LL_ERR, "failed to read data file: %m");
        goto EXIT;
    }

    tmp[done] = 0;
    tmp[strcspn(tmp, "\n")] = 0;

    mce_log(LL_DEBUG, "status from data file: %s", tmp);

    if( !mempressure_limit_parse(&mempressure_status, tmp) ) {
        mce_log(LL_ERR, "failed to parse status");
        goto EXIT;
    }

    res = true;

EXIT:
    if( !res )
        mempressure_limit_clear(&mempressure_status);

    return res;
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

    if (gcv == NULL) {
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
static void mempressure_setting_init(void)
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
static void mempressure_setting_quit(void)
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
    mempressure_cgroup_quit();
    mempressure_setting_quit();
}

static bool
mempressure_plugin_init(void)
{
    bool success = false;

    mempressure_setting_init();

    if( !mempressure_cgroup_init() )
        goto EXIT;

    if ( !mempressure_psi_init() )
        goto EXIT;

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
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module)
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
G_MODULE_EXPORT void g_module_unload(GModule *module)
{
    (void)module;

    mce_log(LL_DEBUG, "unloading mempressure plugin");
    mempressure_plugin_quit();

    return;
}
