/**
 * @file powerkey.h
 * Headers for the power key logic for the Mode Control Entity
 * <p>
 * Copyright © 2004-2010 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
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
#ifndef _POWERKEY_H_
#define _POWERKEY_H_

#include <glib.h>

/** Path to the GConf settings for the powerkey module */
# define MCE_GCONF_POWERKEY_PATH       "/system/osso/dsm/powerkey"

/** Path to the powerkey  mode GConf setting */
# define MCE_GCONF_POWERKEY_MODE       MCE_GCONF_POWERKEY_PATH "/mode"

/** Power key action enable modes */
typedef enum
{
        /** Power key actions disabled */
        PWRKEY_ENABLE_NEVER,

        /** Power key actions always enabled */
        PWRKEY_ENABLE_ALWAYS,

        /** Power key actions enabled when PS is not covered */
        PWRKEY_ENABLE_NO_PROXIMITY,

        PWRKEY_ENABLE_DEFAULT = PWRKEY_ENABLE_ALWAYS,
} pwrkey_mode_t;

/** Configuration value used for the disabled policy */
#define POWER_DISABLED_STR				"disabled"
/** Configuration value used for the device menu policy */
#define POWER_MENU_STR					"menu"
/** Configuration value used for poweroff */
#define POWER_POWEROFF_STR				"poweroff"
/** Configuration value used for soft poweroff */
#define POWER_SOFT_POWEROFF_STR				"softpoweroff"
/** Configuration value used for tklock lock */
#define POWER_TKLOCK_UNLOCK_STR				"tklock-unlock"
/** Configuration value used for tklock unlock */
#define POWER_TKLOCK_LOCK_STR				"tklock-lock"
/** Configuration value used for tklock both */
#define POWER_TKLOCK_BOTH_STR				"tklock-both"
/** Configuration value used for D-Bus signal */
#define POWER_DBUS_SIGNAL_STR				"dbus-signal-"

/** Action to perform on [power] keypress */
typedef enum {
/** No action */
	POWER_DISABLED = 0,
/** Show device menu */
	POWER_MENU = 1,
/** Default for long press */
	DEFAULT_POWERKEY_LONG_ACTION = 2,
/** Shutdown */
	POWER_POWEROFF = 2,
/** Soft poweroff */
	POWER_SOFT_POWEROFF = 3,
/** Default for short press */
	DEFAULT_POWERKEY_SHORT_ACTION = 4,
/** Lock the TKLock if unlocked */
	POWER_TKLOCK_LOCK = 4,
/** Unlock the TKLock if locked */
	POWER_TKLOCK_UNLOCK = 5,
/** Lock the TKLock if unlocked, unlock the TKLock if locked */
	POWER_TKLOCK_BOTH = 6,
/** Default for double press */
	DEFAULT_POWERKEY_DOUBLE_ACTION = 7,
/** Send a D-Bus signal */
	POWER_DBUS_SIGNAL = 7
} poweraction_t;

/** Name of Powerkey configuration group */
#define MCE_CONF_POWERKEY_GROUP		"PowerKey"

/** Name of configuration key for medium [power] press delay */
#define MCE_CONF_POWERKEY_MEDIUM_DELAY	"PowerKeyMediumDelay"

/** Name of configuration key for long [power] press delay */
#define MCE_CONF_POWERKEY_LONG_DELAY	"PowerKeyLongDelay"

/** Name of configuration key for double [power] press delay */
#define MCE_CONF_POWERKEY_DOUBLE_DELAY	"PowerKeyDoubleDelay"

/** Name of configuration key for short [power] press action */
#define MCE_CONF_POWERKEY_SHORT_ACTION	"PowerKeyShortAction"

/** Name of configuration key for long [power] press action */
#define MCE_CONF_POWERKEY_LONG_ACTION	"PowerKeyLongAction"

/** Name of configuration key for double [power] press action */
#define MCE_CONF_POWERKEY_DOUBLE_ACTION	"PowerKeyDoubleAction"

/**
 * Long delay for the [power] button in milliseconds; 1.5 seconds
 */
#define DEFAULT_POWER_LONG_DELAY	1500

/** Medium delay for the [power] button in milliseconds; 1 second */
#define DEFAULT_POWER_MEDIUM_DELAY	1000

/** Double press timeout for the [power] button in milliseconds; 0.5 seconds */
#define DEFAULT_POWER_DOUBLE_DELAY	500

/* When MCE is made modular, this will be handled differently */
gboolean mce_powerkey_init(void);
void mce_powerkey_exit(void);

#endif /* _POWERKEY_H_ */
