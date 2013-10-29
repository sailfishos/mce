/**
 * @file proximity.c
 * Proximity sensor module
 * <p>
 * Copyright © 2010-2011 Nokia Corporation and/or its subsidiary(-ies).
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
#include "proximity.h"

#include "mce-gconf.h"
#include "mce-io.h"			/* mce_read_chunk_from_file(),
					 * mce_write_string_to_file(),
					 * mce_write_number_string_to_file(),
					 * mce_register_io_monitor_chunk(),
					 * mce_unregister_io_monitor()
					 */
#include "mce-hal.h"			/* get_sysinfo_value() */
#include "mce-log.h"			/* mce_log(), LL_* */
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
					 * append_input_trigger_to_datapipe(),
					 * remove_input_trigger_from_datapipe()
					 */

#ifdef ENABLE_HYBRIS
# include "../mce-hybris.h"
#endif

#ifdef ENABLE_SENSORFW
# include "mce-sensorfw.h"
#endif

#if 0 // DEBUG: make all logging from this module "critical"
# undef mce_log
# define mce_log(LEV, FMT, ARGS...) \
	mce_log_file(LL_CRIT, __FILE__, __FUNCTION__, FMT , ## ARGS)
#endif

/** Request enabling of proximity sensor; reference counted */
#define MCE_REQ_PS_ENABLE		"req_proximity_sensor_enable"

/** Request disabling of proximity sensor; reference counted */
#define MCE_REQ_PS_DISABLE		"req_proximity_sensor_disable"

/** Maximum number of monitored proximity sensor owners */
#define PS_MAX_MONITORED		16

/** Module name */
#define MODULE_NAME		"proximity"

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

/** Proximity sensor type */
typedef enum {
	/** Sensor type unset */
	PS_TYPE_UNSET = -1,
	/** No sensor available */
	PS_TYPE_NONE = 0,
	/** Dipro (BH1770GLC/SFH7770) type sensor */
	PS_TYPE_DIPRO = 1,
	/** Avago (APDS990x (QPDS-T900)) type sensor */
	PS_TYPE_AVAGO = 2,
	/** Android adaptation via libhybris */
#ifdef ENABLE_HYBRIS
	PS_TYPE_HYBRIS = 3,
#endif
#ifdef ENABLE_SENSORFW
	/* sensors via sensorfw */
	PS_TYPE_SENSORFW = 4,
#endif

} ps_type_t;

/** State of proximity sensor monitoring */
static gboolean proximity_monitor_active = FALSE;

/** ID for the proximity sensor I/O monitor */
static gconstpointer proximity_sensor_iomon_id = NULL;

/** Path to the proximity sensor device file entry */
static const gchar *ps_device_path = NULL;
/** Path to the proximity sensor enable/disable file entry */
static const gchar *ps_enable_path = NULL;
/** Path to the proximity sensor on/off mode file entry */
static output_state_t ps_onoff_mode_output =
{
  .context = "ps_onoff_mode",
  .truncate_file = TRUE,
  .close_on_exit = TRUE,
};
/** Path to the first proximity sensor calibration point sysfs entry */
static output_state_t ps_calib0_output =
{
  .context = "ps_calib0",
  .truncate_file = TRUE,
  .close_on_exit = TRUE,
};
/** Path to the second proximity sensor calibration point sysfs entry */
static output_state_t ps_calib1_output =
{
  .context = "ps_calib1",
  .truncate_file = TRUE,
  .close_on_exit = TRUE,
};

/** Proximity threshold */
static hysteresis_t *ps_threshold = NULL;

/** Last proximity sensor state */
static cover_state_t old_proximity_sensor_state = COVER_UNDEF;

/** Cached call state */
static call_state_t call_state = CALL_STATE_INVALID;

/** Cached alarm UI state */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_INVALID_INT32;

/** Cached display state */
static display_state_t display_state = MCE_DISPLAY_UNDEF;

/** Cached submode state */
static submode_t submode = MCE_NORMAL_SUBMODE;

/** External reference count for proximity sensor */
static guint ps_external_refcount = 0;

/** List of monitored proximity sensor owners */
static GSList *proximity_sensor_owner_monitor_list = NULL;

/** Proximity threshold for the Dipro proximity sensor */
static hysteresis_t dipro_ps_threshold_dipro = {
	/** Rising hysteresis threshold for Dipro */
	.threshold_rising = 80,
	/** Falling hysteresis threshold for Dipro */
	.threshold_falling = 70,
};

/**
 * Get the proximity sensor type
 *
 * @return The sensor-type
 */
static ps_type_t get_ps_type(void)
{
	static ps_type_t ps_type = PS_TYPE_UNSET;

	/* If we have the sensor-type already, return it */
	if (ps_type != PS_TYPE_UNSET)
		goto EXIT;

#ifdef ENABLE_SENSORFW
	if( ps_type == PS_TYPE_UNSET ) {
		// bogus 'if' used to align code flow and keep
		// static analyzers at least semi-happy ...
		ps_type = PS_TYPE_SENSORFW;
	} else
#endif


	if (g_access(PS_DEVICE_PATH_AVAGO, R_OK) == 0) {
		ps_type = PS_TYPE_AVAGO;
		ps_device_path = PS_DEVICE_PATH_AVAGO;
		ps_enable_path = PS_PATH_AVAGO_ENABLE;
		ps_onoff_mode_output.path = PS_PATH_AVAGO_ONOFF_MODE;
	} else if (g_access(PS_DEVICE_PATH_DIPRO, R_OK) == 0) {
		ps_type = PS_TYPE_DIPRO;
		ps_device_path = PS_DEVICE_PATH_DIPRO;
		ps_calib0_output.path = PS_CALIB_PATH_DIPRO;
		ps_threshold = &dipro_ps_threshold_dipro;
	}
#ifdef ENABLE_HYBRIS
	else if (mce_hybris_ps_init()) {
		ps_type = PS_TYPE_HYBRIS;
	}
#endif
	else {
		/* Device either has no proximity sensor,
		 * or handles it through gpio-keys
		 */
		ps_type = PS_TYPE_NONE;
		ps_device_path = NULL;
	}

	errno = 0;

	mce_log(LL_DEBUG, "Proximity sensor-type: %d", ps_type);

EXIT:
	return ps_type;
}

/**
 * Enable proximity sensor
 */
static void enable_proximity_sensor(void)
{
	mce_log(LL_DEBUG, "enable PS input");
	switch (get_ps_type()) {
#ifdef ENABLE_SENSORFW
	case PS_TYPE_SENSORFW:
		mce_sensorfw_ps_enable();
		break;
#endif
#ifdef ENABLE_HYBRIS
	case PS_TYPE_HYBRIS:
		mce_hybris_ps_set_active(1);
		break;
#endif

	default:
		if (ps_enable_path != NULL)
			mce_write_string_to_file(ps_enable_path, "1");
		break;
	}
}

/**
 * Disable proximity sensor
 */
static void disable_proximity_sensor(void)
{
	mce_log(LL_DEBUG, "disable PS input");
	switch (get_ps_type()) {
#ifdef ENABLE_SENSORFW
	case PS_TYPE_SENSORFW:
		mce_sensorfw_ps_disable();
		break;
#endif
#ifdef ENABLE_HYBRIS
	case PS_TYPE_HYBRIS:
		mce_hybris_ps_set_active(0);
		break;
#endif

	default:
		if (ps_enable_path != NULL)
			mce_write_string_to_file(ps_enable_path, "0");
		break;
	}
}

/**
 * Calibrate the proximity sensor using calibration values from CAL
 */
static void calibrate_ps(void)
{
	guint32 calib0 = 0;
	guint32 calib1 = 0;
	guint8 *tmp = NULL;
	gsize count;
	gulong len;

	/* If we don't have any calibration points, don't bother */
	if ((ps_calib0_output.path == NULL) && (ps_calib1_output.path == NULL))
		goto EXIT;

	/* Retrieve the calibration data from sysinfo */
	if (get_sysinfo_value(PS_CALIB_IDENTIFIER, &tmp, &len) == FALSE) {
		mce_log(
#ifdef ENABLE_SYSINFOD_QUERIES
			LL_ERR,
#else
			LL_NOTICE,
#endif
			"Failed to retrieve PS calibration data");
		goto EXIT;
	}

	/* the memory properly aligned? */
	if ((len % sizeof (guint32)) != 0) {
		mce_log(LL_ERR,
			"Invalid PS calibration data returned");
		goto EXIT2;
	}

	count = len / sizeof (guint32);

	/* We don't have any calibration data */
	if (count == 0) {
		mce_log(LL_INFO,
			"No PS calibration data available");
		goto EXIT2;
	}

	switch (count) {
	default:
		mce_log(LL_INFO,
			"Ignored excess PS calibration data");
		/* Fall-through */

	case 2:
		memcpy(&calib1, tmp, sizeof (calib1));
		/* Fall-through */

	case 1:
		memcpy(&calib0, tmp, sizeof (calib0));
		break;
	}

	/* Write calibration value 0 */
	if (ps_calib0_output.path != NULL) {
		mce_write_number_string_to_file(&ps_calib0_output, calib0);
	}

	/* Write calibration value 1 */
	if ((ps_calib1_output.path != NULL) && (count > 1)) {
		mce_write_number_string_to_file(&ps_calib1_output, calib1);
	}

EXIT2:
	free(tmp);

EXIT:
	return;
}

/** Broadcast proximity state within MCE
 *
 * @param state COVER_CLOSED or COVER_OPEN
 */
static void report_proximity(cover_state_t state)
{
	/* Get current proximity datapipe value */
	cover_state_t old_state = datapipe_get_gint(proximity_sensor_pipe);

	/* Execute datapipe if state has changed */
	if( old_state != state ) {
		mce_log(LL_CRIT, "state: %d -> %d", old_state, state);
		execute_datapipe(&proximity_sensor_pipe,
				 GINT_TO_POINTER(state),
				 USE_INDATA, CACHE_INDATA);
	}

	/* Update last-seen proximity state */
	old_proximity_sensor_state = state;
}

/**
 * I/O monitor callback for the proximity sensor (Avago)
 *
 * @param data The new data
 * @param bytes_read Unused
 * @return Always returns FALSE to return remaining chunks (if any)
 */
static gboolean ps_avago_iomon_cb(gpointer data, gsize bytes_read)
{
	cover_state_t proximity_sensor_state = COVER_UNDEF;
	struct avago_ps *ps;

	/* Don't process invalid reads */
	if (bytes_read != sizeof (struct avago_ps)) {
		goto EXIT;
	}

	ps = data;

	if ((ps->status & APDS990X_PS_UPDATED) == 0)
		goto EXIT;

	if (ps->ps != 0)
		proximity_sensor_state = COVER_CLOSED;
	else
		proximity_sensor_state = COVER_OPEN;

	report_proximity(proximity_sensor_state);

EXIT:
	return FALSE;
}

/**
 * I/O monitor callback for the proximity sensor (Dipro)
 *
 * @param data The new data
 * @param bytes_read Unused
 * @return Always returns FALSE to return remaining chunks (if any)
 */
static gboolean ps_dipro_iomon_cb(gpointer data, gsize bytes_read)
{
	cover_state_t proximity_sensor_state = COVER_UNDEF;
	struct dipro_ps *ps;

	/* Don't process invalid reads */
	if (bytes_read != sizeof (struct dipro_ps)) {
		goto EXIT;
	}

	ps = data;

	if (old_proximity_sensor_state == COVER_UNDEF) {
		if (ps->led1 < ps_threshold->threshold_rising)
			proximity_sensor_state = COVER_OPEN;
		else
			proximity_sensor_state = COVER_CLOSED;
	} else if (ps->led1 > ps_threshold->threshold_rising) {
		proximity_sensor_state = COVER_CLOSED;
	} else if (ps->led1 < ps_threshold->threshold_falling) {
		proximity_sensor_state = COVER_OPEN;
	} else {
		goto EXIT;
	}

	report_proximity(proximity_sensor_state);

EXIT:
	return FALSE;
}

/**
 * I/O monitor callback for the proximity sensor (sensorfw)
 *
 * @param covered  proximity sensor covered
 */
#ifdef ENABLE_SENSORFW
static void ps_sensorfw_iomon_cb(bool covered)
{
	cover_state_t proximity_sensor_state = COVER_UNDEF;

	if( covered )
		proximity_sensor_state = COVER_CLOSED;
	else
		proximity_sensor_state = COVER_OPEN;

	report_proximity(proximity_sensor_state);

	return;
}
#endif /* ENABLE_SENSORFW */

/**
 * I/O monitor callback for the proximity sensor (libhybris)
 *
 * @param timestamp event time
 * @param distance  distance from proximity sensor to object
 */

#ifdef ENABLE_HYBRIS
static void ps_hybris_iomon_cb(int64_t timestamp, float distance)
{
	(void)timestamp;

	static const float minval = 2.0f; // [cm]

	cover_state_t proximity_sensor_state = COVER_UNDEF;

	if( distance <= minval )
		proximity_sensor_state = COVER_CLOSED;
	else
		proximity_sensor_state = COVER_OPEN;

	report_proximity(proximity_sensor_state);

	return;
}
#endif /* ENABLE_HYBRIS */

/**
 * Update the proximity state (Avago)
 *
 * @note Only gives reasonable readings when the proximity sensor is enabled
 */
static void update_proximity_sensor_state_avago(void)
{
	cover_state_t proximity_sensor_state;
	struct avago_ps *ps;
	void *tmp = NULL;
	gssize len = sizeof (struct avago_ps);

	if (mce_read_chunk_from_file(ps_device_path, &tmp, &len, 0) == FALSE)
		goto EXIT;

	if (len != sizeof (struct avago_ps)) {
		mce_log(LL_ERR,
			"Short read from `%s'",
			ps_device_path);
		goto EXIT;
	}

	ps = (struct avago_ps *)tmp;

	if ((ps->status & APDS990X_PS_UPDATED) == 0)
		goto EXIT;

	if (ps->ps != 0)
		proximity_sensor_state = COVER_CLOSED;
	else
		proximity_sensor_state = COVER_OPEN;

	report_proximity(proximity_sensor_state);

EXIT:
	g_free(tmp);

	return;
}

/**
 * Update the proximity state (Dipro)
 *
 * @note Only gives reasonable readings when the proximity sensor is enabled
 */
static void update_proximity_sensor_state_dipro(void)
{
	cover_state_t proximity_sensor_state;
	struct dipro_ps *ps;
	void *tmp = NULL;
	gssize len = sizeof (struct dipro_ps);

	if (mce_read_chunk_from_file(ps_device_path, &tmp, &len, 0) == FALSE)
		goto EXIT;

	if (len != sizeof (struct dipro_ps)) {
		mce_log(LL_ERR,
			"Short read from `%s'",
			ps_device_path);
		goto EXIT;
	}

	ps = (struct dipro_ps *)tmp;

	if (ps->led1 < ps_threshold->threshold_rising)
		proximity_sensor_state = COVER_OPEN;
	else
		proximity_sensor_state = COVER_CLOSED;

	report_proximity(proximity_sensor_state);

EXIT:
	g_free(tmp);

	return;
}

/** Enable the proximity monitoring
 */
static void enable_proximity_monitor(void)
{
	/* Already enabled? */
	if( proximity_monitor_active )
		goto EXIT;

	mce_log(LL_DEBUG, "enable PS monitoring");
	proximity_monitor_active = TRUE;

	/* install input processing hooks, update current state */

	switch( get_ps_type() ) {
#ifdef ENABLE_SENSORFW
	case PS_TYPE_SENSORFW:
		mce_sensorfw_ps_set_notify(ps_sensorfw_iomon_cb);
		enable_proximity_sensor();
		goto EXIT;
#endif
#ifdef ENABLE_HYBRIS
	case PS_TYPE_HYBRIS:
		/* hook first, then enable */
		mce_hybris_ps_set_callback(ps_hybris_iomon_cb);
		enable_proximity_sensor();

		/* FIXME: Is there a way to get immediate reading
		 *        via Android libhardware? For now we
		 *        just need to wait for data ... */
		goto EXIT;
#endif
	default:
		break;
	}
	if( !proximity_sensor_iomon_id ) {
		/* enable first, then hook and update current value */
		enable_proximity_sensor();

		/* Register proximity sensor I/O monitor */
		/* FIXME: is code forking the only way to do these? */
		switch (get_ps_type()) {
		case PS_TYPE_AVAGO:
			if ((proximity_sensor_iomon_id = mce_register_io_monitor_chunk(-1, ps_device_path, MCE_IO_ERROR_POLICY_WARN, G_IO_IN | G_IO_PRI | G_IO_ERR, FALSE, ps_avago_iomon_cb, sizeof (struct avago_ps))) == NULL)
				goto EXIT;

			update_proximity_sensor_state_avago();
			break;

		case PS_TYPE_DIPRO:
			if ((proximity_sensor_iomon_id = mce_register_io_monitor_chunk(-1, ps_device_path, MCE_IO_ERROR_POLICY_WARN, G_IO_IN | G_IO_PRI | G_IO_ERR, FALSE, ps_dipro_iomon_cb, sizeof (struct dipro_ps))) == NULL)
				goto EXIT;

			update_proximity_sensor_state_dipro();
			break;
		default:
			break;
		}
	}
EXIT:
	return;

}

/** Disable the proximity monitoring
 */
static void disable_proximity_monitor(void)
{
	/* Already disabled? */
	if( !proximity_monitor_active )
		goto EXIT;

	mce_log(LL_DEBUG, "disable PS monitoring");
	proximity_monitor_active = FALSE;

	/* disable input */
	disable_proximity_sensor();

	/* remove input processing hooks */
	switch( get_ps_type() ) {
#ifdef ENABLE_SENSORFW
	case PS_TYPE_SENSORFW:
		mce_sensorfw_ps_set_notify(0);
		break;
#endif
#ifdef ENABLE_HYBRIS
	case PS_TYPE_HYBRIS:
		mce_hybris_ps_set_callback(0);
		break;
#endif

	default:
		/* Unregister proximity sensor I/O monitor */
		if( proximity_sensor_iomon_id ) {
			mce_unregister_io_monitor(proximity_sensor_iomon_id);
			proximity_sensor_iomon_id = NULL;
		}
		break;
	}
EXIT:
	return;
}


/** Configuration value for use proximity sensor */
static gboolean use_ps_conf_value = TRUE;

/** Configuration change id for use proximity sensor */
static guint use_ps_conf_id = 0;



/**
 * Update the proximity monitoring
 */
static void update_proximity_monitor(void)
{
	static gboolean old_enable = FALSE;

	gboolean enable = FALSE;
	gboolean fake_open = FALSE;


	if (get_ps_type() == PS_TYPE_NONE) {
		fake_open = TRUE;
		goto EXIT;
	}

	if ((display_state == MCE_DISPLAY_ON) ||
	    (display_state == MCE_DISPLAY_LPM_ON) ||
	    (((submode & MCE_TKLOCK_SUBMODE) != 0) &&
	     ((display_state == MCE_DISPLAY_OFF) ||
	      (display_state == MCE_DISPLAY_LPM_OFF))) ||
	    (ps_external_refcount > 0) ||
	    (call_state == CALL_STATE_RINGING) ||
	    (call_state == CALL_STATE_ACTIVE) ||
	    (alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32) ||
	    (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32)) {
		enable = TRUE;
	}

	if( !use_ps_conf_value ) {
		fake_open = TRUE;
		enable = FALSE;
	}

	if( old_enable == enable )
		goto EXIT;

	if( (old_enable = enable) ) {
		enable_proximity_monitor();
	} else {
		disable_proximity_monitor();
	}

EXIT:
	if( !enable && fake_open )
		report_proximity(COVER_OPEN);
	return;
}

/** GConf callback for use proximity sensor setting
 *
 * @param gcc   (not used)
 * @param id    Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data  (not used)
 */
static void use_ps_conf_cb(GConfClient *const gcc, const guint id,
			   GConfEntry *const entry, gpointer const data)
{
	(void)gcc; (void)data;

	const GConfValue *gcv;

	if( id != use_ps_conf_id ) {
		mce_log(LL_WARN, "Spurious GConf value received; confused!");
		goto EXIT;
	}

	if( !(gcv = gconf_entry_get_value(entry)) ) {
		// config removed -> use proximity sensor
		use_ps_conf_value = TRUE;
	}
	else {
		use_ps_conf_value = gconf_value_get_bool(gcv);
	}

	update_proximity_monitor();
EXIT:
	return;
}

/**
 * Handle call state change
 *
 * @param data The call state stored in a pointer
 */
static void call_state_trigger(gconstpointer const data)
{
	call_state = GPOINTER_TO_INT(data);

	update_proximity_monitor();
}

/**
 * Handle alarm ui state change
 *
 * @param data The alarm state stored in a pointer
 */
static void alarm_ui_state_trigger(gconstpointer const data)
{
	alarm_ui_state = GPOINTER_TO_INT(data);

	update_proximity_monitor();
}

/**
 * Handle display state change
 *
 * @param data The display state stored in a pointer
 */
static void display_state_trigger(gconstpointer data)
{
	display_state = GPOINTER_TO_INT(data);

	update_proximity_monitor();
}

/** Handle submode change
 *
 * @param data The submode stored in a pointer
 */
static void submode_trigger(gconstpointer data)
{
	submode = GPOINTER_TO_INT(data);

	update_proximity_monitor();
}
/**
 * D-Bus callback used for reference counting proximity sensor enabling;
 * if the requesting process exits, immediately decrease the refcount
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean proximity_sensor_owner_monitor_dbus_cb(DBusMessage *const msg)
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

	/* Remove the name monitor for the proximity sensor owner */
	retval = mce_dbus_owner_monitor_remove(service, &proximity_sensor_owner_monitor_list);

	if (retval == -1) {
		mce_log(LL_INFO,
			"Failed to remove name owner monitoring for `%s'",
			service);
	} else {
		if ((ps_external_refcount > 0) && (retval == 0)) {
			if (ps_onoff_mode_output.path != NULL)
				mce_write_number_string_to_file(&ps_onoff_mode_output, 1);

			update_proximity_monitor();
		}

		ps_external_refcount = retval;
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus callback for the proximity sensor enabling method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean proximity_sensor_enable_req_dbus_cb(DBusMessage *const msg)
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
			"Received invalid proximity sensor enable request "
			"(sender == NULL)");
		goto EXIT;
	}

	mce_log(LL_DEBUG,
		"Received proximity sensor enable request from %s",
		sender);

	retval = mce_dbus_owner_monitor_add(sender, proximity_sensor_owner_monitor_dbus_cb, &proximity_sensor_owner_monitor_list, PS_MAX_MONITORED);

	if (retval == -1) {
		mce_log(LL_INFO,
			"Failed to add name owner monitoring for `%s'",
			sender);
	} else {
		if ((ps_external_refcount == 0) && (retval == 1)) {
			if (ps_onoff_mode_output.path != NULL)
				mce_write_number_string_to_file(&ps_onoff_mode_output, 0);
			update_proximity_monitor();
		}

		ps_external_refcount = retval;
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
 * D-Bus callback for the proximity sensor disabling method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean proximity_sensor_disable_req_dbus_cb(DBusMessage *const msg)
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
			"Received invalid proximity sensor disable request "
			"(sender == NULL)");
		goto EXIT;
	}

	mce_log(LL_DEBUG,
		"Received proximity sensor disable request from %s",
		sender);

	retval = mce_dbus_owner_monitor_remove(sender,
					       &proximity_sensor_owner_monitor_list);

	if (retval == -1) {
		mce_log(LL_INFO,
			"Failed to remove name owner monitoring for `%s'",
			sender);
	} else {
		if ((ps_external_refcount > 0) && (retval == 0)) {
			if (ps_onoff_mode_output.path != NULL)
				mce_write_number_string_to_file(&ps_onoff_mode_output, 1);

			update_proximity_monitor();
		}

		ps_external_refcount = retval;
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
 * Init function for the proximity sensor module
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

	/* Get initial state of datapipes */
	call_state = datapipe_get_gint(call_state_pipe);
        alarm_ui_state = datapipe_get_gint(alarm_ui_state_pipe);
        display_state = display_state_get();
        submode = datapipe_get_gint(submode_pipe);

	/* Append triggers/filters to datapipes */
	append_input_trigger_to_datapipe(&call_state_pipe,
					 call_state_trigger);
	append_input_trigger_to_datapipe(&alarm_ui_state_pipe,
					 alarm_ui_state_trigger);
	append_output_trigger_to_datapipe(&display_state_pipe,
					  display_state_trigger);
	append_output_trigger_to_datapipe(&submode_pipe,
					  submode_trigger);

	/* req_proximity_sensor_enable */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_REQ_PS_ENABLE,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 proximity_sensor_enable_req_dbus_cb) == NULL)
		goto EXIT;

	/* req_proximity_sensor_disable */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_REQ_PS_DISABLE,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 proximity_sensor_disable_req_dbus_cb) == NULL)
		goto EXIT;

	/* PS enabled setting */
	mce_gconf_notifier_add(MCE_GCONF_PROXIMITY_PATH,
			       MCE_GCONF_PROXIMITY_PS_ENABLED_PATH,
			       use_ps_conf_cb,
			       &use_ps_conf_id);
	mce_gconf_get_bool(MCE_GCONF_PROXIMITY_PS_ENABLED_PATH,
			   &use_ps_conf_value);

	if (get_ps_type() != PS_TYPE_NONE) {
		/* Calibrate the proximity sensor */
		calibrate_ps();
	}

	if (ps_onoff_mode_output.path != NULL)
		mce_write_number_string_to_file(&ps_onoff_mode_output, 1);

	ps_external_refcount = 0;

	/* enable/disable sensor based on initial conditions */
	update_proximity_monitor();

EXIT:
	return NULL;
}

/**
 * Exit function for the proximity sensor module
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&display_state_pipe,
					    display_state_trigger);
	remove_input_trigger_from_datapipe(&alarm_ui_state_pipe,
					   alarm_ui_state_trigger);
	remove_input_trigger_from_datapipe(&call_state_pipe,
					   call_state_trigger);
	remove_output_trigger_from_datapipe(&submode_pipe,
					    submode_trigger);

	/* Unregister I/O monitors */
	mce_unregister_io_monitor(proximity_sensor_iomon_id);

	/* Disable proximity monitoring to remove callbacks
	 * to unloaded module */
	disable_proximity_monitor();
	return;
}
