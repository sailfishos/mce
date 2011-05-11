/**
 * @file tklock.h
 * Headers for the touchscreen/keypad lock component
 * of the Mode Control Entity
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _TKLOCK_H_
#define _TKLOCK_H_

#include <glib.h>

#ifndef MCE_GCONF_LOCK_PATH
/** Path to the GConf settings for the touchscreen/keypad lock */
#define MCE_GCONF_LOCK_PATH		"/system/osso/dsm/locks"
#endif /* MCE_GCONF_LOCK_PATH */

/** SysFS interface to enable/disable RX-51 keyboard IRQs */
#define MCE_RX51_KEYBOARD_SYSFS_DISABLE_PATH		"/sys/class/i2c-adapter/i2c-1/1-004a/twl4030_keypad/disable_kp"
/** SysFS interface to enable/disable keypad IRQs */
#define MCE_KEYPAD_SYSFS_DISABLE_PATH			"/sys/devices/platform/omap2_mcspi.1/spi1.0/disable_kp"
/** SysFS interface to enable/disable the RX-44/RX-48 keyboard IRQs */
#define MCE_RX44_KEYBOARD_SYSFS_DISABLE_PATH		"/sys/devices/platform/i2c_omap.2/i2c-0/0-0045/disable_kp"

/**
 * Default double tap gesture:
 *
 * 0 - Gesture disabled
 * 1 - Show unlock screen
 * 2 - Unlock tklock
 */
#define DEFAULT_DOUBLETAP_GESTURE_POLICY		1

/** Proximity timeout for double tap gesture; in seconds */
#define DEFAULT_POCKET_MODE_PROXIMITY_TIMEOUT		5

/** Proximity timeout for double tap gesture; in seconds */
#define DEFAULT_DOUBLETAP_PROXIMITY_TIMEOUT		0

/**
 * SysFS interface to enable/disable
 * RM-680/RM-690/RM-696/RM-716 double tap gesture recognition
 */
#define MCE_RM680_DOUBLETAP_SYSFS_PATH			"/sys/class/i2c-adapter/i2c-2/2-004b/wait_for_gesture"

/**
 * SysFS interface to recalibrate
 * RM-680/RM-690/RM-696/RM-716 touchscreen
 */
#define MCE_RM680_TOUCHSCREEN_CALIBRATION_PATH		"/sys/class/i2c-adapter/i2c-2/2-004b/calibrate"

/**
 * SysFS interface to enable/disable
 * RM-680/RM-690/RM-696/RM-716 touchscreen IRQs
 */
#define MCE_RM680_TOUCHSCREEN_SYSFS_DISABLE_PATH	"/sys/class/i2c-adapter/i2c-2/2-004b/disable_ts"

/** SysFS interface to enable/disable RX-44/RX-48/RX-51 touchscreen IRQs */
#define MCE_RX44_TOUCHSCREEN_SYSFS_DISABLE_PATH		    "/sys/devices/platform/omap2_mcspi.1/spi1.0/disable_ts"
#define MCE_RX44_TOUCHSCREEN_SYSFS_DISABLE_PATH_KERNEL2637	"/sys/devices/platform/omap2_mcspi.1/spi1.0/disable"

/** Touch screen enable delay for calibration **/
#define MCE_TOUCHSCREEN_CALIBRATION_DELAY		100000 /* 100 milliseconds */

/** Default fallback setting for the touchscreen/keypad autolock */
#define DEFAULT_TK_AUTOLOCK		FALSE		/* FALSE / TRUE */

/** Path to the touchscreen/keypad autolock GConf setting */
#define MCE_GCONF_TK_AUTOLOCK_ENABLED_PATH	MCE_GCONF_LOCK_PATH "/touchscreen_keypad_autolock_enabled"

/** Path to the touchscreen/keypad double tap gesture GConf setting */
#define MCE_GCONF_TK_DOUBLE_TAP_GESTURE_PATH	MCE_GCONF_LOCK_PATH "/tklock_double_tap_gesture"

/** Name of D-Bus callback to provide to Touchscreen/Keypad Lock SystemUI */
#define MCE_TKLOCK_CB_REQ		"tklock_callback"
/** Delay before the touchscreen/keypad is unlocked */
#define MCE_TKLOCK_UNLOCK_DELAY		500		/**< 0.5 seconds */

#ifndef MCE_CONF_TKLOCK_GROUP
/** Name of Touchscreen/Keypad lock configuration group */
#define MCE_CONF_TKLOCK_GROUP		"TKLock"
#endif /* MCE_CONF_TKLOCK_GROUP */

/** Name of configuration key for touchscreen/keypad immediate blanking */
#define MCE_CONF_BLANK_IMMEDIATELY	"BlankImmediately"

/** Name of configuration key for touchscreen/keypad immediate dimming */
#define MCE_CONF_DIM_IMMEDIATELY	"DimImmediately"

/** Name of configuration key for touchscreen/keypad dim timeout */
#define MCE_CONF_DIM_DELAY		"DimDelay"

/** Name of configuration key for touchscreen immediate disabling */
#define MCE_CONF_TS_OFF_IMMEDIATELY	"DisableTSImmediately"

/** Name of configuration key for keypad immediate disabling */
#define MCE_CONF_KP_OFF_IMMEDIATELY	"DisableKPImmediately"

/** Name of configuration key for keyboard slide autolock */
#define MCE_CONF_AUTOLOCK_SLIDE_OPEN	"AutolockWhenSlideOpen"

/** Name of configuration key for keyboard slide proximity lock */
#define MCE_CONF_PROXIMITY_LOCK_SLIDE_OPEN	"ProximityLockWhenSlideOpen"

/** Name of configuration key for keyboard slide unconditional lock on close */
#define MCE_CONF_LOCK_ON_SLIDE_CLOSE	"AlwaysLockOnSlideClose"

/** Name of configuration key for lens cover triggered tklock unlocking */
#define MCE_CONF_LENS_COVER_UNLOCK	"LensCoverUnlock"

/** Name of configuration key for volume key triggered unlock screen */
#define MCE_CONF_VOLKEY_VISUAL_TRIGGER	"TriggerUnlockScreenWithVolumeKeys"

/** Double tap timeout for the touchscreen in milliseconds; 0.5 seconds */
#define DEFAULT_TS_DOUBLE_DELAY		500

/** Default fallback setting for tklock immediate blanking */
#define DEFAULT_BLANK_IMMEDIATELY	FALSE		/* FALSE / TRUE */

/** Default fallback setting for tklock immediate dimming */
#define DEFAULT_DIM_IMMEDIATELY		FALSE		/* FALSE / TRUE */

/** Default visual lock blank timeout */
#define DEFAULT_VISUAL_BLANK_DELAY	5		/* 5 seconds */

/** Default visual lock blank timeout */
#define DEFAULT_VISUAL_FORCED_BLANK_DELAY	30	/* 30 seconds */

/** Default delay before the display dims */
#define DEFAULT_DIM_DELAY		3		/* 3 seconds */

/** Default powerkey repeat emulation delay */
#define DEFAULT_POWERKEY_REPEAT_DELAY	1		/* 1 second */

/** Default powerkey repeat count limit */
#define DEFAULT_POWERKEY_REPEAT_LIMIT	10

/** Default fallback setting for touchscreen immediate disabling */
#define DEFAULT_TS_OFF_IMMEDIATELY	2		/* 0 / 1 / _2_ */

/** Default fallback setting for keypad immediate disabling */
#define DEFAULT_KP_OFF_IMMEDIATELY	2		/* 0 / 1 / _2_ */

/** Default fallback setting for autolock with open keyboard slide */
#define DEFAULT_AUTOLOCK_SLIDE_OPEN	FALSE		/* FALSE */

/** Default fallback setting for proximity lock with open keyboard slide */
#define DEFAULT_PROXIMITY_LOCK_SLIDE_OPEN	FALSE		/* FALSE */

/** Default fallback setting for unconditional lock on close keyboard slide */
#define DEFAULT_LOCK_ON_SLIDE_CLOSE	FALSE		/* FALSE */

/** Default fallback setting for lens cover triggered tklock unlocking */
#define DEFAULT_LENS_COVER_UNLOCK	TRUE		/* TRUE */

/** Default fallback setting for proximity lock when callstate == ringing */
#define DEFAULT_PROXIMITY_LOCK_WHEN_RINGING	FALSE		/* FALSE */

/** Default fallback setting for volume key triggered unlock screen */
#define DEFAULT_VOLKEY_VISUAL_TRIGGER		TRUE		/* TRUE */

/** Default fallback setting for auto lock after call end */
#define DEFAULT_AUTORELOCK_AFTER_CALL_END	FALSE		/* FALSE */

/* when MCE is made modular, this will be handled differently
 */
gboolean mce_tklock_init(void);
void mce_tklock_exit(void);

#endif /* _TKLOCK_H_ */
