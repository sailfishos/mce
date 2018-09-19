/* ------------------------------------------------------------------------- *
 * Copyright (C) 2014 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: LGPLv2.1
 * ------------------------------------------------------------------------- */

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-dbus.h"

#include <stdlib.h>
#include <string.h>

#include <gmodule.h>

/* ========================================================================= *
 * D-BUS CONSTANTS
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * DBUS DAEMON ITSELF
 * ------------------------------------------------------------------------- */

//      DBUS_INTERFACE_DBUS                "org.freedesktop.DBus"
#define DBUS_DAEMON_REQ_GET_NAME_OWNER     "GetNameOwner"
#define DBUS_DAEMON_SIG_NAME_OWNER_CHANGED "NameOwnerChanged"

/* ------------------------------------------------------------------------- *
 * DBUS PROPERTIES INTERFACE
 * ------------------------------------------------------------------------- */

//      DBUS_INTERFACE_PROPERTIES "org.freedesktop.DBus.Properties"
#define PROPERTIES_REQ_GET        "Get"
#define PROPERTIES_REQ_GET_ALL    "GetAll"
#define PROPERTIES_REQ_SET        "Set"
#define PROPERTIES_SIG_CHANGED    "PropertiesChanged"

/* ------------------------------------------------------------------------- *
 * PACKAGEKIT
 * ------------------------------------------------------------------------- */

#define PKGKIT_SERVICE               "org.freedesktop.PackageKit"
#define PKGKIT_INTERFACE             "org.freedesktop.PackageKit"
#define PKGKIT_OBJECT                "/org/freedesktop/PackageKit"

/* ------------------------------------------------------------------------- *
 * SYSTEMD
 * ------------------------------------------------------------------------- */

#define SYSTEMD_SERVICE               "org.freedesktop.systemd1"
#define SYSTEMD_OBJECT                "/org/freedesktop/systemd1"
#define SYSTEMD_MANAGER_INTERFACE     "org.freedesktop.systemd1.Manager"
#define SYSTEMD_MANAGER_START_UNIT    "StartUnit"

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

// STATE_MANAGEMENT

static void     xpkgkit_set_locked_state        (bool locked);
static void     xpkgkit_set_available_state     (bool available);

// DBUS_HELPERS

static void     xpkgkit_parse_changed_properties(DBusMessageIter *body);
static void     xpkgkit_parse_dropped_properties(DBusMessageIter *body);

// DBUS_IPC

static void     xpkgkit_get_properties_cb       (DBusPendingCall *pc, void*aptr);
static void     xpkgkit_get_properties          (void);

static void     xpkgkit_check_name_owner_cb     (DBusPendingCall *pc, void*aptr);
static void     xpkgkit_check_name_owner        (void);

// UPDATE_LOGGING

static void     xpkgkit_logging_request_start_cb(DBusPendingCall *pc, void *aptr);
static void     xpkgkit_logging_request_start   (void);
static void     xpkgkit_logging_cancel_start    (void);

// DATAPIPE_HANDLERS

static void     xpkgkit_datapipe_osupdate_running_cb(gconstpointer data);
static void     xpkgkit_datapipe_init          (void);
static void     xpkgkit_datapipe_quit          (void);

// DBUS_HANDLERS

static gboolean xpkgkit_name_owner_changed_cb   (DBusMessage *rsp);
static gboolean xpkgkit_property_changed_cb     (DBusMessage *sig);

// MODULE_LOAD_UNLOAD

G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
G_MODULE_EXPORT void         g_module_unload    (GModule *module);

/* ========================================================================= *
 * STATE_MANAGEMENT
 * ========================================================================= */

/* PackageKit is on D-Bus and Locked property is set */
static bool xpkgkit_is_locked = false;

static void
xpkgkit_set_locked_state(bool locked)
{
    if( xpkgkit_is_locked == locked )
        goto EXIT;

    xpkgkit_is_locked = locked;
    mce_log(LL_DEBUG, "packagekit is %slocked", locked ? "" : "not ");

    datapipe_exec_full(&packagekit_locked_pipe,
                       GINT_TO_POINTER(xpkgkit_is_locked),
                       DATAPIPE_USE_INDATA, DATAPIPE_CACHE_INDATA);

EXIT:
    return;
}

/* PackageKit is on D-Bus */
static bool xpkgkit_is_available = false;

/** Handle "org.freedesktop.PackageKit" D-Bus name owner changes
 *
 * Clear xpkgkit_is_locked on all changes.
 *
 * Query properties if there is a new owner.
 *
 * @param available true if PackageKit name has an owner, false if not
 */
static void
xpkgkit_set_available_state(bool available)
{
    if( xpkgkit_is_available == available )
        goto EXIT;

    mce_log(LL_DEBUG, "%s is %savailable", PKGKIT_SERVICE,
            available ? "" : "not ");

    /* unlocked until proven otherwise */
    xpkgkit_set_locked_state(false);

    if( (xpkgkit_is_available = available) ) {
        /* start (async) property query */
        xpkgkit_get_properties();
    }

EXIT:
  return;
}

/* ========================================================================= *
 * DBUS_HELPERS
 * ========================================================================= */

/** Parse array of (string key, variant value) entries
 *
 * Update xpkgkit_is_locked as needed.
 *
 * @param body D-Bus message iterator
 */
static void
xpkgkit_parse_changed_properties(DBusMessageIter *body)
{
    // initialize to current value
    bool locked = xpkgkit_is_locked;

    if( !body )
        goto EXIT;

    DBusMessageIter arr, ent, var;
    // <arg type="a{sv}" name="changed_properties"/>

    if( !mce_dbus_iter_get_array(body, &arr) )
        goto EXIT;

    while( !mce_dbus_iter_at_end(&arr) ) {
        const char *key = 0;

        if( !mce_dbus_iter_get_entry(&arr, &ent) )
            goto EXIT;

        if( !mce_dbus_iter_get_string(&ent, &key) )
            goto EXIT;

        if( !mce_dbus_iter_get_variant(&ent, &var) )
            goto EXIT;

        if( !strcmp(key, "Locked") ) {
            bool val = false;
            if( !mce_dbus_iter_get_bool(&var, &val) )
                goto EXIT;
            mce_log(LL_DEBUG, "%s = bool %d", key, val);
            locked = val;
        }
        else {
            char *val = mce_dbus_message_iter_repr(&var);
            mce_log(LL_DEBUG, "%s = %s", key, val);
            free(val);
        }
    }

EXIT:
    xpkgkit_set_locked_state(locked);
    return;
}

/** Parse array of dropped property keys
 *
 * Update xpkgkit_is_locked as needed.
 *
 * @param body D-Bus message iterator
 */
static void
xpkgkit_parse_dropped_properties(DBusMessageIter *body)
{
    // initialize to current value
    bool locked = xpkgkit_is_locked;

    if( !body )
        goto EXIT;

    DBusMessageIter arr;

    // <arg type="as" name="invalidated_properties"/>

    if( !mce_dbus_iter_get_array(body, &arr) )
        goto EXIT;

    while( !mce_dbus_iter_at_end(&arr) ) {
        const char *key = 0;

        if( !mce_dbus_iter_get_string(&arr, &key) )
            goto EXIT;

        mce_log(LL_DEBUG, "%s = <dropped>", key);

        if( !strcmp(key, "Locked") ) {
            locked = false;
        }
    }

EXIT:
    xpkgkit_set_locked_state(locked);
    return;
}

/* ========================================================================= *
 * DBUS_IPC
 * ========================================================================= */

/** Handle reply to xpkgkit_get_properties()
 *
 * @param pc   pending call object
 * @param aptr (not used)
 */
static void
xpkgkit_get_properties_cb(DBusPendingCall *pc, void *aptr)
{
    (void)aptr;

    mce_log(LL_DEBUG, "%s.%s %s",
            DBUS_INTERFACE_PROPERTIES,
            PROPERTIES_REQ_GET_ALL,
            "reply");

    DBusMessage *rsp = 0;
    DBusError    err = DBUS_ERROR_INIT;

    if( !pc )
        goto EXIT;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) )
        goto EXIT;

    if( dbus_set_error_from_message(&err, rsp) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    DBusMessageIter body;

    dbus_message_iter_init(rsp, &body);
    xpkgkit_parse_changed_properties(&body);

EXIT:
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

/** Get list of PackageKit properties [async]
 *
 * Used for probing the initial state after PackageKit shows up
 * on the SystemBus.
 */
static void
xpkgkit_get_properties(void)
{
    const char *interface = PKGKIT_INTERFACE;

    bool res = dbus_send(PKGKIT_SERVICE,
                         PKGKIT_OBJECT,
                         DBUS_INTERFACE_PROPERTIES,
                         PROPERTIES_REQ_GET_ALL,
                         xpkgkit_get_properties_cb,
                         DBUS_TYPE_STRING, &interface,
                         DBUS_TYPE_INVALID);

    mce_log(LL_DEBUG, "%s.%s %s",
            DBUS_INTERFACE_PROPERTIES,
            PROPERTIES_REQ_GET_ALL,
            res ? "sent ..." : "failed");
}

/** Handle reply to asynchronous PackageKit service name ownership query
 *
 * @param pc   State data for asynchronous D-Bus method call
 * @param aptr (not used)
 */
static void
xpkgkit_check_name_owner_cb(DBusPendingCall *pc, void *aptr)
{
    (void)aptr;

    DBusMessage *rsp   = 0;
    const char  *owner = 0;
    DBusError    err   = DBUS_ERROR_INIT;

    if( !pc )
        goto EXIT;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) )
        goto EXIT;

    if( dbus_set_error_from_message(&err, rsp) ||
        !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_STRING, &owner,
                               DBUS_TYPE_INVALID) )
    {
        if( strcmp(err.name, DBUS_ERROR_NAME_HAS_NO_OWNER) ) {
            mce_log(LL_WARN, "%s: %s", err.name, err.message);
            goto EXIT;
        }
    }

    xpkgkit_set_available_state(owner && *owner);

EXIT:
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

/** Initiate asynchronous PackageKit service name ownership query
 *
 * Updates xpkgkit_is_available flag when reply message is received.
 */
static void
xpkgkit_check_name_owner(void)
{
    const char *name = PKGKIT_SERVICE;

    dbus_send(DBUS_SERVICE_DBUS,
              DBUS_PATH_DBUS,
              DBUS_INTERFACE_DBUS,
              DBUS_DAEMON_REQ_GET_NAME_OWNER,
              xpkgkit_check_name_owner_cb,
              DBUS_TYPE_STRING, &name,
              DBUS_TYPE_INVALID);
}

/* ========================================================================= *
 * UPDATE_LOGGING
 * ========================================================================= */

/** Name of the logging unit to start */
static const char xpkgkit_logging_unit_name[] = "osupdate-logging.service";

/** Stopping other units to fulfill dependencies is not ok */
static const char xpkgkit_logging_unit_start_mode[] = "fail";

/** Pending unit start request */
static DBusPendingCall *xpkgkit_logging_request_start_pc = 0;

/** Handle reply to logging unit start request from systemd
 */
static void
xpkgkit_logging_request_start_cb(DBusPendingCall *pc, void *aptr)
{
    (void)aptr;

    DBusMessage *rsp = 0;
    DBusError    err = DBUS_ERROR_INIT;
    const char  *job = 0;

    if( !pc || pc != xpkgkit_logging_request_start_pc )
        goto EXIT;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
        mce_log(LL_ERR, "%s(%s): no reply",
                SYSTEMD_MANAGER_START_UNIT,
                xpkgkit_logging_unit_name);
        goto EXIT;
    }

    if( dbus_set_error_from_message(&err, rsp) ) {
        mce_log(LL_ERR, "%s(%s): %s: %s",
                SYSTEMD_MANAGER_START_UNIT,
                xpkgkit_logging_unit_name,
                err.name, err.message);
        goto EXIT;
    }

    if( !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_OBJECT_PATH, &job,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "%s(%s): %s: %s",
                SYSTEMD_MANAGER_START_UNIT,
                xpkgkit_logging_unit_name,
                err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEVEL, "%s(%s): job %s",
            SYSTEMD_MANAGER_START_UNIT,
            xpkgkit_logging_unit_name,
            job ?: "n/a");

EXIT:
    if( pc && pc == xpkgkit_logging_request_start_pc ) {
        dbus_pending_call_unref(xpkgkit_logging_request_start_pc),
            xpkgkit_logging_request_start_pc = 0;
    }

    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);

    return;
}

/** Send logging unit start request to systemd
 */
static void
xpkgkit_logging_request_start(void)
{
    if( xpkgkit_logging_request_start_pc )
        goto EXIT;

    const char *unit = xpkgkit_logging_unit_name;
    const char *mode = xpkgkit_logging_unit_start_mode;

    dbus_send_ex(SYSTEMD_SERVICE,
                 SYSTEMD_OBJECT,
                 SYSTEMD_MANAGER_INTERFACE,
                 SYSTEMD_MANAGER_START_UNIT,
                 xpkgkit_logging_request_start_cb,
                 0, 0,
                 &xpkgkit_logging_request_start_pc,
                 DBUS_TYPE_STRING, &unit,
                 DBUS_TYPE_STRING, &mode,
                 DBUS_TYPE_INVALID);
EXIT:
    return;
}

/** Cancel pending logging unit start request
 */
static void xpkgkit_logging_cancel_start(void)
{
    if( !xpkgkit_logging_request_start_pc )
        goto EXIT;

    dbus_pending_call_cancel(xpkgkit_logging_request_start_pc);
    dbus_pending_call_unref(xpkgkit_logging_request_start_pc),
        xpkgkit_logging_request_start_pc = 0;

EXIT:
  return;
}

/* ========================================================================= *
 * DATAPIPE_HANDLERS
 * ========================================================================= */

/** Update mode is active; assume false */
static bool osupdate_running = false;

/** Change notifications for osupdate_running
 */
static void xpkgkit_datapipe_osupdate_running_cb(gconstpointer data)
{
    bool prev = osupdate_running;
    osupdate_running = GPOINTER_TO_INT(data);

    if( osupdate_running == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "osupdate_running = %d -> %d", prev, osupdate_running);

    if( osupdate_running ) {
        /* When update mode gets activated, we start a systemd
         * service that will store journal to a persistent file
         * until the next reboot. */
        xpkgkit_logging_request_start();
    }

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t xpkgkit_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &osupdate_running_pipe,
        .output_cb = xpkgkit_datapipe_osupdate_running_cb,
    },

    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t xpkgkit_datapipe_bindings =
{
    .module   = "xpkgkit",
    .handlers = xpkgkit_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void xpkgkit_datapipe_init(void)
{
    mce_datapipe_init_bindings(&xpkgkit_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void xpkgkit_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&xpkgkit_datapipe_bindings);
}

/* ========================================================================= *
 * DBUS_HANDLERS
 * ========================================================================= */

/** Handle D-Bus name owner changed signals for "org.freedesktop.PackageKit"
 */
static gboolean
xpkgkit_name_owner_changed_cb(DBusMessage *sig)
{
    const gchar *name = 0;
    const gchar *prev = 0;
    const gchar *curr = 0;
    DBusError    err  = DBUS_ERROR_INIT;

    if( dbus_set_error_from_message(&err, sig) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    if( !dbus_message_get_args(sig, &err,
                              DBUS_TYPE_STRING, &name,
                              DBUS_TYPE_STRING, &prev,
                              DBUS_TYPE_STRING, &curr,
                              DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    if( !name || strcmp(name, PKGKIT_SERVICE) )
        goto EXIT;

    xpkgkit_set_available_state(curr && *curr);

EXIT:
    dbus_error_free(&err);
    return TRUE;
}

/** Handle "PropertiesChanged" signals from "org.freedesktop.PackageKit"
 */
static gboolean
xpkgkit_property_changed_cb(DBusMessage *sig)
{
    const char *interface = 0;

    DBusMessageIter body;

    dbus_message_iter_init(sig, &body);

    if( !mce_dbus_iter_get_string(&body, &interface) )
        goto EXIT;

    if( strcmp(interface, PKGKIT_INTERFACE) )
        goto EXIT;

    mce_log(LL_DEBUG, "properties changed");

    xpkgkit_parse_changed_properties(&body);
    xpkgkit_parse_dropped_properties(&body);

EXIT:
    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t handlers[] =
{
    /* signals */
    {
        .interface = DBUS_INTERFACE_DBUS,
        .name      = DBUS_DAEMON_SIG_NAME_OWNER_CHANGED,
        .rules     = "arg0='"PKGKIT_SERVICE"'",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xpkgkit_name_owner_changed_cb,
    },
    {
        .interface = DBUS_INTERFACE_PROPERTIES,
        .name      = PROPERTIES_SIG_CHANGED,
        .rules     = "path='"PKGKIT_OBJECT"'",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xpkgkit_property_changed_cb,
    },

    /* sentinel */
    {
        .interface = 0,
    }
};

/* ========================================================================= *
 * MODULE_LOAD_UNLOAD
 * ========================================================================= */

/** Init function for the PackageKit module
 *
 * @param module (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    /* install datapipe handlers */
    xpkgkit_datapipe_init();

    /* install dbus message handlers */
    mce_dbus_handler_register_array(handlers);

    /* initiate async query to find out initial state of PackageKit */
    xpkgkit_check_name_owner();

    return NULL;
}

/** Exit function for the PackageKit module
 *
 * @param module (not used)
 */
void g_module_unload(GModule *module)
{
    (void)module;

    /* remove dbus message handlers */
    mce_dbus_handler_unregister_array(handlers);

    /* remove datapipe handlers */
    xpkgkit_datapipe_quit();

    /* cancel pending dbus requests */
    xpkgkit_logging_cancel_start();
    return;
}
