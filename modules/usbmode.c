/**
 * @file usbmode.c
 *
 * USB mode tracking module for the Mode Control Entity
 * <p>
 * Copyright © 2015 Jolla Ltd.
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

#include <string.h>

#include <gmodule.h>

#include <usb_moded-dbus.h>
#include <usb_moded-modes.h>

/* ========================================================================= *
 * CABLE_STATE
 * ========================================================================= */

static usb_cable_state_t usbmode_cable_state_lookup (const char *name);
static void              usbmode_cable_state_update (const char *mode);

/* ========================================================================= *
 * DBUS_IPC
 * ========================================================================= */

static void usbmode_dbus_query_cb     (DBusPendingCall *pc, void *aptr);
static void usbmode_dbus_query_cancel (void);
static void usbmode_dbus_query_start  (void);

/* ========================================================================= *
 * DBUS_HANDLERS
 * ========================================================================= */

static gboolean usbmode_dbus_mode_changed_cb (DBusMessage *const msg);

static void     usbmode_dbus_init            (void);
static void     usbmode_dbus_quit            (void);

/* ========================================================================= *
 * DATAPIPE_HANDLERS
 * ========================================================================= */

static void usbmode_datapipe_usbmoded_service_state_cb (gconstpointer data);

static void usbmode_datapipe_init                  (void);
static void usbmode_datapipe_quit                  (void);

/* ========================================================================= *
 * MODULE_LOAD_UNLOAD
 * ========================================================================= */

G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
G_MODULE_EXPORT void         g_module_unload(GModule *module);

/* ========================================================================= *
 * CABLE_STATE
 * ========================================================================= */

/** Lookup table for mode strings usb_moded can be expected to emit
 *
 * The set of available modes is not static. New modes can be added
 * via usb-moded configuration files, but basically
 *
 * - "undefined" means cable is not connected
 * - any other name means cable is connected (=charging should be possible)
 * - some special cases signify that fs is mounted or otherwise directly
 *   accessed via usb (mass storage and mtp modes)
 */
static const struct
{
    const char        *mode;
    usb_cable_state_t  state;
} mode_lut[] =
{
    /* No cable attached */
    { MODE_UNDEFINED,             USB_CABLE_DISCONNECTED },

    /* Attach / detach dedicated charger */
    { CHARGER_CONNECTED,          USB_CABLE_CONNECTED    },
    { MODE_CHARGER,               USB_CABLE_CONNECTED    },
    { CHARGER_DISCONNECTED,       USB_CABLE_DISCONNECTED },

    /* Attach / detach pc cable */
    { USB_CONNECTED,              USB_CABLE_CONNECTED    },
    { MODE_CHARGING_FALLBACK,     USB_CABLE_CONNECTED    },
    { USB_CONNECTED_DIALOG_SHOW,  USB_CABLE_ASK_USER     },
    { MODE_ASK,                   USB_CABLE_ASK_USER     },
    { MODE_MASS_STORAGE,          USB_CABLE_CONNECTED    },
    { MODE_MTP,                   USB_CABLE_CONNECTED    },
    { MODE_PC_SUITE,              USB_CABLE_CONNECTED    },
    { MODE_DEVELOPER,             USB_CABLE_CONNECTED    },
    { MODE_CHARGING,              USB_CABLE_CONNECTED    },
    { MODE_HOST,                  USB_CABLE_CONNECTED    },
    { MODE_CONNECTION_SHARING,    USB_CABLE_CONNECTED    },
    { MODE_DIAG,                  USB_CABLE_CONNECTED    },
    { MODE_ADB,                   USB_CABLE_CONNECTED    },
    { USB_DISCONNECTED,           USB_CABLE_DISCONNECTED },

    /* Busy can occur both on connect / after disconnect */
    { MODE_BUSY,                  USB_CABLE_UNDEF        },

    /* Events ignored while evaluating cable state */
    { DATA_IN_USE,                USB_CABLE_UNDEF        },
    { USB_REALLY_DISCONNECT,      USB_CABLE_UNDEF        },
    { USB_PRE_UNMOUNT,            USB_CABLE_UNDEF        },
    { RE_MOUNT_FAILED,            USB_CABLE_UNDEF        },
    { MODE_SETTING_FAILED,        USB_CABLE_UNDEF        },
    { UMOUNT_ERROR,               USB_CABLE_UNDEF        },
};

/** Map reported usb mode to usb_cable_state_t used within mce
 *
 * @param mode Name of USB mode as reported by usb_moded
 *
 * @return usb_cable_state_t enumeration value
 */
static usb_cable_state_t
usbmode_cable_state_lookup(const char *mode)
{
    usb_cable_state_t state = USB_CABLE_DISCONNECTED;

    /* Getting a null/empty string here means that for one or another
     * reason we were not able to get the current mode from usb_moded. */
    if( !mode || !*mode )
        goto cleanup;

    /* Try to lookup from known set of modes */
    for( size_t i = 0; i < G_N_ELEMENTS(mode_lut); ++i ) {
        if( strcmp(mode_lut[i].mode, mode) )
            continue;

        state = mode_lut[i].state;
        goto cleanup;
    }

    /* The "undefined" that usb_moded uses to signal no usb cable connected
     * is included in the lookup table -> any unknown mode name is assumed
     * to mean that cable is connected & charging should be possible */

    mce_log(LL_INFO, "unknown usb mode '%s'; assuming connected", mode);
    state = USB_CABLE_CONNECTED;

cleanup:
    return state;
}

/** Update usb_cable_state_pipe according tomatch  USB mode reported by usb_moded
 *
 * @param mode Name of USB mode as reported by usb_moded
 */
static void
usbmode_cable_state_update(const char *mode)
{
    mce_log(LL_NOTICE, "usb mode: %s", mode);

    usb_cable_state_t prev = datapipe_get_gint(usb_cable_state_pipe);
    usb_cable_state_t curr = usbmode_cable_state_lookup(mode);

    if( curr == USB_CABLE_UNDEF )
        goto EXIT;

    if( prev == curr )
        goto EXIT;

    mce_log(LL_DEVEL, "usb cable state: %s -> %s",
            usb_cable_state_repr(prev),
            usb_cable_state_repr(curr));

    datapipe_exec_full(&usb_cable_state_pipe, GINT_TO_POINTER(curr),
                       DATAPIPE_USE_INDATA, DATAPIPE_CACHE_INDATA);
EXIT:
    return;
}

/* ========================================================================= *
 * DBUS_IPC
 * ========================================================================= */

static DBusPendingCall *usbmode_dbus_query_pc = 0;

/** Handle reply to async query made from usbmode_dbus_query_async()
 */
static void
usbmode_dbus_query_cb(DBusPendingCall *pc, void *aptr)
{
    (void)aptr;

    DBusMessage *rsp  = 0;
    DBusError    err  = DBUS_ERROR_INIT;
    const char  *mode = 0;

    if( pc != usbmode_dbus_query_pc )
        goto EXIT;

    usbmode_dbus_query_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
        mce_log(LL_WARN, "no reply");
        goto EXIT;
    }

    if( dbus_set_error_from_message(&err, rsp) ||
        !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_STRING, &mode,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    usbmode_cable_state_update(mode);

EXIT:
    if( rsp ) dbus_message_unref(rsp);

    dbus_error_free(&err);

    dbus_pending_call_unref(pc);

    return;
}

/** Cancel pending async usb mode query
 */
static void
usbmode_dbus_query_cancel(void)
{
    if( usbmode_dbus_query_pc ) {
        dbus_pending_call_cancel(usbmode_dbus_query_pc);
        dbus_pending_call_unref(usbmode_dbus_query_pc);
        usbmode_dbus_query_pc = 0;
    }
}

/** Initiate async query to find out current usb mode
 */
static void
usbmode_dbus_query_start(void)
{
    usbmode_dbus_query_cancel();

    dbus_send_ex(USB_MODED_DBUS_SERVICE,
                 USB_MODED_DBUS_OBJECT,
                 USB_MODED_DBUS_INTERFACE,
                 USB_MODED_QUERY_MODE_REQ,
                 usbmode_dbus_query_cb, 0, 0,
                 &usbmode_dbus_query_pc,
                 DBUS_TYPE_INVALID);
}

/* ========================================================================= *
 * DBUS_HANDLERS
 * ========================================================================= */

/** Handle usb mode change signals
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean usbmode_dbus_mode_changed_cb(DBusMessage *const msg)
{
    const char *mode = 0;
    DBusError   err  = DBUS_ERROR_INIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &mode,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_WARN, "parse error: %s: %s", err.name, err.message);
        goto EXIT;
    }

    usbmode_cable_state_update(mode);

EXIT:
    dbus_error_free(&err);

    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t usbmode_dbus_handlers[] =
{
    /* signals */
    {
        .interface = USB_MODED_DBUS_INTERFACE,
        .name      = USB_MODED_MODE_CHANGED_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = usbmode_dbus_mode_changed_cb,
    },

    /* sentinel */
    {
        .interface = 0
    }
};

/** Install dbus message handlers
 */
static void usbmode_dbus_init(void)
{
    mce_dbus_handler_register_array(usbmode_dbus_handlers);
}

/** Remove dbus message handlers
 */
static void usbmode_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(usbmode_dbus_handlers);
}

/* ========================================================================= *
 * DATAPIPE_HANDLERS
 * ========================================================================= */

static service_state_t usbmoded_service_state = SERVICE_STATE_UNDEF;

/** Handle display_state_request_pipe notifications
 *
 * This is where display state transition starts
 *
 * @param data Requested display_state_t (as void pointer)
 */
static void usbmode_datapipe_usbmoded_service_state_cb(gconstpointer data)
{
    service_state_t prev = usbmoded_service_state;
    usbmoded_service_state = GPOINTER_TO_INT(data);

    if( usbmoded_service_state == prev )
        goto EXIT;

    mce_log(LL_NOTICE, "usbmoded_service_state = %s -> %s",
            service_state_repr(prev),
            service_state_repr(usbmoded_service_state));

    if( usbmoded_service_state == SERVICE_STATE_RUNNING )
        usbmode_dbus_query_start();
    else
        usbmode_dbus_query_cancel();

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t usbmode_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &usbmoded_service_state_pipe,
        .output_cb = usbmode_datapipe_usbmoded_service_state_cb,
    },

    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t usbmode_datapipe_bindings =
{
    .module   = "usbmode",
    .handlers = usbmode_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void usbmode_datapipe_init(void)
{
    // triggers
    datapipe_bindings_init(&usbmode_datapipe_bindings);
}

/** Remove triggers/filters from datapipes */
static void usbmode_datapipe_quit(void)
{
    // triggers
    datapipe_bindings_quit(&usbmode_datapipe_bindings);
}

/* ========================================================================= *
 * MODULE_LOAD_UNLOAD
 * ========================================================================= */

/** Init function for the usb mode tracking module
 *
 * @param module (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    /* Append triggers/filters to datapipes */
    usbmode_datapipe_init();

    /* Install dbus message handlers */
    usbmode_dbus_init();

    return NULL;
}

/** Exit function for the usb mode tracking module
 *
 * @param module (not used)
 */
void g_module_unload(GModule *module)
{
    (void)module;

    /* Remove dbus message handlers */
    usbmode_dbus_quit();

    /* Remove triggers/filters from datapipes */
    usbmode_datapipe_quit();

    /* Stop timers, async calls etc */
    usbmode_dbus_query_cancel();
    return;
}
