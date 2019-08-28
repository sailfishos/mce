/**
 * @file mce-common.c
 * Common state logic for Mode Control Entity
 * <p>
 * Copyright (C) 2017-2019 Jolla Ltd.
 * Copyright (c) 2019 Open Mobile Platform LLC.
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
#include "mce-common.h"

#include "mce.h"
#include "mce-dbus.h"
#include "mce-lib.h"
#include "mce-log.h"

#include <string.h>

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

/* ========================================================================= *
 * TYPES
 * ========================================================================= */

/** Bookkeeping data for on-condition callback function */
typedef struct
{
    /** Source identification, for mass cancelation */
    gchar         *srce;

    /** Function to call */
    GDestroyNotify func;

    /** Parameter to give to the function */
    gpointer       aptr;
} on_condition_t;

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * ON_CONDITION
 * ------------------------------------------------------------------------- */

static bool            on_condition_matches           (on_condition_t *self, const char *srce, GDestroyNotify func, gpointer aptr);
static on_condition_t *on_condition_create            (const char *srce, GDestroyNotify func, gpointer aptr);
static void            on_condition_delete            (on_condition_t *self);
static void            on_condition_exec              (on_condition_t *self);
static void            on_condition_exec_and_delete_cb(gpointer self);
static void            on_condition_delete_cb         (gpointer self);

/* ------------------------------------------------------------------------- *
 * COMMON_ON_PROXIMITY
 * ------------------------------------------------------------------------- */

static gboolean common_on_proximity_exec_cb (gpointer aptr);
static void     common_on_proximity_exec    (void);
void            common_on_proximity_schedule(const char *srce, GDestroyNotify func, gpointer aptr);
void            common_on_proximity_cancel  (const char *srce, GDestroyNotify func, gpointer aptr);
static void     common_on_proximity_quit    (void);

/* ------------------------------------------------------------------------- *
 * COMMON_DBUS
 * ------------------------------------------------------------------------- */

static void     common_dbus_send_usb_cable_state  (DBusMessage *const req);
static gboolean common_dbus_get_usb_cable_state_cb(DBusMessage *const req);
static void     common_dbus_send_charger_type     (DBusMessage *const req);
static gboolean common_dbus_get_charger_type_cb   (DBusMessage *const req);
static void     common_dbus_send_charger_state    (DBusMessage *const req);
static gboolean common_dbus_get_charger_state_cb  (DBusMessage *const req);
static void     common_dbus_send_battery_status   (DBusMessage *const req);
static gboolean common_dbus_get_battery_status_cb (DBusMessage *const req);
static void     common_dbus_send_battery_level    (DBusMessage *const req);
static gboolean common_dbus_get_battery_level_cb  (DBusMessage *const req);
static gboolean common_dbus_initial_cb            (gpointer aptr);
static void     common_dbus_init                  (void);
static void     common_dbus_quit                  (void);

/* ------------------------------------------------------------------------- *
 * COMMON_DATAPIPE
 * ------------------------------------------------------------------------- */

static void common_datapipe_usb_cable_state_cb        (gconstpointer data);
static void common_datapipe_charger_type_cb           (gconstpointer data);
static void common_datapipe_charger_state_cb          (gconstpointer data);
static void common_datapipe_battery_status_cb         (gconstpointer data);
static void common_datapipe_battery_level_cb          (gconstpointer data);
static void common_datapipe_proximity_sensor_actual_cb(gconstpointer data);
static void common_datapipe_init                      (void);
static void common_datapipe_quit                      (void);

/* ------------------------------------------------------------------------- *
 * MCE_COMMON
 * ------------------------------------------------------------------------- */

bool mce_common_init(void);
void mce_common_quit(void);

/* ========================================================================= *
 * STATE_DATA
 * ========================================================================= */

/** USB cable status; assume undefined */
static usb_cable_state_t usb_cable_state = USB_CABLE_UNDEF;

/** Charger type; assume none */
static charger_type_t charger_type = CHARGER_TYPE_NONE;

/** Charger state; assume undefined */
static charger_state_t charger_state = CHARGER_STATE_UNDEF;

/** Battery status; assume undefined */
static battery_status_t battery_status = BATTERY_STATUS_UNDEF;

/** Battery charge level: assume 100% */
static gint battery_level = BATTERY_LEVEL_INITIAL;

/** Cached (raw) proximity sensor state */
static cover_state_t proximity_sensor_actual = COVER_UNDEF;

/* ========================================================================= *
 * ON_CONDITION
 * ========================================================================= */

static bool
on_condition_matches(on_condition_t *self,
                     const char *srce, GDestroyNotify func, gpointer aptr)
{
    bool matches = false;

    if( !srce || !self || !self->srce )
        goto EXIT;

    if( strcmp(self->srce, srce) )
        goto EXIT;

    if( self->func == func && self->aptr == aptr )
        matches = true;
    else
        matches = !func && !srce;

EXIT:
    return matches;
}

static on_condition_t *
on_condition_create(const char *srce, GDestroyNotify func, gpointer aptr)
{
    on_condition_t *self = g_slice_alloc0(sizeof *self);
    self->srce = g_strdup(srce);
    self->func = func;
    self->aptr = aptr;
    return self;
}

static void
on_condition_delete(on_condition_t *self)
{
    if( self ) {
        g_free(self->srce);
        g_slice_free1(sizeof *self, self);
    }
}

static void
on_condition_exec(on_condition_t *self)
{
    if( self )
        self->func(self->aptr);
}

static void
on_condition_exec_and_delete_cb(gpointer self)
{
    on_condition_exec(self);
    on_condition_delete(self);
}

static void
on_condition_delete_cb(gpointer self)
{
    on_condition_delete(self);
}

/* ========================================================================= *
 * COMMON_ON_PROXIMITY
 * ========================================================================= */

#define COMMON_ON_DEMAND_TAG "common_on_proximity"

static GSList *common_on_proximity_actions = 0;
static guint   common_on_proximity_exec_id = 0;

static gboolean
common_on_proximity_exec_cb(gpointer aptr)
{
    (void)aptr;

    gboolean result = G_SOURCE_REMOVE;

    GSList *todo;

    /* Execute queued actions */
    if( (todo = common_on_proximity_actions) ) {
        common_on_proximity_actions = 0;
        todo = g_slist_reverse(todo);
        g_slist_free_full(todo, on_condition_exec_and_delete_cb);
    }

    /* Check if executed actions queued more actions */
    if( common_on_proximity_actions ) {
        /* Repeat to handle freshly added actions */
        result = G_SOURCE_CONTINUE;
    }
    else {
        /* Queue exchausted - sensor no longer needed */
        datapipe_exec_full(&proximity_sensor_required_pipe,
                           PROXIMITY_SENSOR_REQUIRED_REM
                           COMMON_ON_DEMAND_TAG);
    }

    if( result == G_SOURCE_REMOVE )
        common_on_proximity_exec_id = 0;

    return result;
}

static void
common_on_proximity_exec(void)
{
    /* Execute via idle to make sure all proximity
     * datapipe listeners have had a chance to
     * register sensor state before callbacks
     * get triggered. */
    if( !common_on_proximity_exec_id ) {
        common_on_proximity_exec_id =
            mce_wakelocked_idle_add(common_on_proximity_exec_cb,
                                    0);
    }
}

/** Execute callback function when actual proximity sensor state is available
 *
 * @param srce  Call site identification
 * @param func  Callback function pointer
 * @param aptr  Parameter for the callback function
 */
void
common_on_proximity_schedule(const char *srce, GDestroyNotify func, gpointer aptr)
{
    /* In order to execute actions in the requested order,
     * immediate execution can be allowed only when proximity
     * sensor state is known and the already queued actions
     * have been executed.
     */
    if( proximity_sensor_actual == COVER_UNDEF ||
        common_on_proximity_actions ||
        common_on_proximity_exec_id ) {
        // TODO: all failures to communicate sensor power up with
        //       sensorfwd should lead to mce-sensorfw module
        //       declaring proximity=not-covered, but having an
        //       explicit timeout here would not hurt ...
        if( !common_on_proximity_actions )
            datapipe_exec_full(&proximity_sensor_required_pipe,
                               PROXIMITY_SENSOR_REQUIRED_ADD
                               COMMON_ON_DEMAND_TAG);

        common_on_proximity_actions =
            g_slist_prepend(common_on_proximity_actions,
                            on_condition_create(srce, func, aptr));
    }
    else {
        func(aptr);
    }
}

/** Cancel pending on-proximity callback
 *
 * @param srce  Call site identification
 * @param func  Callback function pointer, or NULL for all
 * @param aptr  Parameter for the callback function, or NULL for all
 */
void
common_on_proximity_cancel(const char *srce, GDestroyNotify func, gpointer aptr)
{
    for( GSList *iter = common_on_proximity_actions; iter; iter = iter->next ) {
        on_condition_t *item = iter->data;

        if( !item )
            continue;

        if( !on_condition_matches(item, srce, func, aptr) )
            continue;

        /* Detach from list and delete.
         *
         * Note that the list itself is not garbage collected in order not
         * to disturb possibly pending execute etc logic -> all iterators
         * must be prepared to bump into empty links.
         */
        iter->data = 0;
        on_condition_delete(item);
    }
}

static void
common_on_proximity_quit(void)
{
    /* Cancel pending "on_condition" actions */
    g_slist_free_full(common_on_proximity_actions,
                      on_condition_delete_cb),
        common_on_proximity_actions = 0;

    /* Do not leave active timers behind */
    if( common_on_proximity_exec_id ) {
        g_source_remove(common_on_proximity_exec_id),
            common_on_proximity_exec_id = 0;
    }
}

/* ========================================================================= *
 * DBUS_FUNCTIONS
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * usb_cable_state
 * ------------------------------------------------------------------------- */

/** Send usb_cable_state D-Bus signal / method call reply
 *
 * @param req  method call message to reply, or NULL to send signal
 */
static void
common_dbus_send_usb_cable_state(DBusMessage *const req)
{
    static const char *last = 0;

    DBusMessage *msg = NULL;

    const char *value = usb_cable_state_to_dbus(usb_cable_state);

    if( req ) {
        msg = dbus_new_method_reply(req);
    }
    else if( last == value ) {
        goto EXIT;
    }
    else {
        last = value;
        msg = dbus_new_signal(MCE_SIGNAL_PATH,
                              MCE_SIGNAL_IF,
                              MCE_USB_CABLE_STATE_SIG);
    }

    if( !dbus_message_append_args(msg,
                                  DBUS_TYPE_STRING, &value,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    mce_log(LL_DEBUG, "%s: %s = %s",
            req ? "reply" : "broadcast",
            "usb_cable_state", value);

    dbus_send_message(msg), msg = 0;

EXIT:

    if( msg )
        dbus_message_unref(msg);
}

/** Callback for handling usb_cable_state D-Bus queries
 *
 * @param req  method call message to reply
 */
static gboolean
common_dbus_get_usb_cable_state_cb(DBusMessage *const req)
{
    mce_log(LL_DEBUG, "usb_cable_state query from: %s",
            mce_dbus_get_message_sender_ident(req));

    if( !dbus_message_get_no_reply(req) )
        common_dbus_send_usb_cable_state(req);

    return TRUE;
}

/* ------------------------------------------------------------------------- *
 * charger_type
 * ------------------------------------------------------------------------- */

/** Send charger_type D-Bus signal / method call reply
 *
 * @param req  method call message to reply, or NULL to send signal
 */
static void
common_dbus_send_charger_type(DBusMessage *const req)
{
    static const char *last = 0;

    DBusMessage *msg = NULL;

    const char *value = charger_type_to_dbus(charger_type);

    if( req ) {
        msg = dbus_new_method_reply(req);
    }
    else if( last == value ) {
        goto EXIT;
    }
    else {
        last = value;
        msg = dbus_new_signal(MCE_SIGNAL_PATH,
                              MCE_SIGNAL_IF,
                              MCE_CHARGER_TYPE_SIG);
    }

    if( !dbus_message_append_args(msg,
                                  DBUS_TYPE_STRING, &value,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    mce_log(LL_DEBUG, "%s: %s = %s",
            req ? "reply" : "broadcast",
            "charger_type", value);

    dbus_send_message(msg), msg = 0;

EXIT:

    if( msg )
        dbus_message_unref(msg);
}

/** Callback for handling charger_type D-Bus queries
 *
 * @param req  method call message to reply
 */
static gboolean
common_dbus_get_charger_type_cb(DBusMessage *const req)
{
    mce_log(LL_DEBUG, "charger_type query from: %s",
            mce_dbus_get_message_sender_ident(req));

    if( !dbus_message_get_no_reply(req) )
        common_dbus_send_charger_type(req);

    return TRUE;
}

/* ------------------------------------------------------------------------- *
 * charger_state
 * ------------------------------------------------------------------------- */

/** Send charger_state D-Bus signal / method call reply
 *
 * @param req  method call message to reply, or NULL to send signal
 */
static void
common_dbus_send_charger_state(DBusMessage *const req)
{
    static const char *last = 0;

    DBusMessage *msg = NULL;

    const char *value = charger_state_to_dbus(charger_state);

    if( req ) {
        msg = dbus_new_method_reply(req);
    }
    else if( last == value ) {
        goto EXIT;
    }
    else {
        last = value;
        msg = dbus_new_signal(MCE_SIGNAL_PATH,
                              MCE_SIGNAL_IF,
                              MCE_CHARGER_STATE_SIG);
    }

    if( !dbus_message_append_args(msg,
                                  DBUS_TYPE_STRING, &value,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    mce_log(LL_DEBUG, "%s: %s = %s",
            req ? "reply" : "broadcast",
            "charger_state", value);

    dbus_send_message(msg), msg = 0;

EXIT:

    if( msg )
        dbus_message_unref(msg);
}

/** Callback for handling charger_state D-Bus queries
 *
 * @param req  method call message to reply
 */
static gboolean
common_dbus_get_charger_state_cb(DBusMessage *const req)
{
    mce_log(LL_DEBUG, "charger_state query from: %s",
            mce_dbus_get_message_sender_ident(req));

    if( !dbus_message_get_no_reply(req) )
        common_dbus_send_charger_state(req);

    return TRUE;
}

/* ------------------------------------------------------------------------- *
 * battery_status
 * ------------------------------------------------------------------------- */

/** Send battery_status D-Bus signal / method call reply
 *
 * @param req  method call message to reply, or NULL to send signal
 */
static void
common_dbus_send_battery_status(DBusMessage *const req)
{
    static const char *last = 0;

    DBusMessage *msg = NULL;

    const char *value = battery_status_to_dbus(battery_status);

    if( req ) {
        msg = dbus_new_method_reply(req);
    }
    else if( last == value ) {
        goto EXIT;
    }
    else {
        last = value;
        msg = dbus_new_signal(MCE_SIGNAL_PATH,
                              MCE_SIGNAL_IF,
                              MCE_BATTERY_STATUS_SIG);
    }

    if( !dbus_message_append_args(msg,
                                  DBUS_TYPE_STRING, &value,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    mce_log(LL_DEBUG, "%s: %s = %s",
            req ? "reply" : "broadcast",
            "battery_status", value);

    dbus_send_message(msg), msg = 0;

EXIT:

    if( msg )
        dbus_message_unref(msg);
}

/** Callback for handling battery_status D-Bus queries
 *
 * @param req  method call message to reply
 */
static gboolean
common_dbus_get_battery_status_cb(DBusMessage *const req)
{
    mce_log(LL_DEBUG, "battery_status query from: %s",
            mce_dbus_get_message_sender_ident(req));

    if( !dbus_message_get_no_reply(req) )
        common_dbus_send_battery_status(req);

    return TRUE;
}

/* ------------------------------------------------------------------------- *
 * battery_level
 * ------------------------------------------------------------------------- */

/** Send battery_level D-Bus signal / method call reply
 *
 * @param req  method call message to reply, or NULL to send signal
 */
static void
common_dbus_send_battery_level(DBusMessage *const req)
{
    static dbus_int32_t last = MCE_BATTERY_LEVEL_UNKNOWN - 1;

    DBusMessage *msg = NULL;

    dbus_int32_t value = battery_level;

    /* Normalize to values allowed by MCE D-Bus api documentation */
    if( value < 0 )
        value = MCE_BATTERY_LEVEL_UNKNOWN;
    else if( value > 100 )
        value = 100;

    if( req ) {
        msg = dbus_new_method_reply(req);
    }
    else if( last == value ) {
        goto EXIT;
    }
    else {
        last = value;
        msg = dbus_new_signal(MCE_SIGNAL_PATH,
                              MCE_SIGNAL_IF,
                              MCE_BATTERY_LEVEL_SIG);
    }

    if( !dbus_message_append_args(msg,
                                  DBUS_TYPE_INT32, &value,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    mce_log(LL_DEBUG, "%s: %s = %d",
            req ? "reply" : "broadcast",
            "battery_level", value);

    dbus_send_message(msg), msg = 0;

EXIT:

    if( msg )
        dbus_message_unref(msg);
}

/** Callback for handling battery_level D-Bus queries
 *
 * @param req  method call message to reply
 */
static gboolean
common_dbus_get_battery_level_cb(DBusMessage *const req)
{
    mce_log(LL_DEBUG, "battery_level query from: %s",
            mce_dbus_get_message_sender_ident(req));

    if( !dbus_message_get_no_reply(req) )
        common_dbus_send_battery_level(req);

    return TRUE;
}

/* ------------------------------------------------------------------------- *
 * init/quit
 * ------------------------------------------------------------------------- */

/** Array of dbus message handlers */
static mce_dbus_handler_t common_dbus_handlers[] =
{
    /* signals - outbound (for Introspect purposes only) */
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_USB_CABLE_STATE_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"usb_cable_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_CHARGER_TYPE_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"charger_type\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_CHARGER_STATE_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"charger_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_BATTERY_STATUS_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"battery_status\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_BATTERY_LEVEL_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"battery_level\" type=\"i\"/>\n"
    },
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_USB_CABLE_STATE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = common_dbus_get_usb_cable_state_cb,
        .args      =
            "    <arg direction=\"out\" name=\"usb_cable_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_CHARGER_TYPE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = common_dbus_get_charger_type_cb,
        .args      =
            "    <arg direction=\"out\" name=\"charger_type\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_CHARGER_STATE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = common_dbus_get_charger_state_cb,
        .args      =
            "    <arg direction=\"out\" name=\"charger_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_BATTERY_STATUS_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = common_dbus_get_battery_status_cb,
        .args      =
            "    <arg direction=\"out\" name=\"battery_status\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_BATTERY_LEVEL_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = common_dbus_get_battery_level_cb,
        .args      =
            "    <arg direction=\"out\" name=\"battery_level\" type=\"i\"/>\n"
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Timer callback id for broadcasting initial states */
static guint common_dbus_initial_id = 0;

/** Timer callback function for broadcasting initial states
 *
 * @param aptr (not used)
 *
 * @return FALSE to stop idle callback from repeating
 */
static gboolean common_dbus_initial_cb(gpointer aptr)
{
    (void)aptr;

    /* Do explicit broadcast of initial states.
     *
     * Note that we expect nothing to happen here, unless the
     * datapipe initialization for some reason ends up leaving
     * some values to undefined state.
     */
    common_dbus_send_usb_cable_state(0);
    common_dbus_send_charger_type(0);
    common_dbus_send_charger_state(0);
    common_dbus_send_battery_status(0);
    common_dbus_send_battery_level(0);

    common_dbus_initial_id = 0;
    return FALSE;
}

/** Add dbus handlers
 */
static void common_dbus_init(void)
{
    mce_dbus_handler_register_array(common_dbus_handlers);

    /* To avoid unnecessary jitter on startup, allow dbus service tracking
     * and datapipe initialization some time to come up with proper initial
     * state values before forcing broadcasting to dbus */
    if( !common_dbus_initial_id )
        common_dbus_initial_id = g_timeout_add(1000, common_dbus_initial_cb, 0);
}

/** Remove dbus handlers
 */
static void common_dbus_quit(void)
{
    if( common_dbus_initial_id ) {
        g_source_remove(common_dbus_initial_id),
            common_dbus_initial_id = 0;
    }

    mce_dbus_handler_unregister_array(common_dbus_handlers);
}

/* ========================================================================= *
 * DATAPIPE_FUNCTIONS
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * usb_cable_state
 * ------------------------------------------------------------------------- */

/** Callback for handling usb_cable_state_pipe state changes
 *
 * @param data usb_cable_state (as void pointer)
 */
static void common_datapipe_usb_cable_state_cb(gconstpointer data)
{
    usb_cable_state_t prev = usb_cable_state;
    usb_cable_state = GPOINTER_TO_INT(data);

    if( usb_cable_state == prev )
        goto EXIT;

    /* The enumerated states do not have 1:1 string mapping, so to
     * avoid sending duplicate signals also the representation
     * values need to be checked. */
    const char *value_old = usb_cable_state_to_dbus(prev);
    const char *value_new = usb_cable_state_to_dbus(usb_cable_state);

    if( !strcmp(value_old, value_new) )
        goto EXIT;

    mce_log(LL_DEBUG, "usb_cable_state = %s -> %s",
            value_old, value_new);

    common_dbus_send_usb_cable_state(0);

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * charger_type
 * ------------------------------------------------------------------------- */

/** Callback for handling charger_type_pipe state changes
 *
 * @param data charger_type (as void pointer)
 */
static void common_datapipe_charger_type_cb(gconstpointer data)
{
    charger_type_t prev = charger_type;
    charger_type = GPOINTER_TO_INT(data);

    if( charger_type == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "charger_type = %s -> %s",
            charger_type_repr(prev),
            charger_type_repr(charger_type));

    common_dbus_send_charger_type(0);

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * charger_state
 * ------------------------------------------------------------------------- */

/** Callback for handling charger_state_pipe state changes
 *
 * @param data charger_state (as void pointer)
 */
static void common_datapipe_charger_state_cb(gconstpointer data)
{
    charger_state_t prev = charger_state;
    charger_state = GPOINTER_TO_INT(data);

    if( charger_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "charger_state = %s -> %s",
            charger_state_repr(prev),
            charger_state_repr(charger_state));

    common_dbus_send_charger_state(0);

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * battery_status
 * ------------------------------------------------------------------------- */

/** Callback for handling battery_status_pipe state changes
 *
 * @param data battery_status (as void pointer)
 */
static void common_datapipe_battery_status_cb(gconstpointer data)
{
    battery_status_t prev = battery_status;
    battery_status = GPOINTER_TO_INT(data);

    if( battery_status == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "battery_status = %s -> %s",
            battery_status_repr(prev),
            battery_status_repr(battery_status));

    common_dbus_send_battery_status(0);

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * battery_level
 * ------------------------------------------------------------------------- */

/** Callback for handling battery_level_pipe state changes
 *
 * @param data battery_level (as void pointer)
 */
static void common_datapipe_battery_level_cb(gconstpointer data)
{
    gint prev = battery_level;
    battery_level = GPOINTER_TO_INT(data);

    if( battery_level == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "battery_level = %d -> %d",
            prev, battery_level);

    common_dbus_send_battery_level(0);

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * proximity_sensor
 * ------------------------------------------------------------------------- */

/** Change notifications for proximity_sensor_actual
 */
static void common_datapipe_proximity_sensor_actual_cb(gconstpointer data)
{
    cover_state_t prev = proximity_sensor_actual;
    proximity_sensor_actual = GPOINTER_TO_INT(data);

    if( proximity_sensor_actual == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "proximity_sensor_actual = %s -> %s",
            proximity_state_repr(prev),
            proximity_state_repr(proximity_sensor_actual));

    if( proximity_sensor_actual != COVER_UNDEF )
        common_on_proximity_exec();

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * init/quit
 * ------------------------------------------------------------------------- */

/** Array of datapipe handlers */
static datapipe_handler_t common_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &usb_cable_state_pipe,
        .output_cb = common_datapipe_usb_cable_state_cb,
    },
    {
        .datapipe  = &charger_type_pipe,
        .output_cb = common_datapipe_charger_type_cb,
    },
    {
        .datapipe  = &charger_state_pipe,
        .output_cb = common_datapipe_charger_state_cb,
    },
    {
        .datapipe  = &battery_status_pipe,
        .output_cb = common_datapipe_battery_status_cb,
    },
    {
        .datapipe  = &battery_level_pipe,
        .output_cb = common_datapipe_battery_level_cb,
    },
    {
        .datapipe  = &proximity_sensor_actual_pipe,
        .output_cb = common_datapipe_proximity_sensor_actual_cb,
    },
    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t common_datapipe_bindings =
{
    .module   = "common",
    .handlers = common_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void common_datapipe_init(void)
{
    mce_datapipe_init_bindings(&common_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void common_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&common_datapipe_bindings);
}

/* ========================================================================= *
 * MODULE_INIT_QUIT
 * ========================================================================= */

/** Initialize common functionality
 */
bool
mce_common_init(void)
{
    /* attach to internal state variables */
    common_datapipe_init();

    /* set up dbus message handlers */
    common_dbus_init();

    return true;
}

/** De-initialize common functionality
 */
void
mce_common_quit(void)
{
    /* remove all handlers */
    common_dbus_quit();
    common_datapipe_quit();
    common_on_proximity_quit();
}
