/**
 * @file buttonbacklight.c
 *
 * buttonbacklight module -- implements MENU/HOME/BACK button backlight policy
 * <p>
 * Copyright Copyright (C) 2017 Jolla Ltd.
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

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-dbus.h"
#include "../mce-conf.h"
#include "../evdev.h"

#include <linux/input.h>

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include <mce/dbus-names.h>

#include <gmodule.h>

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** Module name */
#define MODULE_NAME               "buttonbacklight"

/** Maximum number of concurrent button backlight enabler clients */
#define BBL_MAX_CLIENTS 15

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * BBL_SYSFS
 * ------------------------------------------------------------------------- */

static void  bbl_sysfs_write(const char *path, const char *data);

/* ------------------------------------------------------------------------- *
 * BBL_INACTIVE
 * ------------------------------------------------------------------------- */

static gboolean  bbl_inactive_cb      (gpointer aptr);
static void      bbl_inactive_cancel  (void);
static void      bbl_inactive_schedule(void);

/* ------------------------------------------------------------------------- *
 * BBL_STATE
 * ------------------------------------------------------------------------- */

static void  bbl_state_set_physical    (tristate_t state);
static void  bbl_state_set_logical     (tristate_t state);
static void  bbl_state_rethink_physical(void);
static void  bbl_state_rethink_logical (void);

/* ------------------------------------------------------------------------- *
 * BBL_DATAPIPE
 * ------------------------------------------------------------------------- */

static void  bbl_datapipe_system_state_cb       (gconstpointer data);
static void  bbl_datapipe_display_state_curr_cb (gconstpointer data);
static void  bbl_datapipe_submode_cb            (gconstpointer data);
static void  bbl_datapipe_user_activity_event_cb(gconstpointer data);
static void  bbl_datapipe_init                  (void);
static void  bbl_datapipe_quit                  (void);

/* ------------------------------------------------------------------------- *
 * BBL_DBUS
 * ------------------------------------------------------------------------- */

static gboolean  bbl_dbus_client_exit_cb         (DBusMessage *const sig);
static void      bbl_dbus_add_client             (const char *dbus_name);
static void      bbl_dbus_remove_client          (const char *dbus_name);
static void      bbl_dbus_remove_all_clients     (void);
static gboolean  bbl_dbus_send_backlight_state   (DBusMessage *const req);
static gboolean  bbl_dbus_set_backlight_state_cb (DBusMessage *const req);
static gboolean  bbl_dbus_get_button_backlight_cb(DBusMessage *const req);
static void      bbl_dbus_init                   (void);
static void      bbl_dbus_quit                   (void);

/* ------------------------------------------------------------------------- *
 * BBL_CONFIG
 * ------------------------------------------------------------------------- */

static bool  bbl_config_exists(void);
static void  bbl_config_init  (void);
static void  bbl_config_quit  (void);

/* ------------------------------------------------------------------------- *
 * G_MODULE
 * ------------------------------------------------------------------------- */

const gchar  *g_module_check_init(GModule *module);
void          g_module_unload    (GModule *module);

/* ========================================================================= *
 * MODULE_DATA
 * ========================================================================= */

/** Current system state; undefined initially */
static system_state_t system_state = MCE_SYSTEM_STATE_UNDEF;

/** Current display state; undefined initially */
static display_state_t display_state_curr = MCE_DISPLAY_UNDEF;

/** Current submode: Initialized to invalid placeholder value */
static submode_t submode = MCE_SUBMODE_INVALID;

/** Current logical backlight state: unknown initially */
static tristate_t backlight_state_logical = TRISTATE_UNKNOWN;

/** Current physical backlight state: unknown initially */
static tristate_t backlight_state_physical = TRISTATE_UNKNOWN;

/** List of monitored bus clients */
static GSList *bbl_dbus_monitored_clients = NULL;

/** Sysfs control file path for backlight */
static gchar *bbl_control_path = 0;

/** Value to write when enabling backlight */
static gchar *bbl_control_value_enable = 0;

/** Value to write when disabling backlight */
static gchar *bbl_control_value_disable = 0;

/** Timer for: Turn off backlight after user inactivity */
static guint bbl_inactive_id = 0;

/* ========================================================================= *
 * BBL_SYSFS
 * ========================================================================= */

/** Helper for writing to sysfs files
 */
static void
bbl_sysfs_write(const char *path, const char *data)
{
    int fd = -1;

    if( !path || !data )
        goto EXIT;

    if( (fd = TEMP_FAILURE_RETRY(open(path, O_WRONLY))) == -1 ) {
        mce_log(LOG_ERR, "%s: %s: %m", path, "open");
        goto EXIT;
    }

    if( TEMP_FAILURE_RETRY(write(fd, data, strlen(data))) == -1 ) {
        mce_log(LOG_ERR, "%s: %s: %m", path, "write");
        goto EXIT;
    }

    mce_log(LL_DEBUG, "%s << %s", path, data);

EXIT:
    if( fd != -1 )
        TEMP_FAILURE_RETRY(close(fd));
}

/* ========================================================================= *
 * BBL_INACTIVE
 * ========================================================================= */

static gboolean
bbl_inactive_cb(gpointer aptr)
{
    (void)aptr;

    bbl_inactive_id = 0;
    bbl_state_set_physical(TRISTATE_FALSE);
    return G_SOURCE_REMOVE;
}

static void
bbl_inactive_cancel(void)
{
    if( bbl_inactive_id ) {
        g_source_remove(bbl_inactive_id),
            bbl_inactive_id = 0;
    }
}

static void
bbl_inactive_schedule(void)
{
    bbl_inactive_cancel();
    bbl_inactive_id = g_timeout_add(5000, bbl_inactive_cb, 0);
}

/* ========================================================================= *
 * BBL_STATE
 * ========================================================================= */

/** Set current button backlight state
 *
 * @param state one of TRISTATE_TRUE, TRISTATE_FALSE, or TRISTATE_UNKNOWN
 */
static void
bbl_state_set_physical(tristate_t state)
{
    if( backlight_state_physical == state )
        goto EXIT;

    mce_log(LL_DEBUG, "backlight_state_physical: %s -> %s",
            tristate_repr(backlight_state_physical),
            tristate_repr(state));
    backlight_state_physical = state;

    const char *value = 0;

    switch( backlight_state_physical ) {
    case TRISTATE_TRUE:
        bbl_inactive_schedule();
        value = bbl_control_value_enable;
        break;
    case TRISTATE_FALSE:
        bbl_inactive_cancel();
        value = bbl_control_value_disable;
        break;
    default:
        goto EXIT;
    }

    bbl_sysfs_write(bbl_control_path, value);

EXIT:
    return;
}

/** Set current button backlight state
 *
 * @param state one of TRISTATE_TRUE, TRISTATE_FALSE, or TRISTATE_UNKNOWN
 */
static void
bbl_state_set_logical(tristate_t state)
{
    if( backlight_state_logical == state )
        goto EXIT;

    mce_log(LL_DEBUG, "backlight_state_logical: %s -> %s",
            tristate_repr(backlight_state_logical),
            tristate_repr(state));
    backlight_state_logical = state;

    bbl_state_set_physical(backlight_state_logical);

    bbl_dbus_send_backlight_state(0);

EXIT:
    return;
}

/** Evaluate what the current button backlight state should be
 */
static void
bbl_state_rethink_physical(void)
{
    if( backlight_state_logical == TRISTATE_TRUE ) {
        if( backlight_state_physical == TRISTATE_TRUE )
            bbl_inactive_schedule();
        else
            bbl_state_set_physical(TRISTATE_TRUE);
    }
}

/** Evaluate what the current button backlight state should be
 */
static void
bbl_state_rethink_logical(void)
{
    /* Assume button backlight needs to be disabled */
    tristate_t state = TRISTATE_FALSE;

    /* Any clients that have requested enabling ? */
    if( bbl_dbus_monitored_clients == 0 )
        goto EXIT;

    /* Sane sysfs config has been defined ? */
    if( !bbl_config_exists() )
        goto EXIT;

    /* Device running in USER mode ? */
    switch( system_state ) {
    case MCE_SYSTEM_STATE_USER:
        break;
    default:
        goto EXIT;
    }

    /* Display is ON or DIM */
    switch( display_state_curr ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        break;
    default:
        goto EXIT;
    }

    /* Lokcscreen is not active ? */
    if( submode & (MCE_SUBMODE_TKLOCK | MCE_SUBMODE_INVALID) )
        goto EXIT;

    /* Button backlight should be enabled */
    state = TRISTATE_TRUE;

EXIT:
    bbl_state_set_logical(state);
}

/* ========================================================================= *
 * BBL_DATAPIPE
 * ========================================================================= */

/** Handle system state change notifications
 *
 * @param data system state (as void pointer)
 */
static void
bbl_datapipe_system_state_cb(gconstpointer data)
{
    system_state_t prev = system_state;
    system_state = GPOINTER_TO_INT(data);

    if( prev == system_state )
        goto EXIT;

    mce_log(LL_DEBUG, "system_state: %s -> %s",
            system_state_repr(prev),
            system_state_repr(system_state));

    bbl_state_rethink_logical();

    /* Consider turning backlight on again */
    if( system_state == MCE_SYSTEM_STATE_USER )
        bbl_state_rethink_physical();

EXIT:
    return;
}

/** Handle display state change notifications
 *
 * @param data display state (as void pointer)
 */
static void
bbl_datapipe_display_state_curr_cb(gconstpointer data)
{
    display_state_t prev = display_state_curr;
    display_state_curr = GPOINTER_TO_INT(data);

    if( display_state_curr == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_curr = %s -> %s",
            display_state_repr(prev),
            display_state_repr(display_state_curr));

    bbl_state_rethink_logical();

    /* Consider turning backlight on again */
    if( display_state_curr == MCE_DISPLAY_ON ||
        display_state_curr == MCE_DISPLAY_DIM )
        bbl_state_rethink_physical();

EXIT:
    return;
}

/** Handle submode change notifications
 *
 * @param data submode (as void pointer)
 */
static void
bbl_datapipe_submode_cb(gconstpointer data)
{
    submode_t prev = submode;
    submode = GPOINTER_TO_INT(data);

    if( submode == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "submode: %s", submode_change_repr(prev, submode));

    bbl_state_rethink_logical();

    /* Consider turning backlight on again */
    if( (prev & MCE_SUBMODE_TKLOCK) && !(submode & MCE_SUBMODE_TKLOCK) )
        bbl_state_rethink_physical();

EXIT:
    return;
}

/** Handle real user activity
 *
 * @param data Unused
 */
static void bbl_datapipe_user_activity_event_cb(gconstpointer data)
{
    const struct input_event *ev = data;

    if( !ev )
        goto EXIT;

    switch( ev->type ) {
    case EV_KEY:
        /* Consider turning backlight on again */
        mce_log(LL_DEBUG, "%s:%s %d",
                evdev_get_event_type_name(ev->type),
                evdev_get_event_code_name(ev->type, ev->code),
                ev->value);
        bbl_state_rethink_physical();
        break;
    default:
        break;
    }

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t bbl_datapipe_handlers[] =
{
    {
        .datapipe  = &system_state_pipe,
        .output_cb = bbl_datapipe_system_state_cb,
    },
    {
        .datapipe  = &display_state_curr_pipe,
        .output_cb = bbl_datapipe_display_state_curr_cb,
    },
    {
        .datapipe  = &submode_pipe,
        .output_cb = bbl_datapipe_submode_cb,
    },
    {
        .datapipe  = &user_activity_event_pipe,
        .output_cb = bbl_datapipe_user_activity_event_cb,
    },
    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t bbl_datapipe_bindings =
{
    .module   = MODULE_NAME,
    .handlers = bbl_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void
bbl_datapipe_init(void)
{
    mce_datapipe_init_bindings(&bbl_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void
bbl_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&bbl_datapipe_bindings);
}

/* ========================================================================= *
 * BBL_DBUS
 * ========================================================================= */

/** Callback used for monitoring button backlight clients
 *
 * If processes that have enabled button backlight drop out
 * from dbus, treat it as if they had asked for backlight disable.
 *
 * @param sig NameOwnerChanged D-Bus signal
 *
 * @return TRUE
 */
static gboolean
bbl_dbus_client_exit_cb(DBusMessage *const sig)
{
    DBusError   error     = DBUS_ERROR_INIT;
    const char *dbus_name = 0;
    const char *old_owner = 0;
    const char *new_owner = 0;

    if( !dbus_message_get_args(sig, &error,
                               DBUS_TYPE_STRING, &dbus_name,
                               DBUS_TYPE_STRING, &old_owner,
                               DBUS_TYPE_STRING, &new_owner,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to parse NameOwnerChanged: %s: %s",
                error.name, error.message);
        goto EXIT;
    }

    if( !*new_owner )
        bbl_dbus_remove_client(dbus_name);

EXIT:
    dbus_error_free(&error);
    return TRUE;
}

/** Register a client that has enabled button backlight
 */
static void
bbl_dbus_add_client(const char *dbus_name)
{
    gssize rc = mce_dbus_owner_monitor_add(dbus_name,
                                           bbl_dbus_client_exit_cb,
                                           &bbl_dbus_monitored_clients,
                                           BBL_MAX_CLIENTS);
    if( rc < 0 )
        mce_log(LL_WARN, "client %s ignored; BBL_MAX_CLIENTS exceeded",
                dbus_name);
    else if( rc > 0 )
        mce_log(LL_DEBUG, "client %s added for tracking", dbus_name);
    else
        mce_log(LL_DEBUG, "client %s already tracked", dbus_name);

    bbl_state_rethink_logical();
}

/** Unregister a client that has enabled button backlight
 */
static void
bbl_dbus_remove_client(const char *dbus_name)
{
    gssize rc = mce_dbus_owner_monitor_remove(dbus_name,
                                              &bbl_dbus_monitored_clients);

    if( rc < 0 )
        mce_log(LL_WARN, "client %s ignored; is not tracked",dbus_name);
    else
        mce_log(LL_DEBUG, "client %s removed from tracking", dbus_name);

    bbl_state_rethink_logical();
}

/** Unregister all clients that have enabled button backlight
 */
static void
bbl_dbus_remove_all_clients(void)
{
    mce_dbus_owner_monitor_remove_all(&bbl_dbus_monitored_clients);
    bbl_state_rethink_logical();
}

/** Send the button backlight state
 *
 * @param req  Method call to reply, or NULL to broadcast a signal
 *
 * @return TRUE
 */
static gboolean
bbl_dbus_send_backlight_state(DBusMessage *const req)
{
    static tristate_t prev = TRISTATE_UNKNOWN;
    tristate_t        curr = backlight_state_logical;
    dbus_bool_t        arg = FALSE;
    DBusMessage       *msg = 0;

    /* Externally TRISTATE_UNKNOWN is signaled as TRISTATE_FALSE */
    if( curr == TRISTATE_TRUE )
        arg = TRUE;
    else
        curr = TRISTATE_FALSE;

    if( req != 0 ) {
        /* Send reply to explicit query */
        if( dbus_message_get_no_reply(req) )
            goto EXIT;
        msg = dbus_new_method_reply(req);
    }
    else {
        /* Broadcast change signal */
        if( prev == curr )
            goto EXIT;
        prev = curr;
        msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_BUTTON_BACKLIGHT_SIG);
    }

    if( !dbus_message_append_args(msg,
                                  DBUS_TYPE_BOOLEAN, &arg,
                                  DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to append arguments to D-Bus message");
        goto EXIT;
    }

    mce_log(LL_DEVEL, "send button backlight %s: state=%s",
            req ? "reply" : "signal", arg ? "enabled" : "disabled");

    dbus_send_message(msg), msg = 0;

EXIT:
    if( msg )
        dbus_message_unref(msg);

    return TRUE;
}

/** D-Bus callback for the button backlight state change request method call
 *
 * @param req  Method call message to handle
 *
 * @return TRUE
 */
static gboolean
bbl_dbus_set_backlight_state_cb(DBusMessage *const req)
{
    const char    *sender = dbus_message_get_sender(req);
    DBusError      err    = DBUS_ERROR_INIT;
    dbus_bool_t    enable = false;
    DBusMessage   *rsp    = 0;

    mce_log(LL_DEVEL, "button backlight request from %s",
            mce_dbus_get_name_owner_ident(sender));

    if( !dbus_message_get_args(req, &err,
                               DBUS_TYPE_BOOLEAN, &enable,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to get argument from %s.%s: %s: %s",
                MCE_REQUEST_IF, MCE_BUTTON_BACKLIGHT_CHANGE_REQ,
                err.name, err.message);
        enable = false;
        rsp = dbus_message_new_error(req, err.name, err.message);
    }

    if( enable )
        bbl_dbus_add_client(sender);
    else
        bbl_dbus_remove_client(sender);

    if( !dbus_message_get_no_reply(req) ) {
        if( !rsp )
            rsp = dbus_new_method_reply(req);
        dbus_send_message(rsp), rsp = 0;
    }

    if( rsp )
        dbus_message_unref(rsp);

    dbus_error_free(&err);

    return TRUE;
}

/**
 * D-Bus callback for the get button backlight state method call
 *
 * @param req  Method call message to handle
 *
 * @return TRUE
 */
static gboolean
bbl_dbus_get_button_backlight_cb(DBusMessage *const req)
{
    const char    *sender = dbus_message_get_sender(req);

    mce_log(LL_DEVEL, "button backlight query from %s",
            mce_dbus_get_name_owner_ident(sender));

    if( !dbus_message_get_no_reply(req) )
        bbl_dbus_send_backlight_state(req);

    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t bbl_dbus_handlers[] =
{
    /* signals - outbound (for Introspect purposes only) */
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_BUTTON_BACKLIGHT_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"enabled\" type=\"b\"/>\n"
    },
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_BUTTON_BACKLIGHT_CHANGE_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = bbl_dbus_set_backlight_state_cb,
        .args      =
            "    <arg direction=\"in\" name=\"enable\" type=\"b\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_BUTTON_BACKLIGHT_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = bbl_dbus_get_button_backlight_cb,
        .args      =
            "    <arg direction=\"out\" name=\"enabled\" type=\"b\"/>\n"
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Setup dbus handlers
 */
static void
bbl_dbus_init(void)
{
    mce_dbus_handler_register_array(bbl_dbus_handlers);
}

/** Remove dbus handlers
 */
static void
bbl_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(bbl_dbus_handlers);
    bbl_dbus_remove_all_clients();
}

/* ========================================================================= *
 * BBL_CONFIG
 * ========================================================================= */

/** Predicate for: All required configuration items are available
 */
static bool
bbl_config_exists(void)
{
    return (bbl_control_path &&
            bbl_control_value_enable &&
            bbl_control_value_disable);
}

/** Parse button backlight configuration
 */
static void
bbl_config_init(void)
{
    bool ack = false;

    /* Silently ignore if config group is missing altogether */
    if( !mce_conf_has_group(MCE_CONF_BUTTON_BACKLIGHT_GROUP) )
        goto EXIT;

    bbl_control_path =
        mce_conf_get_string(MCE_CONF_BUTTON_BACKLIGHT_GROUP,
                            MCE_CONF_BUTTON_BACKLIGHT_CONTROL_PATH, 0);

    bbl_control_value_enable =
        mce_conf_get_string(MCE_CONF_BUTTON_BACKLIGHT_GROUP,
                            MCE_CONF_BUTTON_BACKLIGHT_CONTROL_VALUE_ENABLE, 0);

    bbl_control_value_disable =
        mce_conf_get_string(MCE_CONF_BUTTON_BACKLIGHT_GROUP,
                            MCE_CONF_BUTTON_BACKLIGHT_CONTROL_VALUE_DISABLE, 0);

    if( !bbl_config_exists() ) {
        mce_log(LL_WARN, "Config group [%s] is missing required entries",
                MCE_CONF_BUTTON_BACKLIGHT_GROUP);
        goto EXIT;
    }

    if( access(bbl_control_path, W_OK) == -1 ) {
        mce_log(LL_WARN, "%s: is not writable: %m", bbl_control_path);
        goto EXIT;
    }

    ack = true;

EXIT:
    /* All or nothing */
    if( !ack )
        bbl_config_quit();
}

/** Release button backlight configuration
 */
static void
bbl_config_quit(void)
{
    g_free(bbl_control_path),
        bbl_control_path = 0;

    g_free(bbl_control_value_enable),
        bbl_control_value_enable = 0;

    g_free(bbl_control_value_disable),
        bbl_control_value_disable = 0;
}

/* ========================================================================= *
 * G_MODULE
 * ========================================================================= */

/** Init function for the button backlight module
 *
 * @param module Unused
 *
 * @return NULL on success, a string with an error message on failure
 */

const gchar *
g_module_check_init(GModule *module)
{
    (void)module;

    /* Lookup static configuration */
    bbl_config_init();

    /* Install datapipe hooks */
    bbl_datapipe_init();

    /* Install dbus handlers */
    bbl_dbus_init();

    return NULL;
}

/** Exit function for the button backlight module
 *
 * @param module Unused
 */
void
g_module_unload(GModule *module)
{
    (void)module;

    /* Remove dbus handlers */
    bbl_dbus_quit();

    /* Remove datapipe hooks */
    bbl_datapipe_quit();

    /* Do not leave backlight on when mce is exiting */
    bbl_state_set_logical(TRISTATE_FALSE);

    /* Release static configuration */
    bbl_config_quit();

    bbl_inactive_cancel();
    return;
}
