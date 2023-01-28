/** @file dummy_compositor.c
 *
 * Tool for creating a mid compositor hand off stop gap
 * <p>
 * Copyright (c) 2019 - 2023 Jolla Ltd.
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

#include "../mce-dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <getopt.h>

#include <glib.h>

#include <dbus/dbus.h>
#include "../dbus-gmain/dbus-gmain.h"

/* ========================================================================= *
 * Constants
 * ========================================================================= */

/** How long to delay exit after succesful D-Bus name acquisition */
#define DC_EXIT_DELAY_MS 500

/* ========================================================================= *
 * Macros
 * ========================================================================= */

#define dc_log_emit(LEV,FMT,ARGS...)\
     do { \
       if( dc_log_p(LEV) ) {\
         dc_log_emit_real(LEV, FMT, ##ARGS);\
       }\
     } while(0)

#define dc_log_crit(  FMT,ARGS...) dc_log_emit(LOG_CRIT,    FMT, ##ARGS)
#define dc_log_err(   FMT,ARGS...) dc_log_emit(LOG_ERR,     FMT, ##ARGS)
#define dc_log_warn(  FMT,ARGS...) dc_log_emit(LOG_WARNING, FMT, ##ARGS)
#define dc_log_notice(FMT,ARGS...) dc_log_emit(LOG_NOTICE,  FMT, ##ARGS)
#define dc_log_info(  FMT,ARGS...) dc_log_emit(LOG_INFO,    FMT, ##ARGS)
#define dc_log_debug( FMT,ARGS...) dc_log_emit(LOG_DEBUG,   FMT, ##ARGS)

/* ========================================================================= *
 * Types
 * ========================================================================= */

/** Diagnostic logging targets */
typedef enum
{
    /** Use syslog() */
    DC_LOG_TO_SYSLOG = 0,

    /** Write to stderr */
    DC_LOG_TO_STDERR = 1,
} dc_log_to_t;

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * UTILITY
 * ------------------------------------------------------------------------- */

static const char *dc_name_repr    (const char *name);
static const char *dc_bool_repr    (bool val);
static bool        dc_equal_p      (const char *s1, const char *s2);
static void        dc_print_usage  (void);
static void        dc_print_version(void);

/* ------------------------------------------------------------------------- *
 * DC_LOG
 * ------------------------------------------------------------------------- */

static int         dc_log_clip_level(int level);
static const char *dc_log_get_name  (void);
static void        dc_log_set_name  (const char *name);
static const char *dc_log_level_repr(int level);
static int         dc_log_get_level (void);
static void        dc_log_set_level (int level);
static bool        dc_log_p         (int level);
static void        dc_log_emit_real (int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ------------------------------------------------------------------------- *
 * DC_DBUS
 * ------------------------------------------------------------------------- */

static bool               dc_dbus_parse_message                            (DBusMessage *msg, int type, ...);
static void               dc_dbus_handle_name_lost_signal                  (DBusMessage *sig);
static void               dc_dbus_handle_name_acquired_signal              (DBusMessage *sig);
static void               dc_dbus_handle_name_owner_changed_signal         (DBusMessage *sig);
static void               dc_dbus_handle_disconnected_signal               (DBusMessage *sig);
static DBusMessage       *dc_dbus_handle_set_updates_enabled_method_call   (DBusMessage *req);
static DBusMessage       *dc_dbus_handle_get_topmost_window_pid_method_call(DBusMessage *req);
static DBusHandlerResult  dc_dbus_message_filter_cb                        (DBusConnection *con, DBusMessage *msg, void *aptr);
static bool               dc_dbus_connect                                  (void);
static bool               dc_dbus_connected                                (void);
static void               dc_dbus_disconnect                               (void);
static bool               dc_dbus_reserve_name                             (void);
static bool               dc_dbus_release_name                             (void);

/* ------------------------------------------------------------------------- *
 * DC_MAINLOOP
 * ------------------------------------------------------------------------- */

int             dc_mainloop_run                  (void);
void            dc_mainloop_exit                 (int exit_code);
static gboolean dc_mainloop_delayed_exit_cb      (gpointer aptr);
static void     dc_mainloop_schedule_delayed_exit(void);
static void     dc_mainloop_cancel_delayed_exit  (void);

/* ------------------------------------------------------------------------- *
 * MAIN
 * ------------------------------------------------------------------------- */

int main(int ac, char **av);

/* ========================================================================= *
 * UTILITY
 * ========================================================================= */

/** Human readable representation for D-Bus names
 *
 * @param name  D-Bus name string, or NULL
 *
 * @return Valid human readable C-string
 */
static const char *
dc_name_repr(const char *name)
{
    return !name ? "<null>" : !*name ? "<none>" : name;
}

/** Human readable representation for boolean values
 *
 * @param val  Boolean value
 *
 * @return Human readable C-string
 */
static const char *
dc_bool_repr(bool val)
{
    return val ? "true" : "false";
}

/** String equality predicate
 *
 * @param s1  First string, or NULL
 * @param s2  Second string. or NULL
 *
 * @return true if both strings are non-null and equal, false otherwise
 */
static bool
dc_equal_p(const char *s1, const char *s2)
{
    return s1 && s2 && !strcmp(s1, s2);
}

/* ========================================================================= *
 * DC_LOG
 * ========================================================================= */

/** Cached application name */
static const char *dc_log_name  = 0;

/** Currently chosen verbosity level */
static int         dc_log_level = LOG_WARNING;

/** Currently chosen diagnostic logging target */
static dc_log_to_t dc_log_to    = DC_LOG_TO_SYSLOG;

/** Normalize logging verbosity level to expected range
 *
 * @param level  syslog() compatible logging levels
 *
 * @return the given level clipped to [LOG_CRIT ... LOG_DEBUG] range
 */
static int
dc_log_clip_level(int level)
{
    if( level < LOG_CRIT )
        level = LOG_CRIT;
    else if( level > LOG_DEBUG )
        level = LOG_DEBUG;
    return level;
}

/** Get application name to use for diagnostic logging
 *
 * @returns value set via #dc_log_set_name(), or "unnamed"
 */
static const char *
dc_log_get_name(void)
{
    return dc_log_name ?: "unnamed";
}

/** Set application name to use for diagnostic logging
 *
 * @note The name is used as is and thus must be valid for the whole
 *       lifetime of the process / until changed.
 *
 * @param name  name string
 */
static void
dc_log_set_name(const char *name)
{
    dc_log_name = name;
}

/** Get verbosity level indicator tag for use in diagnostic logging
 *
 * @param level  Message verbosity level
 *
 * @return Single character verbosity level indicator string
 */
static const char *
dc_log_level_repr(int level)
{
    const char *tag = "?";
    switch( dc_log_clip_level(level) ) {
    case LOG_EMERG:   tag = "X"; break;
    case LOG_ALERT:   tag = "A"; break;
    case LOG_CRIT:    tag = "C"; break;
    case LOG_ERR:     tag = "E"; break;
    case LOG_WARNING: tag = "W"; break;
    case LOG_NOTICE:  tag = "N"; break;
    case LOG_INFO:    tag = "I"; break;
    case LOG_DEBUG:   tag = "D"; break;
    default: break;
    }
    return tag;
}

/** Get current verbosity level
 *
 * @return verbosity level
 */
static int
dc_log_get_level(void)
{
    return dc_log_level;
}

/** Set current verbosity level
 *
 * @param level syslog() compatible verbosity level
 */
static void
dc_log_set_level(int level)
{
    dc_log_level = dc_log_clip_level(level);
}

/** Message logging predicate
 *
 * @param level syslog() compatible verbosity level
 *
 * @return true if logging at given verbosity is allowed, false otherwise
 */
static bool
dc_log_p(int level)
{
    return level <= dc_log_level;
}

/** Emit diagnostic message
 *
 * @note This function should not be called directly, but via
 *       macros like dc_log_err() etc.
 *
 * @param level syslog() compatible verbosity level
 * @param fmt   printf() compatible format string
 * @param ...   Arguments required by format string
 */
static void __attribute__((format(printf, 2, 3)))
dc_log_emit_real(int level, const char *fmt, ...)
{
    if( dc_log_p(level) ) {
        int saved = errno;
        va_list va;
        va_start(va, fmt);
        char *msg = 0;
        if( vasprintf(&msg, fmt, va) < 0 )
            msg = 0;
        va_end(va);

        if( dc_log_to == DC_LOG_TO_SYSLOG ) {
            syslog(level, "%s", msg ?: fmt);
        }
        else {
            fprintf(stderr, "%s: %s: %s\n",
                    dc_log_get_name(),
                    dc_log_level_repr(level),
                    msg ?: fmt);
            fflush(stderr);
        }
        free(msg);
        errno = saved;
    }
}

/* ========================================================================= *
 * DC_DBUS
 * ========================================================================= */

static bool dc_exit_on_enable = false;
static bool dc_release_name = false;
static bool dc_name_released = false;
static unsigned dc_setup_actions = COMPOSITOR_ACTION_NONE;

/** Cached system bus connection */
static DBusConnection *dc_dbus_con = 0;

/** Signal match rules to add on connect */
static const char * const dc_dbus_matches[] =
{
    "type=signal"
    ",interface='"DBUS_INTERFACE_DBUS"'"
    ",path='"DBUS_PATH_DBUS"'"
    ",member='NameOwnerChanged'"
    ",arg0='"COMPOSITOR_SERVICE"'",
    0
};

/** Helper for parsing dbus message arguments
 *
 * Can be used as replacement for dbus_message_get_args() when
 * caller is not interested in possible error details.
 *
 * @param msg  D-Bus message to parse
 * @param type Type of the 1st argument to parse
 * @para  ...  as with dbus_message_get_args()
 *
 * @returns true if all arguments were succesfully parsed, false otherwise
 */
static bool
dc_dbus_parse_message(DBusMessage *msg, int type, ...)
{
    DBusError err = DBUS_ERROR_INIT;
    va_list va;
    va_start(va, type);
    bool ack = dbus_message_get_args_valist(msg, &err, type, va);
    va_end(va);
    if( !ack )
        dc_log_err("parse error: %s: %s", err.name, err.message);
    dbus_error_free(&err);
    return ack;
}

/** Handle incoming org.freedesktop.DBus.NameLost signals
 *
 * @param sig D-Bus signal message
 */
static void
dc_dbus_handle_name_lost_signal(DBusMessage *sig)
{
    const char *name = 0;

    if( !dc_dbus_parse_message(sig,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_INVALID) )
        goto EXIT;

    dc_log_info("name lost: %s", dc_name_repr(name));

    if( dc_equal_p(name, COMPOSITOR_SERVICE) ) {
        /* Something took name ownership from us
         * -> expected when delayed exit is disabled
         * -> assume success
         * -> exit immediately
         */
        if( !dc_release_name )
            dc_mainloop_exit(EXIT_SUCCESS);
    }

EXIT:
    return;
}

/** Handle incoming org.freedesktop.DBus.NameAcquired signals
 *
 * @param sig D-Bus signal message
 */
static void
dc_dbus_handle_name_acquired_signal(DBusMessage *sig)
{
    const char *name = 0;

    if( !dc_dbus_parse_message(sig,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_INVALID) )
        goto EXIT;

    dc_log_info("name acquired: %s", dc_name_repr(name));

    if( dc_equal_p(name, COMPOSITOR_SERVICE) ) {
        /* We gained name ownership
         * -> success
         * -> exit (after brief delay)
         */
        if( !dc_exit_on_enable )
            dc_mainloop_schedule_delayed_exit();
    }

EXIT:
    return;
}

/** Handle incoming org.freedesktop.DBus.NameOwnerChanged signals
 *
 * @param sig D-Bus signal message
 */
static void
dc_dbus_handle_name_owner_changed_signal(DBusMessage *sig)
{
    const char *name = 0;
    const char *prev = 0;
    const char *curr = 0;

    if( !dc_dbus_parse_message(sig,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_STRING, &prev,
                               DBUS_TYPE_STRING, &curr,
                               DBUS_TYPE_INVALID) )
        goto EXIT;

    dc_log_info("name owner changed: %s: %s -> %s",
                dc_name_repr(name),
                dc_name_repr(prev),
                dc_name_repr(curr));

    if( dc_equal_p(name, COMPOSITOR_SERVICE) ) {
        if( dc_equal_p(curr, "") ) {
            /* Compositor has no name owner
             * -> unexpected, but assume success
             * -> exit immediately
             */
            if( !dc_release_name )
                dc_mainloop_exit(EXIT_SUCCESS);
        }
    }

EXIT:
    return;
}

/** Handle dispatched org.freedesktop.DBus.Local.Disconnected signals
 *
 * @param sig D-Bus signal message
 */
static void
dc_dbus_handle_disconnected_signal(DBusMessage *sig)
{
    (void)sig;

    /* While we expect to get terminated/killed, make
     * orderly exit also when/if systembus dies.
     */
    dc_mainloop_exit(EXIT_FAILURE);
}

/** Handle incoming compositor setUpdatesEnabled method calls
 *
 * @param req D-Bus method call message
 *
 * @return D-Bus method call reply message
 */
static DBusMessage *
dc_dbus_handle_set_updates_enabled_method_call(DBusMessage *req)
{
    DBusMessage *rsp = 0;

    dbus_bool_t enabled = false;

    if( !dc_dbus_parse_message(req,
                               DBUS_TYPE_BOOLEAN, &enabled,
                               DBUS_TYPE_INVALID) )
        goto EXIT;

    dc_log_debug("set_updates_enabled(%s)", dc_bool_repr(enabled));

    if( enabled && dc_exit_on_enable ) {
        /* We have gained name ownership and
         * mce gave us permission to draw
         * -> exit
         */
        dc_mainloop_schedule_delayed_exit();
    }

EXIT:
    rsp = dbus_message_new_method_return(req);
    return rsp;
}

/** Handle incoming privateGetSetupActions method calls
 *
 * @param req D-Bus method call message
 *
 * @return D-Bus method call reply message
 */
static DBusMessage *
dc_dbus_handle_get_setup_actions_method_call(DBusMessage *req)
{
    DBusMessage   *rsp   = NULL;
    dbus_uint32_t  flags = dc_setup_actions;

    if( !(rsp = dbus_message_new_method_return(req)) )
        goto EXIT;

    dbus_message_append_args(rsp,
                             DBUS_TYPE_UINT32, &flags,
                             DBUS_TYPE_INVALID);
    dc_log_debug("get_setup_actions() -> 0x%x", (unsigned)flags);

EXIT:
    return rsp;
}

/** Handle incoming compositor privateTopmostWindowProcessId method calls
 *
 * @param req D-Bus method call message
 *
 * @return D-Bus method call reply message
 */
static DBusMessage *
dc_dbus_handle_get_topmost_window_pid_method_call(DBusMessage *req)
{
    DBusMessage *rsp = 0;
    rsp = dbus_message_new_method_return(req);
    dbus_int32_t pid = getpid();
    dbus_message_append_args(rsp,
                             DBUS_TYPE_INT32, &pid,
                             DBUS_TYPE_INVALID);
    dc_log_debug("get_topmost_window_pid() -> %d", (int)pid);
    return rsp;
}

/** System bus message filter callback
 *
 * @param con  D-Bus connection
 * @param msg  Incoming D-Bus message
 * @param aptr (unused) User data pointer
 *
 * @return DBUS_HANDLER_RESULT_HANDLED if msg was method call message
 *         handled by the filter, DBUS_HANDLER_RESULT_NOT_YET_HANDLED
 *         otherwise
 */
static DBusHandlerResult
dc_dbus_message_filter_cb(DBusConnection *con, DBusMessage *msg, void *aptr)
{
    (void)aptr;

    DBusHandlerResult res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    DBusMessage      *rsp = 0;

    const char *path = dbus_message_get_path(msg);
    const char *ifce = dbus_message_get_interface(msg);
    const char *memb = dbus_message_get_member(msg);
    int         type = dbus_message_get_type(msg);

    if( type == DBUS_MESSAGE_TYPE_SIGNAL ) {
        if( dc_equal_p(path, DBUS_PATH_DBUS) &&
            dc_equal_p(ifce, DBUS_INTERFACE_DBUS) ) {
            if( dc_equal_p(memb, "NameLost") )
                dc_dbus_handle_name_lost_signal(msg);
            else if( dc_equal_p(memb, "NameAcquired") )
                dc_dbus_handle_name_acquired_signal(msg);
            else if( dc_equal_p(memb, "NameOwnerChanged") )
                dc_dbus_handle_name_owner_changed_signal(msg);
        }
        else if( dc_equal_p(path, DBUS_PATH_LOCAL) &&
                 dc_equal_p(ifce, DBUS_INTERFACE_LOCAL) ) {
            if( dc_equal_p(memb, "Disconnected") )
                dc_dbus_handle_disconnected_signal(msg);
        }
    }
    else if( type == DBUS_MESSAGE_TYPE_METHOD_CALL ) {
        const char *dest = dbus_message_get_destination(msg);
        if( dc_equal_p(dest, COMPOSITOR_SERVICE) &&
            dc_equal_p(path, COMPOSITOR_PATH) &&
            dc_equal_p(ifce, COMPOSITOR_IFACE) ) {
            if( dc_equal_p(memb, COMPOSITOR_SET_UPDATES_ENABLED) )
                rsp = dc_dbus_handle_set_updates_enabled_method_call(msg);
            else if( dc_equal_p(memb, COMPOSITOR_GET_TOPMOST_WINDOW_PID) )
                rsp = dc_dbus_handle_get_topmost_window_pid_method_call(msg);
            else if( dc_equal_p(memb, COMPOSITOR_GET_SETUP_ACTIONS) )
                rsp = dc_dbus_handle_get_setup_actions_method_call(msg);
        }
    }

    if( rsp ) {
        res = DBUS_HANDLER_RESULT_HANDLED;
        if( !dbus_message_get_no_reply(msg) )
            dbus_connection_send(con, rsp, 0);
        dbus_message_unref(rsp);
    }

    return res;
}

/** Connect to D-Bus SystemBus
 *
 * @return true on success, false otherwise
 */
static bool
dc_dbus_connect(void)
{
    DBusError err = DBUS_ERROR_INIT;
    int       bus = DBUS_BUS_SYSTEM;

    /* Already done? */
    if( dc_dbus_con )
        goto EXIT;

    dc_log_debug("dbus connect");

    if( !(dc_dbus_con = dbus_bus_get(bus, &err)) ) {
        dc_log_err("dbus connect failed: %s: %s",
                err.name, err.message);
        goto EXIT;
    }

    if( !dbus_connection_add_filter(dc_dbus_con,
                                    dc_dbus_message_filter_cb, 0, 0) )
        goto EXIT;

    for( size_t i = 0; dc_dbus_matches[i]; ++i )
        dbus_bus_add_match(dc_dbus_con, dc_dbus_matches[i], 0);

    dbus_connection_set_exit_on_disconnect(dc_dbus_con, false);

    dbus_gmain_set_up_connection(dc_dbus_con, 0);

EXIT:
    dbus_error_free(&err);

    return dc_dbus_con != 0;
}

/** Connected to SystemBus predicate
 *
 * If dbus daemon should get killed / something similar, we are left with
 * valid dbus connection object that is in disconnected state - any attempt
 * to do IPC over such connection generates a lot of debugging noise and/or
 * libdbus calls exit.
 *
 * @return true when a connection exists and is alive, false otherwise
 */
static bool
dc_dbus_connected(void)
{
    return (dc_dbus_con &&
            dbus_connection_get_is_connected(dc_dbus_con));
}

/** Disconnect from D-Bus SystemBus
 */
static void
dc_dbus_disconnect(void)
{
    if( dc_dbus_con ) {
        dc_log_debug("dbus disconnect");

        /* Detach filter callback */
        dbus_connection_remove_filter(dc_dbus_con,
                                      dc_dbus_message_filter_cb, 0);

        if( dbus_connection_get_is_connected(dc_dbus_con) ) {
            for( size_t i = 0; dc_dbus_matches[i]; ++i )
                dbus_bus_remove_match(dc_dbus_con, dc_dbus_matches[i], 0);

            /* Note: Releasing D-Bus name is intentionally
             *       handled implicitly i.e. when process
             *       exits and drops out from system bus.
             */
        }

        dbus_connection_unref(dc_dbus_con),
            dc_dbus_con = 0;
    }
}

/** Request ownership of COMPOSITOR_SERVICE D-Bus name
 *
 * @return false if request was outright denied, true otherwise
 */
static bool
dc_dbus_reserve_name(void)
{
    DBusError err = DBUS_ERROR_INIT;
    bool ack = false;

    if( !dc_dbus_connected() )
        goto EXIT;

    int flags = 0;
    flags |= DBUS_NAME_FLAG_ALLOW_REPLACEMENT;
    flags |= DBUS_NAME_FLAG_REPLACE_EXISTING;

    int rc = dbus_bus_request_name(dc_dbus_con,
                                   COMPOSITOR_SERVICE,
                                   flags, &err);

    switch( rc ) {
    case DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER:
        /* A NameAcquired signal should arrive shortly */
        dc_log_debug("primary name owner");
        break;
    case DBUS_REQUEST_NAME_REPLY_IN_QUEUE:
        /* A NameAcquired signal should eventually arrive */
        dc_log_debug("queued for name ownership");
        break;
    default:
        /* We are not going to get name ownerhip */
        dc_log_err("reserving dbus name failed");
        goto EXIT;
    }

    ack = true;

EXIT:
    dbus_error_free(&err);
    return ack;
}

/** Release ownership of COMPOSITOR_SERVICE D-Bus name
 *
 * @return true on success, false otherwise
 */
static bool
dc_dbus_release_name(void)
{
    DBusError err = DBUS_ERROR_INIT;
    bool ack = false;

    if( !dc_dbus_connected() )
        goto EXIT;

    int rc = dbus_bus_release_name(dc_dbus_con,
                                   COMPOSITOR_SERVICE,
                                   &err);

    switch( rc ) {
    case DBUS_RELEASE_NAME_REPLY_RELEASED:
        /* A NameLost signal should arrive shortly */
        dc_log_debug("name released");
        break;
    case DBUS_RELEASE_NAME_REPLY_NOT_OWNER:
        dc_log_err("can't release name: not owner");
        goto EXIT;
    case DBUS_RELEASE_NAME_REPLY_NON_EXISTENT:
        dc_log_err("can't release name: does not exist");
        goto EXIT;
    default:
        dc_log_err("releasing dbus name failed");
        goto EXIT;
    }

    ack = true;

EXIT:
    dbus_error_free(&err);
    return ack;
}

/* ========================================================================= *
 * DC_MAINLOOP
 * ========================================================================= */

/** Exit code applicable after leaving mainloop */
static int        dc_mainloop_exit_code       = EXIT_SUCCESS;

/** Delayed exit timer id */
static guint      dc_mainloop_delayed_exit_id = 0;

/** Delayed exit timer duration */
static int        dc_mainloop_exit_delay      = DC_EXIT_DELAY_MS;

/** Currently active mainloop */
static GMainLoop *dc_mainloop_handle          = 0;

/** Run application mainloop
 *
 * @return exit code to use on process exit
 */
int
dc_mainloop_run(void)
{
    dc_mainloop_handle = g_main_loop_new(0, 0);

    g_main_loop_run(dc_mainloop_handle);

    g_main_loop_unref(dc_mainloop_handle), dc_mainloop_handle = 0;

    return dc_mainloop_exit_code;
}

/** Stop application mainloop
 *
 * @param exit_code exit code to use on process exit
 */
void
dc_mainloop_exit(int exit_code)
{
    if( dc_mainloop_exit_code < exit_code )
        dc_mainloop_exit_code = exit_code;

    if( !dc_mainloop_handle ) {
        dc_log_crit("exit from mainloop without mainloop; exit immediately");
        _exit(dc_mainloop_exit_code);
    }

    g_main_loop_quit(dc_mainloop_handle);
}

/** Delayed exit timer callback
 *
 * @param aptr (unused) User data pointer
 *
 * @return G_SOURCE_REMOVE (to stop timer from repeating)
 */
static gboolean
dc_mainloop_delayed_exit_cb(gpointer aptr)
{
    (void)aptr;

    if( dc_release_name && !dc_name_released ) {
        dc_name_released = true;
        dc_dbus_release_name();
        return G_SOURCE_CONTINUE;
    }

    dc_log_debug("delayed exit: triggered");
    dc_mainloop_delayed_exit_id = 0;
    dc_mainloop_exit(EXIT_SUCCESS);
    return G_SOURCE_REMOVE;
}

/** Schedule delayed exit
 */
static void
dc_mainloop_schedule_delayed_exit(void)
{
    if( !dc_mainloop_delayed_exit_id && dc_mainloop_exit_delay >= 0 ) {
        dc_log_debug("delayed exit: scheduled");
        dc_mainloop_delayed_exit_id
            = g_timeout_add(dc_mainloop_exit_delay,
                            dc_mainloop_delayed_exit_cb,
                            0);
    }
}

/** Cancel scheduled delayed exit
 */
static void
dc_mainloop_cancel_delayed_exit(void)
{
    if( dc_mainloop_delayed_exit_id ) {
        dc_log_debug("delayed exit: canceled");
        g_source_remove(dc_mainloop_delayed_exit_id),
            dc_mainloop_delayed_exit_id = 0;
    }
}

/* ========================================================================= *
 * MAIN
 * ========================================================================= */

/** Output application usage information
 */
static void
dc_print_usage(void)
{
    static const char format[] =
        "NAME\n"
        "    %s - dummy Sailfish OS Compositor D-Bus service\n"
        "\n"
        "SYNOPSIS\n"
        "    %s [options]\n"
        "\n"
        "DESCRIPTION\n"
        "    Attempts to acquire compositor service D-Bus name and then\n"
        "    exits either immediately or after a brief delay.\n"
        "\n"
        "    Normally switching from one compositor to another happens\n"
        "    so that compositor A (such as unlock ui) allows replacing\n"
        "    dbus service name owner and compositor B (e.g. lipstick)\n"
        "    takes over display management by acquiring the D-Bus name.\n"
        "\n"
        "    In situations where compositor A does not work / interferes\n"
        "    with android services while compositor B requires the android\n"
        "    services to function properly, dummy compositor service can\n"
        "    be used as a stop gap where relevant android services are\n"
        "    started and/or stopped as required.\n"
        "\n"
        "OPTIONS\n"
        "    -h --help             Print usage information.\n"
        "    -V --version          Print version information.\n"
        "    -v --verbose          Increase program verbosity.\n"
        "    -q --quiet            Decrease program verbosity.\n"
        "    -s --force-syslog     Use syslog for logging.\n"
        "    -T --force-stderr     Use stderr for logging.\n"
        "    -d --exit-delay=<ms>  Set successful exit delay [ms].\n"
        "    -e --exit-on-enable   Exit on setUpdatesEnabled(true).\n"
        "    -r --release-name     Release name before exiting.\n"
        "    --hwc-stop            Stop hwc service before enabling updates.\n"
        "    --hwc-start           Start hwc service before enabling updates.\n"
        "    --hwc-restart         Re-start hwc service before enabling updates.\n"
        "\n";
    const char *name = dc_log_get_name();
    fprintf(stderr, format, name, name);
}

/** Output application version information
 */
static void
dc_print_version(void)
{
    const char *name = dc_log_get_name();
    const char *vers = G_STRINGIFY(PRG_VERSION);
    fprintf(stdout, "%s %s\n", name, vers);
}

/** Application main entry point
 */
int
main(int ac, char **av)
{
    static struct option opt_long[] = {
        { "help",            no_argument,       0, 'h' },
        { "version",         no_argument,       0, 'V' },
        { "verbose",         no_argument,       0, 'v' },
        { "quiet",           no_argument,       0, 'q' },
        { "exit-delay",      required_argument, 0, 'd' },
        { "exit-on-enable",  no_argument,       0, 'e' },
        { "release-name",    no_argument,       0, 'r' },
        { "force-syslog",    no_argument,       0, 's' },
        { "force-stderr",    no_argument,       0, 'T' },
        { "hwc-stop",        no_argument,       0, 901 },
        { "hwc-start",       no_argument,       0, 902 },
        { "hwc-restart",     no_argument,       0, 903 },
        { 0,                 0,                 0,  0  }
    };

    static const char opt_short[] = "hVvqd:ersT";

    int xc = EXIT_FAILURE;

    dc_log_debug("parse arguments");

    dc_log_set_name(basename(*av));

    int level = dc_log_get_level();
    for( ;; ) {
        int opt = getopt_long(ac, av, opt_short, opt_long, 0);
        if( opt == -1 )
            break;
        switch( opt ) {
        case 'h':
            dc_print_usage();
            exit(EXIT_SUCCESS);
        case 'V':
            dc_print_version();
            exit(EXIT_SUCCESS);
        case 'v':
            ++level;
            break;
        case 'q':
            --level;
            break;
        case 'd':
            dc_mainloop_exit_delay = strtol(optarg, 0, 0);
            break;
        case 'e':
            dc_exit_on_enable = true;
            break;
        case 'r':
            dc_release_name = true;
            break;
        case 's':
            dc_log_to = DC_LOG_TO_SYSLOG;
            break;
        case 'T':
            dc_log_to = DC_LOG_TO_STDERR;
            break;
        case 901:
            dc_setup_actions |= COMPOSITOR_ACTION_STOP_HWC;
            break;
        case 902:
            dc_setup_actions |= COMPOSITOR_ACTION_START_HWC;
            break;
        case 903:
            dc_setup_actions |= COMPOSITOR_ACTION_RESTART_HWC;
            break;
        case '?':
            exit(EXIT_FAILURE);
        default:
            fprintf(stderr, "getopt() returned code %d\n", opt);
            exit(EXIT_FAILURE);
        }
    }
    dc_log_set_level(level);

    dc_log_debug("initalize");

    if( !dc_dbus_connect() )
        goto EXIT;

    if( !dc_dbus_reserve_name() )
        goto EXIT;

    dc_log_debug("enter mainloop");

    xc = dc_mainloop_run();

    dc_log_debug("leave mainloop");

EXIT:
    dc_log_debug("cleanup");

    dc_mainloop_cancel_delayed_exit();

    dc_dbus_disconnect();

    dc_log_info("exit %d", xc);

    return xc;
}
