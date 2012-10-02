/**
 * @file display.h
 * Headers for the display module
 * <p>
 * Copyright © 2007-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author Mika Laitio <ext-mika.1.laitio@nokia.com>
 * based on the display.c code by
 * David Weinehall <david.weinehall@nokia.com>
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
#ifndef _DISPLAYMEEGO_H_
#define _DISPLAYMEEGO_H_

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
#define DEFAULT_BRIGHTNESS_DECREASE_CONSTANT_TIME	5000

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

/** Path to the GConf settings for the display */
#ifndef MCE_GCONF_DISPLAY_PATH
#define MCE_GCONF_DISPLAY_PATH			"/system/osso/dsm/display"
#endif /* MCE_GCONF_DISPLAY_PATH */
/** Path to the display brightness GConf setting */
#define MCE_GCONF_DISPLAY_BRIGHTNESS_PATH	MCE_GCONF_DISPLAY_PATH "/display_brightness"
/** Path to the blank timeout GConf setting */
#define MCE_GCONF_DISPLAY_BLANK_TIMEOUT_PATH	MCE_GCONF_DISPLAY_PATH "/display_blank_timeout"
/** Path to the blanking inhibit GConf setting */
#define MCE_GCONF_BLANKING_INHIBIT_MODE_PATH	MCE_GCONF_DISPLAY_PATH "/inhibit_blank_mode"

/** Default display brightness percentage (0 - 100) */
#define DEFAULT_DISP_BRIGHTNESS			40
/** Default display brightness (power save mode active) percentage (0 - 100) */
#define DEFAULT_PSM_DISP_BRIGHTNESS		20	/* 20% */
/** Default blank timeout, in seconds */
#define DEFAULT_BLANK_TIMEOUT			60	/* 60 seconds */
#define BOOTUP_ADDITIONAL_TIMEOUT		120	/* 120 seconds */

/**
 * Blank prevent timeout, in seconds;
 * Don't alter this, since this is part of the defined behavior
 * for blanking inhibit that applications rely on
 */
#define BLANK_PREVENT_TIMEOUT			60	/* 60 seconds */

/**
 * Default maximum brightness;
 * used if the maximum brightness cannot be read from SysFS
 */
#define DEFAULT_MAXIMUM_DISPLAY_BRIGHTNESS	127

/** Maximum number of monitored services that calls blanking pause */
#define MAX_MONITORED_SERVICES			5

#endif /* _DISPLAYMEEGO_H_ */
