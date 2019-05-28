/**
 * @file event-switches.h
 * Headers for the switch event provider for the Mode Control Entity
 * <p>
 * Copyright © 2007-2010 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _EVENT_SWITCHES_H_
#define _EVENT_SWITCHES_H_

#include <glib.h>

/** Path to the SysFS interface for the lock flicker-key status */
#define MCE_FLICKER_KEY_STATE_PATH                      "/sys/devices/platform/gpio-switch/kb_lock/state"

/** Value for the lock flicker-key active state */
#define MCE_FLICKER_KEY_ACTIVE                          "closed"

/** Value for the lock flicker-key inactive state */
#define MCE_FLICKER_KEY_INACTIVE                        "open"

/** Path to the SysFS interface for the keyboard slide status */
#define MCE_KBD_SLIDE_STATE_PATH                        "/sys/devices/platform/gpio-switch/slide/state"

/** Value for the keyboard slide open state */
#define MCE_KBD_SLIDE_OPEN                              "open"

/** Value for the keyboard slide closed state */
#define MCE_KBD_SLIDE_CLOSED                            "closed"

/** Path to the SysFS interface for the camera focus state */
#define MCE_CAM_FOCUS_STATE_PATH                        "/sys/devices/platform/gpio-switch/cam_focus/state"

/** Value for the camera focus active state */
#define MCE_CAM_FOCUS_ACTIVE                            "active"

/** Value for the camera focus inactive state */
#define MCE_CAM_FOCUS_INACTIVE                          "inactive"

/** SysFS interface to enable/disable camera focus IRQs */
#define MCE_CAM_FOCUS_DISABLE_PATH                      "/sys/devices/platform/gpio-switch/cam_focus/disable"

/** Path to the SysFS interface for the camera launch state */
#define MCE_CAM_LAUNCH_STATE_PATH                       "/sys/devices/platform/gpio-switch/cam_launch/state"

/** Value for the camera launch active state */
#define MCE_CAM_LAUNCH_ACTIVE                           "active"

/** Value for the camera launch inactive state */
#define MCE_CAM_LAUNCH_INACTIVE                         "inactive"

/** Path to the SysFS interface for the lid cover status */
#define MCE_LID_COVER_STATE_PATH                        "/sys/devices/platform/gpio-switch/prot_shell/cover_switch"

/** Value for the lid cover open state */
#define MCE_LID_COVER_OPEN                              "open"

/** Value for the lid cover closed state */
#define MCE_LID_COVER_CLOSED                            "closed"

/** Path to the SysFS interface for the proximity sensor status */
#define MCE_PROXIMITY_SENSOR_STATE_PATH                 "/sys/devices/platform/gpio-switch/proximity/state"

/** Value for the proximity sensor open state */
#define MCE_PROXIMITY_SENSOR_OPEN                       "open"

/** Value for the proximity sensor closed state */
#define MCE_PROXIMITY_SENSOR_CLOSED                     "closed"

/** SysFS interface to enable/disable proximity sensor IRQs */
#define MCE_PROXIMITY_SENSOR_DISABLE_PATH               "/sys/devices/platform/gpio-switch/proximity/disable"

/**
 * Path to the SysFS interface for the MUSB HDRC USB cable status;
 * RX-51
 */
#define MCE_MUSB_OMAP3_USB_CABLE_STATE_PATH             "/sys/class/i2c-adapter/i2c-1/1-0048/twl4030_usb/vbus"

/** Value for the MUSB HDRC USB cable connected state */
#define MCE_MUSB_OMAP3_USB_CABLE_CONNECTED              "1"

/** Value for the MUSB HDRC USB cable disconnected state */
#define MCE_MUSB_OMAP3_USB_CABLE_DISCONNECTED           "0"

/** Path to the SysFS interface for the RX-51 MMC0 cover status */
#define MCE_MMC0_COVER_STATE_PATH                       "/sys/class/mmc_host/mmc0/cover_switch"

/** Value for the RX-51 MMC0 cover open state */
#define MCE_MMC_COVER_OPEN                              "open"

/** Value for the RX-51 MMC0 cover closed state */
#define MCE_MMC_COVER_CLOSED                            "closed"

/** Path to the SysFS interface for the MMC cover status */
#define MCE_MMC_COVER_STATE_PATH                        "/sys/devices/platform/gpio-switch/mmci-omap.2/cover_switch"

/** Value for the MMC cover open state */
#define MCE_MMC_COVER_OPEN                              "open"

/** Value for the MMC cover closed state */
#define MCE_MMC_COVER_CLOSED                            "closed"

/** Path to the SysFS interface for the lens cover status */
#define MCE_LENS_COVER_STATE_PATH                       "/sys/devices/platform/gpio-switch/cam_shutter/state"

/** Value for the lens cover open state */
#define MCE_LENS_COVER_OPEN                             "open"

/** Value for the lens cover closed state */
#define MCE_LENS_COVER_CLOSED                           "closed"

/** Path to the SysFS interface for the battery cover status */
#define MCE_BATTERY_COVER_STATE_PATH                    "/sys/devices/platform/gpio-switch/bat_cover/cover_switch"

/** Value for the battery cover open state */
#define MCE_BATTERY_COVER_OPEN                          "open"

/** Value for the battery cover closed state */
#define MCE_BATTERY_COVER_CLOSED                        "closed"

/* When MCE is made modular, this will be handled differently */
gboolean mce_switches_init(void);
void mce_switches_exit(void);

#endif /* _EVENT_SWITCHES_H_ */
