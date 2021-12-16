/**
 * @file audiorouting.c
 * Audio routing module -- this listens to the audio routing
 * <p>
 * Copyright © 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2014-2019 Jolla Ltd.
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

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-dbus.h"

#include <stdio.h>
#include <string.h>

#include <gmodule.h>

/** Module name */
#define MODULE_NAME             "audiorouting"

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
        /** Name of the module */
        .name = MODULE_NAME,
        /** Module provides */
        .provides = provides,
        /** Module priority */
        .priority = 100
};

/** D-Bus interface for the policy framework */
#define POLICY_DBUS_INTERFACE           "com.nokia.policy"

/** D-Bus signal for actions from the policy framework */
#define POLICY_AUDIO_ACTIONS            "audio_actions"

/** Bits for members values available in ohm_decision_t */
enum
{
    DF_NONE     = 0,
    DF_TYPE     = 1 << 0,
    DF_DEVICE   = 1 << 1,
    DF_MUTE     = 1 << 2,
    DF_GROUP    = 1 << 3,
    DF_CORK     = 1 << 4,
    DF_MODE     = 1 << 5,
    DF_HWID     = 1 << 6,
    DF_VARIABLE = 1 << 7,
    DF_VALUE    = 1 << 8,
    DF_LIMIT    = 1 << 9,
};

/** Generic struct capable of holding any ohm decision data */
typedef struct
{
    unsigned      fields;
    const char   *type;
    const char   *device;
    const char   *mute;
    const char   *group;
    const char   *cork;
    const char   *mode;
    const char   *hwid;
    const char   *variable;
    const char   *value;
    dbus_int32_t  limit;
} ohm_decision_t;

/** Lookup table for parsing/showing ohm_decision_t content */
static const struct
{
    const char *name;
    unsigned    field;
    int         type;
    size_t      offs;
} field_lut[] =
{
    // fields
    { "type",    DF_TYPE,    's', offsetof(ohm_decision_t, type)     },
    { "device",  DF_DEVICE,  's', offsetof(ohm_decision_t, device)   },
    { "mute",    DF_MUTE,    's', offsetof(ohm_decision_t, mute)     },
    { "group",   DF_GROUP,   's', offsetof(ohm_decision_t, group)    },
    { "cork",    DF_CORK,    's', offsetof(ohm_decision_t, cork)     },
    { "mode",    DF_MODE,    's', offsetof(ohm_decision_t, mode)     },
    { "hwid",    DF_HWID,    's', offsetof(ohm_decision_t, hwid)     },
    { "variable",DF_VARIABLE,'s', offsetof(ohm_decision_t, variable) },
    { "value",   DF_VALUE,   's', offsetof(ohm_decision_t, value)    },
    { "limit",   DF_LIMIT,   'i', offsetof(ohm_decision_t, limit)    },

    // sentinel
    { 0,         DF_NONE,    '-', 0 }
};

/** Lookup table for mce audio route from sink device reported by ohmd */
static const struct
{
    const char        *device;
    audio_route_t      route;
} route_lut[] = {
    { "bta2dp",           AUDIO_ROUTE_HEADSET, },
    { "bthfp",            AUDIO_ROUTE_HEADSET, },
    { "bthsp",            AUDIO_ROUTE_HEADSET, },
    { "earpiece",         AUDIO_ROUTE_HANDSET, },
    { "earpieceandtvout", AUDIO_ROUTE_HANDSET, },
    { "fmtx",             AUDIO_ROUTE_UNDEF,   },
    { "headphone",        AUDIO_ROUTE_UNDEF,   },
    { "headset",          AUDIO_ROUTE_HEADSET, },
    { "ihf",              AUDIO_ROUTE_SPEAKER, },
    { "ihfandbthsp",      AUDIO_ROUTE_SPEAKER, },
    { "ihfandfmtx",       AUDIO_ROUTE_SPEAKER, },
    { "ihfandheadset",    AUDIO_ROUTE_HEADSET, },
    { "ihfandtvout",      AUDIO_ROUTE_SPEAKER, },
    { "null",             AUDIO_ROUTE_UNDEF,   },
    { "tvout",            AUDIO_ROUTE_UNDEF,   },
    { "tvoutandbta2dp",   AUDIO_ROUTE_HEADSET, },
    { "tvoutandbthsp",    AUDIO_ROUTE_HEADSET, },
    // sentinel
    { NULL,               AUDIO_ROUTE_UNDEF,   },
};

/** Audio route; derived from audio sink device name */
static audio_route_t audio_route = AUDIO_ROUTE_UNDEF;

/** Audio playback: derived from media_state */
static tristate_t media_playback_state = TRISTATE_UNKNOWN;

/* Volume limits used for "music playback" heuristics */
static int volume_limit_player     = 100;
static int volume_limit_flash      = 100;
static int volume_limit_inputsound = 100;

/* Prototypes for local functions */
static void     audio_mute_cb(ohm_decision_t *ohm);
static void     audio_cork_cb(ohm_decision_t *ohm);
static void     audio_route_sink(ohm_decision_t *ohm);
static void     audio_route_cb(ohm_decision_t *ohm);
static void     volume_limit_cb(ohm_decision_t *ohm);
static void     context_cb(ohm_decision_t *ohm);
static void     ohm_decision_reset_fields(ohm_decision_t *self);
static void     ohm_decision_show_fields(ohm_decision_t *self);
static bool     ohm_decision_parse_field(ohm_decision_t *self, const char *field, DBusMessageIter *from);
static bool     ohm_decision_parse(ohm_decision_t *self, DBusMessageIter *arr);
static bool     handle_policy_decisions(DBusMessageIter *ent, void (*cb)(ohm_decision_t *));
static bool     handle_policy(DBusMessageIter *arr);
static gboolean actions_dbus_cb(DBusMessage *sig);

/** Helper for doing base + offset address calculations */
static inline void *lea(const void *base, off_t offs)
{
    return (char *)(base) + offs;
}

/** Handle com.nokia.policy.audio_mute decision
 */
static void audio_mute_cb(ohm_decision_t *ohm)
{
    unsigned want = DF_DEVICE | DF_MUTE;
    unsigned have = ohm->fields & want;

    if( have != want )
        goto EXIT;

    // nothing for mce in here

EXIT:
    return;
}

/** Handle com.nokia.policy.audio_cork decision
 */
static void audio_cork_cb(ohm_decision_t *ohm)
{
    unsigned want = DF_GROUP | DF_CORK;
    unsigned have = ohm->fields & want;

    if( have != want )
        goto EXIT;

    // nothing for mce in here

EXIT:
    return;
}

/** Handle com.nokia.policy.audio_route decision for sink device
 */
static void audio_route_sink(ohm_decision_t *ohm)
{
    /* Lookup audio route id from sink device name.
     *
     * Note: For the purposes of mce device names
     * "xxx" and "xxxforcall" are considered equal.
     */
    for( int i = 0; ; ++i ) {
        if( !route_lut[i].device ) {
            mce_log(LL_WARN, "unknown audio sink device = '%s'", ohm->device);
            audio_route = route_lut[i].route;
            break;
        }

        int n = strlen(route_lut[i].device);

        if( strncmp(route_lut[i].device, ohm->device, n) )
            continue;

        if( ohm->device[n] && strcmp(ohm->device + n, "forcall") )
            continue;

        audio_route = route_lut[i].route;
        break;
    }

    mce_log(LL_DEBUG, "audio sink '%s' -> audio route %s",
            ohm->device, audio_route_repr(audio_route));
}

/** Handle com.nokia.policy.audio_route decision
 */
static void audio_route_cb(ohm_decision_t *ohm)
{
    unsigned want = DF_TYPE | DF_DEVICE | DF_MODE | DF_HWID;
    unsigned have = ohm->fields & want;

    if( have != want )
        goto EXIT;

    mce_log(LL_DEBUG, "handling: %s - %s - %s - %s",
            ohm->type, ohm->device, ohm->mode, ohm->hwid);

    if( !strcmp(ohm->type, "sink") ) {
        audio_route_sink(ohm);
    }

EXIT:
    return;
}

/** Handle com.nokia.policy.volume_limit decision
 */
static void volume_limit_cb(ohm_decision_t *ohm)
{
    unsigned want = DF_GROUP | DF_LIMIT;
    unsigned have = ohm->fields & want;

    if( have != want )
        goto EXIT;

    if( !strcmp(ohm->group, "player") ) {
        if( volume_limit_player != ohm->limit ) {
            mce_log(LL_DEBUG, "volume_limit_player: %d -> %d",
                    volume_limit_player, ohm->limit);
            volume_limit_player = ohm->limit;
        }
    }
    else if( !strcmp(ohm->group, "flash") ) {
        if( volume_limit_flash != ohm->limit ) {
            mce_log(LL_DEBUG, "volume_limit_flash: %d -> %d",
                    volume_limit_flash, ohm->limit);
            volume_limit_flash = ohm->limit;
        }
    }
    else if( !strcmp(ohm->group, "inputsound") ) {
        if( volume_limit_inputsound != ohm->limit ) {
            mce_log(LL_DEBUG, "volume_limit_inputsound: %d -> %d",
                    volume_limit_inputsound, ohm->limit);
            volume_limit_inputsound = ohm->limit;
        }
    }
EXIT:
    return;
}

/** Handle com.nokia.policy.context decision
 */
static void context_cb(ohm_decision_t *ohm)
{
    unsigned want = DF_VARIABLE | DF_VALUE;
    unsigned have = ohm->fields & want;

    if( have != want )
        goto EXIT;

    if( !strcmp(ohm->variable, "media_state") ) {
        tristate_t state = TRISTATE_UNKNOWN;

        if( !strcmp(ohm->value, "active") || !strcmp(ohm->value, "background") )
            state = TRISTATE_TRUE;
        else
            state = TRISTATE_FALSE;

        if( media_playback_state != state ) {
            mce_log(LL_DEBUG, "media_playback_state: %s -> %s",
                    tristate_repr(media_playback_state),
                    tristate_repr(state));
            media_playback_state = state;
        }
    }

EXIT:
    return;
}

/** Reset ohm_decision_t fields
 */
static void ohm_decision_reset_fields(ohm_decision_t *self)
{
    self->fields   = DF_NONE;

    self->type     = 0;
    self->device   = 0;
    self->mute     = 0;
    self->group    = 0;
    self->cork     = 0;
    self->mode     = 0;
    self->hwid     = 0;
    self->variable = 0;
    self->value    = 0;

    self->limit    = -1;
}

/** Show ohm_decision_t fields
 */
static void ohm_decision_show_fields(ohm_decision_t *self)
{
    char buf[1024];

    *buf = 0;

    for( int i = 0; field_lut[i].name; ++i ) {
        if( !(self->fields & field_lut[i].field) )
            continue;
        switch( field_lut[i].type ) {
        case 's':
            sprintf(strchr(buf,0), " %s='%s'",
                    field_lut[i].name,
                    *(const char **)lea(self, field_lut[i].offs));
            break;
        case 'i':
            sprintf(strchr(buf,0), " %s=%ld",
                    field_lut[i].name,
                    (long)*(dbus_int32_t *)lea(self, field_lut[i].offs));
            break;
        default:
            sprintf(strchr(buf,0), " %s=???", field_lut[i].name);
            break;
        }
    }

    if( *buf )
        mce_log(LL_DEBUG, "%s", buf+1);
}

/** Parse one named field from dbus message iterator
 */
static bool ohm_decision_parse_field(ohm_decision_t *self,
                                     const char *field,
                                     DBusMessageIter *from)
{

    bool ack = false;

    for( int i = 0; ; ++i ) {
        if( !field_lut[i].name ) {
            mce_log(LL_WARN, "unhandled ohm field '%s'", field);
            break;
        }

        if( strcmp(field, field_lut[i].name) )
            continue;

        switch( field_lut[i].type ) {
        case 's':
            ack = mce_dbus_iter_get_string(from, lea(self, field_lut[i].offs));
            break;
        case 'i':
            ack = mce_dbus_iter_get_int32(from, lea(self, field_lut[i].offs));
            break;
        default:
            ack = true;
            break;
        }

        self->fields |= field_lut[i].field;
        break;
    }

    return ack;
}

/** Parse all decision fields from dbus message iterator
 */
static bool ohm_decision_parse(ohm_decision_t *self, DBusMessageIter *arr)
{
    bool ack = false;

    DBusMessageIter str, var;

    while( !mce_dbus_iter_at_end(arr) ) {
        const char *key  = 0;
        if( !mce_dbus_iter_get_struct(arr, &str) )
            goto EXIT;
        if( !mce_dbus_iter_get_string(&str, &key) )
            goto EXIT;
        if( !mce_dbus_iter_get_variant(&str, &var) )
            goto EXIT;
        if( !ohm_decision_parse_field(self, key, &var) )
            goto EXIT;
    }

    ack = true;
EXIT:
    return ack;
}

/** Handle policy decision blocks within audio_actions signal */
static bool handle_policy_decisions(DBusMessageIter *ent, void (*cb)(ohm_decision_t *))
{
    bool ack = false;

    DBusMessageIter arr1, arr2;

    if( !mce_dbus_iter_get_array(ent, &arr1) )
        goto EXIT;

    while( !mce_dbus_iter_at_end(&arr1) ) {
        if( !mce_dbus_iter_get_array(&arr1, &arr2) )
            goto EXIT;

        ohm_decision_t ohm;
        ohm_decision_reset_fields(&ohm);

        if( !ohm_decision_parse(&ohm, &arr2) )
            goto EXIT;

        if( mce_log_p(LL_DEBUG) )
            ohm_decision_show_fields(&ohm);

        cb(&ohm);
    }

    ack = true;
EXIT:
    return ack;
}

/** Handle policy block within audio_actions signal
 */
static bool handle_policy(DBusMessageIter *arr)
{
    bool ack = false;

    const char *name = 0;

    DBusMessageIter ent;

    if( !mce_dbus_iter_get_entry(arr, &ent) )
        goto EXIT;

    if( !mce_dbus_iter_get_string(&ent, &name) )
        goto EXIT;

    mce_log(LL_DEBUG, "policy name = %s", name);

    void (*cb)(ohm_decision_t *) = 0;

    /* com.nokia.policy.audio_mute
     *  device - mute
     *
     * com.nokia.policy.audio_cork
     *  group - cork
     *
     * com.nokia.policy.audio_route
     *  type - device - mode - hwid
     *
     * com.nokia.policy.volume_limit
     *  group - limit
     *
     * com.nokia.policy.context
     *  variable - value
     */

    if( !strcmp(name, "com.nokia.policy.audio_mute") )
        cb = audio_mute_cb;
    else if( !strcmp(name, "com.nokia.policy.audio_cork") )
        cb = audio_cork_cb;
    else if( !strcmp(name, "com.nokia.policy.audio_route") )
        cb = audio_route_cb;
    else if( !strcmp(name, "com.nokia.policy.volume_limit") )
        cb = volume_limit_cb;
    else if( !strcmp(name, "com.nokia.policy.context") )
        cb = context_cb;
    else
        mce_log(LL_WARN, "unknown policy '%s'", name);

    if( cb && !handle_policy_decisions(&ent, cb) )
        goto EXIT;

    ack = true;
EXIT:
    return ack;
}

/**
 * D-Bus callback for the actions signal
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean actions_dbus_cb(DBusMessage *sig)
{
    bool playback = false;

    DBusMessageIter body, arr;

    mce_log(LL_DEVEL, "Received audio policy actions from %s",
            mce_dbus_get_message_sender_ident(sig));

    dbus_message_iter_init(sig, &body);

    dbus_uint32_t unused = 0;

    if( !mce_dbus_iter_get_uint32(&body, &unused) )
        goto EXIT;

    if( !mce_dbus_iter_get_array(&body, &arr) )
        goto EXIT;

    while( !mce_dbus_iter_at_end(&arr) ) {
        if( !handle_policy(&arr) )
            goto EXIT;
    }

    if( media_playback_state != TRISTATE_UNKNOWN ) {
        /* Use media_state from com.nokia.policy.context
         * when it is included in OHM policy signal. */
        playback = (media_playback_state == TRISTATE_TRUE);
    }
    else {
        /* Fallback to volume limit heuristics */
        playback = (volume_limit_player > 0 &&
                    volume_limit_flash <= 0 &&
                    volume_limit_inputsound <= 0);
    }

EXIT:
    if( datapipe_get_gint(music_playback_ongoing_pipe) != playback ) {
        mce_log(LL_DEVEL, "music playback: %d", playback);
        datapipe_exec_full(&music_playback_ongoing_pipe,
                           GINT_TO_POINTER(playback));
    }

    if( datapipe_get_gint(audio_route_pipe) != audio_route ) {
        mce_log(LL_DEVEL, "audio route: %s", audio_route_repr(audio_route));
        datapipe_exec_full(&audio_route_pipe,
                           GINT_TO_POINTER(audio_route));
    }

    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t handlers[] =
{
    /* signals */
    {
        .interface = POLICY_DBUS_INTERFACE,
        .name      = POLICY_AUDIO_ACTIONS,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = actions_dbus_cb,
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/**
 * Init function for the audio routing module
 *
 * @param module Unused
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    mce_dbus_handler_register_array(handlers);

    return NULL;
}

/**
 * Exit function for the audio routing module
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
    (void)module;

    mce_dbus_handler_unregister_array(handlers);

    return;
}
