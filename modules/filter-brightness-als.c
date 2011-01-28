/**
 * @file filter-brightness-als.c
 * Ambient Light Sensor level adjusting filter module
 * for display backlight, key backlight, and LED brightness
 * This file implements a filter module for MCE
 * <p>
 * Copyright © 2007-2010 Nokia Corporation and/or its subsidiary(-ies).
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
#include <glib.h>
#include <gmodule.h>
#include <glib/gstdio.h>		/* g_access */

#include <fcntl.h>			/* O_NONBLOCK */
#include <unistd.h>			/* R_OK */
#include <stdlib.h>			/* free() */
#include <string.h>			/* memcpy() */

#include "mce.h"
#include "filter-brightness-als.h"

#include "mce-io.h"			/* mce_read_chunk_from_file(),
					 * mce_read_number_string_from_file()
					 */
#include "mce-lib.h"			/* mce_translate_string_to_int_with_default(),
					 * mce_translation_t
					 */
#include "mce-hal.h"			/* get_sysinfo_value() */
#include "mce-log.h"			/* mce_log(), LL_* */
#include "mce-conf.h"			/* mce_conf_get_int(),
					 * mce_conf_get_string()
					 */
#include "mce-gconf.h"			/* mce_gconf_get_bool(),
					 * mce_gconf_notifier_add(),
					 * gconf_entry_get_key(),
					 * gconf_value_get_bool(),
					 * GConfClient, GConfEntry, GConfValue
					 */
#include "datapipe.h"			/* execute_datapipe(),
					 * append_output_trigger_to_datapipe(),
					 * append_filter_to_datapipe(),
					 * remove_filter_from_datapipe(),
					 * remove_output_trigger_from_datapipe()
					 */
#include "median_filter.h"		/* median_filter_init(),
					 * median_filter_map()
					 */

/** Module name */
#define MODULE_NAME		"filter-brightness-als"

/** Functionality provided by this module */
static const gchar *const provides[] = {
	"display-brightness-filter",
	"led-brightness-filter",
	"key-backlight-brightness-filter",
	NULL
};

/** Functionality that this module enhances */
static const gchar *const enhances[] = {
	"display-brightness",
	"led-brightness",
	"key-backlight-brightness",
	NULL
};

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
	/** Name of the module */
	.name = MODULE_NAME,
	/** Module enhances */
	.enhances = enhances,
	/** Module provides */
	.provides = provides,
	/** Module priority */
	.priority = 100
};

/** GConf callback ID for ALS enabled */
static guint als_enabled_gconf_cb_id = 0;

/** ID for the ALS I/O monitor */
static gconstpointer als_iomon_id = NULL;

/** Path to the ALS device file entry */
static const gchar *als_device_path = NULL;
/** Path to the ALS lux sysfs entry */
static const gchar *als_lux_path = NULL;
/** Path to the first ALS calibration point sysfs entry */
static const gchar *als_calib0_path = NULL;
/** Path to the second ALS calibration point sysfs entry */
static const gchar *als_calib1_path = NULL;
/** Path to the ALS threshold range sysfs entry */
static const gchar *als_threshold_range_path = NULL;
/** Is there an ALS available? */
static gboolean als_available = TRUE;
/** Filter things through ALS? */
static gboolean als_enabled = TRUE;
/** Pass input through a median filter? */
static gboolean use_median_filter = FALSE;
/** Lux reading from the ALS */
static gint als_lux = -1;
/** ALS profiles for the display */
static als_profile_struct *display_als_profiles = NULL;
/** ALS profiles for the LED */
static als_profile_struct *led_als_profiles = NULL;
/** ALS profiles for the keyboard backlight */
static als_profile_struct *kbd_als_profiles = NULL;
/** ALS lower threshold for display brightness */
static gint display_brightness_lower = -1;
/** ALS upper threshold for display brightness */
static gint display_brightness_upper = -1;
/** ALS lower threshold for led brightness */
static gint led_brightness_lower = -1;
/** ALS upper threshold for led brightness */
static gint led_brightness_upper = -1;
/** ALS lower threshold for keyboard backlight */
static gint kbd_brightness_lower = -1;
/** ALS upper threshold for keyboard backlight */
static gint kbd_brightness_upper = -1;

/** Display state */
static display_state_t display_state = MCE_DISPLAY_UNDEF;

/** Median filter */
static median_filter_struct median_filter;

/** ALS poll interval */
static gint als_poll_interval = ALS_DISPLAY_ON_POLL_FREQ;

/** ID for ALS poll timer source */
static guint als_poll_timer_cb_id = 0;

/** FILE * for the ambient_light_sensor */
static FILE *als_fp = NULL;

/** Ambient Light Sensor type */
typedef enum {
	/** ALS type unset */
	ALS_TYPE_UNSET = -1,
	/** No ALS available */
	ALS_TYPE_NONE = 0,
	/** TSL2562 type ALS */
	ALS_TYPE_TSL2562 = 1,
	/** TSL2563 type ALS */
	ALS_TYPE_TSL2563 = 2,
	/** BH1780GLI type ALS */
	ALS_TYPE_BH1780GLI = 3,
	/** Dipro (BH1770GLC/SFH7770) type ALS */
	ALS_TYPE_DIPRO = 4,
	/** Avago (APDS990x (QPDS-T900)) type ALS */
	ALS_TYPE_AVAGO = 5,
} als_type_t;

static void cancel_als_poll_timer(void);

/** Brightness level step policies */
typedef enum {
	/** Policy not set */
	BRIGHTNESS_STEP_POLICY_INVALID = MCE_INVALID_TRANSLATION,
	/** Brightness level step instantly */
	BRIGHTNESS_STEP_DIRECT = 0,
	/** Only step after a blank->unblank cycle (only for step-down) */
	BRIGHTNESS_STEP_UNBLANK = 1,
	/** Default setting when performing brightness level step-down */
	DEFAULT_BRIGHTNESS_STEP_DOWN_POLICY = BRIGHTNESS_STEP_DIRECT
} brightness_step_policy_t;

/** Mapping of brightness level step integer <-> policy string */
static const mce_translation_t brightness_step_policy_translation[] = {
	{
		.number = BRIGHTNESS_STEP_DIRECT,
		.string = "direct",
	}, {
		.number = BRIGHTNESS_STEP_UNBLANK,
		.string = "unblank",
	}, { /* MCE_INVALID_TRANSLATION marks the end of this array */
		.number = MCE_INVALID_TRANSLATION,
		.string = NULL
	}
};

/** Brightness step-down policy */
static brightness_step_policy_t brightness_step_down_policy =
					DEFAULT_BRIGHTNESS_STEP_DOWN_POLICY;

/**
 * GConf callback for ALS settings
 *
 * @param gcc Unused
 * @param id Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data Unused
 */
static void als_gconf_cb(GConfClient *const gcc, const guint id,
			 GConfEntry *const entry, gpointer const data)
{
	const GConfValue *gcv = gconf_entry_get_value(entry);

	(void)gcc;
	(void)data;

	/* Key is unset */
	if (gcv == NULL) {
		mce_log(LL_DEBUG,
			"GConf Key `%s' has been unset",
			gconf_entry_get_key(entry));
		goto EXIT;
	}

	if (id == als_enabled_gconf_cb_id) {
		gint tmp = gconf_value_get_bool(gcv);

		/* Only care about the setting if there's an ALS available */
		if (als_available == TRUE)
			als_enabled = tmp;
	} else {
		mce_log(LL_WARN,
			"Spurious GConf value received; confused!");
	}

EXIT:
	return;
}

/**
 * Get the ALS type
 *
 * @return The ALS-type
 */
static als_type_t get_als_type(void)
{
	static als_type_t als_type = ALS_TYPE_UNSET;

	/* If we have the ALS-type already, return it */
	if (als_type != ALS_TYPE_UNSET)
		goto EXIT;

	if (g_access(ALS_DEVICE_PATH_AVAGO, R_OK) == 0) {
		als_type = ALS_TYPE_AVAGO;
		als_device_path = ALS_DEVICE_PATH_AVAGO;
		als_calib0_path = ALS_CALIB_PATH_AVAGO;
		als_threshold_range_path = ALS_THRESHOLD_RANGE_PATH_AVAGO;
		display_als_profiles = display_als_profiles_rm696;
		led_als_profiles = led_als_profiles_rm696;
		use_median_filter = FALSE;
	} else if (g_access(ALS_DEVICE_PATH_DIPRO, R_OK) == 0) {
		als_type = ALS_TYPE_DIPRO;
		als_device_path = ALS_DEVICE_PATH_DIPRO;
		als_calib0_path = ALS_CALIB_PATH_DIPRO;
		als_threshold_range_path = ALS_THRESHOLD_RANGE_PATH_DIPRO;
		display_als_profiles = display_als_profiles_rm680;
		led_als_profiles = led_als_profiles_rm680;
		kbd_als_profiles = kbd_als_profiles_rm680;
		use_median_filter = FALSE;
	} else if (g_access(ALS_LUX_PATH_TSL2563, R_OK) == 0) {
		als_type = ALS_TYPE_TSL2563;
		als_lux_path = ALS_LUX_PATH_TSL2563;
		als_calib0_path = ALS_CALIB0_PATH_TSL2563;
		als_calib1_path = ALS_CALIB1_PATH_TSL2563;
		display_als_profiles = display_als_profiles_rx51;
		led_als_profiles = led_als_profiles_rx51;
		kbd_als_profiles = kbd_als_profiles_rx51;
		use_median_filter = TRUE;
	} else if (g_access(ALS_LUX_PATH_TSL2562, R_OK) == 0) {
		als_type = ALS_TYPE_TSL2562;
		als_lux_path = ALS_LUX_PATH_TSL2562;
		als_calib0_path = ALS_CALIB0_PATH_TSL2562;
		als_calib1_path = ALS_CALIB1_PATH_TSL2562;
		display_als_profiles = display_als_profiles_rx44;
		led_als_profiles = led_als_profiles_rx44;
		kbd_als_profiles = kbd_als_profiles_rx44;
		use_median_filter = TRUE;
	} else {
		als_type = ALS_TYPE_NONE;
	}

	/* If the range path doesn't exist, disable it */
	if (als_threshold_range_path != NULL) {
		if (g_access(als_threshold_range_path, W_OK) == -1)
			als_threshold_range_path = NULL;
	}

	mce_log(LL_DEBUG, "ALS-type: %d", als_type);

EXIT:
	return als_type;
}

/**
 * Calibrate the ALS using calibration values from CAL
 */
static void calibrate_als(void)
{
	guint32 calib0 = 0;
	guint32 calib1 = 0;
	guint8 *tmp = NULL;
	gsize count;
	gulong len;

	/* If we don't have any calibration points, don't bother */
	if ((als_calib0_path == NULL) && (als_calib1_path == NULL))
		goto EXIT;

	/* Retrieve the calibration data from sysinfo */
	if (get_sysinfo_value(ALS_CALIB_IDENTIFIER, &tmp, &len) == FALSE) {
		mce_log(LL_ERR,
			"Failed to retrieve calibration data");
		goto EXIT;
	}

	/* Is the memory properly aligned? */
	if ((len % sizeof (guint32)) != 0) {
		mce_log(LL_ERR,
			"Invalid calibration data returned");
		goto EXIT2;
	}

	count = len / sizeof (guint32);

	/* We don't have any calibration data */
	if (count == 0) {
		mce_log(LL_INFO,
			"No calibration data available");
		goto EXIT2;
	}

	switch (count) {
	default:
		mce_log(LL_INFO,
			"Ignored excess calibration data");

	case 2:
		memcpy(&calib1, tmp, sizeof (calib1));

	case 1:
		memcpy(&calib0, tmp, sizeof (calib0));
	}

	/* Write calibration value 0 */
	if (als_calib0_path != NULL) {
		mce_write_number_string_to_file(als_calib0_path,
						calib0, NULL, TRUE, TRUE);
	}

	/* Write calibration value 1 */
	if ((als_calib1_path != NULL) && (count > 1)) {
		mce_write_number_string_to_file(als_calib1_path,
						calib1, NULL, TRUE, TRUE);
	}

EXIT2:
	free(tmp);

EXIT:
	return;
}

/**
 * Use the ALS profiles to calculate proper ALS modified values;
 * also reprogram the sensor thresholds if the sensor supports such
 *
 * @param profiles The profile struct to use for calculations
 * @param profile The profile to use
 * @param lux The lux value
 * @param[in,out] level The old level; will be replaced by the new level
 * @param[out] lower The new lower ALS interrupt threshold
 * @param[out] upper The new upper ALS interrupt threshold
 * @return The brightness in % of maximum
 */
static gint filter_data(als_profile_struct *profiles, als_profile_t profile,
			gint lux, gint *level, gint *lower, gint *upper)
{
	gint tmp = *level;
	gint i;

	if (tmp == -1)
		tmp = 0;
	else if (tmp > ALS_RANGES)
		tmp = ALS_RANGES;

	for (i = 0; i < ALS_RANGES; i++) {
		*level = i;

		if (profiles[profile].range[i][0] == -1)
			break;

		if (lux < profiles[profile].range[i][(((i + 1) - tmp) > 0) ? 1 : 0])
			break;
	}

	*lower = (i == 0) ? 0 : profiles[profile].range[i - 1][0];

	if (i >= ALS_RANGES) {
		/* This is a programming error! */
		mce_log(LL_CRIT,
			"The ALS profile %d lacks terminating { -1, -1 }",
			profile);
		*upper = 65535;
	} else {
		*upper = (profiles[profile].range[i][1] == -1) ? 65535 : profiles[profile].range[i][1];
	}

	return profiles[profile].value[*level];
}

/**
 * Ambient Light Sensor filter for display brightness
 *
 * @param data The un-processed brightness setting (1-5) stored in a pointer
 * @return The processed brightness value (percentage)
 */
static gpointer display_brightness_filter(gpointer data)
{
	/** Display ALS level */
	static gint display_als_level = -1;
	gint raw = GPOINTER_TO_INT(data) - 1;
	gpointer retval;

	/* If the display is off, don't update its brightness */
	if (display_state == MCE_DISPLAY_OFF) {
		raw = 0;
		goto EXIT;
	}

	/* Safety net */
	if (raw < ALS_PROFILE_MINIMUM)
		raw = ALS_PROFILE_MINIMUM;
	else if (raw > ALS_PROFILE_MAXIMUM)
		raw = ALS_PROFILE_MAXIMUM;

	if ((als_enabled == TRUE) && (display_als_profiles != NULL)) {
		gint percentage = filter_data(display_als_profiles, raw,
					      als_lux, &display_als_level,
					      &display_brightness_lower,
					      &display_brightness_upper);

		raw = percentage;
	} else {
		raw = (raw + 1) * 20;
	}

EXIT:
	retval = GINT_TO_POINTER(raw);

	return retval;
}

/**
 * Ambient Light Sensor filter for LED brightness
 *
 * @param data The un-processed brightness setting (1-5) stored in a pointer
 * @return The processed brightness value
 */
static gpointer led_brightness_filter(gpointer data)
{
	/** LED ALS level */
	static gint led_als_level = -1;
	gint brightness;

	if ((als_enabled == TRUE) && (led_als_profiles != NULL)) {
		/* XXX: this always uses the NORMAL profile */
		gint percentage = filter_data(led_als_profiles,
					      ALS_PROFILE_NORMAL,
					      als_lux, &led_als_level,
					      &led_brightness_lower,
					      &led_brightness_upper);
		brightness = (GPOINTER_TO_INT(data) * percentage) / 100;
	} else {
		brightness = GPOINTER_TO_INT(data);
	}

	return GINT_TO_POINTER(brightness);
}

/**
 * Ambient Light Sensor filter for keyboard backlight brightness
 *
 * @param data The un-processed brightness setting (1-5) stored in a pointer
 * @return The processed brightness value
 */
static gpointer key_backlight_filter(gpointer data)
{
	/** Keyboard ALS level */
	static gint kbd_als_level = -1;
	gint brightness = 0;

	if ((als_enabled == TRUE) && (kbd_als_profiles != NULL)) {
		/* XXX: this always uses the NORMAL profile */
		gint percentage = filter_data(kbd_als_profiles,
					      ALS_PROFILE_NORMAL,
					      als_lux, &kbd_als_level,
					      &kbd_brightness_lower,
					      &kbd_brightness_upper);
		brightness = (GPOINTER_TO_INT(data) * percentage) / 100;
	} else {
		brightness = GPOINTER_TO_INT(data);
	}

	return GINT_TO_POINTER(brightness);
}

/**
 * Wrapper function for median_filter_init()
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean als_median_filter_init(void)
{
	gboolean status = TRUE;

	if (use_median_filter == FALSE)
		goto EXIT;

	/* Re-initialise the median filter */
	if (median_filter_init(&median_filter,
			       MEDIAN_FILTER_WINDOW_SIZE) == FALSE) {
		mce_log(LL_CRIT, "median_filter_init() failed");
		als_enabled = FALSE;
		status = FALSE;
	}

EXIT:
	return status;
}

/**
 * Wrapper function for median_filter_map()
 *
 * @param value The value to insert
 * @return The filtered value
 */
static gint als_median_filter_map(gint value)
{
	return (use_median_filter == TRUE) ?
		median_filter_map(&median_filter, value) : value;
}

/**
 * Read a value from the ALS and update the median filter
 *
 * @return the filtered result of the read,
 *         -1 on failure,
 *         -2 if the ALS is disabled
 */
static gint als_read_value_filtered(void)
{
	gint filtered_read = -2;
	gulong lux;

	if (als_enabled == FALSE)
		goto EXIT;

	if (get_als_type() == ALS_TYPE_AVAGO) {
		struct avago_als *als;
		void *tmp = NULL;
		gssize len = sizeof (struct avago_als);

		if (mce_read_chunk_from_file(als_device_path, &tmp, &len,
					     0, -1) == FALSE) {
			filtered_read = -1;
			goto EXIT;
		}

		if (len != sizeof (struct avago_als)) {
			mce_log(LL_ERR,
				"Short read from `%s'",
				als_device_path);
			g_free(tmp);
			filtered_read = -1;
			goto EXIT;
		}

		als = (struct avago_als *)tmp;
		lux = als->lux;

		g_free(tmp);
	} else if (get_als_type() == ALS_TYPE_DIPRO) {
		struct dipro_als *als;
		void *tmp = NULL;
		gssize len = sizeof (struct dipro_als);

		if (mce_read_chunk_from_file(als_device_path, &tmp, &len,
					     0, -1) == FALSE) {
			filtered_read = -1;
			goto EXIT;
		}

		if (len != sizeof (struct dipro_als)) {
			mce_log(LL_ERR,
				"Short read from `%s'",
				als_device_path);
			g_free(tmp);
			filtered_read = -1;
			goto EXIT;
		}

		als = (struct dipro_als *)tmp;
		lux = als->lux;

		g_free(tmp);
	} else {
		/* Read lux value from ALS */
		if (mce_read_number_string_from_file(als_lux_path,
						     &lux, &als_fp,
						     TRUE, FALSE) == FALSE) {
			filtered_read = -1;
			goto EXIT;
		}
	}

	filtered_read = als_median_filter_map(lux);

EXIT:
	return filtered_read;
}

/**
 * Adjust ALS thresholds if supported
 *
 * @param lower Lower threshold;
 *              any reading below this will generate interrupts
 * @param upper Upper threshold;
 *              any reading above this will generate interrupts
 */
static void adjust_als_thresholds(gint lower, gint upper)
{
	/* Only adjust thresholds if there's support for doing so */
	if (als_threshold_range_path == NULL)
		goto EXIT;

	/* If the lower threshold is higher than the upper threshold,
	 * set both to 0 to guarantee that we get a new interupt
	 */
	if (lower >= upper) {
		lower = 0;
		upper = 0;
	}

	/* Only write to the threshold registers
	 * if we are monitoring the ALS
	 */
	if ((als_poll_timer_cb_id != 0) || (als_iomon_id != NULL)) {
		gchar *str = g_strdup_printf("%d %d", lower, upper);
		mce_write_string_to_file(als_threshold_range_path, str);
		g_free(str);
	}

EXIT:
	return;
}

/**
 * Timer callback for polling of the Ambient Light Sensor
 *
 * @param data Unused
 * @return Always returns TRUE, for continuous polling,
           unless the ALS is disabled
 */
static gboolean als_poll_timer_cb(gpointer data)
{
	gboolean status = FALSE;
	gint new_lux;
	gint lower;
	gint upper;

	(void)data;

	/* Read lux value from ALS */
	if ((new_lux = als_read_value_filtered()) == -2)
		goto EXIT;

	/* There's no point in readjusting the brightness
	 * if the read failed; also no readjustment is needed
	 * if the read is identical to the old value, unless
	 * we've never set the threshold values before
	 */
	if ((new_lux == -1) ||
	    ((als_lux == new_lux) && (display_brightness_lower != -1)))
		goto EXIT2;

	als_lux = new_lux;

	/* Re-filter the brightness */
	(void)execute_datapipe(&display_brightness_pipe, NULL,
			       USE_CACHE, DONT_CACHE_INDATA);
	(void)execute_datapipe(&led_brightness_pipe, NULL,
			       USE_CACHE, DONT_CACHE_INDATA);
	(void)execute_datapipe(&key_backlight_pipe, NULL,
			       USE_CACHE, DONT_CACHE_INDATA);

	/* The lower threshold is the largest of the lower thresholds */
	lower = display_brightness_lower;

	if (led_als_profiles != NULL)
		lower = MAX(lower, led_brightness_lower);

	if (kbd_als_profiles != NULL)
		lower = MAX(lower, kbd_brightness_lower);

	/* The upper threshold is the smallest of the upper thresholds */
	upper = display_brightness_upper;

	if (led_als_profiles != NULL)
		upper = MIN(upper, led_brightness_upper);

	if (kbd_als_profiles != NULL)
		upper = MIN(upper, kbd_brightness_upper);

	adjust_als_thresholds(lower, upper);

EXIT2:
	status = TRUE;

EXIT:
	if (status == FALSE)
		als_poll_timer_cb_id = 0;

	return status;
}

/**
 * I/O monitor callback for the Ambient Light Sensor
 *
 * @param lux The lux value
 */
static void als_iomon_common(gint lux)
{
	gboolean status = FALSE;
	gint new_lux;
	gint lower;
	gint upper;

	new_lux = als_median_filter_map(lux);

	/* No readjustment is needed if the read is identical
	 * to the old value, unless we've never set the threshold
	 * values before
	 */
	if ((als_lux == new_lux) && (display_brightness_lower != -1))
		goto EXIT;

	als_lux = new_lux;

	/* Re-filter the brightness */
	(void)execute_datapipe(&display_brightness_pipe, NULL,
			       USE_CACHE, DONT_CACHE_INDATA);
	(void)execute_datapipe(&led_brightness_pipe, NULL,
			       USE_CACHE, DONT_CACHE_INDATA);
	(void)execute_datapipe(&key_backlight_pipe, NULL,
			       USE_CACHE, DONT_CACHE_INDATA);

	/* The lower threshold is the largest of the lower thresholds */
	lower = display_brightness_lower;

	if (led_als_profiles != NULL)
		lower = MAX(lower, led_brightness_lower);

	if (kbd_als_profiles != NULL)
		lower = MAX(lower, kbd_brightness_lower);

	/* The upper threshold is the smallest of the upper thresholds */
	upper = display_brightness_upper;

	if (led_als_profiles != NULL)
		upper = MIN(upper, led_brightness_upper);

	if (kbd_als_profiles != NULL)
		upper = MIN(upper, kbd_brightness_upper);

	adjust_als_thresholds(lower, upper);

EXIT:
	status = TRUE;

	return;
}


/**
 * I/O monitor callback for the Dipro Ambient Light Sensor
 *
 * @param data The new data
 * @param bytes_read Unused
 */
static void als_iomon_dipro_cb(gpointer data, gsize bytes_read)
{
	struct dipro_als *als;
	als = data;

	/* Don't process invalid reads */
	if (bytes_read != sizeof (struct dipro_als)) {
		als_poll_timer_cb_id = 0;
		cancel_als_poll_timer();
		goto EXIT;
	}

	als_iomon_common(als->lux);

EXIT:
	return;
}

/**
 * I/O monitor callback for the Avago Ambient Light Sensor
 *
 * @param data The new data
 * @param bytes_read Unused
 */
static void als_iomon_avago_cb(gpointer data, gsize bytes_read)
{
	struct avago_als *als;
	als = data;

	/* Don't process invalid reads */
	if (bytes_read != sizeof (struct avago_als)) {
		als_poll_timer_cb_id = 0;
		cancel_als_poll_timer();
		goto EXIT;
	}

	/* The ALS hasn't got anything to offer */
	if ((als->status & APDS990X_ALS_UPDATED) == 0)
		goto EXIT;

	if ((als->status & APDS990X_ALS_SATURATED) != 0) {
		als_iomon_common(G_MAXUINT);
	} else {
		als_iomon_common(als->lux);
	}

EXIT:
	return;
}


/**
 * Cancel Ambient Light Sensor poll timer
 */
static void cancel_als_poll_timer(void)
{
	/* Unregister ALS I/O monitor */
	if (als_iomon_id != NULL) {
		mce_unregister_io_monitor(als_iomon_id);
		als_iomon_id = NULL;
	}

	/* Disable old ALS timer */
	if (als_poll_timer_cb_id != 0) {
		g_source_remove(als_poll_timer_cb_id);
		als_poll_timer_cb_id = 0;
	}
}

/**
 * Setup Ambient Light Sensor poll timer
 */
static void setup_als_poll_timer(void)
{
	/* If we don't want polling to take place, disable it */
	if (als_poll_interval == 0) {
		cancel_als_poll_timer();

		/* Close the file pointer when we disable the als polling
		 * to ensure that the ALS can sleep
		 */
		(void)mce_close_file(als_lux_path, &als_fp);
		goto EXIT;
	}

	switch (get_als_type()) {
	case ALS_TYPE_AVAGO:
		/* If we already have have an I/O monitor registered,
		 * we can skip this
		 */
		if (als_iomon_id != NULL)
			goto EXIT;

		/* Register ALS I/O monitor */
		als_iomon_id = mce_register_io_monitor_chunk(-1, als_device_path, MCE_IO_ERROR_POLICY_WARN, G_IO_IN | G_IO_PRI | G_IO_ERR, FALSE, als_iomon_avago_cb, sizeof (struct avago_als));
		break;

	case ALS_TYPE_DIPRO:
		/* If we already have have an I/O monitor registered,
		 * we can skip this
		 */
		if (als_iomon_id != NULL)
			goto EXIT;

		/* Register ALS I/O monitor */
		als_iomon_id = mce_register_io_monitor_chunk(-1, als_device_path, MCE_IO_ERROR_POLICY_WARN, G_IO_IN | G_IO_PRI | G_IO_ERR, FALSE, als_iomon_dipro_cb, sizeof (struct dipro_als));
		break;

	default:
		/* Setup new timer;
		 * for light sensors that we don't use polling for
		 */
		cancel_als_poll_timer();
		als_poll_timer_cb_id = g_timeout_add(als_poll_interval,
						     als_poll_timer_cb, NULL);
		break;
	}

EXIT:
	return;
}

/**
 * Handle display state change
 *
 * @param data The display stated stored in a pointer
 */
static void display_state_trigger(gconstpointer data)
{
	static display_state_t old_display_state = MCE_DISPLAY_UNDEF;
	gint old_als_poll_interval = als_poll_interval;
	display_state = GPOINTER_TO_INT(data);

	if (als_enabled == FALSE)
		goto EXIT;

	old_als_poll_interval = als_poll_interval;

	/* Update poll timeout */
	switch (display_state) {
	case MCE_DISPLAY_OFF:
		als_poll_interval = ALS_DISPLAY_OFF_POLL_FREQ;
		break;

	case MCE_DISPLAY_DIM:
		als_poll_interval = ALS_DISPLAY_DIM_POLL_FREQ;
		break;

	case MCE_DISPLAY_UNDEF:
	case MCE_DISPLAY_ON:
	default:
		als_poll_interval = ALS_DISPLAY_ON_POLL_FREQ;
		break;
	}

	/* Re-fill the median filter */
	if ((old_display_state == MCE_DISPLAY_OFF) &&
	    ((display_state == MCE_DISPLAY_ON) ||
	     (display_state == MCE_DISPLAY_DIM))) {
		gint new_lux;

		cancel_als_poll_timer();

#ifdef ALS_DISPLAY_OFF_FLUSH_FILTER
		/* Re-initialise the median filter */
		if (als_median_filter_init() == FALSE)
			goto EXIT;
#endif /* ALS_DISPLAY_OFF_FLUSH_FILTER */

		/* Read lux value from ALS */
		new_lux = als_read_value_filtered();

		/* There's no point in readjusting the brightness
		 * if the ambient light did not change,
		 * unless we use the unblank policy for step-downs
		 */
		if ((new_lux >= 0) &&
		    ((als_lux != new_lux) ||
		     (brightness_step_down_policy ==
		      BRIGHTNESS_STEP_UNBLANK))) {
			/* Re-filter the brightness */
			(void)execute_datapipe(&display_brightness_pipe, NULL, USE_CACHE, DONT_CACHE_INDATA);
			(void)execute_datapipe(&led_brightness_pipe, NULL, USE_CACHE, DONT_CACHE_INDATA);
			(void)execute_datapipe(&key_backlight_pipe, NULL, USE_CACHE, DONT_CACHE_INDATA);
		}
	}

	/* Reprogram timer, if needed */
	if ((als_poll_interval != old_als_poll_interval) ||
	    ((als_poll_timer_cb_id == 0) && (als_iomon_id == NULL)))
		setup_als_poll_timer();

EXIT:
	old_display_state = display_state;

	return;
}

/**
 * Init function for the ALS filter
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
	gchar *str = NULL;

	(void)module;

	/* Append triggers/filters to datapipes */
	append_filter_to_datapipe(&display_brightness_pipe,
				  display_brightness_filter);
	append_filter_to_datapipe(&led_brightness_pipe,
				  led_brightness_filter);
	append_filter_to_datapipe(&key_backlight_pipe,
				  key_backlight_filter);
	append_output_trigger_to_datapipe(&display_state_pipe,
					  display_state_trigger);

	/* ALS enabled */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_bool(MCE_GCONF_DISPLAY_ALS_ENABLED_PATH,
				 &als_enabled);

	if (mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
				   MCE_GCONF_DISPLAY_ALS_ENABLED_PATH,
				   als_gconf_cb,
				   &als_enabled_gconf_cb_id) == FALSE)
		goto EXIT;

	/* Do we have an ALS at all?
	 * If so, make an initial read
	 */
	if (get_als_type() != ALS_TYPE_NONE) {
		/* Initialise the median filter */
		if (als_median_filter_init() == FALSE) {
			goto EXIT;
		}

		/* Calibrate the ALS */
		calibrate_als();

		/* Initial read of lux value from ALS */
		if ((als_lux = als_read_value_filtered()) >= 0) {
			/* Set initial polling interval */
			als_poll_interval = ALS_DISPLAY_ON_POLL_FREQ;

			/* Setup ALS polling */
			setup_als_poll_timer();
		} else {
			/* Reading from the ALS failed */
			als_lux = -1;
			als_available = FALSE;
			als_enabled = FALSE;
		}
	} else {
		/* We don't have an ALS */
		als_lux = -1;
		als_available = FALSE;
		als_enabled = FALSE;
	}

	/* Re-filter the brightness if we got an ALS-reading */
	if (als_lux != -1) {
		(void)execute_datapipe(&display_brightness_pipe, NULL,
				       USE_CACHE, DONT_CACHE_INDATA);
		(void)execute_datapipe(&led_brightness_pipe, NULL,
				       USE_CACHE, DONT_CACHE_INDATA);
		(void)execute_datapipe(&key_backlight_pipe, NULL,
				       USE_CACHE, DONT_CACHE_INDATA);
	}

	/* Get configuration options */
	str = mce_conf_get_string(MCE_CONF_ALS_GROUP,
				  MCE_CONF_STEP_DOWN_POLICY,
				  "",
				  NULL);

	brightness_step_down_policy = mce_translate_string_to_int_with_default(brightness_step_policy_translation, str, DEFAULT_BRIGHTNESS_STEP_DOWN_POLICY);
	g_free(str);

EXIT:
	return NULL;
}

/**
 * Exit function for the ALS filter
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	als_enabled = FALSE;

	/* Close the ALS file pointer */
	(void)mce_close_file(als_lux_path, &als_fp);

	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&display_state_pipe,
					    display_state_trigger);
	remove_filter_from_datapipe(&key_backlight_pipe,
				    key_backlight_filter);
	remove_filter_from_datapipe(&led_brightness_pipe,
				    led_brightness_filter);
	remove_filter_from_datapipe(&display_brightness_pipe,
				    display_brightness_filter);

	/* Remove all timer sources */
	cancel_als_poll_timer();

	return;
}
