/**
 * @file bluetooth.c
 * Bluetooth module -- this implements bluez tracking for MCE
 * <p>
 * Copyright (C) 2014 Jolla Ltd
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

#include <stdlib.h>

#include <gmodule.h>

/* ------------------------------------------------------------------------- *
 * SUSPEND_BLOCK
 * ------------------------------------------------------------------------- */

static gboolean bluetooth_suspend_block_timer_cb(gpointer aptr);
static void     bluetooth_suspend_block_stop(void);
static void     bluetooth_suspend_block_start(void);

/* ------------------------------------------------------------------------- *
 * DBUS_HANDLERS
 * ------------------------------------------------------------------------- */

static gboolean bluetooth_dbus_bluez4_signal_cb(DBusMessage *const msg);

static void     bluetooth_dbus_init(void);
static void     bluetooth_dbus_quit(void);

/* ------------------------------------------------------------------------- *
 * MODULE_LOAD_UNLOAD
 * ------------------------------------------------------------------------- */

G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
G_MODULE_EXPORT void         g_module_unload(GModule *module);

/* ========================================================================= *
 * SUSPEND_BLOCK
 * ========================================================================= */

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
        g_timeout_add(5000, bluetooth_suspend_block_timer_cb, 0);
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
{
    if( mce_log_p(LL_DEBUG) ) {
        char *repr = mce_dbus_message_repr(msg);
        mce_log(LL_DEBUG, "%s", repr ?: "bluez sig");
        free(repr);
    }

    bluetooth_suspend_block_start();
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

/** Availability of bluez; from bluez_available_pipe */
static service_state_t bluez_available = SERVICE_STATE_UNDEF;

/** Datapipe trigger for bluez availability
 *
 * @param data bluez D-Bus service availability (as a void pointer)
 */
static void bluetooth_datapipe_bluez_available_cb(gconstpointer const data)
{
    service_state_t prev = bluez_available;
    bluez_available = GPOINTER_TO_INT(data);

    if( bluez_available == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "bluez dbus service: %s -> %s",
            service_state_repr(prev),
            service_state_repr(bluez_available));

    switch( bluez_available ) {
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
        .datapipe  = &bluez_available_pipe,
        .output_cb = bluetooth_datapipe_bluez_available_cb,
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
    datapipe_bindings_init(&bluetooth_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void bluetooth_datapipe_quit(void)
{
    datapipe_bindings_quit(&bluetooth_datapipe_bindings);
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
