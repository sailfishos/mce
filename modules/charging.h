/**
 * @file charging.h
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

#ifndef  CHARGING_H_
# define CHARGING_H_

/* ========================================================================= *
 * Static configuration
 * ========================================================================= */

/** Group for charging configuration keys */
# define MCE_CONF_CHARGING_GROUP                "Charging"

/** Control file where to write */
# define MCE_CONF_CHARGING_CONTROL_PATH         "ControlPath"
# define DEFAULT_CHARGING_CONTROL_PATH          NULL

/* Value to write when enabling */
# define MCE_CONF_CHARGING_ENABLE_VALUE         "EnableValue"
# define DEFAULT_CHARGING_ENABLE_VALUE          "1"

/* Value to write when disabling */
# define MCE_CONF_CHARGING_DISABLE_VALUE        "DisableValue"
# define DEFAULT_CHARGING_DISABLE_VALUE         "0"

/* ========================================================================= *
 * Dynamic settings
 * ========================================================================= */

/** Prefix for charging setting keys */
# define MCE_SETTING_CHARGING_PATH              "/system/osso/dsm/charging"

/** Charging disable/enable mode */
# define MCE_SETTING_CHARGING_MODE              MCE_SETTING_CHARGING_PATH "/charging_mode"
# define MCE_DEFAULT_CHARGING_MODE              1 // = CHARGING_MODE_ENABLE

/** Battery level at which to disable charging
 *
 * The value is dictated by hardcoded expectations in settings ui.
 */
# define MCE_SETTING_CHARGING_LIMIT_DISABLE     MCE_SETTING_CHARGING_PATH "/limit_disable"
# define MCE_DEFAULT_CHARGING_LIMIT_DISABLE     90

/** Battery level at which to enable charging
 *
 * The value is dictated by hardcoded expectations in settings ui.
 */
# define MCE_SETTING_CHARGING_LIMIT_ENABLE      MCE_SETTING_CHARGING_PATH "/limit_enable"
# define MCE_DEFAULT_CHARGING_LIMIT_ENABLE      87 // = MCE_DEFAULT_CHARGING_LIMIT_DISABLE - 3

/* ========================================================================= *
 * Types
 * ========================================================================= */

typedef enum
{
    /* Keep charger disabled */
    CHARGING_MODE_DISABLE,

    /* Keep charger enabled (default behavior) */
    CHARGING_MODE_ENABLE,

    /* Apply thresholds without waiting for battery full */
    CHARGING_MODE_APPLY_THRESHOLDS,

    /* Apply thresholds after battery full is reached */
    CHARGING_MODE_APPLY_THRESHOLDS_AFTER_FULL,
} charging_mode_t;

typedef enum
{
    /** Battery should not be charged */
    CHARGING_STATE_DISABLED,

    /** Charging logic decides whether to charge or not */
    CHARGING_STATE_ENABLED,

    /** Placeholder values used during initialization */
    CHARGING_STATE_UNKNOWN,
} charging_state_t;

#endif /* CHARGING_H_ */
