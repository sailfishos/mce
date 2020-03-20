/**
 * @file battery-udev.c
 * Battery module -- this implements battery and charger logic for MCE
 * <p>
 * Copyright (c) 2018 - 2020 Jolla Ltd.
 * Copyright (c) 2019 - 2020 Open Mobile Platform LLC.
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
#include "../mce-dbus.h"
#include "../mce-wakelock.h"

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

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
#define PROP_PRESENT   "POWER_SUPPLY_PRESENT"
#define PROP_ONLINE    "POWER_SUPPLY_ONLINE"
#define PROP_CAPACITY  "POWER_SUPPLY_CAPACITY"
#define PROP_STATUS    "POWER_SUPPLY_STATUS"
#define PROP_REAL_TYPE "POWER_SUPPLY_REAL_TYPE"
#define PROP_TYPE      "POWER_SUPPLY_TYPE"

/** INI-file group for blacklisting device properties */
#define MCE_CONF_BATTERY_UDEV_PROPERTY_BLACKLIST_GROUP "BatteryUDevPropertyBlacklist"

/** INI-file group for blacklisting devices */
#define MCE_CONF_BATTERY_UDEV_DEVICE_BLACKLIST_GROUP   "BatteryUDevDeviceBlacklist"

/** INI-file group for configuring charger types */
#define MCE_CONF_BATTERY_UDEV_DEVICE_CHARGERTYPE_GROUP "BatteryUDevChargerTypes"

/** INI-file group for miscellaneous settings */
#define MCE_CONF_BATTERY_UDEV_SETTINGS_GROUP           "BatteryUDevSettings"

/** Setting for forced refresh on udev notify event */
#define MCE_CONF_BATTERY_UDEV_REFRESH_ON_NOTIFY        "RefreshOnNotify"
#define DEFAULT_BATTERY_UDEV_REFRESH_ON_NOTIFY         false

/** Setting for forced refresh on system heartbeat */
#define MCE_CONF_BATTERY_UDEV_REFRESH_ON_HEARTBEAT     "RefreshOnHeartbeat"
#define DEFAULT_BATTERY_UDEV_REFRESH_ON_HEARTBEAT      true

/** Delay between udev notifications and battery state evaluation
 *
 * The purpose is to increase chances of getting battery and
 * charger notifications handled in one go and thus decrease
 * changes of getting false positive battery full blips.
 */
#define BATTERY_REEVALUATE_DELAY 50 // [ms]

/** Delay between udev notifications and refreshing all devices
 *
 * Some kernels do better job with udev notifications than
 * others... if we get notification about any device node
 * that is used for battery / charger tracking, all properties
 * of all tracked devices are checked after brief delay
 *
 * As this is relatively costly operation -> the wait should
 * be long enought to cover all related notifications that
 * kernel will send.
 *
 * As suspend is blocked during the wait -> the wait should
 * be as short as possible.
 *
 * As delay affects ui responce to physical actions taken by
 * user (e.g. detaching charger cable) -> the wait should be
 * in "perceived immediate" time span.
 *
 * As a compromize, relatively short delay is used and the timer
 * is restarted whenever we get udev notifications -> a burst of
 * udev activity leads to only one evaluation round and suspend
 * blocking ends soon after udev goes idle.
 */
#define DEVICES_REFRESH_DELAY 250

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

    /** Battery UNKNOWN|CHARGING|DISCHARGING|NOT_CHARGING|FULL"*/
    battery_state_t  battery_state;

    /** Charger connected; for use with charger_state_pipe */
    charger_state_t  charger_state;

    /** Charger type; for tweaking UI behavior */
    charger_type_t   charger_type;
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

    /** Flag for: The latest evaluated status was "Charging" */
    bool        udd_charging;
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
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * DBUS_HANDLERS
 * ------------------------------------------------------------------------- */

#ifdef ENABLE_BATTERY_SIMULATION
static void     mcebat_dbus_remove_client          (const char *dbus_name);
static gboolean mcebat_dbus_client_removed_cb      (DBusMessage *const msg);
static bool     mcebat_dbus_add_client             (const char *dbus_name);
static void     mcebat_dbus_evaluate_battery_status(void);
static gboolean mcebat_dbus_charger_type_req_cb    (DBusMessage *const msg);
static gboolean mcebat_dbus_charger_state_req_cb   (DBusMessage *const msg);
static gboolean mcebat_dbus_battery_level_req_cb   (DBusMessage *const msg);
#endif // ENABLE_BATTERY_SIMULATION
static void     mcebat_dbus_init                   (void);
static void     mcebat_dbus_quit                   (void);

/* ------------------------------------------------------------------------- *
 * MCEBAT
 * ------------------------------------------------------------------------- */

static void     mcebat_update         (void);
static gboolean mcebat_init_tracker_cb(gpointer aptr);
static void     mcebat_init_settings  (void);

/* ------------------------------------------------------------------------- *
 * UDEVPROPERTY
 * ------------------------------------------------------------------------- */

static void             udevproperty_init_types (void);
static void             udevproperty_quit_types (void);
static property_type_t  udevproperty_lookup_type(const char *key);
static bool             udevproperty_is_used    (const char *key);
static bool             udevproperty_is_ignored (const char *key);
static udevproperty_t  *udevproperty_create     (udevdevice_t *dev, const char *key);
static void             udevproperty_delete     (udevproperty_t *self);
static void             udevproperty_delete_cb  (void *self);
static const char      *udevproperty_key        (const udevproperty_t *self);
static const char      *udevproperty_get        (const udevproperty_t *self);
static bool             udevproperty_set        (udevproperty_t *self, const char *val);

/* ------------------------------------------------------------------------- *
 * UDEVDEVICE
 * ------------------------------------------------------------------------- */

static battery_state_t  udevdevice_lookup_battery_state(const char *status);
static charger_type_t   udevdevice_lookup_charger_type (const char *name);
static void             udevdevice_init_chargertype    (void);
static void             udevdevice_quit_chargertype    (void);
static void             udevdevice_init_blacklist      (void);
static void             udevdevice_quit_blacklist      (void);
static bool             udevdevice_is_blacklisted      (const char *name);
static udevdevice_t    *udevdevice_create              (const char *name);
static void             udevdevice_delete              (udevdevice_t *self);
static void             udevdevice_delete_cb           (void *self);
static const char      *udevdevice_name                (const udevdevice_t *self);
static udevproperty_t  *udevdevice_get_prop            (udevdevice_t *self, const char *key);
static udevproperty_t  *udevdevice_add_prop            (udevdevice_t *self, const char *key);
static bool             udevdevice_set_prop            (udevdevice_t *self, const char *key, const char *val);
static const char      *udevdevice_get_str_prop        (udevdevice_t *self, const char *key, const char *def);
static int              udevdevice_get_int_prop        (udevdevice_t *self, const char *key, int def);
static bool             udevdevice_refresh             (udevdevice_t *self, struct udev_device *dev);
static bool             udevdevice_is_battery          (udevdevice_t *self);
static bool             udevdevice_is_charger          (udevdevice_t *self);
static void             udevdevice_evaluate_charger    (udevdevice_t *self, mcebat_t *mcebat);
static void             udevdevice_evaluate_charger_cb (gpointer key, gpointer value, gpointer aptr);
static void             udevdevice_evaluate_battery    (udevdevice_t *self, mcebat_t *mcebat);
static void             udevdevice_evaluate_battery_cb (gpointer key, gpointer value, gpointer aptr);

/* ------------------------------------------------------------------------- *
 * UDEVTRACKER
 * ------------------------------------------------------------------------- */

static udevtracker_t *udevtracker_create          (void);
static void           udevtracker_delete          (udevtracker_t *self);
static void           udevtracker_rethink         (udevtracker_t *self);
static gboolean       udevtracker_rethink_cb      (gpointer aptr);
static void           udevtracker_cancel_rethink  (udevtracker_t *self);
static void           udevtracker_schedule_rethink(udevtracker_t *self);
static udevdevice_t  *udevtracker_add_dev         (udevtracker_t *self, const char *path, const char *name);
static bool           udevtracker_update_device   (udevtracker_t *self, struct udev_device *dev);
static bool           udevtracker_start           (udevtracker_t *self);
static void           udevtracker_stop            (udevtracker_t *self);
static gboolean       udevtracker_event_cb        (GIOChannel *chn, GIOCondition cnd, gpointer aptr);
static void           udevtracker_refresh_all     (udevtracker_t *self);
static gboolean       udevtracker_refresh_cb      (gpointer aptr);
static void           udevtracker_schedule_refresh(void);
static void           udevtracker_cancel_refresh  (void);

/* ------------------------------------------------------------------------- *
 * DATAPIPE_HANDLERS
 * ------------------------------------------------------------------------- */

static void mcebat_datapipe_heartbeat_event_cb(gconstpointer data);
static void mcebat_datapipe_init              (void);
static void mcebat_datapipe_quit              (void);

/* ------------------------------------------------------------------------- *
 * G_MODULE
 * ------------------------------------------------------------------------- */

const gchar *g_module_check_init(GModule *module);
void         g_module_unload    (GModule *module);

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
static mcebat_t mcebat_datapipe = {
    .battery_level  = MCE_BATTERY_LEVEL_UNKNOWN,
    .battery_status = BATTERY_STATUS_UNDEF,
    .battery_state  = BATTERY_STATE_UNKNOWN,
    .charger_state  = CHARGER_STATE_UNDEF,
    .charger_type   = CHARGER_TYPE_NONE,
};

/** Cached battery state as derived from udev
 */
static mcebat_t mcebat_actual = {
    .battery_level  = MCE_BATTERY_LEVEL_UNKNOWN,
    .battery_status = BATTERY_STATUS_UNDEF,
    .battery_state  = BATTERY_STATE_UNKNOWN,
    .charger_state  = CHARGER_STATE_UNDEF,
    .charger_type   = CHARGER_TYPE_NONE,
};

#ifdef ENABLE_BATTERY_SIMULATION
/** Maximum number of concurrent call state requesters */
# define CLIENTS_MONITOR_COUNT 1

/** List of monitored battery state requesters */
static GSList *clients_monitor_list = NULL;

/** Cached battery state as requested over D-Bus
 *
 * Synchronized with mcebat_datapipe when simulation
 * is activated, so no initialization is needed.
 */
static mcebat_t mcebat_simulated;
#endif // ENABLE_BATTERY_SIMULATION

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

/** Lookup table for determining charger types */
static GHashTable      *udevdevice_chargertype_lut = 0;

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
    PROP_REAL_TYPE,
    PROP_TYPE,
    // battery
    PROP_CAPACITY,
    PROP_STATUS,
    NULL
};

/** Cached MCE_CONF_BATTERY_UDEV_REFRESH_ON_NOTIFY value */
static bool mcebat_refresh_on_notify = DEFAULT_BATTERY_UDEV_REFRESH_ON_NOTIFY;

/** Cached MCE_CONF_BATTERY_UDEV_REFRESH_ON_HEARTBEAT value  */
static bool mcebat_refresh_on_heartbeat = DEFAULT_BATTERY_UDEV_REFRESH_ON_HEARTBEAT;

/* ========================================================================= *
 * CLIENT
 * ========================================================================= */

#ifdef ENABLE_BATTERY_SIMULATION
/** Unregister battery simulation client
 *
 * When the last client is removed, actual battery/charger state is
 * taken back to use.
 *
 * @param dbus_name  Private D-Bus name of the client
 */
static void
mcebat_dbus_remove_client(const char *dbus_name)
{
    gssize rc = mce_dbus_owner_monitor_remove(dbus_name,
                                              &clients_monitor_list);

    if( rc < 0 )
        goto EXIT;

    if( rc == 0 ) {
        mce_log(LL_WARN, "client %s removed - stop simulation", dbus_name);
        mcebat_update();
    }

EXIT:
    return;
}

/** D-Bus callback: A tracked client dropped from bus
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
mcebat_dbus_client_removed_cb(DBusMessage *const msg)
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

    mcebat_dbus_remove_client(dbus_name);

EXIT:
    dbus_error_free(&error);
    return TRUE;
}

/** Register battery simulation client
 *
 * When the first client is registered, simulated battery state
 * is synchronized with actual state.
 *
 * @param dbus_name  Private D-Bus name of the client
 *
 * @return true if client was registered, false otherwise
 */
static bool
mcebat_dbus_add_client(const char *dbus_name)
{
    bool ack = false;
    gssize rc = mce_dbus_owner_monitor_add(dbus_name,
                                           mcebat_dbus_client_removed_cb,
                                           &clients_monitor_list,
                                           CLIENTS_MONITOR_COUNT);
    if( rc < 0 ) {
        mce_log(LL_WARN, "client %s not added", dbus_name);
        goto EXIT;
    }

    if( rc == 1 ) {
        mce_log(LL_WARN, "client %s added - start simulation", dbus_name);
        /* Note: Simulation starts from current state, so there
         *       is no need to re-evaluate immediately. */
        mcebat_simulated = mcebat_datapipe;
    }

    ack = true;

EXIT:
    return ack;
}

/** Evaluate simulated battery status
 *
 * Should be called whenever the simulation values controlled
 * by clients change so that also derived values are re-evaluated.
 */
static void
mcebat_dbus_evaluate_battery_status(void)
{
    /* Handle charger-connected special cases */
    if( mcebat_simulated.charger_state == CHARGER_STATE_ON ) {
        mcebat_simulated.battery_state  = BATTERY_STATE_CHARGING;

        if( mcebat_simulated.battery_level >= 100 ) {
            /* Battery full reached */
            mcebat_simulated.battery_status = BATTERY_STATUS_FULL;
            mcebat_simulated.battery_state  = BATTERY_STATE_FULL;
            goto EXIT;
        }
        if( mcebat_simulated.battery_status == BATTERY_STATUS_FULL &&
            mcebat_simulated.battery_level  >= BATTERY_CAPACITY_FULL ) {
            /* Maintenance charging retains full status*/
            goto EXIT;
        }
        if( mcebat_simulated.battery_level  >  BATTERY_CAPACITY_UNDEF ) {
            /* Low/empty does not apply while charging */
            mcebat_simulated.battery_status = BATTERY_STATUS_OK;
            goto EXIT;
        }
    }
    else {
        mcebat_simulated.battery_state  = BATTERY_STATE_DISCHARGING;
    }

    /* Evaluate based on battery level */
    if( mcebat_simulated.battery_level <= BATTERY_CAPACITY_UNDEF )
        mcebat_simulated.battery_status = BATTERY_STATUS_UNDEF;
    else if( mcebat_simulated.battery_level <= BATTERY_CAPACITY_EMPTY )
        mcebat_simulated.battery_status = BATTERY_STATUS_EMPTY;
    else if( mcebat_simulated.battery_level <= BATTERY_CAPACITY_LOW )
        mcebat_simulated.battery_status = BATTERY_STATUS_LOW;
    else
        mcebat_simulated.battery_status = BATTERY_STATUS_OK;

EXIT:
    return;
}

/** D-Bus callback: Simulated charger type requested
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
mcebat_dbus_charger_type_req_cb(DBusMessage *const msg)
{
    dbus_bool_t   accepted  = false;
    const char    *sender   = dbus_message_get_sender(msg);
    DBusError      error    = DBUS_ERROR_INIT;
    const char    *type     = 0;
    DBusMessage   *reply    = 0;

    mce_log(LL_DEVEL, "charger type request from %s",
            mce_dbus_get_name_owner_ident(sender));

    if( !mcebat_dbus_add_client(sender) )
        goto EXIT;

    if( !dbus_message_get_args(msg, &error,
                               DBUS_TYPE_STRING, &type,
                               DBUS_TYPE_INVALID) ) {
        goto EXIT;
    }

    if( !strcmp(type, MCE_CHARGER_TYPE_NONE) )
        mcebat_simulated.charger_type = CHARGER_TYPE_NONE;
    else if( !strcmp(type, MCE_CHARGER_TYPE_USB) )
        mcebat_simulated.charger_type = CHARGER_TYPE_USB;
    else if( !strcmp(type, MCE_CHARGER_TYPE_DCP) )
        mcebat_simulated.charger_type = CHARGER_TYPE_DCP;
    else if( !strcmp(type, MCE_CHARGER_TYPE_HVDCP) )
        mcebat_simulated.charger_type = CHARGER_TYPE_HVDCP;
    else if( !strcmp(type, MCE_CHARGER_TYPE_CDP) )
        mcebat_simulated.charger_type = CHARGER_TYPE_CDP;
    else if( !strcmp(type, MCE_CHARGER_TYPE_WIRELESS) )
        mcebat_simulated.charger_type = CHARGER_TYPE_WIRELESS;
    else
        mcebat_simulated.charger_type = CHARGER_TYPE_OTHER;

    mcebat_dbus_evaluate_battery_status();
    mcebat_update();

    accepted = true;

EXIT:
    /* Setup the reply */
    reply = dbus_new_method_reply(msg);

    /* Append the result */
    if( !dbus_message_append_args(reply,
                                  DBUS_TYPE_BOOLEAN, &accepted,
                                  DBUS_TYPE_INVALID)) {
        mce_log(LL_ERR,"Failed to append reply arguments to D-Bus "
                "message for %s.%s",
                MCE_REQUEST_IF, dbus_message_get_member(msg));
    }
    else if( !dbus_message_get_no_reply(msg) ) {
        dbus_send_message(reply), reply = 0;
    }

    if( reply )
        dbus_message_unref(reply);

    dbus_error_free(&error);

    return TRUE;
}

/** D-Bus callback: Simulated charger state requested
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
mcebat_dbus_charger_state_req_cb(DBusMessage *const msg)
{
    dbus_bool_t   accepted  = false;
    const char    *sender   = dbus_message_get_sender(msg);
    DBusError      error    = DBUS_ERROR_INIT;
    const char    *state    = 0;
    DBusMessage   *reply    = 0;

    mce_log(LL_DEVEL, "charger state request from %s",
            mce_dbus_get_name_owner_ident(sender));

    if( !mcebat_dbus_add_client(sender) )
        goto EXIT;

    if( !dbus_message_get_args(msg, &error,
                               DBUS_TYPE_STRING, &state,
                               DBUS_TYPE_INVALID) ) {
        goto EXIT;
    }

    if( !strcmp(state, MCE_CHARGER_STATE_ON) )
        mcebat_simulated.charger_state = CHARGER_STATE_ON;
    else if( !strcmp(state, MCE_CHARGER_STATE_OFF) )
        mcebat_simulated.charger_state = CHARGER_STATE_OFF;
    else
        mcebat_simulated.charger_state = CHARGER_STATE_UNDEF;

    mcebat_dbus_evaluate_battery_status();
    mcebat_update();

    accepted = true;

EXIT:
    /* Setup the reply */
    reply = dbus_new_method_reply(msg);

    /* Append the result */
    if( !dbus_message_append_args(reply,
                                  DBUS_TYPE_BOOLEAN, &accepted,
                                  DBUS_TYPE_INVALID)) {
        mce_log(LL_ERR,"Failed to append reply arguments to D-Bus "
                "message for %s.%s",
                MCE_REQUEST_IF, dbus_message_get_member(msg));
    }
    else if( !dbus_message_get_no_reply(msg) ) {
        dbus_send_message(reply), reply = 0;
    }

    if( reply )
        dbus_message_unref(reply);

    dbus_error_free(&error);

    return TRUE;
}

/** D-Bus callback: Simulated charger state requested
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
mcebat_dbus_battery_level_req_cb(DBusMessage *const msg)
{
    dbus_bool_t   accepted  = false;
    const char    *sender   = dbus_message_get_sender(msg);
    DBusError      error    = DBUS_ERROR_INIT;
    dbus_int32_t   level    = 0;
    DBusMessage   *reply    = 0;

    mce_log(LL_DEVEL, "battery level request from %s",
            mce_dbus_get_name_owner_ident(sender));

    if( !mcebat_dbus_add_client(sender) )
        goto EXIT;

    if( !dbus_message_get_args(msg, &error,
                               DBUS_TYPE_INT32, &level,
                               DBUS_TYPE_INVALID) ) {
        goto EXIT;
    }

    mcebat_simulated.battery_level = level;

    mcebat_dbus_evaluate_battery_status();
    mcebat_update();

    accepted = true;

EXIT:
    /* Setup the reply */
    reply = dbus_new_method_reply(msg);

    /* Append the result */
    if( !dbus_message_append_args(reply,
                                  DBUS_TYPE_BOOLEAN, &accepted,
                                  DBUS_TYPE_INVALID)) {
        mce_log(LL_ERR,"Failed to append reply arguments to D-Bus "
                "message for %s.%s",
                MCE_REQUEST_IF, dbus_message_get_member(msg));
    }
    else if( !dbus_message_get_no_reply(msg) ) {
        dbus_send_message(reply), reply = 0;
    }

    if( reply )
        dbus_message_unref(reply);

    dbus_error_free(&error);

    return TRUE;
}
#endif // ENABLE_BATTERY_SIMULATION

/** Array of dbus message handlers */
static mce_dbus_handler_t callstate_dbus_handlers[] =
{
    /* method calls */
#ifdef ENABLE_BATTERY_SIMULATION
    {
        .interface  = MCE_REQUEST_IF,
        .name       = MCE_CHARGER_TYPE_REQ,
        .type       = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback   = mcebat_dbus_charger_type_req_cb,
        .privileged = true,
        .args       =
            "    <arg direction=\"in\" name=\"charger_type\" type=\"s\"/>\n"
            "    <arg direction=\"out\" name=\"accepted\" type=\"b\"/>\n"
    },
    {
        .interface  = MCE_REQUEST_IF,
        .name       = MCE_CHARGER_STATE_REQ,
        .type       = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback   = mcebat_dbus_charger_state_req_cb,
        .privileged = true,
        .args       =
            "    <arg direction=\"in\" name=\"charger_state\" type=\"s\"/>\n"
            "    <arg direction=\"out\" name=\"accepted\" type=\"b\"/>\n"
    },
    {
        .interface  = MCE_REQUEST_IF,
        .name       = MCE_BATTERY_LEVEL_REQ,
        .type       = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback   = mcebat_dbus_battery_level_req_cb,
        .privileged = true,
        .args       =
            "    <arg direction=\"in\" name=\"battery_level\" type=\"i\"/>\n"
            "    <arg direction=\"out\" name=\"accepted\" type=\"b\"/>\n"
    },
#endif // ENABLE_BATTERY_SIMULATION
    /* sentinel */
    {
        .interface = 0
    }
};

/** Add dbus handlers
 */
static void mcebat_dbus_init(void)
{
    mce_dbus_handler_register_array(callstate_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mcebat_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(callstate_dbus_handlers);

#ifdef ENABLE_BATTERY_SIMULATION
    /* Just release resources, do not re-evaluate state */
    mce_dbus_owner_monitor_remove_all(&clients_monitor_list);
#endif
}

/* ========================================================================= *
 * MCEBAT
 * ========================================================================= */

/** Update battery state visible in datapipes
 *
 * @param curr  Battery state data to expose.
 */
static void
mcebat_update(void)
{
    const mcebat_t *curr = &mcebat_actual;

#ifdef ENABLE_BATTERY_SIMULATION
    if( clients_monitor_list )
        curr = &mcebat_simulated;
#endif

    mcebat_t prev = mcebat_datapipe;
    mcebat_datapipe = *curr;

    if( prev.charger_type != curr->charger_type ) {
        mce_log(LL_CRUCIAL, "charger_type: %s -> %s",
                charger_type_repr(prev.charger_type),
                charger_type_repr(curr->charger_type));

        datapipe_exec_full(&charger_type_pipe,
                           GINT_TO_POINTER(curr->charger_type));
    }

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

    if( prev.battery_state != curr->battery_state ) {
        mce_log(LL_CRUCIAL, "battery_state: %s -> %s",
                battery_state_repr(prev.battery_state),
                battery_state_repr(curr->battery_state));

        /* Battery charging state */
        datapipe_exec_full(&battery_state_pipe,
                           GINT_TO_POINTER(curr->battery_state));
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

/** Lookup mce battery state based on udev battery status property value
 *
 * @param status  udev battery status
 *
 * @return battery_state_t enumeration value
 */

static battery_state_t
udevdevice_lookup_battery_state(const char *status)
{
    battery_state_t state = BATTERY_STATE_UNKNOWN;

    if( !g_strcmp0(status, "Charging") )
        state = BATTERY_STATE_CHARGING;
    else if( !g_strcmp0(status, "Discharging") )
        state = BATTERY_STATE_DISCHARGING;
    else if( !g_strcmp0(status, "Not charging") )
        state = BATTERY_STATE_NOT_CHARGING;
    else if( !g_strcmp0(status, "Full") )
        state = BATTERY_STATE_FULL;
    else if( g_strcmp0(status, "Unknown") )
        mce_log(LL_WARN, "unrecognized power supply state '%s'", status);

    return state;
}

/** Lookup charger type based on device name / value of type property
 *
 * @param name  string to match
 *
 * @return charger_type_t enumeration value
 */
static charger_type_t
udevdevice_lookup_charger_type(const char *name)
{
    charger_type_t type = CHARGER_TYPE_INVALID;
    gchar         *key  = 0;
    gpointer       val;

    if( !name || !udevdevice_chargertype_lut )
        goto EXIT;

    key = g_ascii_strdown(name, -1);

    /* Try exact match 1st, then relaxed one which equates
     * "chipname-ac" with plain "ac".
     */
    val = g_hash_table_lookup(udevdevice_chargertype_lut, key);
    if( !val ) {
        const char *end = strrchr(key, '-');
        if( end )
            val = g_hash_table_lookup(udevdevice_chargertype_lut, end + 1);
    }
    type = GPOINTER_TO_INT(val);

EXIT:
    if( type == CHARGER_TYPE_INVALID ) {
        mce_log(LL_WARN, "unknown charger type: %s", name ?: "null");
        type = CHARGER_TYPE_OTHER;
    }
    g_free(key);

    mce_log(LL_DEBUG, "charger type: %s -> %s", name ?: "null", charger_type_repr(type));
    return type;
}

static void
udevdevice_init_chargertype(void)
{
    static const struct {
        const char     *name;
        charger_type_t  type;
    } lut[] = {
        /* Type map - adapted from statefs sources
         */
        { "CDP",         CHARGER_TYPE_CDP      },
        { "USB_CDP",     CHARGER_TYPE_CDP      },
        { "USB_DCP",     CHARGER_TYPE_DCP      },
        { "USB_HVDCP",   CHARGER_TYPE_HVDCP    },
        { "USB_HVDCP_3", CHARGER_TYPE_HVDCP    },
        { "Mains",       CHARGER_TYPE_DCP      },
        { "USB",         CHARGER_TYPE_USB      },
        { "USB_ACA",     CHARGER_TYPE_USB      },

        /* Additions since leaving statefs behind
         */
        { "WIRELESS",    CHARGER_TYPE_WIRELESS },
        { "AC",          CHARGER_TYPE_DCP      },

        /* Pinephone chargers
         */
        { "axp813-ac",    CHARGER_TYPE_DCP },
        { "axp20x-usb",   CHARGER_TYPE_USB },
        
        /* To make connect/disconnect transitions
         * cleaner, ignore "Unknown" reporting
         */
        { "Unknown",     CHARGER_TYPE_NONE     },

        /* Sentinel */
        { 0, 0 }
    };

    static const char grp[] = MCE_CONF_BATTERY_UDEV_DEVICE_CHARGERTYPE_GROUP;

    if( udevdevice_chargertype_lut )
        goto EXIT;

    udevdevice_chargertype_lut =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 0);

    /* Seed with built-in values */
    for( size_t i = 0; lut[i].name; ++i ) {
        g_hash_table_insert(udevdevice_chargertype_lut,
                            g_ascii_strdown(lut[i].name, -1),
                            GINT_TO_POINTER(lut[i].type));
    }

#if 0
    /* Dump as ini-file for use as an example */
    {
        GHashTableIter iter;
        gpointer key, val;
        g_hash_table_iter_init(&iter, udevdevice_chargertype_lut);
        printf("[%s]\n", grp);
        while ( g_hash_table_iter_next (&iter, &key, &val) )
            printf("#%s = %s\n", key, charger_type_repr(GPOINTER_TO_INT(val)));
    }
#endif

    /* Override with configuration */
    if( mce_conf_has_group(grp) ) {
        mce_log(LL_DEBUG, "using configured chargertypes");
        gsize   count = 0;
        gchar **keys  = mce_conf_get_keys(grp, &count);

        for( gsize i = 0; i < count; ++i ) {
            const gchar    *name  = keys[i];
            gchar          *value = mce_conf_get_string(grp, name, 0);
            charger_type_t  type  = charger_type_parse(value);

            if( type != CHARGER_TYPE_INVALID ) {
                g_hash_table_insert(udevdevice_chargertype_lut,
                                    g_ascii_strdown(name, -1),
                                    GINT_TO_POINTER(type));
            }

            g_free(value);
        }
        g_strfreev(keys);
    }

EXIT:
    return;
}

/** Release device chargertype lookup table
 */
static void
udevdevice_quit_chargertype(void)
{
    if( udevdevice_chargertype_lut ) {
        g_hash_table_unref(udevdevice_chargertype_lut),
            udevdevice_chargertype_lut = 0;
    }
}

/** Initialize device blacklist lookup table
 */
static void
udevdevice_init_blacklist(void)
{
    static const char grp[] = MCE_CONF_BATTERY_UDEV_DEVICE_BLACKLIST_GROUP;
    static const char * const builtin_blacklist[] = {
        "bcl",
        "bms",
        "dc",
        "fg_adc",
        "main",
        "parallel",
        "pc_port",
        "pm8921-dc",
        0
    };

    if( udevdevice_blacklist_lut )
        goto EXIT;

    udevdevice_blacklist_lut =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 0);

    if( mce_conf_has_group(grp) ) {
        mce_log(LL_DEBUG, "using configured device blacklist");
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
    }
    else {
        mce_log(LL_DEBUG, "using built-in device blacklist");
        for( size_t i = 0; builtin_blacklist[i]; ++i ) {
            g_hash_table_replace(udevdevice_blacklist_lut,
                                 g_strdup(builtin_blacklist[i]),
                                 GINT_TO_POINTER(true));
        }
    }

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
    self->udd_full     = false;
    self->udd_charging = false;

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

/** Predicate for: power_supply device is a battery
 *
 * @param self  device object
 *
 * @return true device is a battery, false otherwise
 */
static bool
udevdevice_is_battery(udevdevice_t *self)
{
    return (udevdevice_get_prop(self, PROP_STATUS) &&
            udevdevice_get_prop(self, PROP_CAPACITY));
}

/** Predicate for: power_supply device is a charger
 *
 * @param self  device object
 *
 * @return true device is a charger, false otherwise
 */
static bool
udevdevice_is_charger(udevdevice_t *self)
{
    if( udevdevice_is_battery(self) )
        return false;

    return (udevdevice_get_prop(self, PROP_PRESENT) ||
            udevdevice_get_prop(self, PROP_ONLINE));
}

/** Update mce style battery data based on device properties
 *
 * @param self    device object
 * @param mcebat  mce style battery data to update
 */
static void
udevdevice_evaluate_charger(udevdevice_t *self, mcebat_t *mcebat)
{
    if( !udevdevice_is_charger(self) )
        goto EXIT;

    int present = udevdevice_get_int_prop(self, PROP_PRESENT, -1);
    int online  = udevdevice_get_int_prop(self, PROP_ONLINE, -1);

    /* Device is a charger.
     *
     * Whatever the meaning of present / online properties
     * is supposed to be, the best guess we can make is that
     * we ought to be able to charge when either one gets
     * non-zero value ... */

    bool active = (present == 1 || online == 1);

    if( active ) {
        mcebat->charger_state = CHARGER_STATE_ON;

        /* Charger is online, evaluate charger type
         *
         * Legacy QC devices have TYPE property that has
         * content sfos sw stack knows how to interpret.
         *
         * More recent QC devices might expose "USB_PD" in
         * TYPE prop and have additional REAL_TYPE property
         * containing old style data.
         *
         * MTK devices have multiple power supply device
         * nodes visible in udev and charger type must be
         * determined from device node name.
         */
        const char *name;
        if( !(name = udevdevice_get_str_prop(self, PROP_REAL_TYPE, 0)) ) {
            if( !(name = udevdevice_get_str_prop(self, PROP_TYPE, 0)) )
                name = udevdevice_name(self);
        }

        charger_type_t type = udevdevice_lookup_charger_type(name);

        /* Update effective charger type exposed on D-Bus
         */
        if( mcebat->charger_type < type )
            mcebat->charger_type = type;
    }

    mce_log(LL_DEBUG, "%s: charger @ "
            "present=%d online=%d -> active=%d",
            udevdevice_name(self),
            present, online, active);
EXIT:
    return;
}

/** g_hash_table_foreach() compatible udevdevice_evaluate_charger() wrapper callback
 *
 * @param key   (unused) device sysname as void pointer
 * @param value  device object as void pointer
 * @param aptr   mce battery data object as void pointer
 */
static void
udevdevice_evaluate_charger_cb(gpointer key, gpointer value, gpointer aptr)
{
    (void)key;

    mcebat_t     *mcebat = aptr;
    udevdevice_t *self   = value;

    udevdevice_evaluate_charger(self, mcebat);
}

/** Update mce style battery data based on device properties
 *
 * @param self    device object
 * @param mcebat  mce style battery data to update
 */
static void
udevdevice_evaluate_battery(udevdevice_t *self, mcebat_t *mcebat)
{
    if( !udevdevice_is_battery(self) )
        goto EXIT;

    /* Device is a battery.
     *
     * FIXME: There is a built-in assumption that there will be only
     *        one battery device - if there should be more than one,
     *        then the one that happens to be the last to be seen
     *        during g_hash_table_foreach() ends up being used.
     */

    int         capacity = udevdevice_get_int_prop(self, PROP_CAPACITY, -1);
    const char *status   = udevdevice_get_str_prop(self, PROP_STATUS, 0);

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

    /* udev status is "Unknown|Charging|Discharging|Not charging|Full" */

    mcebat->battery_state = udevdevice_lookup_battery_state(status);

    /* "Charging" and "Full" override capacity based mce battery status
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
    if( mcebat->battery_state == BATTERY_STATE_FULL ) {
        mcebat->charger_state  = CHARGER_STATE_ON;
        mcebat->battery_status = BATTERY_STATUS_FULL;
        self->udd_full = true;
    }
    else if( mcebat->battery_state == BATTERY_STATE_CHARGING ) {
        mcebat->charger_state  = CHARGER_STATE_ON;
        mcebat->battery_status = BATTERY_STATUS_OK;
        if( self->udd_full && capacity >= BATTERY_CAPACITY_FULL )
            mcebat->battery_status = BATTERY_STATUS_FULL;
        else
            self->udd_full = false;
    }
    /* Some devices go:
     *   Charging -> Full -> Discharging -> Charging -> Full
     *
     * Others might go:
     *   Charging -> Not charging -> Charging -> Not charging
     *
     * Use heuristics to normalize such things to battery full too.
     */
    else if( mcebat->charger_state == CHARGER_STATE_ON &&
             capacity >= BATTERY_CAPACITY_FULL &&
             (self->udd_full || self->udd_charging) ) {
        mcebat->battery_status = BATTERY_STATUS_FULL;

        if( !self->udd_full ) {
            mce_log(LL_WARN, "assuming end of charging due to battery full");
            self->udd_full = true;
        }
    }
    else {
        self->udd_full = false;
    }

    /* Override udev status on heuristically determined battery full */
    if( mcebat->battery_status == BATTERY_STATUS_FULL )
        mcebat->battery_state = BATTERY_STATE_FULL;

    mce_log(LL_DEBUG, "%s: battery @ cap=%d status=%s full=%d",
            udevdevice_name(self), capacity, status, self->udd_full);

    self->udd_charging = !g_strcmp0(status, "Charging");

EXIT:
    return;
}

/** g_hash_table_foreach() compatible udevdevice_evaluate_battery() wrapper callback
 *
 * @param key   (unused) device sysname as void pointer
 * @param value  device object as void pointer
 * @param aptr   mce battery data object as void pointer
 */
static void
udevdevice_evaluate_battery_cb(gpointer key, gpointer value, gpointer aptr)
{
    (void)key;

    mcebat_t     *mcebat = aptr;
    udevdevice_t *self   = value;

    udevdevice_evaluate_battery(self, mcebat);
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

    /* Give charger_state special treatment: Assume charger is
     * disconnect & rectify if any of the battery/charger devices
     * indicate charging activity. */
    mcebat_actual.charger_state = CHARGER_STATE_OFF;

    /* Reset charger type, iterator chooses maximum of
     * none < other < wall chargers < pc connection. */
    mcebat_actual.charger_type = CHARGER_TYPE_NONE;

    g_hash_table_foreach(self->udt_devices, udevdevice_evaluate_charger_cb, &mcebat_actual);
    g_hash_table_foreach(self->udt_devices, udevdevice_evaluate_battery_cb, &mcebat_actual);

    /* Sync to datapipes */
    mcebat_update();
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
        self->udt_rethink_id =
            mce_wakelocked_timeout_add(BATTERY_REEVALUATE_DELAY,
                                       udevtracker_rethink_cb,
                                       self);
        mce_log(LL_DEBUG, "battery state re-evaluation sheduled");
    }
}

/** Add device object to track
 *
 * @param self  tracker object
 * @param path  device syspath
 * @param name  device sysname
 *
 * @return device object
 */
static udevdevice_t *
udevtracker_add_dev(udevtracker_t *self, const char *path, const char *name)
{
    udevdevice_t *dev = g_hash_table_lookup(self->udt_devices, path);

    if( !dev ) {
        dev = udevdevice_create(name);
        g_hash_table_replace(self->udt_devices, g_strdup(path), dev);
    }
    return dev;
}

/** Update properties of tracked device
 *
 * @param self  tracker object
 * @param dev   udev device object
 *
 * @return true if device is used, or false if ignored
 */
static bool
udevtracker_update_device(udevtracker_t *self, struct udev_device *dev)
{
    bool rethink = false;

    /* TODO: Currently it is assumed that we receive only
     *       "add" or "change" notifications for power
     *       supply devices after the initial enumeration.
     */

    const char   *sysname  = udev_device_get_sysname(dev);
    const char   *syspath  = udev_device_get_syspath(dev);
    const char   *action   = udev_device_get_action(dev);

    if( udevdevice_is_blacklisted(sysname) ) {
        /* Report blacklisted devices during initial enumeration */
        if( !action )
            mce_log(LL_DEBUG, "%s: is blacklisted", sysname);
        goto EXIT;
    }

    udevdevice_t *powerdev = udevtracker_add_dev(self, syspath, sysname);
    if( (rethink = udevdevice_refresh(powerdev, dev)) )
        udevtracker_schedule_rethink(self);
EXIT:
    return rethink;
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
    mce_log(LL_DEBUG, "ENTER - get initial state");
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
    mce_log(LL_DEBUG, "LEAVE - get initial state");

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
    mce_log(LL_DEBUG, "ENTER - udev notification");

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
        bool changed = udevtracker_update_device(self, dev);
        if( changed && mcebat_refresh_on_notify )
            udevtracker_schedule_refresh();
        udev_device_unref(dev);
    }

    result = G_SOURCE_CONTINUE;

EXIT:
    if( result != G_SOURCE_CONTINUE && self->udt_udev_event_id != 0 ) {
        mce_log(LL_CRIT, "disabling udev io watch");
        self->udt_udev_event_id = 0;
        udevtracker_stop(self);
    }

    mce_log(LL_DEBUG, "LEAVE - udev notification");
    mce_wakelock_release(udevtracker_wakelock);

    return result;
}

static void
udevtracker_refresh_all(udevtracker_t *self)
{
    /* Doing it now, cancel delayed refresh */
    udevtracker_cancel_refresh();

    /* Operate on copy of keys just in case the hash
     * table should change due to changes made from here.
     */
    GList *syspaths = g_hash_table_get_keys(self->udt_devices);
    for( GList *iter = syspaths; iter; iter = iter->next ) {
        const gchar *syspath = iter->data;
        iter->data = g_strdup(syspath);
    }

    /* Assumption based on taking a peek at libudev code:
     * properties for freshly created udev_device are
     * populated by reading appropriate uevent file and
     * thus are not something that would be cached at
     * libudev level -> we get current data from kernel.
     */
    for( GList *iter = syspaths; iter; iter = iter->next ) {
        const gchar *syspath = iter->data;
        struct udev_device *dev =
            udev_device_new_from_syspath(self->udt_udev_handle, syspath);
        if( dev ) {
            udevtracker_update_device(self, dev);
            udev_device_unref(dev);
        }
    }

    g_list_free_full(syspaths, g_free);
}

static guint udevtracker_refresh_id = 0;
static gboolean udevtracker_refresh_cb(gpointer aptr)
{
    (void)aptr;
    if( udevtracker_refresh_id ) {
        udevtracker_refresh_id = 0;

        mce_log(LL_DEBUG, "ENTER - refresh on notify");

        if( udevtracker_object )
            udevtracker_refresh_all(udevtracker_object);

        mce_log(LL_DEBUG, "LEAVE - refresh on notify");
    }
    return G_SOURCE_REMOVE;
}

static void udevtracker_schedule_refresh(void)
{
    if( !udevtracker_refresh_id )
        mce_log(LL_DEBUG, "forced value refresh scheduled");
    else
        g_source_remove(udevtracker_refresh_id);
    udevtracker_refresh_id =
        mce_wakelocked_timeout_add(DEVICES_REFRESH_DELAY,
                                   udevtracker_refresh_cb, 0);
}

static void udevtracker_cancel_refresh(void)
{
    if( udevtracker_refresh_id ) {
        mce_log(LL_DEBUG, "forced value refresh cancelled");
        g_source_remove(udevtracker_refresh_id),
            udevtracker_refresh_id = 0;
    }
}

/* ========================================================================= *
 * DATAPIPE_HANDLERS
 * ========================================================================= */

/** Change notifications for heartbeat_event_pipe
 *
 * @param data (unused dummy parameter)
 */
static void mcebat_datapipe_heartbeat_event_cb(gconstpointer data)
{
    (void)data;

    mce_log(LL_DEBUG, "ENTER - refresh on heartbeat");

    if( mcebat_refresh_on_heartbeat && udevtracker_object )
        udevtracker_refresh_all(udevtracker_object);

    mce_log(LL_DEBUG, "LEAVE - refresh on heartbeat");
}

/** Array of datapipe handlers */
static datapipe_handler_t mcebat_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &heartbeat_event_pipe,
        .output_cb = mcebat_datapipe_heartbeat_event_cb,
    },
    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t mcebat_datapipe_bindings =
{
    .module   = "battery_udev",
    .handlers = mcebat_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void mcebat_datapipe_init(void)
{
    mce_datapipe_init_bindings(&mcebat_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void mcebat_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&mcebat_datapipe_bindings);
}

/* ========================================================================= *
 * G_MODULE
 * ========================================================================= */

static guint mcebat_init_tracker_id = 0;

static gboolean
mcebat_init_tracker_cb(gpointer aptr)
{
    (void)aptr;

    udevtracker_object = udevtracker_create();

    if( !udevtracker_start(udevtracker_object) )
        goto EXIT;

EXIT:
    mcebat_init_tracker_id = 0;
    return G_SOURCE_REMOVE;
}

static void
mcebat_init_settings(void)
{
    mcebat_refresh_on_notify =
        mce_conf_get_bool(MCE_CONF_BATTERY_UDEV_SETTINGS_GROUP,
                          MCE_CONF_BATTERY_UDEV_REFRESH_ON_NOTIFY,
                          DEFAULT_BATTERY_UDEV_REFRESH_ON_NOTIFY);

    mcebat_refresh_on_heartbeat =
        mce_conf_get_bool(MCE_CONF_BATTERY_UDEV_SETTINGS_GROUP,
                          MCE_CONF_BATTERY_UDEV_REFRESH_ON_HEARTBEAT,
                          DEFAULT_BATTERY_UDEV_REFRESH_ON_HEARTBEAT);
}

/** Init function for the battery and charger module
 *
 * @param module (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    mcebat_init_settings();
    udevdevice_init_blacklist();
    udevdevice_init_chargertype();
    udevproperty_init_types();

    mcebat_dbus_init();
    mcebat_datapipe_init();

    /* Initial udev probing can take a long time.
     * Do it from idle callback in order not to delay
     * reaching systemd unit ready state.
     */
    mcebat_init_tracker_id = g_idle_add(mcebat_init_tracker_cb, 0);
    mce_log(LL_DEBUG, "%s: loaded", MODULE_NAME);

    return NULL;
}

/** Exit function for the battery and charger module
 *
 * @param module (not used)
 */
G_MODULE_EXPORT void g_module_unload(GModule *module)
{
    (void)module;

    if( mcebat_init_tracker_id )
        g_source_remove(mcebat_init_tracker_id), mcebat_init_tracker_id = 0;

    mcebat_datapipe_quit();
    mcebat_dbus_quit();

    udevtracker_delete(udevtracker_object), udevtracker_object = 0;

    udevproperty_quit_types();
    udevdevice_quit_chargertype();
    udevdevice_quit_blacklist();
    udevtracker_cancel_refresh();

    mce_log(LL_DEBUG, "%s: unloaded", MODULE_NAME);
}
