/**
 * @file proximity.h
 * Headers for the proximity sensor module
 * <p>
 * Copyright © 2010 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _PROXIMITY_H_
#define _PROXIMITY_H_

/**
 * Paths for the Avago (APDS990x (QPDS-T900)) proximity sensor
 */

/** Device path for the Avago PS */
#define PS_DEVICE_PATH_AVAGO		"/dev/apds990x0"

/** Struct for the Avago data */
struct avago_ps {
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

/** Base path to the Avago proximity sensor */
#define PS_PATH_AVAGO			"/sys/class/misc/apds990x0/device"
/** Path to the first calibration point for the Avago ALS */
/* FIXME: There is no calibration sysfs for Avago */
/* #define PS_CALIB_PATH_AVAGO		PS_PATH_AVAGO "/fixme" */

/** Path to enable/disable Avago proximity sensor */
#define PS_PATH_AVAGO_ENABLE		PS_PATH_AVAGO "/prox_enable"

/** Proximity Sensor status */
#ifndef APDS990X_PS_UPDATED
/** Sensor has up to date data */
#define APDS990X_PS_UPDATED		0x8
#endif /* APDS990X_PS_UPDATED */

/**
 * Paths for the Dipro (BH1770GLC/SFH7770) proximity sensor
 */

/** Device path for the Dipro PS */
#define PS_DEVICE_PATH_DIPRO		"/dev/bh1770glc_ps"

/** Struct for the Dipro data */
struct dipro_ps {
	/** The amount of reflected light from LED 1 */
	guint8 led1;
	/** The amount of reflected light from LED 2 */
	guint8 led2;
	/** The amount of reflected light from LED 3 */
	guint8 led3;
} __attribute__((packed));

/** Base path to the Dipro proximity sensor */
#define PS_PATH_DIPRO			"/sys/class/misc/bh1770glc_ps/device"
/** Path to the first calibration point for the Dipro ALS */
#define PS_CALIB_PATH_DIPRO		PS_PATH_DIPRO "/ps_calib"

/** Hysteresis levels */
typedef struct {
	/** Rising hysteresis threshold */
	guint threshold_rising;
	/** Falling hysteresis threshold */
	guint threshold_falling;
} hysteresis_t;

/** Proximity threshold for the Dipro proximity sensor */
static hysteresis_t dipro_ps_threshold_dipro = {
	/** Rising hysteresis threshold for Dipro */
	.threshold_rising = 80,
	/** Falling hysteresis threshold for Dipro */
	.threshold_falling = 70,
};

/** CAL identifier for the proximity sensor calibration values */
#define PS_CALIB_IDENTIFIER		"ps_calib"

#endif /* _PROXIMITY_H_ */
