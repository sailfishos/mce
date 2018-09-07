/**
 * @file keypad.h
 * Headers for the keypad module
 * <p>
 * Copyright © 2004-2009 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _KEYPAD_H_
#define _KEYPAD_H_

/** Path to keypad backlight fade-time /sys entry */
#define MCE_KEYPAD_BACKLIGHT_FADETIME_SYS_PATH          MCE_LED_DIRECT_SYS_PATH MCE_LED_COVER_PREFIX "/time"

/** Path to keyboard backlight /sys directory */
#define MCE_KEYBOARD_BACKLIGHT_SYS_PATH                 "/sys/class/leds/keyboard"

/** Path to the SysFS interface for the keyboard backlight fade-time */
#define MCE_KEYBOARD_BACKLIGHT_FADETIME_SYS_PATH        MCE_LED_DIRECT_SYS_PATH MCE_LED_KEYBOARD_PREFIX "/time"

/** Maximum Lysti backlight LED current */
#define MAXIMUM_LYSTI_BACKLIGHT_LED_CURRENT             50      /* 5 mA */

/** Default key backlight brightness */
#define DEFAULT_KEY_BACKLIGHT_LEVEL                     255

/** Default key backlight timeout in seconds */
#define DEFAULT_KEY_BACKLIGHT_TIMEOUT                   30      /* 30 s */

/** Default key backlight fade in time in milliseconds */
#define DEFAULT_KEY_BACKLIGHT_FADE_IN_TIME              250     /* 250 ms */

/** Default key backlight fade out time in milliseconds */
#define DEFAULT_KEY_BACKLIGHT_FADE_OUT_TIME             1000    /* 1000 ms */

#ifndef MCE_CONF_KEYPAD_GROUP
/** Name of Keypad configuration group */
# define MCE_CONF_KEYPAD_GROUP                          "KeyPad"
#endif

/** Name of configuration key for keyboard backlight timeout */
#define MCE_CONF_KEY_BACKLIGHT_TIMEOUT                  "BacklightTimeout"

/** Name of configuration key for keyboard backlight fade in time */
#define MCE_CONF_KEY_BACKLIGHT_FADE_IN_TIME             "BacklightFadeInTime"

/** Name of configuration key for keyboard backlight fade out time */
#define MCE_CONF_KEY_BACKLIGHT_FADE_OUT_TIME            "BacklightFadeOutTime"

/** Name of configuration key for keyboard backlight path */
#define MCE_CONF_KEY_BACKLIGHT_SYS_PATH                 "BrightnessDirectory"

#endif /* _KEYPAD_H_ */
