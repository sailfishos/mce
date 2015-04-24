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
# define MCE_GCONF_LOCK_PATH		"/system/osso/dsm/locks"
#endif

/** SysFS interface to enable/disable RX-51 keyboard IRQs */
#define MCE_RX51_KEYBOARD_SYSFS_DISABLE_PATH		"/sys/class/i2c-adapter/i2c-1/1-004a/twl4030_keypad/disable_kp"

/** SysFS interface to enable/disable keypad IRQs */
#define MCE_KEYPAD_SYSFS_DISABLE_PATH			"/sys/devices/platform/omap2_mcspi.1/spi1.0/disable_kp"

/** SysFS interface to enable/disable the RX-44/RX-48 keyboard IRQs */
#define MCE_RX44_KEYBOARD_SYSFS_DISABLE_PATH		"/sys/devices/platform/i2c_omap.2/i2c-0/0-0045/disable_kp"

/** Double tap wakeup action modes */
typedef enum
{
    /** Gesture disabled */
    DBLTAP_ACTION_DISABLED = 0,

    /** Show unlock screen */
    DBLTAP_ACTION_UNBLANK  = 1,

    /* Unlock tklock */
    DBLTAP_ACTION_TKUNLOCK = 2,

    DBLTAP_ACTION_DEFAULT  = DBLTAP_ACTION_UNBLANK
} dbltap_action_t;

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

/** Path to the automatic tklock dim/blank disable GConf setting */
#define MCE_GCONF_TK_AUTO_BLANK_DISABLE_PATH	MCE_GCONF_LOCK_PATH "/tklock_blank_disable"

/** Default value for MCE_GCONF_TK_AUTO_BLANK_DISABLE_PATH */
#define DEFAULT_TK_AUTO_BLANK_DISABLE		0

/** Automatic lpm triggering modes GConf setting */
# define MCE_GCONF_LPMUI_TRIGGERING             MCE_GCONF_LOCK_PATH "/lpm_triggering"

/** Default value for MCE_GCONF_LPMUI_TRIGGERING
 *
 * Note: Keep this in sync with the entry in builtin-gconf.c
 */
#define DEFAULT_LPMUI_TRIGGERING		LPMUI_TRIGGERING_FROM_POCKET

/** Proximity can block touch input GConf setting */
# define MCE_GCONF_PROXIMITY_BLOCKS_TOUCH       MCE_GCONF_LOCK_PATH "/proximity_blocks_touch"

/** Default value for can block touch input GConf setting */
# define PROXIMITY_BLOCKS_TOUCH_DEFAULT         false

/** Devicelock is in lockscreen GConf setting */
# define MCE_GCONF_DEVICELOCK_IN_LOCKSCREEN     MCE_GCONF_LOCK_PATH "/devicelock_in_lockscreen"

/** Default value for MCE_GCONF_DEVICELOCK_IN_LOCKSCREEN */
# define DEFAULT_DEVICELOCK_IN_LOCKSCREEN       false

/** Lid sensor enabled GConf setting */
# define MCE_GCONF_LID_SENSOR_ENABLED           MCE_GCONF_LOCK_PATH "/lid_sensor_enabled"

/** Default value for MCE_GCONF_PROXIMITY_PS_ENABLED_PATH */
# define DEFAULT_LID_SENSOR_ENABLED             true

/** Autolock delay GConf setting [ms]*/
# define MCE_GCONF_AUTOLOCK_DELAY		MCE_GCONF_LOCK_PATH "/autolock_delay"
# define DEFAULT_AUTOLOCK_DELAY                 30000
# define MINIMUM_AUTOLOCK_DELAY                 0
# define MAXIMUM_AUTOLOCK_DELAY                 600000

/** Automatic lpm triggering modes */
enum
{
    /** Automatic triggering disabled */
    LPMUI_TRIGGERING_NONE        = 0,

    /** Proximity sensor based out-of-pocket triggering */
    LPMUI_TRIGGERING_FROM_POCKET = 1<<0,

    /** Proximity sensor based hover-over triggering */
    LPMUI_TRIGGERING_HOVER_OVER  = 1<<1,
};

/** Name of D-Bus callback to provide to Touchscreen/Keypad Lock SystemUI */
#define MCE_TKLOCK_CB_REQ		"tklock_callback"

/** Name of Touchscreen/Keypad lock configuration group */
#define MCE_CONF_TKLOCK_GROUP		"TKLock"

/** Name of configuration key for camera popout unlock */
#define MCE_CONF_CAMERA_POPOUT_UNLOCK			"CameraPopoutUnlock"

/* when MCE is made modular, this will be handled differently
 */
gboolean mce_tklock_init(void);
void mce_tklock_exit(void);

#endif /* _TKLOCK_H_ */
