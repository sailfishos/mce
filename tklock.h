/**
 * @file tklock.h
 * Headers for the touchscreen/keypad lock component
 * of the Mode Control Entity
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2013-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Tapio Rantala <ext-tapio.rantala@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Jukka Turunen <ext-jukka.t.turunen@nokia.com>
 * @author Mika Laitio <lamikr@pilppa.org>
 * @author Markus Lehtonen <markus.lehtonen@iki.fi>
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
#ifndef TKLOCK_H_
# define TKLOCK_H_

# include "mce.h"

/* ========================================================================= *
 * Settings
 * ========================================================================= */

/** Prefix for tklock setting keys */
# define MCE_SETTING_TK_PATH                     "/system/osso/dsm/locks"

/** Whether lockscreen should be activated when display turns off */
# define MCE_SETTING_TK_AUTOLOCK_ENABLED         MCE_SETTING_TK_PATH "/touchscreen_keypad_autolock_enabled"
# define MCE_DEFAULT_TK_AUTOLOCK_ENABLED         true

/** Whether automatic blanking from lockscreen is disabled */
# define MCE_SETTING_TK_AUTO_BLANK_DISABLE       MCE_SETTING_TK_PATH "/tklock_blank_disable"
# define MCE_DEFAULT_TK_AUTO_BLANK_DISABLE       0

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

/** What conditions can trigger lpm mode display */
# define MCE_SETTING_TK_LPMUI_TRIGGERING         MCE_SETTING_TK_PATH "/lpm_triggering"
# define MCE_DEFAULT_TK_LPMUI_TRIGGERING         1 // = LPMUI_TRIGGERING_FROM_POCKET

/** Whether touch input should remain blocked until ps is not covered
 *
 * To avoid accidental touch interaction related to lipstick
 * crashes/restarts occurring while the device is in a pocket,
 * it would be highly desirable to block touch input while
 * proximity sensor is covered. However, this causes massive
 * problems during bootup for devices with unreliably working
 * proximity sensor, so this must remain disabled by default.
 */
# define MCE_SETTING_TK_PROXIMITY_BLOCKS_TOUCH   MCE_SETTING_TK_PATH "/proximity_blocks_touch"
# define MCE_DEFAULT_TK_PROXIMITY_BLOCKS_TOUCH   false

/** Whether device unlocking happens in lockscreen
 *
 * Deactivating lockscreen must not be allowed if device unlock code
 * is entered in lockscreen context.
 */
# define MCE_SETTING_TK_DEVICELOCK_IN_LOCKSCREEN MCE_SETTING_TK_PATH "/devicelock_in_lockscreen"
# define MCE_DEFAULT_TK_DEVICELOCK_IN_LOCKSCREEN false

/** Whether MCE is allowed to use lid sensor
 *
 * Note: Unlike other sensors that are powered on/off via sensorfw,
 *       it is assumed that lid sensor is powered on if present and
 *       this settings just controls whether mce uses or ignores the
 *       lid events that it receives.
 */
# define MCE_SETTING_TK_LID_SENSOR_ENABLED       MCE_SETTING_TK_PATH "/lid_sensor_enabled"
# define MCE_DEFAULT_TK_LID_SENSOR_ENABLED       true

/** Whether lid sensor state should be verified with light sensor
 *
 * If enabled, lid sensor state changes are ignored unless they
 * occur in close proximity to ambient light level changes. This
 * is mainly useful for filtering out false state changes reported
 * by hall effect sensors confused by unexpected magnetic fields.
 */
# define MCE_SETTING_TK_FILTER_LID_WITH_ALS      MCE_SETTING_TK_PATH"/filter_lid_with_als"
# define MCE_DEFAULT_TK_FILTER_LID_WITH_ALS      false

/** Maximum amount of light ALS is expected to report when LID is closed */
# define MCE_SETTING_TK_FILTER_LID_ALS_LIMIT     MCE_SETTING_TK_PATH"/filter_lid_als_limit"
# define MCE_DEFAULT_TK_FILTER_LID_ALS_LIMIT     0

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

/** Actions to be taken when lid is opened */
# define MCE_SETTING_TK_LID_OPEN_ACTIONS         MCE_SETTING_TK_PATH "/lid_open_actions"
# define MCE_DEFAULT_TK_LID_OPEN_ACTIONS         1 // = LID_OPEN_ACTION_UNBLANK

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

/** Actions to be taken when lid is closed */
# define MCE_SETTING_TK_LID_CLOSE_ACTIONS        MCE_SETTING_TK_PATH "/lid_close_actions"
# define MCE_DEFAULT_TK_LID_CLOSE_ACTIONS        2 // = LID_CLOSE_ACTION_TKLOCK

/** When to react to kbd slide closed events */
typedef enum {
    KBD_OPEN_TRIGGER_NEVER        = 0,
    KBD_OPEN_TRIGGER_ALWAYS       = 1,
    KBD_OPEN_TRIGGER_NO_PROXIMITY = 2,
} kbd_open_trigger_t;

/** When MCE should react to kbd slide open */
# define MCE_SETTING_TK_KBD_OPEN_TRIGGER         MCE_SETTING_TK_PATH "/keyboard_open_trigger"
# define MCE_DEFAULT_TK_KBD_OPEN_TRIGGER         2 // = KBD_OPEN_TRIGGER_NO_PROXIMITY

/** Actions to be taken when kbd slide is opened */
# define MCE_SETTING_TK_KBD_OPEN_ACTIONS         MCE_SETTING_TK_PATH "/keyboard_open_actions"
# define MCE_DEFAULT_TK_KBD_OPEN_ACTIONS         1 // = LID_OPEN_ACTION_UNBLANK

/* When to react to kbd slide opened events */
typedef enum {
    KBD_CLOSE_TRIGGER_NEVER      = 0,
    KBD_CLOSE_TRIGGER_ALWAYS     = 1,
    KBD_CLOSE_TRIGGER_AFTER_OPEN = 2,
} kbd_close_trigger_t;

/** When MCE should react to kbd slide close */
# define MCE_SETTING_TK_KBD_CLOSE_TRIGGER        MCE_SETTING_TK_PATH "/keyboard_close_trigger"
# define MCE_DEFAULT_TK_KBD_CLOSE_TRIGGER        2 // = KBD_CLOSE_TRIGGER_AFTER_OPEN

/** Actions to be taken when kbd slide is closed */
# define MCE_SETTING_TK_KBD_CLOSE_ACTIONS        MCE_SETTING_TK_PATH "/keyboard_close_actions"
# define MCE_DEFAULT_TK_KBD_CLOSE_ACTIONS        2 // = LID_CLOSE_ACTION_TKLOCK

/** Autolock delay after display off [ms]
 *
 * When display gets dimmed and then blanked, there is
 * a "grace period" before the application that was
 * active gets backgrounded and lockscreen activated.
 */
# define MCE_SETTING_TK_AUTOLOCK_DELAY           MCE_SETTING_TK_PATH "/autolock_delay"
# define MCE_DEFAULT_TK_AUTOLOCK_DELAY           30000
# define MINIMUM_AUTOLOCK_DELAY                  0
# define MAXIMUM_AUTOLOCK_DELAY                  600000

/** Volume key input policy modes */
typedef enum {
    /** Default rules apply */
    VOLKEY_POLICY_DEFAULT    = 0,

    /** Volume keys are enabled only when there is music playback. */
    VOLKEY_POLICY_MEDIA_ONLY = 1,
} volkey_policy_t;

/** When MCE should block/allow volume key events */
# define MCE_SETTING_TK_VOLKEY_POLICY            MCE_SETTING_TK_PATH "/volume_key_input_policy"
# define MCE_DEFAULT_TK_VOLKEY_POLICY            0 // = VOLKEY_POLICY_DEFAULT

/** How long to keep display on after incoming call ends [ms] */
# define MCE_SETTING_TK_EXCEPT_LEN_CALL_IN       MCE_SETTING_TK_PATH"/exception_length_call_in"
# define MCE_DEFAULT_TK_EXCEPT_LEN_CALL_IN       5000

/** How long to keep display on after outgoing call ends [ms] */
# define MCE_SETTING_TK_EXCEPT_LEN_CALL_OUT      MCE_SETTING_TK_PATH"/exception_length_call_out"
# define MCE_DEFAULT_TK_EXCEPT_LEN_CALL_OUT      2500

/** How long to keep display on after alarm is handled [ms] */
# define MCE_SETTING_TK_EXCEPT_LEN_ALARM         MCE_SETTING_TK_PATH"/exception_length_alarm"
# define MCE_DEFAULT_TK_EXCEPT_LEN_ALARM         1250

/** How long to keep display on when usb cable is connected [ms] */
# define MCE_SETTING_TK_EXCEPT_LEN_USB_CONNECT   MCE_SETTING_TK_PATH"/exception_length_usb_connect"
# define MCE_DEFAULT_TK_EXCEPT_LEN_USB_CONNECT   5000

/** How long to keep display on when usb mode dialog is shown [ms] */
# define MCE_SETTING_TK_EXCEPT_LEN_USB_DIALOG    MCE_SETTING_TK_PATH"/exception_length_usb_dialog"
# define MCE_DEFAULT_TK_EXCEPT_LEN_USB_DIALOG    10000

/** How long to keep display on when charging starts [ms] */
# define MCE_SETTING_TK_EXCEPT_LEN_CHARGER       MCE_SETTING_TK_PATH"/exception_length_charger"
# define MCE_DEFAULT_TK_EXCEPT_LEN_CHARGER       3000

/** How long to keep display on after battery full [ms] */
# define MCE_SETTING_TK_EXCEPT_LEN_BATTERY       MCE_SETTING_TK_PATH"/exception_length_battery"
# define MCE_DEFAULT_TK_EXCEPT_LEN_BATTERY       0

/** How long to keep display on when audio jack is inserted [ms] */
# define MCE_SETTING_TK_EXCEPT_LEN_JACK_IN       MCE_SETTING_TK_PATH"/exception_length_jack_in"
# define MCE_DEFAULT_TK_EXCEPT_LEN_JACK_IN       3000

/** How long to keep display on when audio jack is removed [ms] */
# define MCE_SETTING_TK_EXCEPT_LEN_JACK_OUT      MCE_SETTING_TK_PATH"/exception_length_jack_out"
# define MCE_DEFAULT_TK_EXCEPT_LEN_JACK_OUT      3000

/** How long to keep display on when camera button is pressed [ms] */
# define MCE_SETTING_TK_EXCEPT_LEN_CAMERA        MCE_SETTING_TK_PATH"/exception_length_camera"
# define MCE_DEFAULT_TK_EXCEPT_LEN_CAMERA        3000

/** How long to keep display on when volume button is pressed [ms] */
# define MCE_SETTING_TK_EXCEPT_LEN_VOLUME        MCE_SETTING_TK_PATH"/exception_length_volume"
# define MCE_DEFAULT_TK_EXCEPT_LEN_VOLUME        2000

/** How long to extend display on when there is user activity [ms] */
# define MCE_SETTING_TK_EXCEPT_LEN_ACTIVITY      MCE_SETTING_TK_PATH"/exception_length_activity"
# define MCE_DEFAULT_TK_EXCEPT_LEN_ACTIVITY      2000

/** Is mce allowed to grab input devices */
# define MCE_SETTING_TK_INPUT_POLICY_ENABLED     MCE_SETTING_TK_PATH "/touchscreen_policy_enabled"
# define MCE_DEFAULT_TK_INPUT_POLICY_ENABLED     true

/** Whether mce should allow lockscreen animations on unblank */
# define MCE_SETTING_TK_LOCKSCREEN_ANIM_ENABLED  MCE_SETTING_TK_PATH "/lockscreen_animation_enabled"
# define MCE_DEFAULT_TK_LOCKSCREEN_ANIM_ENABLED  true

/** Default proximity sensor uncover handling [ms]*/
# define MCE_SETTING_TK_PROXIMITY_DELAY_DEFAULT  MCE_SETTING_TK_PATH "/proximity_delay_default"
# define MCE_DEFAULT_TK_PROXIMITY_DELAY_DEFAULT  100

/** In-call proximity sensor uncover handling [ms]*/
# define MCE_SETTING_TK_PROXIMITY_DELAY_INCALL   MCE_SETTING_TK_PATH "/proximity_delay_incall"
# define MCE_DEFAULT_TK_PROXIMITY_DELAY_INCALL   500

/** Accpeted range for proximity sensor uncover delay: [0s, 1h] */
# define MCE_MINIMUM_TK_PROXIMITY_DELAY          0
# define MCE_MAXIMUM_TK_PROXIMITY_DELAY          (60 * 60 * 1000)

/* ========================================================================= *
 * Configuration
 * ========================================================================= */

/** Name of Touchscreen/Keypad lock configuration group */
# define MCE_CONF_TKLOCK_GROUP           "TKLock"

/** Name of configuration key for camera popout unlock */
# define MCE_CONF_CAMERA_POPOUT_UNLOCK   "CameraPopoutUnlock"

/* ========================================================================= *
 * HW Constants
 * ========================================================================= */

/** SysFS interface to enable/disable RX-51 keyboard IRQs */
# define MCE_RX51_KEYBOARD_SYSFS_DISABLE_PATH            "/sys/class/i2c-adapter/i2c-1/1-004a/twl4030_keypad/disable_kp"

/** SysFS interface to enable/disable keypad IRQs */
# define MCE_KEYPAD_SYSFS_DISABLE_PATH                   "/sys/devices/platform/omap2_mcspi.1/spi1.0/disable_kp"

/** SysFS interface to enable/disable the RX-44/RX-48 keyboard IRQs */
# define MCE_RX44_KEYBOARD_SYSFS_DISABLE_PATH            "/sys/devices/platform/i2c_omap.2/i2c-0/0-0045/disable_kp"

/**
 * SysFS interface to enable/disable
 * RM-680/RM-690/RM-696/RM-716 double tap gesture recognition
 */
# define MCE_RM680_DOUBLETAP_SYSFS_PATH                  "/sys/class/i2c-adapter/i2c-2/2-004b/wait_for_gesture"

/**
 * SysFS interface to recalibrate
 * RM-680/RM-690/RM-696/RM-716 touchscreen
 */
# define MCE_RM680_TOUCHSCREEN_CALIBRATION_PATH          "/sys/class/i2c-adapter/i2c-2/2-004b/calibrate"

/**
 * SysFS interface to enable/disable
 * RM-680/RM-690/RM-696/RM-716 touchscreen IRQs
 */
# define MCE_RM680_TOUCHSCREEN_SYSFS_DISABLE_PATH        "/sys/class/i2c-adapter/i2c-2/2-004b/disable_ts"

/** SysFS interface to enable/disable RX-44/RX-48/RX-51 touchscreen IRQs */
# define MCE_RX44_TOUCHSCREEN_SYSFS_DISABLE_PATH             "/sys/devices/platform/omap2_mcspi.1/spi1.0/disable_ts"
# define MCE_RX44_TOUCHSCREEN_SYSFS_DISABLE_PATH_KERNEL2637  "/sys/devices/platform/omap2_mcspi.1/spi1.0/disable"

/** Touch screen enable delay for calibration **/
# define MCE_TOUCHSCREEN_CALIBRATION_DELAY               100000 /* 100 milliseconds */

/* ========================================================================= *
 * Functions
 * ========================================================================= */

gboolean mce_tklock_init(void);
void      mce_tklock_exit(void);
void      mce_tklock_unblank(display_state_t to_state);

#endif /* TKLOCK_H_ */
