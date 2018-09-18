/**
 * @file datapipe.c
 * This file implements the sinmple datapipe framework;
 * this can be used to filter data and to setup data triggers
 * <p>
 * Copyright © 2007-2008 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2014-2017 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
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

#include "mce.h"
#include "mce-log.h"
#include "mce-lib.h"

#include <mce/mode-names.h>

#include <linux/input.h>

/* Available datapipes */

/** LED brightness */
datapipe_struct led_brightness_pipe;

/** LPM brightness */
datapipe_struct lpm_brightness_pipe;

/** State of device; read only */
datapipe_struct device_inactive_pipe;

/** Device inactivity events; read only */
datapipe_struct inactivity_event_pipe;

/** LED pattern to activate; read only */
datapipe_struct led_pattern_activate_pipe;

/** LED pattern to deactivate; read only */
datapipe_struct led_pattern_deactivate_pipe;

/** resumed from suspend notification; read only */
datapipe_struct resume_detected_event_pipe;

/** Non-synthetized user activity; read only */
datapipe_struct user_activity_event_pipe;

/** State of display; read only */
datapipe_struct display_state_curr_pipe;

/** Desired state of display; write only */
datapipe_struct display_state_request_pipe;

/** Next (non-transitional) state of display; read only */
datapipe_struct display_state_next_pipe;

/** exceptional ui state; read write */
datapipe_struct uiexception_type_pipe;

/**
 * Display brightness;
 * bits 0-7 is brightness in percent (0-100)
 * upper 8 bits is high brightness boost (0-2)
 */
datapipe_struct display_brightness_pipe;

/** Key backlight */
datapipe_struct key_backlight_brightness_pipe;

/** A key has been pressed */
datapipe_struct keypress_event_pipe;

/** Touchscreen activity took place */
datapipe_struct touchscreen_event_pipe;

/** The lock-key has been pressed; read only */
datapipe_struct lockkey_state_pipe;

/** The init-done condition has been reached; read only */
datapipe_struct init_done_pipe;

/** Keyboard open/closed; read only */
datapipe_struct keyboard_slide_state_pipe;

/** Keyboard available; read only */
datapipe_struct keyboard_available_state_pipe;

/** Lid sensor is working state; read/write */
datapipe_struct lid_sensor_is_working_pipe;

/** Lid cover sensor open/closed; read only */
datapipe_struct lid_sensor_actual_pipe;

/** Lid cover policy state; read only */
datapipe_struct lid_sensor_filtered_pipe;

/** Lens cover open/closed; read only */
datapipe_struct lens_cover_state_pipe;

/** Proximity sensor; read only */
datapipe_struct proximity_sensor_actual_pipe;

/** Ambient light sensor; read only */
datapipe_struct light_sensor_actual_pipe;

/** Filtered ambient light level; read only */
datapipe_struct light_sensor_filtered_pipe;

/** Temporary ambient light sensor enable; read/write */
datapipe_struct light_sensor_poll_request_pipe;

/** Orientation sensor; read only */
datapipe_struct orientation_sensor_actual_pipe;

/** The alarm UI state */
datapipe_struct alarm_ui_state_pipe;

/** The device state; read only */
datapipe_struct system_state_pipe;

/** Enable/disable radios */
datapipe_struct master_radio_enabled_pipe;

/** The device submode */
datapipe_struct submode_pipe;

/** The call state */
datapipe_struct call_state_pipe;

/** The ignore incoming call "state".
 *
 * Note: Related actions happen when true is executed on the
 *       datapipe, but the cached value always remains at false.
 */
datapipe_struct ignore_incoming_call_event_pipe;

/** The call type */
datapipe_struct call_type_pipe;

/** The touchscreen/keypad lock state */
datapipe_struct tklock_request_pipe;

/** UI side is in a state where user interaction is expected */
datapipe_struct interaction_expected_pipe;

/** Charger state; read only */
datapipe_struct charger_state_pipe;

/** Battery status; read only */
datapipe_struct battery_status_pipe;

/** Battery charge level; read only */
datapipe_struct battery_level_pipe;

/** Topmost window PID; read only */
datapipe_struct topmost_window_pid_pipe;

/** Camera button; read only */
datapipe_struct camera_button_state_pipe;

/** The inactivity timeout; read only */
datapipe_struct inactivity_delay_pipe;

/** Audio routing state; read only */
datapipe_struct audio_route_pipe;

/** USB cable has been connected/disconnected; read only */
datapipe_struct usb_cable_state_pipe;

/** A jack connector has been connected/disconnected; read only */
datapipe_struct jack_sense_state_pipe;

/** Power save mode is active; read only */
datapipe_struct power_saving_mode_active_pipe;

/** Thermal state; read only */
datapipe_struct thermal_state_pipe;

/** Heartbeat; read only */
datapipe_struct heartbeat_event_pipe;

/** compositor availability; read only */
datapipe_struct compositor_service_state_pipe;

/** lipstick availability; read only */
datapipe_struct lipstick_service_state_pipe;

/** devicelock availability; read only */
datapipe_struct devicelock_service_state_pipe;

/** usbmoded availability; read only */
datapipe_struct usbmoded_service_state_pipe;

/** ngfd availability; read only */
datapipe_struct ngfd_service_state_pipe;

/** Request ngfd event playing */
datapipe_struct ngfd_event_request_pipe;

/** dsme availability; read only */
datapipe_struct dsme_service_state_pipe;

/** bluez availability; read only */
datapipe_struct bluez_service_state_pipe;

/** PackageKit Locked status; read only */
datapipe_struct packagekit_locked_pipe;

/** Update mode active status; read only */
datapipe_struct osupdate_running_pipe;

/** Shutting down; read only */
datapipe_struct shutting_down_pipe;

/** Device Lock state; read only */
datapipe_struct devicelock_state_pipe;

/** touchscreen input detected; read only */
datapipe_struct touch_detected_pipe;

/** touchscreen input grab required; read/write */
datapipe_struct touch_grab_wanted_pipe;

/** touchscreen input grab active; read only */
datapipe_struct touch_grab_active_pipe;

/** keypad input grab required; read/write */
datapipe_struct keypad_grab_wanted_pipe;

/** keypad input grab active; read only */
datapipe_struct keypad_grab_active_pipe;

/** music playback active; read only */
datapipe_struct music_playback_ongoing_pipe;

/** proximity blanking; read only */
datapipe_struct proximity_blanked_pipe;

/** fingerprint daemon availability; read only */
datapipe_struct fpd_service_state_pipe;

/** fingerprint daemon state; read only */
datapipe_struct fpstate_pipe;

/** fingerprint is enrolling; read only */
datapipe_struct enroll_in_progress_pipe;

/**
 * Execute the input triggers of a datapipe
 *
 * @param datapipe The datapipe to execute
 * @param indata The input data to run through the datapipe
 * @param use_cache USE_CACHE to use data from cache,
 *                  USE_INDATA to use indata
 * @param cache_indata CACHE_INDATA to cache the indata,
 *                     DONT_CACHE_INDATA to keep the old data
 */
void datapipe_exec_input_triggers(datapipe_struct *const datapipe,
				  gpointer const indata,
				  const data_source_t use_cache)
{
	void (*trigger)(gconstpointer const input);
	gpointer data;
	gint i;

	if (datapipe == NULL) {
		/* Potential memory leak! */
		mce_log(LL_ERR,
			"datapipe_exec_input_triggers() called "
			"without a valid datapipe");
		goto EXIT;
	}

	data = (use_cache == USE_CACHE) ? datapipe->cached_data : indata;

	for (i = 0; (trigger = g_slist_nth_data(datapipe->input_triggers,
						i)) != NULL; i++) {
		trigger(data);
	}

EXIT:
	return;
}

/**
 * Execute the filters of a datapipe
 *
 * @param datapipe The datapipe to execute
 * @param indata The input data to run through the datapipe
 * @param use_cache USE_CACHE to use data from cache,
 *                  USE_INDATA to use indata
 * @return The processed data
 */
gconstpointer datapipe_exec_filters(datapipe_struct *const datapipe,
				    gpointer indata,
				    const data_source_t use_cache)
{
	gpointer (*filter)(gpointer input);
	gpointer data;
	gconstpointer retval = NULL;
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"datapipe_exec_filters() called "
			"without a valid datapipe");
		goto EXIT;
	}

	data = (use_cache == USE_CACHE) ? datapipe->cached_data : indata;

	for (i = 0; (filter = g_slist_nth_data(datapipe->filters,
					       i)) != NULL; i++) {
		gpointer tmp = filter(data);

		if( datapipe->free_cache == FREE_CACHE ) {
			/* When dealing with dynamic data, the transitional
			 * values need to be released - except for the value
			 * that is cached at the datapipe
			 */
			if( tmp != data && data != datapipe->cached_data )
				g_free(data);
		}

		data = tmp;
	}

	retval = data;

EXIT:
	return retval;
}

/**
 * Execute the output triggers of a datapipe
 *
 * @param datapipe The datapipe to execute
 * @param indata The input data to run through the datapipe
 * @param use_cache USE_CACHE to use data from cache,
 *                  USE_INDATA to use indata
 */
void datapipe_exec_output_triggers(const datapipe_struct *const datapipe,
				   gconstpointer indata,
				   const data_source_t use_cache)
{
	void (*trigger)(gconstpointer input);
	gconstpointer data;
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"datapipe_exec_output_triggers() called "
			"without a valid datapipe");
		goto EXIT;
	}

	data = (use_cache == USE_CACHE) ? datapipe->cached_data : indata;

	for (i = 0; (trigger = g_slist_nth_data(datapipe->output_triggers,
						i)) != NULL; i++) {
		trigger(data);
	}

EXIT:
	return;
}

/**
 * Execute the datapipe
 *
 * @param datapipe The datapipe to execute
 * @param indata The input data to run through the datapipe
 * @param use_cache USE_CACHE to use data from cache,
 *                  USE_INDATA to use indata
 * @param cache_indata CACHE_INDATA to cache the indata,
 *                     DONT_CACHE_INDATA to keep the old data
 * @return The processed data
 */
gconstpointer datapipe_exec_full(datapipe_struct *const datapipe,
				 gpointer indata,
				 const data_source_t use_cache,
				 const caching_policy_t cache_indata)
{
	gconstpointer outdata = NULL;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"datapipe_exec_full() called "
			"without a valid datapipe");
		goto EXIT;
	}

	/* Determine input value */
	if( use_cache == USE_CACHE )
		indata = datapipe->cached_data;

	/* Optionally cache the value at the input stage */
	if( cache_indata & (CACHE_INDATA|CACHE_OUTDATA) ) {
		if( datapipe->free_cache == FREE_CACHE &&
		    datapipe->cached_data != indata )
			g_free(datapipe->cached_data);
		datapipe->cached_data = indata;
	}

	/* Execute input value callbacks */
	datapipe_exec_input_triggers(datapipe, indata, USE_INDATA);

	/* Determine output value */
	if (datapipe->read_only == READ_ONLY) {
		outdata = indata;
	} else {
		outdata = datapipe_exec_filters(datapipe, indata, USE_INDATA);
	}

	/* Optionally cache the value at the output stage */
	if( cache_indata & CACHE_OUTDATA ) {
		if( datapipe->free_cache == FREE_CACHE &&
		    datapipe->cached_data != outdata )
			g_free(datapipe->cached_data);

		datapipe->cached_data = (gpointer)outdata;
	}

	/* Execute output value callbacks */
	datapipe_exec_output_triggers(datapipe, outdata, USE_INDATA);

EXIT:
	return outdata;
}

/**
 * Append a filter to an existing datapipe
 *
 * @param datapipe The datapipe to manipulate
 * @param filter The filter to add to the datapipe
 */
void datapipe_add_filter(datapipe_struct *const datapipe,
			 gpointer (*filter)(gpointer data))
{
	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"datapipe_add_filter() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (filter == NULL) {
		mce_log(LL_ERR,
			"datapipe_add_filter() called "
			"without a valid filter");
		goto EXIT;
	}

	if (datapipe->read_only == READ_ONLY) {
		mce_log(LL_ERR,
			"datapipe_add_filter() called "
			"on read only datapipe");
		goto EXIT;
	}

	datapipe->filters = g_slist_append(datapipe->filters, filter);

EXIT:
	return;
}

/**
 * Remove a filter from an existing datapipe
 * Non-existing filters are ignored
 *
 * @param datapipe The datapipe to manipulate
 * @param filter The filter to remove from the datapipe
 */
void datapipe_remove_filter(datapipe_struct *const datapipe,
			    gpointer (*filter)(gpointer data))
{
	guint oldlen;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"datapipe_remove_filter() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (filter == NULL) {
		mce_log(LL_ERR,
			"datapipe_remove_filter() called "
			"without a valid filter");
		goto EXIT;
	}

	if (datapipe->read_only == READ_ONLY) {
		mce_log(LL_ERR,
			"datapipe_remove_filter() called "
			"on read only datapipe");
		goto EXIT;
	}

	oldlen = g_slist_length(datapipe->filters);

	datapipe->filters = g_slist_remove(datapipe->filters, filter);

	/* Did we remove any entry? */
	if (oldlen == g_slist_length(datapipe->filters)) {
		mce_log(LL_DEBUG,
			"Trying to remove non-existing filter");
		goto EXIT;
	}

EXIT:
	return;
}

/**
 * Append an input trigger to an existing datapipe
 *
 * @param datapipe The datapipe to manipulate
 * @param trigger The trigger to add to the datapipe
 */
void datapipe_add_input_trigger(datapipe_struct *const datapipe,
				void (*trigger)(gconstpointer data))
{
	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"datapipe_add_input_trigger() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"datapipe_add_input_trigger() called "
			"without a valid trigger");
		goto EXIT;
	}

	datapipe->input_triggers = g_slist_append(datapipe->input_triggers,
						  trigger);

EXIT:
	return;
}

/**
 * Remove an input trigger from an existing datapipe
 * Non-existing triggers are ignored
 *
 * @param datapipe The datapipe to manipulate
 * @param trigger The trigger to remove from the datapipe
 */
void datapipe_remove_input_trigger(datapipe_struct *const datapipe,
				   void (*trigger)(gconstpointer data))
{
	guint oldlen;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"datapipe_remove_input_trigger() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"datapipe_remove_input_trigger() called "
			"without a valid trigger");
		goto EXIT;
	}

	oldlen = g_slist_length(datapipe->input_triggers);

	datapipe->input_triggers = g_slist_remove(datapipe->input_triggers,
						  trigger);

	/* Did we remove any entry? */
	if (oldlen == g_slist_length(datapipe->input_triggers)) {
		mce_log(LL_DEBUG,
			"Trying to remove non-existing input trigger");
		goto EXIT;
	}

EXIT:
	return;
}

/**
 * Append an output trigger to an existing datapipe
 *
 * @param datapipe The datapipe to manipulate
 * @param trigger The trigger to add to the datapipe
 */
void datapipe_add_output_trigger(datapipe_struct *const datapipe,
				 void (*trigger)(gconstpointer data))
{
	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"datapipe_add_output_trigger() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"datapipe_add_output_trigger() called "
			"without a valid trigger");
		goto EXIT;
	}

	datapipe->output_triggers = g_slist_append(datapipe->output_triggers,
						   trigger);

EXIT:
	return;
}

/**
 * Remove an output trigger from an existing datapipe
 * Non-existing triggers are ignored
 *
 * @param datapipe The datapipe to manipulate
 * @param trigger The trigger to remove from the datapipe
 */
void datapipe_remove_output_trigger(datapipe_struct *const datapipe,
				    void (*trigger)(gconstpointer data))
{
	guint oldlen;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"datapipe_remove_output_trigger() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"datapipe_remove_output_trigger() called "
			"without a valid trigger");
		goto EXIT;
	}

	oldlen = g_slist_length(datapipe->output_triggers);

	datapipe->output_triggers = g_slist_remove(datapipe->output_triggers,
						   trigger);

	/* Did we remove any entry? */
	if (oldlen == g_slist_length(datapipe->output_triggers)) {
		mce_log(LL_DEBUG,
			"Trying to remove non-existing output trigger");
		goto EXIT;
	}

EXIT:
	return;
}

/**
 * Initialise a datapipe
 *
 * @param datapipe The datapipe to manipulate
 * @param read_only READ_ONLY if the datapipe is read only,
 *                  READ_WRITE if it's read/write
 * @param free_cache FREE_CACHE if the cached data needs to be freed,
 *                   DONT_FREE_CACHE if the cache data should not be freed
 * @param datasize Pass size of memory to copy,
 *		   or 0 if only passing pointers or data as pointers
 * @param initial_data Initial cache content
 */
void datapipe_init(datapipe_struct *const datapipe,
		   const read_only_policy_t read_only,
		   const cache_free_policy_t free_cache,
		   const gsize datasize, gpointer initial_data)
{
	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"datapipe_init() called "
			"without a valid datapipe");
		goto EXIT;
	}

	datapipe->filters = NULL;
	datapipe->input_triggers = NULL;
	datapipe->output_triggers = NULL;
	datapipe->datasize = datasize;
	datapipe->read_only = read_only;
	datapipe->free_cache = free_cache;
	datapipe->cached_data = initial_data;

EXIT:
	return;
}

/**
 * Deinitialize a datapipe
 *
 * @param datapipe The datapipe to manipulate
 */
void datapipe_free(datapipe_struct *const datapipe)
{
	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"datapipe_free() called "
			"without a valid datapipe");
		goto EXIT;
	}

	/* Warn about still registered filters/triggers */
	if (datapipe->filters != NULL) {
		mce_log(LL_INFO,
			"datapipe_free() called on a datapipe that "
			"still has registered filter(s)");
	}

	if (datapipe->input_triggers != NULL) {
		mce_log(LL_INFO,
			"datapipe_free() called on a datapipe that "
			"still has registered input_trigger(s)");
	}

	if (datapipe->output_triggers != NULL) {
		mce_log(LL_INFO,
			"datapipe_free() called on a datapipe that "
			"still has registered output_trigger(s)");
	}

	if (datapipe->free_cache == FREE_CACHE) {
		g_free(datapipe->cached_data);
	}

EXIT:
	return;
}

/** Setup all datapipes
 */
void mce_datapipe_init(void)
{
	datapipe_init(&system_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(MCE_SYSTEM_STATE_UNDEF));
	datapipe_init(&master_radio_enabled_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&call_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(CALL_STATE_NONE));
	datapipe_init(&ignore_incoming_call_event_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(false));
	datapipe_init(&call_type_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(CALL_TYPE_NORMAL));
	datapipe_init(&alarm_ui_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(MCE_ALARM_UI_INVALID_INT32));
	datapipe_init(&submode_pipe, READ_WRITE, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(MCE_SUBMODE_NORMAL));
	datapipe_init(&display_state_curr_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(MCE_DISPLAY_UNDEF));
	datapipe_init(&display_state_request_pipe, READ_WRITE, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(MCE_DISPLAY_UNDEF));
	datapipe_init(&display_state_next_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(MCE_DISPLAY_UNDEF));
	datapipe_init(&uiexception_type_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(UIEXCEPTION_TYPE_NONE));
	datapipe_init(&display_brightness_pipe, READ_WRITE, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(3));
	datapipe_init(&led_brightness_pipe, READ_WRITE, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&lpm_brightness_pipe, READ_WRITE, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&led_pattern_activate_pipe, READ_ONLY, FREE_CACHE,
		      0, NULL);
	datapipe_init(&resume_detected_event_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, NULL);
	datapipe_init(&led_pattern_deactivate_pipe, READ_ONLY, FREE_CACHE,
		      0, NULL);
	datapipe_init(&user_activity_event_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, NULL);
	datapipe_init(&key_backlight_brightness_pipe, READ_WRITE, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&keypress_event_pipe, READ_ONLY, FREE_CACHE,
		      sizeof (struct input_event), NULL);
	datapipe_init(&touchscreen_event_pipe, READ_ONLY, FREE_CACHE,
		      sizeof (struct input_event), NULL);
	datapipe_init(&device_inactive_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(TRUE));
	datapipe_init(&inactivity_event_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(TRUE));
	datapipe_init(&lockkey_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(KEY_STATE_UNDEF));
	datapipe_init(&init_done_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(TRISTATE_UNKNOWN));
	datapipe_init(&keyboard_slide_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(COVER_CLOSED));
	datapipe_init(&keyboard_available_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(COVER_CLOSED));
	datapipe_init(&lid_sensor_is_working_pipe, READ_ONLY,
		      DONT_FREE_CACHE, 0, GINT_TO_POINTER(FALSE));
	datapipe_init(&lid_sensor_actual_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(COVER_UNDEF));
	datapipe_init(&lid_sensor_filtered_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(COVER_UNDEF));
	datapipe_init(&lens_cover_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(COVER_CLOSED));
	datapipe_init(&proximity_sensor_actual_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(COVER_OPEN));
	datapipe_init(&light_sensor_actual_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(400));
	datapipe_init(&light_sensor_filtered_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(400));
	datapipe_init(&light_sensor_poll_request_pipe, READ_WRITE, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(false));
	datapipe_init(&orientation_sensor_actual_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(MCE_ORIENTATION_UNDEFINED));
	datapipe_init(&tklock_request_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(TKLOCK_REQUEST_UNDEF));
	datapipe_init(&interaction_expected_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(false));
	datapipe_init(&charger_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(CHARGER_STATE_UNDEF));
	datapipe_init(&battery_status_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(BATTERY_STATUS_UNDEF));
	datapipe_init(&battery_level_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(BATTERY_LEVEL_INITIAL));
	datapipe_init(&topmost_window_pid_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(-1));
	datapipe_init(&camera_button_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(CAMERA_BUTTON_UNDEF));
	datapipe_init(&inactivity_delay_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(DEFAULT_INACTIVITY_DELAY));
	datapipe_init(&audio_route_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(AUDIO_ROUTE_UNDEF));
	datapipe_init(&usb_cable_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(USB_CABLE_UNDEF));
	datapipe_init(&jack_sense_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(COVER_UNDEF));
	datapipe_init(&power_saving_mode_active_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&thermal_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(THERMAL_STATE_UNDEF));
	datapipe_init(&heartbeat_event_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&compositor_service_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(SERVICE_STATE_UNDEF));
	datapipe_init(&lipstick_service_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(SERVICE_STATE_UNDEF));
	datapipe_init(&devicelock_service_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(SERVICE_STATE_UNDEF));
	datapipe_init(&usbmoded_service_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(SERVICE_STATE_UNDEF));
	datapipe_init(&ngfd_service_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(SERVICE_STATE_UNDEF));
	datapipe_init(&ngfd_event_request_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, NULL);

	datapipe_init(&dsme_service_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(SERVICE_STATE_UNDEF));

	datapipe_init(&bluez_service_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(SERVICE_STATE_UNDEF));

	datapipe_init(&packagekit_locked_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&osupdate_running_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&shutting_down_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&devicelock_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(DEVICELOCK_STATE_UNDEFINED));
	datapipe_init(&touch_detected_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&touch_grab_wanted_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&touch_grab_active_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&keypad_grab_wanted_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&keypad_grab_active_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&music_playback_ongoing_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&proximity_blanked_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&fpd_service_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(SERVICE_STATE_UNDEF));
	datapipe_init(&fpstate_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(FPSTATE_UNSET));
	datapipe_init(&enroll_in_progress_pipe, READ_ONLY, DONT_FREE_CACHE,
		      0, GINT_TO_POINTER(false));
}

/** Free all datapipes
 */
void mce_datapipe_quit(void)
{
	datapipe_free(&thermal_state_pipe);
	datapipe_free(&power_saving_mode_active_pipe);
	datapipe_free(&jack_sense_state_pipe);
	datapipe_free(&usb_cable_state_pipe);
	datapipe_free(&audio_route_pipe);
	datapipe_free(&inactivity_delay_pipe);
	datapipe_free(&battery_level_pipe);
	datapipe_free(&topmost_window_pid_pipe);
	datapipe_free(&camera_button_state_pipe);
	datapipe_free(&battery_status_pipe);
	datapipe_free(&charger_state_pipe);
	datapipe_free(&interaction_expected_pipe);
	datapipe_free(&tklock_request_pipe);
	datapipe_free(&proximity_sensor_actual_pipe);
	datapipe_free(&light_sensor_actual_pipe);
	datapipe_free(&light_sensor_filtered_pipe);
	datapipe_free(&light_sensor_poll_request_pipe);
	datapipe_free(&orientation_sensor_actual_pipe);
	datapipe_free(&lens_cover_state_pipe);
	datapipe_free(&lid_sensor_is_working_pipe);
	datapipe_free(&lid_sensor_actual_pipe);
	datapipe_free(&lid_sensor_filtered_pipe);
	datapipe_free(&keyboard_slide_state_pipe);
	datapipe_free(&keyboard_available_state_pipe);
	datapipe_free(&lockkey_state_pipe);
	datapipe_free(&init_done_pipe);
	datapipe_free(&device_inactive_pipe);
	datapipe_free(&inactivity_event_pipe);
	datapipe_free(&touchscreen_event_pipe);
	datapipe_free(&keypress_event_pipe);
	datapipe_free(&key_backlight_brightness_pipe);
	datapipe_free(&user_activity_event_pipe);
	datapipe_free(&led_pattern_deactivate_pipe);
	datapipe_free(&led_pattern_activate_pipe);
	datapipe_free(&resume_detected_event_pipe);
	datapipe_free(&led_brightness_pipe);
	datapipe_free(&lpm_brightness_pipe);
	datapipe_free(&display_brightness_pipe);
	datapipe_free(&display_state_curr_pipe);
	datapipe_free(&display_state_request_pipe);
	datapipe_free(&display_state_next_pipe);
	datapipe_free(&uiexception_type_pipe);
	datapipe_free(&submode_pipe);
	datapipe_free(&alarm_ui_state_pipe);
	datapipe_free(&call_type_pipe);
	datapipe_free(&call_state_pipe);
	datapipe_free(&ignore_incoming_call_event_pipe);
	datapipe_free(&master_radio_enabled_pipe);
	datapipe_free(&system_state_pipe);
	datapipe_free(&heartbeat_event_pipe);
	datapipe_free(&compositor_service_state_pipe);
	datapipe_free(&lipstick_service_state_pipe);
	datapipe_free(&devicelock_service_state_pipe);
	datapipe_free(&usbmoded_service_state_pipe);
	datapipe_free(&ngfd_service_state_pipe);
	datapipe_free(&ngfd_event_request_pipe);
	datapipe_free(&dsme_service_state_pipe);
	datapipe_free(&bluez_service_state_pipe);
	datapipe_free(&packagekit_locked_pipe);
	datapipe_free(&osupdate_running_pipe);
	datapipe_free(&shutting_down_pipe);
	datapipe_free(&devicelock_state_pipe);
	datapipe_free(&touch_grab_active_pipe);
	datapipe_free(&touch_grab_wanted_pipe);
	datapipe_free(&touch_detected_pipe);
	datapipe_free(&keypad_grab_active_pipe);
	datapipe_free(&keypad_grab_wanted_pipe);
	datapipe_free(&music_playback_ongoing_pipe);
	datapipe_free(&proximity_blanked_pipe);
	datapipe_free(&fpd_service_state_pipe);
	datapipe_free(&fpstate_pipe);
	datapipe_free(&enroll_in_progress_pipe);
}

/** Convert submode_t bitmap changes to human readable string
 *
 * @param prev  submode_t bitmap changed from
 * @param curr  submode_t bitmap changed to
 *
 * @return human readable representation of submode
 */
const char *submode_change_repr(submode_t prev, submode_t curr)
{
	static const struct {
		submode_t bit; const char *name;
	} lut[] = {
		{ MCE_SUBMODE_INVALID,          "INVALID"          },
		{ MCE_SUBMODE_TKLOCK,           "TKLOCK"           },
		{ MCE_SUBMODE_EVEATER,          "EVEATER"          },
		{ MCE_SUBMODE_BOOTUP,           "BOOTUP"           },
		{ MCE_SUBMODE_TRANSITION,       "TRANSITION"       },
		{ MCE_SUBMODE_AUTORELOCK,       "AUTORELOCK"       },
		{ MCE_SUBMODE_VISUAL_TKLOCK,    "VISUAL_TKLOCK"    },
		{ MCE_SUBMODE_POCKET,           "POCKET"           },
		{ MCE_SUBMODE_PROXIMITY_TKLOCK, "PROXIMITY_TKLOCK" },
		{ MCE_SUBMODE_MALF,             "MALF"             },
		{ 0,0 }
	};

	static char buff[128];
	char *pos = buff;
	char *end = buff + sizeof buff - 1;

	auto inline void add(const char *str)
	{
		while( pos < end && *str )
			*pos++ = *str++;
	}

	for( int i = 0; lut[i].name; ++i ) {
		const char *tag = 0;

		if( curr & lut[i].bit ) {
			tag = (prev & lut[i].bit) ? "" : "+";
		}
		else if( prev & lut[i].bit ) {
			tag = "-";
		}

		if( tag ) {
			if( pos> buff ) add(" ");
			add(tag);
			add(lut[i].name);
		}
	}
	if( pos == buff ) {
		add("NONE");
	}
	*pos = 0;
	return buff;
}

/** Convert submode_t bitmap to human readable string
 *
 * @param submode submode_t bitmap
 *
 * @return human readable representation of submode
 */
const char *submode_repr(submode_t submode)
{
  return submode_change_repr(submode, submode);
}

/** Convert system_state_t enum to human readable string
 *
 * @param state system_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *system_state_repr(system_state_t state)
{
	const char *res = "UNKNOWN";

	switch( state ) {
	case MCE_SYSTEM_STATE_UNDEF:    res = "UNDEF";    break;
	case MCE_SYSTEM_STATE_SHUTDOWN: res = "SHUTDOWN"; break;
	case MCE_SYSTEM_STATE_USER:     res = "USER";     break;
	case MCE_SYSTEM_STATE_ACTDEAD:  res = "ACTDEAD";  break;
	case MCE_SYSTEM_STATE_REBOOT:   res = "REBOOT";   break;
	case MCE_SYSTEM_STATE_BOOT:     res = "BOOT";     break;
	default: break;
	}

	return res;
}

/** Convert devicelock_state_t enum to human readable string
 *
 * @param state devicelock_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *devicelock_state_repr(devicelock_state_t state)
{
	const char *res = "unknown";

	switch( state ) {
	case DEVICELOCK_STATE_UNLOCKED:  res = "unlocked";  break;
	case DEVICELOCK_STATE_LOCKED:    res = "locked";    break;
	case DEVICELOCK_STATE_UNDEFINED: res = "undefined"; break;
	default: break;
	}

	return res;
}

/** Convert uiexception_type_t enum to human readable string
 *
 * Note that while uiexception_type_t actually is a bitmask
 * and this function is expected to be used in situations
 * where at maximum one bit is set.
 *
 * @param type uiexception_type_t exception stata
 *
 * @return human readable representation of type
 */
const char *uiexception_type_repr(uiexception_type_t type)
{
	const char *res = "unknown";
	switch( type ) {
	case UIEXCEPTION_TYPE_NONE:   res = "none";   break;
	case UIEXCEPTION_TYPE_LINGER: res = "linger"; break;
	case UIEXCEPTION_TYPE_CALL:   res = "call";   break;
	case UIEXCEPTION_TYPE_ALARM:  res = "alarm";  break;
	case UIEXCEPTION_TYPE_NOTIF:  res = "notif";  break;
	case UIEXCEPTION_TYPE_NOANIM: res = "noanim"; break;
	default: break;
	}
	return res;
}

/** Convert uiexception_type_t enum to dbus argument string
 *
 * Note that while uiexception_type_t actually is a bitmask
 * and this function is expected to be used in situations
 * where at maximum one bit is set.
 *
 * @param type uiexception_type_t exception stata
 *
 * @return representation of type for use over dbus
 */
const char *uiexception_type_to_dbus(uiexception_type_t type)
{
	const char *res = MCE_BLANKING_POLICY_DEFAULT_STRING;

	switch( type ) {
	case UIEXCEPTION_TYPE_NOTIF:
	case UIEXCEPTION_TYPE_NOANIM:
		res = MCE_BLANKING_POLICY_NOTIFICATION_STRING;
		break;

	case UIEXCEPTION_TYPE_ALARM:
		res = MCE_BLANKING_POLICY_ALARM_STRING;
		break;

	case UIEXCEPTION_TYPE_CALL:
		res = MCE_BLANKING_POLICY_CALL_STRING;
		break;

	case UIEXCEPTION_TYPE_LINGER:
		res = MCE_BLANKING_POLICY_LINGER_STRING;
		break;

	case UIEXCEPTION_TYPE_NONE:
		break;

	default:
		mce_log(LL_WARN, "unknown blanking policy; using default");
		break;
	}
	return res;
}

/** Convert service_state_t enum to human readable string
 *
 * @param state service_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *service_state_repr(service_state_t state)
{
	const char *res = "unknown";

	switch( state ) {
	case SERVICE_STATE_UNDEF:   res = "undefined"; break;
	case SERVICE_STATE_STOPPED: res = "stopped";   break;
	case SERVICE_STATE_RUNNING: res = "running";   break;
	default: break;
	}

	return res;
}

/** Convert usb_cable_state_t enum to human readable string
 *
 * @param state usb_cable_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *usb_cable_state_repr(usb_cable_state_t state)
{
	const char *res = "unknown";

	switch( state ) {
	case USB_CABLE_UNDEF:        res = "undefined";    break;
	case USB_CABLE_DISCONNECTED: res = "disconnected"; break;
	case USB_CABLE_CONNECTED:    res = "connected";    break;
	case USB_CABLE_ASK_USER:     res = "ask_user";     break;
	default: break;
	}

	return res;
}

/** Convert usb_cable_state_t enum to dbus argument string
 *
 * @param state usb_cable_state_t enumeration value
 *
 * @return representation of state for use over dbus
 */
const char *usb_cable_state_to_dbus(usb_cable_state_t state)
{
	const char *res = MCE_USB_CABLE_STATE_UNKNOWN;

	switch( state ) {
	case USB_CABLE_DISCONNECTED:
		res = MCE_USB_CABLE_STATE_DISCONNECTED;
		break;

	case USB_CABLE_ASK_USER:
	case USB_CABLE_CONNECTED:
		res = MCE_USB_CABLE_STATE_CONNECTED;
		break;
	default:
		break;
	}

	return res;
}

/** Convert charger_state_t enum to human readable string
 *
 * @param state charger_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *charger_state_repr(charger_state_t state)
{
	const char *res = "unknown";

	switch( state ) {
	case CHARGER_STATE_UNDEF: res = "undefined"; break;
	case CHARGER_STATE_OFF:   res = "off";       break;
	case CHARGER_STATE_ON:    res = "on";        break;
	default: break;
	}

	return res;
}

/** Convert charger_state_t enum to dbus argument string
 *
 * @param state charger_state_t enumeration value
 *
 * @return representation of state for use over dbus
 */
const char *charger_state_to_dbus(charger_state_t state)
{
	const char *res = MCE_CHARGER_STATE_UNKNOWN;

	switch( state ) {
	case CHARGER_STATE_OFF:
		res = MCE_CHARGER_STATE_OFF;
		break;
	case CHARGER_STATE_ON:
		res = MCE_CHARGER_STATE_ON;
		break;
	default:
		break;
	}

	return res;
}

/** Convert tklock_request_t enum to human readable string
 *
 * @param state tklock_request_t enumeration value
 *
 * @return human readable representation of state
 */
const char *tklock_request_repr(tklock_request_t state)
{
	const char *res = "unknown";
	switch( state ) {
	case TKLOCK_REQUEST_UNDEF:         res = "undef";         break;
	case TKLOCK_REQUEST_OFF:           res = "off";           break;
	case TKLOCK_REQUEST_OFF_DELAYED:   res = "off_delayed";   break;
	case TKLOCK_REQUEST_OFF_PROXIMITY: res = "off_proximity"; break;
	case TKLOCK_REQUEST_ON:            res = "on";            break;
	case TKLOCK_REQUEST_ON_DIMMED:     res = "on_dimmed";     break;
	case TKLOCK_REQUEST_ON_PROXIMITY:  res = "on_proximity";  break;
	case TKLOCK_REQUEST_TOGGLE:        res = "toggle";        break;
	case TKLOCK_REQUEST_ON_DELAYED:    res = "on_delayed";    break;
	default: break;
	}
	return res;
}

/** Conver tklock status code from system ui to human readable string
 *
 * @param status TKLOCK_xxx value defined in mode-names.h from mce-dev.
 *
 * @return human readable representation of status
 */
const char *tklock_status_repr(int status)
{
	const char *repr = "TKLOCK_UNKNOWN";

	switch( status ) {
	case TKLOCK_UNLOCK:  repr = "TKLOCK_UNLOCK";  break;
	case TKLOCK_RETRY:   repr = "TKLOCK_RETRY";   break;
	case TKLOCK_TIMEOUT: repr = "TKLOCK_TIMEOUT"; break;
	case TKLOCK_CLOSED:  repr = "TKLOCK_CLOSED";  break;
	default: break;
	}

	return repr;
}

/** Convert battery_status_t enum to human readable string
 *
 * @param state battery_status_t enumeration value
 *
 * @return human readable representation of state
 */
const char *battery_status_repr(battery_status_t state)
{
	const char *res = "unknown";
	switch( state ) {
	case BATTERY_STATUS_UNDEF: res = "undefined"; break;
	case BATTERY_STATUS_FULL:  res = "full";      break;
	case BATTERY_STATUS_OK:    res = "ok";        break;
	case BATTERY_STATUS_LOW:   res = "low";       break;
	case BATTERY_STATUS_EMPTY: res = "empty";     break;
	default: break;
	}
	return res;
}

/** Convert battery_status_t enum to dbus argument string
 *
 * @param state battery_status_t enumeration value
 *
 * @return representation of state for use over dbus
 */
const char *battery_status_to_dbus(battery_status_t state)
{
	const char *res = MCE_BATTERY_STATUS_UNKNOWN;
	switch( state ) {
	case BATTERY_STATUS_FULL:
		res = MCE_BATTERY_STATUS_FULL;
		break;
	case BATTERY_STATUS_OK:
		res = MCE_BATTERY_STATUS_OK;
		break;
	case BATTERY_STATUS_LOW:
		res = MCE_BATTERY_STATUS_LOW;
		break;
	case BATTERY_STATUS_EMPTY:
		res = MCE_BATTERY_STATUS_EMPTY;
		break;
	default:
		break;
	}
	return res;
}

/** Convert alarm_ui_state_t enum to human readable string
 *
 * @param state alarm_ui_state_t enumeration value
 *
 * @return human readable representation of state
 */
const char *alarm_state_repr(alarm_ui_state_t state)
{
	const char *res = "UNKNOWN";

	switch( state )	{
	case MCE_ALARM_UI_INVALID_INT32: res = "INVALID"; break;
	case MCE_ALARM_UI_OFF_INT32:     res = "OFF";     break;
	case MCE_ALARM_UI_RINGING_INT32: res = "RINGING"; break;
	case MCE_ALARM_UI_VISIBLE_INT32: res = "VISIBLE"; break;
	default: break;
	}

	return res;
}

/** Mapping of call state integer <-> call state string */
static const mce_translation_t call_state_translation[] =
{
	{
		.number = CALL_STATE_NONE,
		.string = MCE_CALL_STATE_NONE
	},
	{
		.number = CALL_STATE_RINGING,
		.string = MCE_CALL_STATE_RINGING,
	},
	{
		.number = CALL_STATE_ACTIVE,
		.string = MCE_CALL_STATE_ACTIVE,
	},
	{
		.number = CALL_STATE_SERVICE,
		.string = MCE_CALL_STATE_SERVICE
	},
	{ /* MCE_INVALID_TRANSLATION marks the end of this array */
		.number = MCE_INVALID_TRANSLATION,
		.string = MCE_CALL_STATE_NONE
	}
};

/** MCE call state number to string on D-Bus
 */
const char *call_state_to_dbus(call_state_t state)
{
	return mce_translate_int_to_string(call_state_translation, state);
}

/** String from D-Bus to MCE call state number */
call_state_t call_state_from_dbus(const char *name)
{
	return mce_translate_string_to_int(call_state_translation, name);
}

/** MCE call state number to string (for diagnostic logging)
 */
const char *call_state_repr(call_state_t state)
{
	const char *repr = "invalid";

	switch( state ) {
	case CALL_STATE_NONE:
		repr = MCE_CALL_STATE_NONE;
		break;
	case CALL_STATE_RINGING:
		repr = MCE_CALL_STATE_RINGING;
		break;
	case CALL_STATE_ACTIVE:
		repr = MCE_CALL_STATE_ACTIVE;
		break;
	case CALL_STATE_SERVICE:
		repr = MCE_CALL_STATE_SERVICE;
		break;
	case CALL_STATE_IGNORED:
		repr = "ignored";
		break;
	default:
		break;
	}
	return repr;
}

/** Mapping of call type integer <-> call type string */
static const mce_translation_t call_type_translation[] =
{
	{
		.number = CALL_TYPE_NORMAL,
		.string = MCE_NORMAL_CALL
	},
	{
		.number = CALL_TYPE_EMERGENCY,
		.string = MCE_EMERGENCY_CALL
	},
	{ /* MCE_INVALID_TRANSLATION marks the end of this array */
		.number = MCE_INVALID_TRANSLATION,
		.string = MCE_NORMAL_CALL
	}
};

/** MCE call type number to string
 */
const char *call_type_repr(call_type_t type)
{
	return mce_translate_int_to_string(call_type_translation, type);
}

/** String to MCE call type number
 */
call_type_t call_type_parse(const char *name)
{
	return mce_translate_string_to_int(call_type_translation, name);
}

/** Helper for getting cover_state_t in human readable form
 *
 * @param state cover state enum value
 *
 * @return enum name without "COVER_" prefix
 */
const char *cover_state_repr(cover_state_t state)
{
	const char *res = "UNKNOWN";
	switch( state ) {
	case COVER_UNDEF:  res = "UNDEF";  break;
	case COVER_CLOSED: res = "CLOSED"; break;
	case COVER_OPEN:   res = "OPEN";   break;
	default: break;
	}
	return res;
}

/** Proximity state enum to human readable string
 *
 * @param state Cover state enumeration value
 *
 * @return cover state as human readable proximity state name
 */
const char *proximity_state_repr(cover_state_t state)
{
	const char *repr = "unknown";
	switch( state ) {
	case COVER_UNDEF:  repr = "undefined";   break;
	case COVER_CLOSED: repr = "covered";     break;
	case COVER_OPEN:   repr = "not covered"; break;
	default:
		break;
	}
	return repr;
}

/** Display state to human readable string
 */
const char *display_state_repr(display_state_t state)
{
	const char *repr = "UNKNOWN";

	switch( state ) {
	case MCE_DISPLAY_UNDEF:      repr = "UNDEF";      break;
	case MCE_DISPLAY_OFF:        repr = "OFF";        break;
	case MCE_DISPLAY_LPM_OFF:    repr = "LPM_OFF";    break;
	case MCE_DISPLAY_LPM_ON:     repr = "LPM_ON";     break;
	case MCE_DISPLAY_DIM:        repr = "DIM";        break;
	case MCE_DISPLAY_ON:         repr = "ON";         break;
	case MCE_DISPLAY_POWER_UP:   repr = "POWER_UP";   break;
	case MCE_DISPLAY_POWER_DOWN: repr = "POWER_DOWN"; break;
	default: break;
	}

	return repr;
}

/** Translate orientation state to human readable form */
const char *orientation_state_repr(orientation_state_t state)
{
	const char *name = "UNKNOWN";
	switch( state ) {
	case MCE_ORIENTATION_UNDEFINED:   name = "UNDEFINED";   break;
	case MCE_ORIENTATION_LEFT_UP:     name = "LEFT_UP";     break;
	case MCE_ORIENTATION_RIGHT_UP:    name = "RIGHT_UP";    break;
	case MCE_ORIENTATION_BOTTOM_UP:   name = "BOTTOM_UP";   break;
	case MCE_ORIENTATION_BOTTOM_DOWN: name = "BOTTOM_DOWN"; break;
	case MCE_ORIENTATION_FACE_DOWN:   name = "FACE_DOWN";   break;
	case MCE_ORIENTATION_FACE_UP:     name = "FACE_UP";     break;
	default: break;
	}
	return name;
}

/** Translate key state to human readable form */
const char *key_state_repr(key_state_t state)
{
	const char *repr = "UNKNOWN";
	switch( state ) {
	case KEY_STATE_UNDEF:    repr = "UNDEF";    break;
	case KEY_STATE_RELEASED: repr = "RELEASED"; break;
	case KEY_STATE_PRESSED:  repr = "PRESSED";  break;
	default: break;
	}
	return repr;
}

/** Translate tristate to human readable form */
const char *tristate_repr(tristate_t state)
{
	const char *repr = "invalid";
	switch( state ) {
	case TRISTATE_UNKNOWN: repr = "unknown"; break;
	case TRISTATE_FALSE:   repr = "false";   break;
	case TRISTATE_TRUE:    repr = "true";    break;
	default: break;
	}
	return repr;
}

/** Lookup table for fpstate_t <--> string */
static const mce_translation_t fpstate_lut[] =
{
	{ FPSTATE_UNSET,           "FPSTATE_UNSET"       },
	{ FPSTATE_ENUMERATING,     "FPSTATE_ENUMERATING" },
	{ FPSTATE_IDLE,            "FPSTATE_IDLE"        },
	{ FPSTATE_ENROLLING,       "FPSTATE_ENROLLING"   },
	{ FPSTATE_IDENTIFYING,     "FPSTATE_IDENTIFYING" },
	{ FPSTATE_REMOVING,        "FPSTATE_REMOVING"    },
	{ FPSTATE_VERIFYING,       "FPSTATE_VERIFYING"   },
	{ FPSTATE_ABORTING,        "FPSTATE_ABORTING"    },
	{ FPSTATE_TERMINATING,     "FPSTATE_TERMINATING" },
	// sentinel
	{ MCE_INVALID_TRANSLATION, 0                     }
};

/** Parse fpd state from dbus string */
fpstate_t fpstate_parse(const char *name)
{
	return mce_translate_string_to_int_with_default(fpstate_lut,
							name,
							FPSTATE_UNSET);
}

/** Translate fpd state to human readable form */
const char *fpstate_repr(fpstate_t state)
{
	return mce_translate_int_to_string_with_default(fpstate_lut,
							state,
							"FPSTATE_UNKNOWN");
}

void datapipe_handlers_install(datapipe_handler_t *bindings)
{
    if( !bindings )
	goto EXIT;

    for( size_t i = 0; bindings[i].datapipe; ++i ) {
	if( bindings[i].bound )
	    continue;

	if( bindings[i].filter_cb )
	    datapipe_add_filter(bindings[i].datapipe,
				bindings[i].filter_cb);

	if( bindings[i].input_cb )
	    datapipe_add_input_trigger(bindings[i].datapipe,
				       bindings[i].input_cb);

	if( bindings[i].output_cb )
	    datapipe_add_output_trigger(bindings[i].datapipe,
					bindings[i].output_cb);
	bindings[i].bound = true;
    }

EXIT:
    return;
}

void datapipe_handlers_remove(datapipe_handler_t *bindings)
{
    if( !bindings )
	goto EXIT;

    for( size_t i = 0; bindings[i].datapipe; ++i ) {
	if( !bindings[i].bound )
	    continue;

	if( bindings[i].filter_cb )
	    datapipe_remove_filter(bindings[i].datapipe,
				   bindings[i].filter_cb);

	if( bindings[i].input_cb )
	    datapipe_remove_input_trigger(bindings[i].datapipe,
					  bindings[i].input_cb);

	if( bindings[i].output_cb )
	    datapipe_remove_output_trigger(bindings[i].datapipe,
					   bindings[i].output_cb);
	bindings[i].bound = false;
    }

EXIT:
    return;
}

void datapipe_handlers_execute(datapipe_handler_t *bindings)
{
    if( !bindings )
	goto EXIT;

    for( size_t i = 0; bindings[i].datapipe; ++i ) {
	if( !bindings[i].bound )
	    continue;

	if( bindings[i].output_cb )
	  bindings[i].output_cb(bindings[i].datapipe->cached_data);
    }

EXIT:
    return;
}

static gboolean datapipe_handlers_execute_cb(gpointer aptr)
{
    datapipe_bindings_t *self = aptr;

    if( !self )
	goto EXIT;

    if( !self->execute_id )
	goto EXIT;

    self->execute_id = 0;

    mce_log(LL_INFO, "module=%s", self->module ?: "unknown");
    datapipe_handlers_execute(self->handlers);

EXIT:
    return FALSE;
}

/** Append triggers/filters to datapipes
 */
void datapipe_bindings_init(datapipe_bindings_t *self)
{
    mce_log(LL_INFO, "module=%s", self->module ?: "unknown");

    /* Set up datapipe callbacks */
    datapipe_handlers_install(self->handlers);

    /* Get initial values for output triggers from idle
     * callback, i.e. when all modules have been loaded */
    if( !self->execute_id )
	self->execute_id = g_idle_add(datapipe_handlers_execute_cb, self);
}

/** Remove triggers/filters from datapipes
 */
void datapipe_bindings_quit(datapipe_bindings_t *self)
{
    mce_log(LL_INFO, "module=%s", self->module ?: "unknown");

    /* Remove the get initial values timer if still active */
    if( self->execute_id ) {
	g_source_remove(self->execute_id),
	    self->execute_id = 0;
    }

    /* Remove datapipe callbacks */
    datapipe_handlers_remove(self->handlers);
}
