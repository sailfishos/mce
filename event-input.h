/**
 * @file event-input.h
 * Headers for the /dev/input event provider for the Mode Control Entity
 * <p>
 * Copyright © 2007-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2013-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
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

/** Name of the evdev event mapping configuration group
 *
 * This configuration group can be used to remap the events
 * sent by the kernel to the ones expected by mce.
 *
 * Items in the group are of form:
 *
 * <event_emitted_by_kernel>=<event_expected_by_mce>
 *
 * Where both event_emitted_by_kernel and event_expected_by_mce are event
 * code names such: as SW_LID, SW_KEYPAD_SLIDE, ...
 *
 * See inifiles/evdev.ini for details and examples.
 */
#define MCE_CONF_EVDEV_GROUP            "EVDEV"

/** Name of the evdev type configuration group
 *
 * This configuration group can be used to explicitly define input device
 * types in cases where probing heuristic in mce code fails to work.
 *
 * Items in the group are of form:
 *
 * <device_name>=<device_type>
 *
 * Where device_name is sanitized evdev device name obtained via EVIOCGNAME
 * ioctl(), and device_type is one of : TOUCH, VOLKEY, DBLTAP, ...
 *
 * See evio_sanitize_key_name() and evin_evdevtype_parse() for details.
 */
#define MCE_CONF_EVDEV_TYPE_GROUP       "EVDEV_TYPE"

/* ========================================================================= *
 * Settings
 * ========================================================================= */

/** Prefix for event input setting keys */
# define MCE_SETTING_EVENT_INPUT_PATH       "/system/osso/dsm/event_input"

# ifdef ENABLE_DOUBLETAP_EMULATION
/** Whether double tap emulation is enabled
 *
 * When enabled, mce does double tap detection from "normal" touch events
 * that are received in display off an lpm states.
 */
#  define MCE_SETTING_USE_FAKE_DOUBLETAP    MCE_SETTING_EVENT_INPUT_PATH "/use_fake_double_tap"
#  define MCE_DEFAULT_USE_FAKE_DOUBLETAP    true
# endif

/** How long to delay touch unblock after display power up [ms]
 *
 * This can be used to fine tune input policy to make it less
 * likely that accidental touch events reach ui during in-call
 * proximity blanking etc.
 */
# define MCE_SETTING_TOUCH_UNBLOCK_DELAY    MCE_SETTING_EVENT_INPUT_PATH "/touch_unblock_delay"
# define MCE_DEFAULT_TOUCH_UNBLOCK_DELAY    100

/** What kind of evdev sources mce is allowed to grab [bitmask]
 *
 * By default mce grabs evdev sources dealing with touch events and volume
 * keys under certain circumstances.
 *
 * If this is not desirable for some reason (for example touchscreens
 * that use multitouch protocol B for reporting should not be grabbed),
 * the default value of this setting should be overridden in hw adaptation
 * specific configuration.
 */
# define MCE_SETTING_INPUT_GRAB_ALLOWED     MCE_SETTING_EVENT_INPUT_PATH "/input_grab_allowed"
# define MCE_INPUT_GRAB_ALLOW_NONE          (0)
# define MCE_INPUT_GRAB_ALLOW_TS            (1<<0)
# define MCE_INPUT_GRAB_ALLOW_KP            (1<<1)
# define MCE_DEFAULT_INPUT_GRAB_ALLOWED     0 // = MCE_INPUT_GRAB_ALLOW_NONE

/* ========================================================================= *
 * Functions
 * ========================================================================= */

gboolean mce_input_init(void);
void     mce_input_exit(void);

#endif /* EVENT_INPUT_H_ */
