/**
 * @file memnotify.c
 * Memory use tracking and notification plugin for the Mode Control Entity
 * <p>
 * Copyright (C) 2014 Jolla Ltd.
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

#include "../mce-log.h"
#include "../mce-dbus.h"
#include "../mce-setting.h"

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include <gmodule.h>

/* ========================================================================= *
 * GENERIC_UTILITIES
 * ========================================================================= */

static int    memnotify_char_is_black  (int ch);
static int    memnotify_char_is_white  (int ch);
static char  *memnotify_token_parse    (char **ppos);
static guint  memnotify_iowatch_add    (int fd, bool close_on_unref, GIOCondition cnd, GIOFunc io_cb, gpointer aptr);

/* ========================================================================= *
 * MEMORY_LEVELS
 * ========================================================================= */

/** Supported memory usage levels
 *
 * Note: The ordering must match:
 *       1) memnotify_limit[] array
 *       2) memnotify_dev[] array
 */
typedef enum
{
    /** No excess memory pressure */
    MEMNOTIFY_LEVEL_NORMAL,

    /** Non-essential caches etc should be released */
    MEMNOTIFY_LEVEL_WARNING,

    /** Non-essential prosesses should be terminated */
    MEMNOTIFY_LEVEL_CRITICAL,

    /* Not initialized yet or memnotify is not supported */
    MEMNOTIFY_LEVEL_UNKNOWN,

    MEMNOTIFY_LEVEL_COUNT
} memnotify_level_t;

static const char *memnotify_level_name(memnotify_level_t lev);

/* ========================================================================= *
 * LIMIT_OBJECTS
 * ========================================================================= */

/** Structure for holding for /dev/memnotify compatible limit data */
typedef struct
{
    /** Estimate of number of non-discardable RAM pages */
    gint        mnl_used;

    /** Number of active RAM pages (TODO: what is it?) */
    gint        mnl_active;

    /** Number of RAM pages the system has*/
    gint        mnl_total;

} memnotify_limit_t;

static void memnotify_limit_clear    (memnotify_limit_t *self);
static int  memnotify_limit_repr     (const memnotify_limit_t *self, char *data, size_t size);
static bool memnotify_limit_parse    (memnotify_limit_t *self, const char *data);
static bool memnotify_limit_exceeded (const memnotify_limit_t *self, const memnotify_limit_t *that);

/* ========================================================================= *
 * STATUS_EVALUATION
 * ========================================================================= */

static memnotify_level_t memnotify_status_evaluate_level  (void);
static void              memnotify_status_update_level    (void);
static void              memnotify_status_update_triggers (void);
static void              memnotify_status_show_triggers   (void);

/* ========================================================================= *
 * KERNEL_INTERFACE
 * ========================================================================= */

/** Structure for holding memnotify device file descriptors etc */
typedef struct
{
    /** Flag for: Slot is not a dummy
     *
     * The structures are allocated per notification level. To allow
     * dummy padding slots to remain zero initialized, this flag must
     * be set to true for slots that are actually used. */
    bool  mnd_in_use;

    /** Device file descriptor for /dev/memnotify
     *
     * If mnd_in_use is true, must be initialized to -1.
     */
    int   mnd_fd;

    /** Glib io watch id for mnd_fd
     *
     * If mnd_in_use is true, must be initialized to 0.
     */
    guint mnd_rx_id;
} memnotify_dev_t;

static bool     memnotify_dev_is_available (void);

static gboolean memnotify_dev_rx_cb        (GIOChannel *chn, GIOCondition cnd, gpointer aptr);

static void     memnotify_dev_close        (memnotify_level_t lev);
static bool     memnotify_dev_open         (memnotify_level_t lev);

static bool     memnotify_dev_set_trigger  (memnotify_level_t lev, const memnotify_limit_t *limit);
static bool     memnotify_dev_get_status   (memnotify_level_t lev, memnotify_limit_t *state);

/* ========================================================================= *
 * DYNAMIC_SETTINGS
 * ========================================================================= */

static void memnotify_setting_cb   (GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);
static void memnotify_setting_init (void);
static void memnotify_setting_quit (void);

/* ========================================================================= *
 * DBUS_INTERFACE
 * ========================================================================= */

static void memnotify_dbus_broadcast_level (void);
static void memnotify_dbus_init            (void);
static void memnotify_dbus_quit            (void);

/* ========================================================================= *
 * PLUGIN_INTEFACE
 * ========================================================================= */

G_MODULE_EXPORT const gchar *g_module_check_init (GModule *module);
G_MODULE_EXPORT void         g_module_unload     (GModule *module);

/* ========================================================================= *
 * GENERIC_UTILITIES
 * ========================================================================= */

/** Simple locale agnostic whitespace character predicate
 */
static  int
memnotify_char_is_white(int ch)
{
    return (ch > 0) && (ch <= 32);
}

/** Simple locale agnostic non-white character predicate
 */
static int
memnotify_char_is_black(int ch)
{
    return (ch > 32);
}

/** Slice the next sequence of non-white characters from parse position
 */
static char *
memnotify_token_parse(char **ppos)
{
    unsigned char *pos = (unsigned char *)*ppos;

    // skip leading white space
    while( memnotify_char_is_white(*pos) )
        ++pos;

    // find non-white part
    unsigned char *res = pos;

    while( memnotify_char_is_black(*pos) )
        ++pos;

    if( *pos )
        *pos++ = 0;

    // skip trailing white space

    while( memnotify_char_is_white(*pos) )
        ++pos;

    return *ppos = (char *)pos, (char *)res;
}

/** Add a glib I/O notification for a file descriptor
 */
static guint
memnotify_iowatch_add(int fd, bool close_on_unref,
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
 * MEMORY_LEVELS
 * ========================================================================= */

/** Translate level enum to human readable string
 *
 * Note: Also used as argument for the change signal and thus
 *       changes here can cause API breaks.
 */
static const char *
memnotify_level_name(memnotify_level_t lev)
{
    static const char * const lut[MEMNOTIFY_LEVEL_COUNT] =
    {
        [MEMNOTIFY_LEVEL_NORMAL]   = MCE_MEMORY_LEVEL_NORMAL,
        [MEMNOTIFY_LEVEL_WARNING]  = MCE_MEMORY_LEVEL_WARNING,
        [MEMNOTIFY_LEVEL_CRITICAL] = MCE_MEMORY_LEVEL_CRITICAL,
        [MEMNOTIFY_LEVEL_UNKNOWN]  = MCE_MEMORY_LEVEL_UNKNOWN,
    };

    return (lev < MEMNOTIFY_LEVEL_COUNT) ? lut[lev] : "undefined";
}

/* ========================================================================= *
 * LIMIT_OBJECTS
 * ========================================================================= */

/** Reset limit object values
 */
static void
memnotify_limit_clear(memnotify_limit_t *self)
{
    self->mnl_used   = 0;
    self->mnl_active = 0;
    self->mnl_total  = 0;
}

/** Convert limit object values to /dev/memnotify compatible ascii form
 */
static int
memnotify_limit_repr(const memnotify_limit_t *self, char *data, size_t size)
{
    int res = snprintf(data, size, "used %d active %d total %d",
                       self->mnl_used,
                       self->mnl_active,
                       self->mnl_total);
    return res;
}

/** Parse limit object from /dev/memnotify compatible ascii form
 */
static bool
memnotify_limit_parse(memnotify_limit_t *self, const char *data)
{
    bool  res = false;
    char *tmp = 0;

    if( !self )
        goto EXIT;

    memnotify_limit_clear(self);

    if( !data )
        goto EXIT;

    if( !(tmp = strdup(data)) )
        goto EXIT;

    res = true;

    for( char *pos = tmp; *pos; ) {
        char *key = memnotify_token_parse(&pos);
        char *val = memnotify_token_parse(&pos);
        char *end = 0;
        gint  num = (gint)strtol(val, &end, 0);

        if( *key == 0 )
            continue;

        if( end <= val || *end != 0 ) {
            mce_log(LL_WARN, "%s: '%s' is not a number", key, val);
            continue;
        }

        if( !strcmp(key, "used") ) {
            self->mnl_used = num;
        }
        else if( !strcmp(key, "active") ) {
            self->mnl_active = num;
        }
        else if( !strcmp(key, "total") ) {
            self->mnl_total = num;
        }
        else {
            mce_log(LL_DEBUG, "%s: unknown value", key);
        }
    }

EXIT:

    free(tmp);

    return res;
}

/** Check if limit object values are exceeded by given state data
 */
static bool
memnotify_limit_exceeded(const memnotify_limit_t *self,
                         const memnotify_limit_t *state)
{
    // limit <= state

#define X(memb) (self->memb != 0 && self->memb <= state->memb)

    bool res = (X(mnl_used) || X(mnl_active) || X(mnl_total));

#undef X

    return res;
}

/* ========================================================================= *
 * STATUS_EVALUATION
 * ========================================================================= */

/** Configuration limits for normal/warning/critical levels */
static memnotify_limit_t memnotify_limit[] =
{
    [MEMNOTIFY_LEVEL_NORMAL] = {
        .mnl_used   = 0,
        .mnl_active = 0,
        .mnl_total  = 0,
    },
    [MEMNOTIFY_LEVEL_WARNING] = {
        // values come from config - disabled by default
        .mnl_used   = 0,
        .mnl_active = 0,
        .mnl_total  = 0,
    },
    [MEMNOTIFY_LEVEL_CRITICAL] = {
        // values come from config - disabled by default
        .mnl_used   = 0,
        .mnl_active = 0,
        .mnl_total  = 0,
    },
};

/** Cached status read from kernel device */
static memnotify_limit_t memnotify_state =
{
    .mnl_used   = 0,
    .mnl_active = 0,
    .mnl_total  = 0,
};

/** Cached memory use level */
static memnotify_level_t memnotify_level = MEMNOTIFY_LEVEL_UNKNOWN;

/** Check current memory status against triggering levels
 */
static memnotify_level_t
memnotify_status_evaluate_level(void)
{
    memnotify_level_t res = MEMNOTIFY_LEVEL_NORMAL;
    memnotify_level_t lev = MEMNOTIFY_LEVEL_NORMAL + 1;
    for( ; lev < G_N_ELEMENTS(memnotify_limit); ++lev ) {
        if( memnotify_limit_exceeded(memnotify_limit+lev, &memnotify_state) )
            res = lev;
    }
    return res;
}

/** Re-evaluate memory use level and broadcast dbus signal if changed
 */
static void
memnotify_status_update_level(void)
{
    memnotify_level_t level = memnotify_status_evaluate_level();

    if( memnotify_level == level )
        goto EXIT;

    memnotify_level = level;

    memnotify_dbus_broadcast_level();

EXIT:

    return;
}

/** Set kernel side triggering levels and update current status
 */
static void
memnotify_status_update_triggers(void)
{
    /* Program new limits to kernel side */
    memnotify_dev_set_trigger(MEMNOTIFY_LEVEL_WARNING,
                              memnotify_limit + MEMNOTIFY_LEVEL_WARNING);

    memnotify_dev_set_trigger(MEMNOTIFY_LEVEL_CRITICAL,
                              memnotify_limit + MEMNOTIFY_LEVEL_CRITICAL);

    /* Read current status and re-evaluate level
     *
     * The MEMNOTIFY_LEVEL_WARNING is just a slot for which we should
     * have an open /dev/memnotify file descriptor.
     */
    if( memnotify_dev_get_status(MEMNOTIFY_LEVEL_WARNING, &memnotify_state) )
        memnotify_status_update_level();
}

/** Log current memory level configuration for debugging purposes
 */
static void
memnotify_status_show_triggers(void)
{
    for( size_t i = 0; i < G_N_ELEMENTS(memnotify_limit); ++i ) {
        char tmp[256];
        memnotify_limit_repr(memnotify_limit+i, tmp, sizeof tmp);
        mce_log(LL_DEBUG, "%s: %s", memnotify_level_name(i), tmp);
    }
}

/* ========================================================================= *
 * KERNEL_INTERFACE
 * ========================================================================= */

/** Path to memonotify device node */
static const char memnotify_dev_path[] = "/dev/memnotify";

/** Tracking data for open /dev/memnotify instances */
static memnotify_dev_t memnotify_dev[MEMNOTIFY_LEVEL_COUNT] =
{
    [MEMNOTIFY_LEVEL_WARNING] = {
        .mnd_in_use = true,
        .mnd_fd     = -1,
        .mnd_rx_id  = 0,
    },
    [MEMNOTIFY_LEVEL_CRITICAL] = {
        .mnd_in_use = true,
        .mnd_fd     = -1,
        .mnd_rx_id  = 0,
    },

    /* Note: Any uninitialized slots will have mnd_in_use==false and
     *       are ignored by memnotify_dev_xxx() functions. */
};

/** Probe if the memnotify device node is present
 */
static bool
memnotify_dev_is_available(void)
{
    return access(memnotify_dev_path, R_OK|W_OK) == 0;
}

/** Input watch callback for memonotify device node
 */
static gboolean
memnotify_dev_rx_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
{
    (void) chn;
    (void) cnd;

    memnotify_level_t lev = GPOINTER_TO_INT(aptr);

    gboolean keep_going = FALSE;

    if( !memnotify_dev[lev].mnd_rx_id )
        goto EXIT;

    mce_log(LL_DEBUG, "notify trigger (%s)", memnotify_level_name(lev));

    if( cnd & ~G_IO_IN ) {
        mce_log(LL_WARN, "unexpected input watch condition");
        goto EXIT;
    }

    if( !memnotify_dev_get_status(lev, &memnotify_state) )
        goto EXIT;

    keep_going = TRUE;

    memnotify_status_update_level();

EXIT:

    if( !keep_going && memnotify_dev[lev].mnd_rx_id ) {
        memnotify_dev[lev].mnd_rx_id = 0;
        mce_log(LL_CRIT, "disabling input watch");
    }
    return keep_going;
}

/** Close memonotify device node and remove associated io watch
 */
static void
memnotify_dev_close(memnotify_level_t lev)
{
    if( !memnotify_dev[lev].mnd_in_use )
        goto EXIT;

    if( memnotify_dev[lev].mnd_rx_id ) {
        g_source_remove(memnotify_dev[lev].mnd_rx_id),
            memnotify_dev[lev].mnd_rx_id = 0;
    }

    if( memnotify_dev[lev].mnd_fd != -1 ) {
        close(memnotify_dev[lev].mnd_fd),
            memnotify_dev[lev].mnd_fd = -1;
    }

EXIT:

    return;
}

/** Open memonotify device node and install io watch for it
 */
static bool
memnotify_dev_open(memnotify_level_t lev)
{
    bool res = false;

    if( !memnotify_dev[lev].mnd_in_use )
        goto EXIT;

    if( (memnotify_dev[lev].mnd_fd = open(memnotify_dev_path, O_RDWR)) == -1 ) {
        mce_log(LL_ERR, "could not open: %s: %m", memnotify_dev_path);
        goto EXIT;
    }

    memnotify_dev[lev].mnd_rx_id =
        memnotify_iowatch_add(memnotify_dev[lev].mnd_fd,
                              false,
                              G_IO_IN,
                              memnotify_dev_rx_cb,
                              GINT_TO_POINTER(lev));

    if( !memnotify_dev[lev].mnd_rx_id ) {
        mce_log(LL_ERR, "could add iowatch: %s", memnotify_dev_path);
        goto EXIT;
    }

    if( !memnotify_dev_get_status(lev, &memnotify_state) )
        goto EXIT;

    res = true;

    memnotify_status_update_level();

EXIT:

    // all or nothing
    if( !res )
        memnotify_dev_close(lev);

    return res;
}

static void
memnotify_dev_close_all(void)
{
    for( memnotify_level_t lev = 0; lev < MEMNOTIFY_LEVEL_COUNT; ++lev )
        memnotify_dev_close(lev);
}

static bool
memnotify_dev_open_all(void)
{
    bool res = false;

    for( memnotify_level_t lev = 0; lev < MEMNOTIFY_LEVEL_COUNT; ++lev ) {
        if( !memnotify_dev[lev].mnd_in_use )
            continue;
        if( !memnotify_dev_open(lev) )
            goto EXIT;
    }

    res = true;

EXIT:

    // all or nothing
    if( !res )
        memnotify_dev_close_all();

    return res;
}

/** Program kernel side memory use notification limits
 */
static bool
memnotify_dev_set_trigger(memnotify_level_t lev, const memnotify_limit_t *limit)
{
    bool res = false;

    char tmp[256];

    if( memnotify_dev[lev].mnd_fd == -1 )
        goto EXIT;

    int todo = memnotify_limit_repr(limit, tmp, sizeof tmp);
    if( todo <= 0 )
        goto EXIT;

    int done = write(memnotify_dev[lev].mnd_fd, tmp, todo);
    if( done != todo )
        goto EXIT;

    mce_log(LL_DEBUG, "write %s -> %s", memnotify_level_name(lev), tmp);

    res = true;

EXIT:

    return res;
}

/** Read current memory use status from kernel side
 */
static bool
memnotify_dev_get_status(memnotify_level_t lev, memnotify_limit_t *state)
{
    bool res = false;

    char tmp[256];

    if( memnotify_dev[lev].mnd_fd == -1 ) {
        mce_log(LL_WARN, "device not opened");
        goto EXIT;
    }

    errno = 0;

    int done = read(memnotify_dev[lev].mnd_fd, tmp, sizeof tmp - 1);
    if( done <= 0 ) {
        mce_log(LL_ERR, "no data: %m");
        goto EXIT;
    }

    tmp[done-1] = 0;

    mce_log(LL_DEBUG, "read %s <- %s", memnotify_level_name(lev), tmp);

    if( !memnotify_limit_parse(state, tmp) )
        goto EXIT;

    res = true;

EXIT:

    return res;
}

/* ========================================================================= *
 * DYNAMIC_SETTINGS
 * ========================================================================= */

/** GConf notification id for memnotify.warning.used level */
static guint memnotify_setting_warning_used_id = 0;

/** GConf notification id for memnotify.warning.active level */
static guint memnotify_setting_warning_active_id = 0;

/** GConf notification id for memnotify.critical.used level */
static guint memnotify_setting_critical_used_id = 0;

/** GConf notification id for memnotify.critical.active level */
static guint memnotify_setting_critical_active_id = 0;

/** GConf callback for memnotify related settings
 *
 * @param gcc    (not used)
 * @param id     Connection ID from gconf_client_notify_add()
 * @param entry  The modified GConf entry
 * @param data   (not used)
 */
static void
memnotify_setting_cb(GConfClient *const gcc, const guint id,
                     GConfEntry *const entry, gpointer const data)
{
    const GConfValue *gcv = gconf_entry_get_value(entry);

    (void)gcc;
    (void)data;

    /* Key is unset */
    if (gcv == NULL) {
        mce_log(LL_DEBUG, "GConf Key `%s' has been unset",
                gconf_entry_get_key(entry));
        goto EXIT;
    }

    if( id == memnotify_setting_warning_used_id ) {
        gint old = memnotify_limit[MEMNOTIFY_LEVEL_WARNING].mnl_used;
        gint val = gconf_value_get_int(gcv);
        if( old != val ) {
            mce_log(LL_DEBUG, "memnotify.warning.used: %d -> %d", old, val);
            memnotify_limit[MEMNOTIFY_LEVEL_WARNING].mnl_used = val;
            memnotify_status_update_triggers();
        }
    }
    else if( id == memnotify_setting_warning_active_id ) {
        gint old = memnotify_limit[MEMNOTIFY_LEVEL_WARNING].mnl_active;
        gint val = gconf_value_get_int(gcv);
        if( old != val ) {
            mce_log(LL_DEBUG, "memnotify.warning.active: %d -> %d", old, val);
            memnotify_limit[MEMNOTIFY_LEVEL_WARNING].mnl_active = val;
            memnotify_status_update_triggers();
        }
    }
    else if( id == memnotify_setting_critical_used_id ) {
        gint old = memnotify_limit[MEMNOTIFY_LEVEL_CRITICAL].mnl_used;
        gint val = gconf_value_get_int(gcv);
        if( old != val ) {
            mce_log(LL_DEBUG, "memnotify.critical.used: %d -> %d", old, val);
            memnotify_limit[MEMNOTIFY_LEVEL_CRITICAL].mnl_used = val;
            memnotify_status_update_triggers();
        }
    }
    else if( id == memnotify_setting_critical_active_id ) {
        gint old = memnotify_limit[MEMNOTIFY_LEVEL_CRITICAL].mnl_active;
        gint val = gconf_value_get_int(gcv);
        if( old != val ) {
            mce_log(LL_DEBUG, "memnotify.critical.active: %d -> %d", old, val);
            memnotify_limit[MEMNOTIFY_LEVEL_CRITICAL].mnl_active = val;
            memnotify_status_update_triggers();
        }
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }

EXIT:

    return;
}

/** Get initial setting values and start tracking changes
 */
static void memnotify_setting_init(void)
{
    /* memnotify.warning.used level */
    mce_setting_notifier_add(MCE_SETTING_MEMNOTIFY_WARNING_PATH,
                             MCE_SETTING_MEMNOTIFY_WARNING_USED,
                             memnotify_setting_cb,
                             &memnotify_setting_warning_used_id);

    mce_setting_get_int(MCE_SETTING_MEMNOTIFY_WARNING_USED,
                        &memnotify_limit[MEMNOTIFY_LEVEL_WARNING].mnl_used);

    /* memnotify.warning.active level */
    mce_setting_notifier_add(MCE_SETTING_MEMNOTIFY_WARNING_PATH,
                             MCE_SETTING_MEMNOTIFY_WARNING_ACTIVE,
                             memnotify_setting_cb,
                             &memnotify_setting_warning_active_id);

    mce_setting_get_int(MCE_SETTING_MEMNOTIFY_WARNING_ACTIVE,
                        &memnotify_limit[MEMNOTIFY_LEVEL_WARNING].mnl_active);

    /* memnotify.critical.used level */
    mce_setting_notifier_add(MCE_SETTING_MEMNOTIFY_CRITICAL_PATH,
                             MCE_SETTING_MEMNOTIFY_CRITICAL_USED,
                             memnotify_setting_cb,
                             &memnotify_setting_critical_used_id);

    mce_setting_get_int(MCE_SETTING_MEMNOTIFY_CRITICAL_USED,
                        &memnotify_limit[MEMNOTIFY_LEVEL_CRITICAL].mnl_used);

    /* memnotify.critical.active level */
    mce_setting_notifier_add(MCE_SETTING_MEMNOTIFY_CRITICAL_PATH,
                             MCE_SETTING_MEMNOTIFY_CRITICAL_ACTIVE,
                             memnotify_setting_cb,
                             &memnotify_setting_critical_active_id);

    mce_setting_get_int(MCE_SETTING_MEMNOTIFY_CRITICAL_ACTIVE,
                        &memnotify_limit[MEMNOTIFY_LEVEL_CRITICAL].mnl_active);

    memnotify_status_show_triggers();
}

/** Stop tracking setting changes
 */
static void memnotify_setting_quit(void)
{
    mce_setting_notifier_remove(memnotify_setting_warning_used_id),
        memnotify_setting_warning_used_id = 0;

    mce_setting_notifier_remove(memnotify_setting_warning_active_id),
        memnotify_setting_warning_active_id = 0;

    mce_setting_notifier_remove(memnotify_setting_critical_used_id),
        memnotify_setting_critical_used_id = 0;

    mce_setting_notifier_remove(memnotify_setting_critical_active_id),
        memnotify_setting_critical_active_id = 0;
}

/* ========================================================================= *
 * DBUS_INTERFACE
 * ========================================================================= */

/** Send memory use level signal on system bus
 */
static void
memnotify_dbus_broadcast_level(void)
{
    const char *sig = MCE_MEMORY_LEVEL_SIG;
    const char *arg = memnotify_level_name(memnotify_level);
    mce_log(LL_DEVEL, "sending dbus signal: %s %s", sig, arg);
    dbus_send(0, MCE_SIGNAL_PATH, MCE_SIGNAL_IF, sig, 0,
              DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
}

/** D-Bus callback for the get memory level method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
memnotify_dbus_get_level_cb(DBusMessage *const req)
{
    mce_log(LL_DEVEL, "Received memory leve get request from %s",
            mce_dbus_get_message_sender_ident(req));

    DBusMessage *rsp = dbus_new_method_reply(req);
    const char  *arg = memnotify_level_name(memnotify_level);

    if( !dbus_message_append_args(rsp,
                                  DBUS_TYPE_STRING, &arg,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    mce_log(LL_DEBUG, "sending memory level reply: %s", arg);

    dbus_send_message(rsp), rsp = 0;

EXIT:
    if( rsp )
        dbus_message_unref(rsp);

    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t memnotify_dbus_handlers[] =
{
    /* signals - outbound (for Introspect purposes only) */
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_MEMORY_LEVEL_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"memory_level\" type=\"s\"/>\n"
    },
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_MEMORY_LEVEL_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = memnotify_dbus_get_level_cb,
        .args      =
            "    <arg direction=\"out\" name=\"memory_level\" type=\"s\"/>\n"
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Add dbus handlers
 */
static void memnotify_dbus_init(void)
{
    mce_dbus_handler_register_array(memnotify_dbus_handlers);
}

/** Remove dbus handlers
 */
static void memnotify_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(memnotify_dbus_handlers);
}

/* ========================================================================= *
 * PLUGIN_INTEFACE
 * ========================================================================= */

/** Init function for the memnotify plugin
 *
 * @param module  (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    memnotify_dbus_init();
    memnotify_setting_init();

    /* Do not even attempt to set up tracking if the memnotify
     * device node is not available */
    if( !memnotify_dev_is_available() ) {
        /* Since it is expectional that  /dev/memnotify is present,
         * we must not complain about it missing in default verbosity
         * level
         */
        mce_log(LL_NOTICE, "memnotify not available");

        /* The plugin stays loaded, but no signals are emitted and
         * level query will return "unknown". */
        goto EXIT;
    }

    if( !memnotify_dev_open_all() )
        goto EXIT;

    memnotify_status_update_triggers();

    mce_log(LL_NOTICE, "memnotify plugin active");

EXIT:

    return NULL;
}

/** Exit function for the memnotify plugin
 *
 * @param module  (not used)
 */
void g_module_unload(GModule *module)
{
    (void)module;

    mce_log(LL_DEBUG, "unloading memnotify plugin");

    memnotify_setting_quit();
    memnotify_dbus_quit();
    memnotify_dev_close_all();

    return;
}
