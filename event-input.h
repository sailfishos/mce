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
#ifndef EVENT_INPUT_H_
# define EVENT_INPUT_H_

# include <glib.h>

/* ========================================================================= *
 * Constants
 * ========================================================================= */

/** Path to the input device directory */
# define DEV_INPUT_PATH                 "/dev/input"

/** Prefix for event files */
# define EVENT_FILE_PREFIX              "event"

/** Path to the GPIO key disable interface */
# define GPIO_KEY_DISABLE_PATH          "/sys/devices/platform/gpio-keys/disabled_keys"

/* ========================================================================= *
 * Settings
 * ========================================================================= */

/** Path to the GConf settings for the event input */
# define MCE_SETTING_EVENT_INPUT_PATH       "/system/osso/dsm/event_input"

# ifdef ENABLE_DOUBLETAP_EMULATION
/** Path to the use Fake Double Tap setting */
#  define MCE_SETTING_USE_FAKE_DOUBLETAP    MCE_SETTING_EVENT_INPUT_PATH "/use_fake_double_tap"
#  define MCE_DEFAULT_USE_FAKE_DOUBLETAP    true
# endif

/** Path to the touch unblock delay setting */
# define MCE_SETTING_TOUCH_UNBLOCK_DELAY    MCE_SETTING_EVENT_INPUT_PATH "/touch_unblock_delay"
# define MCE_DEFAULT_TOUCH_UNBLOCK_DELAY    100

/** Input device grabbing allowed setting */
# define MCE_SETTING_INPUT_GRAB_ALLOWED     MCE_SETTING_EVENT_INPUT_PATH "/input_grab_allowed"
# define MCE_INPUT_GRAB_ALLOW_NONE          (0)
# define MCE_INPUT_GRAB_ALLOW_TS            (1<<0)
# define MCE_INPUT_GRAB_ALLOW_KP            (1<<1)
# define MCE_DEFAULT_INPUT_GRAB_ALLOWED     3 // = MCE_INPUT_GRAB_ALLOW_TS | KP

/* ========================================================================= *
 * Functions
 * ========================================================================= */

gboolean mce_input_init(void);
void     mce_input_exit(void);

#endif /* EVENT_INPUT_H_ */
