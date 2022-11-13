/**
 * @file charging.c
 *
 * Charging -- this module handles user space charger enable/disable
 * <p>
 * Copyright (c) 2017 - 2022 Jolla Ltd.
  * <p>
 * @author Simo Piiroinen <simo.piiroinen@jolla.com>
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

#include "charging.h"

#include "../mce.h"
#include "../mce-conf.h"
#include "../mce-log.h"
#include "../datapipe.h"
#include "../mce-setting.h"
#include "../mce-dbus.h"

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#include <gmodule.h>

/* ========================================================================= *
 * TYPES & CONSTANTS
 * ========================================================================= */

/** Module name */
#define MODULE_NAME                     "charging"

/** Minimum battery level where charging can be disabled [%]
 *
 * Having charger connected but not charging from it can delay (USER mode)
 * or inhibit (ACTDEAD mode) battery empty shutdown -> allow charging when
 * battery level is approaching battery empty shutdown level - regardless
 * of possible user configured limits.
 */
#define MCH_MINIMUM_BATTERY_LEVEL       5

/* ========================================================================= *
 * Types
 * ========================================================================= */

/** Charging mode override */
typedef enum {
    /** Transient / placeholder value */
    FORCED_CHARGING_UNKNOWN,
    /** Charging mode is ignored and battery is charged until full */
    FORCED_CHARGING_ENABLED,
    /** Battery is charged according to charging mode settings */
    FORCED_CHARGING_DISABLED,
} forced_charging_t;

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * FORCED_CHARGING
 * ------------------------------------------------------------------------- */

const char        *forced_charging_repr (forced_charging_t value);
forced_charging_t  forced_charging_parse(const char *repr);

/* ------------------------------------------------------------------------- *
 * CHARGING_MODE
 * ------------------------------------------------------------------------- */

static const char *charging_mode_repr(charging_mode_t mode);

/* ------------------------------------------------------------------------- *
 * CHARGING_STATE
 * ------------------------------------------------------------------------- */

static const char *charging_state_repr(charging_state_t state);

/* ------------------------------------------------------------------------- *
 * UTILITY
 * ------------------------------------------------------------------------- */

static int mch_clamp(int value, int minval, int maxval);

/* ------------------------------------------------------------------------- *
 * MCH_SYSFS
 * ------------------------------------------------------------------------- */

static bool mch_sysfs_write(const char *path, const char *text);

/* ------------------------------------------------------------------------- *
 * MCH_POLICY
 * ------------------------------------------------------------------------- */

static void mch_policy_set_battery_full       (bool battery_full);
static void mch_policy_set_charging_state     (charging_state_t charging_state);
static void mch_policy_evaluate_charging_state(void);
static void mch_policy_set_charging_mode      (charging_mode_t charging_mode);
static void mch_policy_set_limit_disable      (int limit_disable);
static void mch_policy_set_limit_enable       (int limit_enable);
static void mch_policy_set_forced_charging_ex (forced_charging_t forced_charging, bool evaluate_state);
static void mch_policy_set_forced_charging    (forced_charging_t forced_charging);

/* ------------------------------------------------------------------------- *
 * MCH_SETTINGS
 * ------------------------------------------------------------------------- */

static void mch_settings_cb  (GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);
static void mch_settings_init(void);
static void mch_settings_quit(void);

/* ------------------------------------------------------------------------- *
 * MCH_CONFIG
 * ------------------------------------------------------------------------- */

static void mch_config_init(void);
static void mch_config_quit(void);

/* ------------------------------------------------------------------------- *
 * MCH_DATAPIPE
 * ------------------------------------------------------------------------- */

static void mch_datapipe_usb_cable_state_cb(gconstpointer data);
static void mch_datapipe_charger_state_cb  (gconstpointer data);
static void mch_datapipe_battery_status_cb (gconstpointer data);
static void mch_datapipe_battery_level_cb  (gconstpointer data);
static void mch_datapipe_init              (void);
static void mch_datapipe_quit              (void);

/* ------------------------------------------------------------------------- *
 * MCH_DBUS
 * ------------------------------------------------------------------------- */

static void     mch_dbus_send_charging_state         (DBusMessage *const req);
static gboolean mch_dbus_get_charging_state_cb       (DBusMessage *const req);
static void     mch_dbus_send_forced_charging_state  (DBusMessage *const req);
static gboolean mch_dbus_get_forced_charging_state_cb(DBusMessage *const req);
static gboolean mch_dbus_set_forced_charging_state_cb(DBusMessage *const req);
static gboolean mch_dbus_get_charging_suspendable_cb (DBusMessage *const req);
static gboolean mch_dbus_initial_cb                  (gpointer aptr);
static void     mch_dbus_init                        (void);
static void     mch_dbus_quit                        (void);

/* ------------------------------------------------------------------------- *
 * G_MODULE
 * ------------------------------------------------------------------------- */

const gchar *g_module_check_init(GModule *module);
void         g_module_unload    (GModule *module);

/* ========================================================================= *
 * DATA
 * ========================================================================= */

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Functionality that this module depends on */
static const gchar *const depends[] = { NULL };

/** Functionality that this module recommends */
static const gchar *const recommends[] = { NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
    /** Name of the module */
    .name = MODULE_NAME,
    /** Module dependencies */
    .depends = depends,
    /** Module recommends */
    .recommends = recommends,
    /** Module provides */
    .provides = provides,
    /** Module priority */
    .priority = 250
};

/** USB cable status; assume undefined */
static usb_cable_state_t usb_cable_state = USB_CABLE_UNDEF;

/** Charger state; assume undefined */
static charger_state_t charger_state = CHARGER_STATE_UNDEF;

/** Battery status; assume undefined */
static battery_status_t battery_status = BATTERY_STATUS_UNDEF;

/** Battery charge level: assume unknown */
static gint battery_level = MCE_BATTERY_LEVEL_UNKNOWN;

/** Policy setting: When to disable/enable charging */
static charging_mode_t mch_charging_mode = MCE_DEFAULT_CHARGING_MODE;
static guint mch_charging_mode_id = 0;

/** Whether to override charging mode policy settings */
static forced_charging_t mch_forced_charging = FORCED_CHARGING_DISABLED;

/** Policy decision: Whether charging is disabled/enabled */
static charging_state_t mch_charging_state = CHARGING_STATE_UNKNOWN;

/** Battery full seen */
static bool mch_battery_full = false;

/** Battery low threshold (allow charging) */
static gint mch_limit_enable = MCE_DEFAULT_CHARGING_LIMIT_ENABLE;
static guint mch_limit_enable_id = 0;

/** Battery high threshold (disable charging) */
static gint mch_limit_disable = MCE_DEFAULT_CHARGING_LIMIT_DISABLE;
static guint mch_limit_disable_id = 0;

/** Path to charging control sysfs file */
static gchar *mch_control_path = 0;

/** Value to write when enabling charging */
static gchar *mch_control_enable_value = 0;

/** Value to write when disabling charging */
static gchar *mch_control_disable_value = 0;

/* ========================================================================= *
 * FORCED_CHARGING
 * ========================================================================= */

/** Convert forced_charging_t enum values to human readable string
 *
 * @param value  forced_charging_t enumeration value
 *
 * @return human readable representation of enumeration value
 */
const char *
forced_charging_repr(forced_charging_t value)
{
    const char *repr = MCE_FORCED_CHARGING_UNKNOWN;
    switch( value ) {
    case FORCED_CHARGING_ENABLED:  repr = MCE_FORCED_CHARGING_ENABLED;  break;
    case FORCED_CHARGING_DISABLED: repr = MCE_FORCED_CHARGING_DISABLED; break;
    default: break;
    }
    return repr;
}

/** Convert human readable string to forced_charging_t enum value
 *
 * @param repr  human readable forced charging mode
 *
 * @return forced_charging_t enum value, or FORCED_CHARGING_UNKNOWN
 */
forced_charging_t
forced_charging_parse(const char *repr)
{
    forced_charging_t value = FORCED_CHARGING_UNKNOWN;
    if( !g_strcmp0(repr, MCE_FORCED_CHARGING_ENABLED) )
        value = FORCED_CHARGING_ENABLED;
    else if( !g_strcmp0(repr, MCE_FORCED_CHARGING_DISABLED) )
        value = FORCED_CHARGING_DISABLED;
    else if( g_strcmp0(repr, MCE_FORCED_CHARGING_UNKNOWN) )
        mce_log(LL_WARN, "invalid forced_charging value '%s'", repr ?: "<null>");
    return value;
}

/* ========================================================================= *
 * CHARGING_MODE
 * ========================================================================= */

static const char *
charging_mode_repr(charging_mode_t mode)
{
    const char *repr = "invalid";
    switch( mode ) {
    case CHARGING_MODE_DISABLE:
      repr = "disable" ;
      break;
    case CHARGING_MODE_ENABLE:
      repr = "enable";
      break;
    case CHARGING_MODE_APPLY_THRESHOLDS:
      repr = "apply_thresholds";
      break;
    case CHARGING_MODE_APPLY_THRESHOLDS_AFTER_FULL:
      repr = "apply_thresholds_after_full";
      break;
    default:
      break;
    }
    return repr;
}

/* ========================================================================= *
 * CHARGING_STATE
 * ========================================================================= */

static const char *
charging_state_repr(charging_state_t state)
{
    const char *repr = "invalid";

    switch( state ) {
    case CHARGING_STATE_UNKNOWN:  repr = "unknown";  break;
    case CHARGING_STATE_ENABLED:  repr = "allowed";  break;
    case CHARGING_STATE_DISABLED: repr = "disabled"; break;
    default: break;
    }

    return repr;
}

/* ========================================================================= *
 * UTILITY
 * ========================================================================= */

static int
mch_clamp(int value, int minval, int maxval)
{
    return (value < minval) ? minval : (value > maxval) ? maxval : value;
}

/* ========================================================================= *
 * MCH_SYSFS
 * ========================================================================= */

static bool
mch_sysfs_write(const char *path, const char *text)
{
    bool ack = false;
    int  fd  = -1;

    if( !path || !text )
        goto EXIT;

    if( (fd = open(path, O_WRONLY)) == -1 ) {
        mce_log(LL_ERR, "can't open %s: %m", path);
        goto EXIT;
    }

    size_t todo = strlen(text);
    ssize_t done = write(fd, text, todo);

    if( done == -1 ) {
        mce_log(LL_ERR, "can't write to %s: %m", path);
        goto EXIT;
    }

    if( done != (ssize_t)todo ) {
        mce_log(LL_ERR, "can't write to %s: partial success", path);
        goto EXIT;
    }

    mce_log(LL_DEBUG, "set %s to %s", path, text);

    ack = true;

EXIT:
    if( fd != -1 )
        close(fd);

    return ack;
}

/* ========================================================================= *
 * MCH_POLICY
 * ========================================================================= */

static void
mch_policy_set_battery_full(bool battery_full)
{
    if( mch_battery_full == battery_full )
        goto EXIT;

    mce_log(LL_DEBUG, "mch_battery_full: %s -> %s",
            mch_battery_full ? "true" : "false",
            battery_full     ? "true" : "false");

    mch_battery_full = battery_full;

    /* No action */

EXIT:
    return;
}

static void
mch_policy_set_charging_state(charging_state_t charging_state)
{
    if( charging_state != CHARGING_STATE_DISABLED ) {
        /* CHARGING_STATE_UNKNOWN is valid only as initial state */
        charging_state = CHARGING_STATE_ENABLED;
    }
    else if( !mch_control_path ) {
        /* No control path -> can't disable -> report as enabled */
        charging_state = CHARGING_STATE_ENABLED;
    }

    if( mch_charging_state == charging_state )
        goto EXIT;

    mce_log(LL_CRUCIAL, "mch_charging_state: %s -> %s",
            charging_state_repr(mch_charging_state),
            charging_state_repr(charging_state));

    mch_charging_state = charging_state;

    mch_sysfs_write(mch_control_path,
                    mch_charging_state == CHARGING_STATE_DISABLED ?
                    mch_control_disable_value :
                    mch_control_enable_value);

    mch_dbus_send_charging_state(0);
EXIT:
    return;
}

static void
mch_policy_evaluate_charging_state(void)
{
    /* Default to retaining current state */
    charging_state_t charging_state = mch_charging_state;

    /* Sanitize limits before use */
    int limit_enable  = mch_clamp(mch_limit_enable, 0, 100);
    int limit_disable = mch_clamp(mch_limit_disable, 0, 100);
    if( limit_disable <= limit_enable )
        limit_disable = 100;

    if( usb_cable_state == USB_CABLE_DISCONNECTED ) {
        /* Clear battery full seen on disconnect */
        mch_policy_set_battery_full(false);

        switch( mch_charging_mode ) {
        default:
            /* Return to defaults */
            charging_state = CHARGING_STATE_ENABLED;
            break;

        case CHARGING_MODE_DISABLE:
            /* Keep disabled */
            charging_state = CHARGING_STATE_DISABLED;
            break;
        }
    }
    else {
        /* Remember if battery full has been observed since:
         * charger was disconnected, charging mode was changed,
         * or forced charging was enabled.
         *
         * Note that for the purposes of this module reaching 100%
         * battery capacity is enough and there is no need to wait
         * for kernel to explictly declare battery fully charged.
         */
        if( battery_status == BATTERY_STATUS_FULL || battery_level >= 100 )
            mch_policy_set_battery_full(true);

        /* Evaluate based on active mode */
        switch( mch_charging_mode ) {
        case CHARGING_MODE_DISABLE:
            /* Keep disabled */
            charging_state = CHARGING_STATE_DISABLED;
            break;

        default:
        case CHARGING_MODE_ENABLE:
            /* Use defaults */
            charging_state = CHARGING_STATE_ENABLED;
            break;

        case CHARGING_MODE_APPLY_THRESHOLDS_AFTER_FULL:
            if( !mch_battery_full ) {
                /* Use defaults while waiting for battery full */
                charging_state = CHARGING_STATE_ENABLED;
                break;
            }
            /* Fall through */

        case CHARGING_MODE_APPLY_THRESHOLDS:
            if( battery_level <= limit_enable ) {
                /* Enable when dropped below low limit */
                charging_state = CHARGING_STATE_ENABLED;
            }
            else if( battery_level >= limit_disable ) {
                /* Disable when raises above high limit */
                charging_state = CHARGING_STATE_DISABLED;
            }
            break;
        }
    }

    /* Handle "charge once to full" override */
    if( mch_forced_charging != FORCED_CHARGING_DISABLED ) {
        /* Automatically disable on charger disconnect / battery full */
        if( usb_cable_state == USB_CABLE_DISCONNECTED ||  mch_battery_full )
            mch_policy_set_forced_charging_ex(FORCED_CHARGING_DISABLED, false);

        /* If enabled, override policy decision made above */
        if( mch_forced_charging == FORCED_CHARGING_ENABLED )
            charging_state = CHARGING_STATE_ENABLED;
    }

    /* In any case, do not allow battery to get too empty */
    if( battery_level < MCH_MINIMUM_BATTERY_LEVEL )
            charging_state = CHARGING_STATE_ENABLED;

    /* Update control value */
    mch_policy_set_charging_state(charging_state);
}

static void mch_policy_set_charging_mode(charging_mode_t charging_mode)
{
    if( mch_charging_mode == charging_mode )
        goto EXIT;

    mce_log(LL_CRUCIAL, "mch_charging_mode: %s -> %s",
            charging_mode_repr(mch_charging_mode),
            charging_mode_repr(charging_mode));

    mch_charging_mode = charging_mode;

    /* Clear battery-full-seen on mode change */
    mch_policy_set_battery_full(false);

    /* Clear forced charging on mode change */
    mch_policy_set_forced_charging_ex(FORCED_CHARGING_DISABLED, false);

    mch_policy_evaluate_charging_state();

EXIT:
    return;
}

static void mch_policy_set_limit_disable(int limit_disable)
{
    if( mch_limit_disable == limit_disable )
        goto EXIT;

    mce_log(LL_CRUCIAL, "mch_limit_disable: %d -> %d",
            mch_limit_disable,
            limit_disable);

    mch_limit_disable = limit_disable;

    mch_policy_evaluate_charging_state();

EXIT:
    return;
}

static void mch_policy_set_limit_enable(int limit_enable)
{
    if( mch_limit_enable == limit_enable )
        goto EXIT;

    mce_log(LL_CRUCIAL, "mch_limit_enable: %d -> %d",
            mch_limit_enable,
            limit_enable);

    mch_limit_enable = limit_enable;

    mch_policy_evaluate_charging_state();

EXIT:
    return;
}

static void
mch_policy_set_forced_charging_ex(forced_charging_t forced_charging,
                                  bool evaluate_state)
{
    if( mch_forced_charging == forced_charging )
        goto EXIT;

    mce_log(LL_CRUCIAL, "mch_forced_charging: %s -> %s",
            forced_charging_repr(mch_forced_charging),
            forced_charging_repr(forced_charging));

    mch_forced_charging = forced_charging;

    /* Clear battery-full-seen on forced-charging enable */
    if( mch_forced_charging == FORCED_CHARGING_ENABLED )
        mch_policy_set_battery_full(false);

    mch_dbus_send_forced_charging_state(0);

    if( evaluate_state )
        mch_policy_evaluate_charging_state();

EXIT:
    return;
}

static void
mch_policy_set_forced_charging(forced_charging_t forced_charging)
{
    mch_policy_set_forced_charging_ex(forced_charging, true);
}

/* ========================================================================= *
 * MCH_SETTINGS
 * ========================================================================= */

static void
mch_settings_cb(GConfClient *const gcc, const guint id,
               GConfEntry *const entry, gpointer const data)
{
    (void)gcc;
    (void)data;

    const GConfValue *gcv = gconf_entry_get_value(entry);

    if( !gcv ) {
        mce_log(LL_DEBUG, "GConf Key `%s' has been unset",
                gconf_entry_get_key(entry));
        goto EXIT;
    }

    if( id == mch_charging_mode_id ) {
        mch_policy_set_charging_mode(gconf_value_get_int(gcv));
    }
    else if( id == mch_limit_disable_id ) {
        mch_policy_set_limit_disable(gconf_value_get_int(gcv));
    }
    else if( id == mch_limit_enable_id ) {
        mch_policy_set_limit_enable(gconf_value_get_int(gcv));
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }

EXIT:

    return;
}

static void
mch_settings_init(void)
{
    mce_setting_track_int(MCE_SETTING_CHARGING_LIMIT_ENABLE,
                          &mch_limit_enable,
                          MCE_DEFAULT_CHARGING_LIMIT_ENABLE,
                          mch_settings_cb,
                          &mch_limit_enable_id);

    mce_setting_track_int(MCE_SETTING_CHARGING_LIMIT_DISABLE,
                          &mch_limit_disable,
                          MCE_DEFAULT_CHARGING_LIMIT_DISABLE,
                          mch_settings_cb,
                          &mch_limit_disable_id);

    gint charging_mode = 0;
    mce_setting_track_int(MCE_SETTING_CHARGING_MODE,
                          &charging_mode,
                          MCE_DEFAULT_CHARGING_MODE,
                          mch_settings_cb,
                          &mch_charging_mode_id);
    mch_charging_mode = charging_mode;
}

static void
mch_settings_quit(void)
{
    mce_setting_notifier_remove(mch_limit_enable_id),
        mch_limit_enable_id = 0;

    mce_setting_notifier_remove(mch_limit_disable_id),
        mch_limit_disable_id = 0;

    mce_setting_notifier_remove(mch_charging_mode_id),
        mch_charging_mode_id = 0;
}

/* ========================================================================= *
 * MCH_CONFIG
 * ========================================================================= */

static const struct {
    const char *control_path;
    const char *enable_value;
    const char *disable_value;
} mch_autoconfig[] = {
    {
        .control_path  = "/sys/class/power_supply/battery/charging_enabled",
        .enable_value  = "1",
        .disable_value = "0",
    },
    {
        .control_path  = "/sys/class/power_supply/battery/input_suspend",
        .enable_value  = "0",
        .disable_value = "1",
    },
    {
        .control_path = NULL,
    }
};

static void
mch_config_init(void)
{
    bool ack = false;

    if( mce_conf_has_group(MCE_CONF_CHARGING_GROUP) ) {
        mch_control_path =
            mce_conf_get_string(MCE_CONF_CHARGING_GROUP,
                                MCE_CONF_CHARGING_CONTROL_PATH,
                                NULL);
    }

    if( mch_control_path ) {
        if( access(mch_control_path, W_OK) == -1 ) {
            mce_log(LL_ERR, "%s: not writable: %m", mch_control_path);
            goto EXIT;
        }
        mch_control_enable_value =
            mce_conf_get_string(MCE_CONF_CHARGING_GROUP,
                                MCE_CONF_CHARGING_ENABLE_VALUE,
                                DEFAULT_CHARGING_ENABLE_VALUE);

        mch_control_disable_value =
            mce_conf_get_string(MCE_CONF_CHARGING_GROUP,
                                MCE_CONF_CHARGING_DISABLE_VALUE,
                                DEFAULT_CHARGING_DISABLE_VALUE);
    }
    else {
        for( size_t i = 0; ; ++i ) {
            if( !mch_autoconfig[i].control_path )
                goto EXIT;

            if( access(mch_autoconfig[i].control_path, W_OK) == -1 )
                continue;

            mch_control_path = g_strdup(mch_autoconfig[i].control_path);
            mch_control_enable_value =
                g_strdup(mch_autoconfig[i].enable_value);
            mch_control_disable_value =
                g_strdup(mch_autoconfig[i].disable_value);
            break;
        }
    }

    ack = true;

EXIT:
    if( !ack )
        mch_config_quit();

    mce_log(LL_DEBUG, "control: %s", mch_control_path          ?: "N/A");
    mce_log(LL_DEBUG, "enable:  %s", mch_control_enable_value  ?: "N/A");
    mce_log(LL_DEBUG, "disable: %s", mch_control_disable_value ?: "N/A");

    return;
}

static void
mch_config_quit(void)
{
    g_free(mch_control_path),
        mch_control_path = 0;

    g_free(mch_control_enable_value),
        mch_control_enable_value = 0;

    g_free(mch_control_disable_value),
        mch_control_disable_value = 0;
}

/* ========================================================================= *
 * MCH_DATAPIPE
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * usb_cable_state
 * ------------------------------------------------------------------------- */

/** Callback for handling usb_cable_state_pipe state changes
 *
 * @param data usb_cable_state (as void pointer)
 */
static void
mch_datapipe_usb_cable_state_cb(gconstpointer data)
{
    usb_cable_state_t prev = usb_cable_state;
    usb_cable_state = GPOINTER_TO_INT(data);

    if( usb_cable_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "usb_cable_state = %s -> %s",
            usb_cable_state_repr(prev),
            usb_cable_state_repr(usb_cable_state));

    mch_policy_evaluate_charging_state();

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
static void
mch_datapipe_charger_state_cb(gconstpointer data)
{
    charger_state_t prev = charger_state;
    charger_state = GPOINTER_TO_INT(data);

    if( charger_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "charger_state = %s -> %s",
            charger_state_repr(prev),
            charger_state_repr(charger_state));

    mch_policy_evaluate_charging_state();

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
static void
mch_datapipe_battery_status_cb(gconstpointer data)
{
    battery_status_t prev = battery_status;
    battery_status = GPOINTER_TO_INT(data);

    if( battery_status == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "battery_status = %s -> %s",
            battery_status_repr(prev),
            battery_status_repr(battery_status));

    mch_policy_evaluate_charging_state();

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
static void
mch_datapipe_battery_level_cb(gconstpointer data)
{
    gint prev = battery_level;
    battery_level = GPOINTER_TO_INT(data);

    if( battery_level == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "battery_level = %d -> %d",
            prev, battery_level);

    mch_policy_evaluate_charging_state();

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * setup
 * ------------------------------------------------------------------------- */

/** Array of datapipe handlers */
static datapipe_handler_t mch_datapipe_handlers[] =
{
    /* Output Triggers */
    {
        .datapipe  = &usb_cable_state_pipe,
        .output_cb = mch_datapipe_usb_cable_state_cb,
    },
    {
        .datapipe  = &charger_state_pipe,
        .output_cb = mch_datapipe_charger_state_cb,
    },
    {
        .datapipe  = &battery_status_pipe,
        .output_cb = mch_datapipe_battery_status_cb,
    },
    {
        .datapipe  = &battery_level_pipe,
        .output_cb = mch_datapipe_battery_level_cb,
    },
    /* Sentinel */
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t mch_datapipe_bindings =
{
    .module   = MODULE_NAME,
    .handlers = mch_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void mch_datapipe_init(void)
{
    mce_datapipe_init_bindings(&mch_datapipe_bindings);
}

/** Remove triggers/filters from datapipes */
static void mch_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&mch_datapipe_bindings);
}

/* ========================================================================= *
 * MCH_DBUS
 * ========================================================================= */

/** Send charging_state D-Bus signal / method call reply
 *
 * @param req  method call message to reply, or NULL to send signal
 */
static void
mch_dbus_send_charging_state(DBusMessage *const req)
{
    const char * const lut[] =  {
        [CHARGING_STATE_DISABLED] = MCE_CHARGING_STATE_DISABLED,
        [CHARGING_STATE_ENABLED]  = MCE_CHARGING_STATE_ENABLED,
        [CHARGING_STATE_UNKNOWN]  = MCE_CHARGING_STATE_UNKNOWN,
    };

    static const char *last = 0;

    DBusMessage *msg = NULL;

    const char *value = lut[mch_charging_state];

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
                              MCE_CHARGING_STATE_SIG);
    }

    if( !dbus_message_append_args(msg,
                                  DBUS_TYPE_STRING, &value,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    mce_log(LL_DEBUG, "%s: %s = %s",
            req ? "reply" : "broadcast",
            "charging_state", value);

    dbus_send_message(msg), msg = 0;

EXIT:

    if( msg )
        dbus_message_unref(msg);
}

/** Callback for handling charging_state D-Bus queries
 *
 * @param req  method call message to reply
 */
static gboolean
mch_dbus_get_charging_state_cb(DBusMessage *const req)
{
    mce_log(LL_DEBUG, "charging_state query from: %s",
            mce_dbus_get_message_sender_ident(req));

    if( !dbus_message_get_no_reply(req) )
        mch_dbus_send_charging_state(req);

    return TRUE;
}

/** Send forced_charging_state D-Bus signal / method call reply
 *
 * @param req  method call message to reply, or NULL to send signal
 */
static void
mch_dbus_send_forced_charging_state(DBusMessage *const req)
{
    static const char *last = 0;

    DBusMessage *msg = NULL;

    const char *value = forced_charging_repr(mch_forced_charging);

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
                              MCE_FORCED_CHARGING_SIG);
    }

    if( !dbus_message_append_args(msg,
                                  DBUS_TYPE_STRING, &value,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    mce_log(LL_DEBUG, "%s: %s = %s",
            req ? "reply" : "broadcast",
            "forced_charging_state", value);

    dbus_send_message(msg), msg = 0;

EXIT:

    if( msg )
        dbus_message_unref(msg);
}

/** Callback for handling forced_charging_state D-Bus queries
 *
 * @param req  method call message to reply
 */
static gboolean
mch_dbus_get_forced_charging_state_cb(DBusMessage *const req)
{
    mce_log(LL_DEBUG, "forced_charging_state query from: %s",
            mce_dbus_get_message_sender_ident(req));

    if( !dbus_message_get_no_reply(req) )
        mch_dbus_send_forced_charging_state(req);

    return TRUE;
}

/** Callback for handling forced_charging_state D-Bus requests
 *
 * @param req  method call message to reply
 */
static gboolean
mch_dbus_set_forced_charging_state_cb(DBusMessage *const req)
{
    DBusMessage   *rsp    = 0;
    DBusError      err    = DBUS_ERROR_INIT;
    const char    *arg    = 0;

    mce_log(LL_DEBUG, "forced_charging_state request from: %s",
            mce_dbus_get_message_sender_ident(req));

    if( !dbus_message_get_args(req, &err,
                               DBUS_TYPE_STRING, &arg,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to get argument from %s.%s: %s: %s",
                MCE_REQUEST_IF, MCE_FORCED_CHARGING_REQ,
                err.name, err.message);
        rsp = dbus_message_new_error(req, err.name, err.message);
        goto EXIT;
    }

    forced_charging_t value = forced_charging_parse(arg);
    if( value == FORCED_CHARGING_UNKNOWN ) {
        rsp = dbus_message_new_error_printf(req, DBUS_ERROR_INVALID_ARGS,
                                            "Invalid forced charging state \"%s\" requested",
                                            arg);
        goto EXIT;
    }

    mch_policy_set_forced_charging(value);

EXIT:
    if( !dbus_message_get_no_reply(req) ) {
        if( !rsp )
            rsp = dbus_new_method_reply(req);
        dbus_send_message(rsp), rsp = 0;
    }

    if( rsp )
        dbus_message_unref(rsp);

    dbus_error_free(&err);

    return TRUE;
}

/** Callback for handling charging suspendable D-Bus queries
 *
 * @param req  method call message to reply
 */
static gboolean
mch_dbus_get_charging_suspendable_cb(DBusMessage *const req)
{
    DBusMessage *rsp = NULL;
    dbus_bool_t  val = (mch_control_path != NULL);

    mce_log(LL_DEBUG, "%s query from: %s",
            dbus_message_get_member(req),
            mce_dbus_get_message_sender_ident(req));

    if( !(rsp = dbus_new_method_reply(req)) )
        goto EXIT;

    if( !dbus_message_append_args(rsp,
                                  DBUS_TYPE_BOOLEAN, &val,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    mce_log(LL_DEBUG, "%s reply: %s",
            dbus_message_get_member(req),
            val ? "true" : "false");

    if( !dbus_message_get_no_reply(req) )
        dbus_send_message(rsp), rsp = 0;

EXIT:

    if( rsp )
        dbus_message_unref(rsp);

    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t mch_dbus_handlers[] =
{
    /* signals - outbound (for Introspect purposes only) */
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_CHARGING_STATE_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
        "    <arg name=\"charging_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_FORCED_CHARGING_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
        "    <arg name=\"forced_charging_state\" type=\"s\"/>\n"
    },
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_CHARGING_STATE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mch_dbus_get_charging_state_cb,
        .args      =
        "    <arg direction=\"out\" name=\"charging_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_FORCED_CHARGING_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mch_dbus_get_forced_charging_state_cb,
        .args      =
        "    <arg direction=\"out\" name=\"forced_charging_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_FORCED_CHARGING_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mch_dbus_set_forced_charging_state_cb,
        .args      =
        "    <arg direction=\"in\" name=\"forced_charging_state\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_CHARGING_SUSPENDABLE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = mch_dbus_get_charging_suspendable_cb,
        .args      =
        "    <arg direction=\"out\" name=\"charging_suspendable\" type=\"b\"/>\n"
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Timer callback id for broadcasting initial states */
static guint mch_dbus_initial_id = 0;

/** Timer callback function for broadcasting initial states
 *
 * @param aptr (not used)
 *
 * @return FALSE to stop idle callback from repeating
 */
static gboolean mch_dbus_initial_cb(gpointer aptr)
{
    (void)aptr;
    mch_dbus_initial_id = 0;

    mch_dbus_send_charging_state(0);
    mch_dbus_send_forced_charging_state(0);
    return FALSE;
}

/** Add dbus handlers
 */
static void mch_dbus_init(void)
{
    mce_dbus_handler_register_array(mch_dbus_handlers);

    /* To avoid unnecessary jitter on startup, allow dbus service tracking
     * and datapipe initialization some time to come up with proper initial
     * state values before forcing broadcasting to dbus */
    if( !mch_dbus_initial_id )
        mch_dbus_initial_id = g_timeout_add(1000, mch_dbus_initial_cb, 0);
}

/** Remove dbus handlers
 */
static void mch_dbus_quit(void)
{
    if( mch_dbus_initial_id ) {
        g_source_remove(mch_dbus_initial_id),
            mch_dbus_initial_id = 0;
    }

    mce_dbus_handler_unregister_array(mch_dbus_handlers);
}

/* ========================================================================= *
 * G_MODULE
 * ========================================================================= */

/** Init function for the charging module
 *
 * @param module Unused
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    mch_config_init();
    mch_settings_init();
    mch_datapipe_init();
    mch_dbus_init();

    mch_policy_evaluate_charging_state();

    return NULL;
}

/** Exit function for the charging module
 *
 * @param module Unused
 */
void g_module_unload(GModule *module)
{
    (void)module;

    mch_dbus_quit();
    mch_datapipe_quit();
    mch_settings_quit();
    mch_config_quit();

    return;
}
