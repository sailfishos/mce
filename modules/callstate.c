/**
 * @file callstate.c
 * Call state module -- this handles the call state for MCE
 * <p>
 * Copyright © 2008-2009 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
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
#include <glib.h>
#include <stdlib.h>
#include <gmodule.h>
#include <stdbool.h>
#include <string.h>
#include "mce.h"
#include "callstate.h"
#include <mce/mode-names.h>
#include "mce.h"
#include "mce-lib.h"
#include "mce-log.h"
#include "mce-dbus.h"
#include "datapipe.h"

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
 * MCE CALL STATE / TYPE UTILS
 * ========================================================================= */

/** Mapping of call state integer <-> call state string */
static const mce_translation_t call_state_translation[] = {
        {
                .number = CALL_STATE_NONE,
                .string = MCE_CALL_STATE_NONE
        }, {
                .number = CALL_STATE_RINGING,
                .string = MCE_CALL_STATE_RINGING,
        }, {
                .number = CALL_STATE_ACTIVE,
                .string = MCE_CALL_STATE_ACTIVE,
        }, {
                .number = CALL_STATE_SERVICE,
                .string = MCE_CALL_STATE_SERVICE
        }, { /* MCE_INVALID_TRANSLATION marks the end of this array */
                .number = MCE_INVALID_TRANSLATION,
                .string = MCE_CALL_STATE_NONE
        }
};

/** MCE call state number to string
 */
static const char *call_state_repr(call_state_t state)
{
    return mce_translate_int_to_string(call_state_translation, state);
}

/** String to MCE call state number */
static call_state_t call_state_parse(const char *name)
{
    return mce_translate_string_to_int(call_state_translation, name);
}

/** Mapping of call type integer <-> call type string */
static const mce_translation_t call_type_translation[] = {
        {
                .number = NORMAL_CALL,
                .string = MCE_NORMAL_CALL
        }, {
                .number = EMERGENCY_CALL,
                .string = MCE_EMERGENCY_CALL
        }, { /* MCE_INVALID_TRANSLATION marks the end of this array */
                .number = MCE_INVALID_TRANSLATION,
                .string = MCE_NORMAL_CALL
        }
};

/** MCE call type number to string
 */
static const char *call_type_repr(call_type_t type)
{
    return mce_translate_int_to_string(call_type_translation, type);
}

/** String to MCE call type number
 */
static call_type_t call_type_parse(const char *name)
{
    return mce_translate_string_to_int(call_type_translation, name);
}

/* ========================================================================= *
 * MODULE DATA
 * ========================================================================= */

/** List of monitored call state requesters; holds zero or one entries */
static GSList *call_state_monitor_list = NULL;

static void call_state_rethink_schedule(void);
static bool call_state_rethink_forced(void);

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
        mce = CALL_STATE_RINGING;
        break;

    case OFONO_CALL_STATE_DIALING:
    case OFONO_CALL_STATE_ALERTING:
    case OFONO_CALL_STATE_ACTIVE:
    case OFONO_CALL_STATE_HELD:
    case OFONO_CALL_STATE_WAITING:
        mce = CALL_STATE_ACTIVE;
        break;
    }

    return mce;
}

/** oFono emergency flag to mce call type number
 */
static call_type_t ofono_calltype_to_mce(bool emergency)
{
    return emergency ? EMERGENCY_CALL : NORMAL_CALL;
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

/** Merge emergency data to oFono voice call object
 *
 * @param self      oFono voice call object
 * @param emergency emergency state to merge
 */
static void
ofono_vcall_merge_emergency(ofono_vcall_t *self, bool emergency)
{
    if( emergency )
        self->type = EMERGENCY_CALL;
}

/** Merge state data from oFono voice call object to another
 *
 * @param self      oFono voice call object
 * @param emergency emergency state to merge
 */
static void
ofono_vcall_merge_vcall(ofono_vcall_t *self, const ofono_vcall_t *that)
{
    /* if any call is active, we have active call.
     * otherwise we can have incoming call too */
    switch( that->state ) {
    case CALL_STATE_ACTIVE:
        self->state = CALL_STATE_ACTIVE;
        break;

    case CALL_STATE_RINGING:
        if( self->state != CALL_STATE_ACTIVE )
            self->state = CALL_STATE_RINGING;
        break;

    default:
        break;
    }

    /* if any call is emergency, we have emergency call */
    if( that->type == EMERGENCY_CALL )
        self->type = EMERGENCY_CALL;
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
    self->type   = NORMAL_CALL;

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
    char *name;
    bool probed;

    bool emergency;

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

/** Get voice calls associated with a modem [sync]
 *
 * Populate voice call lookup table with the reply data
 *
 * @param modem D-Bus object path
 */
static void
xofono_get_vcalls(const char *modem)
{
    DBusMessage *rsp = 0;
    DBusError    err = DBUS_ERROR_INIT;
    int          cnt = 0;

    /* FIXME: mce-dbus not have a async method call with
     * context data pointer -> using sync call for now ... */
    rsp = dbus_send_with_block(OFONO_SERVICE,
                               modem,
                               OFONO_VCALLMANAGER_INTERFACE,
                               OFONO_VCALLMANAGER_REQ_GET_CALLS,
                               -1,
                               DBUS_TYPE_INVALID);
    if( !rsp )
        goto EXIT;

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

        if( !modem->probed )
            xofono_get_vcalls(modem->name);

        ofono_modem_update_N(modem, &mod);
        ++cnt;
    }
    call_state_rethink_schedule();

EXIT:
    mce_log(LL_DEBUG, "added %d modems", cnt);
    if( rsp ) dbus_message_unref(rsp);
    dbus_pending_call_unref(pc);
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
        if( !modem->probed )
            xofono_get_vcalls(modem->name);

        ofono_modem_update_N(modem, &body);
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
    if( pc  ) dbus_pending_call_unref(pc);
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
                sstate = call_state_repr(datapipe_get_gint(call_state_pipe));

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
	if (dbus_message_append_args(msg,
				     DBUS_TYPE_STRING, &sstate,
				     DBUS_TYPE_STRING, &stype,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append %sarguments to D-Bus message "
			"for %s.%s",
			method_call ? "reply " : "",
			method_call ? MCE_REQUEST_IF :
				      MCE_SIGNAL_IF,
			method_call ? MCE_CALL_STATE_GET :
				      MCE_CALL_STATE_SIG);
		dbus_message_unref(msg);
		goto EXIT;
	}

	/* Send the message */
	status = dbus_send_message(msg);

EXIT:
	return status;
}

/** Simulated call state */
static ofono_vcall_t simulated =
{
    .state = CALL_STATE_NONE,
    .type  = NORMAL_CALL,
};

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
	gboolean status = FALSE;
	const gchar *old_name;
	const gchar *new_name;
	const gchar *service;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	/* Extract result */
	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &service,
				  DBUS_TYPE_STRING, &old_name,
				  DBUS_TYPE_STRING, &new_name,
				  DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_ERR,
			"Failed to get argument from %s.%s; %s",
			"org.freedesktop.DBus", "NameOwnerChanged",
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	/* Remove the name monitor for the call state requester */
	if (mce_dbus_owner_monitor_remove(service,
					  &call_state_monitor_list) == 0) {
            simulated.state = CALL_STATE_NONE;
            simulated.type  = NORMAL_CALL;
            call_state_rethink_schedule();
	}

	status = TRUE;

EXIT:
	return status;
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
    gboolean     status = FALSE;
    const char  *state  = 0;
    const char  *type   = 0;
    const gchar *sender = dbus_message_get_sender(msg);

    call_state_t call_state = CALL_STATE_NONE;
    call_type_t  call_type = NORMAL_CALL;
    DBusMessage *reply = NULL;
    DBusError    error = DBUS_ERROR_INIT;
    dbus_bool_t changed = false;

    mce_log(LL_DEBUG, "Received set call state request");

    if (dbus_message_get_args(msg, &error,
                              DBUS_TYPE_STRING, &state,
                              DBUS_TYPE_STRING, &type,
                              DBUS_TYPE_INVALID) == FALSE) {
        // XXX: return an error!
        mce_log(LL_CRIT, "Failed to get argument from %s.%s: %s",
                MCE_REQUEST_IF, MCE_CALL_STATE_CHANGE_REQ,
                error.message);
        dbus_error_free(&error);
        goto EXIT;
    }

    /* Convert call state to enum */
    call_state = call_state_parse(state);
    if (call_state == MCE_INVALID_TRANSLATION) {
	mce_log(LL_DEBUG,
                "Invalid call state received; request ignored");
        goto EXIT;
    }

    /* Convert call type to enum */
    call_type = call_type_parse(type);
    if (call_type == MCE_INVALID_TRANSLATION) {
        mce_log(LL_DEBUG,
                "Invalid call type received; request ignored");
        goto EXIT;
    }

    /* reject no-call emergency calls ... */
    if( call_state == CALL_STATE_NONE )
        call_type = NORMAL_CALL;

    /* If call state isn't monitored or if the request comes from
     * the owner of the current state, then some additional changes
     * are ok
     */
    if( call_state_monitor_list &&
        !mce_dbus_is_owner_monitored(sender,
                                     call_state_monitor_list) ) {
        mce_log(LL_DEBUG, "Call state already has owner; ignoring request");
        goto EXIT;
    }

    /* Only transitions to/from "none" are allowed,
     * and from "ringing" to "active",
     * to avoid race conditions; except when new tuple
     * is active:emergency
     */
    if( call_state == CALL_STATE_ACTIVE &&
        simulated.state != CALL_STATE_RINGING &&
        call_type != EMERGENCY_CALL )
    {
        mce_log(LL_INFO,
                "Call state change vetoed.  Requested: %i:%i "
                "(current: %i:%i)",
                call_state, call_type,
                simulated.state, simulated.type);
        goto EXIT;
    }

    if( call_state != CALL_STATE_NONE &&
        mce_dbus_owner_monitor_add(sender, call_state_owner_monitor_dbus_cb,
                                   &call_state_monitor_list, 1) != -1 ) {

        simulated.state = call_state;
        simulated.type  = call_type;
    }
    else {
        mce_dbus_owner_monitor_remove(sender, &call_state_monitor_list);
        simulated.state = CALL_STATE_NONE;
        simulated.type  = NORMAL_CALL;
    }
    changed = call_state_rethink_forced();

EXIT:
    /* Setup the reply */
    reply = dbus_new_method_reply(msg);

    /* Append the result */
    if (dbus_message_append_args(reply,
                                 DBUS_TYPE_BOOLEAN, &changed,
                                 DBUS_TYPE_INVALID) == FALSE) {
        mce_log(LL_CRIT,
                "Failed to append reply arguments to D-Bus "
                "message for %s.%s",
                MCE_REQUEST_IF, MCE_CALL_STATE_CHANGE_REQ);
        dbus_message_unref(reply);
    } else {
        /* Send the message */
        status = dbus_send_message(reply);
    }

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
	if (send_call_state(msg, NULL, NULL) == FALSE)
		goto EXIT;

	status = TRUE;

EXIT:
	return status;
}

/* ========================================================================= *
 * MANAGE CALL STATE TRANSITIONS
 * ========================================================================= */

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
        .type  = NORMAL_CALL,
    };

    ofono_vcall_t combined =
    {
        .state = CALL_STATE_NONE,
        .type  = NORMAL_CALL,
    };

    /* consider simulated call state */
    ofono_vcall_merge_vcall(&combined, &simulated);

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

    execute_datapipe(&call_state_pipe,
                     GINT_TO_POINTER(call_state),
                     USE_INDATA, CACHE_INDATA);

    execute_datapipe(&call_type_pipe,
                     GINT_TO_POINTER(call_type),
                     USE_INDATA, CACHE_INDATA);

EXIT:
    return changed;
}

/** Timer id for evaluating call state */
static guint call_state_rethink_id = 0;

/** Timer callback for evaluating call state */
static gboolean
call_state_rethink_cb(gpointer aptr)
{
    (void)aptr;

    if( !call_state_rethink_id )
        goto EXIT;
    call_state_rethink_id = 0;

    call_state_rethink_now();

EXIT:
    return G_SOURCE_REMOVE;
}

/** Cancel delayed call state evaluation */
static void
call_state_rethink_cancel(void)
{
    if( call_state_rethink_id )
        g_source_remove(call_state_rethink_id), call_state_rethink_id = 0;
}

/** Request delayed call state evaluation */
static void
call_state_rethink_schedule(void)
{
    if( !call_state_rethink_id )
        call_state_rethink_id = g_idle_add(call_state_rethink_cb, 0);
}

/** Request immediate call state evaluation */
static bool
call_state_rethink_forced(void)
{
    call_state_rethink_cancel();
    return call_state_rethink_now();
}

/* ========================================================================= *
 * MODULE LOAD / UNLOAD
 * ========================================================================= */

/** Array of dbus message handlers */
static mce_dbus_handler_t handlers[] =
{
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_CALL_STATE_CHANGE_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = change_call_state_dbus_cb,
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_CALL_STATE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = get_call_state_dbus_cb,
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

    /* create look up tables */
    vcalls_init();
    modems_init();

    /* install dbus message handlers */
    mce_dbus_handler_register_array(handlers);

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
    mce_dbus_handler_unregister_array(handlers);

    /* remove all timers & callbacks */
    call_state_rethink_cancel();

    /* delete look up tables */
    modems_quit();
    vcalls_quit();

    return;
}
