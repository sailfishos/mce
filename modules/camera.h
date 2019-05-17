/**
 * @file camera.h
 * Headers for the camera LED-indicator module
 * <p>
 * Copyright © 2007 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2014-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
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
#ifndef _CAMERA_H_
#define _CAMERA_H_

/** Path to the SysFS interface for the camera active state */
#define CAMERA_ACTIVE_STATE_PATH                        "/sys/devices/platform/omap24xxcam/streaming"

/** Value for the camera active state */
#define MCE_CAMERA_ACTIVE                               "active"

/** Value for the camera inactive state */
#define MCE_CAMERA_INACTIVE                             "inactive"

/** Path to the SysFS interface for the camera pop-out state */
#define CAMERA_POPOUT_STATE_PATH                        "/sys/devices/platform/gpio-switch/cam_act/state"

/** Value for the camera in popped out state */
#define MCE_CAMERA_POPPED_OUT                           "active"

/** Value for the camera in popped in state */
#define MCE_CAMERA_POPPED_IN                            "inactive"

/** Default fallback setting for the touchscreen/keypad autolock */
#define DEFAULT_CAMERA_POPOUT_UNLOCK                    TRUE            /* FALSE / TRUE */

#endif /* _CAMERA_H_ */
