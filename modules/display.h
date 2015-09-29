/**
 * @file display.h
 * Headers for the display module
 * <p>
 * Copyright © 2007-2011 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _DISPLAY_H_
#define _DISPLAY_H_

/** Name of the display backlight configuration group */
#define MCE_CONF_DISPLAY_GROUP                  "Display"

/** List of backlight control directories to try */
#define MCE_CONF_BACKLIGHT_DIRECTORY            "BrightnessDirectory"

/** List of backlight control files to try */
#define MCE_CONF_BACKLIGHT_PATH                 "BrightnessPath"

/** List of max backlight control files to try */
#define MCE_CONF_MAX_BACKLIGHT_PATH             "MaxBrightnessPath"

/** Default timeout for the high brightness mode; in seconds */
#define DEFAULT_HBM_TIMEOUT				1800	/* 30 min */

/** Path to the SysFS entry for the CABC controls */
#define DISPLAY_BACKLIGHT_PATH			"/sys/class/backlight"

/** CABC brightness file */
#define DISPLAY_CABC_BRIGHTNESS_FILE		"/brightness"

/** CABC maximum brightness file */
#define DISPLAY_CABC_MAX_BRIGHTNESS_FILE	"/max_brightness"

/** CABC mode file */
#define DISPLAY_CABC_MODE_FILE			"/cabc_mode"

/** CABC available modes file */
#define DISPLAY_CABC_AVAILABLE_MODES_FILE	"/cabc_available_modes"

/** Generic name for the display in newer hardware */
#define DISPLAY_DISPLAY0			"/display0"

/** The name of the directory for the Sony acx565akm display */
#define DISPLAY_ACX565AKM			"/acx565akm"

/** The name of the directory for the EID l4f00311 display */
#define DISPLAY_L4F00311			"/l4f00311"

/** The name of the directory for the Taal display */
#define DISPLAY_TAAL				"/taal"

/** The name of the directory for the Himalaya display */
#define DISPLAY_HIMALAYA			"/himalaya"

/** The name of the directory for ACPI controlled displays */
#define DISPLAY_ACPI_VIDEO0			"/acpi_video0"

/** Display device path */
#define DISPLAY_DEVICE_PATH			"/device"

/** Path to hardware dimming support */
#define DISPLAY_HW_DIMMING_FILE			"/dimming"

/** Low Power Mode file */
#define DISPLAY_LPM_FILE			"/lpm"

/** High Brightness Mode file */
#define DISPLAY_HBM_FILE			"/hbm"

/** CABC name for CABC disabled */
#define CABC_MODE_OFF				"off"

/** CABC name for UI mode */
#define CABC_MODE_UI				"ui"

/** CABC name for still image mode */
#define CABC_MODE_STILL_IMAGE			"still-image"

/** CABC name for moving image mode */
#define CABC_MODE_MOVING_IMAGE			"moving-image"

/** Default CABC mode */
#define DEFAULT_CABC_MODE			CABC_MODE_UI

/** Default CABC mode (power save mode active) */
#define DEFAULT_PSM_CABC_MODE			CABC_MODE_MOVING_IMAGE

/** Path to the SysFS entry for the generic display interface */
#define DISPLAY_GENERIC_PATH			"/sys/class/graphics/fb0/device/panel"

/** Generic brightness file */
#define DISPLAY_GENERIC_BRIGHTNESS_FILE		"/backlight_level"

/** Generic maximum brightness file */
#define DISPLAY_GENERIC_MAX_BRIGHTNESS_FILE	"/backlight_max"

/** Path to the GConf settings for the display */
#define MCE_GCONF_DISPLAY_PATH			"/system/osso/dsm/display"

/** ALS enabled setting */
#define MCE_GCONF_DISPLAY_ALS_ENABLED                   MCE_GCONF_DISPLAY_PATH "/als_enabled"

/** Default value for MCE_GCONF_DISPLAY_ALS_ENABLED */
#define ALS_ENABLED_DEFAULT                             true

/** ALS constrols brightness setting */
#define MCE_GCONF_DISPLAY_ALS_AUTOBRIGHTNESS            MCE_GCONF_DISPLAY_PATH "/als_autobrightness"

/** Default value for MCE_GCONF_DISPLAY_ALS_AUTOBRIGHTNESS settings */
#define ALS_AUTOBRIGHTNESS_DEFAULT                      true

/** ALS input filter setting */
#define MCE_GCONF_DISPLAY_ALS_INPUT_FILTER              MCE_GCONF_DISPLAY_PATH "/als_input_filter"

/** Default value for MCE_GCONF_DISPLAY_ALS_INPUT_FILTER */
#define ALS_INPUT_FILTER_DEFAULT                        "median"

/** ALS sample time GConf setting */
#define MCE_GCONF_DISPLAY_ALS_SAMPLE_TIME               MCE_GCONF_DISPLAY_PATH "/als_sample_time"
#define ALS_SAMPLE_TIME_DEFAULT                         125
#define ALS_SAMPLE_TIME_MIN                             50
#define ALS_SAMPLE_TIME_MAX                             1000

/** Path to the color profile GConf setting */
#define MCE_GCONF_DISPLAY_COLOR_PROFILE                 MCE_GCONF_DISPLAY_PATH "/color_profile"

/** Display brightness GConf setting */
#define MCE_GCONF_DISPLAY_BRIGHTNESS                    MCE_GCONF_DISPLAY_PATH "/display_brightness"

/** Display brightness level count GConf setting */
#define MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_COUNT        MCE_GCONF_DISPLAY_PATH "/max_display_brightness_levels"

/** Display brightness level size GConf setting */
#define MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_SIZE         MCE_GCONF_DISPLAY_PATH "/display_brightness_level_step"

/** List of possible dim timeouts GConf setting */
#define MCE_GCONF_DISPLAY_DIM_TIMEOUT_LIST              MCE_GCONF_DISPLAY_PATH "/possible_display_dim_timeouts"

/** Dim timeout GConf setting */
#define MCE_GCONF_DISPLAY_DIM_TIMEOUT                   MCE_GCONF_DISPLAY_PATH "/display_dim_timeout"

/** Blank timeout GConf setting */
#define MCE_GCONF_DISPLAY_BLANK_TIMEOUT                 MCE_GCONF_DISPLAY_PATH "/display_blank_timeout"

/** Blank from lockscreen timeout GConf setting */
#define MCE_GCONF_DISPLAY_BLANK_FROM_LOCKSCREEN_TIMEOUT MCE_GCONF_DISPLAY_PATH "/display_blank_from_locksreen_timeout"

/** Blank from lpm-on timeout GConf setting */
#define MCE_GCONF_DISPLAY_BLANK_FROM_LPM_ON_TIMEOUT     MCE_GCONF_DISPLAY_PATH "/display_blank_from_lpm_on_timeout"

/** Blank from lpm-off timeout GConf setting */
#define MCE_GCONF_DISPLAY_BLANK_FROM_LPM_OFF_TIMEOUT    MCE_GCONF_DISPLAY_PATH "/display_blank_from_lpm_off_timeout"

/** Never blank GConf setting */
#define MCE_GCONF_DISPLAY_NEVER_BLANK                   MCE_GCONF_DISPLAY_PATH "/display_never_blank"

/** Adaptive display dimming GConf setting */
#define MCE_GCONF_DISPLAY_ADAPTIVE_DIMMING              MCE_GCONF_DISPLAY_PATH "/use_adaptive_display_dimming"

/** Adaptive display threshold timeout GConf setting */
#define MCE_GCONF_DISPLAY_ADAPTIVE_DIM_THRESHOLD        MCE_GCONF_DISPLAY_PATH "/adaptive_display_dim_threshold"

/** Blanking inhibit GConf setting */
#define MCE_GCONF_BLANKING_INHIBIT_MODE                 MCE_GCONF_DISPLAY_PATH "/inhibit_blank_mode"

/** Kbd slide inhibit type */
typedef enum {
    /** Kbd slide state does not affect display blanking */
    KBD_SLIDE_INHIBIT_OFF                = 0,

    /** Keep display on while kbd slide is open */
    KBD_SLIDE_INHIBIT_STAY_ON_WHEN_OPEN  = 1,

    /** Allow dimming but not blanking  while kbd slide is open */
    KBD_SLIDE_INHIBIT_STAY_DIM_WHEN_OPEN = 2,
} kbd_slide_inhibit_t;

/** Kbd slide blanking inhibit GConf setting */
#define MCE_GCONF_KBD_SLIDE_INHIBIT                     MCE_GCONF_DISPLAY_PATH "/kbd_slide_inhibit_blank_mode"
#define DEFAULT_KBD_SLIDE_INHIBIT                       0 // = KBD_SLIDE_INHIBIT_OFF

/** Use Low Power Mode GConf setting */
#define MCE_GCONF_USE_LOW_POWER_MODE                    MCE_GCONF_DISPLAY_PATH "/use_low_power_mode"

/** Use autosuspend GConf setting */
#define MCE_GCONF_USE_AUTOSUSPEND                       MCE_GCONF_DISPLAY_PATH "/autosuspend_policy"

/** Use cpu scaling governor GConf setting */
#define MCE_GCONF_CPU_SCALING_GOVERNOR                  MCE_GCONF_DISPLAY_PATH "/cpu_scaling_governor"

/** Unresponsive lipstick core dump delay */
#define MCE_GCONF_LIPSTICK_CORE_DELAY                   MCE_GCONF_DISPLAY_PATH "/lipstick_core_dump_delay"

/** Blanking mode for display off requests GConf setting */
# define MCE_GCONF_DISPLAY_OFF_OVERRIDE                 MCE_GCONF_DISPLAY_PATH"/display_off_override"

/** Values for MCE_GCONF_DISPLAY_OFF_OVERRIDE setting */
typedef enum
{
	/** Display off request turns display off */
	DISPLAY_OFF_OVERRIDE_DISABLED,

	/** Display off request puts display to lpm state */
	DISPLAY_OFF_OVERRIDE_USE_LPM,
} display_off_blanking_mode_t;

/** Default brightness fade duration [ms]
 *
 * Used for all display state changes that do not have
 * a separate duration speficied, for example DIM->ON
 */
#define MCE_GCONF_BRIGHTNESS_FADE_DEFAULT_MS            MCE_GCONF_DISPLAY_PATH "/brightness_fade_default_ms"

/** Dimming brightness fade duration [ms]
 *
 * Used when changing display state changes to DIM
 * (except from OFF states, which use unblank duration).
 */
#define MCE_GCONF_BRIGHTNESS_FADE_DIMMING_MS            MCE_GCONF_DISPLAY_PATH "/brightness_fade_dimming_ms"

/** ALS brightness fade duration [ms]
 *
 * Used when brightness changes due to ambient light
 * sensor input and/or display brightness setting changes.
 */
#define MCE_GCONF_BRIGHTNESS_FADE_ALS_MS                MCE_GCONF_DISPLAY_PATH "/brightness_fade_als_ms"

/** Blanking brightness fade duration [ms]
 *
 * Used when making transition to display OFF states.
 */
#define MCE_GCONF_BRIGHTNESS_FADE_BLANK_MS              MCE_GCONF_DISPLAY_PATH "/brightness_fade_blank_ms"

/** Unblanking brightness fade duration [ms]
 *
 * Used when making transition from display OFF states.
 */
#define MCE_GCONF_BRIGHTNESS_FADE_UNBLANK_MS            MCE_GCONF_DISPLAY_PATH "/brightness_fade_unblank_ms"

/* NOTE: The following defines the legacy mce brightness scale. It is
 *       carved in stone for the sake of backwards compatibility. On
 *       startup mce will migrate existing, possibly modified by user
 *       brightness settings to 1-100 range - Which is then used for
 *       actual brightness control.
 */

/** Default display brightness on a scale from 1-5 */
#define DEFAULT_DISP_BRIGHTNESS			3	/* 60% */

/** Number of display brightness steps */
#define DEFAULT_DISP_BRIGHTNESS_STEP_COUNT	5

/** Logical size of each step; not sure if this has ever been used */
#define DEFAULT_DISP_BRIGHTNESS_STEP_SIZE	1

/** Default value for MCE_GCONF_DISPLAY_BLANK_TIMEOUT setting */
#define DEFAULT_BLANK_TIMEOUT			3

/** Default value for MCE_GCONF_DISPLAY_BLANK_FROM_LOCKSCREEN_TIMEOUT setting */
#define DEFAULT_BLANK_FROM_LOCKSCREEN_TIMEOUT	10

/** Default value for MCE_GCONF_DISPLAY_BLANK_FROM_LPM_ON_TIMEOUT setting */
#define DEFAULT_BLANK_FROM_LPM_ON_TIMEOUT	5

/** Default value for MCE_GCONF_DISPLAY_BLANK_FROM_LPM_OFF_TIMEOUT setting */
#define DEFAULT_BLANK_FROM_LPM_OFF_TIMEOUT	5

/** Default adaptive dimming threshold, in milliseconds */
#define DEFAULT_ADAPTIVE_DIMMING_ENABLED	TRUE	/* TRUE */

/** Default adaptive dimming threshold, in milliseconds */
#define DEFAULT_ADAPTIVE_DIMMING_THRESHOLD	3000	/* 3 seconds */

/** Default dim timeout, in seconds */
#define DEFAULT_DIM_TIMEOUT			30	/* 30 seconds */

/** Minimum blanking delay after bootup, in seconds */
#define AFTERBOOT_BLANKING_TIMEOUT		30

/**
 * Blank prevent timeout, in seconds;
 * Don't alter this, since this is part of the defined behaviour
 * for blanking inhibit that applications rely on
 */
#define BLANK_PREVENT_TIMEOUT			60	/* 60 seconds */

/**
 * Default maximum brightness;
 * used if the maximum brightness cannot be read from SysFS
 */
#define DEFAULT_MAXIMUM_DISPLAY_BRIGHTNESS	127

/** Dimmed display brightness GConf setting, in percent of hw maximum */
#define MCE_GCONF_DISPLAY_DIM_STATIC_BRIGHTNESS MCE_GCONF_DISPLAY_PATH "/display_dim_static"
#define DEFAULT_DISPLAY_DIM_STATIC_BRIGHTNESS   3

/** Dimmed display brightness GConf setting, in percent of current on level */
#define MCE_GCONF_DISPLAY_DIM_DYNAMIC_BRIGHTNESS MCE_GCONF_DISPLAY_PATH "/display_dim_dynamic"
#define DEFAULT_DISPLAY_DIM_DYNAMIC_BRIGHTNESS   50

/* High compositor dimming threshold setting
 *
 * If delta between display on and display dim backlight levels is smaller
 * than this value, compositor side fade to black animation is used instead.
 */
#define MCE_GCONF_DISPLAY_DIM_COMPOSITOR_HI     MCE_GCONF_DISPLAY_PATH "/display_dim_compositor_hi"
#define DEFAULT_DISPLAY_DIM_COMPOSITOR_HI       10

/** Low compositor dimming threshold setting
 *
 * If delta between display on and display dim backlight levels is smaller
 * than this value but still larger than MCE_GCONF_DISPLAY_DIM_COMPOSITOR_HI,
 * limited opacity compositor side fade to black animation is used instead.
 *
 * If the value is smaller than MCE_GCONF_DISPLAY_DIM_COMPOSITOR_HI, no
 * opacity interpolation is done i.e. compositor fading uses on/off
 * control at high threshold point.
 */
#define MCE_GCONF_DISPLAY_DIM_COMPOSITOR_LO     MCE_GCONF_DISPLAY_PATH "/display_dim_compositor_lo"
#define DEFAULT_DISPLAY_DIM_COMPOSITOR_LO       0

/** CPU scaling covernor policy states */
enum
{
	GOVERNOR_UNSET,
	GOVERNOR_DEFAULT,
	GOVERNOR_INTERACTIVE,
};

/** Default value for MCE_GCONF_DISPLAY_NEVER_BLANK setting */
#define DEFAULT_DISPLAY_NEVER_BLANK		0

/** Default value for MCE_GCONF_USE_LOW_POWER_MODE setting */
#define DEFAULT_USE_LOW_POWER_MODE		false

/** Default value for MCE_GCONF_LIPSTICK_CORE_DELAY setting */
#define DEFAULT_LIPSTICK_CORE_DELAY		30

/** Default value for MCE_GCONF_BRIGHTNESS_FADE_DEFAULT_MS setting */
#define DEFAULT_BRIGHTNESS_FADE_DEFAULT_MS	150

/** Default value for MCE_GCONF_BRIGHTNESS_FADE_DIMMING_MS setting */
#define DEFAULT_BRIGHTNESS_FADE_DIMMING_MS	1000

/** Default value for MCE_GCONF_BRIGHTNESS_FADE_ALS_MS setting */
#define DEFAULT_BRIGHTNESS_FADE_ALS_MS		1000

/** Default value for MCE_GCONF_BRIGHTNESS_FADE_BLANK_MS setting */
#define DEFAULT_BRIGHTNESS_FADE_BLANK_MS	100

/** Default value for MCE_GCONF_BRIGHTNESS_FADE_UNBLANK_MS setting */
#define DEFAULT_BRIGHTNESS_FADE_UNBLANK_MS	90

/** Default value for MCE_GCONF_DISPLAY_OFF_OVERRIDE setting
 *
 * Note: keep in sync with entry in builtin-gconf.c
 */
#define DEFAULT_DISPLAY_OFF_OVERRIDE		DISPLAY_OFF_OVERRIDE_DISABLED

/** Default value for MCE_GCONF_DISPLAY_DIM_TIMEOUT_LIST setting */
#define DEFAULT_DISPLAY_DIM_TIMEOUT_LIST	15,30,60,120,180

/** Use Orientation sensor GConf setting */
# define MCE_GCONF_ORIENTATION_SENSOR_ENABLED   MCE_GCONF_DISPLAY_PATH"/orientation_sensor_enabled"

/** Default value for MCE_GCONF_ORIENTATION_SENSOR_ENABLED setting */
#define DEFAULT_ORIENTATION_SENSOR_ENABLED      true

/** Flipover gesture detection enabled GConf setting */
# define MCE_GCONF_FLIPOVER_GESTURE_ENABLED     MCE_GCONF_DISPLAY_PATH"/flipover_gesture_enabled"
# define DEFAULT_FLIPOVER_GESTURE_ENABLED       true

/** Orientation change is user activity GConf setting */
# define MCE_GCONF_ORIENTATION_CHANGE_IS_ACTIVITY MCE_GCONF_DISPLAY_PATH"/orientation_change_is_activity"
# define DEFAULT_ORIENTATION_CHANGE_IS_ACTIVITY   true

/** Display blanking pause modes */
typedef enum {
    /** Ignore blanking pause requests */
    BLANKING_PAUSE_MODE_DISABLED  = 0,

    /** Blanking pause keeps display on */
    BLANKING_PAUSE_MODE_KEEP_ON   = 1,

    /** Display can be dimmed during Blanking pause */
    BLANKING_PAUSE_MODE_ALLOW_DIM = 2,
} blanking_pause_mode_t;

/** Blanking pause mode GConf setting */
#define MCE_GCONF_DISPLAY_BLANKING_PAUSE_MODE  MCE_GCONF_DISPLAY_PATH "/blanking_pause_mode"

/** Default value for MCE_GCONF_DISPLAY_BLANK_FROM_LOCKSCREEN_TIMEOUT setting */
#define DEFAULT_BLANKING_PAUSE_MODE	1 // = BLANKING_PAUSE_MODE_KEEP_ON

#endif /* _DISPLAY_H_ */
