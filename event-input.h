/**
 * @file event-input.h
 * Headers for the /dev/input event provider for the Mode Control Entity
 * <p>
 * Copyright © 2007-2010 Nokia Corporation and/or its subsidiary(-ies).
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

#include <linux/input.h>	/* KEY_POWER */

/** Path to the input device directory */
#define DEV_INPUT_PATH			"/dev/input"
/** Prefix for event files */
#define EVENT_FILE_PREFIX		"event"
/** Path to the GPIO key disable interface */
#define GPIO_KEY_DISABLE_PATH		"/sys/devices/platform/gpio-keys/disabled_keys"

/* XXX:
 * We should probably use
 * /dev/input/keypad
 * /dev/input/gpio-keys
 * /dev/input/pwrbutton
 * /dev/input/ts
 * and add whitelist entries for misc devices instead
 */

/**
 * List of drivers that provide touchscreen events
 * XXX: If this is made case insensitive,
 *      we could search for "* touchscreen" instead
 */
static const gchar *const touchscreen_event_drivers[] = {
	/** Input layer name for the Atmel mXT touchscreen */
	"Atmel mXT Touchscreen",

	/** Input layer name for the Atmel QT602240 touchscreen */
	"Atmel QT602240 Touchscreen",

	/** TSC2005 touchscreen */
	"TSC2005 touchscreen",

	/** TSC2301 touchscreen */
	"TSC2301 touchscreen",

	/** ADS784x touchscreen */
	"ADS784x touchscreen",

	/** No more entries */
	NULL
};

/**
 * List of drivers that provide keyboard events
 */
static const gchar *const keyboard_event_drivers[] = {
	/** Input layer name for the TWL4030 keyboard/keypad */
	"TWL4030 Keypad",

	/** Legacy input layer name for the TWL4030 keyboard/keypad */
	"omap_twl4030keypad",

	/** Generic input layer name for keyboard/keypad */
	"Internal keyboard",

	/** Input layer name for the LM8323 keypad */
	"LM8323 keypad",

	/** Generic input layer name for keypad */
	"Internal keypad",

	/** Input layer name for the TSC2301 keypad */
	"TSC2301 keypad",

	/** Legacy generic input layer name for keypad */
	"omap-keypad",

	/** Input layer name for standard PC keyboards */
	"AT Translated Set 2 keyboard",

	/** Input layer name for the TWL4030 power button */
	"twl4030_pwrbutton",

	/** Input layer name for the Triton 2 power button */
	"triton2-pwrbutton",

	/** Input layer name for the Retu powerbutton */
	"retu-pwrbutton",

	/** Input layer name for the PC Power button */
	"Power Button",

	/** Input layer name for the PC Sleep button */
	"Sleep Button",

	/** Input layer name for the Thinkpad extra buttons */
	"Thinkpad Extra Buttons",

	/** Input layer name for ACPI virtual keyboard */
	"ACPI Virtual Keyboard Device",

	/** Input layer name for GPIO-keys */
	"gpio-keys",

	/** Input layer name for DFL-61/TWL4030 jack sense */
	"dfl61-twl4030 Jack",

	/** Legacy input layer name for TWL4030 jack sense */
	"rx71-twl4030 Jack",

	/** Input layer name for PC Lid switch */
	"Lid Switch",

	/** No more entries */
	NULL
};

/**
 * List of drivers that we should not monitor
 */
static const gchar *const driver_blacklist[] = {
	/** Input layer name for the AMI305 magnetometer */
	"ami305 magnetometer",

	/** Input layer name for the ST LIS3LV02DL accelerometer */
	"ST LIS3LV02DL Accelerometer",

	/** Input layer name for the ST LIS302DL accelerometer */
	"ST LIS302DL Accelerometer",

	/** Input layer name for the TWL4030 vibrator */
	"twl4030:vibrator",

	/** Input layer name for AV accessory */
	"AV Accessory",

	/** Input layer name for the video bus */
	"Video Bus",

	/** Input layer name for the PC speaker */
	"PC Speaker",

	/** Input layer name for the Intel HDA headphone */
	"HDA Intel Headphone",

	/** Input layer name for the Intel HDA microphone */
	"HDA Intel Mic",

	/** Input layer name for the UVC 17ef:4807 webcam in thinkpad X301 */
	"UVC Camera (17ef:4807)",

	/** Input layer name for the UVC 17ef:480c webcam in thinkpad X301si */
	"UVC Camera (17ef:480c)",

	/** No more entries */
	NULL
};

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
