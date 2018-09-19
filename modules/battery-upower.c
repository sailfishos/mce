/**
 * @file battery-upower.c
 * Battery module -- this implements battery and charger logic for MCE
 * <p>
 * Copyright (C) 2013 Jolla Ltd.
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
#include "../mce-log.h"
#include "../mce-dbus.h"

#include <stdlib.h>
#include <string.h>

#include <gmodule.h>

#if 0 // DEBUG: make all logging from this module "critical"
# undef mce_log
# define mce_log(LEV, FMT, ARGS...) \
        mce_log_file(LL_CRIT, __FILE__, __FUNCTION__, FMT , ## ARGS)
#endif

/* ========================================================================= *
 * CONFIGURATION
 * ========================================================================= */

/** Delay from 1st property change to state machine update; [ms] */
#define UPDATE_DELAY 100

/** Whether to support legacy pattery low led pattern; nonzero for yes */
#define SUPPORT_BATTERY_LOW_LED_PATTERN 0

/** Well known D-Bus service name for upowerd */
#define UPOWER_SERVICE "org.freedesktop.UPower"

/** UPower D-Bus interface name */
#define UPOWER_INTERFACE "org.freedesktop.UPower"

/** UPower D-Bus object path */
#define UPOWER_PATH      "/org/freedesktop/UPower"

/** Upower Device D-Bus interface name */
#define UPOWER_INTERFACE_DEVICE "org.freedesktop.UPower.Device"

/* ========================================================================= *
 * union uval_t
 * ========================================================================= */

/** Placeholder for any basic dbus data type */
typedef union
{
    dbus_int16_t i16;
    dbus_int32_t i32;
    dbus_int64_t i64;

    dbus_uint16_t u16;
    dbus_uint32_t u32;
    dbus_uint64_t u64;

    dbus_bool_t   b;
    unsigned char o;
    char         *s;
    double        d;

} uval_t;

/* ========================================================================= *
 * struct MceBattery holds battery data available via UPower
 * ========================================================================= */

/** Values for upower device State property */
enum
{
    UPOWER_STATE_UNKNOWN           = 0,
    UPOWER_STATE_CHARGING          = 1,
    UPOWER_STATE_DISCHARGING       = 2,
    UPOWER_STATE_EMPTY             = 3,
    UPOWER_STATE_FULLY_CHARGED     = 4,
    UPOWER_STATE_PENDING_CHARGE    = 5,
    UPOWER_STATE_PENDING_DISCHARGE = 6,
};

/** Values for upower device Type property */
enum
{
    UPOWER_TYPE_UNKNOWN    = 0,
    UPOWER_TYPE_LINE_POWER = 1,
    UPOWER_TYPE_BATTERY    = 2,
    UPOWER_TYPE_UPS        = 3,
    UPOWER_TYPE_MONITOR    = 4,
    UPOWER_TYPE_MOUSE      = 5,
    UPOWER_TYPE_KEYBOARD   = 6,
    UPOWER_TYPE_PDA        = 7,
    UPOWER_TYPE_PHONE      = 8,
};

/** Values for upower device Technology property */
enum
{
    UPOWER_TECHNOLOGY_UNKNOWN                = 0,
    UPOWER_TECHNOLOGY_LITHIUM_ION            = 1,
    UPOWER_TECHNOLOGY_LITHIUM_POLYMER        = 2,
    UPOWER_TECHNOLOGY_LITHIUM_IRON_PHOSPHATE = 3,
    UPOWER_TECHNOLOGY_LEAD_ACID              = 4,
    UPOWER_TECHNOLOGY_NICKEL_CADMIUM         = 5,
    UPOWER_TECHNOLOGY_NICKEL_METAL_HYDRIDE   = 6,
};

/** Battery properties available via upower */
typedef struct
{
    int Percentage;
    int State;
} UpowerBattery;

static void        upowbat_init(void);

/* ========================================================================= *
 * struct MceBattery holds mce legacy compatible battery data
 * ========================================================================= */

/** Battery properties in mce statemachine compatible form */
typedef struct
{
    /** Battery charge percentage; for use with battery_level_pipe */
    int         level;

    /** Battery FULL/OK/LOW/EMPTY; for use with battery_status_pipe */
    int         status;

    /** Charger connected; for use with charger_state_pipe */
    charger_state_t charger;
} MceBattery;

static void     mcebat_init(void);
static void     mcebat_update_from_upowbat(void);
static gboolean mcebat_update_cb(gpointer user_data);
static void     mcebat_update_cancel(void);
static void     mcebat_update_schedule(void);

/* ========================================================================= *
 * UPOWER PROPERTY
 * ========================================================================= */

/** UPower property object */
typedef struct
{
    char   *p_key;
    int     p_type;
    uval_t  p_val;
} uprop_t;

/** Invalidate property
 *
 * @param self property
 */
static void uprop_set_invalid(uprop_t *self)
{
    switch( self->p_type ) {
    case DBUS_TYPE_INVALID:
        break;

    case DBUS_TYPE_BYTE:
    case DBUS_TYPE_BOOLEAN:
    case DBUS_TYPE_INT16:
    case DBUS_TYPE_UINT16:
    case DBUS_TYPE_INT32:
    case DBUS_TYPE_UINT32:
    case DBUS_TYPE_INT64:
    case DBUS_TYPE_UINT64:
    case DBUS_TYPE_DOUBLE:
        break;

    case DBUS_TYPE_STRING:
    case DBUS_TYPE_OBJECT_PATH:
    case DBUS_TYPE_SIGNATURE:
        free(self->p_val.s), self->p_val.s = 0;
        break;

    default:
    case DBUS_TYPE_UNIX_FD:
    case DBUS_TYPE_ARRAY:
    case DBUS_TYPE_VARIANT:
    case DBUS_TYPE_STRUCT:
    case DBUS_TYPE_DICT_ENTRY:
        break;
    }
    self->p_type = DBUS_TYPE_INVALID;
}

/** Get property value from dbus message iterator
 *
 * @param self property
 * @param iter dbus message parse position
 */
static bool uprop_set_from_iter(uprop_t *self, DBusMessageIter *iter)
{
    bool res  = false;
    int  type = dbus_message_iter_get_arg_type(iter);

    uprop_set_invalid(self);

    if( dbus_type_is_basic(type) ) {
        dbus_message_iter_get_basic(iter, &self->p_val);
        switch( dbus_message_iter_get_arg_type(iter) ) {
        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH:
        case DBUS_TYPE_SIGNATURE:
            self->p_val.s = strdup(self->p_val.s);
            break;
        default:
            break;
        }
        self->p_type = type, res = true;
    }
    return res;
}

/** Get property value as integer number
 *
 * @param self property
 * @param val  where to store the number
 *
 * @return true on success, false on failure
 */
static bool uprop_get_int(const uprop_t *self, int *val)
{
    bool res = true;
    switch( self->p_type ) {
    case DBUS_TYPE_BYTE:    *val = (int)self->p_val.o;         break;
    case DBUS_TYPE_BOOLEAN: *val = (int)self->p_val.b;         break;
    case DBUS_TYPE_INT16:   *val = (int)self->p_val.i16;       break;
    case DBUS_TYPE_UINT16:  *val = (int)self->p_val.u16;       break;
    case DBUS_TYPE_INT32:   *val = (int)self->p_val.i32;       break;
    case DBUS_TYPE_UINT32:  *val = (int)self->p_val.u32;       break;
    case DBUS_TYPE_INT64:   *val = (int)self->p_val.i64;       break;
    case DBUS_TYPE_UINT64:  *val = (int)self->p_val.u64;       break;
    case DBUS_TYPE_DOUBLE:  *val = (int)(self->p_val.d + 0.5); break;
    default:
        res = false;
        break;
    }
    return res;
}

/** Get property value as string
 *
 * @param self property
 * @param val  where to store the string
 *
 * @return true on success, false on failure
 */
static bool uprop_get_string(const uprop_t *self, const char **val)
{
    bool res = true;
    switch( self->p_type ) {
    case DBUS_TYPE_STRING:
    case DBUS_TYPE_OBJECT_PATH:
    case DBUS_TYPE_SIGNATURE:
        *val = self->p_val.s;
        break;
    default:
        res = false;
        break;
    }
    return res;
}

/** Create propety object
 *
 * @param key property name
 *
 * @return property object
 */
static uprop_t * uprop_create(const char *key)
{
    uprop_t *self = calloc(1, sizeof *self);

    self->p_key  = strdup(key);;
    self->p_type = DBUS_TYPE_INVALID;

    return self;
}

/** Delete propety object
 *
 * @param self property object
 */
static void uprop_delete(uprop_t *self)
{
    if( self != 0 ) {
        uprop_set_invalid(self);
        free(self->p_key);
        free(self);
    }
}

/** Type agnostic delete propety object callback function
 *
 * @param self property object as void pointer
 */
static void uprop_delete_cb(void *self)
{
    uprop_delete(self);
}

/* ========================================================================= *
 * SET OF UPOWER PROPERTIES
 * ========================================================================= */

/** UPower device object */
typedef struct updev_t
{
    char  *d_path;
    GList *d_prop;
} updev_t;

/** Create UPower device object
 *
 * @param path dbus object path
 *
 * @return device object
 */
static updev_t *updev_create(const char *path)
{
    updev_t *self = calloc(1, sizeof *self);

    self->d_path = strdup(path);
    self->d_prop = 0;

    return self;
}

/** Delete UPower device object
 *
 * @param self device object
 */
static void updev_delete(updev_t *self)
{
    if( self != 0 ) {
        g_list_free_full(self->d_prop, uprop_delete_cb);
        free(self->d_path);
        free(self);
    }
}

/** Type agnostic delete UPower device object callback
 *
 * @param self device object as void pointer
 */
static void updev_delete_cb(void *self)
{
    updev_delete(self);
}

/** Mark all device object properties as invalid
 *
 * @param self device object
 */
static void updev_set_invalid_all(updev_t *self)
{
    for( GList *now = self->d_prop; now; now = g_list_next(now) )
        uprop_set_invalid(now->data);
}

/** Find device object property
 *
 * @param self device object
 * @param key  property name
 *
 * @return property object, or NULL
 */
static uprop_t *updev_get_prop(const updev_t *self, const char *key)
{
    uprop_t *res = 0;
    for( GList *now = self->d_prop; now; now = g_list_next(now) ) {
        uprop_t *prop = now->data;
        if( strcmp(prop->p_key, key) )
            continue;
        res = prop;
        break;
    }
    return res;
}

/** Find or create device object property
 *
 * @param self device object
 * @param key  property name
 *
 * @return property object
 */
static uprop_t *updev_add_prop(updev_t *self, const char *key)
{
    uprop_t *res = updev_get_prop(self, key);
    if( !res ) {
        res = uprop_create(key);
        self->d_prop = g_list_append(self->d_prop, res);
    }
    return res;
}

/** Get device object property value as integer number
 *
 * @param self device object
 * @param key  property name
 * @param val  where to store the integer number
 *
 * @return true on success, otherwise false
 */
static bool updev_get_int(const updev_t *self, const char *key, int *val)
{
    bool res = false;
    uprop_t *prop = updev_get_prop(self, key);
    if( prop )
        res = uprop_get_int(prop, val);
    return res;
}

/** Get device object property value as string
 *
 * @param self device object
 * @param key  property name
 * @param val  where to store the string
 *
 * @return true on success, otherwise false
 */
static bool updev_get_string(const updev_t *self, const char *key,
                             const char **val)
{
    bool res = false;
    uprop_t *prop = updev_get_prop(self, key);
    if( prop )
        res = uprop_get_string(prop, val);
    return res;
}

/** Device object is battery predicate
 *
 * @param self device object
 *
 * @return true if device object is battery, false otherwise
 */
static bool updev_is_battery(const updev_t *self)
{
    bool is_battery = false;

    if( !self )
        goto EXIT;

    const char *native_path = 0;

    if( !updev_get_string(self, "NativePath", &native_path) )
        goto EXIT;

    if( !native_path || strcmp(native_path, "battery") )
        goto EXIT;

    is_battery = true;

EXIT:
    return is_battery;
}

/* ========================================================================= *
 * LIST OF UPOWER DEVICES
 * ========================================================================= */

/** List of UPower device objects */
static GList *devlist = 0;

/** Find device object
 *
 * @param path dbus object path
 *
 * @return device object, or NULL if not found
 */
static updev_t *devlist_get_dev(const char *path)
{
    updev_t *res = 0;
    for( GList *now = devlist; now; now = g_list_next(now) ) {
        updev_t *dev = now->data;
        if( strcmp(dev->d_path, path) )
            continue;
        res = dev;
        break;
    }
    return res;
}

/** Find 1st battery device object
 *
 * @return device object, or NULL if not found
 */
static updev_t *devlist_get_dev_battery(void)
{
    updev_t *res = 0;
    for( GList *now = devlist; now; now = g_list_next(now) ) {
        updev_t *dev = now->data;
        if( !updev_is_battery(dev) )
            continue;
        res = dev;
        break;
    }
    return res;
}

/** Find or create device object
 *
 * @param path dbus object path
 *
 * @return device object
 */
static updev_t *devlist_add_dev(const char *path)
{
    updev_t *res = devlist_get_dev(path);
    if( !res ) {
        res = updev_create(path);
        devlist = g_list_append(devlist, res);
    }
    return res;
}

/** Forget device object
 *
 * @param path dbus object path
 */
static void devlist_rem_dev(const char *path)
{
    for( GList *now = devlist; now; now = g_list_next(now) ) {
        updev_t *dev = now->data;
        if( strcmp(dev->d_path, path) )
            continue;

        if( updev_is_battery(dev) )
            mcebat_update_schedule();

        devlist = g_list_remove_link(devlist, now);
        updev_delete(dev);
        break;
    }
}

/** Forget all device objects
 */
static void devlist_rem_dev_all(void)
{
    g_list_free_full(devlist, updev_delete_cb);
    devlist = 0;
}

/* ========================================================================= *
 * struct UpowerBattery
 * ========================================================================= */

/** UPower battery state data */
static UpowerBattery upowbat;

/** Initialize UPower battery state data
 */
static void
upowbat_init(void)
{
    memset(&upowbat, 0, sizeof upowbat);
    upowbat.Percentage = 50;
    upowbat.State = UPOWER_STATE_UNKNOWN;
}

/** Update UPower battery state data
 */
static void
upowbat_update(void)
{
    updev_t *dev = devlist_get_dev_battery();
    if( dev ) {
        int val = 0;
        if( updev_get_int(dev, "Percentage", &val) && upowbat.Percentage != val ) {
            mce_log(LL_DEBUG, "Percentage: %d -> %d", upowbat.Percentage, val);
            upowbat.Percentage = val;
        }
        if( updev_get_int(dev, "State", &val) && upowbat.State != val ) {
            mce_log(LL_DEBUG, "State: %d -> %d", upowbat.State, val);
            upowbat.State = val;
        }
    }
}

/* ========================================================================= *
 * struct MceBattery
 * ========================================================================= */

/** Timer for processing battery status changes */
static guint mcebat_update_id = 0;

/** Current battery status in mce legacy compatible form */
static MceBattery mcebat;

/** Provide intial guess of mce battery status
 */
static void
mcebat_init(void)
{
    memset(&mcebat, 0, sizeof mcebat);
    mcebat.level   = 50;
    mcebat.status  = BATTERY_STATUS_UNDEF;
    mcebat.charger = CHARGER_STATE_UNDEF;
}

/** Update mce battery status from UPower battery data
 */
static void
mcebat_update_from_upowbat(void)
{
    mcebat.level   = upowbat.Percentage;
    mcebat.status  = BATTERY_STATUS_OK;
    mcebat.charger = CHARGER_STATE_OFF;

    // FIXME: hardcoded 5% as low battery limit
    if( mcebat.level < 5 )
        mcebat.status = BATTERY_STATUS_LOW;

    switch( upowbat.State ) {
    case UPOWER_STATE_UNKNOWN:
        mcebat.charger = CHARGER_STATE_UNDEF;
        break;

    case UPOWER_STATE_CHARGING:
        mcebat.charger = CHARGER_STATE_ON;
        break;

    case UPOWER_STATE_DISCHARGING:
        break;

    case UPOWER_STATE_EMPTY:
        mcebat.status = BATTERY_STATUS_EMPTY;
        break;

    case UPOWER_STATE_FULLY_CHARGED:
        mcebat.status  = BATTERY_STATUS_FULL;
        mcebat.charger = CHARGER_STATE_ON;
        break;

    case UPOWER_STATE_PENDING_CHARGE:
        mcebat.charger = CHARGER_STATE_ON;
        break;

    case UPOWER_STATE_PENDING_DISCHARGE:
        break;

    default:
        break;
    }
}

/** Process accumulated upower battery status changes
 *
 * @param user_data (not used)
 *
 * @return FALSE (to stop timer from repeating)
 */
static gboolean
mcebat_update_cb(gpointer user_data)
{
    (void)user_data;

    if( !mcebat_update_id )
        return FALSE;

    mce_log(LL_INFO, "----( state machine )----");

    /* Get a copy of current status */
    MceBattery prev = mcebat;

    /* Update from UPower based information */
    upowbat_update();
    mcebat_update_from_upowbat();

    /* Process changes */
    if( mcebat.charger != prev.charger ) {
        mce_log(LL_INFO, "charger: %s -> %s",
                charger_state_repr(prev.charger),
                charger_state_repr(mcebat.charger));

        /* Charger connected state */
        datapipe_exec_full(&charger_state_pipe, GINT_TO_POINTER(mcebat.charger),
                           DATAPIPE_CACHE_INDATA);

        /* Charging led pattern */
        if( mcebat.charger == CHARGER_STATE_ON ) {
            datapipe_exec_output_triggers(&led_pattern_activate_pipe,
                                          MCE_LED_PATTERN_BATTERY_CHARGING);
        }
        else {
            datapipe_exec_output_triggers(&led_pattern_deactivate_pipe,
                                          MCE_LED_PATTERN_BATTERY_CHARGING);
        }

        /* Generate activity */
        datapipe_exec_full(&inactivity_event_pipe, GINT_TO_POINTER(FALSE),
                           DATAPIPE_CACHE_OUTDATA);
    }

    if( mcebat.status != prev.status ) {
        mce_log(LL_INFO, "status: %d -> %d", prev.status, mcebat.status);

        /* Battery full led pattern */
        if( mcebat.status == BATTERY_STATUS_FULL ) {
            datapipe_exec_output_triggers(&led_pattern_activate_pipe,
                                          MCE_LED_PATTERN_BATTERY_FULL);
        }
        else {
            datapipe_exec_output_triggers(&led_pattern_deactivate_pipe,
                                          MCE_LED_PATTERN_BATTERY_FULL);
        }

#if SUPPORT_BATTERY_LOW_LED_PATTERN
        /* Battery low led pattern */
        if( mcebat.status == BATTERY_STATUS_LOW ||
            mcebat.status == BATTERY_STATUS_EMPTY ) {
            datapipe_exec_output_triggers(&led_pattern_activate_pipe,
                                          MCE_LED_PATTERN_BATTERY_LOW);
        }
        else {
            datapipe_exec_output_triggers(&led_pattern_deactivate_pipe,
                                          MCE_LED_PATTERN_BATTERY_LOW);
        }
#endif /* SUPPORT_BATTERY_LOW_LED_PATTERN */

        /* Battery charge state */
        datapipe_exec_full(&battery_status_pipe,
                           GINT_TO_POINTER(mcebat.status),
                           DATAPIPE_CACHE_INDATA);

    }

    if( mcebat.level != prev.level ) {
        mce_log(LL_INFO, "level: %d -> %d", prev.level, mcebat.level);

        /* Battery charge percentage */
        datapipe_exec_full(&battery_level_pipe,
                           GINT_TO_POINTER(mcebat.level),
                           DATAPIPE_CACHE_INDATA);
    }

    /* Clear the timer id and do not repeat */
    return mcebat_update_id = 0, FALSE;
}

/** Cancel processing of upower battery status changes
 */
static void
mcebat_update_cancel(void)
{
    if( mcebat_update_id )
        g_source_remove(mcebat_update_id), mcebat_update_id = 0;
}

/** Initiate delayed processing of upower battery status changes
 */
static void
mcebat_update_schedule(void)
{
    if( !mcebat_update_id )
        mcebat_update_id = g_timeout_add(UPDATE_DELAY, mcebat_update_cb, 0);
}

/* ========================================================================= *
 * UPOWER IPC
 * ========================================================================= */

/** Handle reply to async UPower device properties query
 */
static void xup_properties_get_all_cb(DBusPendingCall *pc, void *aptr)
{
    (void)aptr;

    bool          res  = false;
    DBusError     err  = DBUS_ERROR_INIT;
    DBusMessage  *rsp  = 0;
    const char   *path = aptr;
    updev_t      *dev  = devlist_add_dev(path);

    mce_log(LL_INFO, "path = %s", path);

    DBusMessageIter body, arr, dic, var;

    updev_set_invalid_all(dev);

    if( !(rsp = dbus_pending_call_steal_reply(pc)) )
        goto EXIT;

    if( dbus_set_error_from_message(&err, rsp) ) {
        mce_log(LL_ERR, "als lux error reply: %s: %s",
                err.name, err.message);
        goto EXIT;
    }

    if( !dbus_message_iter_init(rsp, &body) )
        goto EXIT;

    if( dbus_message_iter_get_arg_type(&body) != DBUS_TYPE_ARRAY )
        goto EXIT;
    dbus_message_iter_recurse(&body, &arr);
    dbus_message_iter_next(&body);

    while( dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY ) {
        dbus_message_iter_recurse(&arr, &dic);
        dbus_message_iter_next(&arr);

        const char *key = 0;
        if( dbus_message_iter_get_arg_type(&dic) != DBUS_TYPE_STRING )
            goto EXIT;
        dbus_message_iter_get_basic(&dic, &key);
        dbus_message_iter_next(&dic);
        if( !key )
            goto EXIT;

        if( dbus_message_iter_get_arg_type(&dic) != DBUS_TYPE_VARIANT )
            goto EXIT;
        dbus_message_iter_recurse(&dic, &var);
        dbus_message_iter_next(&dic);

        uprop_t *prop = updev_add_prop(dev, key);
        uprop_set_from_iter(prop, &var);
    }

    mce_log(LL_DEBUG, "%s is %sBATTERY", path,
            updev_is_battery(dev) ? "" : "NOT ");

    if( updev_is_battery(dev) )
        mcebat_update_schedule();

    res = true;

EXIT:
    if( !res ) mce_log(LL_WARN, "failed to parse reply");

    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

/** Start async UPower device properties query
 */
static void xup_properties_get_all(const char *path)
{
    DBusConnection  *bus = 0;
    DBusMessage     *req = 0;
    DBusPendingCall *pc  = 0;
    const char      *arg = UPOWER_INTERFACE_DEVICE;

    if( !(bus = dbus_connection_get()) )
        goto EXIT;

    req = dbus_message_new_method_call(UPOWER_SERVICE,
                                       path,
                                       DBUS_INTERFACE_PROPERTIES,
                                       "GetAll");
    if( !req )
        goto EXIT;

    if( !dbus_message_append_args(req,
                                  DBUS_TYPE_STRING, &arg,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    if( !dbus_connection_send_with_reply(bus, req, &pc, -1) )
        goto EXIT;

    if( !pc )
        goto EXIT;

    mce_dbus_pending_call_blocks_suspend(pc);

    if( !dbus_pending_call_set_notify(pc, xup_properties_get_all_cb,
                                      strdup(path), free) )
        goto EXIT;

EXIT:
    if( pc )  dbus_pending_call_unref(pc);
    if( req ) dbus_message_unref(req);
    if( bus ) dbus_connection_unref(bus);
}

/** Handle reply to async UPower device enumeration query
 */
static void xup_enumerate_devices_cb(DBusPendingCall *pc, void *aptr)
{
    (void)aptr;

    bool          res = false;
    DBusError     err = DBUS_ERROR_INIT;
    DBusMessage  *rsp = 0;

    char **vec = 0;
    int    cnt = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) )
        goto EXIT;

    if( dbus_set_error_from_message(&err, rsp) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    if( !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_ARRAY,
                               DBUS_TYPE_OBJECT_PATH, &vec, &cnt,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    for( int i = 0; i < cnt; ++i ) {
        mce_log(LL_DEBUG, "[%d] '%s'", i, vec[i]);
        xup_properties_get_all(vec[i]);
    }

    res = true;

EXIT:
    if( !res ) mce_log(LL_WARN, "failed to parse reply");

    dbus_free_string_array(vec);
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

/** Start async UPower device enumeration query
 */
static void xup_enumerate_devices(void)
{
    dbus_send(UPOWER_SERVICE, UPOWER_PATH, UPOWER_INTERFACE,
              "EnumerateDevices", xup_enumerate_devices_cb,
              DBUS_TYPE_INVALID);
}

/** Handle addition of UPowerd device object
 */
static gboolean xup_device_added_cb(DBusMessage *const msg)
{
    DBusError   err  = DBUS_ERROR_INIT;
    const char *path = 0;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &path,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "dev = %s", path);
    xup_properties_get_all(path);

EXIT:
    dbus_error_free(&err);
    return TRUE;
}

/** Handle UPowerd device object property changes
 */
static gboolean xup_device_changed_cb(DBusMessage *const msg)
{
    DBusError   err  = DBUS_ERROR_INIT;
    const char *path = 0;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &path,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "dev = %s", path);
    updev_t *dev = devlist_get_dev(path);

    /* Get properties if we know that it is battery, or
     * if we do not know what it is yet */
    if( !dev || updev_is_battery(dev) )
        xup_properties_get_all(path);

EXIT:
    dbus_error_free(&err);
    return TRUE;
}

/** Handle removal of UPowerd device object
 */
static gboolean xup_device_removed_cb(DBusMessage *const msg)
{
    DBusError   err  = DBUS_ERROR_INIT;
    const char *path = 0;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &path,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "dev = %s", path);
    devlist_rem_dev(path);

EXIT:
    dbus_error_free(&err);
    return TRUE;
}

/** Handle UPowerd dbus name ownership change signal
 */
static gboolean xup_name_owner_cb(DBusMessage *const msg)
{
    DBusError   err  = DBUS_ERROR_INIT;

    const char *service   = 0;
    const char *old_owner = 0;
    const char *new_owner = 0;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &service,
                               DBUS_TYPE_STRING, &old_owner,
                               DBUS_TYPE_STRING, &new_owner,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "%s: %s", err.name, err.message);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "upowerd %s", *new_owner ? "running" : "stopped");

    /* Flush cached device object properties when upowerd
     * stops or starts */
    devlist_rem_dev_all();

    /* If upowerd started up, get fresh list of device paths */
    if( *new_owner )
        xup_enumerate_devices();

EXIT:
    dbus_error_free(&err);
    return TRUE;
}

/** Module name */
#define MODULE_NAME "battery_upower"

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

/** Array of dbus message handlers */
static mce_dbus_handler_t battery_upower_dbus_handlers[] =
{
    /* signals */
    {
        .interface = UPOWER_INTERFACE,
        .name      = "DeviceAdded",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xup_device_added_cb,
    },
    {
        .interface = UPOWER_INTERFACE,
        .name      = "DeviceChanged",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xup_device_changed_cb,
    },
    {
        .interface = UPOWER_INTERFACE,
        .name      = "DeviceRemoved",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xup_device_removed_cb,
    },

    {
        .interface = DBUS_INTERFACE_DBUS,
        .name      = "NameOwnerChanged",
        .rules     = "arg0='"UPOWER_SERVICE"'",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = xup_name_owner_cb,
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Add dbus handlers
 */
static void mce_battery_init_dbus(void)
{
    mce_dbus_handler_register_array(battery_upower_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mce_battery_quit_dbus(void)
{
    mce_dbus_handler_unregister_array(battery_upower_dbus_handlers);
}

/** Init function for the battery and charger module
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    /* reset data used by the state machine */
    mcebat_init();
    upowbat_init();

    /* Add dbus handlers */
    mce_battery_init_dbus();

    /* Initiate available device objects query.
     * Properties will be probed when reply arrives.
     * This will start upowerd if not already running.
     */
    xup_enumerate_devices();

    return NULL;
}

/** Exit function for the battery and charger module
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
    (void)module;

    /* Remove dbus handlers */
    mce_battery_quit_dbus();

    devlist_rem_dev_all();
    mcebat_update_cancel();
}
