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
	/**
	 * Avago (APDS990x (QPDS-T900)) type sensor */
	PS_TYPE_AVAGO = 2
} ps_type_t;

/** ID for the proximity sensor I/O monitor */
static gconstpointer proximity_sensor_iomon_id = NULL;

/** Path to the proximity sensor device file entry */
static const gchar *ps_device_path = NULL;
/** Path to the proximity sensor enable/disable file entry */
static const gchar *ps_enable_path = NULL;
/** Path to the proximity sensor on/off mode file entry */
static const gchar *ps_onoff_mode_path = NULL;
/** Path to the first proximity sensor calibration point sysfs entry */
static const gchar *ps_calib0_path = NULL;
/** Path to the second proximity sensor calibration point sysfs entry */
static const gchar *ps_calib1_path = NULL;

/** Proximity threshold */
static hysteresis_t *ps_threshold = NULL;

/** Last proximity sensor state */
static cover_state_t old_proximity_sensor_state = COVER_UNDEF;

/** Cached call state */
static call_state_t call_state = CALL_STATE_INVALID;

/** Cached alarm UI state */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_INVALID_INT32;

/** External reference count for proximity sensor */
static guint ps_external_refcount = 0;

/** List of monitored proximity sensor owners */
static GSList *proximity_sensor_owner_monitor_list = NULL;

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

	if (g_access(PS_DEVICE_PATH_AVAGO, R_OK) == 0) {
		ps_type = PS_TYPE_AVAGO;
		ps_device_path = PS_DEVICE_PATH_AVAGO;
		ps_enable_path = PS_PATH_AVAGO_ENABLE;
		ps_onoff_mode_path = PS_PATH_AVAGO_ONOFF_MODE;
	} else if (g_access(PS_DEVICE_PATH_DIPRO, R_OK) == 0) {
		ps_type = PS_TYPE_DIPRO;
		ps_device_path = PS_DEVICE_PATH_DIPRO;
		ps_calib0_path = PS_CALIB_PATH_DIPRO;
		ps_threshold = &dipro_ps_threshold_dipro;
	} else {
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
	if (ps_enable_path != NULL)
		mce_write_string_to_file(ps_enable_path, "1");
}

/**
 * Disable proximity sensor
 */
static void disable_proximity_sensor(void)
{
	if (ps_enable_path != NULL)
		mce_write_string_to_file(ps_enable_path, "0");
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
	if ((ps_calib0_path == NULL) && (ps_calib1_path == NULL))
		goto EXIT;

	/* Retrieve the calibration data from sysinfo */
	if (get_sysinfo_value(PS_CALIB_IDENTIFIER, &tmp, &len) == FALSE) {
		mce_log(LL_ERR,
			"Failed to retrieve calibration data");
		goto EXIT;
	}

	/* the memory properly aligned? */
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
	if (ps_calib0_path != NULL) {
		mce_write_number_string_to_file(ps_calib0_path,
						calib0, NULL, TRUE, TRUE);
	}

	/* Write calibration value 1 */
	if ((ps_calib1_path != NULL) && (count > 1)) {
		mce_write_number_string_to_file(ps_calib1_path,
						calib1, NULL, TRUE, TRUE);
	}

EXIT2:
	free(tmp);

EXIT:
	return;
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

	if (old_proximity_sensor_state == proximity_sensor_state)
		goto EXIT;

	old_proximity_sensor_state = proximity_sensor_state;

	(void)execute_datapipe(&proximity_sensor_pipe,
			       GINT_TO_POINTER(proximity_sensor_state),
			       USE_INDATA, CACHE_INDATA);

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
		proximity_sensor_state = old_proximity_sensor_state;
		goto EXIT;
	}

	if (old_proximity_sensor_state == proximity_sensor_state)
		goto EXIT;

	old_proximity_sensor_state = proximity_sensor_state;

	(void)execute_datapipe(&proximity_sensor_pipe,
			       GINT_TO_POINTER(proximity_sensor_state),
			       USE_INDATA, CACHE_INDATA);

EXIT:
	return FALSE;
}

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

	old_proximity_sensor_state = proximity_sensor_state;

	(void)execute_datapipe(&proximity_sensor_pipe,
			       GINT_TO_POINTER(proximity_sensor_state),
			       USE_INDATA, CACHE_INDATA);

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

	old_proximity_sensor_state = proximity_sensor_state;

	(void)execute_datapipe(&proximity_sensor_pipe,
			       GINT_TO_POINTER(proximity_sensor_state),
			       USE_INDATA, CACHE_INDATA);

EXIT:
	g_free(tmp);

	return;
}

/**
 * Update the proximity monitoring
 */
static void update_proximity_monitor(void)
{
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
	submode_t submode = mce_get_submode_int32();

	if (get_ps_type() == PS_TYPE_NONE)
		goto EXIT;

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
		/* Register proximity sensor I/O monitor */
		if (proximity_sensor_iomon_id == NULL) {
			(void)enable_proximity_sensor();

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
	} else {
		/* Unregister proximity sensor I/O monitor */
		disable_proximity_sensor();
		mce_unregister_io_monitor(proximity_sensor_iomon_id);
		proximity_sensor_iomon_id = NULL;
	}

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
	(void)data;

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
	retval = mce_dbus_owner_monitor_remove(old_name, &proximity_sensor_owner_monitor_list);

	if (retval == -1) {
		mce_log(LL_INFO,
			"Failed to remove name owner monitoring for `%s'",
			old_name);
	} else {
		if ((ps_external_refcount > 0) && (retval == 0)) {
			if (ps_onoff_mode_path != NULL)
				mce_write_number_string_to_file(ps_onoff_mode_path, 1, NULL, TRUE, TRUE);

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
			if (ps_onoff_mode_path != NULL)
				mce_write_number_string_to_file(ps_onoff_mode_path, 0, NULL, TRUE, TRUE);

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
			if (ps_onoff_mode_path != NULL)
				mce_write_number_string_to_file(ps_onoff_mode_path, 1, NULL, TRUE, TRUE);

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

	/* Append triggers/filters to datapipes */
	append_input_trigger_to_datapipe(&call_state_pipe,
					 call_state_trigger);
	append_input_trigger_to_datapipe(&alarm_ui_state_pipe,
					 alarm_ui_state_trigger);
	append_output_trigger_to_datapipe(&display_state_pipe,
					  display_state_trigger);

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

	if (get_ps_type() != PS_TYPE_NONE) {
		/* Calibrate the proximity sensor */
		calibrate_ps();
	}

	if (ps_onoff_mode_path != NULL)
		mce_write_number_string_to_file(ps_onoff_mode_path,
						1, NULL, TRUE, TRUE);

	ps_external_refcount = 0;

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

	/* Unregister I/O monitors */
	mce_unregister_io_monitor(proximity_sensor_iomon_id);

	return;
}
