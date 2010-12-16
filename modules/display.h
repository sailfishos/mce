/**
 * @file display.h
 * Headers for the display module
 * <p>
 * Copyright © 2007-2010 Nokia Corporation and/or its subsidiary(-ies).
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

/** Name of Display configuration group */
#define MCE_CONF_DISPLAY_GROUP			"Display"

/** Name of the configuration key for the brightness increase policy */
#define MCE_CONF_BRIGHTNESS_INCREASE_POLICY	"BrightnessIncreasePolicy"

/** Name of the configuration key for the step-time for brightness increase */
#define MCE_CONF_STEP_TIME_INCREASE		"StepTimeIncrease"

/** Name of the configuration key for the constant time brightness increase */
#define MCE_CONF_CONSTANT_TIME_INCREASE		"ConstantTimeIncrease"

/** Name of the configuration key for the brightness decrease policy */
#define MCE_CONF_BRIGHTNESS_DECREASE_POLICY	"BrightnessDecreasePolicy"

/** Name of the configuration key for the step-time for brightness decrease */
#define MCE_CONF_STEP_TIME_DECREASE		"StepTimeDecrease"

/** Name of the configuration key for the constant time brightness decrease */
#define MCE_CONF_CONSTANT_TIME_DECREASE		"ConstantTimeDecrease"

/** Default brightness increase step-time */
#define DEFAULT_BRIGHTNESS_INCREASE_STEP_TIME		5

/** Default brightness increase constant time */
#define DEFAULT_BRIGHTNESS_INCREASE_CONSTANT_TIME	2000

/** Default brightness decrease step-time */
#define DEFAULT_BRIGHTNESS_DECREASE_STEP_TIME		10

/** Default brightness decrease constant time */
#define DEFAULT_BRIGHTNESS_DECREASE_CONSTANT_TIME	3000

/** Path to the SysFS entry for the CABC controls */
#define DISPLAY_CABC_PATH			"/sys/class/backlight"
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

/** Path to hardware dimming support */
#define DISPLAY_HARDWARE_DIMMING		"/sys/devices/omapdss/display0/dimming"

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

/** Path to the GConf settings for the display */
#ifndef MCE_GCONF_DISPLAY_PATH
#define MCE_GCONF_DISPLAY_PATH			"/system/osso/dsm/display"
#endif /* MCE_GCONF_DISPLAY_PATH */
/** Path to the display brightness GConf setting */
#define MCE_GCONF_DISPLAY_BRIGHTNESS_PATH	MCE_GCONF_DISPLAY_PATH "/display_brightness"
/** Path to the list of possible dim timeouts GConf setting */
#define MCE_GCONF_DISPLAY_DIM_TIMEOUT_LIST_PATH	MCE_GCONF_DISPLAY_PATH "/possible_display_dim_timeouts"
/** Path to the dim timeout GConf setting */
#define MCE_GCONF_DISPLAY_DIM_TIMEOUT_PATH	MCE_GCONF_DISPLAY_PATH "/display_dim_timeout"
/** Path to the blank timeout GConf setting */
#define MCE_GCONF_DISPLAY_BLANK_TIMEOUT_PATH	MCE_GCONF_DISPLAY_PATH "/display_blank_timeout"
/** Path to the adaptive display dimming GConf setting */
#define MCE_GCONF_DISPLAY_ADAPTIVE_DIMMING_PATH	MCE_GCONF_DISPLAY_PATH "/use_adaptive_display_dimming"
/** Path to the adaptive display threshold timeout GConf setting */
#define MCE_GCONF_DISPLAY_ADAPTIVE_DIM_THRESHOLD_PATH	MCE_GCONF_DISPLAY_PATH "/adaptive_display_dim_threshold"
/** Path to the blanking inhibit GConf setting */
#define MCE_GCONF_BLANKING_INHIBIT_MODE_PATH	MCE_GCONF_DISPLAY_PATH "/inhibit_blank_mode"

/** Default display brightness on a scale from 1-5 */
#define DEFAULT_DISP_BRIGHTNESS			3	/* 60% */
/** Default display brightness (power save mode active) on a scale from 1-5 */
#define DEFAULT_PSM_DISP_BRIGHTNESS		1	/* 20% */
/** Default blank timeout, in seconds */
#define DEFAULT_BLANK_TIMEOUT			3	/* 3 seconds */
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

/** Maximum number of monitored services that calls blanking pause */
#define MAX_MONITORED_SERVICES			5

#endif /* _DISPLAY_H_ */
