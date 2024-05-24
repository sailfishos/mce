/**
 * @file callstate.c
 * Call state module -- this handles the call state for MCE
 * <p>
 * Copyright (c) 2008 - 2009 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (c) 2012 - 2023 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Kalle Jokiniemi <kalle.jokiniemi@jolla.com>
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
#include "../mce-wltimer.h"

#include <stdlib.h>
#include <string.h>

#include <mce/dbus-names.h>

#include <gmodule.h>

#if 0 // DEBUG: make all logging from this module "critical"
# undef mce_log
# define mce_log(LEV, FMT, ARGS...) \
        mce_log_file(LL_CRIT, __FILE__, __FUNCTION__, FMT , ## ARGS)
#endif

/* ========================================================================= *
 * OFONO DBUS CONSTANTS
 * ========================================================================= */

#define OFONO_SERVICE                       "org.ofono"

#define OFONO_MANAGER_INTERFACE             "org.ofono.Manager"
#define OFONO_MANAGER_OBJECT                "/"
#define OFONO_MANAGER_REQ_GET_MODEMS        "GetModems"
#define OFONO_MANAGER_SIG_MODEM_ADDED       "ModemAdded"
#define OFONO_MANAGER_SIG_MODEM_REMOVED     "ModemRemoved"

#define OFONO_MODEM_INTERFACE               "org.ofono.Modem"
#define OFONO_MODEM_SIG_PROPERTY_CHANGED    "PropertyChanged"

#define OFONO_VCALLMANAGER_INTERFACE        "org.ofono.VoiceCallManager"
#define OFONO_VCALLMANAGER_REQ_GET_CALLS    "GetCalls"
#define OFONO_VCALLMANAGER_SIG_CALL_ADDED   "CallAdded"
#define OFONO_VCALLMANAGER_SIG_CALL_REMOVED "CallRemoved"

#define OFONO_VCALL_INTERFACE               "org.ofono.VoiceCall"
#define OFONO_VCALL_SIG_PROPERTY_CHANGED    "PropertyChanged"

/* ========================================================================= *
 * MODULE DETAILS
 * ========================================================================= */

/** Module name */
#define MODULE_NAME             "callstate"

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
        /** Name of the module */
        .name = MODULE_NAME,
        /** Module provides */
        .provides = provides,
        /** Module priority */
        .priority = 250
};

/* ========================================================================= *
 * MODULE DATA
 * ========================================================================= */

/** Maximum number of concurrent call state requesters */
#define CLIENTS_MONITOR_COUNT 15

/** List of monitored call state requesters */
static GSList *clients_monitor_list = NULL;

/** Lookup table for state data / each call state requester */
static GHashTable *clients_state_lut = 0;

static void call_state_rethink_schedule(void);
static bool call_state_rethink_forced(void);

static void xofono_get_vcalls(const char *modem);

/* ========================================================================= *
 * OFONO CALL STATE HELPERS
 * ========================================================================= */

/** Enumeration of oFono voice call states */
typedef enum
{
    OFONO_CALL_STATE_UNKNOWN,
    OFONO_CALL_STATE_ACTIVE,
    OFONO_CALL_STATE_HELD,
    OFONO_CALL_STATE_DIALING,
    OFONO_CALL_STATE_ALERTING,
    OFONO_CALL_STATE_INCOMING,
    OFONO_CALL_STATE_WAITING,
    OFONO_CALL_STATE_DISCONNECTED,

    OFONO_CALL_STATE_COUNT
} ofono_callstate_t;

/** Lookup table for oFono voice call states */
static const char * const ofono_callstate_lut[OFONO_CALL_STATE_COUNT] =
{
    [OFONO_CALL_STATE_UNKNOWN]      = "unknown",
    [OFONO_CALL_STATE_ACTIVE]       = "active",
    [OFONO_CALL_STATE_HELD]         = "held",
    [OFONO_CALL_STATE_DIALING]      = "dialing",
    [OFONO_CALL_STATE_ALERTING]     = "alerting",
    [OFONO_CALL_STATE_INCOMING]     = "incoming",
    [OFONO_CALL_STATE_WAITING]      = "waiting",
    [OFONO_CALL_STATE_DISCONNECTED] = "disconnected",
};

/** oFono call state number to name
 */
#ifdef DEAD_CODE
static const char *
ofono_callstate_name(ofono_callstate_t callstate)
{
    if( callstate < OFONO_CALL_STATE_COUNT )
        return ofono_callstate_lut[callstate];
    return ofono_callstate_lut[OFONO_CALL_STATE_UNKNOWN];
}
#endif

/** oFono call state name to number
 */
static ofono_callstate_t
ofono_callstate_lookup(const char *name)
{
    ofono_callstate_t res = OFONO_CALL_STATE_UNKNOWN;

    for( int i = 0; i < OFONO_CALL_STATE_COUNT; ++i ) {
        if( !strcmp(ofono_callstate_lut[i], name) ) {
            res = i;
            break;
        }
    }

    return res;
}

/** oFono call state name to mce call state number
 */
static call_state_t
ofono_callstate_to_mce(const char *name)
{
    ofono_callstate_t ofono = ofono_callstate_lookup(name);
    call_state_t   mce = CALL_STATE_INVALID;

    switch( ofono ) {
    default:
    case OFONO_CALL_STATE_UNKNOWN:
    case OFONO_CALL_STATE_DISCONNECTED:
        mce = CALL_STATE_NONE;
        break;

    case OFONO_CALL_STATE_INCOMING:
    case OFONO_CALL_STATE_WAITING:
        mce = CALL_STATE_RINGING;
        break;

    case OFONO_CALL_STATE_DIALING:
    case OFONO_CALL_STATE_ALERTING:
    case OFONO_CALL_STATE_ACTIVE:
    case OFONO_CALL_STATE_HELD:
        mce = CALL_STATE_ACTIVE;
        break;
    }

    return mce;
}

/** oFono emergency flag to mce call type number
 */
static call_type_t ofono_calltype_to_mce(bool emergency)
{
    return emergency ? CALL_TYPE_EMERGENCY : CALL_TYPE_NORMAL;
}

/* ========================================================================= *
 * OFONO VOICECALL OBJECTS
 * ========================================================================= */

/** oFono voice call state data
 */
typedef struct
{
    char *name;
    bool probed;

    call_state_t state;
    call_type_t  type;
} ofono_vcall_t;

static void clients_merge_state_cb(gpointer key, gpointer val, gpointer aptr);
static void clients_merge_state   (ofono_vcall_t *combined);
static void clients_set_state     (const char *dbus_name, const ofono_vcall_t *vcall);
static void clients_get_state     (const char *dbus_name, ofono_vcall_t *vcall);
static void clients_init          (void);
static void clients_quit          (void);

/** Mark incoming vcall as ignored
 *
 * @param self      oFono voice call object
 */
static void
ofono_vcall_ignore_incoming_call(ofono_vcall_t *self)
{
    if( self->state == CALL_STATE_RINGING ) {
        mce_log(LL_DEBUG, "ignoring incoming vcall: %s",
                self->name ?: "unnamed");
        self->state = CALL_STATE_IGNORED;
    }
}

/** Merge emergency data to oFono voice call object
 *
 * @param self      oFono voice call object
 * @param emergency emergency state to merge
 */
static void
ofono_vcall_merge_emergency(ofono_vcall_t *self, bool emergency)
{
    if( emergency )
        self->type = CALL_TYPE_EMERGENCY;
}

/** Merge state data from oFono voice call object to another
 *
 * @param self      oFono voice call object
 * @param emergency emergency state to merge
 */
static void
ofono_vcall_merge_vcall(ofono_vcall_t *self, const ofono_vcall_t *that)
{
    /* When evaluating combined call state, we must
     * give "ringing" state priority over "active"
     * so that display and suspend policy works in
     * expected manner. */
    switch( that->state ) {
    case CALL_STATE_ACTIVE:
        if( self->state != CALL_STATE_RINGING )
            self->state = CALL_STATE_ACTIVE;
        break;

    case CALL_STATE_RINGING:
        self->state = CALL_STATE_RINGING;
        break;

    default:
        break;
    }

    /* if any call is emergency, we have emergency call */
    if( that->type == CALL_TYPE_EMERGENCY )
        self->type = CALL_TYPE_EMERGENCY;
}

/** Create oFono voice call object
 *
 * @param name D-Bus object path
 *
 * @return oFono voice call object
 */
static ofono_vcall_t *
ofono_vcall_create(const char *path)
{
    ofono_vcall_t *self = calloc(1, sizeof *self);

    self->name   = g_strdup(path);
    self->probed = false;
    self->state  = CALL_STATE_INVALID;
    self->type   = CALL_TYPE_NORMAL;

    mce_log(LL_DEBUG, "vcall=%s", self->name);
    return self;
}

/** Delete oFono voice call object
 *
 * @param self oFono voice call object
 */
static void
ofono_vcall_delete(ofono_vcall_t *self)
{
    if( self ) {
        mce_log(LL_DEBUG, "vcall=%s", self->name);
        g_free(self->name);
        free(self);
    }
}

/** Type agnostic callback function for deleting oFono voice call objects
 *
 * @param self oFono voice call object
 */
static void
ofono_vcall_delete_cb(gpointer self)
{
    ofono_vcall_delete(self);
}

/** Update oFono voice call object from key string and variant
 *
 * @param self oFono voice call object
 */
static void
ofono_vcall_update_1(ofono_vcall_t *self, DBusMessageIter *iter)
{
    const char *key = 0;
    DBusMessageIter var;

    if( !mce_dbus_iter_get_string(iter, &key) )
        goto EXIT;

    if( !mce_dbus_iter_get_variant(iter, &var) )
        goto EXIT;

    if( !strcmp(key, "Emergency") ) {
        bool emergency = false;
        if( !mce_dbus_iter_get_bool(&var, &emergency) )
            goto EXIT;
        self->type = ofono_calltype_to_mce(emergency);

        mce_log(LL_DEBUG, "* %s = ofono:%s -> mce:%s", key,
                emergency ? "true" : "false",
                call_type_repr(self->type));
    }
    else if( !strcmp(key, "State") ) {
        const char *str = 0;
        if( !mce_dbus_iter_get_string(&var, &str) )
            goto EXIT;
        self->state = ofono_callstate_to_mce(str);
        mce_log(LL_DEBUG, "* %s = ofono:%s -> mce:%s", key,str,
                call_state_repr(self->state));
    }
#if 0
    else {
        mce_log(LL_DEBUG, "* %s = %s", key, "...");
    }
#endif

EXIT:
    return;
}

/** Update oFono voice call object from array of dict entries
 *
 * @param self oFono voice call object
 */
static void
ofono_vcall_update_N(ofono_vcall_t *self, DBusMessageIter *iter)
{
    DBusMessageIter arr2, dict;

    // <arg name="properties" type="a{sv}"/>

    self->probed = true;

    if( !mce_dbus_iter_get_array(iter, &arr2) )
        goto EXIT;

    while( !mce_dbus_iter_at_end(&arr2) ) {
        if( !mce_dbus_iter_get_entry(&arr2, &dict) )
            goto EXIT;

        ofono_vcall_update_1(self, &dict);
    }
EXIT:
    return;
}

/* ========================================================================= *
 * MANAGE VOICE CALL OBJECTS
 * ========================================================================= */

/** Lookup table for tracked voice call objects */
static GHashTable *vcalls_lut = 0;

/** Lookup a voice call objects by name
 *
 * @param name D-Bus object path
 *
 * @return object pointer, or NULL if not found
 */
static ofono_vcall_t *
vcalls_get_call(const char *name)
{
    ofono_vcall_t *self = 0;

    if( !vcalls_lut )
        goto EXIT;

    self = g_hash_table_lookup(vcalls_lut, name);

EXIT:
    return self;
}

/** Lookup or create a voice call objects by name
 *
 * @param name D-Bus object path
 *
 * @return object pointer, or NULL on errors
 */
static ofono_vcall_t *
vcalls_add_call(const char *name)
{
    ofono_vcall_t *self = 0;

    if( !vcalls_lut )
        goto EXIT;

    if( !(self = g_hash_table_lookup(vcalls_lut, name)) ) {
        self = ofono_vcall_create(name);
        g_hash_table_replace(vcalls_lut, g_strdup(name), self);
    }

EXIT:
    return self;
}

/** Remove a voice call objects by name
 *
 * @param name D-Bus object path
 */
static void
vcalls_rem_call(const char *name)
{
    if( !vcalls_lut )
        goto EXIT;

    g_hash_table_remove(vcalls_lut, name);

EXIT:
    return;
}

/** Remove all tracked voice call objects
 *
 * @param name D-Bus object path
 */
static void
vcalls_rem_calls(void)
{
    if( vcalls_lut )
        g_hash_table_remove_all(vcalls_lut);
}

/** Initialize voice call object look up table */
static void
vcalls_init(void)
{
    if( !vcalls_lut )
        vcalls_lut = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, ofono_vcall_delete_cb);
}

/** Delete voice call object look up table */
static void
vcalls_quit(void)
{
    if( vcalls_lut )
        g_hash_table_unref(vcalls_lut), vcalls_lut = 0;
}

/* ========================================================================= *
 * MODEM OBJECTS
 * ========================================================================= */

/** oFono modem tracking data
 */
typedef struct
{
    /** D-Bus object path for the modem */
    char *name;

    /** Flag for: Properties for this modem have been processed */
    bool probed;

    /** Flag for: The Emergency call property for the modem is set */
    bool emergency;

    /** Flag for: org.ofono.VoiceCallManager inteface is available */
    bool vcalls_iface;

    /** Flag for: async dbus query to get vcalls for this modem is made */
    bool vcalls_probed;

} ofono_modem_t;

/** Create oFono modem tracking object
 *
 * @param name D-Bus object path
 *
 * @return object pointer
 */
static ofono_modem_t *
ofono_modem_create(const char *path)
{
    ofono_modem_t *self = calloc(1, sizeof *self);

    self->name      = g_strdup(path);
    self->probed    = false;
    self->emergency = false;

    self->vcalls_iface  = false;
    self->vcalls_probed = false;

    mce_log(LL_DEBUG, "modem=%s", self->name);
    return self;
}

/** Delete oFono modem tracking object
 *
 * @param self object pointer, or NULL
 */
static void
ofono_modem_delete(ofono_modem_t *self)
{
    if( self ) {
        mce_log(LL_DEBUG, "modem=%s", self->name);
        g_free(self->name);
        free(self);
    }
}

/** Type agnostic delete callback function for  oFono modem tracking objects
 *
 * @param self object pointer, or NULL
 */
static void
ofono_modem_delete_cb(gpointer self)
{
    ofono_modem_delete(self);
}

/** Update oFono modem tracking object from key + variant data
 *
 * @param self object pointer
 * @param iter dbus message iterator pointing to key string + variant data
 */
static void
ofono_modem_update_1(ofono_modem_t *self, DBusMessageIter *iter)
{
    const char *key = 0;
    DBusMessageIter var;

    if( !mce_dbus_iter_get_string(iter, &key) )
        goto EXIT;

    if( !mce_dbus_iter_get_variant(iter, &var) )
        goto EXIT;

    if( !strcmp(key, "Emergency") ) {
        if( !mce_dbus_iter_get_bool(&var, &self->emergency) )
            goto EXIT;
        mce_log(LL_DEBUG, "* %s = %s", key,
                self->emergency ? "true" : "false");
    }
    else if( !strcmp(key, "Interfaces") ) {
        DBusMessageIter arr;

        if( !mce_dbus_iter_get_array(&var, &arr) )
            goto EXIT;

        bool vcalls_iface = false;

        while( !mce_dbus_iter_at_end(&arr) ) {
            const char *iface = 0;

            if( !mce_dbus_iter_get_string(&arr, &iface) )
                goto EXIT;

            if( strcmp(iface, OFONO_VCALLMANAGER_INTERFACE) )
                continue;

            vcalls_iface = true;
            break;
        }

        if( self->vcalls_iface != vcalls_iface ) {
            self->vcalls_iface  = vcalls_iface;
            self->vcalls_probed = false;

            mce_log(LL_NOTICE, "%s interface %savailable",
                   OFONO_VCALLMANAGER_INTERFACE,
                    self->vcalls_iface ? "" : "not ");

        }
    }
#if 0
    else {
        mce_log(LL_DEBUG, "* %s = %s", key, "...");
    }
#endif
EXIT:
    return;
}

/** Update oFono modem tracking object from array of dict entries
 *
 * @param self object pointer
 * @param iter dbus message iterator pointing to array of dict entries
 */
static void
ofono_modem_update_N(ofono_modem_t *self, DBusMessageIter *iter)
{
    DBusMessageIter arr2, dict;

    self->probed = true;

    // <arg name="properties" type="a{sv}"/>

    if( !mce_dbus_iter_get_array(iter, &arr2) )
        goto EXIT;

    while( !mce_dbus_iter_at_end(&arr2) ) {
        if( !mce_dbus_iter_get_entry(&arr2, &dict) )
            goto EXIT;
        ofono_modem_update_1(self, &dict);
    }
EXIT:
    return;
}

/** Get voice calls for a modem
 *
 * If org.ofono.VoiceCallManager D-Bus interface is available for use,
 * enumerate voice call objects for the modem to get initial properties
 * of the calls.
 *
 * @param self object pointer
 */
static void
ofono_modem_get_vcalls(ofono_modem_t *self)
{
    /* Interface available? */
    if( !self->vcalls_iface )
        goto EXIT;

    /* Already done? */
    if( self->vcalls_probed )
        goto EXIT;

    /* Mark as done */
    self->vcalls_probed = true;

    /* Start async D-Bus query */
    xofono_get_vcalls(self->name);

EXIT:
    return;
}

/* ========================================================================= *
 * MODEMS
 * ========================================================================= */

/** Lookup table for tracked oFono modem objects */
static GHashTable *modems_lut = 0;

/** Lookup modem object by name
 *
 * @param name D-Bus object path
 *
 * @return object pointer, or NULL
 */
static ofono_modem_t *
modems_get_modem(const char *name)
{
    ofono_modem_t *self = 0;

    if( !modems_lut )
        goto EXIT;

    self = g_hash_table_lookup(modems_lut, name);

EXIT:
    return self;
}

/** Lookup existing / insert new modem object by name
 *
 * @param name D-Bus object path
 *
 * @return object pointer, or NULL
 */
static ofono_modem_t *
modems_add_modem(const char *name)
{
    ofono_modem_t *self = 0;

    if( !modems_lut )
        goto EXIT;

    if( !(self = g_hash_table_lookup(modems_lut, name)) ) {
        self = ofono_modem_create(name);
        g_hash_table_replace(modems_lut, g_strdup(name), self);
    }

EXIT:
    return self;
}

/** Remove modem object by name
 *
 * @param name D-Bus object path
 */
static void
modems_rem_modem(const char *name)
{
    if( !modems_lut )
        goto EXIT;

    g_hash_table_remove(modems_lut, name);

EXIT:
    return;
}

/** Remove all tracked modem objects
 */
static void
modems_rem_all_modems(void)
{
    if( modems_lut )
        g_hash_table_remove_all(modems_lut);
}

/** Initialize lookup table for oFono modem tracking objects
 */
static void
modems_init(void)
{
    if( !modems_lut )
        modems_lut = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, ofono_modem_delete_cb);
}

/** Delete lookup table for oFono modem tracking objects
 */
static void
modems_quit(void)
{
    if( modems_lut )
        g_hash_table_unref(modems_lut), modems_lut = 0;
}

/* ========================================================================= *
 * OFONO DBUS GLUE
 * ========================================================================= */

/** Handle reply to voice calls query
 *
 * @param pc   pending call object
 * @param aptr (not used)
 */
static void
xofono_get_vcalls_cb(DBusPendingCall *pc, void *aptr)
{
    (void) aptr;

    DBusMessage *rsp = 0;
    DBusError    err = DBUS_ERROR_INIT;
    int          cnt = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
        mce_log(LL_ERR, "%s: no reply",
                OFONO_VCALLMANAGER_REQ_GET_CALLS);
        goto EXIT;
    }

    if( dbus_set_error_from_message(&err, rsp) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    // <arg name="calls_with_properties" type="a(oa{sv})" direction="out"/>

    DBusMessageIter body, arr1, mod;
    dbus_message_iter_init(rsp, &body);
    if( !mce_dbus_iter_get_array(&body, &arr1) )
        goto EXIT;

    while( !mce_dbus_iter_at_end(&arr1) ) {
        const char *name = 0;

        if( !mce_dbus_iter_get_struct(&arr1, &mod) )
            goto EXIT;
        if( !mce_dbus_iter_get_object(&mod, &name) )
            goto EXIT;

        ofono_vcall_t *vcall = vcalls_add_call(name);
        if( !vcall )
            continue;
        ofono_vcall_update_N(vcall, &mod);
        ++cnt;
    }
    call_state_rethink_schedule();

EXIT:
    mce_log(LL_DEBUG, "added %d calls", cnt);
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
    return;
}

/** Get voice calls associated with a modem
 *
 * Populate voice call lookup table with the reply data
 *
 * @param modem D-Bus object path
 */
static void
xofono_get_vcalls(const char *modem)
{
    dbus_send_ex(OFONO_SERVICE,
                 modem,
                 OFONO_VCALLMANAGER_INTERFACE,
                 OFONO_VCALLMANAGER_REQ_GET_CALLS,
                 xofono_get_vcalls_cb,
                 0,0,0,
                 DBUS_TYPE_INVALID);
}

/** Handle voice call changed signal
 *
 * Update voice call lookup table with the content
 *
 * @param msg property changed signal
 */
static gboolean
xofono_vcall_changed_cb(DBusMessage *const msg)
{
    (void)msg;

    DBusMessageIter body;
    dbus_message_iter_init(msg, &body);

    const char *name = dbus_message_get_path(msg);
    if( !name )
        goto EXIT;

    ofono_vcall_t *vcall = vcalls_get_call(name);
    if( !vcall )
        goto EXIT;

    ofono_vcall_update_1(vcall, &body);
    call_state_rethink_schedule();

EXIT:
    return TRUE;
}

/** Handle voice call added signal
 *
 * Update voice call lookup table with the content
 *
 * @param msg call added signal
 */
static gboolean
xofono_vcall_added_cb(DBusMessage *const msg)
{
    (void)msg;

    DBusMessageIter body;
    dbus_message_iter_init(msg, &body);

    const char *name = 0;

    if( !mce_dbus_iter_get_object(&body, &name) )
        goto EXIT;

    ofono_vcall_t *vcall = vcalls_add_call(name);
    if( vcall )
        ofono_vcall_update_N(vcall, &body);

    call_state_rethink_schedule();

EXIT:
    return TRUE;
}

/** Handle voice call removed signal
 *
 * Update voice call lookup table with the content
 *
 * @param msg call removed signal
 */
static gboolean
xofono_vcall_removed_cb(DBusMessage *const msg)
{
    (void)msg;

    DBusMessageIter body;
    dbus_message_iter_init(msg, &body);

    const char *name = 0;

    if( !mce_dbus_iter_get_object(&body, &name) )
        goto EXIT;

    vcalls_rem_call(name);
    call_state_rethink_schedule();

EXIT:
    return TRUE;
}

/** Handle reply to xofono_get_modems()
 *
 * @param pc   pending call object
 * @param aptr (not used)
 */
static void
xofono_get_modems_cb(DBusPendingCall *pc, void *aptr)
{
    (void)aptr;

    mce_log(LL_DEBUG, "%s.%s %s", OFONO_MANAGER_INTERFACE,
            OFONO_MANAGER_REQ_GET_MODEMS, "reply");

    DBusMessage *rsp = 0;
    DBusError    err = DBUS_ERROR_INIT;
    int          cnt = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) )
        goto EXIT;

    if( dbus_set_error_from_message(&err, rsp) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    // <arg name="modems" type="a(oa{sv})" direction="out"/>

    DBusMessageIter body, arr1, mod;
    dbus_message_iter_init(rsp, &body);
    if( !mce_dbus_iter_get_array(&body, &arr1) )
        goto EXIT;

    while( !mce_dbus_iter_at_end(&arr1) ) {
        const char *name = 0;

        if( !mce_dbus_iter_get_struct(&arr1, &mod) )
            goto EXIT;
        if( !mce_dbus_iter_get_object(&mod, &name) )
            goto EXIT;

        ofono_modem_t *modem = modems_add_modem(name);
        if( !modem )
            continue;

        ofono_modem_update_N(modem, &mod);
        ofono_modem_get_vcalls(modem);
        ++cnt;
    }
    call_state_rethink_schedule();

EXIT:
    mce_log(LL_DEBUG, "added %d modems", cnt);
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

/** Get list of modems [async]
 *
 * Populate modem lookup table with the reply data
 */
static void
xofono_get_modems(void)
{
    bool res = dbus_send(OFONO_SERVICE,
                         OFONO_MANAGER_OBJECT,
                         OFONO_MANAGER_INTERFACE,
                         OFONO_MANAGER_REQ_GET_MODEMS,
                         xofono_get_modems_cb,
                         DBUS_TYPE_INVALID);

    mce_log(LL_DEBUG, "%s.%s %s", OFONO_MANAGER_INTERFACE,
            OFONO_MANAGER_REQ_GET_MODEMS, res ? "sent ..." : "failed");
}

/** Handle modem changed signal
 *
 * Update modem lookup table with the content
 *
 * @param msg property changed signal
 */
static gboolean
xofono_modem_changed_cb(DBusMessage *const msg)
{
    DBusMessageIter body;
    dbus_message_iter_init(msg, &body);

    const char *name = dbus_message_get_path(msg);
    if( !name )
        goto EXIT;

    mce_log(LL_NOTICE, "modem=%s", name);

    ofono_modem_t *modem = modems_get_modem(name);
    if( modem ) {
        ofono_modem_update_1(modem, &body);
        ofono_modem_get_vcalls(modem);
    }
    call_state_rethink_schedule();

EXIT:
    return TRUE;
}

/** Handle modem added signal
 *
 * Update modem lookup table with the content
 *
 * @param msg modem added signal
 */
static gboolean
xofono_modem_added_cb(DBusMessage *const msg)
{
    DBusMessageIter body;
    dbus_message_iter_init(msg, &body);

    const char *name = 0;

    if( !mce_dbus_iter_get_object(&body, &name) )
        goto EXIT;

    mce_log(LL_NOTICE, "modem=%s", name);

    ofono_modem_t *modem = modems_add_modem(name);
    if( modem ) {
        ofono_modem_update_N(modem, &body);
        ofono_modem_get_vcalls(modem);
    }
    call_state_rethink_schedule();

EXIT:
    return TRUE;
}

/** Handle modem removed signal
 *
 * Update modem lookup table with the content
 *
 * @param msg modem removed signal
 */
static gboolean
xofono_modem_removed_cb(DBusMessage *const msg)
{
    DBusMessageIter body;
    dbus_message_iter_init(msg, &body);

    const char *name = 0;

    if( !mce_dbus_iter_get_object(&body, &name) )
        goto EXIT;

    mce_log(LL_NOTICE, "modem=%s", name);
    modems_rem_modem(name);
    call_state_rethink_schedule();

EXIT:
    return TRUE;
}

/* ========================================================================= *
 * OFONO TRACKING
 * ========================================================================= */

/** Flag for "org.ofono" D-Bus name has owner */
static bool xofono_is_available = false;

/** Handle "org.ofono" D-Bus name owner changes
 *
 * Flush tracked modems and voice calls when name owner changes.
 *
 * Re-enumerate modems and calls when there is a new owner.
 *
 * @param available true if ofono name has an owner, false if not
 */
static void
xofono_availability_set(bool available)
{
    if( xofono_is_available != available ) {

        mce_log(LL_DEBUG, "%s is %savailable", OFONO_SERVICE,
                available ? "" : "not ");

        vcalls_rem_calls();
        modems_rem_all_modems();

        call_state_rethink_schedule();

        if( (xofono_is_available = available) ) {
            /* start enumerating modems (async) */
            xofono_get_modems();
        }
    }
}

/** Handle D-Bus name owner changed signals for "org.ofono"
 */
static gboolean
xofono_name_owner_changed_cb(DBusMessage *rsp)
{
    const gchar *name = 0;
    const gchar *prev = 0;
    const gchar *curr = 0;
    DBusError    err  = DBUS_ERROR_INIT;

    if( dbus_set_error_from_message(&err, rsp) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    if( !dbus_message_get_args(rsp, &err,
                              DBUS_TYPE_STRING, &name,
                              DBUS_TYPE_STRING, &prev,
                              DBUS_TYPE_STRING, &curr,
                              DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    if( !name || strcmp(name, OFONO_SERVICE) )
        goto EXIT;

    xofono_availability_set(curr && *curr != 0 );

EXIT:
    dbus_error_free(&err);
    return TRUE;
}

/** Handle reply to asynchronous ofono service name ownership query
 *
 * @param pc        State data for asynchronous D-Bus method call
 * @param user_data (not used)
 */
static void
xofono_name_owner_get_cb(DBusPendingCall *pc, void *user_data)
{
    (void)user_data;

    DBusMessage *rsp   = 0;
    const char  *owner = 0;
    DBusError    err   = DBUS_ERROR_INIT;

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

    xofono_availability_set(owner && *owner);

EXIT:
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

/** Initiate asynchronous ofono service name ownership query
 *
 * @return TRUE if the method call was initiated, or FALSE in case of errors
 */
static gboolean
xofono_name_owner_get(void)
{
    gboolean         res  = FALSE;
    const char      *name = OFONO_SERVICE;

    res = dbus_send(DBUS_SERVICE_DBUS,
                    DBUS_PATH_DBUS,
                    DBUS_INTERFACE_DBUS,
                    "GetNameOwner",
                    xofono_name_owner_get_cb,
                    DBUS_TYPE_STRING, &name,
                    DBUS_TYPE_INVALID);
    return res;
}

/* ========================================================================= *
 * SIMULATED CALL STATE (for debugging purposes)
 * ========================================================================= */

/** Dummy vcall data used for clients that are not tracked */
static const ofono_vcall_t clients_vcall_def =
{
    .state = CALL_STATE_NONE,
    .type  = CALL_TYPE_NORMAL,
};

/** Enumeration callback for ignoring incoming calls
 *
 * @param key   D-Bus name of the client (as void pointer)
 * @param val   ofono_vcall_t data of the client (as void pointer)
 * @param aptr  NULL (unused)
 */
static void
clients_ignore_incoming_calls_cb(gpointer key, gpointer val, gpointer aptr)
{
    (void)key;
    (void)aptr;

    ofono_vcall_t *simulated = val;

    ofono_vcall_ignore_incoming_call(simulated);
}

/** Mark all incoming calls as ignored
 *
 * @param combined  ofono_vcall_t data to update
 */
static void
clients_ignore_incoming_calls(void)
{
    if( !clients_state_lut )
        goto EXIT;

    g_hash_table_foreach(clients_state_lut, clients_ignore_incoming_calls_cb, 0);

EXIT:
    return;
}

/** Enumeration callback for evaluating combined dbus client state
 *
 * @param key   D-Bus name of the client (as void pointer)
 * @param val   ofono_vcall_t data of the client (as void pointer)
 * @param aptr  ofono_vcall_t data to update (as void pointer)
 */
static void
clients_merge_state_cb(gpointer key, gpointer val, gpointer aptr)
{
    (void)key;

    ofono_vcall_t *combined  = aptr;
    ofono_vcall_t *simulated = val;

    ofono_vcall_merge_vcall(combined, simulated);
}

/** Update overall call state by inspecting all active dbus client states
 *
 * @param combined  ofono_vcall_t data to update
 */
static void
clients_merge_state(ofono_vcall_t *combined)
{
    if( !clients_state_lut )
        goto EXIT;

    g_hash_table_foreach(clients_state_lut, clients_merge_state_cb, combined);

EXIT:
    return;
}

/** Set state of one dbus client
 *
 * @ dbus_name  D-Bus name of the client
 * @ vcall      call state for the client, or NULL to remove client data
 */
static void
clients_set_state(const char *dbus_name, const ofono_vcall_t *vcall)
{
    if( !clients_state_lut || !dbus_name )
        goto EXIT;

    if( vcall == 0 || vcall->state == CALL_STATE_NONE ) {
        g_hash_table_remove(clients_state_lut, dbus_name);
        goto EXIT;
    }

    ofono_vcall_t *cached = g_hash_table_lookup(clients_state_lut, dbus_name);
    if( !cached ) {
        cached = g_malloc0(sizeof *cached);
        g_hash_table_replace(clients_state_lut, g_strdup(dbus_name), cached);
    }

    *cached = *vcall;

EXIT:
    return;
}

/** Get state of one dbus client
 *
 * Note: Untracked clients are assumed to be in none:normal call state.
 *
 * @ dbus_name  D-Bus name of the client
 * @ vcall      call state to fill in
 */
static void
clients_get_state(const char *dbus_name, ofono_vcall_t *vcall)
{
    ofono_vcall_t *cached = 0;

    if( clients_state_lut && dbus_name )
        cached = g_hash_table_lookup(clients_state_lut, dbus_name);

    *vcall = cached ? *cached : clients_vcall_def;
}

/** Initialize dbus client tracking */
static void clients_init(void)
{
    if( !clients_state_lut ) {
        clients_state_lut = g_hash_table_new_full(g_str_hash,
                                                  g_str_equal,
                                                  g_free, g_free);
    }
}

/** Stop dbus client tracking */

static void clients_quit(void)
{
    /* Remove name owner monitors */
    mce_dbus_owner_monitor_remove_all(&clients_monitor_list);

    /* Flush client state data */
    if( clients_state_lut ) {
        g_hash_table_unref(clients_state_lut),
            clients_state_lut = 0;
    }
}

/**
 * Send the call state and type
 *
 * @param method_call A DBusMessage to reply to;
 *                    pass NULL to send a signal instead
 * @param call_state A string representation of an alternate state
 *                   to send instead of the real call state
 * @param call_type A string representation of an alternate type
 *                  to send instead of the real call type
 * @return TRUE on success, FALSE on failure
 */
static gboolean
send_call_state(DBusMessage *const method_call,
                const gchar *const call_state,
                const gchar *const call_type)
{
        DBusMessage *msg = NULL;
        gboolean status = FALSE;
        const gchar *sstate;
        const gchar *stype;

        /* Allow spoofing */
        if (call_state != NULL)
                sstate = call_state;
        else
                sstate = call_state_to_dbus(datapipe_get_gint(call_state_pipe));

        if (call_type != NULL)
                stype = call_type;
        else
                stype = call_type_repr(datapipe_get_gint(call_type_pipe));

        /* If method_call is set, send a reply,
         * otherwise, send a signal
         */
        if (method_call != NULL) {
                msg = dbus_new_method_reply(method_call);
        } else {
                /* sig_call_state_ind */
                msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                                      MCE_CALL_STATE_SIG);
                mce_log(LL_DEVEL, "call state = %s / %s", sstate, stype);
        }

        /* Append the call state and call type */
        if( !dbus_message_append_args(msg,
                                      DBUS_TYPE_STRING, &sstate,
                                      DBUS_TYPE_STRING, &stype,
                                      DBUS_TYPE_INVALID) ) {
                mce_log(LL_ERR,
                        "Failed to append %sarguments to D-Bus message "
                        "for %s.%s",
                        method_call ? "reply " : "",
                        method_call ? MCE_REQUEST_IF :
                                      MCE_SIGNAL_IF,
                        method_call ? MCE_CALL_STATE_GET :
                                      MCE_CALL_STATE_SIG);
                goto EXIT;
        }

        /* Send the message if it is signal or wanted method reply */
        if( !method_call || !dbus_message_get_no_reply(method_call) )
                status = dbus_send_message(msg), msg = 0;

EXIT:
        if( msg )
                dbus_message_unref(msg);

        return status;
}

/**
 * D-Bus callback used for monitoring the process that requested
 * the call state; if that process exits, immediately
 * restore the call state to "none" and call type to "normal"
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean call_state_owner_monitor_dbus_cb(DBusMessage *const msg)
{
    DBusError   error     = DBUS_ERROR_INIT;
    const char *dbus_name = 0;
    const char *old_owner = 0;
    const char *new_owner = 0;

    if( !dbus_message_get_args(msg, &error,
                               DBUS_TYPE_STRING, &dbus_name,
                               DBUS_TYPE_STRING, &old_owner,
                               DBUS_TYPE_STRING, &new_owner,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to parse NameOwnerChanged: %s: %s",
                error.name, error.message);
        goto EXIT;
    }

    /* Remove the name monitor for the call state requester */
    if( mce_dbus_owner_monitor_remove(dbus_name,
                                      &clients_monitor_list) != -1 ) {
        clients_set_state(dbus_name, 0);
        call_state_rethink_schedule();
    }

EXIT:
    dbus_error_free(&error);
    return TRUE;
}

/**
 * D-Bus callback for the call state change request method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean
change_call_state_dbus_cb(DBusMessage *const msg)
{
    gboolean       status   = FALSE;
    const char    *state    = 0;
    const char    *type     = 0;
    const char    *sender   = dbus_message_get_sender(msg);
    DBusMessage   *reply    = NULL;
    DBusError      error    = DBUS_ERROR_INIT;
    dbus_bool_t    changed  = false;
    ofono_vcall_t  prev     = clients_vcall_def;
    ofono_vcall_t  curr     = clients_vcall_def;

    mce_log(LL_DEVEL, "Received set call state request from %s",
            mce_dbus_get_name_owner_ident(sender));

    clients_get_state(sender, &prev);

    if( !dbus_message_get_args(msg, &error,
                               DBUS_TYPE_STRING, &state,
                               DBUS_TYPE_STRING, &type,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to get argument from %s.%s: %s",
                MCE_REQUEST_IF, MCE_CALL_STATE_CHANGE_REQ,
                error.message);
        goto EXIT;
    }

    /* Convert call state to enum */
    curr.state = call_state_from_dbus(state);
    if( curr.state == CALL_STATE_INVALID ) {
        mce_log(LL_WARN, "Invalid call state received; request ignored");
        goto EXIT;
    }

    /* Convert call type to enum */
    curr.type = call_type_parse(type);
    if( curr.type == CALL_TYPE_INVALID ) {
        mce_log(LL_WARN, "Invalid call type received; request ignored");
        goto EXIT;
    }

    /* reject no-call emergency calls ... */
    if( curr.state == CALL_STATE_NONE )
        curr.type = CALL_TYPE_NORMAL;

    mce_log(LL_DEBUG, "Client call state changed: %s:%s -> %s:%s",
            call_state_repr(prev.state),
            call_type_repr(prev.type),
            call_state_repr(curr.state),
            call_type_repr(curr.type));

    if( curr.state != CALL_STATE_NONE &&
        mce_dbus_owner_monitor_add(sender, call_state_owner_monitor_dbus_cb,
                                   &clients_monitor_list,
                                   CLIENTS_MONITOR_COUNT) != -1 ) {
        clients_set_state(sender, &curr);
    }
    else {
        mce_dbus_owner_monitor_remove(sender, &clients_monitor_list);
        clients_set_state(sender, 0);
    }
    changed = call_state_rethink_forced();

EXIT:
    /* Setup the reply */
    reply = dbus_new_method_reply(msg);

    /* Append the result */
    if( !dbus_message_append_args(reply,
                                  DBUS_TYPE_BOOLEAN, &changed,
                                  DBUS_TYPE_INVALID)) {
        mce_log(LL_ERR,"Failed to append reply arguments to D-Bus "
                "message for %s.%s",
                MCE_REQUEST_IF, MCE_CALL_STATE_CHANGE_REQ);
    }
    else if( !dbus_message_get_no_reply(msg) ) {
        status = dbus_send_message(reply), reply = 0;
    }

    if( reply )
        dbus_message_unref(reply);

    dbus_error_free(&error);

    return status;
}

/**
 * D-Bus callback for the get call state method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean
get_call_state_dbus_cb(DBusMessage *const msg)
{
        gboolean status = FALSE;

        mce_log(LL_DEBUG, "Received call state get request");

        /* Try to send a reply that contains the current call state and type */
        if( !send_call_state(msg, NULL, NULL) )
                goto EXIT;

        status = TRUE;

EXIT:
        return status;
}

/* ========================================================================= *
 * MANAGE CALL STATE TRANSITIONS
 * ========================================================================= */

/** Callback for ignoring incoming voice call states
 */
static void
call_state_ignore_incoming_calls_cb(gpointer key, gpointer value, gpointer aptr)
{
    (void)key; //const char *name  = key;
    (void)aptr;
    ofono_vcall_t *vcall = value;

    ofono_vcall_ignore_incoming_call(vcall);
}

/** Internally ignore incoming calls
 */
static void
call_state_ignore_incoming_calls(void)
{
    /* Consider simulated call states */
    clients_ignore_incoming_calls();

    /* consider ofono voice call properties */
    if( vcalls_lut )
       g_hash_table_foreach(vcalls_lut, call_state_ignore_incoming_calls_cb, 0);
}

/** Callback for merging voice call stats
 */
static void
call_state_merge_vcall_cb(gpointer key, gpointer value, gpointer aptr)
{
    (void)key; //const char *name  = key;
    ofono_vcall_t *vcall = value;
    ofono_vcall_t *stats = aptr;

    ofono_vcall_merge_vcall(stats, vcall);
}

/** Callback for merging merging modem emergency data to voice call state
 */
static void
call_state_merge_modem_cb(gpointer key, gpointer value, gpointer aptr)
{
    (void)key; //const char *name  = key;
    ofono_modem_t *modem = value;
    ofono_vcall_t *stats = aptr;

    ofono_vcall_merge_emergency(stats, modem->emergency);
}

/** Evaluate mce call state
 *
 * Emit signals and update data pipes as needed
 *
 * @return true if call state / type changed, false otherwise
 */
static bool
call_state_rethink_now(void)
{
    bool         changed    = false;

    static ofono_vcall_t previous =
    {
        .state = CALL_STATE_INVALID,
        .type  = CALL_TYPE_NORMAL,
    };

    ofono_vcall_t combined =
    {
        .state = CALL_STATE_NONE,
        .type  = CALL_TYPE_NORMAL,
    };

    /* consider simulated call state */
    clients_merge_state(&combined);

    /* consider ofono modem emergency properties */
    if( modems_lut )
       g_hash_table_foreach(modems_lut, call_state_merge_modem_cb, &combined);

    /* consider ofono voice call properties */
    if( vcalls_lut )
       g_hash_table_foreach(vcalls_lut, call_state_merge_vcall_cb, &combined);

    /* skip broadcast if no change */
    if( !memcmp(&previous, &combined, sizeof combined) )
        goto EXIT;

    changed = true;
    previous = combined;

    call_state_t call_state = combined.state;
    call_type_t  call_type  = combined.type;
    const char   *state_str = call_state_repr(call_state);
    const char   *type_str  = call_type_repr(call_type);

    mce_log(LL_DEBUG, "call_state=%s, call_type=%s", state_str, type_str);

    /* If the state changed, signal the new state;
     * first externally, then internally
     *
     * The reason we do it externally first is to
     * make sure that the camera application doesn't
     * grab audio, otherwise the ring tone might go missing
     */

    // TODO: is the above legacy statement still valid?

    send_call_state(NULL, state_str, type_str);

    datapipe_exec_full(&call_state_pipe,
                       GINT_TO_POINTER(call_state));

    datapipe_exec_full(&call_type_pipe,
                       GINT_TO_POINTER(call_type));

EXIT:
    return changed;
}

/** Idle timer for evaluating call state */
static mce_wltimer_t *call_state_rethink_tmr = 0;

/** Timer callback for evaluating call state */
static gboolean
call_state_rethink_cb(gpointer aptr)
{
    (void)aptr;

    call_state_rethink_now();

    return G_SOURCE_REMOVE;
}

/** Cancel delayed call state evaluation */
static void
call_state_rethink_cancel(void)
{
    mce_wltimer_stop(call_state_rethink_tmr);
}

/** Request delayed call state evaluation */
static void
call_state_rethink_schedule(void)
{
    mce_wltimer_start(call_state_rethink_tmr);
}

/** Request immediate call state evaluation */
static bool
call_state_rethink_forced(void)
{
    call_state_rethink_cancel();
    return call_state_rethink_now();
}

/* ========================================================================= *
 * D-BUS CALLBACKS
 * ========================================================================= */

/** Array of dbus message handlers */
static mce_dbus_handler_t callstate_dbus_handlers[] =
{
    /* signals - outbound (for Introspect purposes only) */
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_CALL_STATE_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"call_state\" type=\"s\"/>\n"
            "    <arg name=\"call_type\" type=\"s\"/>\n"
    },
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_CALL_STATE_CHANGE_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = change_call_state_dbus_cb,
        .args      =
            "    <arg direction=\"in\" name=\"call_state\" type=\"s\"/>\n"
            "    <arg direction=\"in\" name=\"call_type\" type=\"s\"/>\n"
            "    <arg direction=\"out\" name=\"accepted\" type=\"b\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_CALL_STATE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = get_call_state_dbus_cb,
        .args      =
            "    <arg direction=\"out\" name=\"call_state\" type=\"s\"/>\n"
            "    <arg direction=\"out\" name=\"call_type\" type=\"s\"/>\n"
    },
    /* signals */
    {
        .interface = OFONO_MANAGER_INTERFACE,
        .name      = OFONO_MANAGER_SIG_MODEM_ADDED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xofono_modem_added_cb,
    },
    {
        .interface = OFONO_MANAGER_INTERFACE,
        .name      = OFONO_MANAGER_SIG_MODEM_REMOVED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xofono_modem_removed_cb,
    },
    {
        .interface = OFONO_MODEM_INTERFACE,
        .name      = OFONO_MODEM_SIG_PROPERTY_CHANGED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xofono_modem_changed_cb,
    },
    {
        .interface = OFONO_VCALLMANAGER_INTERFACE,
        .name      = OFONO_VCALLMANAGER_SIG_CALL_ADDED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xofono_vcall_added_cb,
    },
    {
        .interface = OFONO_VCALLMANAGER_INTERFACE,
        .name      = OFONO_VCALLMANAGER_SIG_CALL_REMOVED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xofono_vcall_removed_cb,
    },
    {
        .interface = OFONO_VCALL_INTERFACE,
        .name      = OFONO_VCALL_SIG_PROPERTY_CHANGED,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xofono_vcall_changed_cb,
    },
    {
        .interface = DBUS_INTERFACE_DBUS,
        .name      = "NameOwnerChanged",
        .rules     = "arg0='"OFONO_SERVICE"'",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xofono_name_owner_changed_cb,
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Add dbus handlers
 */
static void mce_callstate_init_dbus(void)
{
    mce_dbus_handler_register_array(callstate_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mce_callstate_quit_dbus(void)
{
    mce_dbus_handler_unregister_array(callstate_dbus_handlers);
}

/* ========================================================================= *
 * DATAPIPE CALLBACKS
 * ========================================================================= */

/** Handle call state change notifications
 *
 * @param data call state (as void pointer)
 */
static void
callstate_datapipe_ignore_incoming_call_event_cb(gconstpointer data)
{
    bool ignore_incoming_call = GPOINTER_TO_INT(data);

    mce_log(LL_DEBUG, "ignore_incoming_call = %s",
            ignore_incoming_call ? "YES" : "NO");

    // Note: Edge triggered
    if( !ignore_incoming_call )
        goto EXIT;

    call_state_ignore_incoming_calls();
    call_state_rethink_now();

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t callstate_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &ignore_incoming_call_event_pipe,
        .output_cb = callstate_datapipe_ignore_incoming_call_event_cb,
    },
    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t callstate_datapipe_bindings =
{
    .module   = "callstate",
    .handlers = callstate_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void
callstate_datapipes_init(void)
{
    mce_datapipe_init_bindings(&callstate_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void
callstate_datapipes_quit(void)
{
    mce_datapipe_quit_bindings(&callstate_datapipe_bindings);
}

/* ========================================================================= *
 * MODULE LOAD / UNLOAD
 * ========================================================================= */

/**
 * Init function for the call state module
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    call_state_rethink_tmr = mce_wltimer_create("call_state_rethink",
                                                0, call_state_rethink_cb, 0);

    /* create look up tables */
    clients_init();
    vcalls_init();
    modems_init();

    /* install datapipe hooks */
    callstate_datapipes_init();

    /* install dbus message handlers */
    mce_callstate_init_dbus();

    /* initiate async query to find out current state of ofono */
    xofono_name_owner_get();

    return NULL;
}

/**
 * Exit function for the call state module
 *
 * @todo D-Bus unregistration
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
    (void)module;

    /* remove dbus message handlers */
    mce_callstate_quit_dbus();

    /* remove datapipe hooks */
    callstate_datapipes_quit();

    /* remove all timers & callbacks */
    mce_wltimer_delete(call_state_rethink_tmr),
        call_state_rethink_tmr = 0;

    /* delete look up tables */
    modems_quit();
    vcalls_quit();
    clients_quit();

    return;
}
