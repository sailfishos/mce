/**
 * @file bluetooth.c
 * Bluetooth module -- this implements bluez tracking for MCE
 * <p>
 * Copyright (c) 2014 - 2019 Jolla Ltd.
 * Copyright (c) 2026 Jolla Mobile Ltd
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
#include "../libwakelock.h"

#include <gmodule.h>

/* Unlike the other standard dbus interfaces, the object manager seems
 * not to be defined in dbus-shared.h header file ... */
#ifndef  DBUS_INTERFACE_OBJECT_MANAGER
# define DBUS_INTERFACE_OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#endif

/* ------------------------------------------------------------------------- *
 * SUSPEND_BLOCK
 * ------------------------------------------------------------------------- */

static gboolean bluetooth_suspend_block_timer_cb(gpointer aptr);
static void     bluetooth_suspend_block_stop(void);
static void     bluetooth_suspend_block_start(void);

/* ------------------------------------------------------------------------- *
 * LEDS
 * ------------------------------------------------------------------------- */

static void     bluetooth_leds_update(int discovering, int connected);

/* ------------------------------------------------------------------------- *
 * DBUS_HANDLERS
 * ------------------------------------------------------------------------- */

static gboolean bluetooth_dbus_bluez4_signal_cb(DBusMessage *const msg);

static void     bluetooth_dbus_bluez5_parse_changed_properties(DBusMessageIter *body);
static void     bluetooth_dbus_bluez5_parse_dropped_properties(DBusMessageIter *body);
static gboolean bluetooth_dbus_bluez5_signal_cb(DBusMessage *const msg);

static void     bluetooth_dbus_init(void);
static void     bluetooth_dbus_quit(void);

/* ------------------------------------------------------------------------- *
 * DATAPIPE_TRACKING
 * ------------------------------------------------------------------------- */

static void     bluetooth_datapipe_bluez_service_state_cb(gconstpointer const data);
static void     bluetooth_datapipe_init(void);
static void     bluetooth_datapipe_quit(void);

/* ------------------------------------------------------------------------- *
 * MODULE_LOAD_UNLOAD
 * ------------------------------------------------------------------------- */

G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
G_MODULE_EXPORT void         g_module_unload(GModule *module);

/* ========================================================================= *
 * SUSPEND_BLOCK
 * ========================================================================= */

/** How long bluetooth dbus activity is allowed to delay suspend */
#define BLUETOOTH_SUSPEND_BLOCK_MS 2000

/** Timer id for cancelling suspend blocking */
static guint bluetooth_suspend_block_timer_id = 0;

/** Timer callback for cancelling suspend blocking
 *
 * @param aptr user data pointer (not used)
 *
 * @return FALSE to stop timer from repeating
 */
static gboolean bluetooth_suspend_block_timer_cb(gpointer aptr)
{
    (void)aptr; // not used

    if( bluetooth_suspend_block_timer_id ) {
        bluetooth_suspend_block_timer_id = 0;
        mce_log(LL_DEVEL, "bt suspend blocking ended");
        wakelock_unlock("mce_bluez_wait");
    }
    return FALSE;
}

/** Cancel suspend blocking
 */
static void bluetooth_suspend_block_stop(void)
{
    if( bluetooth_suspend_block_timer_id ) {
        g_source_remove(bluetooth_suspend_block_timer_id),
            bluetooth_suspend_block_timer_id = 0;
        mce_log(LL_DEVEL, "bt suspend blocking cancelled");
        wakelock_unlock("mce_bluez_wait");
    }
}

/** Start/extend suspend blocking
 */
static void bluetooth_suspend_block_start(void)
{
    if( bluetooth_suspend_block_timer_id ) {
        g_source_remove(bluetooth_suspend_block_timer_id);
    }
    else {
        wakelock_lock("mce_bluez_wait", -1);
        mce_log(LL_DEVEL, "bt suspend blocking started");
    }
    bluetooth_suspend_block_timer_id =
        g_timeout_add(BLUETOOTH_SUSPEND_BLOCK_MS,
                      bluetooth_suspend_block_timer_cb, 0);
}

/* ========================================================================= *
 * LEDS
 * ========================================================================= */

static void
bluetooth_leds_update(int discovering, int connected)
{
    if( discovering != -1 ) {
        if( discovering )
            datapipe_exec_full(&led_pattern_activate_pipe, MCE_LED_PATTERN_BLUETOOTH_IN_DISCOVERY_MODE);
        else
            datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_BLUETOOTH_IN_DISCOVERY_MODE );
    }

    if( connected != -1 ) {
        if( connected ) {
            datapipe_exec_full(&led_pattern_activate_pipe, MCE_LED_PATTERN_BLUETOOTH_DEVICE_CONNECTED);
            datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_BLUETOOTH_DEVICE_DISCONNECTED);
        }
        else {
            datapipe_exec_full(&led_pattern_activate_pipe, MCE_LED_PATTERN_BLUETOOTH_DEVICE_DISCONNECTED);
            datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_BLUETOOTH_DEVICE_CONNECTED);
        }
    }
}

/* ========================================================================= *
 * DBUS_HANDLERS
 * ========================================================================= */

/** Handle signal originating from bluez4
 *
 * MCE is not interested in the signal content per se, any incoming
 * signals just mean there is bluetooth activity and mce should allow
 * related ipc and processing to happen without the device getting
 * suspened too soon.
 *
 * @param msg dbus signal message
 *
 * @return TRUE
 */
static gboolean
bluetooth_dbus_bluez4_signal_cb(DBusMessage *const msg)
{
    if( mce_log_p(LL_DEBUG) ) {
        char *repr = mce_dbus_message_repr(msg);
        mce_log(LL_DEBUG, "%s", repr ?: "bluez sig");
        free(repr);
    }

    bluetooth_suspend_block_start();
    return TRUE;
}

#define PROP_DISCOVERING "Discovering"
#define PROP_CONNECTED   "Connected"

static void
bluetooth_dbus_bluez5_parse_changed_properties(DBusMessageIter *body)
{
    // Initialize to "no changes seen"
    int discovering = -1;
    int connected   = -1;

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

        if( !strcmp(key, PROP_DISCOVERING) ) {
            bool val = false;
            if( !mce_dbus_iter_get_bool(&var, &val) )
                goto EXIT;
            mce_log(LL_DEBUG, "%s = bool %d", key, val);
            discovering = val;
        }
        else if( !strcmp(key, PROP_CONNECTED) ) {
            bool val = false;
            if( !mce_dbus_iter_get_bool(&var, &val) )
                goto EXIT;
            mce_log(LL_DEBUG, "%s = bool %d", key, val);
            connected = val;
        }
        else if( mce_log_p(LL_DEBUG) ) {
            char *val = mce_dbus_message_iter_repr(&var);
            mce_log(LL_DEBUG, "%s = %s", key, val);
            free(val);
        }
    }

    bluetooth_leds_update(discovering, connected);

EXIT:

    return;
}

static void
bluetooth_dbus_bluez5_parse_dropped_properties(DBusMessageIter *body)
{
    // Initialize to "no changes seen"
    int discovering = -1;
    int connected   = -1;

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

        if( !strcmp(key, PROP_DISCOVERING) ) {
            discovering = false;
        }
        else if( !strcmp(key, PROP_CONNECTED) ) {
            connected = false;
        }
    }

    bluetooth_leds_update(discovering, connected);

EXIT:
    return;
}

/** Handle signal originating from bluez5
 *
 * MCE is not interested in the signal content per se, any incoming
 * signals just mean there is bluetooth activity and mce should allow
 * related ipc and processing to happen without the device getting
 * suspened too soon.
 *
 * @param msg dbus signal message
 *
 * @return TRUE
 */
static gboolean
bluetooth_dbus_bluez5_signal_cb(DBusMessage *const msg)
{
    static const char name[] = "org.bluez";

    /* Note: The signal match rule can and should use the
     *       well-known name, but the actual signals that
     *       we receive are going to have the private name
     *       as sender.
     */

    /* Get name owner from tracking cache. Assume that
     * no bluez signals are sent before the well known
     * name is claimed or after it is released. */
    const char *owner = mce_dbus_nameowner_get(name);
    if( !owner )
        goto EXIT;

    /* Check if the signal sender matches the supposed
     * owner (or the well-known-name, just in case) */
    const char *sender = dbus_message_get_sender(msg);
    if( !sender )
        goto EXIT;

    if( strcmp(sender, owner) && strcmp(sender, name) )
        goto EXIT;

    DBusMessageIter body;

    dbus_message_iter_init(msg, &body);

    /* Check whether the property changes are about an
     * interface we are interested in */
    const char *interface = NULL;
    if( !mce_dbus_iter_get_string(&body, &interface) )
        goto EXIT;

    static const char adapter[] = "org.bluez.Adapter";
    static const char device[]  = "org.bluez.Device";

    if( !g_str_has_prefix(interface, adapter) && !g_str_has_prefix(interface, device) )
        goto EXIT;

    /* We are not actually interested in tracking
     * adapter / device states, we just want to see
     * and react to some transition events.
     *
     * Assumptions:
     * - Adapters have PROP_DISCOVERING, devices do not
     * - Devices have PROP_CONNECTED, adapters do not
     * - Bluez sends only true property changes
     *
     * -> can use common code for both interfaces that
     *    looks for PROP_DISCOVERING / PROP_CONNECTED
     *    change notifications.
     */
    mce_log(LL_DEBUG, "properties changed");

    bluetooth_dbus_bluez5_parse_changed_properties(&body);
    bluetooth_dbus_bluez5_parse_dropped_properties(&body);

    bluetooth_suspend_block_start();

EXIT:
    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t bluetooth_dbus_handlers[] =
{
    /* bluez4 signals */
    {
        .interface = "org.bluez.Manager",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = bluetooth_dbus_bluez4_signal_cb,
    },
    {
        .interface = "org.bluez.Adapter",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = bluetooth_dbus_bluez4_signal_cb,
    },
    {
        .interface = "org.bluez.Device",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = bluetooth_dbus_bluez4_signal_cb,
    },
    {
        .interface = "org.bluez.Input",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = bluetooth_dbus_bluez4_signal_cb,
    },
    {
        .interface = "org.bluez.Audio",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = bluetooth_dbus_bluez4_signal_cb,
    },
    {
        .interface = "org.bluez.SerialProxyManager",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = bluetooth_dbus_bluez4_signal_cb,
    },
    /* bluez5 signals */
    {
        .sender    = "org.bluez",
        .interface = DBUS_INTERFACE_OBJECT_MANAGER,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .name      = "InterfacesAdded",
        .callback  = bluetooth_dbus_bluez5_signal_cb,
    },
    {
        .sender    = "org.bluez",
        .interface = DBUS_INTERFACE_OBJECT_MANAGER,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .name      = "InterfacesRemoved",
        .callback  = bluetooth_dbus_bluez5_signal_cb,
    },
    {
        .sender    = "org.bluez",
        .interface = DBUS_INTERFACE_PROPERTIES,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .name      = "PropertiesChanged",
        .callback  = bluetooth_dbus_bluez5_signal_cb,
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Add dbus handlers
 */
static void bluetooth_dbus_init(void)
{
    mce_dbus_handler_register_array(bluetooth_dbus_handlers);
}

/** Remove dbus handlers
 */
static void bluetooth_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(bluetooth_dbus_handlers);
}
/* ========================================================================= *
 * DATAPIPE_TRACKING
 * ========================================================================= */

/** Availability of bluez; from bluez_service_state_pipe */
static service_state_t bluez_service_state = SERVICE_STATE_UNDEF;

/** Datapipe trigger for bluez availability
 *
 * @param data bluez D-Bus service availability (as a void pointer)
 */
static void bluetooth_datapipe_bluez_service_state_cb(gconstpointer const data)
{
    service_state_t prev = bluez_service_state;
    bluez_service_state = GPOINTER_TO_INT(data);

    if( bluez_service_state == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "bluez dbus service: %s -> %s",
            service_state_repr(prev),
            service_state_repr(bluez_service_state));

    switch( bluez_service_state ) {
    case SERVICE_STATE_RUNNING:
    case SERVICE_STATE_STOPPED:
        bluetooth_suspend_block_start();
        break;

    default:
        break;
    }

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t bluetooth_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &bluez_service_state_pipe,
        .output_cb = bluetooth_datapipe_bluez_service_state_cb,
    },
    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t bluetooth_datapipe_bindings =
{
    .module   = "bluetooth",
    .handlers = bluetooth_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void bluetooth_datapipe_init(void)
{
    mce_datapipe_init_bindings(&bluetooth_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void bluetooth_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&bluetooth_datapipe_bindings);
}

/* ========================================================================= *
 * MODULE_LOAD_UNLOAD
 * ========================================================================= */

/** Init function for the bluetooth module
 *
 * @param module (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    bluetooth_datapipe_init();

    bluetooth_dbus_init();

    return 0;
}

/** Exit function for the bluetooth module
 *
 * @param module (not used)
 */
void g_module_unload(GModule *module)
{
    (void)module;

    bluetooth_datapipe_quit();

    bluetooth_dbus_quit();

    bluetooth_suspend_block_stop();
}
