/**
 * @file powerkey.h
 * Headers for the power key logic for the Mode Control Entity
 * <p>
 * Copyright © 2004-2010 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef POWERKEY_H_
# define POWERKEY_H_

# include <glib.h>

/* ========================================================================= *
 * Settings
 * ========================================================================= */

/** Path to the GConf settings for the powerkey module */
# define MCE_SETTING_POWERKEY_PATH                 "/system/osso/dsm/powerkey"

/** Power key action enable modes */
typedef enum
{
    /** Power key actions disabled */
    PWRKEY_ENABLE_NEVER         = 0,

    /** Power key actions always enabled */
    PWRKEY_ENABLE_ALWAYS        = 1,

    /** Power key actions enabled if PS is not covered */
    PWRKEY_ENABLE_NO_PROXIMITY  = 2,

    /** Power key actions enabled if PS is not covered or display is on */
    PWRKEY_ENABLE_NO_PROXIMITY2 = 3,
} pwrkey_enable_mode_t;

/** Powerkey mode setting */
# define MCE_SETTING_POWERKEY_MODE                 MCE_SETTING_POWERKEY_PATH "/mode"
# define MCE_DEFAULT_POWERKEY_MODE                 1 // = PWRKEY_ENABLE_ALWAYS

typedef enum
{
    /** Pressing power key turns display off */
    PWRKEY_BLANK_TO_OFF         = 0,

    /** Pressing power key puts display to lpm state */
    PWRKEY_BLANK_TO_LPM         = 1,
} pwrkey_blanking_mode_t;

/** Powerkey blanking mode setting */
# define MCE_SETTING_POWERKEY_BLANKING_MODE        MCE_SETTING_POWERKEY_PATH "/blanking_mode"
# define MCE_DEFAULT_POWERKEY_BLANKING_MODE        0 // = PWRKEY_BLANK_TO_OFF

/** Powerkey press count for proximity sensor override */
# define MCE_SETTING_POWERKEY_PS_OVERRIDE_COUNT    MCE_SETTING_POWERKEY_PATH "/ps_override_count"
# define MCE_DEFAULT_POWERKEY_PS_OVERRIDE_COUNT    3

/** Maximum delay between powerkey presses for ps override */
# define MCE_SETTING_POWERKEY_PS_OVERRIDE_TIMEOUT  MCE_SETTING_POWERKEY_PATH "/ps_override_timeout"
# define MCE_DEFAULT_POWERKEY_PS_OVERRIDE_TIMEOUT  333

/** Long press timeout setting */
# define MCE_SETTING_POWERKEY_LONG_PRESS_DELAY     MCE_SETTING_POWERKEY_PATH "/long_press_delay"
# define MCE_DEFAULT_POWERKEY_LONG_PRESS_DELAY     1500

/** Double press timeout setting */
# define MCE_SETTING_POWERKEY_DOUBLE_PRESS_DELAY   MCE_SETTING_POWERKEY_PATH "/double_press_delay"
# define MCE_DEFAULT_POWERKEY_DOUBLE_PRESS_DELAY   400

/** Setting for single press actions from display on */
# define MCE_SETTING_POWERKEY_ACTIONS_SINGLE_ON    MCE_SETTING_POWERKEY_PATH "/actions_single_on"
# define MCE_DEFAULT_POWERKEY_ACTIONS_SINGLE_ON    "blank,tklock"

/** Setting for double press actions from display on */
# define MCE_SETTING_POWERKEY_ACTIONS_DOUBLE_ON    MCE_SETTING_POWERKEY_PATH "/actions_double_on"
# define MCE_DEFAULT_POWERKEY_ACTIONS_DOUBLE_ON    "blank,tklock,devlock,vibrate"

/** Setting for long press actions from display on */
# define MCE_SETTING_POWERKEY_ACTIONS_LONG_ON      MCE_SETTING_POWERKEY_PATH "/actions_long_on"
# define MCE_DEFAULT_POWERKEY_ACTIONS_LONG_ON      "shutdown"

/** Setting for single press actions from display off */
# define MCE_SETTING_POWERKEY_ACTIONS_SINGLE_OFF   MCE_SETTING_POWERKEY_PATH "/actions_single_off"
# define MCE_DEFAULT_POWERKEY_ACTIONS_SINGLE_OFF   "unblank"

/** Setting for double press actions from display off */
# define MCE_SETTING_POWERKEY_ACTIONS_DOUBLE_OFF   MCE_SETTING_POWERKEY_PATH "/actions_double_off"
/** Default actions for double press while display is off */
# define MCE_DEFAULT_POWERKEY_ACTIONS_DOUBLE_OFF   "unblank,tkunlock"

/** Setting for long press actions from display off
 *
 * Note: If kernel side reports immediately power key press + release
 *       when device is suspended, detecting long presses might not
 *       work when display is off -> leave unset by default. */
# define MCE_SETTING_POWERKEY_ACTIONS_LONG_OFF     MCE_SETTING_POWERKEY_PATH "/actions_long_off"
# define MCE_DEFAULT_POWERKEY_ACTIONS_LONG_OFF     ""

/** Setting keys for touch screen gesture actions */
# define MCE_SETTING_POWERKEY_ACTIONS_GESTURE0     MCE_SETTING_POWERKEY_PATH "/actions_gesture0"
# define MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE0     "unblank"

# define MCE_SETTING_POWERKEY_ACTIONS_GESTURE1     MCE_SETTING_POWERKEY_PATH "/actions_gesture1"
# define MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE1     "unblank"

# define MCE_SETTING_POWERKEY_ACTIONS_GESTURE2     MCE_SETTING_POWERKEY_PATH "/actions_gesture2"
# define MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE2     "unblank"

# define MCE_SETTING_POWERKEY_ACTIONS_GESTURE3     MCE_SETTING_POWERKEY_PATH "/actions_gesture3"
# define MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE3     "unblank"

# define MCE_SETTING_POWERKEY_ACTIONS_GESTURE4     MCE_SETTING_POWERKEY_PATH "/actions_gesture4"
# define MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE4     "unblank"

# define MCE_SETTING_POWERKEY_ACTIONS_GESTURE5     MCE_SETTING_POWERKEY_PATH "/actions_gesture5"
# define MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE5     "unblank"

# define MCE_SETTING_POWERKEY_ACTIONS_GESTURE6     MCE_SETTING_POWERKEY_PATH "/actions_gesture6"
# define MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE6     "unblank"

# define MCE_SETTING_POWERKEY_ACTIONS_GESTURE7     MCE_SETTING_POWERKEY_PATH "/actions_gesture7"
# define MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE7     "unblank"

# define MCE_SETTING_POWERKEY_ACTIONS_GESTURE8     MCE_SETTING_POWERKEY_PATH "/actions_gesture8"
# define MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE8     "unblank"

# define MCE_SETTING_POWERKEY_ACTIONS_GESTURE9     MCE_SETTING_POWERKEY_PATH "/actions_gesture9"
# define MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE9     "unblank"

# define MCE_SETTING_POWERKEY_ACTIONS_GESTURE10    MCE_SETTING_POWERKEY_PATH "/actions_gesture10"
# define MCE_DEFAULT_POWERKEY_ACTIONS_GESTURE10    "unblank"

/** Number of configurable touchscreen gestures */
# define POWERKEY_ACTIONS_GESTURE_COUNT            11

/** Setting keys for configurable dbus actions */
# define MCE_SETTING_POWERKEY_DBUS_ACTION1         MCE_SETTING_POWERKEY_PATH "/dbus_action1"
# define MCE_DEFAULT_POWERKEY_DBUS_ACTION1         "event1"

# define MCE_SETTING_POWERKEY_DBUS_ACTION2         MCE_SETTING_POWERKEY_PATH "/dbus_action2"
# define MCE_DEFAULT_POWERKEY_DBUS_ACTION2         "event2"

# define MCE_SETTING_POWERKEY_DBUS_ACTION3         MCE_SETTING_POWERKEY_PATH "/dbus_action3"
# define MCE_DEFAULT_POWERKEY_DBUS_ACTION3         "event3"

# define MCE_SETTING_POWERKEY_DBUS_ACTION4         MCE_SETTING_POWERKEY_PATH "/dbus_action4"
# define MCE_DEFAULT_POWERKEY_DBUS_ACTION4         "event4"

# define MCE_SETTING_POWERKEY_DBUS_ACTION5         MCE_SETTING_POWERKEY_PATH "/dbus_action5"
# define MCE_DEFAULT_POWERKEY_DBUS_ACTION5         "event5"

# define MCE_SETTING_POWERKEY_DBUS_ACTION6         MCE_SETTING_POWERKEY_PATH "/dbus_action6"
# define MCE_DEFAULT_POWERKEY_DBUS_ACTION6         "event6"

# define MCE_SETTING_POWERKEY_DBUS_ACTION7         MCE_SETTING_POWERKEY_PATH "/dbus_action7"
# define MCE_DEFAULT_POWERKEY_DBUS_ACTION7         "event7"

# define MCE_SETTING_POWERKEY_DBUS_ACTION8         MCE_SETTING_POWERKEY_PATH "/dbus_action8"
# define MCE_DEFAULT_POWERKEY_DBUS_ACTION8         "event8"

# define MCE_SETTING_POWERKEY_DBUS_ACTION9         MCE_SETTING_POWERKEY_PATH "/dbus_action9"
# define MCE_DEFAULT_POWERKEY_DBUS_ACTION9         "event9"

# define MCE_SETTING_POWERKEY_DBUS_ACTION10        MCE_SETTING_POWERKEY_PATH "/dbus_action10"
# define MCE_DEFAULT_POWERKEY_DBUS_ACTION10        "event10"

/** Number of configurable dbus actions */
# define POWEKEY_DBUS_ACTION_COUNT                 10

/* ========================================================================= *
 * FUNCTIONS
 * ========================================================================= */

gboolean mce_powerkey_init(void);
void     mce_powerkey_exit(void);

#endif /* POWERKEY_H_ */
