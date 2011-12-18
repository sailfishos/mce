/**
 * @file led.c
 * LED module -- this handles the LED logic for MCE
 * <p>
 * Copyright © 2006-2011 Nokia Corporation and/or its subsidiary(-ies).
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
#include <glib.h>
#include <gmodule.h>

#include <errno.h>                      /* errno, EINVAL, ERANGE */
#include <fcntl.h>			/* open(), O_RDWR, O_CREAT */
#include <stdlib.h>			/* strtoul() */
#include <string.h>			/* strcmp(), strcpy(), strdup() */
#include <unistd.h>			/* close(), W_OK */
#include <sys/ioctl.h>			/* ioctl() */
#include <libi2c/i2c-dev.h>	/* i2c_smbus_write_byte_data(),
					 * I2C_SLAVE_FORCE
					 */

#include "mce.h"
#include "led.h"

#include "mce-io.h"			/* mce_close_file(),
					 * mce_write_string_to_file(),
					 * mce_write_number_string_to_file()
					 */
#include "mce-hal.h"			/* get_product_id(),
					 * product_id_t
					 */
#include "mce-lib.h"			/* bin_to_string() */
#include "mce-log.h"			/* mce_log(), LL_* */
#include "mce-conf.h"			/* mce_conf_get_string_list() */
#include "mce-dbus.h"			/* Direct:
					 * ---
					 * mce_dbus_handler_add(),
					 * dbus_send_message(),
					 * dbus_new_method_reply(),
					 * dbus_message_get_no_reply(),
					 * dbus_message_get_args(),
					 * dbus_error_init(),
					 * dbus_error_free(),
					 * DBUS_MESSAGE_TYPE_METHOD_CALL,
					 * DBUS_TYPE_STRING,
					 * DBUS_TYPE_INVALID,
					 * DBusMessage, DBusError,
					 * dbus_bool_t
					 *
					 * Indirect:
					 * ---
					 * MCE_REQUEST_IF,
					 * MCE_ACTIVATE_LED_PATTERN,
					 * MCE_DEACTIVATE_LED_PATTERN,
					 * MCE_ENABLE_LED,
					 * MCE_DISABLE_LED
					 */
#include "mce-gconf.h"			/* mce_gconf_notifier_add(),
					 * mce_gconf_notifier_remove(),
					 * mce_gconf_get_bool(),
					 * gconf_entry_get_key(),
					 * gconf_entry_get_value(),
					 * gconf_value_get_bool(),
					 * gconf_concat_dir_and_key(),
					 * GConfClient, GConfEntry, GConfValue
					 */
#include "datapipe.h"			/* execute_datapipe(),
					 * datapipe_get_gint(),
					 * append_output_trigger_to_datapipe(),
					 * remove_output_trigger_from_datapipe()
					 */

/** Module name */
#define MODULE_NAME		"led"

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
	/** Name of the module */
	.name = MODULE_NAME,
	/** Module provides */
	.provides = provides,
	/** Module priority */
	.priority = 100
};

/** The pattern queue */
static GQueue *pattern_stack = NULL;
/** The pattern combination rule queue */
static GQueue *combination_rule_list = NULL;
/** The pattern combination rule queue */
static GQueue *combination_rule_xref_list = NULL;
/** The D-Bus controlled LED switch */
static gboolean led_enabled = FALSE;

/** Fields in the patterns */
typedef enum {
	/** Pattern priority field */
	PATTERN_PRIO_FIELD = 0,
	/** Pattern screen display policy field */
	PATTERN_SCREEN_ON_FIELD = 1,
	/** Pattern timeout field */
	PATTERN_TIMEOUT_FIELD = 2,
	/** On-period field for direct-controlled monochrome patterns */
	PATTERN_ON_PERIOD_FIELD = 3,
	/** R-channel pattern field for NJoy-controlled RGB patterns */
	PATTERN_R_CHANNEL_FIELD = 3,
	/** LED-muxing field for Lysti-controlled RGB patterns */
	PATTERN_MUXING_FIELD = 3,
	/**
	 * Engine channel field for Lysti-controlled monochrome patterns
	 * and NJoy-controlled monochrome patterns
	 */
	PATTERN_E_CHANNEL_FIELD = 3,
	/** Number of fields used by Lysti-controlled monochrome patterns */
	NUMBER_OF_PATTERN_FIELDS_LYSTI_MONO = 4,
	/** Number of fields used by NJoy-controlled monochrome patterns */
	NUMBER_OF_PATTERN_FIELDS_NJOY_MONO = 4,
	/** Off-period field for direct-controlled monochrome patterns */
	PATTERN_OFF_PERIOD_FIELD = 4,
	/** G-channel pattern field for NJoy-controlled RGB patterns */
	PATTERN_G_CHANNEL_FIELD = 4,
	/** Engine channel 1 field for Lysti-controlled RGB patterns */
	PATTERN_E1_CHANNEL_FIELD = 4,
	/** Pattern brightness field for direct-controlled monochrome patterns */
	PATTERN_BRIGHTNESS_FIELD = 5,
	/** B-channel pattern field for NJoy-controlled RGB patterns */
	PATTERN_B_CHANNEL_FIELD = 5,
	/** Engine channel 2 field for Lysti-controlled RGB patterns */
	PATTERN_E2_CHANNEL_FIELD = 5,
	/**
	 * Number of fields used by Lysti-controlled RGB patterns,
	 * NJoy-controlled RGB patterns,
	 * and monochrome direct-controlled patterns
	 */
	NUMBER_OF_PATTERN_FIELDS = 6
} pattern_field;

/**
 * Size of each LED channel
 *
 * Multiply the channel size by 2 since we store hexadecimal ASCII
 */
#define CHANNEL_SIZE		32 * 2

/** Structure holding LED patterns */
typedef struct {
	gchar *name;			/**< Pattern name */
	gint priority;			/**< Pattern priority */
	gint policy;			/**< Show pattern when screen is on? */
	gint timeout;			/**< Timeout in seconds */
	gint on_period;			/**< Pattern on-period in ms  */
	gint off_period;		/**< Pattern off-period in ms  */
	gint brightness;		/**< Pattern brightness */
	gboolean active;		/**< Is the pattern active? */
	gboolean enabled;		/**< Is the pattern enabled? */
	guint engine1_mux;		/**< Muxing for engine 1 */
	guint engine2_mux;		/**< Muxing for engine 2 */
	/** Pattern for the R-channel/engine 1 */
	gchar channel1[CHANNEL_SIZE + 1];
	/** Pattern for the G-channel/engine 2 */
	gchar channel2[CHANNEL_SIZE + 1];
	/** Pattern for the B-channel */
	gchar channel3[CHANNEL_SIZE + 1];
	guint gconf_cb_id;		/**< Callback ID for GConf entry */
} pattern_struct;

/** Pattern combination rule struct; this is also used for cross-referencing */
typedef struct {
	/** Name of the combined pattern */
	gchar *rulename;
	/** List of pre-requisite patterns */
	GQueue *pre_requisites;
} combination_rule_struct;

/** Pointer to the top pattern */
static pattern_struct *active_pattern = NULL;
/** The active brightness */
static gint active_brightness = -1;

/** Currently driven leds */
static guint current_lysti_led_pattern = 0;

/** Brightness levels for the mono-LED */
static const gchar *const brightness_map[] = {
	BRIGHTNESS_LEVEL_0,
	BRIGHTNESS_LEVEL_1,
	BRIGHTNESS_LEVEL_2,
	BRIGHTNESS_LEVEL_3,
	BRIGHTNESS_LEVEL_4,
	BRIGHTNESS_LEVEL_5,
	BRIGHTNESS_LEVEL_6,
	BRIGHTNESS_LEVEL_7,
	BRIGHTNESS_LEVEL_8,
	BRIGHTNESS_LEVEL_9,
	BRIGHTNESS_LEVEL_10,
	BRIGHTNESS_LEVEL_11,
	BRIGHTNESS_LEVEL_12,
	BRIGHTNESS_LEVEL_13,
	BRIGHTNESS_LEVEL_14,
	BRIGHTNESS_LEVEL_15
};

/** LED type */
typedef enum {
	/** LED type unset */
	LED_TYPE_UNSET = -1,
	/** No LED available */
	LED_TYPE_NONE = 0,
	/** Monochrome LED, direct LED control */
	LED_TYPE_DIRECT_MONO = 1,
	/** RGB LED, NJoy (LP5521) LED controller */
	LED_TYPE_NJOY_RGB = 2,
	/** Monochrome LED, NJoy (LP5521) LED controller */
	LED_TYPE_NJOY_MONO = 3,
	/** RGB LED, Lysti (LP5523) LED controller */
	LED_TYPE_LYSTI_RGB = 4,
	/** Monochrome LED, Lysti (LP5523) LED controller */
	LED_TYPE_LYSTI_MONO = 5,
} led_type_t;

/**
 * The ID of the LED timer
 */
static guint led_pattern_timeout_cb_id = 0;

/**
 * The configuration group containing the LED pattern
 */
static const gchar *led_pattern_group = NULL;

/** Path to monochrome/red channel LED current path  */
static gchar *led_current_rm_path = NULL;
/** Path to green channel LED current path */
static gchar *led_current_g_path = NULL;
/** Path to blue channel LED current path */
static gchar *led_current_b_path = NULL;

/** Path to monochrome/red channel LED brightness path  */
static gchar *led_brightness_rm_path = NULL;
/** Path to red channel LED brightness path */
static gchar *led_brightness_g_path = NULL;
/** Path to blue channel LED brightness path */
static gchar *led_brightness_b_path = NULL;

/** Path to engine 1 mode */
static gchar *engine1_mode_path = NULL;
/** Path to engine 2 mode */
static gchar *engine2_mode_path = NULL;
/** Path to engine 3 mode */
static gchar *engine3_mode_path = NULL;

/** Path to engine 1 load */
static gchar *engine1_load_path = NULL;
/** Path to engine 2 load */
static gchar *engine2_load_path = NULL;
/** Path to engine 3 load */
static gchar *engine3_load_path = NULL;

/** Path to engine 1 leds */
static gchar *engine1_leds_path = NULL;
/** Path to engine 2 leds */
static gchar *engine2_leds_path = NULL;
/** Path to engine 3 leds */
static gchar *engine3_leds_path = NULL;

/** File pointer for the monochrome/red channel LED current */
static FILE *led_current_rm_fp = NULL;
/** File pointer for the green channel LED current */
static FILE *led_current_g_fp = NULL;
/** File pointer for the blue channel LED current */
static FILE *led_current_b_fp = NULL;

/** File pointer for the monochrome/red channel LED brightness */
static FILE *led_brightness_rm_fp = NULL;
/** File pointer for the green channel LED brightness */
static FILE *led_brightness_g_fp = NULL;
/** File pointer for the blue channel LED brightness */
static FILE *led_brightness_b_fp = NULL;

/** Maximum LED brightness */
static guint maximum_led_brightness = MAXIMUM_LYSTI_MONOCHROME_LED_CURRENT;

static void cancel_pattern_timeout(void);
static void led_update_active_pattern(void);

/**
 * Disable the Reno LED controller
 */
static void disable_reno(void)
{
	int fd;

	mce_log(LL_DEBUG, "Disabling Reno");

	if ((fd = open("/dev/i2c-1", O_RDWR)) == -1) {
		mce_log(LL_CRIT, "Failed to open /dev/i2c-1; %s",
			g_strerror(errno));

		/* Reset errno,
		 * to avoid false positives down the line
		 */
		errno = 0;
		goto EXIT;
	}

	if (ioctl(fd, I2C_SLAVE_FORCE, TWL5031_BCC) == -1) {
		mce_log(LL_CRIT,
			"ioctl() I2C_SLAVE_FORCE (%d) failed on `%s'; %s",
			TWL5031_BCC, "/dev/i2c-1", g_strerror(errno));

		/* Reset errno,
		 * to avoid false positives down the line
		 */
		errno = 0;
		goto EXIT;
	}

        if (i2c_smbus_write_byte_data(fd, LED_DRIVER_CTRL, LEDC_DISABLE) < 0) {
		mce_log(LL_ERR,
			"i2c_smbus_write_byte_data(TWL5031_BCC, ...) failed");
	}

EXIT:
	if (fd != -1) {
		if (close(fd) == -1) {
			mce_log(LL_ERR,
				"Failed to close `%s': %s",
				"/dev/i2c-1", g_strerror(errno));

			/* Reset errno,
			 * to avoid false positives down the line
			 */
			errno = 0;
		}
	}

	return;
}

/**
 * Get the LED type
 *
 * @return The LED type
 */
static led_type_t get_led_type(void)
{
	product_id_t product_id = get_product_id();
	static led_type_t led_type = LED_TYPE_UNSET;

	/* If we have the LED type already, return it */
	if (led_type != LED_TYPE_UNSET)
		goto EXIT;

	/* First build the paths needed to check */
	switch (product_id) {
	case PRODUCT_RM716:
	case PRODUCT_RM696:
		led_type = LED_TYPE_NJOY_MONO;
		led_pattern_group = MCE_CONF_LED_PATTERN_RM696_GROUP;
		maximum_led_brightness = MAXIMUM_NJOY_MONOCHROME_LED_CURRENT;

		/* Build paths */
		led_current_rm_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_CURRENT_SUFFIX, NULL);
		led_brightness_rm_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_BRIGHTNESS_SUFFIX, NULL);

		engine1_mode_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE1, MCE_LED_MODE_SUFFIX, NULL);
		engine2_mode_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE2, MCE_LED_MODE_SUFFIX, NULL);
		engine3_mode_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE3, MCE_LED_MODE_SUFFIX, NULL);

		/* We have 3 engines, but only 1 LED,
		 * so while we need to be able to set the mode of all
		 * engines (to disable the unused ones), we don't need
		 * to program them
		 */
		engine1_load_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE1, MCE_LED_LOAD_SUFFIX, NULL);

		disable_reno();
		break;

	case PRODUCT_RM690:
	case PRODUCT_RM680:
		led_type = LED_TYPE_LYSTI_MONO;
		led_pattern_group = MCE_CONF_LED_PATTERN_RM680_GROUP;
		maximum_led_brightness = MAXIMUM_LYSTI_MONOCHROME_LED_CURRENT;

		/* Build paths */
		led_current_rm_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL8, MCE_LED_CURRENT_SUFFIX, NULL);
		led_brightness_rm_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL8, MCE_LED_BRIGHTNESS_SUFFIX, NULL);

		/* Engine 3 is used by keyboard backlight */
		engine1_mode_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE1, MCE_LED_MODE_SUFFIX, NULL);
		engine2_mode_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE2, MCE_LED_MODE_SUFFIX, NULL);

		engine1_load_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE1, MCE_LED_LOAD_SUFFIX, NULL);
		engine2_load_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE2, MCE_LED_LOAD_SUFFIX, NULL);

		engine1_leds_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE1, MCE_LED_LEDS_SUFFIX, NULL);
		engine2_leds_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE2, MCE_LED_LEDS_SUFFIX, NULL);

		disable_reno();
		break;

	case PRODUCT_RX51:
		led_type = LED_TYPE_LYSTI_RGB;
		led_pattern_group = MCE_CONF_LED_PATTERN_RX51_GROUP;
		maximum_led_brightness = MAXIMUM_LYSTI_RGB_LED_CURRENT;

		/* Build paths */
		led_current_rm_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_CURRENT_SUFFIX, NULL);
		led_current_g_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL1, MCE_LED_CURRENT_SUFFIX, NULL);
		led_current_b_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL2, MCE_LED_CURRENT_SUFFIX, NULL);
		led_brightness_rm_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_BRIGHTNESS_SUFFIX, NULL);
		led_brightness_g_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL1, MCE_LED_BRIGHTNESS_SUFFIX, NULL);
		led_brightness_b_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL2, MCE_LED_BRIGHTNESS_SUFFIX, NULL);

		engine1_mode_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE1, MCE_LED_MODE_SUFFIX, NULL);
		engine2_mode_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE2, MCE_LED_MODE_SUFFIX, NULL);
		engine3_mode_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE3, MCE_LED_MODE_SUFFIX, NULL);

		engine1_load_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE1, MCE_LED_LOAD_SUFFIX, NULL);
		engine2_load_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE2, MCE_LED_LOAD_SUFFIX, NULL);
		engine3_load_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE3, MCE_LED_LOAD_SUFFIX, NULL);

		engine1_leds_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE1, MCE_LED_LEDS_SUFFIX, NULL);
		engine2_leds_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE2, MCE_LED_LEDS_SUFFIX, NULL);
		engine3_leds_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE3, MCE_LED_LEDS_SUFFIX, NULL);
		break;

	case PRODUCT_RX44:
	case PRODUCT_RX48:
		led_type = LED_TYPE_NJOY_RGB;
		maximum_led_brightness = MAXIMUM_NJOY_RGB_LED_CURRENT;

		if (product_id == PRODUCT_RX48)
			led_pattern_group = MCE_CONF_LED_PATTERN_RX48_GROUP;
		else
			led_pattern_group = MCE_CONF_LED_PATTERN_RX44_GROUP;

		/* Build paths */
		led_current_rm_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_CURRENT_SUFFIX, NULL);
		led_brightness_rm_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_BRIGHTNESS_SUFFIX, NULL);

		engine1_mode_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE1, MCE_LED_MODE_SUFFIX, NULL);
		engine2_mode_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL1, MCE_LED_DEVICE, MCE_LED_ENGINE2, MCE_LED_MODE_SUFFIX, NULL);
		engine3_mode_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL2, MCE_LED_DEVICE, MCE_LED_ENGINE3, MCE_LED_MODE_SUFFIX, NULL);

		engine1_load_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_DEVICE, MCE_LED_ENGINE1, MCE_LED_LOAD_SUFFIX, NULL);
		engine2_load_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL1, MCE_LED_DEVICE, MCE_LED_ENGINE2, MCE_LED_LOAD_SUFFIX, NULL);
		engine3_load_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL2, MCE_LED_DEVICE, MCE_LED_ENGINE3, MCE_LED_LOAD_SUFFIX, NULL);
		break;

	case PRODUCT_RX34:
		led_type = LED_TYPE_DIRECT_MONO;
		led_pattern_group = MCE_CONF_LED_PATTERN_RX34_GROUP;

		/* Build paths */
		led_brightness_rm_path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_KEYPAD_PREFIX, MCE_LED_BRIGHTNESS_SUFFIX, NULL);
		break;

	default:
		led_type = LED_TYPE_NONE;
		break;
	}

	mce_log(LL_DEBUG, "LED-type: %d", led_type);

EXIT:
	return led_type;
}

/**
 * Custom find function to get a particular entry in the pattern stack
 *
 * @param data The pattern_struct entry
 * @param userdata The pattern name
 * @return Less than, equal to, or greater than zero depending
 *         whether the name of the pattern struct pointed to by data
 *         is less than, equal to, or greater than entry2
 */
static gint queue_find(gconstpointer data, gconstpointer userdata) G_GNUC_PURE;
static gint queue_find(gconstpointer data, gconstpointer userdata)
{
	pattern_struct *psp;

	if (data == NULL || userdata == NULL)
		return -1;

	psp = (pattern_struct *)data;

	if (psp->name == NULL)
		return -1;

	return strcmp(psp->name, (gchar *)userdata);
}

/**
 * Custom compare function used for priority insertions
 *
 * @param entry1 Queue entry 1
 * @param entry2 Queue entry 2
 * @param userdata The pattern name
 * @return Less than, equal to, or greater than zero depending
 *         whether the priority of entry1 is less than, equal to,
 *         or greater than the priority of entry2
 */
static gint queue_prio_compare(gconstpointer entry1,
			       gconstpointer entry2,
			       gpointer userdata) G_GNUC_PURE;
static gint queue_prio_compare(gconstpointer entry1,
			       gconstpointer entry2,
			       gpointer userdata)
{
	pattern_struct *psp1 = (pattern_struct *)entry1;
	pattern_struct *psp2 = (pattern_struct *)entry2;

	(void)userdata;

	return psp1->priority - psp2->priority;
}

/**
 * Set Lysti-LED brightness
 *
 * @param brightness The brightness of the LED
 *                   (0 - maximum_led_brightness),
 *                   or -1 to adjust colour hues without changing brightness,
 *                   and to reset brightness when the LED has been disabled
 */
static void lysti_set_brightness(gint brightness)
{
	guint r_brightness = 0;
	guint g_brightness = 0;
	guint b_brightness = 0;

	if (brightness < -1 || brightness > (gint)maximum_led_brightness) {
		mce_log(LL_WARN, "Invalid brightness value %d", brightness);
		return;
	}

	if (brightness != -1) {
		if (active_brightness == brightness)
			return;

		active_brightness = brightness;
	}

	if ((current_lysti_led_pattern & MCE_LYSTI_RED_MASK_RX51) &&
	    (get_led_type() == LED_TYPE_LYSTI_RGB)) {
		/* Red is on, tweaking is needed */
		if ((current_lysti_led_pattern & MCE_LYSTI_GREEN_MASK_RX51) &&
		    (current_lysti_led_pattern & MCE_LYSTI_BLUE_MASK_RX51)) {
			/* White */
			r_brightness = (unsigned)active_brightness * 4;
			r_brightness = (r_brightness < maximum_led_brightness) ? r_brightness : maximum_led_brightness;
			g_brightness = r_brightness / 4;
			b_brightness = r_brightness / 4;
		} else if (current_lysti_led_pattern & MCE_LYSTI_GREEN_MASK_RX51) {
			/* Orange */
			r_brightness = (unsigned)active_brightness * 10;
			r_brightness = (r_brightness < maximum_led_brightness) ? r_brightness : maximum_led_brightness;
			g_brightness = r_brightness / 10;
			b_brightness = 0;
		} else {
			/* Purple */
			r_brightness = (unsigned)active_brightness * 4;
			r_brightness = (r_brightness < maximum_led_brightness) ? r_brightness : maximum_led_brightness;
			b_brightness = r_brightness / 4;
			g_brightness = 0;
		}
	} else {
		/* When red is not on, we use brightness as is */
		r_brightness = (unsigned)active_brightness;
		g_brightness = (unsigned)active_brightness;
		b_brightness = (unsigned)active_brightness;
	}

	if (get_led_type() == LED_TYPE_LYSTI_MONO) {
		/* If we have a monochrome LED only set one brightness */
		(void)mce_write_number_string_to_file(led_current_rm_path, r_brightness, &led_current_rm_fp, TRUE, FALSE);

		mce_log(LL_DEBUG,
			"Brightness set to %d",
			active_brightness);
	} else if (get_led_type() == LED_TYPE_LYSTI_RGB) {
		/* If we have an RGB LED set the brightness for all channels */
		(void)mce_write_number_string_to_file(led_current_rm_path, r_brightness, &led_current_rm_fp, TRUE, FALSE);
		(void)mce_write_number_string_to_file(led_current_g_path, g_brightness, &led_current_g_fp, TRUE, FALSE);
		(void)mce_write_number_string_to_file(led_current_b_path, b_brightness, &led_current_b_fp, TRUE, FALSE);

		mce_log(LL_DEBUG,
			"Brightness set to %d (%d, %d, %d)",
			active_brightness, r_brightness,
			g_brightness, b_brightness);
	}
}

/**
 * Set NJoy-LED brightness
 *
 * @param brightness The brightness of the LED
 *                   (0 - maximum_led_brightness),
 *                   or -1 to reset brightness when the LED has been disabled
 */
static void njoy_set_brightness(gint brightness)
{
	if (brightness < -1 || brightness > (gint)maximum_led_brightness) {
		mce_log(LL_WARN, "Invalid brightness value %d", brightness);
		return;
	}

	/* This is a bit questionable, but currently 696 does not have any
	 * use for brightness setting, it only causes unwanted LED
	 * turn-ons when used with ALS. Let zero brightnesses through to
	 * be a bit safer.
	 */
	if ((get_product_id() == PRODUCT_RM696) &&
		((brightness > 0) ||
		 (brightness == -1 && active_brightness != 0))) {
		mce_log(LL_DEBUG, "don't set useless brightness value %d", brightness);
		return;
	}

	if (brightness != -1) {
		if (active_brightness == brightness)
			return;

		active_brightness = brightness;
	}

	(void)mce_write_number_string_to_file(led_brightness_rm_path,
					      (unsigned)active_brightness,
					      &led_brightness_rm_fp,
					      TRUE, FALSE);

	mce_log(LL_DEBUG, "Brightness set to %d", active_brightness);
}

/**
 * Set mono-LED brightness
 *
 * @param brightness The brightness of the LED (0-15)
 */
static void mono_set_brightness(gint brightness)
{
	if (brightness < 0 || brightness > 15) {
		mce_log(LL_WARN, "Invalid brightness value %d", brightness);
		return;
	}

	if (active_brightness == brightness)
		return;

	active_brightness = brightness;
	(void)mce_write_string_to_file(led_brightness_rm_path,
				       brightness_map[brightness]);

	mce_log(LL_DEBUG, "Brightness set to %d", brightness);
}

/**
 * Disable the Lysti-LED
 */
static void lysti_disable_led(void)
{
	/* Disable engine 1 */
	(void)mce_write_string_to_file(engine1_mode_path,
				       MCE_LED_DISABLED_MODE);

	if (get_led_type() == LED_TYPE_LYSTI_MONO) {
		/* Turn off the led */
		(void)mce_write_number_string_to_file(led_brightness_rm_path, 0, &led_brightness_rm_fp, TRUE, FALSE);
	} else if (get_led_type() == LED_TYPE_LYSTI_RGB) {
		/* Disable engine 2 */
		(void)mce_write_string_to_file(engine2_mode_path,
					       MCE_LED_DISABLED_MODE);

		/* Turn off all three leds */
		(void)mce_write_number_string_to_file(led_brightness_rm_path, 0, &led_brightness_rm_fp, TRUE, FALSE);
		(void)mce_write_number_string_to_file(led_brightness_g_path, 0, &led_brightness_g_fp, TRUE, FALSE);
		(void)mce_write_number_string_to_file(led_brightness_b_path, 0, &led_brightness_b_fp, TRUE, FALSE);
	}
}

/**
 * Disable the NJoy-LED
 */
static void njoy_disable_led(void)
{
	/* Disable engine 1 */
	(void)mce_write_string_to_file(engine1_mode_path,
				       MCE_LED_DISABLED_MODE);

	if (get_led_type() == LED_TYPE_NJOY_MONO) {
		/* Turn off the led */
		(void)mce_write_number_string_to_file(led_brightness_rm_path, 0, &led_brightness_rm_fp, TRUE, FALSE);
	} else if (get_led_type() == LED_TYPE_NJOY_RGB) {
		/* Disable engine 2 */
		(void)mce_write_string_to_file(engine2_mode_path,
					       MCE_LED_DISABLED_MODE);

		/* Disable engine 3 */
		(void)mce_write_string_to_file(engine3_mode_path,
					       MCE_LED_DISABLED_MODE);

		/* Turn off all three leds */
		(void)mce_write_number_string_to_file(led_brightness_rm_path, 0, &led_brightness_rm_fp, TRUE, FALSE);
		(void)mce_write_number_string_to_file(led_brightness_g_path, 0, &led_brightness_g_fp, TRUE, FALSE);
		(void)mce_write_number_string_to_file(led_brightness_b_path, 0, &led_brightness_b_fp, TRUE, FALSE);
	}
}

/**
 * Disable the mono-LED
 */
static void mono_disable_led(void)
{
	(void)mce_write_string_to_file(MCE_LED_TRIGGER_PATH,
				       MCE_LED_TRIGGER_NONE);
	mono_set_brightness(0);
}

/**
 * Disable the LED
 */
static void disable_led(void)
{
	cancel_pattern_timeout();

	switch (get_led_type()) {
	case LED_TYPE_LYSTI_RGB:
	case LED_TYPE_LYSTI_MONO:
		lysti_disable_led();
		break;

	case LED_TYPE_NJOY_RGB:
	case LED_TYPE_NJOY_MONO:
		njoy_disable_led();
		break;

	case LED_TYPE_DIRECT_MONO:
		mono_disable_led();
		break;

	default:
		break;
	}
}

/**
 * Timeout callback for LED patterns
 *
 * @param data Unused
 * @return Always returns FALSE to disable timeout
 */
static gboolean led_pattern_timeout_cb(gpointer data)
{
	(void)data;

	led_pattern_timeout_cb_id = 0;

	active_pattern->active = FALSE;
	led_update_active_pattern();

	return FALSE;
}

/**
 * Cancel pattern timeout
 */
static void cancel_pattern_timeout(void)
{
	/* Remove old timeout */
	if (led_pattern_timeout_cb_id != 0) {
		g_source_remove(led_pattern_timeout_cb_id);
		led_pattern_timeout_cb_id = 0;
	}
}

/**
 * Setup pattern timeout
 */
static void setup_pattern_timeout(gint timeout)
{
	cancel_pattern_timeout();

	/* Setup new timeout */
	led_pattern_timeout_cb_id =
		g_timeout_add_seconds(timeout, led_pattern_timeout_cb, NULL);
}

/**
 * Setup and activate a new Lysti-LED pattern
 *
 * @param pattern A pointer to a pattern_struct with the new pattern
 */
static void lysti_program_led(const pattern_struct *const pattern)
{
	/* Disable old LED patterns */
	lysti_disable_led();

	/* Load new patterns, one engine at a time */

	/* Engine 1 */
	(void)mce_write_string_to_file(engine1_mode_path,
				       MCE_LED_LOAD_MODE);
	(void)mce_write_string_to_file(engine1_leds_path,
				       bin_to_string(pattern->engine1_mux));
	(void)mce_write_string_to_file(engine1_load_path,
				       pattern->channel1);

	/* Engine 2; if needed */
	if (get_led_type() == LED_TYPE_LYSTI_RGB) {
		(void)mce_write_string_to_file(engine2_mode_path,
					       MCE_LED_LOAD_MODE);
		(void)mce_write_string_to_file(engine2_leds_path,
					       bin_to_string(pattern->engine2_mux));
		(void)mce_write_string_to_file(engine2_load_path,
					       pattern->channel2);

		/* Run the new pattern; enable engines in reverse order */
		(void)mce_write_string_to_file(engine2_mode_path,
					       MCE_LED_RUN_MODE);
	}

	(void)mce_write_string_to_file(engine1_mode_path,
				       MCE_LED_RUN_MODE);

        /* Save what colors we are driving */
        current_lysti_led_pattern = pattern->engine1_mux | pattern->engine2_mux;

        /* Reset brightness and update color hue
	 * according what leds are driven
	 */
        lysti_set_brightness(-1);
}

/**
 * Setup and activate a new NJoy-LED pattern
 *
 * @param pattern A pointer to a pattern_struct with the new pattern
 */
static void njoy_program_led(const pattern_struct *const pattern)
{
	/* Disable old LED patterns */
	njoy_disable_led();

	/* Load new patterns */

	/* Engine 1 */
	(void)mce_write_string_to_file(engine1_mode_path,
				       MCE_LED_LOAD_MODE);
	(void)mce_write_string_to_file(engine1_load_path,
				       pattern->channel1);

	if (get_led_type() == LED_TYPE_NJOY_RGB) {
		/* Engine 2 */
		(void)mce_write_string_to_file(engine2_mode_path,
					       MCE_LED_LOAD_MODE);
		(void)mce_write_string_to_file(engine2_load_path,
					       pattern->channel2);

		/* Engine 3 */
		(void)mce_write_string_to_file(engine3_mode_path,
					       MCE_LED_LOAD_MODE);
		(void)mce_write_string_to_file(engine3_load_path,
					       pattern->channel3);

		/* Run the new pattern; enable engines in reverse order */
		(void)mce_write_string_to_file(engine3_mode_path,
					       MCE_LED_RUN_MODE);
		(void)mce_write_string_to_file(engine2_mode_path,
					       MCE_LED_RUN_MODE);
	}

	(void)mce_write_string_to_file(engine1_mode_path,
				       MCE_LED_RUN_MODE);

	/* Reset brightness */
        njoy_set_brightness(-1);
}

/**
 * Setup and activate a new mono-LED pattern
 *
 * @param pattern A pointer to a pattern_struct with the new pattern
 */
static void mono_program_led(const pattern_struct *const pattern)
{
	/* This shouldn't happen; disable the LED instead */
	if (pattern->on_period == 0) {
		mono_disable_led();
		goto EXIT;
	}

	/* If we have a normal, on/off pattern,
	 * use a timer trigger, otherwise disable the trigger
	 */
	if (pattern->off_period != 0) {
		(void)mce_write_string_to_file(MCE_LED_TRIGGER_PATH,
					       MCE_LED_TRIGGER_TIMER);
		(void)mce_write_number_string_to_file(MCE_LED_OFF_PERIOD_PATH,
						      (unsigned)pattern->off_period, NULL, TRUE, TRUE);
		(void)mce_write_number_string_to_file(MCE_LED_ON_PERIOD_PATH,
						      (unsigned)pattern->on_period, NULL, TRUE, TRUE);
	} else {
		(void)mce_write_string_to_file(MCE_LED_TRIGGER_PATH,
					       MCE_LED_TRIGGER_NONE);
	}

	mono_set_brightness(pattern->brightness);

EXIT:
	return;
}

/**
 * Setup and activate a new LED pattern
 *
 * @param pattern A pointer to a pattern_struct with the new pattern
 */
static void program_led(const pattern_struct *const pattern)
{
	switch (get_led_type()) {
	case LED_TYPE_LYSTI_RGB:
	case LED_TYPE_LYSTI_MONO:
		lysti_program_led(pattern);
		break;

	case LED_TYPE_NJOY_RGB:
	case LED_TYPE_NJOY_MONO:
		njoy_program_led(pattern);
		break;

	case LED_TYPE_DIRECT_MONO:
		mono_program_led(pattern);
		break;

	default:
		break;
	}
}

/**
 * Recalculate active pattern and update the pattern timer
 */
static void led_update_active_pattern(void)
{
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	pattern_struct *new_active_pattern;
	gint i = 0;

	if (g_queue_is_empty(pattern_stack) == TRUE) {
		disable_led();
		goto EXIT;
	}

	while ((new_active_pattern = g_queue_peek_nth(pattern_stack,
						      i++)) != NULL) {
		mce_log(LL_DEBUG,
			"pattern: %s, active: %d, enabled: %d",
			new_active_pattern->name,
			new_active_pattern->active,
			new_active_pattern->enabled);

		/* If the pattern is deactivated, ignore */
		if (new_active_pattern->active == FALSE)
			continue;

		/* If the pattern is disabled through GConf, ignore */
		if (new_active_pattern->enabled == FALSE)
			continue;

		/* If the LED is disabled,
		 * only patterns with visibility 5 are shown
		 */
		if ((led_enabled == FALSE) &&
		    (new_active_pattern->policy != 5))
			continue;

		/* Always show pattern with visibility 3 or 5 */
		if ((new_active_pattern->policy == 3) ||
		    (new_active_pattern->policy == 5))
			break;

		/* Acting dead behaviour */
		if (system_state == MCE_STATE_ACTDEAD) {
			/* If we're in acting dead,
			 * show patterns with visibility 4
			 */
			if (new_active_pattern->policy == 4)
				break;

			/* If we're in acting dead
			 * and the display is off, show pattern
			 */
			if ((display_state == MCE_DISPLAY_OFF) &&
			    (new_active_pattern->policy == 2))
				break;

			/* If the display is on and visibility is 2,
			 * or if visibility is 1/0, ignore pattern
			 */
			continue;
		}

		/* If the display is off or in low power mode,
		 * we can use any active pattern
		 */
		if ((display_state == MCE_DISPLAY_OFF) ||
		    (display_state == MCE_DISPLAY_LPM_OFF) ||
		    (display_state == MCE_DISPLAY_LPM_ON))
			break;

		/* If the pattern should be shown with screen on, use it */
		if (new_active_pattern->policy == 1)
			break;
	}

	if ((new_active_pattern == NULL) ||
	    ((led_enabled == FALSE) &&
	     (new_active_pattern->policy != 5))) {
		active_pattern = NULL;
		disable_led();
		cancel_pattern_timeout();
		goto EXIT;
	}

	/* Only reprogram the pattern and timer if the pattern changed */
	if (new_active_pattern != active_pattern) {
		disable_led();

		if (new_active_pattern->timeout != -1) {
			setup_pattern_timeout(new_active_pattern->timeout);
		}

		program_led(new_active_pattern);
	}

	active_pattern = new_active_pattern;

EXIT:
	return;
}

/**
 * Find the pattern struct for a pattern
 *
 * @param name The name of the pattern
 * @return A pointer to the pattern struct, or NULL if no such pattern exists
 */
static pattern_struct *find_pattern_struct(const gchar *const name)
{
	pattern_struct *psp = NULL;
	GList *glp;

	if (name == NULL)
		goto EXIT;

	if ((glp = g_queue_find_custom(pattern_stack,
				       name, queue_find)) != NULL) {
		psp = (pattern_struct *)glp->data;
	}

EXIT:
	return psp;
}

/**
 * Update combination rule
 *
 * @param name The rule to process
 * @param data Unused
 */
static void update_combination_rule(gpointer name, gpointer data)
{
	combination_rule_struct *cr;
	gboolean enabled = TRUE;
	pattern_struct *psp;
	GList *glp;
	gchar *tmp;
	gint i;

	(void)data;

	if ((glp = g_queue_find_custom(combination_rule_list,
				       name, queue_find)) == NULL)
		goto EXIT;

	cr = glp->data;

	/* If all patterns in the pre_requisite list are enabled,
	 * then enable this pattern, else disable it
	 */
	for (i = 0; (tmp = g_queue_peek_nth(cr->pre_requisites, i)) != NULL; i++) {
		/* We've got a pattern name; check if that pattern is active */
		if (((psp = find_pattern_struct(tmp)) == NULL) ||
		    (psp->active == FALSE)) {
			enabled = FALSE;
			break;
		}
	}

	if ((psp = find_pattern_struct(name)) == NULL)
		goto EXIT;

	psp->active = enabled;

EXIT:
	return;
}

/**
 * Update activate patterns based on combination rules
 *
 * @param name THe name of the pattern that changed state
 */
static void update_combination_rules(const gchar *const name)
{
	GList *glp;

	if (name == NULL) {
		mce_log(LL_CRIT,
			"called with name == NULL");
		goto EXIT;
	}

	if ((glp = g_queue_find_custom(combination_rule_xref_list, name,
				       queue_find)) != NULL) {
		combination_rule_struct *xrf = glp->data;

		/* Update all combination rules that this pattern influences */
		g_queue_foreach(xrf->pre_requisites,
				update_combination_rule, NULL);
	}

EXIT:
	return;
}

/**
 * Activate a pattern in the pattern-stack
 *
 * @param name The name of the pattern to activate
 */
static void led_activate_pattern(const gchar *const name)
{
	pattern_struct *psp;

	if (name == NULL) {
		mce_log(LL_CRIT,
			"called with name == NULL");
		goto EXIT;
	}

	if ((psp = find_pattern_struct(name)) != NULL) {
		psp->active = TRUE;
		update_combination_rules(name);
		led_update_active_pattern();
		mce_log(LL_DEBUG,
			"LED pattern %s activated",
			name);
	} else {
		mce_log(LL_DEBUG,
			"Received request to activate "
			"a non-existing LED pattern");
	}

EXIT:
	return;
}

/**
 * Deactivate a pattern in the pattern-stack
 *
 * @param name The name of the pattern to deactivate
 */
static void led_deactivate_pattern(const gchar *const name)
{
	pattern_struct *psp;

	if ((psp = find_pattern_struct(name)) != NULL) {
		psp->active = FALSE;
		update_combination_rules(name);
		led_update_active_pattern();
		mce_log(LL_DEBUG,
			"LED pattern %s deactivated",
			name);
	} else {
		mce_log(LL_DEBUG,
			"Received request to deactivate "
			"a non-existing LED pattern");
	}
}

/**
 * Enable the LED
 */
static void led_enable(void)
{
	led_enabled = TRUE;
	led_update_active_pattern();
}

/**
 * Disable the LED
 */
static void led_disable(void)
{
	led_enabled = FALSE;
	disable_led();
}

/**
 * Handle system state change
 *
 * @param data Unused
 */
static void system_state_trigger(gconstpointer data)
{
	(void)data;

	led_update_active_pattern();
}

/**
 * Handle display state change
 *
 * @param data Unused
 */
static void display_state_trigger(gconstpointer data)
{
	static display_state_t old_display_state = MCE_DISPLAY_UNDEF;
	display_state_t display_state = GPOINTER_TO_INT(data);

	if (old_display_state == display_state)
		goto EXIT;

	led_update_active_pattern();
	old_display_state = display_state;

EXIT:
	return;
}

/**
 * Handle led brightness change
 *
 * @param data The LED brightness stored in a pointer
 */
static void led_brightness_trigger(gconstpointer data)
{
	gint led_brightness = GPOINTER_TO_INT(data);

	switch (get_led_type()) {
	case LED_TYPE_LYSTI_RGB:
	case LED_TYPE_LYSTI_MONO:
		lysti_set_brightness(led_brightness);
		break;

	case LED_TYPE_NJOY_RGB:
	case LED_TYPE_NJOY_MONO:
		njoy_set_brightness(led_brightness);
		break;

	case LED_TYPE_DIRECT_MONO:
	case LED_TYPE_UNSET:
	case LED_TYPE_NONE:
	default:
		break;
	}
}

/**
 * Handle LED pattern activate requests
 *
 * @param data The pattern name
 */
static void led_pattern_activate_trigger(gconstpointer data)
{
	led_activate_pattern((gchar *)data);
}

/**
 * Handle LED pattern deactivate requests
 *
 * @param data The pattern name
 */
static void led_pattern_deactivate_trigger(gconstpointer data)
{
	led_deactivate_pattern((gchar *)data);
}

/**
 * Custom find function to get a GConf callback ID in the pattern stack
 *
 * @param data The pattern_struct entry
 * @param userdata The pattern name
 * @return 0 if the GConf callback id of data matches that of userdata,
 *         -1 if they don't match
 */
static gint gconf_cb_find(gconstpointer data, gconstpointer userdata)
{
	pattern_struct *psp;

	if ((data == NULL) || (userdata == NULL))
		return -1;

	psp = (pattern_struct *)data;

	return psp->gconf_cb_id != *(guint *)userdata;
}

/**
 * GConf callback for LED related settings
 *
 * @param gcc Unused
 * @param id Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data Unused
 */
static void led_gconf_cb(GConfClient *const gcc, const guint id,
			 GConfEntry *const entry, gpointer const data)
{
	const GConfValue *gcv = gconf_entry_get_value(entry);
	pattern_struct *psp = NULL;
	GList *glp = NULL;

	(void)gcc;
	(void)data;

	/* Key is unset */
	if (gcv == NULL) {
		mce_log(LL_DEBUG,
			"GConf Key `%s' has been unset",
			gconf_entry_get_key(entry));
		goto EXIT;
	}

	if ((glp = g_queue_find_custom(pattern_stack,
				       &id, gconf_cb_find)) != NULL) {
		psp = (pattern_struct *)glp->data;
		psp->enabled = gconf_value_get_bool(gcv);
		led_update_active_pattern();
	} else {
		mce_log(LL_WARN, "Spurious GConf value received; confused!");
	}

EXIT:
	return;
}

/**
 * Get the enabled/disabled value from GConf and set up a notifier
 */
static gboolean pattern_get_enabled(const gchar *const patternname,
				    guint *gconf_cb_id)
{
	gboolean retval = DEFAULT_PATTERN_ENABLED;
	gchar *path = gconf_concat_dir_and_key(MCE_GCONF_LED_PATH,
					       patternname);

	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_bool(path, &retval);

	if (mce_gconf_notifier_add(MCE_GCONF_LED_PATH, path,
				   led_gconf_cb, gconf_cb_id) == FALSE)
		goto EXIT;

EXIT:
	g_free(path);

	return retval;
}

/**
 * D-Bus callback for the activate LED pattern method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean led_activate_pattern_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	const gchar *pattern = NULL;
	gboolean status = FALSE;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	mce_log(LL_DEBUG, "Received activate LED pattern request");

	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &pattern,
				  DBUS_TYPE_INVALID) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_CRIT,
			"Failed to get argument from %s.%s: %s",
			MCE_REQUEST_IF, MCE_ACTIVATE_LED_PATTERN,
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	led_activate_pattern(pattern);

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
 * D-Bus callback for the deactivate LED pattern method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean led_deactivate_pattern_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	const gchar *pattern = NULL;
	gboolean status = FALSE;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	mce_log(LL_DEBUG, "Received deactivate LED pattern request");

	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &pattern,
				  DBUS_TYPE_INVALID) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_CRIT,
			"Failed to get argument from %s.%s: %s",
			MCE_REQUEST_IF, MCE_DEACTIVATE_LED_PATTERN,
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	led_deactivate_pattern(pattern);

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
 * D-Bus callback for the enable LED method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean led_enable_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	gboolean status = FALSE;

	mce_log(LL_DEBUG, "Received LED enable request");

	led_enable();

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

//EXIT:
	return status;
}

/**
 * D-Bus callback for the disable LED method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean led_disable_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	gboolean status = FALSE;

	mce_log(LL_DEBUG, "Received LED disable request");

	led_disable();
	active_pattern = NULL;

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

//EXIT:
	return status;
}

/**
 * Init LED pattern combination rules
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean init_combination_rules(void)
{
	gboolean status = FALSE;
	gchar **crlist = NULL;
	gsize length;
	gint i;

	/* Get the list of valid LED patttern combination rules */
	crlist = mce_conf_get_string_list(MCE_CONF_LED_GROUP,
					  MCE_CONF_LED_COMBINATION_RULES,
					  &length,
					  NULL);

	/* Treat failed conf-value reads as if they were due to invalid keys
	 * rather than failed allocations; let future allocation attempts fail
	 * instead; otherwise we'll miss the real invalid key failures
	 */
	if (crlist == NULL) {
		mce_log(LL_WARN,
			"Failed to configure LED pattern combination rules");
		status = TRUE;
		goto EXIT;
	}

	/* Used for all combination patterns */
	for (i = 0; crlist[i]; i++) {
		gchar **tmp;

		mce_log(LL_DEBUG,
			"Getting LED pattern combination rule for: %s",
			crlist[i]);

		tmp = mce_conf_get_string_list(led_pattern_group,
					       crlist[i],
					       &length,
					       NULL);

		if (tmp != NULL) {
			combination_rule_struct *cr = NULL;
			guint j;

			if (length < 2) {
				mce_log(LL_ERR,
					"LED Pattern Combination rule `%s'",
					crlist[i]);
				g_strfreev(tmp);
				goto EXIT2;
			}

			cr = g_slice_new(combination_rule_struct);

			if (cr == NULL) {
				g_strfreev(tmp);
				goto EXIT2;
			}

			cr->rulename = strdup(tmp[0]);
			cr->pre_requisites = g_queue_new();

			for (j = 1; j < length; j++) {
				gchar *str = strdup(tmp[j]);
				GList *glp;
				combination_rule_struct *xrf = NULL;

				g_queue_push_head(cr->pre_requisites, str);

				glp = g_queue_find_custom(combination_rule_xref_list, str, queue_find);

				if ((glp == NULL) || (glp->data == NULL)) {
					xrf = g_slice_new(combination_rule_struct);
					xrf->rulename = str;
					xrf->pre_requisites = g_queue_new();
					g_queue_push_head(combination_rule_xref_list, xrf);
				} else {
					xrf = (combination_rule_struct *)glp->data;
				}

				/* If the cross reference isn't in the list
				 * already, add it
				 */
				if (g_queue_find_custom(xrf->pre_requisites, cr->rulename, queue_find) == NULL) {
					g_queue_push_head(xrf->pre_requisites,
							  cr->rulename);
				}
			}

			g_queue_push_head(combination_rule_list, cr);
		}
	}

	status = TRUE;

EXIT2:
	g_strfreev(crlist);

EXIT:
	return status;
}

/**
 * Init patterns for Lysti controlled RGB or monochrome LED
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean init_lysti_patterns(void)
{
	led_type_t led_type = get_led_type();
	gchar **patternlist = NULL;
	gboolean status = FALSE;
	gsize length;
	gint i;

	/* Get the list of valid LED patterns */
	patternlist = mce_conf_get_string_list(MCE_CONF_LED_GROUP,
					       MCE_CONF_LED_PATTERNS,
					       &length,
					       NULL);

	/* Treat failed conf-value reads as if they were due to invalid keys
	 * rather than failed allocations; let future allocation attempts fail
	 * instead; otherwise we'll miss the real invalid key failures
	 */
	if (patternlist == NULL) {
		mce_log(LL_WARN,
			"Failed to configure LED patterns");
		status = TRUE;
		goto EXIT;
	}

	/* Used for Lysti LED patterns */
	for (i = 0; patternlist[i]; i++) {
		gchar **tmp;

		mce_log(LL_DEBUG,
			"Getting LED pattern for: %s",
			patternlist[i]);

		tmp = mce_conf_get_string_list(led_pattern_group,
					       patternlist[i],
					       &length,
					       NULL);

		if (tmp != NULL) {
			pattern_struct *psp;
			guint engine1_mux;
			guint engine2_mux;

			if (((led_type == LED_TYPE_LYSTI_MONO) &&
			     ((length != NUMBER_OF_PATTERN_FIELDS_LYSTI_MONO) ||
			      (strlen(tmp[PATTERN_E_CHANNEL_FIELD]) >
			       CHANNEL_SIZE))) ||
			    ((led_type == LED_TYPE_LYSTI_RGB) &&
			     ((length != NUMBER_OF_PATTERN_FIELDS) ||
			      (strlen(tmp[PATTERN_E1_CHANNEL_FIELD]) >
			       CHANNEL_SIZE) ||
			      (strlen(tmp[PATTERN_E2_CHANNEL_FIELD]) >
			       CHANNEL_SIZE)))) {
				mce_log(LL_ERR,
					"Skipping invalid LED-pattern");
				g_strfreev(tmp);
				continue;
			}

			engine1_mux = 0;
			engine2_mux = 0;

			if (led_type == LED_TYPE_LYSTI_MONO) {
				engine1_mux |= MCE_LYSTI_MONOCHROME_MASK_RM680;
			} else if (led_type == LED_TYPE_LYSTI_RGB) {
				if (strchr(tmp[PATTERN_MUXING_FIELD], 'r'))
					engine1_mux |= MCE_LYSTI_RED_MASK_RX51;

				if (strchr(tmp[PATTERN_MUXING_FIELD], 'R'))
					engine2_mux |= MCE_LYSTI_RED_MASK_RX51;

				if (strchr(tmp[PATTERN_MUXING_FIELD], 'g'))
					engine1_mux |= MCE_LYSTI_GREEN_MASK_RX51;

				if (strchr(tmp[PATTERN_MUXING_FIELD], 'G'))
					engine2_mux |= MCE_LYSTI_GREEN_MASK_RX51;

				if (strchr(tmp[PATTERN_MUXING_FIELD], 'b'))
					engine1_mux |= MCE_LYSTI_BLUE_MASK_RX51;

				if (strchr(tmp[PATTERN_MUXING_FIELD], 'B'))
					engine2_mux |= MCE_LYSTI_BLUE_MASK_RX51;
			}

			if ((engine1_mux & engine2_mux) != 0) {
				mce_log(LL_ERR,
					"Same LED muxed to multiple engines; "
					"skipping invalid LED-pattern");
				g_strfreev(tmp);
				continue;
			}

			psp = g_slice_new(pattern_struct);

			if (!psp) {
				g_strfreev(tmp);
				goto EXIT2;
			}

			psp->priority = strtoul(tmp[PATTERN_PRIO_FIELD],
						NULL, 10);
			psp->policy = strtoul(tmp[PATTERN_SCREEN_ON_FIELD],
						 NULL, 10);

			if ((psp->timeout = strtoul(tmp[PATTERN_TIMEOUT_FIELD],
						    NULL, 10)) == 0)
				psp->timeout = -1;

			/* Catch all error checking for all three strtoul */
			if ((errno == EINVAL) || (errno == ERANGE)) {
				/* Reset errno,
				 * to avoid false positives further down
				 */
				g_strfreev(tmp);
				g_slice_free(pattern_struct, psp);
				continue;
			}

			psp->engine1_mux = engine1_mux;
			psp->engine2_mux = engine2_mux;

			if (led_type == LED_TYPE_LYSTI_MONO) {
				strncpy(psp->channel1,
				       tmp[PATTERN_E_CHANNEL_FIELD],
				       CHANNEL_SIZE);
			} else if (led_type == LED_TYPE_LYSTI_RGB) {
				strncpy(psp->channel1,
				       tmp[PATTERN_E1_CHANNEL_FIELD],
				       CHANNEL_SIZE);
				strncpy(psp->channel2,
				       tmp[PATTERN_E2_CHANNEL_FIELD],
				       CHANNEL_SIZE);
			}

			psp->active = FALSE;

			psp->enabled = pattern_get_enabled(patternlist[i],
							   &(psp->gconf_cb_id));

			psp->name = strdup(patternlist[i]);

			g_strfreev(tmp);

			g_queue_insert_sorted(pattern_stack, psp,
					      queue_prio_compare,
					      NULL);
		}
	}

	init_combination_rules();

	/* Set the LED brightness */
	execute_datapipe(&led_brightness_pipe,
			 GINT_TO_POINTER(maximum_led_brightness),
			 USE_INDATA, CACHE_INDATA);

	status = TRUE;

EXIT2:
	g_strfreev(patternlist);

EXIT:
	return status;
}

/**
 * Init patterns for NJoy controlled RGB LED
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean init_njoy_patterns(void)
{
	led_type_t led_type = get_led_type();
	gchar **patternlist = NULL;
	gboolean status = FALSE;
	gsize length;
	gint i;

	/* Get the list of valid LED patterns */
	patternlist = mce_conf_get_string_list(MCE_CONF_LED_GROUP,
					       MCE_CONF_LED_PATTERNS,
					       &length,
					       NULL);

	/* Treat failed conf-value reads as if they were due to invalid keys
	 * rather than failed allocations; let future allocation attempts fail
	 * instead; otherwise we'll miss the real invalid key failures
	 */
	if (patternlist == NULL) {
		mce_log(LL_WARN,
			"Failed to configure LED patterns");
		status = TRUE;
		goto EXIT;
	}

	/* Used for RGB NJoy LED patterns */
	for (i = 0; patternlist[i]; i++) {
		gchar **tmp;

		mce_log(LL_DEBUG,
			"Getting LED pattern for: %s",
			patternlist[i]);

		tmp = mce_conf_get_string_list(led_pattern_group,
					       patternlist[i],
					       &length,
					       NULL);

		if (tmp != NULL) {
			pattern_struct *psp;

			if (((led_type == LED_TYPE_NJOY_MONO) &&
			    ((length != NUMBER_OF_PATTERN_FIELDS_NJOY_MONO) ||
			     (strlen(tmp[PATTERN_E_CHANNEL_FIELD]) >
			      CHANNEL_SIZE))) ||
			    ((led_type == LED_TYPE_NJOY_RGB) &&
			     ((length != NUMBER_OF_PATTERN_FIELDS) ||
			      (strlen(tmp[PATTERN_R_CHANNEL_FIELD]) >
			       CHANNEL_SIZE) ||
			      (strlen(tmp[PATTERN_G_CHANNEL_FIELD]) >
			       CHANNEL_SIZE) ||
			      (strlen(tmp[PATTERN_B_CHANNEL_FIELD]) >
			       CHANNEL_SIZE)))) {
				mce_log(LL_ERR,
					"Skipping invalid LED-pattern");
				g_strfreev(tmp);
				continue;
			}

			psp = g_slice_new(pattern_struct);

			if (!psp) {
				g_strfreev(tmp);
				goto EXIT2;
			}

			psp->priority = strtoul(tmp[PATTERN_PRIO_FIELD],
						NULL, 10);
			psp->policy = strtoul(tmp[PATTERN_SCREEN_ON_FIELD],
						 NULL, 10);

			if ((psp->timeout = strtoul(tmp[PATTERN_TIMEOUT_FIELD],
						    NULL, 10)) == 0)
				psp->timeout = -1;

			/* Catch all error checking for all three strtoul */
			if ((errno == EINVAL) || (errno == ERANGE)) {
				/* Reset errno,
				 * to avoid false positives further down
				 */
				g_strfreev(tmp);
				g_slice_free(pattern_struct, psp);
				continue;
			}

			if (led_type == LED_TYPE_NJOY_MONO) {
				strncpy(psp->channel1,
				       tmp[PATTERN_E_CHANNEL_FIELD],
				       CHANNEL_SIZE);
			} else {
				strncpy(psp->channel1,
				       tmp[PATTERN_R_CHANNEL_FIELD],
				       CHANNEL_SIZE);
				strncpy(psp->channel2,
				       tmp[PATTERN_G_CHANNEL_FIELD],
				       CHANNEL_SIZE);
				strncpy(psp->channel3,
				       tmp[PATTERN_B_CHANNEL_FIELD],
				       CHANNEL_SIZE);
			}

			psp->active = FALSE;

			psp->enabled = pattern_get_enabled(patternlist[i],
							   &(psp->gconf_cb_id));

			psp->name = strdup(patternlist[i]);

			g_strfreev(tmp);

			g_queue_insert_sorted(pattern_stack, psp,
					      queue_prio_compare,
					      NULL);
		}
	}

	/* Set the LED brightness */
	execute_datapipe(&led_brightness_pipe,
			 GINT_TO_POINTER(maximum_led_brightness),
			 USE_INDATA, CACHE_INDATA);

	status = TRUE;

EXIT2:
	g_strfreev(patternlist);

EXIT:
	return status;
}

/**
 * Init patterns for direct controlled monochrome LED
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean init_mono_patterns(void)
{
	gchar **patternlist = NULL;
	gboolean status = FALSE;
	gsize length;
	gint i;

	/* Get the list of valid LED patterns */
	patternlist = mce_conf_get_string_list(MCE_CONF_LED_GROUP,
					       MCE_CONF_LED_PATTERNS,
					       &length,
					       NULL);

	/* Treat failed conf-value reads as if they were due to invalid keys
	 * rather than failed allocations; let future allocation attempts fail
	 * instead; otherwise we'll miss the real invalid key failures
	 */
	if (patternlist == NULL) {
		mce_log(LL_WARN,
			"Failed to configure LED patterns");
		status = TRUE;
		goto EXIT;
	}

	/* Used for single-colour LED patterns */
	for (i = 0; patternlist[i]; i++) {
		gint *tmp;

		mce_log(LL_DEBUG,
			"Getting LED pattern for: %s",
			patternlist[i]);

		tmp = mce_conf_get_int_list(led_pattern_group,
					    patternlist[i],
					    &length,
					    NULL);

		if (tmp != NULL) {
			pattern_struct *psp;

			if (length != NUMBER_OF_PATTERN_FIELDS) {
				mce_log(LL_ERR,
					"Skipping invalid LED-pattern");
				g_free(tmp);
				continue;
			}

			psp = g_slice_new(pattern_struct);

			if (!psp) {
				g_free(tmp);
				goto EXIT2;
			}

			psp->name = strdup(patternlist[i]);
			psp->priority = tmp[PATTERN_PRIO_FIELD];
			psp->policy = tmp[PATTERN_SCREEN_ON_FIELD];
			psp->timeout = tmp[PATTERN_TIMEOUT_FIELD] ? tmp[PATTERN_TIMEOUT_FIELD] : -1;
			psp->on_period = tmp[PATTERN_ON_PERIOD_FIELD];
			psp->off_period = tmp[PATTERN_OFF_PERIOD_FIELD];
			psp->brightness = tmp[PATTERN_BRIGHTNESS_FIELD];
			psp->active = FALSE;

			psp->enabled = pattern_get_enabled(patternlist[i],
							   &(psp->gconf_cb_id));

			g_free(tmp);

			g_queue_insert_sorted(pattern_stack, psp,
					      queue_prio_compare,
					      NULL);
		}
	}

	status = TRUE;

EXIT2:
	g_strfreev(patternlist);

EXIT:
	return status;
}

/**
 * Init patterns for the LED
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean init_patterns(void)
{
	gboolean status;

	switch (get_led_type()) {
	case LED_TYPE_LYSTI_MONO:
	case LED_TYPE_LYSTI_RGB:
		status = init_lysti_patterns();
		break;

	case LED_TYPE_NJOY_MONO:
	case LED_TYPE_NJOY_RGB:
		status = init_njoy_patterns();
		break;

	case LED_TYPE_DIRECT_MONO:
		status = init_mono_patterns();
		break;

	default:
		status = TRUE;
		break;
	}

	return status;
}

/**
 * Init function for the LED logic module
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
	gchar *status = NULL;

	(void)module;

	/* Append triggers/filters to datapipes */
	append_output_trigger_to_datapipe(&system_state_pipe,
					  system_state_trigger);
	append_output_trigger_to_datapipe(&display_state_pipe,
					  display_state_trigger);
	append_output_trigger_to_datapipe(&led_brightness_pipe,
					  led_brightness_trigger);
	append_output_trigger_to_datapipe(&led_pattern_activate_pipe,
					  led_pattern_activate_trigger);
	append_output_trigger_to_datapipe(&led_pattern_deactivate_pipe,
					  led_pattern_deactivate_trigger);

	/* Setup a pattern stack,
	 * a combination rule stack and a cross-refernce for said stack
	 * and initialise the patterns
	 */
	pattern_stack = g_queue_new();
	combination_rule_list = g_queue_new();
	combination_rule_xref_list = g_queue_new();

	if (init_patterns() == FALSE)
		goto EXIT;

	/* req_led_pattern_activate */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_ACTIVATE_LED_PATTERN,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 led_activate_pattern_dbus_cb) == NULL)
		goto EXIT;

	/* req_led_pattern_deactivate */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_DEACTIVATE_LED_PATTERN,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 led_deactivate_pattern_dbus_cb) == NULL)
		goto EXIT;

	/* req_led_enable */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_ENABLE_LED,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 led_enable_dbus_cb) == NULL)
		goto EXIT;

	/* req_led_disable */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_DISABLE_LED,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 led_disable_dbus_cb) == NULL)
		goto EXIT;

	led_enable();

EXIT:
	return status;
}

/**
 * Exit function for the LED logic module
 *
 * @todo D-Bus unregistration
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);

	(void)module;

	/* Close files */
	mce_close_file(led_current_rm_path, &led_current_rm_fp);
	mce_close_file(led_current_g_path, &led_current_g_fp);
	mce_close_file(led_current_b_path, &led_current_b_fp);

	mce_close_file(led_brightness_rm_path, &led_brightness_rm_fp);
	mce_close_file(led_brightness_g_path, &led_brightness_g_fp);
	mce_close_file(led_brightness_b_path, &led_brightness_b_fp);

	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&led_pattern_deactivate_pipe,
					    led_pattern_deactivate_trigger);
	remove_output_trigger_from_datapipe(&led_pattern_activate_pipe,
					    led_pattern_activate_trigger);
	remove_output_trigger_from_datapipe(&led_brightness_pipe,
					    led_brightness_trigger);
	remove_output_trigger_from_datapipe(&display_state_pipe,
					    display_state_trigger);
	remove_output_trigger_from_datapipe(&system_state_pipe,
					    system_state_trigger);

	/* Don't disable the LED on shutdown/reboot/acting dead */
	if ((system_state != MCE_STATE_ACTDEAD) &&
	    (system_state != MCE_STATE_SHUTDOWN) &&
	    (system_state != MCE_STATE_REBOOT)) {
		led_disable();
	}

	/* Free path strings; this has to be done after led_disable(),
	 * since it uses these paths
	 */
	g_free(led_current_rm_path);
	g_free(led_current_g_path);
	g_free(led_current_b_path);

	g_free(led_brightness_rm_path);
	g_free(led_brightness_g_path);
	g_free(led_brightness_b_path);

	g_free(engine1_mode_path);
	g_free(engine2_mode_path);
	g_free(engine3_mode_path);

	g_free(engine1_load_path);
	g_free(engine2_load_path);
	g_free(engine3_load_path);

	g_free(engine1_leds_path);
	g_free(engine2_leds_path);
	g_free(engine3_leds_path);

	/* Free the pattern stack */
	if (pattern_stack != NULL) {
		pattern_struct *psp;

		while ((psp = g_queue_pop_head(pattern_stack)) != NULL) {
			mce_gconf_notifier_remove(GINT_TO_POINTER(psp->gconf_cb_id), NULL);
			g_free(psp->name);
			psp->name = NULL;
			g_slice_free(pattern_struct, psp);
		}

		g_queue_free(pattern_stack);
		pattern_stack = NULL;
	}

	/* Free the combination rule list */
	if (combination_rule_list != NULL) {
		combination_rule_struct *cr;

		while ((cr = g_queue_pop_head(combination_rule_list)) != NULL) {
			gchar *tmp;

			while ((tmp = g_queue_pop_head(cr->pre_requisites)) != NULL) {
				g_free(tmp);
				tmp = NULL;
			}

			g_queue_free(cr->pre_requisites);
			cr->pre_requisites = NULL;
			g_slice_free(combination_rule_struct, cr);
		}

		g_queue_free(combination_rule_list);
		combination_rule_list = NULL;
	}

	/* Free the combination rule cross reference list */
	if (combination_rule_xref_list != NULL) {
		combination_rule_struct *xrf;

		while ((xrf = g_queue_pop_head(combination_rule_xref_list)) != NULL) {
			g_queue_free(xrf->pre_requisites);
			xrf->pre_requisites = NULL;
			g_slice_free(combination_rule_struct, xrf);
		}

		g_queue_free(combination_rule_xref_list);
		combination_rule_xref_list = NULL;
	}

	/* Remove all timer sources */
	cancel_pattern_timeout();

	return;
}
