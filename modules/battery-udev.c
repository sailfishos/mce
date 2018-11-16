/**
 * @file battery-udev.c
 * Battery module -- this implements battery and charger logic for MCE
 * <p>
 * Copyright (C) 2018 Jolla Ltd.
 * <p>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * <p>
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
#include "../mce-io.h"
#include "../mce-lib.h"
#include "../mce-log.h"
#include "../mce-conf.h"
#include "../mce-wakelock.h"

#include <libudev.h>

#include <gmodule.h>

/* ========================================================================= *
 * Constants
 * ========================================================================= */

/** Module name */
#define MODULE_NAME "battery_udev"

/** Whether to support legacy pattery low led pattern; nonzero for yes */
#define SUPPORT_BATTERY_LOW_LED_PATTERN 0

/* Limits for udev capacity percent -> battery_status_t mapping
 *
 * FIXME: These should be configurable / device type, and they should be
 *        defined in one place only. Currently we have:
 *        - this mce plugin: hardcoded values
 *        - dsme: hardcoded / from config file values
 *        - statefs: hardcoded / from environment values
 */
#define BATTERY_CAPACITY_UNDEF   -1
#define BATTERY_CAPACITY_EMPTY    2 // statefs uses 3, dsme defaults to 2
#define BATTERY_CAPACITY_LOW     10 // statefs uses 10
#define BATTERY_CAPACITY_FULL    90 // statefs uses 96

/* Power supply device properties we are interested in */
#define PROP_PRESENT  "POWER_SUPPLY_PRESENT"
#define PROP_ONLINE   "POWER_SUPPLY_ONLINE"
#define PROP_CAPACITY "POWER_SUPPLY_CAPACITY"
#define PROP_STATUS   "POWER_SUPPLY_STATUS"

#define MCE_CONF_BATTERY_UDEV_PROPERTY_BLACKLIST_GROUP "BatteryUDevPropertyBlacklist"
#define MCE_CONF_BATTERY_UDEV_DEVICE_BLACKLIST_GROUP   "BatteryUDevDeviceBlacklist"

/* ========================================================================= *
 * Types
 * ========================================================================= */

/** Classification of power supply device properties
 */
typedef enum
{
    /** Placeholder value, property type not defined in lookup table
     *
     * Interpreted as PROPERTY_TYPE_DEBUG or PROPERTY_TYPE_IGNORE
     * depending on whether property blacklist configuration block
     * exists or not.
     */
    PROPERTY_TYPE_UNDEF,

    /** Property has been configured to be completely ignored */
    PROPERTY_TYPE_IGNORE,

    /** Property has been configured to be shown for debugging purposes */
    PROPERTY_TYPE_DEBUG,

    /** Property has been configured to be relevant for state evaluation */
    PROPERTY_TYPE_USED,
} property_type_t;

/** Battery properties in mce statemachine compatible form
 */
typedef struct
{
    /** Battery charge percentage; for use with battery_level_pipe */
    int              battery_level;

    /** Battery FULL/OK/LOW/EMPTY; for use with battery_status_pipe */
    battery_status_t battery_status;

    /** Charger connected; for use with charger_state_pipe */
    charger_state_t  charger_state;
} mcebat_t;

typedef struct udevtracker_t  udevtracker_t;
typedef struct udevdevice_t   udevdevice_t;
typedef struct udevproperty_t udevproperty_t;

/** Bookkeeping data for udev power supply device tracking
 */
struct udevtracker_t
{
    /** udev handle */
    struct udev         *udt_udev_handle;

    /** Monitor for power supply devices */
    struct udev_monitor *udt_udev_monitor;

    /** I/O watch id for monitor input */
    guint                udt_udev_event_id;

    /** Timer id for delayed state re-evaluation */
    guint                udt_rethink_id;

    /** Cached charger/battery device data */
    GHashTable          *udt_devices; // [dev_name] -> udevdevice_t *
};

/** Bookkeeping data for a single udev power supply device
 */
struct udevdevice_t
{
    /** Device sysname */
    gchar      *udd_name;

    /** Properties associated with the device */
    GHashTable *udd_props; // [key_name] -> udevproperty_t *

    /** Flag for: Device has reached battery full state */
    bool        udd_full;

    /** Flag for: a "change" notification has been received from udev */
    bool        udd_changed;
};

/** Bookkeeping data for a single udev device property
 */
struct udevproperty_t
{
    /** Containing device */
    udevdevice_t *udp_dev;

    /** Property name */
    gchar        *udp_key;

    /** Property value */
    gchar        *udp_val;

    /** Flag for: Property is used in state evaluation */
    bool          udp_used;
};

/* ========================================================================= *
 * Protos
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * MCEBAT
 * ------------------------------------------------------------------------- */

static void  mcebat_update(const mcebat_t *curr);

/* ------------------------------------------------------------------------- *
 * UDEVPROPERTY
 * ------------------------------------------------------------------------- */

static void              udevproperty_init_types (void);
static void              udevproperty_quit_types (void);
static property_type_t   udevproperty_lookup_type(const char *key);
static bool              udevproperty_is_used    (const char *key);
static bool              udevproperty_is_ignored (const char *key);
static udevproperty_t   *udevproperty_create     (udevdevice_t *dev, const char *key);
static void              udevproperty_delete     (udevproperty_t *self);
static void              udevproperty_delete_cb  (void *self);
static const char       *udevproperty_key        (const udevproperty_t *self);
static const char       *udevproperty_get        (const udevproperty_t *self);
static bool              udevproperty_set        (udevproperty_t *self, const char *val);

/* ------------------------------------------------------------------------- *
 * UDEVDEVICE
 * ------------------------------------------------------------------------- */

static void             udevdevice_init_blacklist(void);
static void             udevdevice_quit_blacklist(void);
static bool             udevdevice_is_blacklisted(const char *name);
static udevdevice_t    *udevdevice_create        (const char *name);
static void             udevdevice_delete        (udevdevice_t *self);
static void             udevdevice_delete_cb     (void *self);
static const char      *udevdevice_name          (const udevdevice_t *self);
static udevproperty_t  *udevdevice_get_prop      (udevdevice_t *self, const char *key);
static udevproperty_t  *udevdevice_add_prop      (udevdevice_t *self, const char *key);
static bool             udevdevice_set_prop      (udevdevice_t *self, const char *key, const char *val);
static const char      *udevdevice_get_str_prop  (udevdevice_t *self, const char *key, const char *def);
static int              udevdevice_get_int_prop  (udevdevice_t *self, const char *key, int def);
static bool             udevdevice_refresh       (udevdevice_t *self, struct udev_device *dev);
static void             udevdevice_evaluate      (udevdevice_t *self, mcebat_t *mcebat);
static void             udevdevice_evaluate_cb   (gpointer key, gpointer value, gpointer aptr);

/* ------------------------------------------------------------------------- *
 * UDEVTRACKER
 * ------------------------------------------------------------------------- */

static udevtracker_t  *udevtracker_create          (void);
static void            udevtracker_delete          (udevtracker_t *self);
static void            udevtracker_rethink         (udevtracker_t *self);
static gboolean        udevtracker_rethink_cb      (gpointer aptr);
static void            udevtracker_cancel_rethink  (udevtracker_t *self);
static void            udevtracker_schedule_rethink(udevtracker_t *self);
static udevdevice_t   *udevtracker_add_dev         (udevtracker_t *self, const char *name);
static void            udevtracker_update_device   (udevtracker_t *self, struct udev_device *dev);
static bool            udevtracker_start           (udevtracker_t *self);
static void            udevtracker_stop            (udevtracker_t *self);
static gboolean        udevtracker_event_cb        (GIOChannel *chn, GIOCondition cnd, gpointer aptr);

/* ------------------------------------------------------------------------- *
 * G_MODULE
 * ------------------------------------------------------------------------- */

const gchar  *g_module_check_init(GModule *module);
void          g_module_unload    (GModule *module);

/* ========================================================================= *
 * Data
 * ========================================================================= */

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info =
{
    /** Name of the module */
    .name = MODULE_NAME,
    /** Module provides */
    .provides = provides,
    /** Module priority */
    .priority = 100
};

/** Cached battery state as exposed in datapipes
 *
 * Note: To avoid mce startup time glitches, these must be kept in
 *       sync with default values held in the relevant datapipes.
 */
static mcebat_t mcebat_current = {
    .battery_level  = BATTERY_LEVEL_INITIAL,
    .battery_status = BATTERY_STATUS_UNDEF,
    .charger_state  = CHARGER_STATE_UNDEF,
};

/** Wakelock used for suspend proofing edev event processing */
static const char       udevtracker_wakelock[]   = "udevtracker_wakeup";

/** The device subsystem we are monitoring */
static const char       udevtracker_subsystem[]  = "power_supply";

/** Tracking state */
static udevtracker_t   *udevtracker_object       = 0;

/** Lookup table for device property classification */
static GHashTable      *udevproperty_type_lut    = 0;

/** Lookup table for device blacklisting */
static GHashTable      *udevdevice_blacklist_lut = 0;

/** How to treat unknown properties; default to ignoring them */
static property_type_t  udevproperty_type_def    = PROPERTY_TYPE_IGNORE;

/** Properties that affect battery/charger evaluation
 *
 * If values for these properties change, battery state
 * re-evaluation is triggered.
 *
 * @see #udevproperty_is_used()
 */
static const char * const udevproperty_used_keys[] = {
    // common
    PROP_PRESENT,
    // charger
    PROP_ONLINE,
    // battery
    PROP_CAPACITY,
    PROP_STATUS,
    NULL
};

/* ========================================================================= *
 * MCEBAT
 * ========================================================================= */

/** Update battery state visible in datapipes
 *
 * @param curr  Battery state data to expose.
 */
static void
mcebat_update(const mcebat_t *curr)
{
    mcebat_t prev = mcebat_current;
    mcebat_current = *curr;

    if( prev.charger_state != curr->charger_state ) {
        mce_log(LL_CRUCIAL, "charger_state: %s -> %s",
                charger_state_repr(prev.charger_state),
                charger_state_repr(curr->charger_state));

        /* Charger connected state */
        datapipe_exec_full(&charger_state_pipe,
                           GINT_TO_POINTER(curr->charger_state));

        /* Charging led pattern */
        if( curr->charger_state == CHARGER_STATE_ON ) {
            datapipe_exec_full(&led_pattern_activate_pipe,
                               MCE_LED_PATTERN_BATTERY_CHARGING);
        }
        else {
            datapipe_exec_full(&led_pattern_deactivate_pipe,
                               MCE_LED_PATTERN_BATTERY_CHARGING);
        }

        /* Generate activity */
        mce_datapipe_generate_activity();
    }

    if( prev.battery_status != curr->battery_status ) {
        mce_log(LL_CRUCIAL, "battery_status: %s -> %s",
                battery_status_repr(prev.battery_status),
                battery_status_repr(curr->battery_status));

        /* Battery full led pattern */
        if( curr->battery_status == BATTERY_STATUS_FULL ) {
            datapipe_exec_full(&led_pattern_activate_pipe,
                               MCE_LED_PATTERN_BATTERY_FULL);
        }
        else {
            datapipe_exec_full(&led_pattern_deactivate_pipe,
                               MCE_LED_PATTERN_BATTERY_FULL);
        }

#if SUPPORT_BATTERY_LOW_LED_PATTERN
        /* Battery low led pattern */
        if( curr->battery_status == BATTERY_STATUS_LOW ||
            curr->battery_status == BATTERY_STATUS_EMPTY ) {
            datapipe_exec_full(&led_pattern_activate_pipe,
                               MCE_LED_PATTERN_BATTERY_LOW);
        }
        else {
            datapipe_exec_full(&led_pattern_deactivate_pipe,
                               MCE_LED_PATTERN_BATTERY_LOW);
        }
#endif /* SUPPORT_BATTERY_LOW_LED_PATTERN */

        /* Battery charge state */
        datapipe_exec_full(&battery_status_pipe,
                           GINT_TO_POINTER(curr->battery_status));
    }

    if( prev.battery_level != curr->battery_level ) {
        mce_log(LL_CRUCIAL, "battery_level : %d -> %d",
                prev.battery_level, curr->battery_level);
        /* Battery charge percentage */
        datapipe_exec_full(&battery_level_pipe,
                           GINT_TO_POINTER(curr->battery_level));
    }
}

/* ========================================================================= *
 * UDEVPROPERTY
 * ========================================================================= */

/** Initialize device property classification lookup table
 */
static void
udevproperty_init_types(void)
{
    static const char grp[] = MCE_CONF_BATTERY_UDEV_PROPERTY_BLACKLIST_GROUP;

    if( udevproperty_type_lut )
        goto EXIT;

    udevproperty_type_lut = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, 0);

    /* Deal with property blacklist configuration */
    if( mce_conf_has_group(grp) ) {
        /* Properties that are not listed in config group
         * are treated as show-for-debugging-purposes.
         */
        udevproperty_type_def = PROPERTY_TYPE_DEBUG;

        gsize   count = 0;
        gchar **keys  = mce_conf_get_keys(grp, &count);
        for( gsize i = 0; i < count; ++i ) {
            bool blacklisted = mce_conf_get_bool(grp, keys[i], true);
            g_hash_table_replace(udevproperty_type_lut,
                                 g_strdup(keys[i]),
                                 GINT_TO_POINTER(blacklisted
                                                 ? PROPERTY_TYPE_IGNORE
                                                 : PROPERTY_TYPE_DEBUG));
        }
        g_strfreev(keys);
    }

    /* Make sure that required properties are not blacklisted */
    for( size_t i = 0; udevproperty_used_keys[i]; ++i ) {
        g_hash_table_replace(udevproperty_type_lut,
                             g_strdup(udevproperty_used_keys[i]),
                             GINT_TO_POINTER(PROPERTY_TYPE_USED));
    }

EXIT:
    return;
}

/** Release device property classification lookup table
 */
static void
udevproperty_quit_types(void)
{
    if( udevproperty_type_lut ) {
        g_hash_table_unref(udevproperty_type_lut),
            udevproperty_type_lut = 0;
    }
}

/** Lookup device property classification
 *
 * @param key  property name
 *
 * @return property classification
 */
static property_type_t
udevproperty_lookup_type(const char *key)
{
    property_type_t type = PROPERTY_TYPE_IGNORE;

    if( udevproperty_type_lut ) {
        gpointer val = g_hash_table_lookup(udevproperty_type_lut, key);
        type = GPOINTER_TO_INT(val);
    }

    return (type == PROPERTY_TYPE_UNDEF) ? udevproperty_type_def : type;
}

/* Predicate for: Property is needed for battery/charging evaluation
 *
 * @param key  Property name
 *
 * @return true if property is used, false otherwise
 */
static bool
udevproperty_is_used(const char *key)
{
    return udevproperty_lookup_type(key) == PROPERTY_TYPE_USED;
}

/* Predicate for: Property should not be cached
 *
 * @param key  Property name
 *
 * @return true if property should be excluded, false otherwise
 */
static bool
udevproperty_is_ignored(const char *key)
{
    return udevproperty_lookup_type(key) == PROPERTY_TYPE_IGNORE;
}

/** Create property value object
 *
 * @param dev  Containing device
 * @param key  Property name
 *
 * @return property object
 */
static udevproperty_t *
udevproperty_create(udevdevice_t *dev, const char *key)
{
    udevproperty_t *self = g_malloc0(sizeof *self);

    self->udp_dev  = dev;
    self->udp_key  = g_strdup(key);
    self->udp_val  = 0;
    self->udp_used = udevproperty_is_used(key);

    return self;
}

/** Delete property value object
 *
 * @param self  property object, or NULL
 */
static void
udevproperty_delete(udevproperty_t *self)
{
    if( self != 0 ) {
        g_free(self->udp_key);
        g_free(self->udp_val);
        g_free(self);
    }
}

/** Type agnostic callback for deleting value objects
 *
 * @param self  property object, or NULL
 */
static void
udevproperty_delete_cb(void *self)
{
    udevproperty_delete(self);
}

/** Get property name
 *
 * @param self  property object
 *
 * @return property name
 */
static const char *
udevproperty_key(const udevproperty_t *self)
{
    return self->udp_key;
}

/** Get property value
 *
 * @param self  property object
 *
 * @return property value
 */
static const char *
udevproperty_get(const udevproperty_t *self)
{
    return self->udp_val;
}

/** Set property value
 *
 * @param self  property object
 * @param val   property value
 *
 * @return true if value was changed and is used for state evalueation,
 *         false otherwise
 */
static bool
udevproperty_set(udevproperty_t *self, const char *val)
{
    bool   rethink = false;
    gchar *prev    = self->udp_val;
    if( g_strcmp0(prev, val) ) {
        rethink = self->udp_used;
        mce_log(LL_DEBUG, "%s.%s : %s -> %s%s",
                udevdevice_name(self->udp_dev),
                udevproperty_key(self), prev, val,
                rethink ? "" : " (ignored)");
        self->udp_val = val ? g_strdup(val) : 0;
        g_free(prev);
    }
    return rethink;
}

/* ========================================================================= *
 * UDEVDEVICE
 * ========================================================================= */

/** Initialize device blacklist lookup table
 */
static void
udevdevice_init_blacklist(void)
{
    static const char grp[] = MCE_CONF_BATTERY_UDEV_DEVICE_BLACKLIST_GROUP;

    if( udevdevice_blacklist_lut )
        goto EXIT;

    if( !mce_conf_has_group(grp) )
        goto EXIT;

    udevdevice_blacklist_lut =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 0);

    gsize   count = 0;
    gchar **keys  = mce_conf_get_keys(grp, &count);

    for( gsize i = 0; i < count; ++i ) {
        bool blacklisted = mce_conf_get_bool(grp, keys[i], true);
        if( blacklisted ) {
            g_hash_table_replace(udevdevice_blacklist_lut,
                                 g_strdup(keys[i]),
                                 GINT_TO_POINTER(true));
        }
    }
    g_strfreev(keys);

EXIT:
    return;
}

/** Release device blacklist lookup table
 */
static void
udevdevice_quit_blacklist(void)
{
    if( udevdevice_blacklist_lut ) {
        g_hash_table_unref(udevdevice_blacklist_lut),
            udevdevice_blacklist_lut = 0;
    }
}

/** Check if device is blacklisted
 *
 * @param name device sysname
 *
 * @return true if device is blacklisted, false otherwise
 */
static bool
udevdevice_is_blacklisted(const char *name)
{
    bool blacklisted = false;

    if( udevdevice_blacklist_lut ) {
        gpointer val = g_hash_table_lookup(udevdevice_blacklist_lut, name);
        blacklisted = GPOINTER_TO_INT(val);
    }

    return blacklisted;
}

/** Create device object
 *
 * @param name device sysname
 *
 * @return device object
 */
static udevdevice_t *
udevdevice_create(const char *name)
{
    udevdevice_t *self = g_malloc0(sizeof *self);

    self->udd_name  = g_strdup(name);
    self->udd_props = g_hash_table_new_full(g_str_hash,
                                            g_str_equal,
                                            g_free,
                                            udevproperty_delete_cb);
    self->udd_full    = 0;
    self->udd_changed = false;

    return self;
}

/** Delete device object
 *
 * @param self  device object, or NULL
 */
static void
udevdevice_delete(udevdevice_t *self)
{
    if( self != 0 ) {
        g_hash_table_unref(self->udd_props);
        g_free(self->udd_name);
        g_free(self);
    }
}

/** Type agnostic callback for deleting device objects
 *
 * @param self  device object, or NULL
 */
static void
udevdevice_delete_cb(void *self)
{
    udevdevice_delete(self);
}

/** Get device object name
 *
 * @param self  device object
 *
 * @return device sysname
 */
static const char *
udevdevice_name(const udevdevice_t *self)
{
    return self->udd_name;
}

/** Get device object property
 *
 * @param self  device object
 * @param key   property name
 *
 * @return property object, or NULL if not found
 */
static udevproperty_t *
udevdevice_get_prop(udevdevice_t *self, const char *key)
{
    udevproperty_t *prop = g_hash_table_lookup(self->udd_props, key);
    return prop;
}

/** Add device object property
 *
 * @param self  device object
 * @param key   property name
 *
 * @return property object
 */
static udevproperty_t *
udevdevice_add_prop(udevdevice_t *self, const char *key)
{
    udevproperty_t *prop = udevdevice_get_prop(self, key);

    if( !prop ) {
        prop = udevproperty_create(self, key);
        g_hash_table_replace(self->udd_props,
                             g_strdup(key),
                             prop);
    }
    return prop;
}

/** Set device object property value
 *
 * @param self  device object
 * @param key   property name
 * @param val   property value
 *
 * @return true if battery state should be re-evaluated, false otherwise
 */
static bool
udevdevice_set_prop(udevdevice_t *self, const char *key, const char *val)
{
    udevproperty_t *prop = udevdevice_add_prop(self, key);
    bool rethink = udevproperty_set(prop, val);
    return rethink;
}

/** Get device object property value as string
 *
 * @param self  device object
 * @param key   property name
 * @param def   fallback value to return if property does not exist
 *
 * @return property value, or the given fallback values
 */
static const char *
udevdevice_get_str_prop(udevdevice_t *self, const char *key, const char *def)
{
    const char *val = 0;
    udevproperty_t *prop = udevdevice_get_prop(self, key);
    if( prop )
        val = udevproperty_get(prop);
    return val ?: def;
}

/** Get device object property value as integer
 *
 * @param self  device object
 * @param key   property name
 * @param def   fallback value to return if property does not exist
 *
 * @return property value, or the given fallback values
 */
static int
udevdevice_get_int_prop(udevdevice_t *self, const char *key, int def)
{
    const char *val = udevdevice_get_str_prop(self, key, 0);
    return val ? strtol(val, 0, 0) : def;
}

/** Update device object properties
 *
 * @param self  device object
 * @param dev   udev device object
 *
 * @return true if battery state should be re-evaluated, false otherwise
 */
static bool
udevdevice_refresh(udevdevice_t *self, struct udev_device *dev)
{
    bool rethink = false;

    /* Flag devices that have received change notifications from udev */
    const char *action = udev_device_get_action(dev);
    if( !g_strcmp0(action, "change") )
        self->udd_changed = true;

    for( struct udev_list_entry *iter =
         udev_device_get_properties_list_entry(dev);
         iter; iter = udev_list_entry_get_next(iter) ) {

        const char *key = udev_list_entry_get_name(iter);

        if( udevproperty_is_ignored(key) )
            continue;

        const char *val = udev_list_entry_get_value(iter);

        if( udevdevice_set_prop(self, key, val) )
            rethink = true;
    }
    return rethink;
}

/** Update mce style battery data based on device properties
 *
 * @param self    device object
 * @param mcebat  mce style battery data to update
 */
static void
udevdevice_evaluate(udevdevice_t *self, mcebat_t *mcebat)
{
    int present = udevdevice_get_int_prop(self, PROP_PRESENT, -1);
    int online  = udevdevice_get_int_prop(self, PROP_ONLINE, -1);

    if( present == -1 && online == -1 ) {
        mce_log(LL_DEBUG, "%s: ignored", udevdevice_name(self));
        goto EXIT;
    }

    int         capacity = udevdevice_get_int_prop(self, PROP_CAPACITY, -1);
    const char *status   = udevdevice_get_str_prop(self, PROP_STATUS, 0);

    if( status ) {
        /* Device is a battery.
         *
         * FIXME: There is a built-in assumption that there will be only
         *        one battery device - if there should be more than one,
         *        then the one that happens to be the last to be seen
         *        during g_hash_table_foreach() ends up being used.
         */

        /* mce level is udev capacity as-is
         */
        mcebat->battery_level = capacity;

        /* mce status is by default derived from udev capacity
         */
        if( capacity <= BATTERY_CAPACITY_UNDEF )
            mcebat->battery_status = BATTERY_STATUS_UNDEF;
        else if( capacity <= BATTERY_CAPACITY_EMPTY )
            mcebat->battery_status = BATTERY_STATUS_EMPTY;
        else if( capacity <= BATTERY_CAPACITY_LOW )
            mcebat->battery_status = BATTERY_STATUS_LOW;
        else
            mcebat->battery_status = BATTERY_STATUS_OK;

        /* udev status is "Unknown|Charging|Discharging|Not charging|Full"
         *
         * "Charging" and "Full" override capacity based mce battery status
         * evaluation above.
         *
         * How maintenance charging is reported after hitting battery
         * full varies from one device to another. To normalize behavior
         * and avoid repeated charging started notifications sequences
         * like "Full"->"Charging"->"Full"->... are compressed into
         * a single "Full" (untill charger is disconnected / battery level
         * makes significant enough drop).
         *
         * Also if battery device indicates that it is getting charged,
         * assume that a charger is connected.
         */
        if( !g_strcmp0(status, "Full") ) {
            mcebat->charger_state  = CHARGER_STATE_ON;
            mcebat->battery_status = BATTERY_STATUS_FULL;
            self->udd_full = true;
        }
        else if( !g_strcmp0(status, "Charging") ) {
            mcebat->charger_state  = CHARGER_STATE_ON;
            mcebat->battery_status = BATTERY_STATUS_OK;
            if( self->udd_full && capacity >= BATTERY_CAPACITY_FULL )
                mcebat->battery_status = BATTERY_STATUS_FULL;
            else
                self->udd_full = false;
        }
        else {
            self->udd_full = false;
        }
        mce_log(LL_DEBUG, "%s: battery @ cap=%d status=%s full=%d",
                udevdevice_name(self), capacity, status, self->udd_full);
    }
    else {
        /* Device is a charger.
         *
         * Assumption is that there are multiple charger devices like
         * "ac", "usb", "wireless", etc - and "a charger is connected"
         * needs to be indicated when at least one of those is "online".
         *
         * Also, it might be that initial enumeration yields devices
         * that will never notify changes. Assumption is that battery
         * devices are less likely to be like that, but charger devices
         * are ignored until a change notification about them is received.
         */

        bool active = false;

        if( self->udd_changed )
            active = (present == 1 || online == 1);

        if( active )
            mcebat->charger_state = CHARGER_STATE_ON;

        mce_log(LL_DEBUG, "%s: charger @ "
                "changed=%d present=%d online=%d -> active=%d",
                udevdevice_name(self),
                self->udd_changed,
                present, online, active);
    }

EXIT:
    return;
}

/** g_hash_table_foreach() compatible udevdevice_evaluate() wrapper callback
 *
 * @param key   (unused) device sysname as void pointer
 * @param value  device object as void pointer
 * @param aptr   mce battery data object as void pointer
 */
static void
udevdevice_evaluate_cb(gpointer key, gpointer value, gpointer aptr)
{
    (void)key;

    mcebat_t     *mcebat = aptr;
    udevdevice_t *self   = value;

    udevdevice_evaluate(self, mcebat);
}

/* ========================================================================= *
 * UDEVTRACKER
 * ========================================================================= */

/** Create udev power supply device tracking object
 *
 * @return tracker object
 */
static udevtracker_t *
udevtracker_create(void)
{
    udevtracker_t *self = g_malloc0(sizeof *self);

    self->udt_udev_handle   = 0;
    self->udt_udev_monitor  = 0;
    self->udt_udev_event_id = 0;
    self->udt_rethink_id    = 0;
    self->udt_devices       = g_hash_table_new_full(g_str_hash,
                                                    g_str_equal,
                                                    g_free,
                                                    udevdevice_delete_cb);
    return self;
}

/** Delete tracking object
 *
 * @param self   tracker object, or NULL
 */
static void
udevtracker_delete(udevtracker_t *self)
{
    if( self != 0 ) {
        /* Detach from udev notifications */
        udevtracker_stop(self);

        /* Flush cached device data */
        g_hash_table_unref(self->udt_devices),
            self->udt_devices = 0;

        /* Cancel pending state re-evaluation */
        udevtracker_cancel_rethink(self);

        g_free(self);
    }
}

/** Update mce battery state according to tracked udev state
 *
 * @param self   tracker object
 */
static void
udevtracker_rethink(udevtracker_t *self)
{
    udevtracker_cancel_rethink(self);

    /* Start with latest data that was pushed to datapipes */
    mcebat_t mcebat = mcebat_current;

    /* Give charger_state special treatment: Assume charger is
     * disconnect & rectify if any of the battery/charger devices
     * indicate charging activity. */
    mcebat.charger_state = CHARGER_STATE_OFF;

    g_hash_table_foreach(self->udt_devices, udevdevice_evaluate_cb, &mcebat);

    /* Sync to datapipes */
    mcebat_update(&mcebat);
}

/** Timer callback for delayed battery state evaluation
 *
 * @param aptr   tracker object as void pointer
 * @return G_SOURCE_REMOVE to stop timer from repeating
 */
static gboolean
udevtracker_rethink_cb(gpointer aptr)
{
    udevtracker_t *self = aptr;
    mce_log(LL_DEBUG, "battery state re-evaluation triggered");
    self->udt_rethink_id = 0;
    udevtracker_rethink(self);
    return G_SOURCE_REMOVE;
}

/** Shedule delayed battery state evaluation
 *
 * @param self  tracker object
 */
static void
udevtracker_cancel_rethink(udevtracker_t *self)
{
    if( self->udt_rethink_id ) {
        mce_log(LL_DEBUG, "battery state re-evaluation canceled");
        g_source_remove(self->udt_rethink_id), self->udt_rethink_id = 0;
    }
}

/** Cancle delayed battery state evaluation
 *
 * @param self  tracker object
 */
static void
udevtracker_schedule_rethink(udevtracker_t *self)
{
    if( !self->udt_rethink_id ) {
        self->udt_rethink_id = mce_wakelocked_idle_add(udevtracker_rethink_cb,
                                                       self);
        mce_log(LL_DEBUG, "battery state re-evaluation sheduled");
    }
}

/** Add device object to track
 *
 * @param self  tracker object
 * @param name  device sysname
 *
 * @return device object
 */
static udevdevice_t *
udevtracker_add_dev(udevtracker_t *self, const char *name)
{
    udevdevice_t *dev = g_hash_table_lookup(self->udt_devices, name);

    if( !dev ) {
        dev = udevdevice_create(name);
        g_hash_table_replace(self->udt_devices, g_strdup(name), dev);
    }
    return dev;
}

/** Update properties of tracked device
 *
 * @param self  tracker object
 * @param dev   udev device object
 */
static void
udevtracker_update_device(udevtracker_t *self, struct udev_device *dev)
{
    /* TODO: Currently it is assumed that we receive only
     *       "add" or "change" notifications for power
     *       supply devices after the initial enumeration.
     */

    const char   *sysname = udev_device_get_sysname(dev);
    const char   *action  = udev_device_get_action(dev);

    if( udevdevice_is_blacklisted(sysname) ) {
        /* Report blacklisted devices during initial enumeration */
        if( !action )
            mce_log(LL_DEBUG, "%s: is blacklisted", sysname);
        goto EXIT;
    }

    udevdevice_t *powerdev = udevtracker_add_dev(self, sysname);
    bool         rethink   = udevdevice_refresh(powerdev, dev);

    if( rethink )
        udevtracker_schedule_rethink(self);
EXIT:
    return;
}

/** Start udev device tracking
 *
 * @param self  tracker object
 *
 * @return true if tracking was successfully started, false otherwise
 */
static bool
udevtracker_start(udevtracker_t *self)
{
    struct udev_enumerate *udev_enum = 0;

    /* Already started? */
    if( self->udt_udev_event_id )
        goto EXIT;

    /* Make sure we start from clean state */
    udevtracker_stop(self);

    if( !(self->udt_udev_handle = udev_new()) )
        goto EXIT;

    /* Scan initial state */
    udev_enum = udev_enumerate_new(self->udt_udev_handle);
    udev_enumerate_add_match_subsystem(udev_enum, udevtracker_subsystem);
    udev_enumerate_scan_devices(udev_enum);

    for( struct udev_list_entry *iter =
         udev_enumerate_get_list_entry(udev_enum);
         iter ; iter = udev_list_entry_get_next(iter) ) {
        const char *path = udev_list_entry_get_name(iter);
        struct udev_device *dev =
            udev_device_new_from_syspath(self->udt_udev_handle, path);
        if( dev ) {
            udevtracker_update_device(self, dev);
            udev_device_unref(dev);
        }
    }

    /* Monitor changes */
    self->udt_udev_monitor =
        udev_monitor_new_from_netlink(self->udt_udev_handle, "udev");
    udev_monitor_filter_add_match_subsystem_devtype(self->udt_udev_monitor,
                                                    udevtracker_subsystem, 0);
    udev_monitor_enable_receiving(self->udt_udev_monitor);

    int fd = udev_monitor_get_fd(self->udt_udev_monitor);
    if( fd == -1 )
        goto EXIT;

    self->udt_udev_event_id = mce_io_add_watch(fd, false, G_IO_IN,
                                               udevtracker_event_cb, self);

EXIT:
    if( udev_enum )
        udev_enumerate_unref(udev_enum);

    if( !self->udt_udev_event_id )
        udevtracker_stop(self);

    return self->udt_udev_event_id != 0;
}

/** Stop udev device tracking
 *
 * @param self  tracker object
 */
static void
udevtracker_stop(udevtracker_t *self)
{
    if( self->udt_udev_event_id )
        g_source_remove(self->udt_udev_event_id), self->udt_udev_event_id = 0;

    if( self->udt_udev_monitor )
        udev_monitor_unref(self->udt_udev_monitor), self->udt_udev_monitor = 0;

    if( self->udt_udev_handle )
        udev_unref(self->udt_udev_handle), self->udt_udev_handle = 0;
}

/** I/O callback for receiving udev device changed notifications
 *
 * @param chn   (unused) glib io channel
 * @param cnd   reason for invoking the callback
 * @param aptr  tracker object as void pointer
 *
 * return G_SOURCE_CONTINUE to keep I/O watch alive, or
 *        G_SOURCE_REMOVE to disable it
 */
static gboolean
udevtracker_event_cb(GIOChannel  *chn, GIOCondition cnd, gpointer aptr)
{
    (void)chn;

    /* Deny suspending while handling timer wakeup */
    mce_wakelock_obtain(udevtracker_wakelock, -1);

    gboolean       result = G_SOURCE_REMOVE;
    udevtracker_t *self   = aptr;

    if( self->udt_udev_event_id == 0 ) {
        mce_log(LL_WARN, "stray udev wakeup");
        goto EXIT;
    }

    if( cnd & ~G_IO_IN ) {
        mce_log(LL_CRIT, "unexpected udev wakeup: %s",
                mce_io_condition_repr(cnd));
        goto EXIT;
    }

    struct udev_device *dev =
        udev_monitor_receive_device(self->udt_udev_monitor);
    if( dev ) {
        udevtracker_update_device(self, dev);
        udev_device_unref(dev);
    }

    result = G_SOURCE_CONTINUE;

EXIT:
    if( result != G_SOURCE_CONTINUE && self->udt_udev_event_id != 0 ) {
        mce_log(LL_CRIT, "disabling udev io watch");
        self->udt_udev_event_id = 0;
        udevtracker_stop(self);
    }

    mce_wakelock_release(udevtracker_wakelock);

    return result;
}

/* ========================================================================= *
 * G_MODULE
 * ========================================================================= */

/** Init function for the battery and charger module
 *
 * @param module (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    udevdevice_init_blacklist();
    udevproperty_init_types();

    udevtracker_object = udevtracker_create();

    if( !udevtracker_start(udevtracker_object) )
        goto EXIT;

    mce_log(LL_DEBUG, "%s: loaded", MODULE_NAME);

EXIT:
    return NULL;
}

/** Exit function for the battery and charger module
 *
 * @param module (not used)
 */
G_MODULE_EXPORT void g_module_unload(GModule *module)
{
    (void)module;

    udevtracker_delete(udevtracker_object), udevtracker_object = 0;

    udevproperty_quit_types();
    udevdevice_quit_blacklist();

    mce_log(LL_DEBUG, "%s: unloaded", MODULE_NAME);
}
