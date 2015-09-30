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

#include "mce.h"

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

/** Whether to use ALS data for LID sensor filtering */
#define MCE_GCONF_FILTER_LID_WITH_ALS           MCE_GCONF_LOCK_PATH"/filter_lid_with_als"
#define DEFAULT_FILTER_LID_WITH_ALS             true

/** Lid sensor open actions */
typedef enum
{
    /** Actions disabled */
    LID_OPEN_ACTION_DISABLED = 0,

    /** Just show lockscreen */
    LID_OPEN_ACTION_UNBLANK  = 1,

    /* Deactivate lockscreen */
    LID_OPEN_ACTION_TKUNLOCK = 2,

} lid_open_action_t;

/** Lid sensor open action GConf setting */
#define MCE_GCONF_TK_LID_OPEN_ACTIONS	MCE_GCONF_LOCK_PATH "/lid_open_actions"
#define DEFAULT_LID_OPEN_ACTION		1 // = LID_OPEN_ACTION_UNBLANK

/** Lid sensor close actions */
typedef enum
{
    /** Actions disabled */
    LID_CLOSE_ACTION_DISABLED = 0,

    /** Just blank screen */
    LID_CLOSE_ACTION_BLANK    = 1,

    /* Activate lockscreen */
    LID_CLOSE_ACTION_TKLOCK   = 2,

} lid_close_action_t;

/** Lid sensor close action GConf setting */
#define MCE_GCONF_TK_LID_CLOSE_ACTIONS	MCE_GCONF_LOCK_PATH "/lid_close_actions"
#define DEFAULT_LID_CLOSE_ACTION	2 // = LID_CLOSE_ACTION_TKLOCK

/** When to react to kbd slide closed events */
typedef enum {
    KBD_OPEN_TRIGGER_NEVER         = 0,
    KBD_OPEN_TRIGGER_ALWAYS        = 1,
    KBD_OPEN_TRIGGER_NO_PROXIMITY  = 2,
} kbd_open_trigger_t;

/* When to react to kbd slide opened events */
typedef enum {
    KBD_CLOSE_TRIGGER_NEVER        = 0,
    KBD_CLOSE_TRIGGER_ALWAYS       = 1,
    KBD_CLOSE_TRIGGER_AFTER_OPEN   = 2,
} kbd_close_trigger_t;

/** Keypad slide open reaction condition GConf setting */
#define MCE_GCONF_TK_KBD_OPEN_TRIGGER   MCE_GCONF_LOCK_PATH "/keyboard_open_trigger"
#define DEFAULT_KBD_OPEN_TRIGGER        2 // = KBD_OPEN_TRIGGER_NO_PROXIMITY

/** Keypad slide open action GConf setting */
#define MCE_GCONF_TK_KBD_OPEN_ACTIONS   MCE_GCONF_LOCK_PATH "/keyboard_open_actions"
#define DEFAULT_KBD_OPEN_ACTION         1 // = LID_OPEN_ACTION_UNBLANK

/** Keypad slide close reaction condition GConf setting */
#define MCE_GCONF_TK_KBD_CLOSE_TRIGGER  MCE_GCONF_LOCK_PATH "/keyboard_close_trigger"
#define DEFAULT_KBD_CLOSE_TRIGGER       2 // = KBD_CLOSE_TRIGGER_AFTER_OPEN

/** Keypad slide close action GConf setting */
#define MCE_GCONF_TK_KBD_CLOSE_ACTIONS  MCE_GCONF_LOCK_PATH "/keyboard_close_actions"
#define DEFAULT_KBD_CLOSE_ACTION        2 // = LID_CLOSE_ACTION_TKLOCK

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

/** How long to keep display on after incoming call ends [ms] */
#define MCE_GCONF_EXCEPTION_LENGTH_CALL_IN      MCE_GCONF_LOCK_PATH"/exception_length_call_in"
#define DEFAULT_EXCEPTION_LENGTH_CALL_IN        5000

/** How long to keep display on after outgoing call ends [ms] */
#define MCE_GCONF_EXCEPTION_LENGTH_CALL_OUT     MCE_GCONF_LOCK_PATH"/exception_length_call_out"
#define DEFAULT_EXCEPTION_LENGTH_CALL_OUT       2500

/** How long to keep display on after alarm is handled [ms] */
#define MCE_GCONF_EXCEPTION_LENGTH_ALARM        MCE_GCONF_LOCK_PATH"/exception_length_alarm"
#define DEFAULT_EXCEPTION_LENGTH_ALARM          1250

/** How long to keep display on when usb cable is connected [ms] */
#define MCE_GCONF_EXCEPTION_LENGTH_USB_CONNECT  MCE_GCONF_LOCK_PATH"/exception_length_usb_connect"
#define DEFAULT_EXCEPTION_LENGTH_USB_CONNECT    5000

/** How long to keep display on when usb mode dialog is shown [ms] */
#define MCE_GCONF_EXCEPTION_LENGTH_USB_DIALOG   MCE_GCONF_LOCK_PATH"/exception_length_usb_dialog"
#define DEFAULT_EXCEPTION_LENGTH_USB_DIALOG     10000

/** How long to keep display on when charging starts [ms] */
#define MCE_GCONF_EXCEPTION_LENGTH_CHARGER      MCE_GCONF_LOCK_PATH"/exception_length_charger"
#define DEFAULT_EXCEPTION_LENGTH_CHARGER        3000

/** How long to keep display on after battery full [ms] */
#define MCE_GCONF_EXCEPTION_LENGTH_BATTERY      MCE_GCONF_LOCK_PATH"/exception_length_battery"
#define DEFAULT_EXCEPTION_LENGTH_BATTERY        0

/** How long to keep display on when audio jack is inserted [ms] */
#define MCE_GCONF_EXCEPTION_LENGTH_JACK_IN      MCE_GCONF_LOCK_PATH"/exception_length_jack_in"
#define DEFAULT_EXCEPTION_LENGTH_JACK_IN        3000

/** How long to keep display on when audio jack is removed [ms] */
#define MCE_GCONF_EXCEPTION_LENGTH_JACK_OUT     MCE_GCONF_LOCK_PATH"/exception_length_jack_out"
#define DEFAULT_EXCEPTION_LENGTH_JACK_OUT       3000

/** How long to keep display on when camera button is pressed [ms] */
#define MCE_GCONF_EXCEPTION_LENGTH_CAMERA       MCE_GCONF_LOCK_PATH"/exception_length_camera"
#define DEFAULT_EXCEPTION_LENGTH_CAMERA         3000

/** How long to keep display on when volume button is pressed [ms] */
#define MCE_GCONF_EXCEPTION_LENGTH_VOLUME       MCE_GCONF_LOCK_PATH"/exception_length_volume"
#define DEFAULT_EXCEPTION_LENGTH_VOLUME         2000

/** How long to extend display on when there is user activity [ms] */
#define MCE_GCONF_EXCEPTION_LENGTH_ACTIVITY     MCE_GCONF_LOCK_PATH"/exception_length_activity"
#define DEFAULT_EXCEPTION_LENGTH_ACTIVITY       2000

/** Is mce allowed to grab input devices */
#define MCE_GCONF_TK_INPUT_POLICY_ENABLED       MCE_GCONF_LOCK_PATH "/touchscreen_policy_enabled"
#define DEFAULT_TK_INPUT_POLICY_ENABLED         true

/** Allow / try to deny lockscreen animations */
#define MCE_GCONF_TK_LOCKSCREEN_ANIM_ENABLED    MCE_GCONF_LOCK_PATH "/lockscreen_animation_enabled"
#define DEFAULT_TK_LOCKSCREEN_ANIM_ENABLED      true

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

void mce_tklock_unblank(display_state_t to_state);
#endif /* _TKLOCK_H_ */
