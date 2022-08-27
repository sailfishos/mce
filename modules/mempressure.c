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

#include "mempressure.h"

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

// /* Paths to relevant cgroup data/control files */
// #define CGROUP_MEMORY_DIRECTORY "/sys/fs/cgroup/memory"
// #define CGROUP_DATA_PATH        CGROUP_MEMORY_DIRECTORY "/memory.usage_in_bytes"
// #define CGROUP_CTRL_PATH        CGROUP_MEMORY_DIRECTORY "/cgroup.event_control"

#define PSI_MEMORY_PATH "/proc/pressure/memory"

/* ========================================================================= *
 * Types
 * ========================================================================= */

/** Structure for holding for /dev/mempressure compatible limit data */
// typedef struct
// {
//     /** Estimate of number of non-discardable RAM pages */
//     gint        mnl_used;
// } mempressure_limit_t;

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * UTILITY
 * ------------------------------------------------------------------------- */

// static int      mempressure_bytes_to_pages(uint64_t bytes);
// static uint64_t mempressure_pages_to_bytes(int pages);
static guint    mempressure_iowatch_add   (int fd, bool close_on_unref, GIOCondition cnd, GIOFunc io_cb, gpointer aptr);
static bool     mempressure_streq         (const char *s1, const char *s2);

/* ------------------------------------------------------------------------- *
 * MEMPRESSURE_STATUS
 * ------------------------------------------------------------------------- */

// static memnotify_level_t mempressure_status_evaluate_level(void);
// static bool              mempressure_status_update_level  (void);
// static void              mempressure_status_show_triggers (void);

/* ------------------------------------------------------------------------- *
 * MEMPRESSURE_CGROUP
 * ------------------------------------------------------------------------- */

// static bool     mempressure_cgroup_is_available     (void);
// static gboolean mempressure_cgroup_event_cb         (GIOChannel *chn, GIOCondition cnd, gpointer aptr);
// static void     mempressure_cgroup_quit             (void);
// static bool     mempressure_cgroup_init             (void);
// static bool     mempressure_cgroup_update_status    (void);
//

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

// /* Convert kernel reported byte count to page count used in configuration
//  */
// static int mempressure_bytes_to_pages(uint64_t bytes)
// {
//     return (int)(bytes / PAGE_SIZE);
// }
//
// /* Convert configuration page count to bytes for use in kernel interface
//  */
// static uint64_t mempressure_pages_to_bytes(int pages)
// {
//     if( pages < 0 )
//         pages = 0;
//     return PAGE_SIZE * (uint64_t)pages;
// }
//

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

/* Null tolerant string equality predicate
 *
 * @param s1  string
 * @param s2  string
 *
 * @return true if both s1 and s2 are null or same string, false otherwise
 */
static bool mempressure_streq(const char *s1, const char *s2)
{
    return (s1 && s2) ? !strcmp(s1, s2) : (s1 == s2);
}

/* ========================================================================= *
 * MEMPRESSURE_LIMIT
 * ========================================================================= */

// /** Reset limit object values
//  */
// static void
// mempressure_limit_clear(mempressure_limit_t *self)
// {
//     self->mnl_used = 0;
// }
//
// /** Limit validity predicate
//  */
// static bool
// mempressure_limit_is_valid(const mempressure_limit_t *self)
// {
//     return self->mnl_used > 0;
// }
//
// /** Convert limit object values to /dev/mempressure compatible ascii form
//  */
// static int
// mempressure_limit_repr(const mempressure_limit_t *self, char *data, size_t size)
// {
//     int res = snprintf(data, size, "used %d", self->mnl_used);
//     return res;
// }
//
// /** Parse limit object from /sys/fs/cgroup/memory/memory.usage_in_bytes format
//  */
// static bool
// mempressure_limit_parse(mempressure_limit_t *self, const char *data)
// {
//     char     *end = 0;
//     uint64_t  val = strtoull(data, &end, 10);
//     bool      res = end > data && *end == 0;
//
//     if( !res )
//         mce_log(LL_ERR, "parse error: '%s' is not a number", data);
//     else
//         self->mnl_used = mempressure_bytes_to_pages(val);
//
//     return res;
// }
//
// /** Check if limit object values are exceeded by given state data
//  */
// static bool
// mempressure_limit_exceeded(const mempressure_limit_t *self,
//                            const mempressure_limit_t *status)
// {
//     return (mempressure_limit_is_valid(self) &&
//             self->mnl_used <= status->mnl_used);
// }

/* ========================================================================= *
 * MEMPRESSURE_STATUS
 * ========================================================================= */

/** Configuration */
static gint mempressure_window = MCE_DEFAULT_MEMPRESSURE_WINDOW;
static gint mempressure_warning_stall = MCE_DEFAULT_MEMPRESSURE_WARNING_STALL;
static char* mempressure_warning_type = NULL;
static gint mempressure_critical_stall = MCE_DEFAULT_MEMPRESSURE_CRITICAL_STALL;
static char* mempressure_critical_type = NULL;

/** Cached memory use level */
static memnotify_level_t mempressure_level = MEMNOTIFY_LEVEL_UNKNOWN;

/** Re-evaluate memory use level and broadcast changes via datapipe
 */
// static bool
// mempressure_status_update_level(void)
// {
//     memnotify_level_t prev = mempressure_level;
//     mempressure_level = mempressure_status_evaluate_level();
//
//     if( mempressure_level == prev )
//         goto EXIT;
//
//     mce_log(LL_WARN, "mempressure_level: %s -> %s",
//             memnotify_level_repr(prev),
//             memnotify_level_repr(mempressure_level));
//
//     datapipe_exec_full(&memnotify_level_pipe,
//                        GINT_TO_POINTER(mempressure_level));
//
// EXIT:
//
//     return mempressure_level != MEMNOTIFY_LEVEL_UNKNOWN;
// }

/* ========================================================================= *
 * MEMPRESSURE_CGROUP
 * ========================================================================= */

/** File descriptor for CGROUP_DATA_PATH */
// static int   mempressure_cgroup_data_fd  = -1;

/** File descriptor for CGROUP_CTRL_PATH */
// static int   mempressure_cgroup_ctrl_fd  = -1;

/** Eventfd for receiving notifications about threshold crossings */
// static int   mempressure_cgroup_event_fd = -1;

/** I/O watch for mempressure_cgroup_event_fd */
// static guint mempressure_cgroup_event_id = 0;

/** Probe if the required PSI sysfs files are present
 */
static bool
mempressure_psi_is_available(void)
{
    return access(PSI_MEMORY_PATH, R_OK) == 0;
}

/** Input watch callback for cgroup memory threshold crossings
 */
// static gboolean
// mempressure_cgroup_event_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
// {
//     (void) chn;
//     (void) aptr;
//
//     gboolean ret = G_SOURCE_REMOVE;
//
//     if( !mempressure_cgroup_event_id )
//         goto EXIT;
//
//     if( mempressure_cgroup_event_fd == -1 )
//         goto EXIT;
//
//     if( mempressure_cgroup_data_fd == -1 )
//         goto EXIT;
//
//     mce_log(LL_DEBUG, "eventfd iowatch notify");
//
//     if( cnd & ~G_IO_IN ) {
//         mce_log(LL_ERR, "unexpected input watch condition");
//         goto EXIT;
//     }
//
//     uint64_t count = 0;
//     ssize_t rc = read(mempressure_cgroup_event_fd, &count, sizeof count);
//
//     if( rc == 0 ) {
//         mce_log(LL_ERR, "eventfd eof");
//         goto EXIT;
//     }
//
//     if( rc == -1 ) {
//         if( errno == EINTR || errno == EAGAIN )
//             ret = G_SOURCE_CONTINUE;
//         else
//             mce_log(LL_ERR, "eventfd error: %m");
//         goto EXIT;
//     }
//
//     if( mempressure_cgroup_update_status() )
//         ret = G_SOURCE_CONTINUE;
//
//     /* Update level anyway -> if we disable iowatch due to
//      * read/parse errors, the level gets reset to 'unknown'.
//      */
//     mempressure_status_update_level();
//
// EXIT:
//
//     if( ret == G_SOURCE_REMOVE && mempressure_cgroup_event_id ) {
//         mempressure_cgroup_event_id = 0;
//         mce_log(LL_CRIT, "disabling eventfd iowatch");
//     }
//
//     return ret;
// }
//
// /** Stop cgroup memory tracking
//  */
// static void
// mempressure_cgroup_quit(void)
// {
//     if( mempressure_cgroup_event_id ) {
//         mce_log(LL_DEBUG, "remove eventfd iowatch");
//         g_source_remove(mempressure_cgroup_event_id),
//             mempressure_cgroup_event_id = 0;
//     }
//
//     if( mempressure_cgroup_event_fd != -1 ) {
//         mce_log(LL_DEBUG, "close eventfd");
//         close(mempressure_cgroup_event_fd),
//             mempressure_cgroup_event_fd = -1;
//     }
//
//     if( mempressure_cgroup_ctrl_fd != -1 ) {
//         mce_log(LL_DEBUG, "close %s", CGROUP_CTRL_PATH);
//         close(mempressure_cgroup_ctrl_fd),
//             mempressure_cgroup_ctrl_fd = -1;
//     }
//
//     if( mempressure_cgroup_data_fd != -1 ) {
//         mce_log(LL_DEBUG, "close %s", CGROUP_DATA_PATH);
//         close(mempressure_cgroup_data_fd),
//             mempressure_cgroup_data_fd = -1;
//     }
// }

static int   mempressure_psi_warning_fd = -1;
static int   mempressure_psi_critical_fd = -1;
static guint mempressure_psi_warn_event_id = 0;
static guint mempressure_psi_crit_event_id = 0;
static guint mempressure_psi_warn_timeout = 0;
static guint mempressure_psi_crit_timeout = 0;

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
        mce_log(LL_INFO, "mempressure_level: %s -> %s",
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
    if ( fd == mempressure_psi_warning_fd ) {
        mce_log(LL_DEBUG, "PSI warning event timeout");
        mempressure_psi_warn_timeout = 0;
    } else if ( fd == mempressure_psi_critical_fd ) {
        mce_log(LL_DEBUG, "PSI critical event timeout");
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
        mce_log(LL_DEBUG, "warning PSI event");
        if (mempressure_psi_warn_timeout != 0) {
            g_source_remove(mempressure_psi_warn_timeout);
        }
        mempressure_psi_warn_timeout = g_timeout_add((mempressure_window / 1000) * 2,
                                                     mempressure_psi_timeout_cb, aptr);
    } else {
        assert(fd == mempressure_psi_critical_fd);
        mce_log(LL_DEBUG, "critical PSI event");
        if (mempressure_psi_crit_timeout != 0) {
            g_source_remove(mempressure_psi_crit_timeout);
        }
        mempressure_psi_crit_timeout = g_timeout_add((mempressure_window / 1000) * 2,
                                                     mempressure_psi_timeout_cb, aptr);
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
    char buf[1024];
    snprintf(buf, sizeof buf,
             "%s %d %d",
             mempressure_warning_type, mempressure_warning_stall, mempressure_window);

    mce_log(LL_DEBUG, "warning threshold: %s", buf);

    if (write(mempressure_psi_warning_fd, buf, strlen(buf) + 1) < 0) {
        mce_log(LL_ERR, "%s: write: %m", PSI_MEMORY_PATH);
        goto EXIT;
    }

    snprintf(buf, sizeof buf,
             "%s %d %d",
             mempressure_critical_type, mempressure_critical_stall, mempressure_window);

    mce_log(LL_DEBUG, "critical threshold: %s", buf);

    if (write(mempressure_psi_critical_fd, buf, strlen(buf) + 1) < 0) {
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

/** Set kernel side triggering levels and update current status
 */
static void
mempressure_psi_update_thresholds(void)
{
    /* TODO: Is there some way to remove trigger thresholds?
     *
     * Meanwhile do a full reinitialization to get rid of old thresholds.
     */
    mempressure_psi_quit();
    mempressure_psi_init();
}

/* ========================================================================= *
 * MEMPRESSURE_SETTING
 * ========================================================================= */

/** GConf notification id for mempressure.window */
static guint mempressure_setting_window_id = 0;

/** GConf notification id for mempressure.warning.stall time */
static guint mempressure_setting_warning_stall_id = 0;

/** GConf notification id for mempressure.warning.type */
static guint mempressure_setting_warning_type_id = 0;

/** GConf notification id for mempressure.critical.stall time */
static guint mempressure_setting_critical_stall_id = 0;

/** GConf notification id for mempressure.critical.type */
static guint mempressure_setting_critical_type_id = 0;

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
    } else if ( id == mempressure_setting_window_id ) {
        gint old = mempressure_window;
        gint val = gconf_value_get_int(gcv);
        if( old != val ) {
            mce_log(LL_DEBUG, "mempressure.window: %d -> %d", old, val);
            mempressure_window = val;
            mempressure_psi_update_thresholds();
        }
    } else if ( id == mempressure_setting_warning_stall_id ) {
        gint old = mempressure_warning_stall;
        gint val = gconf_value_get_int(gcv);
        if( old != val ) {
            mce_log(LL_DEBUG, "mempressure.warning.stall: %d -> %d", old, val);
            mempressure_warning_stall = val;
            mempressure_psi_update_thresholds();
        }
    } else if ( id == mempressure_setting_warning_type_id ) {
        char* old = mempressure_warning_type;
        const char* val = gconf_value_get_string(gcv);
        if( !mempressure_streq(old, val) ) {
            mce_log(LL_DEBUG, "mempressure.warning.type: %s -> %s", old, val);
            g_free(mempressure_warning_type);
            mempressure_warning_type = g_strdup(val);
            mempressure_psi_update_thresholds();
        }
    } else if ( id == mempressure_setting_critical_stall_id ) {
        gint old = mempressure_critical_stall;
        gint val = gconf_value_get_int(gcv);
        if( old != val ) {
            mce_log(LL_DEBUG, "mempressure.critical.stall: %d -> %d", old, val);
            mempressure_critical_stall = val;
            mempressure_psi_update_thresholds();
        }
    } else if ( id == mempressure_setting_critical_type_id ) {
        char* old = mempressure_critical_type;
        const char* val = gconf_value_get_string(gcv);
        if( !mempressure_streq(old, val) ) {
            mce_log(LL_DEBUG, "mempressure.critical.type: %s -> %s", old, val);
            g_free(mempressure_critical_type);
            mempressure_critical_type = g_strdup(val);
            mempressure_psi_update_thresholds();
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
    mce_setting_notifier_add(MCE_SETTING_MEMPRESSURE_PATH,
                             MCE_SETTING_MEMPRESSURE_WINDOW,
                             mempressure_setting_cb,
                             &mempressure_setting_window_id);

    mce_setting_get_int(MCE_SETTING_MEMPRESSURE_WINDOW,
                        &mempressure_window);

    mce_setting_notifier_add(MCE_SETTING_MEMPRESSURE_WARNING_PATH,
                             MCE_SETTING_MEMPRESSURE_WARNING_STALL,
                             mempressure_setting_cb,
                             &mempressure_setting_warning_stall_id);

    mce_setting_get_int(MCE_SETTING_MEMPRESSURE_WARNING_STALL,
                        &mempressure_warning_stall);

    mce_setting_notifier_add(MCE_SETTING_MEMPRESSURE_WARNING_PATH,
                             MCE_SETTING_MEMPRESSURE_WARNING_TYPE,
                             mempressure_setting_cb,
                             &mempressure_setting_warning_type_id);

    mce_setting_get_string(MCE_SETTING_MEMPRESSURE_WARNING_TYPE,
                           &mempressure_warning_type);

    if ( mempressure_warning_type == NULL ) {
        mempressure_warning_type = g_strdup(MCE_DEFAULT_MEMPRESSURE_WARNING_TYPE);
    }

    mce_setting_notifier_add(MCE_SETTING_MEMPRESSURE_CRITICAL_PATH,
                             MCE_SETTING_MEMPRESSURE_CRITICAL_STALL,
                             mempressure_setting_cb,
                             &mempressure_setting_critical_stall_id);

    mce_setting_get_int(MCE_SETTING_MEMPRESSURE_CRITICAL_STALL,
                        &mempressure_critical_stall);

    mce_setting_notifier_add(MCE_SETTING_MEMPRESSURE_CRITICAL_PATH,
                             MCE_SETTING_MEMPRESSURE_CRITICAL_TYPE,
                             mempressure_setting_cb,
                             &mempressure_setting_critical_type_id);

    mce_setting_get_string(MCE_SETTING_MEMPRESSURE_CRITICAL_TYPE,
                           &mempressure_critical_type);

    if ( mempressure_critical_type == NULL ) {
        mempressure_critical_type = g_strdup(MCE_DEFAULT_MEMPRESSURE_CRITICAL_TYPE);
    }
}

/** Stop tracking setting changes
 */
static void mempressure_setting_quit(void)
{
    mce_setting_notifier_remove(mempressure_setting_window_id),
        mempressure_setting_window_id = 0;

    mce_setting_notifier_remove(mempressure_setting_warning_stall_id),
        mempressure_setting_warning_stall_id = 0;

    mce_setting_notifier_remove(mempressure_setting_warning_type_id),
        mempressure_setting_warning_type_id = 0;

    g_free(mempressure_warning_type);
    mempressure_warning_type = NULL;

    mce_setting_notifier_remove(mempressure_setting_critical_stall_id),
        mempressure_setting_critical_stall_id = 0;

    mce_setting_notifier_remove(mempressure_setting_critical_type_id),
        mempressure_setting_critical_type_id = 0;

    g_free(mempressure_critical_type);
    mempressure_critical_type = NULL;
}

/* ========================================================================= *
 * MEMPRESSURE_PLUGIN
 * ========================================================================= */

static void
mempressure_plugin_quit(void)
{
    mempressure_psi_quit();
    mempressure_setting_quit();
}

static bool
mempressure_plugin_init(void)
{
    bool success = false;

    mempressure_setting_init();

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
    if( !mempressure_psi_is_available() ) {
        mce_log(LL_WARN, "mempressure psi interface not available");
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
