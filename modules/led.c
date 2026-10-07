/**
 * @file led.c
 * LED module -- this handles the LED logic for MCE
 * <p>
 * Copyright (c) 2006 - 2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (c) 2012 - 2020 Jolla Ltd.
 * Copyright (c) 2020 Open Mobile Platform LLC.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Tapio Rantala <ext-tapio.rantala@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Jukka Turunen <ext-jukka.t.turunen@nokia.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * @author Islam Amer <islam.amer@jollamobile.com>
 * @author Filip Matijević <filip.matijevic.pz@gmail.com>
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
#include <mce/mode-names.h>

#include <gmodule.h>

/** Helper for making diagnostic messages more human readable
 *
 * @param val Boolean value
 *
 * @return "true" or "false"
 */
static const char *bool_repr(bool val)
{
	return val ? "true" : "false";
}

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

/**
 * Size of each LED channel
 *
 * Multiply the channel size by 2 since we store hexadecimal ASCII
 */
#define CHANNEL_SIZE		32 * 2

/** Structure holding LED patterns
 *
 * Keep led_pattern_create() and led_pattern_delete() in sync.
 */
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
	guint setting_id;		/**< Callback ID for GConf entry */
	guint rgb_color;                /**< RGB24 data for libhybris use */
	gboolean undecided;		/**< Flag for policy=6 lock in */
	bool eligible;                  /**< Pattern could be the active one */
	bool eligible_hybris;           /**< State reported to hybris plugin */
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

/** LED type */
typedef enum {
	/** LED type unset */
	LED_TYPE_UNSET = -1,
	/** No LED available */
	LED_TYPE_NONE = 0,
#ifdef ENABLE_HYBRIS
	/** Android adaptation via libhybris */
	LED_TYPE_HYBRIS = 6,
#endif
} led_type_t;

/**
 * The configuration group containing the LED pattern
 */
static const gchar *led_pattern_group = NULL;

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
static guint maximum_led_brightness = 1; // 1 = safe for all non-zero value

/* Function prototypes */
static led_type_t        get_led_type                   (void);
static gint              queue_find                     (gconstpointer data, gconstpointer userdata);
static gint              queue_prio_compare             (gconstpointer entry1, gconstpointer entry2, gpointer userdata);
static void              hybris_set_brightness          (gint brightness);
static void              hybris_disable_led             (void);
static void              disable_led                    (void);
static pattern_struct   *led_pattern_create             (void);
static void              led_pattern_delete             (pattern_struct *self);
static void              led_pattern_set_active         (pattern_struct *self, gboolean active);
static void              led_pattern_update_eligible    (pattern_struct *self);
static void              led_pattern_sync_eligible      (pattern_struct *self);
static bool              led_pattern_should_breathe     (const pattern_struct *self);
static bool              led_pattern_can_breathe        (const pattern_struct *self);
static bool              led_pattern_is_panic_blink     (const pattern_struct *self);
static gboolean          led_pattern_timeout_cb         (gpointer data);
static void              hybris_program_led             (const pattern_struct *const pattern);
static void              program_led                    (const pattern_struct *const pattern);
static gboolean          allow_sw_breathing_cb          (gpointer aptr);
static void              allow_sw_breathing             (bool enable);
static void              allow_sw_breathing_now         (bool enable);
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
 * Get the LED type
 *
 * @return The LED type
 */
static led_type_t get_led_type(void)
{
	static led_type_t led_type = LED_TYPE_UNSET;

	/* If we have the LED type already, return it */
	if (led_type != LED_TYPE_UNSET)
		goto EXIT;

	led_type = LED_TYPE_NONE;

#ifdef ENABLE_HYBRIS
	/* Use mce-plugin-libhybris if available */
	if( mce_hybris_indicator_init() ) {
		led_type = LED_TYPE_HYBRIS;
		led_pattern_group = MCE_CONF_LED_PATTERN_HYBRIS_GROUP;
		maximum_led_brightness = MAXIMUM_HYBRIS_LED_BRIGHTNESS;
	}
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

	self->name            = NULL;
	self->timeout_id      = 0;
	self->setting_id      = 0;
	self->eligible        = false;
	self->eligible_hybris = false;

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

/** Recalculate pattern eligible property
 *
 * Check if the pattern is in such a state that led policy and
 * device state allows it to be activated.
 *
 * Update pattern eligible property accordingly and pass changes
 * to hybris-plugin too.
 *
 * @param self    pattern object
 */
static void led_pattern_update_eligible(pattern_struct *self)
{
	bool eligible = true;

#if 0 /* While this can be useful when actively debugging led
       * activation logic, it creates so much noise that using
       * debug verbosity becomes impossible - do not compile in
       * by default. */
	mce_log(LL_DEBUG, "pattern: %s, active: %d, enabled: %d", self->name, self->active, self->enabled);
#endif

	/* If the pattern is deactivated, ignore */
	if( !self->active )
		goto NEGATIVE;

	/* If the pattern is disabled through GConf, ignore */
	if( !self->enabled )
		goto NEGATIVE;

	/* If the LED is disabled,
	 * only patterns with visibility 5 are shown
	 */
	if( !led_enabled && self->policy != 5 )
		goto NEGATIVE;

	/* Always show pattern with visibility 3 or 5 */
	if( self->policy == 3 || self->policy == 5 )
		goto POSITIVE;

	/* Show pattern with visibility 7 if display is dimmed */
	if( self->policy == 7 ) {
		if( display_state_curr == MCE_DISPLAY_DIM )
			goto POSITIVE;
		goto NEGATIVE;
	}

	/* Acting dead behaviour */
	if( system_state == MCE_SYSTEM_STATE_ACTDEAD ) {
		/* If we're in acting dead,
		 * show patterns with visibility 4
		 */
		if( self->policy == 4 )
			goto POSITIVE;

		/* If we're in acting dead
		 * and the display is off, show pattern
		 */
		if( display_off_p(display_state_curr) && self->policy == 2 )
			goto POSITIVE;

		/* If the display is on and visibility is 2,
		 * or if visibility is 1/0, ignore pattern
		 */
		goto NEGATIVE;
	}

	/* If the display is off or in low power mode,
	 * we can use any active pattern
	 */
	if( display_off_p(display_state_curr) )
		goto POSITIVE;

	/* If the pattern should be shown with screen on, use it */
	if( self->policy == 1 )
		goto POSITIVE;

NEGATIVE:
	eligible = false;

POSITIVE:
	if( self->eligible != eligible ) {
		mce_log(LL_DEBUG, "pattern: %s, eligible: %s -> %s",
			self->name, bool_repr(self->eligible), bool_repr(eligible));
		self->eligible = eligible;
	}
}

/** Forward pattern eligible property changes to hybris plugin
 *
 * @param self    pattern object
 */
static void led_pattern_sync_eligible(pattern_struct *self)
{
#ifdef ENABLE_HYBRIS
	if( self->eligible_hybris != self->eligible ) {
		self->eligible_hybris = self->eligible;
		if( get_led_type() == LED_TYPE_HYBRIS )
			mce_hybris_indicator_set_active(self->name, self->eligible_hybris);
	}
#endif
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

/** Check if pattern is of panic blink type
 *
 * These patterns with short on/off period should utilize
 * hard blinking instead of soft breathing.
 *
 * @param self led pattern object
 *
 * @return true if pattern is panic, false otherwise
 */
static bool led_pattern_is_panic_blink(const pattern_struct *self)
{
	const int min_ms = 1;
	const int max_ms = 250;

	return (self->on_period  >= min_ms && self->on_period  <= max_ms &&
		self->off_period >= min_ms && self->off_period <= max_ms);
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
#ifdef ENABLE_HYBRIS
	case LED_TYPE_HYBRIS:
		hybris_program_led(pattern);
		break;
#endif
	default:
		break;
	}
}

/** Current sw breathing enabled state */
static bool allow_sw_breathing_active = false;

/** Target for delayed sw breathing enable/disable */
static bool allow_sw_breathing_target = false;

/** Idle callback id for delayed sw breathing enable/disable */
static guint allow_sw_breathing_id = 0;

/** Idle callback for enabling / disabling sw led breathing
 *
 * @param aptr  (unused user data pointer)
 *
 * @return G_SOURCE_REMOVE to remove idle callback source id
 */
static gboolean allow_sw_breathing_cb(gpointer aptr)
{
	(void)aptr;
	allow_sw_breathing_id = 0;
	allow_sw_breathing_now(allow_sw_breathing_target);
	return G_SOURCE_REMOVE;
}

/** Enable/disable led breathing via software - possibly after delay
 *
 * Enabling is done immediately so that pattern activation
 * notifications to hybris plugin happen in already enabled
 * state.
 *
 * Disabling is done from idle callback so that pattern deactivation
 * notifications to hybris plugin happen while still in enabled state.
 *
 * @param enable  Whether breathing should be enabled / disabled
 */
static void allow_sw_breathing(bool enable)
{
	/* If led backend does not support breathing make sure we do
	 * not grab a useless wakelock and block suspend unnecessarily */
	if( !mce_hybris_indicator_can_breathe() )
		enable = false;

	if( allow_sw_breathing_target == enable )
		goto EXIT;

	allow_sw_breathing_target = enable;

#ifdef ENABLE_HYBRIS
	if( get_led_type() == LED_TYPE_HYBRIS ) {
		if( allow_sw_breathing_target )
			allow_sw_breathing_now(true);
		else if( !allow_sw_breathing_id )
			allow_sw_breathing_id = g_idle_add(allow_sw_breathing_cb, NULL);
	}
#endif

EXIT:
	return;
}
/** Enable/disable led breathing via software without delay
 *
 * Any pending delayed enable/disable is canceled.
 *
 * @param enable  Whether breathing should be enabled / disabled
 */

static void allow_sw_breathing_now(bool enable)
{
	static const char name[] = "mce_led_breathing";

	if( allow_sw_breathing_id )
		g_source_remove(allow_sw_breathing_id), allow_sw_breathing_id = 0;

	if( allow_sw_breathing_active != enable ) {
		allow_sw_breathing_active = allow_sw_breathing_target = enable;

		if( allow_sw_breathing_active ) {
			wakelock_lock(name, -1);
			mce_log(LL_DEBUG, "sw breathing wakelock: acquired");
			mce_hybris_indicator_enable_breathing(true);
		}
		else {
			mce_hybris_indicator_enable_breathing(false);
			mce_log(LL_DEBUG, "sw breathing wakelock: released");
			wakelock_unlock(name);
		}
	}
}

/** Setter function for active_pattern
 *
 * @param pattern The led pattern to activate, or NULL to disable
 */
static void led_set_active_pattern(pattern_struct *pattern)
{
	if( active_pattern == pattern )
		goto EXIT;

	mce_log(LL_DEVEL, "active led pattern: %s -> %s",
		active_pattern ? active_pattern->name : "none",
		pattern        ? pattern->name        : "none");

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
	pattern_struct *active = NULL;

	/* First: Handle MCE side transitions
	 */
	if( pattern_stack ) {
		/* Update eligibility of all patterns */
		for( GList *iter = pattern_stack->head; iter; iter = iter->next ) {
			pattern_struct *pattern = iter->data;
			led_pattern_update_eligible(pattern);
		}

		/* Select the first / highest priority eligible pattern */
		for( GList *iter = pattern_stack->head; iter; iter = iter->next ) {
			pattern_struct *pattern = iter->data;
			if( pattern->eligible ) {
				active = pattern;
				break;
			}
		}
	}

	led_set_active_pattern(active);

	/* Then: Synchronize to hybris plugin
	 */
	if( pattern_stack ) {
		/* Re-evaluate breathing policy */
		sw_breathing_rethink();

		/* Communicate eligibility changes to hybris plugin */
		for( GList *iter = pattern_stack->head; iter; iter = iter->next ) {
			pattern_struct *pattern = iter->data;
			led_pattern_sync_eligible(pattern);
		}
	}
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
	sw_breathing_rethink();

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
#ifdef ENABLE_HYBRIS
	case LED_TYPE_HYBRIS:
		hybris_set_brightness(led_brightness);
		break;
#endif
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
			   GINT_TO_POINTER(maximum_led_brightness));

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

/** Current battery percent level: assume unknown */
static int battery_level = MCE_BATTERY_LEVEL_UNKNOWN;

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
	bool breathing_allowed = false;
	bool breathing_needed  = false;

	if( !active_pattern ) {
		/* No active pattern -> no eligible patterns */
		goto EXIT;
	}

	if( led_pattern_should_breathe(active_pattern) ) {
		// Special patterns can override settings
		breathing_needed  = true;
		breathing_allowed = true;
	}
	else if( !sw_breathing_enabled ) {
		// Disabled in settings
	}
	else if( display_state_curr != MCE_DISPLAY_OFF ) {
		// We are not suspending anyway
		breathing_allowed = true;
	}
	else if( charger_state == CHARGER_STATE_ON || battery_level >= sw_breathing_battery_limit ) {
		// Charger connected or battery full enough
		breathing_allowed = true;
	}

	if( breathing_allowed && !breathing_needed ) {
		if( mce_hybris_indicator_has_multiple_leds() ) {
			/* Check if at least one eligible pattern needs breathing */
			for( GList *iter = pattern_stack->head; iter; iter = iter->next ) {
				pattern_struct *pattern = iter->data;
				if( !pattern->eligible )
					continue;
				if( led_pattern_can_breathe(pattern) || led_pattern_is_panic_blink(pattern) ) {
					breathing_needed = true;
					break;
				}
			}
		}
		else {
			/* Check the active pattern needs breathing */
			if( led_pattern_can_breathe(active_pattern) || led_pattern_is_panic_blink(active_pattern) )
				breathing_needed = true;
		}
	}
EXIT:
	allow_sw_breathing(breathing_needed && breathing_allowed);
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

	allow_sw_breathing_now(false);
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
	mce_datapipe_init_bindings(&mce_led_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void
mce_led_datapipes_quit(void)
{
	mce_datapipe_quit_bindings(&mce_led_datapipe_bindings);
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
	charger_state_trigger(datapipe_value(&charger_state_pipe));
	battery_level_trigger(datapipe_value(&battery_level_pipe));

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
	/* Remove triggers/filters from datapipes */
	mce_led_datapipes_quit();

	/* Notify hybris plugin that we are about to exit */
#ifdef ENABLE_HYBRIS
	if( get_led_type() == LED_TYPE_HYBRIS )
		mce_hybris_indicator_shutdown();
#endif

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
