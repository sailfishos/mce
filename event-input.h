/**
 * @file event-input.h
 * Headers for the /dev/input event provider for the Mode Control Entity
 * <p>
 * Copyright © 2007-2011 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _EVENT_INPUT_H_
#define _EVENT_INPUT_H_

#include <glib.h>

/** Path to the input device directory */
#define DEV_INPUT_PATH			"/dev/input"

/** Prefix for event files */
#define EVENT_FILE_PREFIX		"event"

/** Path to the GPIO key disable interface */
#define GPIO_KEY_DISABLE_PATH		"/sys/devices/platform/gpio-keys/disabled_keys"

/** Path to the GConf settings for the event input */
#define MCE_GCONF_EVENT_INPUT_PATH	"/system/osso/dsm/event_input"

#ifdef ENABLE_DOUBLETAP_EMULATION
/** Path to the use Fake Double Tap setting */
# define MCE_GCONF_USE_FAKE_DOUBLETAP_PATH MCE_GCONF_EVENT_INPUT_PATH "/use_fake_double_tap"
#endif

/** Path to the touch unblock delay setting */
#define MCE_GCONF_TOUCH_UNBLOCK_DELAY_PATH MCE_GCONF_EVENT_INPUT_PATH "/touch_unblock_delay"

/**
 * Delay between I/O monitoring setups and keypress repeats; 1 second
 */
#define MONITORING_DELAY		1

/** Name of Homekey configuration group */
#define MCE_CONF_HOMEKEY_GROUP		"HomeKey"

/** Name of configuration key for long [home] press delay */
#define MCE_CONF_HOMEKEY_LONG_DELAY	"HomeKeyLongDelay"

/** Name of configuration key for short [home] press action */
#define MCE_CONF_HOMEKEY_SHORT_ACTION	"HomeKeyShortAction"

/** Name of configuration key for long [home] press action */
#define MCE_CONF_HOMEKEY_LONG_ACTION	"HomeKeyLongAction"

/** Long delay for the [home] button in milliseconds */
#define DEFAULT_HOME_LONG_DELAY		800		/* 0.8 seconds */

/* When MCE is made modular, this will be handled differently */
gboolean mce_input_init(void);
void mce_input_exit(void);

#endif /* _EVENT_INPUT_H_ */
