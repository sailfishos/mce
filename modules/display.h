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

/** Path to the framebuffer device */
#define FB_DEVICE				"/dev/fb0"

#ifndef MCE_GCONF_DISPLAY_PATH
/** Path to the GConf settings for the display */
# define MCE_GCONF_DISPLAY_PATH			"/system/osso/dsm/display"
#endif

/** Path to the display brightness GConf setting */
#define MCE_GCONF_DISPLAY_BRIGHTNESS_PATH	MCE_GCONF_DISPLAY_PATH "/display_brightness"

/** Path to the display brightness level count GConf setting */
#define MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_COUNT_PATH	MCE_GCONF_DISPLAY_PATH "/max_display_brightness_levels"

/** Path to the display brightness level size GConf setting */
#define MCE_GCONF_DISPLAY_BRIGHTNESS_LEVEL_SIZE_PATH	MCE_GCONF_DISPLAY_PATH "/display_brightness_level_step"

/** Path to the list of possible dim timeouts GConf setting */
#define MCE_GCONF_DISPLAY_DIM_TIMEOUT_LIST_PATH	MCE_GCONF_DISPLAY_PATH "/possible_display_dim_timeouts"

/** Path to the dim timeout GConf setting */
#define MCE_GCONF_DISPLAY_DIM_TIMEOUT_PATH	MCE_GCONF_DISPLAY_PATH "/display_dim_timeout"

/** Path to the blank timeout GConf setting */
#define MCE_GCONF_DISPLAY_BLANK_TIMEOUT_PATH	MCE_GCONF_DISPLAY_PATH "/display_blank_timeout"

/** Path to the never blank GConf setting */
#define MCE_GCONF_DISPLAY_NEVER_BLANK_PATH	MCE_GCONF_DISPLAY_PATH "/display_never_blank"

/** Path to the adaptive display dimming GConf setting */
#define MCE_GCONF_DISPLAY_ADAPTIVE_DIMMING_PATH	MCE_GCONF_DISPLAY_PATH "/use_adaptive_display_dimming"

/** Path to the adaptive display threshold timeout GConf setting */
#define MCE_GCONF_DISPLAY_ADAPTIVE_DIM_THRESHOLD_PATH	MCE_GCONF_DISPLAY_PATH "/adaptive_display_dim_threshold"

/** Path to the blanking inhibit GConf setting */
#define MCE_GCONF_BLANKING_INHIBIT_MODE_PATH	MCE_GCONF_DISPLAY_PATH "/inhibit_blank_mode"

/** Path to the use Low Power Mode GConf setting */
#define MCE_GCONF_USE_LOW_POWER_MODE_PATH	MCE_GCONF_DISPLAY_PATH "/use_low_power_mode"

/** Path to the use autosuspend GConf setting */
#define MCE_GCONF_USE_AUTOSUSPEND_PATH		MCE_GCONF_DISPLAY_PATH "/autosuspend_policy"

/** Path to the use cpu scaling governor GConf setting */
#define MCE_GCONF_CPU_SCALING_GOVERNOR_PATH	MCE_GCONF_DISPLAY_PATH "/cpu_scaling_governor"

/** Path to the unresponsive lipstick core dump delay */
#define MCE_GCONF_LIPSTICK_CORE_DELAY_PATH	MCE_GCONF_DISPLAY_PATH "/lipstick_core_dump_delay"

/** Path to the default brightness fade duration [ms] */
#define MCE_GCONF_BRIGHTNESS_FADE_DEF           MCE_GCONF_DISPLAY_PATH "/brightness_fade_def"

/** Path to the dimming brightness fade duration [ms] */
#define MCE_GCONF_BRIGHTNESS_FADE_DIM           MCE_GCONF_DISPLAY_PATH "/brightness_fade_dim"

/** Path to the ALS brightness fade duration [ms] */
#define MCE_GCONF_BRIGHTNESS_FADE_ALS           MCE_GCONF_DISPLAY_PATH "/brightness_fade_als"

/** Path to the blanking brightness fade duration [ms] */
#define MCE_GCONF_BRIGHTNESS_FADE_BLANK         MCE_GCONF_DISPLAY_PATH "/brightness_fade_blank"

/** Path to the unblanking brightness fade duration [ms] */
#define MCE_GCONF_BRIGHTNESS_FADE_UNBLANK       MCE_GCONF_DISPLAY_PATH "/brightness_fade_unblank"

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

/** Default blank timeout, in seconds */
#define DEFAULT_BLANK_TIMEOUT			3	/* 3 seconds */

/**
 * Default blank timeout, in seconds, when low power mode is active
 * and the proximity sensor indicates proximity
 */
#define DEFAULT_LPM_PROXIMITY_BLANK_TIMEOUT	5	/* 5 seconds */

/** Default blank timeout, in seconds, when low power mode is active */
#define DEFAULT_LPM_BLANK_TIMEOUT		3	/* 3 seconds */

/** Default adaptive dimming threshold, in milliseconds */
#define DEFAULT_ADAPTIVE_DIMMING_ENABLED	TRUE	/* TRUE */

/** Default adaptive dimming threshold, in milliseconds */
#define DEFAULT_ADAPTIVE_DIMMING_THRESHOLD	3000	/* 3 seconds */

/** Default dim timeout, in seconds */
#define DEFAULT_DIM_TIMEOUT			30	/* 30 seconds */

/** Additional dim timeout during bootup, in seconds */
#define BOOTUP_DIM_ADDITIONAL_TIMEOUT		120	/* 120 seconds */

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

/** Default dim brightness, in percent */
#define DEFAULT_DIM_BRIGHTNESS			3

/** CPU scaling covernor policy states */
enum
{
	GOVERNOR_UNSET,
	GOVERNOR_DEFAULT,
	GOVERNOR_INTERACTIVE,
};

#endif /* _DISPLAY_H_ */
