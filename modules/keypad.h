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
#define DEFAULT_KEY_BACKLIGHT_LEVEL                     255

/** Default key backlight timeout in seconds */
#define DEFAULT_KEY_BACKLIGHT_TIMEOUT                   30      /* 30 s */

#ifndef MCE_CONF_KEYPAD_GROUP
/** Name of Keypad configuration group */
# define MCE_CONF_KEYPAD_GROUP                          "KeyPad"
#endif

/** Name of configuration key for keyboard backlight timeout */
#define MCE_CONF_KEY_BACKLIGHT_TIMEOUT                  "BacklightTimeout"

/** Name of configuration key for keyboard backlight path */
#define MCE_CONF_KEY_BACKLIGHT_SYS_PATH                 "BrightnessDirectory"

#endif /* _KEYPAD_H_ */
