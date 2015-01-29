/**
 * @file filter-brightness-als.h
 * Headers for the Ambient Light Sensor level adjusting filter module
 * for display backlight, key backlight, and LED brightness
 * <p>
 * Copyright © 2007-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Tuomo Tanskanen <ext-tuomo.1.tanskanen@nokia.com>
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
#ifndef _FILTER_BRIGHTNESS_ALS_H_
#define _FILTER_BRIGHTNESS_ALS_H_

#include <glib.h>

/** Path to get the display manufacturer */
#define DISPLAY_HARDWARE_REVISION_PATH "/sys/devices/omapdss/display0/hw_revision"

/** Name of common group in color profiles conf file */
#define MCE_CONF_COMMON_GROUP	"Common"

/** Name of default color profile id key in color profiles conf file */
#define MCE_CONF_DEFAULT_PROFILE_ID_KEY	"DefaultProfile"

/** Name of the hardcoded color profile */
#define COLOR_PROFILE_ID_HARDCODED	"hardcoded"

/** Color profile group prefix */
#define MCE_COLOR_PROFILE_GROUP_PREFIX		"Display-"

/** Name of ALS configuration group */
#define MCE_CONF_ALS_GROUP			"ALS"

/** Name of the configuration key for the brightness level step-down policy */
#define MCE_CONF_STEP_DOWN_POLICY		"StepDownPolicy"

/*  Paths for Avago APDS990x (QPDS-T900) ALS */

/** Device path for Avago ALS */
#define ALS_DEVICE_PATH_AVAGO			"/dev/apds990x0"

#ifndef APDS990X_ALS_SATURATED
/** Read is saturated */
# define APDS990X_ALS_SATURATED			0x1
#endif

#ifndef APDS990X_ALS_UPDATED
/** Sensor has up to date data */
# define APDS990X_ALS_UPDATED			0x4
#endif

/** Struct for the Avago data */
struct avago_als {
	/** The filtered ambient light in lux */
	guint32 lux;			/* 10x scale */
	/** The raw ambient light in lux */
	guint32 lux_raw;		/* 10x scale */
	/** The filtered proximity */
	guint16 ps;
	/** The raw proximity */
	guint16 ps_raw;
	/** The sensor status */
	guint16 status;
} __attribute__((packed));

/** Base path to the Avago ALS */
#define ALS_PATH_AVAGO			"/sys/class/misc/apds990x0/device"

/** Path to the first calibration point for the Avago ALS */
#define ALS_CALIB_PATH_AVAGO		ALS_PATH_AVAGO "/als_calib"

/** ALS threshold range for the Avago ALS */
#define ALS_THRESHOLD_RANGE_PATH_AVAGO	ALS_PATH_AVAGO "/als_threshold_range"

/** Maximun threshold value for the Avago ALS */
#define ALS_THRESHOLD_MAX_AVAGO		655350

/* Paths for the Dipro (BH1770GLC/SFH7770) ALS */

/** Device path for the Dipro ALS */
#define ALS_DEVICE_PATH_DIPRO		"/dev/bh1770glc_als"

/** Struct for the Dipro data */
struct dipro_als {
	/** The ambient light in lux */
	guint16 lux;
} __attribute__((packed));

/** Base path to the Dipro ALS */
#define ALS_PATH_DIPRO			"/sys/class/misc/bh1770glc_als/device"

/** Path to the first calibration point for the Dipro ALS */
#define ALS_CALIB_PATH_DIPRO		ALS_PATH_DIPRO "/als_calib"

/** ALS threshold range for the Dipro ALS */
#define ALS_THRESHOLD_RANGE_PATH_DIPRO	ALS_PATH_DIPRO "/als_thres_range"

/** Maximun threshold value for the Dipro ALS */
#define ALS_THRESHOLD_MAX_DIPRO		65535

/* Paths for the TSL2563 ALS */

/** Base path to the TSL2563 ALS */
#define ALS_PATH_TSL2563		"/sys/class/i2c-adapter/i2c-2/2-0029"

/** Path to the TSL2563 ALS lux value */
#define ALS_LUX_PATH_TSL2563		ALS_PATH_TSL2563 "/lux"

/** Path to the first calibration point for the TSL2563 ALS */
#define ALS_CALIB0_PATH_TSL2563		ALS_PATH_TSL2563 "/calib0"

/** Path to the second calibration point for the TSL2563 ALS */
#define ALS_CALIB1_PATH_TSL2563		ALS_PATH_TSL2563 "/calib1"

/* Paths for the TSL2562 ALS */

/** Base path to the TSL2562 ALS */
#define ALS_PATH_TSL2562	"/sys/devices/platform/i2c_omap.2/i2c-0/0-0029"

/** Path to the TSL2562 ALS lux value */
#define ALS_LUX_PATH_TSL2562		ALS_PATH_TSL2562 "/lux"

/** Path to the first calibration point for the TSL2562 ALS */
#define ALS_CALIB0_PATH_TSL2562		ALS_PATH_TSL2562 "/calib0"

/** Path to the second calibration point for the TSL2562 ALS */
#define ALS_CALIB1_PATH_TSL2562		ALS_PATH_TSL2562 "/calib1"

/** Default value for als enabled settings */
#define ALS_ENABLED_DEFAULT		TRUE

/** Default value for als input filter settings */
#define ALS_INPUT_FILTER_DEFAULT	"disabled"

/** Default value for als sample time settings */
#define ALS_SAMPLE_TIME_DEFAULT		125

#define ALS_SAMPLE_TIME_MIN		50
#define ALS_SAMPLE_TIME_MAX		1000

/** Default ALS polling frequency when the display is on */
#define ALS_DISPLAY_ON_POLL_FREQ	1500		/* Milliseconds */

/** Default ALS polling frequency when the display is dimmed */
#define ALS_DISPLAY_DIM_POLL_FREQ	5000		/* Milliseconds */

/**
 * Default ALS polling frequency when the display is off
 *
 * 0 disables polling completely;
 * with hardware that supports power saving
 * in a better way, 60000 should be used
 */
#define ALS_DISPLAY_OFF_POLL_FREQ	0		/* Milliseconds */

/**
 * Define this to re-initialise the median filter on display blank;
 * this will trigger a re-read on wakeup
 */
#define ALS_DISPLAY_OFF_FLUSH_FILTER

/** Brightness stepdown delay, secs */
#define ALS_BRIGHTNESS_STEPDOWN_DELAY	5

/** Window size for the median filter */
#define MEDIAN_FILTER_WINDOW_SIZE	5

/** Sysinfo identifier for the ALS calibration values */
#define ALS_CALIB_IDENTIFIER		"/device/als_calib"

/** Number of ranges in ALS profile */
#define ALS_RANGES			11

/** Path to display manager */
#define DISPLAY_MANAGER_PATH			"/sys/devices/platform/omapdss/manager0"

/** Colour phase adjustment enable path */
#define COLOUR_PHASE_ENABLE_PATH		DISPLAY_MANAGER_PATH "/cpr_enable"

/** Colour phase adjustment coefficients path */
#define COLOUR_PHASE_COEFFICIENTS_PATH		DISPLAY_MANAGER_PATH "/cpr_coef"

/** ALS profile */
typedef struct {
	/** Lower and upper bound for each brightness range */
	const gint range[ALS_RANGES][2];
	/** Brightness in % + possible HBM boost (boost level * 256) */
	const gint value[ALS_RANGES + 1];
} als_profile_struct;

/** Colour phase adjustment matrix */
typedef struct {
	/**
	 * Lower and upper bound for each brightness range,
	 * followed by high brightness mode level
	 */
	gint range[3];
	/**
	  * Colour phase adjustment matrix for the specified range;
	  * 9 space separated integers
	  */
	const gchar *const coefficients;
} cpa_profile_struct;

/** Colour profile */
typedef struct {
	/**
	 * Color phase adjustment matrix
	 */
	cpa_profile_struct *profiles;
	/**
	 * Name of profile
	 */
	gchar *name;
} cpa_profile_entry;

/* Colour phase adjustment matrices in filter-brightness-als.c */

/** ALS profiles */
typedef enum {
	ALS_PROFILE_COUNT  = 21,		/**< Number of profiles */
} als_profile_t;

#endif /* _FILTER_BRIGHTNESS_ALS_H_ */
