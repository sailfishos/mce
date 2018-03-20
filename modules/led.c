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

#include "led.h"

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-io.h"
#include "../mce-lib.h"
#include "../mce-hal.h"
#include "../mce-conf.h"
#include "../mce-setting.h"
#include "../mce-dbus.h"
#include "../mce-hbtimer.h"

#ifdef ENABLE_HYBRIS
# include "../mce-hybris.h"
#endif

#include "../libwakelock.h"

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include <sys/time.h>
#include <sys/ioctl.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <mce/dbus-names.h>

#include <gmodule.h>

#if 0 // DEBUG: make all logging from this module "critical"
# undef mce_log
# define mce_log(LEV, FMT, ARGS...) \
	mce_log_file(LL_CRIT, __FILE__, __FUNCTION__, FMT , ## ARGS)
#endif

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
	gint timeout;			/**< Auto-deactivate timeout in seconds */
	mce_hbtimer_t *timeout_id;	/**< Timer for auto-deactivate */
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
	guint setting_id;		/**< Callback ID for GConf entry */
	guint rgb_color;                /**< RGB24 data for libhybris use */
	gboolean undecided;		/**< Flag for policy=6 lock in */
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
#ifdef ENABLE_HYBRIS
	/** Android adaptation via libhybris */
	LED_TYPE_HYBRIS = 6,
#endif
} led_type_t;

/**
 * The configuration group containing the LED pattern
 */
static const gchar *led_pattern_group = NULL;

/** Path to monochrome/red channel LED current path  */
static output_state_t led_current_rm_output =
{
	.context = "led_current_rm",
	.truncate_file = TRUE,
	.close_on_exit = FALSE,
};

/** Path to green channel LED current path */
static output_state_t led_current_g_output =
{
	.context = "led_current_g",
	.truncate_file = TRUE,
	.close_on_exit = FALSE,
};

/** Path to blue channel LED current path */
static output_state_t led_current_b_output =
{
	.context = "led_current_b",
	.truncate_file = TRUE,
	.close_on_exit = FALSE,
};

/** Path to monochrome/red channel LED brightness path  */
static output_state_t led_brightness_rm_output =
{
	.context = "led_brightness_rm",
	.truncate_file = TRUE,
	.close_on_exit = FALSE,
};

/** Path to red channel LED brightness path */
static output_state_t led_brightness_g_output =
{
	.context = "led_brightness_g",
	.truncate_file = TRUE,
	.close_on_exit = FALSE,
};

/** Path to blue channel LED brightness path */
static output_state_t led_brightness_b_output =
{
	.context = "led_brightness_b",
	.truncate_file = TRUE,
	.close_on_exit = FALSE,
};

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

/** Cached display state */
static display_state_t display_state_curr = MCE_DISPLAY_UNDEF;

/** Cached system state */
static system_state_t system_state = MCE_SYSTEM_STATE_UNDEF;

/** Cached led brightness */
static gint led_brightness = 0;

/** Maximum LED brightness
 *
 * The led_brightness_pipe is initialized to maximum_led_brightness
 * value and never modified. There is an ALS based filter for
 * led_brightness_pipe that converts the led brightness profile
 * values [%] into 0 ... maximum_led_brightness range. The latter are
 * then handled by the led_brightness_trigger() function below. */
static guint maximum_led_brightness = MAXIMUM_LYSTI_MONOCHROME_LED_CURRENT;

/* Function prototypes */
static void              disable_reno                   (void);
static led_type_t        get_led_type                   (void);
static gint              queue_find                     (gconstpointer data, gconstpointer userdata);
static gint              queue_prio_compare             (gconstpointer entry1, gconstpointer entry2, gpointer userdata);
static void              lysti_set_brightness           (gint brightness);
static void              njoy_set_brightness            (gint brightness);
static void              mono_set_brightness            (gint brightness);
static void              hybris_set_brightness          (gint brightness);
static void              lysti_disable_led              (void);
static void              njoy_disable_led               (void);
static void              mono_disable_led               (void);
static void              hybris_disable_led             (void);
static void              disable_led                    (void);
static pattern_struct   *led_pattern_create             (void);
static void              led_pattern_delete             (pattern_struct *self);
static void              led_pattern_set_active         (pattern_struct *self, gboolean active);
static bool              led_pattern_should_breathe     (const pattern_struct *self);
static bool              led_pattern_can_breathe        (const pattern_struct *self);
static gboolean          led_pattern_timeout_cb         (gpointer data);
static void              lysti_program_led              (const pattern_struct *const pattern);
static void              njoy_program_led               (const pattern_struct *const pattern);
static void              mono_program_led               (const pattern_struct *const pattern);
static void              hybris_program_led             (const pattern_struct *const pattern);
static void              program_led                    (const pattern_struct *const pattern);
static void              allow_sw_breathing             (bool enable);
static void              led_set_active_pattern         (pattern_struct *pattern);
static gboolean          display_off_p                  (display_state_t state);
static void              led_update_active_pattern      (void);
static pattern_struct   *find_pattern_struct            (const gchar *const name);
static void              update_combination_rule        (gpointer name, gpointer data);
static void              update_combination_rules       (const gchar *const name);
static void              led_activate_pattern           (const gchar *const name);
static void              led_deactivate_pattern         (const gchar *const name);
static void              led_enable                     (void);
static void              led_disable                    (void);
static void              system_state_trigger           (gconstpointer data);
static void              get_monotime                   (struct timeval *tv);
static void              type6_lock_in_cb               (void *data, void *aptr);
static void              type6_revert_cb                (void *data, void *aptr);
static void              type6_deactivate_cb            (void *data, void *aptr);
static void              led_pattern_op                 (GFunc cb);
static void              user_activity_event_trigger    (gconstpointer data);
static void              display_state_curr_trigger     (gconstpointer data);
static void              led_brightness_trigger         (gconstpointer data);
static void              led_pattern_activate_trigger   (gconstpointer data);
static void              led_pattern_deactivate_trigger (gconstpointer data);
static gint              setting_id_find                (gconstpointer data, gconstpointer userdata);
static void              led_setting_cb                 (GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);
static gboolean          pattern_get_enabled            (const gchar *const patternname, guint *setting_id);
static gboolean          led_activate_pattern_dbus_cb   (DBusMessage *const msg);
static gboolean          led_deactivate_pattern_dbus_cb (DBusMessage *const msg);
static gboolean          led_enable_dbus_cb             (DBusMessage *const msg);
static gboolean          led_disable_dbus_cb            (DBusMessage *const msg);
static gboolean          init_combination_rules         (void);
static gboolean          init_lysti_patterns            (void);
static gboolean          init_njoy_patterns             (void);
static gboolean          init_mono_patterns             (void);
static int               list_compare_item              (const void *a, const void *b);
static void              list_remove_duplicates         (gchar **list);
static gboolean          list_includes_item             (gchar **list, const gchar *elem);
static gboolean          init_hybris_patterns           (void);
static gboolean          init_patterns                  (void);
static void              sw_breathing_rethink           (void);
static void              sw_breathing_setting_cb        (GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);
static void              sw_breathing_quit              (void);
static void              sw_breathing_init              (void);
static void              charger_state_trigger          (gconstpointer data);
static void              battery_level_trigger          (gconstpointer data);
static void              mce_led_init_dbus              (void);
static void              mce_led_quit_dbus              (void);

G_MODULE_EXPORT const gchar *g_module_check_init        (GModule *module);
G_MODULE_EXPORT void         g_module_unload            (GModule *module);

/**
 * Disable the Reno LED controller
 */
static void disable_reno(void)
{
	int fd;

	if (access("/dev/i2c-0", F_OK) == 0) {
		mce_log(LL_DEBUG, "Skipping Reno disable - suitable kernel detected");
		/* Reset errno,
		 * to avoid false positives down the line
		 */
		errno = 0;
		return;
	}

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

	struct i2c_smbus_ioctl_data args;
	union i2c_smbus_data data;

	data.byte = LEDC_DISABLE;
	args.read_write = I2C_SMBUS_WRITE;
	args.command = LED_DRIVER_CTRL;
	args.size = I2C_SMBUS_BYTE_DATA;
	args.data = &data;

	if (ioctl(fd, I2C_SMBUS, &args) == -1) {
		mce_log(LL_ERR,
			"ioctl() I2C_SMBUS (write LED_DRIVER_CTRL %d) failed on `%s'; %s",
			LEDC_DISABLE, "/dev/i2c-1", g_strerror(errno));
		errno = 0;
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
	product_id_t product_id = PRODUCT_UNKNOWN;
	static led_type_t led_type = LED_TYPE_UNSET;

	/* If we have the LED type already, return it */
	if (led_type != LED_TYPE_UNSET)
		goto EXIT;

#ifdef ENABLE_HYBRIS
	/* Use mce-plugin-libhybris if available */
	if( mce_hybris_indicator_init() ) {
		led_type = LED_TYPE_HYBRIS;
		led_pattern_group = MCE_CONF_LED_PATTERN_HYBRIS_GROUP;
		maximum_led_brightness = MAXIMUM_HYBRIS_LED_BRIGHTNESS;
		goto DONE;
	}
#endif

	/* Otherwise use product id for determining led type */
	product_id = get_product_id();

	// FIXME: The code below is defunct as get_product_id()
	//        does not work without sysinfod.

	/* First build the paths needed to check */
	switch ( product_id ) {
	case PRODUCT_RM716:
	case PRODUCT_RM696:
		led_type = LED_TYPE_NJOY_MONO;
		led_pattern_group = MCE_CONF_LED_PATTERN_RM696_GROUP;
		maximum_led_brightness = MAXIMUM_NJOY_MONOCHROME_LED_CURRENT;

		/* Build paths */
		led_current_rm_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_CURRENT_SUFFIX, NULL);
		led_brightness_rm_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_BRIGHTNESS_SUFFIX, NULL);

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
		led_current_rm_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL8, MCE_LED_CURRENT_SUFFIX, NULL);
		led_brightness_rm_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL8, MCE_LED_BRIGHTNESS_SUFFIX, NULL);

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
		led_current_rm_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_CURRENT_SUFFIX, NULL);
		led_current_g_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL1, MCE_LED_CURRENT_SUFFIX, NULL);
		led_current_b_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL2, MCE_LED_CURRENT_SUFFIX, NULL);
		led_brightness_rm_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL0, MCE_LED_BRIGHTNESS_SUFFIX, NULL);
		led_brightness_g_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL1, MCE_LED_BRIGHTNESS_SUFFIX, NULL);
		led_brightness_b_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5523_PREFIX, MCE_LED_CHANNEL2, MCE_LED_BRIGHTNESS_SUFFIX, NULL);

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
		led_current_rm_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_CURRENT_SUFFIX, NULL);
		led_brightness_rm_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_LP5521_PREFIX, MCE_LED_CHANNEL0, MCE_LED_BRIGHTNESS_SUFFIX, NULL);

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
		led_brightness_rm_output.path = g_strconcat(MCE_LED_DIRECT_SYS_PATH, MCE_LED_KEYPAD_PREFIX, MCE_LED_BRIGHTNESS_SUFFIX, NULL);
		break;

	default:
		led_type = LED_TYPE_NONE;
		break;
	}

#ifdef ENABLE_HYBRIS
DONE:
#endif
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
		mce_write_number_string_to_file(&led_current_rm_output, r_brightness);

		mce_log(LL_DEBUG,
			"Brightness set to %d",
			active_brightness);
	} else if (get_led_type() == LED_TYPE_LYSTI_RGB) {
		/* If we have an RGB LED set the brightness for all channels */
		mce_write_number_string_to_file(&led_current_rm_output, r_brightness);
		mce_write_number_string_to_file(&led_current_g_output, g_brightness);
		mce_write_number_string_to_file(&led_current_b_output, b_brightness);

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

	mce_write_number_string_to_file(&led_brightness_rm_output,
					(unsigned)active_brightness);

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
	mce_write_string_to_file(led_brightness_rm_output.path,
				 brightness_map[brightness]);

	mce_log(LL_DEBUG, "Brightness set to %d", brightness);
}

#ifdef ENABLE_HYBRIS
static void hybris_program_led(const pattern_struct *const pattern);

/**
 * Set libhybris-LED brightness
 *
 * @param brightness The brightness of the LED
 *                   (0 - maximum_led_brightness),
 *                   or -1 to reset brightness when the LED has been disabled
 */
static void hybris_set_brightness(gint brightness)
{
	if (brightness < -1 || brightness > (gint)maximum_led_brightness) {
		mce_log(LL_WARN, "Invalid brightness value %d", brightness);
		return;
	}

	if( active_brightness == brightness )
		return;

	if( brightness != -1 )
		active_brightness = brightness;

	mce_log(LL_DEBUG, "Brightness set to %d", active_brightness);

	/* Scale from [1...100%] to [1...255] range */
	brightness = mce_xlat_int(1,maximum_led_brightness, 1,255, brightness);
	mce_hybris_indicator_set_brightness(brightness);
}
#endif /* ENABLE_HYBRIS */

/**
 * Disable the Lysti-LED
 */
static void lysti_disable_led(void)
{
	/* Disable engine 1 */
	mce_write_string_to_file(engine1_mode_path,
				 MCE_LED_DISABLED_MODE);

	if (get_led_type() == LED_TYPE_LYSTI_MONO) {
		/* Turn off the led */
		mce_write_number_string_to_file(&led_brightness_rm_output, 0);
	} else if (get_led_type() == LED_TYPE_LYSTI_RGB) {
		/* Disable engine 2 */
		mce_write_string_to_file(engine2_mode_path,
					 MCE_LED_DISABLED_MODE);

		/* Turn off all three leds */
		mce_write_number_string_to_file(&led_brightness_rm_output, 0);
		mce_write_number_string_to_file(&led_brightness_g_output, 0);
		mce_write_number_string_to_file(&led_brightness_b_output, 0);
	}
}

/**
 * Disable the NJoy-LED
 */
static void njoy_disable_led(void)
{
	/* Disable engine 1 */
	mce_write_string_to_file(engine1_mode_path,
				 MCE_LED_DISABLED_MODE);

	if (get_led_type() == LED_TYPE_NJOY_MONO) {
		/* Turn off the led */
		mce_write_number_string_to_file(&led_brightness_rm_output, 0);
	} else if (get_led_type() == LED_TYPE_NJOY_RGB) {
		/* Disable engine 2 */
		mce_write_string_to_file(engine2_mode_path,
					 MCE_LED_DISABLED_MODE);

		/* Disable engine 3 */
		mce_write_string_to_file(engine3_mode_path,
					 MCE_LED_DISABLED_MODE);

		/* Turn off all three leds */
		mce_write_number_string_to_file(&led_brightness_rm_output, 0);
		mce_write_number_string_to_file(&led_brightness_g_output, 0);
		mce_write_number_string_to_file(&led_brightness_b_output, 0);
	}
}

/**
 * Disable the mono-LED
 */
static void mono_disable_led(void)
{
	mce_write_string_to_file(MCE_LED_TRIGGER_PATH,
				 MCE_LED_TRIGGER_NONE);
	mono_set_brightness(0);
}

#ifdef ENABLE_HYBRIS
/** Disable the libhybris-LED
 */
static void hybris_disable_led(void)
{
	mce_hybris_indicator_set_pattern(0,0,0, 0,0);
}
#endif /* ENABLE_HYBRIS */

/**
 * Disable the LED
 */
static void disable_led(void)
{
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

#ifdef ENABLE_HYBRIS
	case LED_TYPE_HYBRIS:
		hybris_disable_led();
		break;
#endif

	default:
		break;
	}
}

/** Allocate and initialize led pattern object
 *
 * @return initialzied led pattern object, or NULL
 */
static pattern_struct *led_pattern_create(void)
{
	pattern_struct *self = g_slice_new0(pattern_struct);

	if( !self )
		goto EXIT;

	self->name       = 0;
	self->timeout_id = 0;
	self->setting_id = 0;

EXIT:
	return self;
}

/** Destroy led pattern object
 *
 * @param initialzied led pattern object, or NULL
 */
static void led_pattern_delete(pattern_struct *self)
{
	if( !self )
		goto EXIT;

	mce_hbtimer_delete(self->timeout_id);
	mce_setting_notifier_remove(self->setting_id);
	free(self->name);

	g_slice_free(pattern_struct, self);

EXIT:
	return;
}

/** Setter for led pattern active property
 *
 * Apart from initialization to FALSE state, all active
 * property changes must go through this function.
 *
 * If the active property actually changes and the pattern
 * is not disabled an appropriate D-Bus signal is broadcast
 * over the system bus.
 *
 * @param self    pattern object
 * @param active  new value for active property
 */
static void led_pattern_set_active(pattern_struct *self, gboolean active)
{
	DBusMessage *msg = NULL;

	if( !self )
		goto EXIT;

	if( self->active == active )
		goto EXIT;

	self->active = active;

	if( !self->enabled )
		goto EXIT;

	if( self->active )
		mce_hbtimer_start(self->timeout_id);
	else
		mce_hbtimer_stop(self->timeout_id);

	mce_log(LL_DEVEL, "led pattern %s %sactivated",
		self->name, self->active ? "" : "de");

	const char *member = (self->active ?
			      MCE_LED_PATTERN_ACTIVATED_SIG :
			      MCE_LED_PATTERN_DEACTIVATED_SIG);

	msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF, member);

	if( !dbus_message_append_args(msg,
				     DBUS_TYPE_STRING, &self->name,
				     DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "failed to construct %s signal", member);
		goto EXIT;
	}

	dbus_send_message(msg), msg = 0;

EXIT:
	if( msg )
		dbus_message_unref(msg);

	return;
}

/** Check if a led pattern should always utilize sw breathing
 *
 * @param self led pattern object
 *
 * @return true if the pattern should always breathe, false otherwise
 */
static bool led_pattern_should_breathe(const pattern_struct *self)
{
	static const char * const lut[] =
	{
		/* Battery full breathes by default. If user has tuned
		 * the pattern config to disable battery full blinking,
		 * the led_pattern_can_breathe() should catch it. */
		MCE_LED_PATTERN_BATTERY_FULL,

		/* The CSD test has some led patterns that should utilize
		 * breathing regardless of the breathing settings and/or
		 * charging status. */
		MCE_LED_PATTERN_CSD_BINARY_BLINK,
		MCE_LED_PATTERN_CSD_WHITE_BLINK,
	};

	bool breathe = false;

	if( !self || !self->name )
		goto EXIT;

	for( size_t i = 0; i < G_N_ELEMENTS(lut); ++i ) {
		if( strcmp(self->name, lut[i]) )
			continue;

		breathe = true;
		break;
	}

EXIT:
	return breathe;
}

/** Check if pattern is breathable
 *
 * @param self led pattern object
 *
 * @return true if pattern should be breathed, false otherwise
 */
static bool led_pattern_can_breathe(const pattern_struct *self)
{
	/* FIXME: This should be directly available in the pattern
	 *        configuration. But until we know better what is
	 *        needed and how to configure it, heuristics are
	 *        used to determine whether a pattern should be
	 *        turned in to breathing kind or not. */

	/* Assume no pattern is breathable */
	bool breathe = false;

	/* What we want to breathe are the normal blinking indicator
	 * patterns. By default these have the following characteristics
	 *  - "on_period"  = 500 ms
	 *  - "off_period" = 1500 ... 2500 ms
	 *
	 * Extend these bounds in case the users have edited the
	 * defaults, or added new patterns.
	 */

	int normal_pattern_minimum_on_period  =  250; // [ms]
	int normal_pattern_maximum_on_period  = 1500; // [ms]

	int normal_pattern_minimum_off_period =  250; // [ms]
	int normal_pattern_maximum_off_period = 5000; // [ms]

	/* Then assume anything out of those limits probably
	 * a) is unbreathable static pattern
	 * b) is rapid panic pattern
	 * c) is custom beacon with short on, long off cycle
	 * d) has too short rise time for timer based adjustments
	 * e) has so long fall time that breathing is unnoticeable
	 *    and should not be made to breathe.
	 */

	if( self->on_period  < normal_pattern_minimum_on_period  ||
	    self->on_period  > normal_pattern_maximum_on_period  ||
	    self->off_period < normal_pattern_minimum_off_period ||
	    self->off_period > normal_pattern_maximum_off_period )
		goto EXIT;

	/* There is no reason not to breathe */
	breathe = true;

EXIT:

	return breathe;
}

/** Timeout callback for LED patterns
 *
 * @param data led pattern object
 *
 * @return Always returns FALSE to disable timeout
 */
static gboolean led_pattern_timeout_cb(gpointer data)
{
	pattern_struct *psp = data;

	led_pattern_set_active(psp, FALSE);
	led_update_active_pattern();

	return FALSE;
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
	mce_write_string_to_file(engine1_mode_path,
				 MCE_LED_LOAD_MODE);
	mce_write_string_to_file(engine1_leds_path,
				 bin_to_string(pattern->engine1_mux));
	mce_write_string_to_file(engine1_load_path,
				 pattern->channel1);

	/* Engine 2; if needed */
	if (get_led_type() == LED_TYPE_LYSTI_RGB) {
		mce_write_string_to_file(engine2_mode_path,
					 MCE_LED_LOAD_MODE);
		mce_write_string_to_file(engine2_leds_path,
					 bin_to_string(pattern->engine2_mux));
		mce_write_string_to_file(engine2_load_path,
					 pattern->channel2);

		/* Run the new pattern; enable engines in reverse order */
		mce_write_string_to_file(engine2_mode_path,
					 MCE_LED_RUN_MODE);
	}

	mce_write_string_to_file(engine1_mode_path,
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
	mce_write_string_to_file(engine1_mode_path,
				 MCE_LED_LOAD_MODE);
	mce_write_string_to_file(engine1_load_path,
				 pattern->channel1);

	if (get_led_type() == LED_TYPE_NJOY_RGB) {
		/* Engine 2 */
		mce_write_string_to_file(engine2_mode_path,
					 MCE_LED_LOAD_MODE);
		mce_write_string_to_file(engine2_load_path,
					 pattern->channel2);

		/* Engine 3 */
		mce_write_string_to_file(engine3_mode_path,
					 MCE_LED_LOAD_MODE);
		mce_write_string_to_file(engine3_load_path,
					 pattern->channel3);

		/* Run the new pattern; enable engines in reverse order */
		mce_write_string_to_file(engine3_mode_path,
					 MCE_LED_RUN_MODE);
		mce_write_string_to_file(engine2_mode_path,
					 MCE_LED_RUN_MODE);
	}

	mce_write_string_to_file(engine1_mode_path,
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
	static output_state_t led_on_period_output =
	{
		.context = "led_on_period",
		.truncate_file = TRUE,
		.close_on_exit = TRUE,
		.path = MCE_LED_ON_PERIOD_PATH,
	};
	static output_state_t led_off_period_output =
	{
		.context = "led_off_period",
		.truncate_file = TRUE,
		.close_on_exit = TRUE,
		.path = MCE_LED_OFF_PERIOD_PATH,
	};

	/* This shouldn't happen; disable the LED instead */
	if (pattern->on_period == 0) {
		mono_disable_led();
		goto EXIT;
	}

	/* If we have a normal, on/off pattern,
	 * use a timer trigger, otherwise disable the trigger
	 */
	if (pattern->off_period != 0) {
		mce_write_string_to_file(MCE_LED_TRIGGER_PATH,
					 MCE_LED_TRIGGER_TIMER);
		mce_write_number_string_to_file(&led_off_period_output,
						(unsigned)pattern->off_period);
		mce_write_number_string_to_file(&led_on_period_output,
						(unsigned)pattern->on_period);
	} else {
		mce_write_string_to_file(MCE_LED_TRIGGER_PATH,
					 MCE_LED_TRIGGER_NONE);
	}

	mono_set_brightness(pattern->brightness);

EXIT:
	return;
}

#ifdef ENABLE_HYBRIS
/**
 * Setup and activate a new libhybris-LED pattern
 *
 * @param pattern A pointer to a pattern_struct with the new pattern
 */
static void hybris_program_led(const pattern_struct *const pattern)
{
	int r = (pattern->rgb_color >> 16) & 0xff;
	int g = (pattern->rgb_color >>  8) & 0xff;
	int b = (pattern->rgb_color >>  0) & 0xff;

	mce_hybris_indicator_set_pattern(r, g, b,
					 pattern->on_period,
					 pattern->off_period);
}
#endif /* ENABLE_HYBRIS */

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

#ifdef ENABLE_HYBRIS
	case LED_TYPE_HYBRIS:
		hybris_program_led(pattern);
		break;
#endif

	default:
		break;
	}
}

/** Enable/disable led breathing via software
 *
 * @param pattern A pointer to a pattern_struct with the new pattern
 */
static void allow_sw_breathing(bool enable)
{
	static bool current = false;

	/* If led backend does not support breathing make sure we do
	 * not grab a useless wakelock and block suspend unnecessarily */
	if( !mce_hybris_indicator_can_breathe() )
		enable = false;

	if( current == enable )
		goto EXIT;

	current = enable;

	switch (get_led_type()) {
#ifdef ENABLE_HYBRIS
	case LED_TYPE_HYBRIS:
		if( enable )
			wakelock_lock("mce_led_breathing", -1);
		mce_hybris_indicator_enable_breathing(enable);
		if( !enable )
			wakelock_unlock("mce_led_breathing");
		break;
#endif

	default:
		break;
	}
EXIT:
	return;
}

/** Setter function for active_pattern
 *
 * @param pattern The led pattern to activate, or NULL to disable
 */
static void led_set_active_pattern(pattern_struct *pattern)
{
	if( active_pattern == pattern )
		goto EXIT;

	active_pattern = pattern;

	if( active_pattern ) {
		program_led(active_pattern);
	}
	else {
		disable_led();
	}

	sw_breathing_rethink();
EXIT:
	return;
}

/** Display state is close enough to "off" predicate
 *
 * @param state display state
 *
 * @return TRUE if display is off, otherwise FALSE
 */
static gboolean display_off_p(display_state_t state)
{
	gboolean is_off = TRUE;

	switch( state ) {
	case MCE_DISPLAY_ON:
	case MCE_DISPLAY_DIM:
	case MCE_DISPLAY_UNDEF:
		is_off = FALSE;
		break;

	default:
	case MCE_DISPLAY_OFF:
	case MCE_DISPLAY_LPM_OFF:
	case MCE_DISPLAY_LPM_ON:
	case MCE_DISPLAY_POWER_UP:
	case MCE_DISPLAY_POWER_DOWN:
		break;
	}

	return is_off;
}

/**
 * Recalculate active pattern and update the pattern timer
 */
static void led_update_active_pattern(void)
{
	pattern_struct *new_active_pattern = 0;

	if( !pattern_stack )
		goto EXIT;

	for( GList *iter = pattern_stack->head; ; iter = iter->next ) {
		if( !iter ) {
			new_active_pattern = 0;
			break;
		}

		new_active_pattern = iter->data;

#if 0 /* While this can be useful when actively debugging led
       * activation logic, it creates so much noise that using
       * debug verbosity becomes impossible - do not compile in
       * by default. */
		mce_log(LL_DEBUG,
			"pattern: %s, active: %d, enabled: %d",
			new_active_pattern->name,
			new_active_pattern->active,
			new_active_pattern->enabled);
#endif

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

		/* Show pattern with visibility 7 if display is dimmed */
		if( new_active_pattern->policy == 7 ) {
			if( display_state_curr == MCE_DISPLAY_DIM )
				break;
			continue;
		}

		/* Acting dead behaviour */
		if (system_state == MCE_SYSTEM_STATE_ACTDEAD) {
			/* If we're in acting dead,
			 * show patterns with visibility 4
			 */
			if (new_active_pattern->policy == 4)
				break;

			/* If we're in acting dead
			 * and the display is off, show pattern
			 */
			if (display_off_p(display_state_curr) &&
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
		if( display_off_p(display_state_curr) )
			break;

		/* If the pattern should be shown with screen on, use it */
		if (new_active_pattern->policy == 1)
			break;
	}

EXIT:
	led_set_active_pattern(new_active_pattern);
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

	led_pattern_set_active(psp, enabled);

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
		if( !psp->active && psp->policy == 6 )
			psp->undecided = TRUE;
		led_pattern_set_active(psp, TRUE);
		update_combination_rules(name);
		led_update_active_pattern();
	} else {
		mce_log(LL_DEBUG,
			"Received request to activate "
			"a non-existing LED pattern '%s'", name);
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
		led_pattern_set_active(psp, FALSE);
		update_combination_rules(name);
		led_update_active_pattern();
	} else {
		mce_log(LL_DEBUG,
			"Received request to deactivate "
			"a non-existing LED pattern '%s'", name);
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
	led_update_active_pattern();
}

/**
 * Handle system state change
 *
 * @param data Unused
 */
static void system_state_trigger(gconstpointer data)
{
	system_state_t prev = system_state;
	system_state = GPOINTER_TO_INT(data);

	if( prev == system_state )
		goto EXIT;

	mce_log(LL_DEBUG, "system_state: %s -> %s",
		system_state_repr(prev),
		system_state_repr(system_state));

	led_update_active_pattern();

EXIT:
	return;
}

/** Monotonic time stamp helper
 *
 * @param tv where to store the time stamp
 */
static void get_monotime(struct timeval *tv)
{
	struct timespec ts;

#ifdef CLOCK_BOOTTIME
	if( clock_gettime(CLOCK_BOOTTIME, &ts) == 0 )
		goto CONVERT;
#endif

#ifdef CLOCK_MONOTIME
	if( clock_gettime(CLOCK_MONOTIME, &ts) == 0 )
		goto CONVERT;
#endif
	if( gettimeofday(tv, 0) != 0 )
		timerclear(tv);

	goto EXIT;

CONVERT:
	TIMESPEC_TO_TIMEVAL(tv, &ts);
EXIT:
	return;
}

/** Timestamp for latest user activity */
static struct timeval       activity_time  = { .tv_sec = 0, .tv_usec = 0 };

/** Timelimit for the activity_time to be considered recent */
static const struct timeval activity_limit = { .tv_sec = 2, .tv_usec = 0 };

/** Lock in undecided policy=6 led patterns
 *
 * For use with led_pattern_op()
 */
static void type6_lock_in_cb(void *data, void *aptr)
{
	(void)aptr;

	pattern_struct *psp = data;

	if( psp->undecided && psp->active && psp->policy == 6 ) {
		mce_log(LL_DEBUG, "LED pattern %s: locked in", psp->name);
	}
	psp->undecided = FALSE;
}

/** Revert undecided policy=6 led patterns
 *
 * For use with led_pattern_op()
 */
static void type6_revert_cb(void *data, void *aptr)
{
	(void)aptr;

	pattern_struct *psp = data;

	if( psp->undecided && psp->active && psp->policy == 6 ) {
		led_pattern_set_active(psp, FALSE);
		update_combination_rules(psp->name);
		mce_log(LL_DEBUG, "LED pattern %s: reverted", psp->name);
	}
	psp->undecided = FALSE;

}

/** De-activate all policy=6 led patterns
 *
 * For use with led_pattern_op()
 */
static void type6_deactivate_cb(void *data, void *aptr)
{
	(void)aptr;

	pattern_struct *psp = data;

	if( psp->active && psp->policy == 6 ) {
		led_pattern_set_active(psp, FALSE);
		update_combination_rules(psp->name);
		mce_log(LL_DEBUG, "LED pattern %s: deactivated", psp->name);
	}
	psp->undecided = FALSE;
}

/** Apply callback on all led patterns
 */
static void led_pattern_op(GFunc cb)
{
	g_queue_foreach(pattern_stack, cb, 0);
}

/** Handle real user activity
 *
 * @param data Unused
 */
static void user_activity_event_trigger(gconstpointer data)
{
	(void)data; // the data is irrelevant

	if( display_state_curr == MCE_DISPLAY_ON )
		led_pattern_op(type6_revert_cb);
	get_monotime(&activity_time);
}

/**
 * Handle display state change
 *
 * @param data Unused
 */
static void display_state_curr_trigger(gconstpointer data)
{
	display_state_t prev = display_state_curr;
	display_state_curr = GPOINTER_TO_INT(data);

	struct timeval tv;

	if (prev == display_state_curr)
		goto EXIT;

	mce_log(LL_DEBUG, "display_state_curr: %s -> %s",
		display_state_repr(prev),
		display_state_repr(display_state_curr));

	get_monotime(&tv);
	timersub(&tv, &activity_time, &tv);

	switch( display_state_curr ) {
	case MCE_DISPLAY_ON:
		if( timercmp(&tv, &activity_limit, <) )
			led_pattern_op(type6_deactivate_cb);
		timerclear(&activity_time);
		break;

	case MCE_DISPLAY_OFF:
	case MCE_DISPLAY_LPM_OFF:
	case MCE_DISPLAY_LPM_ON:
		if( timercmp(&tv, &activity_limit, <) )
			led_pattern_op(type6_revert_cb);
		else
			led_pattern_op(type6_lock_in_cb);
		timerclear(&activity_time);
		break;

	default:
	case MCE_DISPLAY_DIM:
	case MCE_DISPLAY_UNDEF:
	case MCE_DISPLAY_POWER_UP:
	case MCE_DISPLAY_POWER_DOWN:
		break;
	}

	led_update_active_pattern();

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
	gint prev = led_brightness;
	led_brightness = GPOINTER_TO_INT(data);

	if( prev == led_brightness )
		goto EXIT;

	mce_log(LL_DEBUG, "led_brightness: %d -> %d",
		prev, led_brightness);

	switch (get_led_type()) {
	case LED_TYPE_LYSTI_RGB:
	case LED_TYPE_LYSTI_MONO:
		lysti_set_brightness(led_brightness);
		break;

	case LED_TYPE_NJOY_RGB:
	case LED_TYPE_NJOY_MONO:
		njoy_set_brightness(led_brightness);
		break;

#ifdef ENABLE_HYBRIS
	case LED_TYPE_HYBRIS:
		hybris_set_brightness(led_brightness);
		break;
#endif

	case LED_TYPE_DIRECT_MONO:
	case LED_TYPE_UNSET:
	case LED_TYPE_NONE:
	default:
		break;
	}

EXIT:
	return;
}

/**
 * Handle LED pattern activate requests
 *
 * @param data The pattern name
 */
static void led_pattern_activate_trigger(gconstpointer data)
{
	const char *name = data;

	/* The datapipe does not have a state, so we need to
	 * ignore null data that shows up on initialization */

	if( name )
		led_activate_pattern(name);
}

/**
 * Handle LED pattern deactivate requests
 *
 * @param data The pattern name
 */
static void led_pattern_deactivate_trigger(gconstpointer data)
{
	const char *name = data;

	/* The datapipe does not have a state, so we need to
	 * ignore null data that shows up on initialization */

	if( name )
		led_deactivate_pattern(data);
}

/**
 * Custom find function to get a GConf callback ID in the pattern stack
 *
 * @param data The pattern_struct entry
 * @param userdata The pattern name
 * @return 0 if the GConf callback id of data matches that of userdata,
 *         -1 if they don't match
 */
static gint setting_id_find(gconstpointer data, gconstpointer userdata)
{
	pattern_struct *psp;

	if ((data == NULL) || (userdata == NULL))
		return -1;

	psp = (pattern_struct *)data;

	return psp->setting_id != *(guint *)userdata;
}

/**
 * GConf callback for LED related settings
 *
 * @param gcc Unused
 * @param id Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data Unused
 */
static void led_setting_cb(GConfClient *const gcc, const guint id,
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
				       &id, setting_id_find)) != NULL) {
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
				    guint *setting_id)
{
	gboolean retval = MCE_DEFAULT_LED_PATTERN_ENABLED;
	gchar *path = gconf_concat_dir_and_key(MCE_SETTING_LED_PATH,
					       patternname);

	/* Since custom led patterns do not have persistent toggles
	 * in configuration, avoid complaining about missing keys
	 * on default verbosity level. */
	if( !mce_setting_has_key(path) ) {
		mce_log(LL_INFO, "missing led config entry: %s", path);
		goto EXIT;
	}

	/* Since we've set a default, error handling is unnecessary */
	mce_setting_notifier_add(MCE_SETTING_LED_PATH, path,
				 led_setting_cb, setting_id);
	mce_setting_get_bool(path, &retval);

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
	DBusError error = DBUS_ERROR_INIT;

	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &pattern,
				  DBUS_TYPE_INVALID) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_CRIT,
			"Failed to get argument from %s.%s: %s",
			MCE_REQUEST_IF, MCE_ACTIVATE_LED_PATTERN,
			error.message);
		goto EXIT;
	}

	mce_log(LL_DEVEL, "activate LED pattern %s request from %s",
		pattern, mce_dbus_get_message_sender_ident(msg));

	led_activate_pattern(pattern);

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

EXIT:
	dbus_error_free(&error);
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
	DBusError error = DBUS_ERROR_INIT;

	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &pattern,
				  DBUS_TYPE_INVALID) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_CRIT,
			"Failed to get argument from %s.%s: %s",
			MCE_REQUEST_IF, MCE_DEACTIVATE_LED_PATTERN,
			error.message);
		goto EXIT;
	}

	mce_log(LL_DEVEL, "de-activate LED pattern %s request from %s",
		pattern, mce_dbus_get_message_sender_ident(msg));

	led_deactivate_pattern(pattern);

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

EXIT:
	dbus_error_free(&error);
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

	mce_log(LL_DEVEL, "Received LED enable request from %s",
		mce_dbus_get_message_sender_ident(msg));

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

	mce_log(LL_DEVEL, "Received LED disable request from %s",
		mce_dbus_get_message_sender_ident(msg));

	led_disable();

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
					  &length);

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
					       &length);

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
					       MCE_CONF_LED_PATTERNS_REQUIRED,
					       &length);

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
					       &length);

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

			psp = led_pattern_create();

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
				led_pattern_delete(psp);
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

			led_pattern_set_active(psp, FALSE);

			psp->enabled = pattern_get_enabled(patternlist[i],
							   &psp->setting_id);

			psp->name = strdup(patternlist[i]);

			g_strfreev(tmp);

			g_queue_insert_sorted(pattern_stack, psp,
					      queue_prio_compare,
					      NULL);
		}
	}

	init_combination_rules();

	/* Set the LED brightness */
	datapipe_exec_full(&led_brightness_pipe,
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
					       MCE_CONF_LED_PATTERNS_REQUIRED,
					       &length);

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
					       &length);

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

			psp = led_pattern_create();

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
				led_pattern_delete(psp);
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

			led_pattern_set_active(psp, FALSE);

			psp->enabled = pattern_get_enabled(patternlist[i],
							   &psp->setting_id);

			psp->name = strdup(patternlist[i]);

			g_strfreev(tmp);

			g_queue_insert_sorted(pattern_stack, psp,
					      queue_prio_compare,
					      NULL);
		}
	}

	/* Set the LED brightness */
	datapipe_exec_full(&led_brightness_pipe,
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
					       MCE_CONF_LED_PATTERNS_REQUIRED,
					       &length);

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
					    &length);

		if (tmp != NULL) {
			pattern_struct *psp;

			if (length != NUMBER_OF_PATTERN_FIELDS) {
				mce_log(LL_ERR,
					"Skipping invalid LED-pattern");
				g_free(tmp);
				continue;
			}

			psp = led_pattern_create();

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
							   &psp->setting_id);

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

#ifdef ENABLE_HYBRIS
/** Compare operator for sorting arrays of led pattern names
 *
 * @param a pointer to 1st string pointer
 * @param b pointer to 2nd string pointer
 *
 * @return negative, zero or positive if a<b, a==b, or a>b
 */
static int list_compare_item(const void *a, const void *b)
{
	return strcmp(*(const char **)a, *(const char **)b);
}

/** Sort array of led pattern names and remove duplicates
 *
 * @param list array of led pattern names
 */
static void list_remove_duplicates(gchar **list)
{
	size_t i,k,n;
	gchar *s;

	if( !list )
		goto EXIT;

	/* remove empty strings */
	for( n = 0, k = 0; (s = list[k]); ++k ) {
		if( *s )
			list[n++] = s;
		else
			g_free(s);
	}
	list[n] = 0;

	if( n < 2 )
		goto EXIT;

	/* sort elements */
	qsort(list, n, sizeof *list, list_compare_item);

	/* remove duplicate entries */
	for( i = 0, k = 1; k < n; ++k ) {
		s = list[k];
		if( strcmp(list[i], s) )
			list[++i] = s;
		else
			g_free(s);
	}
	list[i+1] = 0;
EXIT:
	return;
}

/** Name exists in array of led pattern names predicate
 *
 * @param list array of led pattern names
 * @param elem led pattern name
 *
 * @return TRUE if elem is in the list, FALSE otherwise
 */
static gboolean list_includes_item(gchar **list, const gchar *elem)
{
	if( !list || !elem )
		return FALSE;

	for( size_t i = 0; list[i]; ++i ) {
		if( !strcmp(list[i], elem) )
			return TRUE;
	}

	return FALSE;
}

/**
 * Init patterns for libhybris-LED
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean init_hybris_patterns(void)
{
	enum {
		IDX_PRIO,       /* Pattern priority field */
		IDX_SCREEN_ON,  /* Pattern screen display policy field */
		IDX_TIMEOUT,    /* Pattern timeout field */
		IDX_ON_PERIOD,  /* On-period field */
		IDX_OFF_PERIOD, /* Off-period field */
		IDX_COLOR,      /* LED color field */
		IDX_NUMOF
	};

	gboolean  status  = FALSE;
	gchar   **require = NULL;
	gchar   **disable = NULL;
	gchar   **pattern = NULL;

	/* Get the list of required LED patterns */
	require = mce_conf_get_string_list(MCE_CONF_LED_GROUP,
					   MCE_CONF_LED_PATTERNS_REQUIRED, 0);
	list_remove_duplicates(require);

	/* Get the list of disabled LED patterns */
	disable = mce_conf_get_string_list(MCE_CONF_LED_GROUP,
					   MCE_CONF_LED_PATTERNS_DISABLED, 0);
	list_remove_duplicates(disable);

	/* Get the list of configured patterns */
	pattern = mce_conf_get_keys(led_pattern_group, 0);
	list_remove_duplicates(pattern);

	if( !pattern || !*pattern ) {
		mce_log(LL_WARN, "No LED patterns configured");
		goto EXIT;
	}

	/* Check if we have data for required patterns */
	if( require && *require ) {
		for( size_t i = 0; require[i]; ++i ) {
			if( !list_includes_item(pattern, require[i]) )
				mce_log(LL_WARN, "Required LED pattern "
					"'%s' not defined", require[i]);
		}
	}

	for( size_t i = 0; pattern[i]; i++ ) {
		const char *name = pattern[i];
		if( list_includes_item(disable, name) ) {
			mce_log(LL_NOTICE,"LED pattern '%s' disabled", name);
			continue;
		}

		gsize length = 0;
		gchar **v = mce_conf_get_string_list(led_pattern_group,
						     name, &length);
		if( !v ) {
			mce_log(LL_WARN,"LED pattern '%s' not configured",
				name);
		}
		else if( length != IDX_NUMOF ) {
			mce_log(LL_ERR,"LED pattern '%s' is invalid",
				name);
		}
		else {
			mce_log(LL_DEBUG,"Getting LED pattern for: %s",
				name);

			pattern_struct *psp = led_pattern_create();

			psp->name       = strdup(name);
			psp->priority   = strtol(v[IDX_PRIO], 0, 0);
			psp->policy     = strtol(v[IDX_SCREEN_ON], 0, 0);
			psp->timeout    = strtol(v[IDX_TIMEOUT], 0, 0) ?: -1;
			psp->on_period  = strtol(v[IDX_ON_PERIOD], 0, 0);
			psp->off_period = strtol(v[IDX_OFF_PERIOD], 0, 0);
			psp->rgb_color  = strtol(v[IDX_COLOR], 0, 16);
			psp->active     = FALSE;
			psp->enabled    = pattern_get_enabled(name,
							      &psp->setting_id);

			g_queue_insert_sorted(pattern_stack, psp,
					      queue_prio_compare,
					      NULL);
		}
		g_strfreev(v);
	}

	init_combination_rules();

	/* Set the LED brightness */
	datapipe_exec_full(&led_brightness_pipe,
			   GINT_TO_POINTER(maximum_led_brightness),
			   USE_INDATA, CACHE_INDATA);

	status = TRUE;

EXIT:
	g_strfreev(pattern);
	g_strfreev(disable);
	g_strfreev(require);

	return status;
}
#endif /* ENABLE_HYBRIS */

/**
 * Init patterns for the LED
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean init_patterns(void)
{
	gboolean status = TRUE;

	/* Type specific pattern configuration */
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

#ifdef ENABLE_HYBRIS
	case LED_TYPE_HYBRIS:
		status = init_hybris_patterns();
		break;
#endif

	default:
		break;
	}

	/* Handle common pattern initialization */
	for( GList *iter = pattern_stack->head; iter; iter = iter->next ) {
		pattern_struct *psp = iter->data;

		/* Add hbtimers for patterns that use timeout */
		if( psp->timeout > 0 ) {
			psp->timeout_id =
				mce_hbtimer_create(psp->name,
						   psp->timeout * 1000,
						   led_pattern_timeout_cb,
						   psp);
		}
	}

	return status;
}

/** Flag for: charger connected */
static charger_state_t charger_state = CHARGER_STATE_UNDEF;

/** Current battery percent level */
static int battery_level = 0;

/** Setting: sw breathing allowed */
static gboolean sw_breathing_enabled = MCE_DEFAULT_LED_SW_BREATH_ENABLED;
static guint    sw_breathing_enabled_setting_id = 0;

/** Setting: battery level at which sw breathing is disabled */
static gint  sw_breathing_battery_limit = MCE_DEFAULT_LED_SW_BREATH_BATTERY_LIMIT;
static guint sw_breathing_battery_limit_setting_id = 0;

/** Re-evaluate sw breathing enable state
 */
static void sw_breathing_rethink(void)
{
	bool breathe = false;

	/* Check breathing configuration */
	if( sw_breathing_enabled ) {
		breathe = (charger_state == CHARGER_STATE_ON ||
			   battery_level >= sw_breathing_battery_limit);
	}

	/* Check if active pattern can utilize breathing */
	if( !active_pattern ) {
		/* If there is no active pattern, breathing must
		 * be disabled so that suspend does not get blocked */
		breathe = false;
	}
	else {
		/* Check for always breathing special cases */
		if( !breathe )
			breathe = led_pattern_should_breathe(active_pattern);

		/* If pattern is configured not to breathe, we should
		 * not breathe even if it were allowed */
		if( !led_pattern_can_breathe(active_pattern) )
			breathe = false;
	}

	allow_sw_breathing(breathe);
}

/** Gconf notification callback function
 */
static void sw_breathing_setting_cb(GConfClient *const gcc, const guint id,
				    GConfEntry *const entry, gpointer const data)
{
	(void)gcc;
	(void)data;
	(void)id;

	const GConfValue *gcv = gconf_entry_get_value(entry);

	if( !gcv ) {
		mce_log(LL_DEBUG, "GConf Key `%s' has been unset",
			gconf_entry_get_key(entry));
		goto EXIT;
	}

	if( id == sw_breathing_enabled_setting_id ) {
		sw_breathing_enabled = gconf_value_get_bool(gcv) ? 1 : 0;
		sw_breathing_rethink();
	}
	else if( id == sw_breathing_battery_limit_setting_id ) {
		sw_breathing_battery_limit = gconf_value_get_int(gcv);
		sw_breathing_rethink();
	}
	else {
		mce_log(LL_WARN, "Spurious GConf value received; confused!");
	}

EXIT:
	return;
}

/** De-initialize sw breathing state data
 */
static void sw_breathing_quit(void)
{
	mce_setting_notifier_remove(sw_breathing_battery_limit_setting_id),
		sw_breathing_battery_limit_setting_id = 0;

	mce_setting_notifier_remove(sw_breathing_enabled_setting_id),
		sw_breathing_enabled_setting_id = 0;

	allow_sw_breathing(false);
}

/** Initialize sw breathing state data
 */
static void sw_breathing_init(void)
{
	/* sw_breath_enabled */
	mce_setting_notifier_add(MCE_SETTING_LED_PATH,
			       MCE_SETTING_LED_SW_BREATH_ENABLED,
			       sw_breathing_setting_cb,
			       &sw_breathing_enabled_setting_id);

	mce_setting_get_bool(MCE_SETTING_LED_SW_BREATH_ENABLED,
			   &sw_breathing_enabled);

	/* sw_breath_battery_limit */
	mce_setting_notifier_add(MCE_SETTING_LED_PATH,
			       MCE_SETTING_LED_SW_BREATH_BATTERY_LIMIT,
			       sw_breathing_setting_cb,
			       &sw_breathing_battery_limit_setting_id);

	mce_setting_get_int(MCE_SETTING_LED_SW_BREATH_BATTERY_LIMIT,
			  &sw_breathing_battery_limit);
}

/** Notification callback function for charger_state_pipe
 */
static void charger_state_trigger(gconstpointer data)
{
	charger_state_t prev = charger_state;
	charger_state = GPOINTER_TO_INT(data);

	if( charger_state == prev )
		goto EXIT;

	mce_log(LL_DEBUG, "charger_state: %s -> %s",
		charger_state_repr(prev),
		charger_state_repr(charger_state));

	sw_breathing_rethink();
EXIT:
	return;
}

/** Notification callback function for battery_level_pipe
 */
static void battery_level_trigger(gconstpointer data)
{
	int prev = battery_level;
	battery_level = GPOINTER_TO_INT(data);

	if( battery_level == prev )
		goto EXIT;

	mce_log(LL_DEBUG, "battery_level: %d -> %d", prev, battery_level);

	sw_breathing_rethink();
EXIT:
	return;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t led_dbus_handlers[] =
{
	/* signals - outbound (for Introspect purposes only) */
	{
		.interface = MCE_SIGNAL_IF,
		.name      = MCE_LED_PATTERN_ACTIVATED_SIG,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.args      =
			"    <arg name=\"pattern_name\" type=\"s\"/>\n"
	},
	{
		.interface = MCE_SIGNAL_IF,
		.name      = MCE_LED_PATTERN_DEACTIVATED_SIG,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.args      =
			"    <arg name=\"pattern_name\" type=\"s\"/>\n"
	},
	/* method calls */
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_ACTIVATE_LED_PATTERN,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = led_activate_pattern_dbus_cb,
		.args      =
			"    <arg direction=\"in\" name=\"pattern_name\" type=\"s\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_DEACTIVATE_LED_PATTERN,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = led_deactivate_pattern_dbus_cb,
		.args      =
			"    <arg direction=\"in\" name=\"pattern_name\" type=\"s\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_ENABLE_LED,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = led_enable_dbus_cb,
		.args      =
			""
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_DISABLE_LED,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = led_disable_dbus_cb,
		.args      =
			""
	},
	/* sentinel */
	{
		.interface = 0
	}
};

/** Add dbus handlers
 */
static void mce_led_init_dbus(void)
{
	mce_dbus_handler_register_array(led_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mce_led_quit_dbus(void)
{
	mce_dbus_handler_unregister_array(led_dbus_handlers);
}

/** Array of datapipe handlers */
static datapipe_handler_t mce_led_datapipe_handlers[] =
{
	// output triggers
	{
		.datapipe  = &user_activity_event_pipe,
		.output_cb = user_activity_event_trigger,
	},
	{
		.datapipe  = &system_state_pipe,
		.output_cb = system_state_trigger,
	},
	{
		.datapipe  = &display_state_curr_pipe,
		.output_cb = display_state_curr_trigger,
	},
	{
		.datapipe  = &led_brightness_pipe,
		.output_cb = led_brightness_trigger,
	},
	{
		.datapipe  = &led_pattern_activate_pipe,
		.output_cb = led_pattern_activate_trigger,
	},
	{
		.datapipe  = &led_pattern_deactivate_pipe,
		.output_cb = led_pattern_deactivate_trigger,
	},
	{
		.datapipe  = &charger_state_pipe,
		.output_cb = charger_state_trigger,
	},
	{
		.datapipe  = &battery_level_pipe,
		.output_cb = battery_level_trigger,
	},
	// sentinel
	{
		.datapipe = 0,
	}
};

static datapipe_bindings_t mce_led_datapipe_bindings =
{
	.module   = "led",
	.handlers = mce_led_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void
mce_led_datapipes_init(void)
{
	datapipe_bindings_init(&mce_led_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void
mce_led_datapipes_quit(void)
{
	datapipe_bindings_quit(&mce_led_datapipe_bindings);
}

/**
 * Init function for the LED logic module
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 * @return NULL on success, a string with an error message on failure
 */
const gchar *g_module_check_init(GModule *module)
{
	gchar *status = NULL;

	(void)module;

	/* Append triggers/filters to datapipes */
	mce_led_datapipes_init();

	/* Setup a pattern stack,
	 * a combination rule stack and a cross-refernce for said stack
	 * and initialise the patterns
	 */
	pattern_stack = g_queue_new();
	combination_rule_list = g_queue_new();
	combination_rule_xref_list = g_queue_new();

	if (init_patterns() == FALSE)
		goto EXIT;

	/* Add dbus handlers */
	mce_led_init_dbus();

	/* Initialize sw breathing state data */
	sw_breathing_init();
	charger_state_trigger(charger_state_pipe.cached_data);
	battery_level_trigger(battery_level_pipe.cached_data);

	/* Evaluate initial active pattern state */
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
void g_module_unload(GModule *module)
{
	(void)module;

	/* Remove dbus handlers */
	mce_led_quit_dbus();

	/* Close files */
	mce_close_output(&led_current_rm_output);
	mce_close_output(&led_current_g_output);
	mce_close_output(&led_current_b_output);

	mce_close_output(&led_brightness_rm_output);
	mce_close_output(&led_brightness_g_output);
	mce_close_output(&led_brightness_b_output);

	/* Remove triggers/filters from datapipes */
	mce_led_datapipes_quit();

	/* Remove breathing timers and wakelocks */
	sw_breathing_quit();

	/* Don't disable the LED on shutdown/reboot/acting dead */
	if ((system_state != MCE_SYSTEM_STATE_ACTDEAD) &&
	    (system_state != MCE_SYSTEM_STATE_SHUTDOWN) &&
	    (system_state != MCE_SYSTEM_STATE_REBOOT)) {
		led_set_active_pattern(0);

		switch (get_led_type()) {
#ifdef ENABLE_HYBRIS
		case LED_TYPE_HYBRIS:
			/* The hybris plugin reprograms the led asynchronously
			 * after some delay. In this case we want to block
			 * until the led is actually turned off. */
			mce_hybris_indicator_quit();
			break;
#endif
		default:
			break;
		}
	}

	/* Free path strings; this has to be done after
	 * led_set_active_pattern(0), since it uses these paths
	 */
	g_free((void*)led_current_rm_output.path);
	g_free((void*)led_current_g_output.path);
	g_free((void*)led_current_b_output.path);

	g_free((void*)led_brightness_rm_output.path);
	g_free((void*)led_brightness_g_output.path);
	g_free((void*)led_brightness_b_output.path);

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
			led_pattern_delete(psp);
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

	return;
}
