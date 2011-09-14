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

/** Name of ALS configuration group */
#define MCE_CONF_ALS_GROUP			"ALS"

/** Name of the configuration key for the brightness level step-down policy */
#define MCE_CONF_STEP_DOWN_POLICY		"StepDownPolicy"

/** Prefix of configuration key for color phase adjustments */
#define MCE_CONF_CPA_PREFIX			"PhaseProfile-"

/*  Paths for Avago APDS990x (QPDS-T900) ALS */

/** Device path for Avago ALS */
#define ALS_DEVICE_PATH_AVAGO			"/dev/apds990x0"

#ifndef APDS990X_ALS_SATURATED
/** Read is saturated */
#define APDS990X_ALS_SATURATED			0x1
#endif /* APDS990X_ALS_SATURATED */

#ifndef APDS990X_ALS_UPDATED
/** Sensor has up to date data */
#define APDS990X_ALS_UPDATED			0x4
#endif /* APDS990X_ALS_UPDATED */

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

/** Path to the GConf settings for the display */
#ifndef MCE_GCONF_DISPLAY_PATH
#define MCE_GCONF_DISPLAY_PATH			"/system/osso/dsm/display"
#endif /* MCE_GCONF_DISPLAY_PATH */
/** Path to the ALS enabled GConf setting */
#define MCE_GCONF_DISPLAY_ALS_ENABLED_PATH	MCE_GCONF_DISPLAY_PATH "/als_enabled"

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

/** Path to display */
#define DISPLAY_PATH			"/sys/devices/platform/omapdss/display0"
/** Display hardware revision path */
#define DISPLAY_REVISION_PATH		DISPLAY_PATH "/hw_revision"

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

/* Colour phase adjustment matrices */

/** Colour phase calibration for RM-696/RM-716 */
static const cpa_profile_struct rm696_phase_profile[] = {
	{	/* 0-100 lux; */
		{ 0, 3.3 * 100, 0 },
		"  199   28   28"
		"    8  209   38"
		"    8    8  239"
	}, {	/* 100-1000 lux; same as previous */
		{ 3.3 * 100, 3.3 * 1000, 0 },
		"  199   28   28"
		"    8  209   38"
		"    8    8  239"
	}, {	/* 1000-10000 lux; normal OLED colours */
		{ 3.3 * 1000, 3.3 * 10000, 0 },
		"  255    0    0"
		"    0  255    0"
		"    0    0  255"
	}, {	/* 10000-inf lux; slightly boosted colours */
		{ 3.3 * 10000, -1, 0 },
		"  275  -10  -10"
		"  -10  275  -10"
		"  -10  -10  275"
	}, {	/* 10000-inf lux, HBM level: 1; more boosted colours */
		{ 3.3 * 10000, -1, 1 },
		"  295  -20  -20"
		"  -20  295  -20"
		"  -20  -20  295"
	}, {	/* 10000-inf lux, HBM level: 2; over-saturated colours */
		{ 3.3 * 10000, -1, 2 },
		"  315  -30  -30"
		"  -30  315  -30"
		"  -30  -30  315"
	}, {
		{ -1, -1, -1 },
		NULL
	}
};

/** Colour phase calibration for RM-680/RM-690 */
static const cpa_profile_struct rm680_phase_profile[] = {
	{	/* 0-inf lux */
		{ 0, -1, 0 },
		"  325  -35  -35"
		"  -30  315  -30"
		"  -20  -20  295"
	}, {
		{ -1, -1, -1 },
		NULL
	}
};

/**
 * ALS profile for the display in:
 * RM-716
 * RM-696
 *
 * [255-212] 100%
 * [211-154] 83%
 * [153-110] 60%
 * [109-77] 42%
 * [76-52] 30%
 * [51-26] 20%
 * [25-13] 10%
 * [12-1] 5%
 * [0] 0%
 */
static als_profile_struct display_als_profiles_rm696[] = {
	{       /* Minimum */
		{
			{ 3.3 * 30, 3.3 * 50 },
			{ 3.3 * 70, 3.3 * 100 },
			{ 3.3 * 150, 3.3 * 200 },
			{ 3.3 * 800, 3.3 * 1100 },
			{ 3.3 * 5000, 3.3 * 10000 },
			{ 3.3 * 10000, 3.3 * 35000 },
			{ 3.3 * 20000, 3.3 * 50000 },
			{ -1, -1 },
		}, { 5, 10, 20, 30, 42, 60, 83, 100 }
	}, {    /* Economy */
		{
			{ 3.3 * 10, 3.3 * 15 },
			{ 3.3 * 30, 3.3 * 50 },
			{ 3.3 * 50, 3.3 * 100 },
			{ 3.3 * 150, 3.3 * 200 },
			{ 3.3 * 800, 3.3 * 1100 },
			{ 3.3 * 5000, 3.3 * 10000 },
			{ 3.3 * 10000, 3.3 * 35000 },
			{ -1, -1 },
		}, { 5, 10, 20, 30, 42, 60, 83, 100 }
	}, {    /* Normal */
		{
			{ 3.3 * 3, 3.3 * 5 },
			{ 3.3 * 10, 3.3 * 15 },
			{ 3.3 * 20, 3.3 * 30 },
			{ 3.3 * 50, 3.3 * 100 },
			{ 3.3 * 150, 3.3 * 200 },
			{ 3.3 * 800, 3.3 * 1100 },
			{ 3.3 * 5000, 3.3 * 10000 },
			{ -1, -1 },
		}, { 5, 10, 20, 30, 42, 60, 83, 100 }
	}, {    /* Bright */
		{
			{ 3.3 * 0, 3.3 * 3 },
			{ 3.3 * 3, 3.3 * 10 },
			{ 3.3 * 10, 3.3 * 20 },
			{ 3.3 * 20, 3.3 * 50 },
			{ 3.3 * 50, 3.3 * 100 },
			{ 3.3 * 100, 3.3 * 200 },
			{ 3.3 * 800, 3.3 * 1100 },
			{ 3.3 * 5000, 3.3 * 10000 },
			{ 3.3 * 20000, 3.3 * 40000 },
			{ -1, -1 },
		}, { 5, 10, 20, 30, 42, 60, 83, 100, 100 + (1 << 8), 100 + (2 << 8) }
	}, {    /* Maximum */
		{
			{ 3.3 * 20, 3.3 * 30 },
			{ 3.3 * 50, 3.3 * 100 },
			{ 3.3 * 5000, 3.3 * 10000 },
			{ -1, -1 },
		}, { 30, 60, 100, 100 + (2 << 8) }
	}
};

/**
 * ALS profile for the display in:
 * RX-71 (HWID: 3xxx)
 * RM-680
 * RM-690
 */
static als_profile_struct display_als_profiles_rm680[] = {
	{       /* Minimum */
		{
			{ 3, 5 },
			{ 20, 30 },
			{ 50, 100 },
			{ 150, 200 },
			{ 800, 1100 },
			{ 8000, 20000 },
			{ 10000, 35000 },
			{ 20000, 50000 },
			{ -1, -1 },
		}, { 1, 3, 6, 13, 22, 35, 50, 70, 100 }
	}, {    /* Economy */
		{
                        { 3, 5 },
                        { 15, 15 },
                        { 20, 30 },
                        { 50, 100 },
                        { 150, 200 },
                        { 800, 1100 },
                        { 8000, 20000 },
                        { 10000, 35000 },
			{ -1, -1 },
		}, { 3, 4, 6, 10, 22, 35, 60, 70, 100 }
	}, {    /* Normal */
		{
                        { 3, 5 },
                        { 10, 15 },
                        { 20, 30 },
                        { 50, 100 },
			{ 150, 200 },
			{ 800, 1100 },
			{ 5000, 10000 },
			{ 8000, 20000 },
			{ -1, -1 },
		}, { 4, 6, 10, 16, 30, 50, 70, 83, 100 }
	}, {    /* Bright */
		{
			{ 3, 5 },
			{ 10, 15 },
			{ 20, 30 },
			{ 50, 100 },
			{ 150, 200 },
			{ 800, 1100 },
			{ 5000, 10000 },
			{ -1, -1 },
		}, { 5, 10, 16, 22, 35, 60, 83, 100 }
	}, {    /* Maximum */
		{
			{ 20, 30 },
			{ 50, 100 },
			{ -1, -1 },
		}, { 30, 50, 100 }
	}
};

/**
 * ALS profile for the display in:
 * RX-51
 */
static als_profile_struct display_als_profiles_rx51[] = {
	{       /* Minimum */
		{
			{ 24, 32 },
			{ 160, 320 },
			{ 720, 1200 },
			{ 14400, 17600 },
			{ -1, -1 },
		}, { 3, 10, 30, 50, 1 }
	}, {    /* Economy */
		{
                        { 24, 40 },
                        { 100, 200 },
                        { 300, 500 },
                        { 720, 1200 },
			{ -1, -1 },
		}, { 10, 20, 40, 60, 80 }
	}, {    /* Normal */
		{
                        { 24, 40 },
                        { 100, 200 },
                        { 300, 500 },
                        { 720, 1200 },
			{ -1, -1 },
		}, { 17, 30, 60, 90, 100 }
	}, {    /* Bright */
		{
			{ 24, 40 },
                        { 50, 70 },
			{ 60, 80 },
			{ 100, 160 },
			{ 200, 300 },
			{ -1, -1 },
		}, { 25, 40, 60, 75, 90, 100 }
	}, {    /* Maximum */
		{
			{ 32, 64 },
			{ 160, 320 },
			{ -1, -1 },
/* XXX: Insane request from higher management */
		}, { 100, 100, 100 }
//		}, { 35, 80, 100 }
	}
};

/**
 * ALS profile for the display in:
 * RX-48
 * RX-44
 */
static als_profile_struct display_als_profiles_rx44[] = {
	{       /* Minimum */
		{
			{ 10000, 13000 },
			{ -1, -1 },
		}, { 5, 20 }
	}, {    /* Economy */
		{
			{ 2, 4 },
			{ 24, 45 },
			{ 260, 400 },
			{ 10000, 13000 },
			{ -1, -1 },
		}, { 5, 20, 40, 50, 70 }
	}, {    /* Normal */
		{
			{ 2, 4 },
			{ 24, 45 },
			{ 260, 400 },
			{ 10000, 13000 },
			{ -1, -1 },
		}, { 10, 20, 50, 80, 100 }
	}, {    /* Bright */
		{
			{ 2, 4 },
			{ 24, 45 },
			{ 260, 400 },
			{ 10000, 13000 },
			{ -1, -1 },
		}, { 30, 60, 80, 90, 100 }
	}, {    /* Maximum */
		{
			{ 2, 4 },
			{ 8, 12 },
			{ -1, -1 },
		}, { 50, 80, 100 }
	}
};

/**
 * ALS profile for the monochrome LED in:
 * RM-716 - FIXME/TODO: No idea if usable for RM-696, just a copy of RM-680
 * RM-696 - FIXME/TODO: No idea if usable for RM-696, just a copy of RM-680
 */
static als_profile_struct led_als_profiles_rm696[] = {
	{	/* Minimum; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Economy; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Normal */
		{
			{ 3, 5 },
			{ -1, -1 },
		}, { 80, 100 }
	}, {	/* Bright; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Maximum; unused */
		{ { -1, -1 } },
		{ }
	}
};

/**
 * ALS profile for the monochrome LED in:
 * RX-71 (HWID: 3xxx)
 * RM-680
 * RM-690
 */
static als_profile_struct led_als_profiles_rm680[] = {
	{	/* Minimum; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Economy; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Normal */
		{
			{ 3, 5 },
			{ -1, -1 },
		}, { 80, 100 }
	}, {	/* Bright; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Maximum; unused */
		{ { -1, -1 } },
		{ }
	}
};

/**
 * ALS profile for the RGB LED in:
 * RX-51
 */
static als_profile_struct led_als_profiles_rx51[] = {
	{	/* Minimum; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Economy; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Normal */
		{
			{ 32, 64 },
			{ 100, 1000 },
			{ -1, -1 },
		}, { 5, 5, 0 }
	}, {	/* Bright; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Maximum; unused */
		{ { -1, -1 } },
		{ }
	}
};

/**
 * ALS profile for the RGB LED in:
 * RX-48
 * RX-44
 */
static als_profile_struct led_als_profiles_rx44[] = {
	{	/* Minimum; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Economy; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Normal */
		{
			{ 3, 5 },
			{ 15, 27 },
			{ -1, -1 },
		}, { 10, 30, 50 }
	}, {	/* Bright */
		{
			{ 3, 5 },
			{ 15, 27 },
			{ -1, -1 },
		}, { 30, 50, 100 }
	}, {	/* Maximum */
		{
			{ 3, 5 },
			{ -1, -1 },
		}, { 50, 100 }
	}
};

/**
 * ALS profile for the keyboard backlight in:
 * RX-71 (HWID: 3xxx)
 * RM-680
 * RM-690
 */
static als_profile_struct kbd_als_profiles_rm680[] = {
	{	/* Minimum; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Economy; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Normal */
		{
			{ 24, 40 },
			{ 100, 1000 },
			{ -1, -1 },
		}, { 25, 50, 0 }
	}, {	/* Bright */
		{ { -1, -1 } },
		{ }
	}, {	/* Maximum; unused */
		{ { -1, -1 } },
		{ }
	}
};

/**
 * ALS profile for the keyboard backlight in:
 * RX-51
 */
static als_profile_struct kbd_als_profiles_rx51[] = {
	{	/* Minimum; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Economy; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Normal */
		{
			{ 24, 40 },
			{ -1, -1 },
		}, { 50, 0 }
	}, {	/* Bright */
		{ { -1, -1 } },
		{ }
	}, {	/* Maximum; unused */
		{ { -1, -1 } },
		{ }
	}
};

/**
 * ALS profile for the keyboard backlight in:
 * RX-48
 * RX-44
 */
static als_profile_struct kbd_als_profiles_rx44[] = {
	{	/* Minimum; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Economy; unused */
		{ { -1, -1 } },
		{ }
	}, {	/* Normal */
		{
			{ 3, 5 },
			{ 15, 27 },
			{ -1, -1 },
		}, { 50, 100, 0 }
	}, {	/* Bright */
		{
			{ 3, 5 },
			{ 15, 27 },
			{ -1, -1 },
		}, { 80, 100, 0 }
	}, {	/* Maximum; unused */
		{ { -1, -1 } },
		{ }
	}
};

/** ALS profiles */
typedef enum {
	ALS_PROFILE_MINIMUM = 0,		/**< Minimum profile */
	ALS_PROFILE_ECONOMY,			/**< Economy profile */
	ALS_PROFILE_NORMAL,			/**< Normal profile */
	ALS_PROFILE_BRIGHT,			/**< Bright profile */
	ALS_PROFILE_MAXIMUM			/**< Maximum profile */
} als_profile_t;

#endif /* _FILTER_BRIGHTNESS_ALS_H_ */
