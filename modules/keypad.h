/**
 * @file keypad.h
 * Headers for the keypad module
 * <p>
 * Copyright © 2004-2009 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2014-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * @author Matti Lehtimäki <matti.lehtimaki@gmail.com>
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

/** Default key backlight brightness */
#define DEFAULT_KEYPAD_BACKLIGHT_LEVEL                  255

/** Default key backlight timeout in seconds */
#define DEFAULT_KEYPAD_BACKLIGHT_TIMEOUT                30 // [s]

/** Name of Keypad configuration group */
#define MCE_CONF_KEYPAD_GROUP                           "KeyPad"

/** Name of configuration key for keypad type */
#define MCE_CONF_KEYPAD_TYPE                            "Type"

/** Name of configuration key for keyboard backlight timeout */
#define MCE_CONF_KEYPAD_BACKLIGHT_TIMEOUT               "BacklightTimeout"

/** Name of configuration key for keyboard backlight brightness directories
 *
 * Acceptable values: directory paths that countain writable "brightness"
 * and readable "max_brightness" file.
 */
#define MCE_CONF_KEYPAD_BACKLIGHT_BRIGHTNESS_DIR        "BrightnessDirectory"

/** Name of configuration key for keyboard backlight binary brightness paths
 *
 * Acceptable values: paths to files where writing 1 / 0 turns backlight on / off.
 */
#define MCE_CONF_KEYPAD_BACKLIGHT_BRIGHTNESS_FILE       "BinaryBrightnessFile"

/** Name of configuration key for enable backlight when charging */
#define MCE_CONF_KEYPAD_ENABLE_BACKLIGHT_WHEN_CHARGING  "EnableBacklightWhenCharging"

#define DEFAULT_KEYPAD_ENABLE_BACKLIGHT_WHEN_CHARGING   false

#endif /* _KEYPAD_H_ */
