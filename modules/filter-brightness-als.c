/**
 * @file filter-brightness-als.c
 * Ambient Light Sensor level adjusting filter module
 * for display backlight, key backlight, and LED brightness
 * This file implements a filter module for MCE
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
#include <glib.h>
#include <gmodule.h>
#include <glib/gstdio.h>		/* g_access */

#include <errno.h>			/* errno */
#include <fcntl.h>			/* O_NONBLOCK */
#include <unistd.h>			/* R_OK */
#include <stdlib.h>			/* free() */
#include <string.h>			/* memcpy() */

#include "mce.h"
#include "filter-brightness-als.h"

#include "mce-io.h"			/* mce_close_file(),
					 * mce_read_chunk_from_file(),
					 * mce_read_number_string_from_file(),
					 * mce_write_string_to_file(),
					 * mce_write_number_string_to_file(),
					 * mce_register_io_monitor_chunk(),
					 * mce_unregister_io_monitor()
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
#include "mce-dbus.h"			/* Direct:
					 * ---
					 * mce_dbus_handler_add(),
					 * dbus_send_message(),
					 * dbus_new_method_reply(),
					 * dbus_new_signal(),
					 * dbus_message_append_args(),
					 * dbus_message_get_no_reply(),
					 * dbus_message_unref(),
					 * DBusMessage,
					 * DBUS_MESSAGE_TYPE_METHOD_CALL,
					 * DBUS_TYPE_INVALID,
					 * dbus_bool_t
					 *
					 * Indirect:
					 * ---
					 * MCE_REQUEST_IF
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

/** Request enabling of ALS; reference counted */
#define MCE_REQ_ALS_ENABLE		"req_als_enable"

/** Request disabling of ALS; reference counted */
#define MCE_REQ_ALS_DISABLE		"req_als_disable"

/** Maximum number of monitored ALS owners */
#define ALS_MAX_MONITORED		16

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
/** Maximum als threshold value */
static gint als_threshold_max = -1;
/** Is there an ALS available? */
static gboolean als_available = TRUE;
/** Filter things through ALS? */
static gboolean als_enabled = TRUE;
/** Pass input through a median filter? */
static gboolean use_median_filter = FALSE;
/** Lux reading from the ALS */
static gint als_lux = -1;
/** Lux cache for delayed brightness stepdown */
static gint delayed_lux = -1;
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
/** Pointer to the hardcoded color profile for the display */
static cpa_profile_struct *display_cpa_profile_static = NULL;
/** Pointer to the loaded color profile for the display */
static cpa_profile_struct *display_cpa_profile_dynamic = NULL;
/** Has colour phase adjustment been enabled */
static gboolean display_cpa_enabled = FALSE;
/** Path to the colour phase adjustment enabling sysfs entry */
static const gchar *display_cpa_enable_path = NULL;
/** Path to the colour phase adjustment coefficients sysfs entry */
static const gchar *display_cpa_coefficients_path = NULL;
/** The current profile id */
static gchar *current_color_profile_id = NULL;

/** Display state */
static display_state_t display_state = MCE_DISPLAY_UNDEF;

/** Median filter */
static median_filter_struct median_filter;

/** ALS poll interval */
static gint als_poll_interval = ALS_DISPLAY_ON_POLL_FREQ;

/** ID for ALS poll timer source */
static guint als_poll_timer_cb_id = 0;

/** Brightness stepdown delay */
static gint brightness_stepdown_delay = ALS_BRIGHTNESS_STEPDOWN_DELAY;

/** ID for brightness stepdown delay timer */
static guint brightness_delay_timer_cb_id = 0;

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
	/** Dipro (BH1770GLC/SFH7770) type ALS */
	ALS_TYPE_DIPRO = 3,
	/** Avago (APDS990x (QPDS-T900)) type ALS */
	ALS_TYPE_AVAGO = 4
} als_type_t;

static void cancel_als_poll_timer(void);
static void cancel_brightness_delay_timer(void);
static void als_iomon_common(gint lux, gboolean no_delay);
static gboolean set_current_color_profile(const gchar* id);
static gboolean save_color_profile_id_into_conf_file(const gchar *id);
static gboolean is_raw_color_profile_valid(const gint *raw_color_profile, gint raw_color_profile_length);

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

/** External reference count for ALS */
static guint als_external_refcount = 0;

/** List of monitored als owners */
static GSList *als_owner_monitor_list = NULL;

/**
 * Get the current color profile
 *
 * @return The pointer to the current color profile or NULL
 */
static const cpa_profile_struct *display_cpa_profile(void)
{
	return display_cpa_profile_dynamic? : display_cpa_profile_static;
}

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
		als_threshold_max = ALS_THRESHOLD_MAX_AVAGO;
		display_als_profiles = display_als_profiles_rm696;
		led_als_profiles = led_als_profiles_rm696;
		use_median_filter = FALSE;

		display_cpa_enable_path = COLOUR_PHASE_ENABLE_PATH;
		display_cpa_coefficients_path = COLOUR_PHASE_COEFFICIENTS_PATH;

		if (g_access(display_cpa_enable_path, W_OK) == 0) {
			display_cpa_profile_static = rm696_phase_profile;
		}
	} else if (g_access(ALS_DEVICE_PATH_DIPRO, R_OK) == 0) {
		als_type = ALS_TYPE_DIPRO;
		als_device_path = ALS_DEVICE_PATH_DIPRO;
		als_calib0_path = ALS_CALIB_PATH_DIPRO;
		als_threshold_range_path = ALS_THRESHOLD_RANGE_PATH_DIPRO;
		als_threshold_max = ALS_THRESHOLD_MAX_DIPRO;
		display_als_profiles = display_als_profiles_rm680;
		led_als_profiles = led_als_profiles_rm680;
		kbd_als_profiles = kbd_als_profiles_rm680;
		use_median_filter = FALSE;

		display_cpa_enable_path = COLOUR_PHASE_ENABLE_PATH;
		display_cpa_coefficients_path = COLOUR_PHASE_COEFFICIENTS_PATH;

		if (g_access(display_cpa_enable_path, W_OK) == 0) {
			display_cpa_profile_static = rm680_phase_profile;
		}
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

	errno = 0;

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
		/* Fall-through */

	case 2:
		memcpy(&calib1, tmp, sizeof (calib1));
		/* Fall-through */

	case 1:
		memcpy(&calib0, tmp, sizeof (calib0));
		break;
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
		*upper = als_threshold_max;
	} else {
		*upper = (profiles[profile].range[i][1] == -1) ? als_threshold_max : profiles[profile].range[i][1];
	}

	return profiles[profile].value[*level];
}

/**
 * Ambient Light Sensor filter for display brightness
 *
 * @param data The un-processed brightness setting (1-5) stored in a pointer
 * @return The processed brightness value (percentage) stored in a pointer
 */
static gpointer display_brightness_filter(gpointer data)
{
	/** Display ALS level */
	static gint display_als_level = -1;
	gint raw = GPOINTER_TO_INT(data) - 1;
	gpointer retval;

	/* If the display is off or in low power mode,
	 * don't update its brightness
	 */
	if ((display_state == MCE_DISPLAY_OFF) ||
	    (display_state == MCE_DISPLAY_LPM_OFF) ||
	    (display_state == MCE_DISPLAY_LPM_ON)) {
		raw = 0;
		goto EXIT;
	}

	/* Safety net */
	if (raw < ALS_PROFILE_MINIMUM)
		raw = ALS_PROFILE_MINIMUM;
	else if (raw > ALS_PROFILE_MAXIMUM)
		raw = ALS_PROFILE_MAXIMUM;

	if ((als_enabled == TRUE) && (display_als_profiles != NULL)) {
		/* Not true percentage,
		 * since this value may be boosted by high brightness mode
		 */
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
	void *tmp = NULL;
	gulong lux;

	if (als_enabled == FALSE)
		goto EXIT;

	if (get_als_type() == ALS_TYPE_AVAGO) {
		struct avago_als *als;
		gssize len = sizeof (struct avago_als);

		if (mce_read_chunk_from_file(als_device_path, &tmp, &len,
					     0) == FALSE) {
			filtered_read = -1;
			goto EXIT;
		}

		if (len != sizeof (struct avago_als)) {
			mce_log(LL_ERR,
				"Short read from `%s'",
				als_device_path);
			filtered_read = -1;
			goto EXIT;
		}

		als = (struct avago_als *)tmp;

		if ((als->status & APDS990X_ALS_SATURATED) != 0) {
			lux = G_MAXINT;
		} else {
			lux = als->lux;
		}
	} else if (get_als_type() == ALS_TYPE_DIPRO) {
		struct dipro_als *als;
		gssize len = sizeof (struct dipro_als);

		if (mce_read_chunk_from_file(als_device_path, &tmp, &len,
					     0) == FALSE) {
			filtered_read = -1;
			goto EXIT;
		}

		if (len != sizeof (struct dipro_als)) {
			mce_log(LL_ERR,
				"Short read from `%s'",
				als_device_path);
			filtered_read = -1;
			goto EXIT;
		}

		als = (struct dipro_als *)tmp;
		lux = als->lux;
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
	g_free(tmp);

	return filtered_read;
}

/**
 * Adjust ALS thresholds if supported
 *
 * @note Call with 0, 0 to unconditionally generate interrupts
 *       Call with -1, -1 to use cached thresholds
 * @param lower Lower threshold;
 *              any reading below this will generate interrupts
 * @param upper Upper threshold;
 *              any reading above this will generate interrupts
 */
static void adjust_als_thresholds(gint lower, gint upper)
{
	static gint cached_lower = -1;
	static gint cached_upper = -1;
	gchar *str;

	/* Only adjust thresholds if there's support for doing so */
	if (als_threshold_range_path == NULL)
		goto EXIT;

	/* Special cases */
	if ((lower > upper) || ((lower == upper) && (lower == 0))) {
		/* If the lower threshold is higher than the upper threshold,
		 * set both to 0 to guarantee that we get a new interupt;
		 * don't cache
		 */
		lower = 0;
		upper = 0;
	} else if ((lower == upper) && (lower == -1)) {
		if (cached_lower == -1) {
			lower = 0;
			upper = 0;
		} else {
			lower = cached_lower;
			upper = cached_upper;
		}
	} else if ((lower == 0) && (upper == als_threshold_max)) {
		/* [0, MAX] is used to disable ALS reads;
		 * do not cache these values
		 */
	} else {
		cached_lower = lower;
		cached_upper = upper;
	}

	str = g_strdup_printf("%d %d", lower, upper);
	mce_write_string_to_file(als_threshold_range_path, str);
	g_free(str);

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

	/* Adjust the colour phase coefficients */
	if (display_cpa_profile() != NULL) {
		gint level = -1;
		gint i;

		for (i = 0; display_cpa_profile()[i].range[0] != -1; i++) {
			if ((als_lux >= display_cpa_profile()[i].range[0]) &&
			    ((als_lux < display_cpa_profile()[i].range[1]) ||
			     (display_cpa_profile()[i].range[1] == -1))) {
				level = i;
				break;
			}
		}

		if (level != -1) {
			mce_write_string_to_file(display_cpa_coefficients_path, display_cpa_profile()[level].coefficients);

			/* If this is the first time we adjust the colour phase
			 * coefficients, enable cpa adjustment
			 */
			if (display_cpa_enabled == FALSE) {
				mce_write_string_to_file(display_cpa_enable_path, "1");
			}
		}
	}

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

	if (als_external_refcount == 0)
		adjust_als_thresholds(lower, upper);

EXIT2:
	status = TRUE;

EXIT:
	if (status == FALSE)
		als_poll_timer_cb_id = 0;

	return status;
}

/**
 * Timer callback for brightness stepdown delay
 *
 * @param data Unused
 * @return Always returns FALSE, this is a one-shot cb
 */
static gboolean brightness_delay_timer_cb(gpointer data)
{
	gboolean status = FALSE;

	(void)data;

	brightness_delay_timer_cb_id = 0;
	/* No delay for lux setting this time, as we already waited. */
	als_iomon_common(delayed_lux, TRUE);

	return status;
}

/**
 * I/O monitor callback for the Ambient Light Sensor
 *
 * @param lux The lux value
 * @param no_delay If TRUE, do not use stepdown delay
 */
static void als_iomon_common(gint lux, gboolean no_delay)
{
	cover_state_t proximity_sensor_state =
				datapipe_get_gint(proximity_sensor_pipe);
	gboolean status = FALSE;
	gint new_lux;
	gint lower;
	gint upper;

	new_lux = als_median_filter_map(lux);

	/* There's no point in readjusting the brightness
	 * if the read failed; also no readjustment is needed
	 * if the read is identical to the old value, unless
	 * we've never set the threshold values before
	 */
	if ((new_lux == -1) ||
	    ((als_lux == new_lux) && (display_brightness_lower != -1)))
		goto EXIT;

	/* Don't readjust the brightness if there's proximity,
	 * to avoid the backlight from changing if the user
	 * inadvertently covers the ALS
	 */
	if (proximity_sensor_state == COVER_CLOSED)
		goto EXIT;

	/* Step-down is delayed */
	if (als_lux > new_lux) {
		if (no_delay == FALSE) {
			/* Setup timer if not already active and store lux
			 * value, then exit
			 */
			if (brightness_delay_timer_cb_id == 0) {
				brightness_delay_timer_cb_id =
					g_timeout_add_seconds(brightness_stepdown_delay,
										  brightness_delay_timer_cb, NULL);
			}
			delayed_lux = lux;
			goto EXIT;
		}
	} else {
		/* Remove delay timer when stepping up */
		cancel_brightness_delay_timer();
	}

	als_lux = new_lux;

	/* Re-filter the brightness */
	(void)execute_datapipe(&display_brightness_pipe, NULL,
			       USE_CACHE, DONT_CACHE_INDATA);
	(void)execute_datapipe(&led_brightness_pipe, NULL,
			       USE_CACHE, DONT_CACHE_INDATA);
	(void)execute_datapipe(&key_backlight_pipe, NULL,
			       USE_CACHE, DONT_CACHE_INDATA);

	/* Adjust the colour phase coefficients */
	if (display_cpa_profile() != NULL) {
		gint level = -1;
		gint i;

		for (i = 0; display_cpa_profile()[i].range[0] != -1; i++) {
			if ((als_lux >= display_cpa_profile()[i].range[0]) &&
			    ((als_lux < display_cpa_profile()[i].range[1]) ||
			     (display_cpa_profile()[i].range[1] == -1))) {
				level = i;
				break;
			}
		}

		if (level != -1) {
			mce_write_string_to_file(display_cpa_coefficients_path, display_cpa_profile()[level].coefficients);

			/* If this is the first time we adjust the colour phase
			 * coefficients, enable cpa adjustment
			 */
			if (display_cpa_enabled == FALSE) {
				mce_write_string_to_file(display_cpa_enable_path, "1");
			}
		}
	}

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

	if (als_external_refcount == 0)
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
 * @return Always returns FALSE to return remaining chunks (if any)
 */
static gboolean als_dipro_iomon_cb(gpointer data, gsize bytes_read)
{
	struct dipro_als *als;

	/* Don't process invalid reads */
	if (bytes_read != sizeof (struct dipro_als)) {
		als_poll_timer_cb_id = 0;
		cancel_als_poll_timer();
		goto EXIT;
	}

	als = data;

	als_iomon_common(als->lux, FALSE);

EXIT:
	return FALSE;
}

/**
 * I/O monitor callback for the Avago Ambient Light Sensor
 *
 * @param data The new data
 * @param bytes_read Unused
 * @return Always returns FALSE to return remaining chunks (if any)
 */
static gboolean als_avago_iomon_cb(gpointer data, gsize bytes_read)
{
	struct avago_als *als;

	/* Don't process invalid reads */
	if (bytes_read != sizeof (struct avago_als)) {
		als_poll_timer_cb_id = 0;
		cancel_als_poll_timer();
		goto EXIT;
	}

	als = data;

	/* The ALS hasn't got anything to offer */
	if ((als->status & APDS990X_ALS_UPDATED) == 0)
		goto EXIT;

	if ((als->status & APDS990X_ALS_SATURATED) != 0) {
		als_iomon_common(G_MAXINT, FALSE);
	} else {
		als_iomon_common(als->lux, FALSE);
	}

EXIT:
	return FALSE;
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
		als_iomon_id = mce_register_io_monitor_chunk(-1, als_device_path, MCE_IO_ERROR_POLICY_WARN, G_IO_IN | G_IO_PRI | G_IO_ERR, FALSE, als_avago_iomon_cb, sizeof (struct avago_als));
		break;

	case ALS_TYPE_DIPRO:
		/* If we already have have an I/O monitor registered,
		 * we can skip this
		 */
		if (als_iomon_id != NULL)
			goto EXIT;

		/* Register ALS I/O monitor */
		als_iomon_id = mce_register_io_monitor_chunk(-1, als_device_path, MCE_IO_ERROR_POLICY_WARN, G_IO_IN | G_IO_PRI | G_IO_ERR, FALSE, als_dipro_iomon_cb, sizeof (struct dipro_als));
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
 * Cancel brightness delay timer
 */
static void cancel_brightness_delay_timer(void)
{
	if (brightness_delay_timer_cb_id != 0) {
		g_source_remove(brightness_delay_timer_cb_id);
		brightness_delay_timer_cb_id = 0;
	}
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
	case MCE_DISPLAY_LPM_OFF:
	case MCE_DISPLAY_LPM_ON:
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
	if (((old_display_state == MCE_DISPLAY_OFF) ||
	     (old_display_state == MCE_DISPLAY_LPM_OFF) ||
	     (old_display_state == MCE_DISPLAY_LPM_ON)) &&
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

		/* Restore threshold values */
		if (als_external_refcount == 0)
			adjust_als_thresholds(-1, -1);
	} else if (((old_display_state == MCE_DISPLAY_ON) ||
	            (old_display_state == MCE_DISPLAY_DIM)) &&
		   ((display_state == MCE_DISPLAY_OFF) ||
		    (display_state == MCE_DISPLAY_LPM_OFF) ||
		    (display_state == MCE_DISPLAY_LPM_ON))) {
		/* Cancel delayed brightness adjustment */
		cancel_brightness_delay_timer();

		/* Set thresholds to not trigger ALS updates */
		if (als_external_refcount == 0)
			adjust_als_thresholds(0, als_threshold_max);
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
 * D-Bus callback used for reference counting ALS enabling;
 * if the requesting process exits, immediately decrease the refcount
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean als_owner_monitor_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;
	const gchar *old_name;
	const gchar *new_name;
	const gchar *service;
	gssize retval;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	/* Extract result */
	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &service,
				  DBUS_TYPE_STRING, &old_name,
				  DBUS_TYPE_STRING, &new_name,
				  DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_ERR,
			"Failed to get argument from %s.%s; %s",
			"org.freedesktop.DBus", "NameOwnerChanged",
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	/* Remove the name monitor for the ALS owner */
	retval = mce_dbus_owner_monitor_remove(old_name, &als_owner_monitor_list);

	if (retval == -1) {
		mce_log(LL_INFO,
			"Failed to remove name owner monitoring for `%s'",
			old_name);
	} else {
		als_external_refcount = retval;

		if (als_external_refcount == 0) {
			if (display_state == MCE_DISPLAY_OFF ||
			    display_state == MCE_DISPLAY_LPM_OFF ||
			    display_state == MCE_DISPLAY_LPM_ON)
				adjust_als_thresholds(0, als_threshold_max);
			else
				adjust_als_thresholds(-1, -1);
		}
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus callback for the ALS enabling method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean als_enable_req_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	const gchar *sender = dbus_message_get_sender(msg);
	gboolean status = FALSE;
	gssize retval;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	if (sender == NULL) {
		mce_log(LL_ERR,
			"Received invalid ALS enable request "
			"(sender == NULL)");
		goto EXIT;
	}

	mce_log(LL_DEBUG,
		"Received ALS enable request from %s",
		sender);

	retval = mce_dbus_owner_monitor_add(sender, als_owner_monitor_dbus_cb, &als_owner_monitor_list, ALS_MAX_MONITORED);

	if (retval == -1) {
		mce_log(LL_INFO,
			"Failed to add name owner monitoring for `%s'",
			sender);
	} else {
		als_external_refcount = retval;

		if (als_external_refcount == 1)
			adjust_als_thresholds(0, 0);
	}

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

EXIT:
	return status;
}

/**
 * D-Bus callback for the ALS disabling method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean als_disable_req_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	const gchar *sender = dbus_message_get_sender(msg);
	gboolean status = FALSE;
	gssize retval;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	if (sender == NULL) {
		mce_log(LL_ERR,
			"Received invalid ALS disable request "
			"(sender == NULL)");
		goto EXIT;
	}

	mce_log(LL_DEBUG,
		"Received ALS disable request from %s",
		sender);

	retval = mce_dbus_owner_monitor_remove(sender,
					       &als_owner_monitor_list);

	if (retval == -1) {
		mce_log(LL_INFO,
			"Failed to remove name owner monitoring for `%s'",
			sender);
	} else {
		als_external_refcount = retval;

		if (als_external_refcount == 0) {
			if (display_state == MCE_DISPLAY_OFF ||
			    display_state == MCE_DISPLAY_LPM_OFF ||
			    display_state == MCE_DISPLAY_LPM_ON)
				adjust_als_thresholds(0, als_threshold_max);
			else
				adjust_als_thresholds(-1, -1);
		}
	}

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

EXIT:
	return status;
}

/**
 * Send the current profile id
 *
 * @param method_call A DBusMessage to reply to
 * @return TRUE on success, FALSE on failure
 */
static gboolean send_current_color_profile(DBusMessage *const method_call)
{
	DBusMessage *msg = NULL;
	gboolean status = FALSE;
	const gchar *id_to_send = current_color_profile_id? : COLOR_PROFILE_ID_HARDCODED;

	msg = dbus_new_method_reply(method_call);

	/* Append the new mode */
	if (dbus_message_append_args(msg,
				     DBUS_TYPE_STRING, &id_to_send,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append reply argument to D-Bus message "
			"for %s.%s", MCE_REQUEST_IF, MCE_COLOR_PROFILE_GET);
		dbus_message_unref(msg);
		goto EXIT;
	}

	/* Send the message */
	status = dbus_send_message(msg);

EXIT:
	return status;
}

/**
 * D-Bus callback for the get color profile method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean color_profile_get_req_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	mce_log(LL_DEBUG, "Received color profile get request");

	/* Try to send a reply that contains the current color profile id */
	if (send_current_color_profile(msg) == FALSE)
		goto EXIT;

	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus callback for the get color profile ids method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean color_profile_ids_get_req_dbus_cb(DBusMessage *const msg)
{
	DBusMessage *reply_msg = NULL;
	GError *error = NULL;
	GKeyFile *color_profiles_conf = NULL;
	gsize color_profiles_ids_count = 0;
	gchar **color_profile_ids = NULL;
	gchar **iter = NULL, **last_ok = NULL;
	gint *raw_color_profile = NULL;
	gsize raw_color_profile_length = 0;
	gchar **strv_to_send = NULL;
	gsize strv_to_send_length = 0;
	const gchar *hardcoded_profile_id = COLOR_PROFILE_ID_HARDCODED;
	gboolean status = FALSE;
	const gchar *display_id = MCE_CONF_DEFAULT_DISPLAY_ID; /* TODO: add logic to choose the display id - see bug 268997*/

	mce_log(LL_DEBUG, "Received list of color profile ids get request");

	if ((color_profiles_conf = mce_conf_read_conf_file(G_STRINGIFY(MCE_COLOR_PROFILES_CONF_FILE))) != NULL) {
		color_profile_ids = g_key_file_get_keys(color_profiles_conf, display_id, &color_profiles_ids_count, &error);

		mce_log(LL_DEBUG, "Conf file has  %d profiles", color_profiles_ids_count);

		for(iter = color_profile_ids, last_ok = color_profile_ids; *iter != NULL; ) {
			mce_log(LL_DEBUG, "Testing profile '%s'", *iter);

			raw_color_profile = mce_conf_get_int_list(display_id, *iter, &raw_color_profile_length, color_profiles_conf);

			if (is_raw_color_profile_valid(raw_color_profile, raw_color_profile_length) == FALSE) {
				mce_log(LL_DEBUG, "Wrong profile '%s' is found; removed from available profiles", *iter);
				g_free(*iter);
				*(iter++) = NULL;
			} else {
				*(last_ok++) = *(iter++);
			}

			g_free(raw_color_profile);
		}

		*last_ok = NULL;

		mce_conf_free_conf_file(color_profiles_conf);

		strv_to_send = color_profile_ids;
		strv_to_send_length = g_strv_length(color_profile_ids);
	}

	if (color_profile_ids == NULL) {
		mce_log(LL_ERR, "Can't read color profiles ids from group '%s' in conf file %s", display_id, G_STRINGIFY(MCE_COLOR_PROFILES_CONF_FILE));
		g_clear_error(&error);
	}

	if ((strv_to_send == NULL) || (strv_to_send_length == 0)) {
		mce_log(LL_WARN, "Only hardcoded profile is available");

		strv_to_send = (gchar **)(&hardcoded_profile_id);
		strv_to_send_length = 1;
	}

	reply_msg = dbus_new_method_reply(msg);

	if (dbus_message_append_args(reply_msg,
				     DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &strv_to_send, strv_to_send_length,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append reply argument to D-Bus message "
			"for %s.%s", MCE_REQUEST_IF, MCE_COLOR_PROFILE_IDS_GET);
		dbus_message_unref(reply_msg);
		goto EXIT;
	}

	status = dbus_send_message(reply_msg);

EXIT:
	g_strfreev(color_profile_ids);
	return status;
}

/**
 * D-Bus callback for the color profile change method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean color_profile_change_req_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	const gchar *id = NULL;
	gboolean status = FALSE;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	mce_log(LL_DEBUG, "Received color profile change request");

	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &id,
				  DBUS_TYPE_INVALID) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_CRIT,
			"Failed to get argument from %s.%s: %s",
			MCE_REQUEST_IF, MCE_COLOR_PROFILE_CHANGE_REQ,
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	if (set_current_color_profile(id) == FALSE)
	{
		mce_log(LL_WARN, "Wrong color profile id '%s' is received; ignored", id);
		goto EXIT;
	}

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

EXIT:
	return status;
}

/**
 * Free memory allocated for the loaded color profile
 */
static void free_dynamic_color_profile(cpa_profile_struct *color_profile)
{
	gint i = 0;

	if (color_profile == NULL)
		return;

	for(i = 0; color_profile[i].coefficients ; ++i) {
		g_free((gchar *)(color_profile[i].coefficients));
	}

	g_free(color_profile);
}

/**
 * Test if color profile read from conf file is valid or not
 *
 * @param raw_color_profile List of ints which represents the profile in conf file
 * @param raw_color_profile_length Length of the list
 * @return TRUE if the profile is valid; FALSE if not
 */
static gboolean is_raw_color_profile_valid(const gint *raw_color_profile, gint raw_color_profile_length)
{
	gboolean status = FALSE;

	if (raw_color_profile == NULL) {
		mce_log(LL_DEBUG, "Can't read the color profile");
		goto EXIT;
	}

	if ((raw_color_profile_length % 12) != 0) {
		mce_log(LL_DEBUG, "Wrong color profile length %d'", raw_color_profile_length);
		goto EXIT;
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * Read requested color profile from conf file
 *
 * @param profile_id Name of the profile in conf file
 * @return Pointer to the color profile on success, NULL on failure
 *          (@ref free_dynamic_color_profile(cpa_profile_struct*) shall be used to free
 *           the memory allocated for the color profile)
 */
static cpa_profile_struct *read_color_profile_from_conf_file(const gchar *profile_id)
{
	GKeyFile *color_profiles_conf = NULL;
	cpa_profile_struct *color_profile = NULL;
	gint *raw_color_profile = NULL;
	gsize raw_color_profile_length = 0;
	gsize color_profile_length = 0;
	gint *iter = NULL;
	gchar **coef_w_p = NULL;
	guint i = 0;
	const gchar *display_id = MCE_CONF_DEFAULT_DISPLAY_ID; /* TODO: add logic to choose the display id - see bug 268997*/

	if ((color_profiles_conf = mce_conf_read_conf_file(G_STRINGIFY(MCE_COLOR_PROFILES_CONF_FILE))) == NULL)
		goto EXIT;

	raw_color_profile = mce_conf_get_int_list(display_id, profile_id, &raw_color_profile_length, color_profiles_conf);

	mce_conf_free_conf_file(color_profiles_conf);

	if (is_raw_color_profile_valid(raw_color_profile, raw_color_profile_length) == FALSE) {
		mce_log(LL_ERR, "Invalid profile '%s' is requested", profile_id);
		goto EXIT;
	}

	color_profile_length = raw_color_profile_length / 12;
	mce_log(LL_DEBUG, "Color profile '%s' has length %d", profile_id, color_profile_length);

	color_profile = g_malloc0((color_profile_length + 1) * sizeof (cpa_profile_struct));

	mce_log(LL_DEBUG, "Color profile data:");
	for (i = 0, iter = raw_color_profile; i < color_profile_length; ++i, iter += 12) {
		color_profile[i].range[0] = iter[0];
		color_profile[i].range[1] = iter[1];
		color_profile[i].range[2] = iter[2];
		coef_w_p = (gchar **)(&color_profile[i].coefficients);
		*coef_w_p = g_strdup_printf(" %d %d %d %d %d %d %d %d %d",
		  iter[3], iter[4], iter[5], iter[6], iter[7], iter[8], iter[9], iter[10], iter[11]);
		mce_log(LL_DEBUG, "%d -> Min: %d, Max: %d, Level: %d;   Coeff: %s", i,
		  color_profile[i].range[0], color_profile[i].range[1],
		  color_profile[i].range[2], color_profile[i].coefficients);
	} 

	color_profile[color_profile_length].range[0] = -1;
	color_profile[color_profile_length].range[1] = -1;
	color_profile[color_profile_length].range[2] = -1;
	coef_w_p = (gchar **)(&color_profile[color_profile_length].coefficients);
	*coef_w_p = NULL;

EXIT:
	g_free(raw_color_profile);
	return color_profile;
}

/**
 * Set the current color profile according to requested
 *
 * @param id Name of requested color profile
 * @return TRUE on success, FALSE on failure
 */
static gboolean set_current_color_profile(const gchar *id)
{
	gboolean status = FALSE;
	cpa_profile_struct *color_profile = NULL;
	gchar *id_dup = NULL;

	if (id == NULL) {
		mce_log(LL_WARN, "Null sent as color profile id; ignored");
		goto EXIT;
	}

	color_profile = read_color_profile_from_conf_file(id);

	if (color_profile == NULL) {
		mce_log(LL_WARN, "The color profile can't be loaded; ignored");
		goto EXIT;
	}

	id_dup = strdup(id);

	if (id_dup == NULL) {
		mce_log(LL_WARN, "Can't allocate memory to store the color profile id; the profile is not changed");
		free_dynamic_color_profile(color_profile);
		color_profile = NULL;
		goto EXIT;
	}

	status = save_color_profile_id_into_conf_file(id_dup);

	if (status == FALSE) {
		mce_log(LL_WARN, "The current color profile id can't be saved into settings file; the profile is not chnaged");
		free_dynamic_color_profile(color_profile);
		color_profile = NULL;
		g_free(id_dup);
		id_dup = NULL;
		goto EXIT;
	}

	g_free(current_color_profile_id);
	current_color_profile_id = id_dup;
	id_dup = NULL;

	free_dynamic_color_profile(display_cpa_profile_dynamic);
	display_cpa_profile_dynamic = color_profile;
	color_profile = NULL;

	display_brightness_lower = -1; /* To force readjustment */
	als_iomon_common(als_lux, TRUE);

EXIT:
	return status;
}

/**
 * Read string value from conf file
 *
 * @param file_name The name of the conf file
 * @param group The name of group in the conf file
 * @param key The name of the key to read in the group in the conf file
 * @return Pointer to allocated string if success; NULL otherwise
 */
static gchar *read_string_from_conf_file(const gchar *file_name, const gchar *group, const gchar *key)
{
	GKeyFile *key_file = NULL;
	gchar *ret = NULL;

	if ((key_file = mce_conf_read_conf_file(file_name)) == NULL)
		goto EXIT;

	ret = mce_conf_get_string(group, key, NULL, key_file);

	mce_conf_free_conf_file(key_file);

EXIT:
	return ret;
}

/**
 * Save the profile id into conf file
 *
 * @param id The profile id to save
 * @return TRUE on success, FALSE on failure
 */
static gboolean save_color_profile_id_into_conf_file(const gchar *id)
{
	/* TODO: Most probably it shall be moved (and reworked for common use) into mce-conf.c */
	GError *error = NULL;
	GKeyFile *keyfileptr = NULL;
	gboolean status = FALSE;
	gchar *keyfile_content = NULL;
	gsize keyfile_content_length = 0;

	if (mce_are_settings_locked() == TRUE) {
		mce_log(LL_WARN,
			"Cannot save current color profile id; backup/restore "
			"or device clear/factory reset pending");
		goto EXIT;
	}

	if ((keyfileptr = g_key_file_new()) == NULL) {
		mce_log(LL_ERR, "No memory to create key file structure");
		goto EXIT;
	}

	if (g_key_file_load_from_file(keyfileptr, G_STRINGIFY(MCE_CURRENT_SETTINGS_CONF_FILE),
				      G_KEY_FILE_NONE, &error) == FALSE) {
		mce_log(LL_DEBUG, "No existing conf file for current settings; new one would be created");
		g_clear_error(&error);
	}

	g_key_file_set_string(keyfileptr, MCE_CONF_COLOR_PROFILE_GROUP, MCE_CONF_CURRENT_PROFILE_ID_KEY, id);
	keyfile_content = g_key_file_to_data(keyfileptr, &keyfile_content_length, &error);
	g_clear_error(&error);

	if (keyfile_content && keyfile_content_length > 0) {
		status = mce_write_string_to_file(G_STRINGIFY(MCE_CURRENT_SETTINGS_CONF_FILE), keyfile_content);
	} else {
		mce_log(LL_ERR, "Can't save file; content was not created somehow!");
	}

	g_free(keyfile_content);
	mce_conf_free_conf_file(keyfileptr);
EXIT:
	return status;
}

/**
 * Read the default color profile id from conf file
 *
 * @return Pointer to allocated string if success; NULL otherwise
 */
static gchar *read_default_color_profile_id_from_conf_file(void)
{
	return read_string_from_conf_file(G_STRINGIFY(MCE_COLOR_PROFILES_CONF_FILE), MCE_CONF_COMMON_GROUP, MCE_CONF_DEFAULT_PROFILE_ID_KEY);
}

/**
 * Read the current color profile id from conf file
 *
 * @return Pointer to allocated string if success; NULL otherwise
 */
static gchar *read_current_color_profile_id_from_conf_file(void)
{
	return read_string_from_conf_file(G_STRINGIFY(MCE_CURRENT_SETTINGS_CONF_FILE), MCE_CONF_COLOR_PROFILE_GROUP, MCE_CONF_CURRENT_PROFILE_ID_KEY);
}

/**
 * Initialization of saveed color profile during boot
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean init_current_color_profile(void)
{
	gchar *tmp = NULL;
	gboolean status = FALSE;

	tmp = read_current_color_profile_id_from_conf_file();

	if (tmp == NULL) {
		tmp = read_default_color_profile_id_from_conf_file();
	}

	if (tmp) {
		status = set_current_color_profile(tmp);
		g_free(tmp);
	}

	return status;
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

	/* req_als_enable */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_REQ_ALS_ENABLE,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 als_enable_req_dbus_cb) == NULL)
		goto EXIT;

	/* req_als_disable */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_REQ_ALS_DISABLE,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 als_disable_req_dbus_cb) == NULL)
		goto EXIT;

	/* get_color_profile */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_COLOR_PROFILE_GET,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 color_profile_get_req_dbus_cb) == NULL)
		goto EXIT;

	/* get_color_profile_ids */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_COLOR_PROFILE_IDS_GET,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 color_profile_ids_get_req_dbus_cb) == NULL)
		goto EXIT;

	/* req_color_profile_change */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_COLOR_PROFILE_CHANGE_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 color_profile_change_req_dbus_cb) == NULL)
		goto EXIT;

	als_external_refcount = 0;

	/* ALS enabled */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_bool(MCE_GCONF_DISPLAY_ALS_ENABLED_PATH,
				 &als_enabled);

	if (mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
				   MCE_GCONF_DISPLAY_ALS_ENABLED_PATH,
				   als_gconf_cb,
				   &als_enabled_gconf_cb_id) == FALSE)
		goto EXIT;

	if (init_current_color_profile() == FALSE) {
		mce_log(LL_WARN, "Can't init dynamic color profile; hardcoded is used instead");
	}

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

	free_dynamic_color_profile(display_cpa_profile_dynamic);
	display_cpa_profile_dynamic = NULL;
	g_free(current_color_profile_id);
	current_color_profile_id = NULL;

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
	cancel_brightness_delay_timer();

	return;
}
