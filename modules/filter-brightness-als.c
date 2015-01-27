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
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
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

#include "filter-brightness-als.h"
#include "display.h"

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-io.h"
#include "../mce-conf.h"
#include "../mce-gconf.h"
#include "../mce-dbus.h"
#include "../mce-sensorfw.h"

#include <stdlib.h>
#include <string.h>

#include <mce/dbus-names.h>

#include <gmodule.h>

#if 0 // DEBUG: make all logging from this module "critical"
# undef mce_log
# define mce_log(LEV, FMT, ARGS...) \
	mce_log_file(LL_CRIT, __FILE__, __FUNCTION__, FMT , ## ARGS)
#endif

/** Request enabling of ALS; reference counted */
#define MCE_REQ_ALS_ENABLE		"req_als_enable"

/** Request disabling of ALS; reference counted */
#define MCE_REQ_ALS_DISABLE		"req_als_disable"

/** Maximum number of monitored ALS owners */
#define ALS_MAX_MONITORED		16

/** Module name */
#define MODULE_NAME			"filter-brightness-als"

enum
{
	/** Maximum number of steps ALS ramps can have
	 *
	 * Enough to cover 5% to 95% in 5% steps.
	 */
	ALS_LUX_STEPS = 21, // allows 5% steps for [5 ... 100] range
};

/** A step in ALS ramp */
typedef struct
{
	int lux; /**< upper lux limit */
	int val; /**< brightness percentage to use */
} als_limit_t;

/** ALS filtering state */
typedef struct
{
	/** Filter name; used for locating configuration data */
	const char *id;

	/* Number of profiles available */
	int profiles;

	/** Threshold: lower lux limit */
	int lux_lo;

	/** Threshold: upper lux limit */
	int lux_hi;

	/** Threshold: active ALS profile */
	als_profile_t prof;

	/** Latest brightness percentage result */
	int val;

	/** Brightness percent from lux value look up table */
	als_limit_t lut[ALS_PROFILE_COUNT][ALS_LUX_STEPS+1];
} als_filter_t;

/** Is there an ALS available? */
static gboolean have_als = TRUE;

/** Filter things through ALS - config value */
static gboolean use_als_flag = TRUE;

/** Filter things through ALS - config notification */
static guint use_als_gconf_id = 0;

/** Cached display state; tracked via display_state_trigger() */
static display_state_t display_state = MCE_DISPLAY_UNDEF;

/** Latest lux reading from the ALS */
static gint als_lux_latest = -1;

/** List of monitored external als enablers (legacy D-Bus API) */
static GSList *ext_als_enablers = NULL;

/** Currently active color profile (dummy implementation) */
static char *color_profile_name = 0;

/** GConf callback ID for colour profile */
static guint color_profile_gconf_id = 0;

/** List of known color profiles (dummy implementation) */
static const char * const color_profiles[] =
{
	COLOR_PROFILE_ID_HARDCODED,
};

/** ALS filtering state for display backlight brightness */
static als_filter_t lut_display =
{
	.id = "Display",
};

/** ALS filtering state for keypad backlight brightness */
static als_filter_t lut_key =
{
	.id = "Keypad",
};

/** ALS filtering state for indication led brightness */
static als_filter_t lut_led =
{
	.id = "Led",
};

/** ALS filtering state for low power mode display simulation */
static als_filter_t lut_lpm =
{
	.id = "LPM",
};

static gboolean set_color_profile(const gchar *id);
static gboolean save_color_profile(const gchar *id);

/** Integer minimum helper */
static inline int imin(int a, int b) { return (a < b) ? a : b; }

/** Check if color profile is supported
 *
 * @param id color profile name
 *
 * @return TRUE if profile is supported, FALSE otherwise
 */
static gboolean color_profile_exists(const char *id)
{
	if( !id )
		return FALSE;

	size_t n = sizeof color_profiles / sizeof *color_profiles;
	for( size_t i = 0; i < n; ++i ) {
		if( !strcmp(color_profiles[i], id) )
			return TRUE;
	}

	return FALSE;
}

/** Remove transition thresholds from filtering state
 *
 * Set thresholds so that any lux value will be out of bounds.
 *
 * @param self ALS filtering state data
 */
static void als_filter_clear_threshold(als_filter_t *self)
{
	self->lux_lo = INT_MAX;
	self->lux_hi = 0;
}

/** Initialize ALS filtering state
 *
 * @param self ALS filtering state data
 */
static void als_filter_init(als_filter_t *self)
{
	/* Reset ramps to a state where any lux value will
	 * yield 100% brightness */
	for( int i = 0; i < ALS_PROFILE_COUNT; ++i ) {
		for( int k = 0; k <= ALS_LUX_STEPS; ++k ) {
			self->lut[i][k].lux = INT_MAX;
			self->lut[i][k].val = 100;
		}
	}

	/* Default to 100% output */
	self->val  = 100;

	/* Invalidate thresholds */
	self->prof = -1;
	als_filter_clear_threshold(self);
}

/** Load ALS ramp into filtering state
 *
 * @param self ALS filtering state data
 * @param grp  Configuration group name
 * @param prof ALS profile id
 */
static bool
als_filter_load_profile(als_filter_t *self, const char *grp,
			als_profile_t prof)
{
	bool  success = false;
	gsize lim_cnt = 0;
	gsize lev_cnt = 0;
	gint *lim_val = 0;
	gint *lev_val = 0;
	char  lim_key[64];
	char  lev_key[64];

	snprintf(lim_key, sizeof lim_key, "LimitsProfile%d",  prof);
	snprintf(lev_key, sizeof lev_key, "LevelsProfile%d",  prof);

	lim_val = mce_conf_get_int_list(grp, lim_key, &lim_cnt);
	lev_val = mce_conf_get_int_list(grp, lev_key, &lev_cnt);

	if( !lim_val || lim_cnt < 1 ) {
		if( prof == 0 )
			mce_log(LL_WARN, "[%s] %s: no items", grp, lim_key);
		goto EXIT;
	}

	if( !lev_val || lev_cnt != lim_cnt ) {
		mce_log(LL_WARN, "[%s] %s: must have %zd items",
			grp, lev_key, lim_cnt);
		goto EXIT;
	}

	if( lim_cnt >  ALS_LUX_STEPS ) {
		lim_cnt = ALS_LUX_STEPS;
		mce_log(LL_WARN, "[%s] %s: excess items",
			grp, lim_key);
	}
	else if( lim_cnt < ALS_LUX_STEPS ) {
		mce_log(LL_DEBUG, "[%s] %s: missing items",
			grp, lim_key);
	}

	for( gsize k = 0; k < lim_cnt; ++k ) {
		self->lut[prof][k].lux = lim_val[k];
		self->lut[prof][k].val = lev_val[k];
	}

	success = true;
EXIT:
	g_free(lim_val);
	g_free(lev_val);

	return success;
}

/** Load ALS ramps into filtering state
 *
 * @param self ALS filtering state data
 */
static void als_filter_load_config(als_filter_t *self)
{
	als_filter_init(self);

	char grp[64];
	snprintf(grp, sizeof grp, "Brightness%s", self->id);

	if( !mce_conf_has_group(grp) ) {
		mce_log(LL_WARN, "[%s]: als config missing", grp);
		goto EXIT;
	}

	for( self->profiles = 0; self->profiles < ALS_PROFILE_COUNT; ++self->profiles ) {
		if( !als_filter_load_profile(self, grp, self->profiles) )
			break;
	}

	if( self->profiles < 1 )
		mce_log(LL_WARN, "[%s]: als config broken", grp);
EXIT:
	return;
}

/** Get lux value for given profile and step in ramp
 *
 * @param self ALS filtering state data
 * @param prof ALS profile id
 * @param slot position in ramp
 *
 * @return lux value
 */
static int als_filter_get_lux(als_filter_t *self, als_profile_t prof, int slot)
{
	if( slot < 0 )
		return 0;
	if( slot < ALS_LUX_STEPS )
		return self->lut[prof][slot].lux;
	return INT_MAX;
}

/** Run ALS filter
 *
 * @param self ALS filtering state data
 * @param prof ALS profile id
 * @param lux  ambient light value
 *
 * @return 0 ... 100 percentage
 */
static int als_filter_run(als_filter_t *self, als_profile_t prof, int lux)
{
	mce_log(LL_DEBUG, "FILTERING: %s", self->id);

	if( lux < 0 ) {
		mce_log(LL_DEBUG, "no lux data yet");
		goto EXIT;
	}

	if( self->prof != prof ) {
		mce_log(LL_DEBUG, "profile changed");
	}
	else if( self->lux_lo <= lux && lux <= self->lux_hi ) {
		mce_log(LL_DEBUG, "within thresholds");
		goto EXIT;
	}

	int slot;

	for( slot = 0; slot < ALS_LUX_STEPS; ++slot ) {
		if( lux < self->lut[prof][slot].lux )
			break;
	}

	self->prof = prof;

	if( slot < ALS_LUX_STEPS )
		self->val = self->lut[prof][slot].val;
	else
		self->val = 100;

	self->lux_lo = 0;
	self->lux_hi = INT_MAX;

	/* Add hysteresis to transitions that make the display dimmer
	 *
	 *                 lux from ALS
	 *                  |
	 *                  |  configuration slot
	 *                  |   |
	 *                  v   |
	 *    0----A------B-----C-----> [lux]
	 *
	 *              |-------|
	 * threshold    lo      hi
	 */

	int a = als_filter_get_lux(self, prof, slot-2);
	int b = als_filter_get_lux(self, prof, slot-1);
	int c = als_filter_get_lux(self, prof, slot+0);

	self->lux_lo = b - imin(b-a, c-b) / 10;
	self->lux_hi = c;

	mce_log(LL_DEBUG, "prof=%d, slot=%d, range=%d...%d",
		prof, slot, self->lux_lo, self->lux_hi);

EXIT:
	return self->val;
}

static void run_datapipes(void)
{
	/* Re-filter the brightness */
	execute_datapipe(&display_brightness_pipe, NULL,
			 USE_CACHE, DONT_CACHE_INDATA);
	execute_datapipe(&led_brightness_pipe, NULL,
			 USE_CACHE, DONT_CACHE_INDATA);
	execute_datapipe(&lpm_brightness_pipe, NULL,
			 USE_CACHE, DONT_CACHE_INDATA);
	execute_datapipe(&key_backlight_pipe, NULL,
			 USE_CACHE, DONT_CACHE_INDATA);
}

/** Handle lux value changed event
 *
 * @param lux ambient light value
 */
static void als_lux_changed(unsigned lux)
{
	gint als_lux_sensor = (gint)lux;

	/* Update / clear cached lux value */
	if( als_lux_sensor < 0 ) {
		if( !have_als || !use_als_flag )
			als_lux_latest = -1;
	}
	else {
		als_lux_latest = als_lux_sensor;
	}

	mce_log(LL_DEBUG, "lux input=%d, using=%d",
		als_lux_sensor, als_lux_latest);

	/* Run brightness filters */
	run_datapipes();

	/* Broadcast via datapipe */
	execute_datapipe(&ambient_light_sensor_pipe,
			 GINT_TO_POINTER(als_lux_latest),
			 USE_INDATA, CACHE_INDATA);
}

/** Check if ALS sensor should be enabled or disabled
 */
static void rethink_als_status(void)
{
	static gboolean enable_old = FALSE;

	gboolean enable_new = FALSE;
	gboolean want_data  = FALSE;

	if( !have_als )
		goto EXIT;

	if( use_als_flag ) {
		switch( display_state ) {
		case MCE_DISPLAY_ON:
		case MCE_DISPLAY_DIM:
		case MCE_DISPLAY_POWER_UP:
		case MCE_DISPLAY_LPM_OFF:
		case MCE_DISPLAY_LPM_ON:
			want_data = TRUE;
			break;

		default:
		case MCE_DISPLAY_POWER_DOWN:
		case MCE_DISPLAY_OFF:
		case MCE_DISPLAY_UNDEF:
			want_data = FALSE;
			break;
		}
	}

	if( want_data || ext_als_enablers  )
		enable_new = TRUE;

	mce_log(LL_DEBUG, "use=%d, want=%d ext=%d -> enable=%d",
		use_als_flag, want_data, ext_als_enablers ? 1 : 0,
		enable_new);

	if( want_data )
		mce_sensorfw_als_set_notify(als_lux_changed);
	else {
		mce_sensorfw_als_set_notify(0);
		als_lux_changed(-1);
	}

	if( enable_old == enable_new )
		goto EXIT;

	enable_old = enable_new;

	if( enable_new ) {
		mce_sensorfw_als_enable();
	}
	else {
		mce_sensorfw_als_disable();

		/* Clear thresholds so that the next reading from
		 * als will not be affected by previous state */
		als_filter_clear_threshold(&lut_display);
		als_filter_clear_threshold(&lut_led);
		als_filter_clear_threshold(&lut_key);
		als_filter_clear_threshold(&lut_lpm);
	}

	run_datapipes();
EXIT:
	return;
}

/**
 * GConf callback for ALS settings
 *
 * @param gcc Unused
 * @param id Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data Unused
 */
static void use_als_gconf_cb(GConfClient *const gcc, const guint id,
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

	if (id == use_als_gconf_id) {
		use_als_flag = gconf_value_get_bool(gcv);
		rethink_als_status();
	} else {
		mce_log(LL_WARN,
			"Spurious GConf value received; confused!");
	}

EXIT:
	return;
}

/**
 * Ambient Light Sensor filter for display brightness
 *
 * @param data The un-processed brightness setting (1-5) stored in a pointer
 * @return The processed brightness value (percentage) stored in a pointer
 */
static gpointer display_brightness_filter(gpointer data)
{
	int setting = GPOINTER_TO_INT(data);

	if( setting < 1 )   setting =   1; else
	if( setting > 100 ) setting = 100;

	int brightness = setting;

	int max_prof = lut_display.profiles - 1;

	if( max_prof < 0 )
		goto EXIT;

	if( !use_als_flag )
		goto EXIT;

	if( als_lux_latest < 0 )
		goto EXIT;

	als_profile_t prof = mce_xlat_int(1,100, 0,max_prof, setting);

	brightness = als_filter_run(&lut_display, prof, als_lux_latest);

EXIT:
	mce_log(LL_DEBUG, "in=%d -> out=%d", setting, brightness);
	return GINT_TO_POINTER(brightness);
}

/**
 * Ambient Light Sensor filter for LED brightness
 *
 * @param data The un-processed brightness setting (1-5) stored in a pointer
 * @return The processed brightness value
 */
static gpointer led_brightness_filter(gpointer data)
{
	int value = GPOINTER_TO_INT(data);
	int scale = 40;

	if( lut_led.profiles < 1 )
		goto EXIT;

	if( als_lux_latest < 0 )
		goto EXIT;

	scale = als_filter_run(&lut_led, 0, als_lux_latest);

EXIT:
	return GINT_TO_POINTER(value * scale / 100);
}

/** Ambient Light Sensor filter for LPM brightness
 *
 * @param data The un-processed brightness setting (1-100) stored in a pointer
 * @return The processed brightness value
 */
static gpointer lpm_brightness_filter(gpointer data)
{
	int value = GPOINTER_TO_INT(data);

	if( lut_lpm.profiles < 1 )
		goto EXIT;

	if( als_lux_latest < 0 )
		goto EXIT;

	/* Note: Input value is ignored and output is
	 *       determined only by the als config */
	value = als_filter_run(&lut_lpm, 0, als_lux_latest);

EXIT:
	return GINT_TO_POINTER(value);
}

/**
 * Ambient Light Sensor filter for keyboard backlight brightness
 *
 * @param data The un-processed brightness setting (1-5) stored in a pointer
 * @return The processed brightness value
 */
static gpointer key_backlight_filter(gpointer data)
{
	int value = GPOINTER_TO_INT(data);
	int scale = 100;

	if( lut_key.profiles < 1 )
		goto EXIT;

	if( als_lux_latest < 0 )
		goto EXIT;

	scale = als_filter_run(&lut_key, 0, als_lux_latest);

EXIT:
	return GINT_TO_POINTER(value * scale / 100);
}

/**
 * Handle display state change
 *
 * @param data The display stated stored in a pointer
 */
static void display_state_trigger(gconstpointer data)
{
	static display_state_t old_display_state = MCE_DISPLAY_UNDEF;
	display_state = GPOINTER_TO_INT(data);

	if( old_display_state == display_state )
		goto EXIT;

	mce_log(LL_DEBUG, "display: %d -> %d",
		old_display_state, display_state);

	old_display_state = display_state;

	rethink_als_status();

EXIT:
	return;
}

/**
 * D-Bus callback used for reference counting ALS enabling;
 * if the requesting process exits, immediately decrease the refcount
 *
 * @param msg The D-Bus message
 * @return TRUE
 */
static gboolean als_owner_monitor_dbus_cb(DBusMessage *const msg)
{
	const gchar *old_name = 0;
	const gchar *new_name = 0;
	const gchar *service  = 0;
	DBusError    error    = DBUS_ERROR_INIT;

	gssize retval;

	if( !dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &service,
				  DBUS_TYPE_STRING, &old_name,
				  DBUS_TYPE_STRING, &new_name,
				  DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR,	"Failed to get argument from %s.%s; %s: %s",
			"org.freedesktop.DBus", "NameOwnerChanged",
			error.name, error.message);
		goto EXIT;
	}

	/* Remove the name monitor for the ALS owner */
	retval = mce_dbus_owner_monitor_remove(service,
					       &ext_als_enablers);

	if (retval == -1) {
		mce_log(LL_WARN, "Failed to remove name owner monitoring"
			" for `%s'", service);
		goto EXIT;
	}

	rethink_als_status();

EXIT:
	dbus_error_free(&error);
	return TRUE;
}

/**
 * D-Bus callback for the ALS enabling method call
 *
 * @param msg The D-Bus message
 * @return TRUE
 */
static gboolean als_enable_req_dbus_cb(DBusMessage *const msg)
{
	const char *sender;

	gssize retval;

	if( !(sender = dbus_message_get_sender(msg)) )
		goto EXIT;

	mce_log(LL_DEVEL, "Received ALS enable request from %s",
		mce_dbus_get_name_owner_ident(sender));

	retval = mce_dbus_owner_monitor_add(sender, als_owner_monitor_dbus_cb,
					    &ext_als_enablers,
					    ALS_MAX_MONITORED);
	if (retval == -1) {
		mce_log(LL_WARN, "Failed to add name owner monitoring"
			" for `%s'", sender);
		goto EXIT;
	}

	rethink_als_status();

EXIT:
	if( !dbus_message_get_no_reply(msg) ) {
		DBusMessage *reply = dbus_new_method_reply(msg);
		dbus_send_message(reply), reply = 0;
	}

	return TRUE;
}

/**
 * D-Bus callback for the ALS disabling method call
 *
 * @param msg The D-Bus message
 * @return TRUE
 */
static gboolean als_disable_req_dbus_cb(DBusMessage *const msg)
{
	const char *sender;

	gssize retval;

	if( !(sender = dbus_message_get_sender(msg)) )
		goto EXIT;

	mce_log(LL_DEVEL, "Received ALS disable request from %s",
		mce_dbus_get_name_owner_ident(sender));

	retval = mce_dbus_owner_monitor_remove(sender,
					       &ext_als_enablers);

	if (retval == -1) {
		mce_log(LL_INFO, "Failed to remove name owner monitoring"
			" for `%s'",sender);
		goto EXIT;
	}

	rethink_als_status();

EXIT:
	if( !dbus_message_get_no_reply(msg) ) {
		DBusMessage *reply = dbus_new_method_reply(msg);
		dbus_send_message(reply), reply = 0;
	}

	return TRUE;
}

/**
 * Send the current profile id
 *
 * @param method_call A DBusMessage to reply to
 * @return TRUE
 */
static gboolean send_current_color_profile(DBusMessage *method_call)
{
	DBusMessage *msg = 0;
	const char  *val = color_profile_name ?: COLOR_PROFILE_ID_HARDCODED;

	if( method_call )
		msg = dbus_new_method_reply(method_call);
	else
		msg = dbus_new_signal(MCE_SIGNAL_PATH,
				      MCE_SIGNAL_IF,
				      MCE_COLOR_PROFILE_SIG);
	if( !msg )
		goto EXIT;

	if( !dbus_message_append_args(msg,
				      DBUS_TYPE_STRING, &val,
				      DBUS_TYPE_INVALID) )
		goto EXIT;

	dbus_send_message(msg), msg = 0;
EXIT:
	if( msg ) dbus_message_unref(msg);

	return TRUE;
}

/**
 * D-Bus callback for the get color profile method call
 *
 * @param msg The D-Bus message
 * @return TRUE
 */
static gboolean color_profile_get_req_dbus_cb(DBusMessage *const msg)
{
	mce_log(LL_DEVEL, "Received get color profile request from %s",
		mce_dbus_get_message_sender_ident(msg));

	if( dbus_message_get_no_reply(msg) )
		goto EXIT;

	send_current_color_profile(msg);

EXIT:
	return TRUE;
}

/** D-Bus callback for the get color profile ids method call
 *
 * @param msg The D-Bus message
 * @return TRUE
 */
static gboolean color_profile_ids_get_req_dbus_cb(DBusMessage *const msg)
{
	mce_log(LL_DEVEL, "Received list color profiles request from %s",
		mce_dbus_get_message_sender_ident(msg));

	DBusMessage *rsp = 0;
	int cnt = sizeof color_profiles / sizeof *color_profiles;
	const char * const * vec = color_profiles;

	if( dbus_message_get_no_reply(msg) )
		goto EXIT;

	if( !(rsp = dbus_message_new_method_return(msg)) )
		goto EXIT;

	if( !dbus_message_append_args(rsp,
				      DBUS_TYPE_ARRAY,
				      DBUS_TYPE_STRING, &vec, cnt,
				      DBUS_TYPE_INVALID) )
		goto EXIT;

	dbus_send_message(rsp), rsp = 0;

EXIT:
	if( rsp ) dbus_message_unref(rsp);

	return TRUE;
}

/** D-Bus callback for the color profile change method call
 *
 * @param msg The D-Bus message
 * @return TRUE
 */
static gboolean color_profile_change_req_dbus_cb(DBusMessage *const msg)
{
	mce_log(LL_DEVEL, "Received set color profile request from %s",
		mce_dbus_get_message_sender_ident(msg));

	const char  *val = 0;
	DBusError    err = DBUS_ERROR_INIT;
	dbus_bool_t  ack = FALSE;
	DBusMessage *rsp = 0;

	if( !dbus_message_get_args(msg, &err,
				  DBUS_TYPE_STRING, &val,
				  DBUS_TYPE_INVALID)) {
		// XXX: should we return an error instead?
		mce_log(LL_ERR,	"Failed to get argument from %s.%s: %s: %s",
			MCE_REQUEST_IF, MCE_COLOR_PROFILE_CHANGE_REQ,
			err.name, err.message);
	}
	else {
		if( set_color_profile(val) )
			ack = TRUE;
	}

	if( dbus_message_get_no_reply(msg) )
		goto EXIT;

	if( !(rsp = dbus_message_new_method_return(msg)) )
		goto EXIT;

	dbus_message_append_args(rsp,
				 DBUS_TYPE_BOOLEAN, &ack,
				 DBUS_TYPE_INVALID);
EXIT:
	if( rsp ) dbus_send_message(rsp), rsp = 0;

	dbus_error_free(&err);
	return TRUE;
}

/**
 * GConf callback for CPA setting
 *
 * @param gcc Unused
 * @param id Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data Unused
 */
static void color_profile_gconf_cb(GConfClient *const gcc, const guint id,
				   GConfEntry *const entry,
				   gpointer const data)
{
	const GConfValue *gcv = gconf_entry_get_value(entry);

	(void)gcc;
	(void)data;

	/* FIXME: we are changing the setting from setting changed
	 *        callback - make sure this does not backfire in
	 *        some nasty way ...
	 */

	/* Key is unset */
	if (gcv == NULL) {
		mce_log(LL_DEBUG,
			"GConf Key `%s' has been unset",
			gconf_entry_get_key(entry));
		save_color_profile(color_profile_name);
		goto EXIT;
	}

	if (id == color_profile_gconf_id) {
		const gchar *profile = gconf_value_get_string(gcv);

		if (set_color_profile(profile) == FALSE)
			save_color_profile(color_profile_name);
	} else {
		mce_log(LL_WARN,
			"Spurious GConf value received; confused!");
	}

EXIT:
	return;
}

/**
 * Set the current color profile according to requested
 *
 * @param id Name of requested color profile
 * @return TRUE on success, FALSE on failure
 */
static gboolean set_color_profile(const gchar *id)
{
	// NOTE: color profile support dropped, this just a stub

	gboolean status = FALSE;

	if( !id ) {
		goto EXIT;
	}

	if( color_profile_name && !strcmp(color_profile_name, id) ) {
		mce_log(LL_DEBUG, "No change in color profile, ignoring");
		status = TRUE;
		goto EXIT;
	}

	if( !color_profile_exists(id) ) {
		mce_log(LL_WARN, "%s: unsupported color profile", id);
		goto EXIT;
	}

	if( !save_color_profile(id) ) {
		mce_log(LL_WARN, "The current color profile id can't be saved");
		goto EXIT;
	}

	free(color_profile_name), color_profile_name = strdup(id);

	status = TRUE;

EXIT:
	return status;
}

/**
 * Save the profile id into conf file
 *
 * @param id The profile id to save
 * @return TRUE on success, FALSE on failure
 */
static gboolean save_color_profile(const gchar *id)
{
	gboolean status = FALSE;

	if (mce_are_settings_locked() == TRUE) {
		mce_log(LL_WARN,
			"Cannot save current color profile id; backup/restore "
			"or device clear/factory reset pending");
		goto EXIT;
	}

	status = mce_gconf_set_string(MCE_GCONF_DISPLAY_COLOR_PROFILE, id);

EXIT:
	return status;
}

/**
 * Read the default color profile id from conf file
 *
 * @return Pointer to allocated string if success; NULL otherwise
 */
static gchar *read_default_color_profile(void)
{
	return mce_conf_get_string(MCE_CONF_COMMON_GROUP,
				   MCE_CONF_DEFAULT_PROFILE_ID_KEY,
				   NULL);
}

/**
 * Read the current color profile id from conf file
 *
 * @return Pointer to allocated string if success; NULL otherwise
 */
static gchar *read_current_color_profile(void)
{
	gchar *retval = NULL;
	(void)mce_gconf_get_string(MCE_GCONF_DISPLAY_COLOR_PROFILE,
				   &retval);

	/* Treat empty string as NULL */
	if( retval && !*retval )
		g_free(retval), retval = 0;

	return retval;
}

static gboolean init_color_profiles(void)
{
	return TRUE;
}

/**
 * Initialization of saveed color profile during boot
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean init_current_color_profile(void)
{
	gchar *profile = NULL;
	gboolean status = FALSE;

	profile = read_current_color_profile();

	if (profile == NULL)
		profile = read_default_color_profile();

	if (profile != NULL) {
		status = set_color_profile(profile);
		g_free(profile);
	}

	return status;
}

static void quit_color_profiles(void)
{
	free(color_profile_name), color_profile_name = 0;
}

/* ========================================================================= *
 * Module load / unload
 * ========================================================================= */

/** Array of dbus message handlers */
static mce_dbus_handler_t filter_brightness_dbus_handlers[] =
{
	/* signals - outbound (for Introspect purposes only) */
	{
		.interface = MCE_SIGNAL_IF,
		.name      = MCE_COLOR_PROFILE_SIG,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.args      =
			"    <arg name=\"active_color_profile\" type=\"s\"/>\n"
	},
	/* method calls */
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_REQ_ALS_ENABLE,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = als_enable_req_dbus_cb,
		.args      =
			""
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_REQ_ALS_DISABLE,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = als_disable_req_dbus_cb,
		.args      =
			""
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_COLOR_PROFILE_GET,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = color_profile_get_req_dbus_cb,
		.args      =
			"    <arg direction=\"out\" name=\"profile_name\" type=\"s\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_COLOR_PROFILE_IDS_GET,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = color_profile_ids_get_req_dbus_cb,
		.args      =
			"    <arg direction=\"out\" name=\"profile_names\" type=\"as\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_COLOR_PROFILE_CHANGE_REQ,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = color_profile_change_req_dbus_cb,
		.args      =
			"    <arg direction=\"in\" name=\"profile_name\" type=\"s\"/>\n"
			"    <arg direction=\"out\" name=\"success\" type=\"b\"/>\n"
	},
	/* sentinel */
	{
		.interface = 0
	}
};

/** Add dbus handlers
 */
static void mce_filter_brightness_init_dbus(void)
{
	mce_dbus_handler_register_array(filter_brightness_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mce_filter_brightness_quit_dbus(void)
{
	mce_dbus_handler_unregister_array(filter_brightness_dbus_handlers);
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
	(void)module;

	/* Read lux ramps from configuration */
	als_filter_load_config(&lut_display);
	als_filter_load_config(&lut_led);
	als_filter_load_config(&lut_key);
	als_filter_load_config(&lut_lpm);

	/* Get intial display state */
	display_state = display_state_get();

	/* Append triggers/filters to datapipes */
	append_filter_to_datapipe(&display_brightness_pipe,
				  display_brightness_filter);
	append_filter_to_datapipe(&led_brightness_pipe,
				  led_brightness_filter);
	append_filter_to_datapipe(&lpm_brightness_pipe,
				  lpm_brightness_filter);
	append_filter_to_datapipe(&key_backlight_pipe,
				  key_backlight_filter);
	append_output_trigger_to_datapipe(&display_state_pipe,
					  display_state_trigger);

	/* Add dbus method call handlers */
	mce_filter_brightness_init_dbus();

	/* ALS enabled setting */
	mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
			       MCE_GCONF_DISPLAY_ALS_ENABLED,
			       use_als_gconf_cb,
			       &use_als_gconf_id);

	mce_gconf_get_bool(MCE_GCONF_DISPLAY_ALS_ENABLED,
			   &use_als_flag);

	/* Color profile setting */
	mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
			       MCE_GCONF_DISPLAY_COLOR_PROFILE,
			       color_profile_gconf_cb,
			       &color_profile_gconf_id);
	if( init_color_profiles() )
		init_current_color_profile();

	rethink_als_status();

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

	/* Remove gconf notifications  */
	mce_gconf_notifier_remove(use_als_gconf_id),
		use_als_gconf_id = 0;

	mce_gconf_notifier_remove(color_profile_gconf_id),
		color_profile_gconf_id = 0;

	/* Remove dbus handlers */
	mce_filter_brightness_quit_dbus();

	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&display_state_pipe,
					    display_state_trigger);
	remove_filter_from_datapipe(&key_backlight_pipe,
				    key_backlight_filter);
	remove_filter_from_datapipe(&led_brightness_pipe,
				    led_brightness_filter);
	remove_filter_from_datapipe(&lpm_brightness_pipe,
				    lpm_brightness_filter);
	remove_filter_from_datapipe(&display_brightness_pipe,
				    display_brightness_filter);

	quit_color_profiles();

	/* Remove callbacks pointing to unloaded module */
	mce_sensorfw_als_set_notify(0);

	return;
}
