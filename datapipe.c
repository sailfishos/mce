/**
 * @file datapipe.c
 * This file implements the sinmple datapipe framework;
 * this can be used to filter data and to setup data triggers
 * <p>
 * Copyright © 2007-2008 Nokia Corporation and/or its subsidiary(-ies).
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

#include "mce.h"
#include "mce-log.h"

#include <linux/input.h>

/* Available datapipes */

/** LED brightness */
datapipe_struct led_brightness_pipe;

/** LPM brightness */
datapipe_struct lpm_brightness_pipe;

/** State of device; read only */
datapipe_struct device_inactive_pipe;

/** LED pattern to activate; read only */
datapipe_struct led_pattern_activate_pipe;

/** LED pattern to deactivate; read only */
datapipe_struct led_pattern_deactivate_pipe;

/** Non-synthetized user activity; read only */
datapipe_struct user_activity_pipe;

/** State of display; read only */
datapipe_struct display_state_pipe;

/** Desired state of display; write only */
datapipe_struct display_state_req_pipe;

/** Next (non-transitional) state of display; read only */
datapipe_struct display_state_next_pipe;

/** exceptional ui state; read write */
datapipe_struct exception_state_pipe;

/**
 * Display brightness;
 * bits 0-7 is brightness in percent (0-100)
 * upper 8 bits is high brightness boost (0-2)
 */
datapipe_struct display_brightness_pipe;

/** Key backlight */
datapipe_struct key_backlight_pipe;

/** A key has been pressed */
datapipe_struct keypress_pipe;

/** Touchscreen activity took place */
datapipe_struct touchscreen_pipe;

/** The lock-key has been pressed; read only */
datapipe_struct lockkey_pipe;

/** Keyboard open/closed; read only */
datapipe_struct keyboard_slide_pipe;

/** Lid cover open/closed; read only */
datapipe_struct lid_cover_pipe;

/** Lens cover open/closed; read only */
datapipe_struct lens_cover_pipe;

/** Proximity sensor; read only */
datapipe_struct proximity_sensor_pipe;

/** Ambient light sensor; read only */
datapipe_struct ambient_light_sensor_pipe;

/** Orientation sensor; read only */
datapipe_struct orientation_sensor_pipe;

/** The alarm UI state */
datapipe_struct alarm_ui_state_pipe;

/** The device state */
datapipe_struct system_state_pipe;

/** Enable/disable radios */
datapipe_struct master_radio_pipe;

/** The device submode */
datapipe_struct submode_pipe;

/** The call state */
datapipe_struct call_state_pipe;

/** The call type */
datapipe_struct call_type_pipe;

/** The touchscreen/keypad lock state */
datapipe_struct tk_lock_pipe;

/** Charger state; read only */
datapipe_struct charger_state_pipe;

/** Battery status; read only */
datapipe_struct battery_status_pipe;

/** Battery charge level; read only */
datapipe_struct battery_level_pipe;

/** Camera button; read only */
datapipe_struct camera_button_pipe;

/** The inactivity timeout; read only */
datapipe_struct inactivity_timeout_pipe;

/** Audio routing state; read only */
datapipe_struct audio_route_pipe;

/** USB cable has been connected/disconnected; read only */
datapipe_struct usb_cable_pipe;

/** A jack connector has been connected/disconnected; read only */
datapipe_struct jack_sense_pipe;

/** Power save mode is active; read only */
datapipe_struct power_saving_mode_pipe;

/** Thermal state; read only */
datapipe_struct thermal_state_pipe;

/** Heartbeat; read only */
datapipe_struct heartbeat_pipe;

/** lipstick availability; read only */
datapipe_struct lipstick_available_pipe;

/** dsme availability; read only */
datapipe_struct dsme_available_pipe;

/** PackageKit Locked status; read only */
datapipe_struct packagekit_locked_pipe;

/** Update mode active status; read only */
datapipe_struct update_mode_pipe;

/** Device Lock active status; read only */
datapipe_struct device_lock_active_pipe;

/** touchscreen input grab required; read/write */
datapipe_struct touch_grab_wanted_pipe;

/** touchscreen input grab active; read only */
datapipe_struct touch_grab_active_pipe;

/** keypad input grab required; read/write */
datapipe_struct keypad_grab_wanted_pipe;

/** keypad input grab active; read only */
datapipe_struct keypad_grab_active_pipe;

/** music playback active; read only */
datapipe_struct music_playback_pipe;

/** proximity blanking; read only */
datapipe_struct proximity_blank_pipe;

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
void execute_datapipe_input_triggers(datapipe_struct *const datapipe,
				     gpointer const indata,
				     const data_source_t use_cache,
				     const caching_policy_t cache_indata)
{
	void (*trigger)(gconstpointer const input);
	gpointer data;
	gint i;

	if (datapipe == NULL) {
		/* Potential memory leak! */
		mce_log(LL_ERR,
			"execute_datapipe_input_triggers() called "
			"without a valid datapipe");
		goto EXIT;
	}

	data = (use_cache == USE_CACHE) ? datapipe->cached_data : indata;

	if (cache_indata == CACHE_INDATA) {
		if (use_cache == USE_INDATA) {
			if (datapipe->free_cache == FREE_CACHE)
				g_free(datapipe->cached_data);

			datapipe->cached_data = data;
		}
	}

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
gconstpointer execute_datapipe_filters(datapipe_struct *const datapipe,
				       gpointer indata,
				       const data_source_t use_cache)
{
	gpointer (*filter)(gpointer input);
	gpointer data;
	gconstpointer retval = NULL;
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"execute_datapipe_filters() called "
			"without a valid datapipe");
		goto EXIT;
	}

	data = (use_cache == USE_CACHE) ? datapipe->cached_data : indata;

	for (i = 0; (filter = g_slist_nth_data(datapipe->filters,
					       i)) != NULL; i++) {
		gpointer tmp = filter(data);

		/* If the data needs to be freed, and this isn't the indata,
		 * or if we're not using the cache, then free the data
		 */
		if ((datapipe->free_cache == FREE_CACHE) &&
		    ((i > 0) || (use_cache == USE_INDATA)))
			g_free(data);

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
void execute_datapipe_output_triggers(const datapipe_struct *const datapipe,
				      gconstpointer indata,
				      const data_source_t use_cache)
{
	void (*trigger)(gconstpointer input);
	gconstpointer data;
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"execute_datapipe_output_triggers() called "
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
gconstpointer execute_datapipe(datapipe_struct *const datapipe,
			       gpointer indata,
			       const data_source_t use_cache,
			       const caching_policy_t cache_indata)
{
	gconstpointer data = NULL;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"execute_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	execute_datapipe_input_triggers(datapipe, indata, use_cache,
					cache_indata);

	if (datapipe->read_only == READ_ONLY) {
		data = indata;
	} else {
		data = execute_datapipe_filters(datapipe, indata, use_cache);
	}

	execute_datapipe_output_triggers(datapipe, data, USE_INDATA);

EXIT:
	return data;
}

/**
 * Append a filter to an existing datapipe
 *
 * @param datapipe The datapipe to manipulate
 * @param filter The filter to add to the datapipe
 */
void append_filter_to_datapipe(datapipe_struct *const datapipe,
			       gpointer (*filter)(gpointer data))
{
	void (*refcount_trigger)(void);
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"append_filter_to_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (filter == NULL) {
		mce_log(LL_ERR,
			"append_filter_to_datapipe() called "
			"without a valid filter");
		goto EXIT;
	}

	if (datapipe->read_only == READ_ONLY) {
		mce_log(LL_ERR,
			"append_filter_to_datapipe() called "
			"on read only datapipe");
		goto EXIT;
	}

	datapipe->filters = g_slist_append(datapipe->filters, filter);

	for (i = 0; (refcount_trigger = g_slist_nth_data(datapipe->refcount_triggers, i)) != NULL; i++) {
		refcount_trigger();
	}

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
void remove_filter_from_datapipe(datapipe_struct *const datapipe,
				 gpointer (*filter)(gpointer data))
{
	void (*refcount_trigger)(void);
	guint oldlen;
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"remove_filter_from_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (filter == NULL) {
		mce_log(LL_ERR,
			"remove_filter_from_datapipe() called "
			"without a valid filter");
		goto EXIT;
	}

	if (datapipe->read_only == READ_ONLY) {
		mce_log(LL_ERR,
			"remove_filter_from_datapipe() called "
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

	for (i = 0; (refcount_trigger = g_slist_nth_data(datapipe->refcount_triggers, i)) != NULL; i++) {
		refcount_trigger();
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
void append_input_trigger_to_datapipe(datapipe_struct *const datapipe,
				      void (*trigger)(gconstpointer data))
{
	void (*refcount_trigger)(void);
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"append_input_trigger_to_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"append_input_trigger_to_datapipe() called "
			"without a valid trigger");
		goto EXIT;
	}

	datapipe->input_triggers = g_slist_append(datapipe->input_triggers,
						  trigger);

	for (i = 0; (refcount_trigger = g_slist_nth_data(datapipe->refcount_triggers, i)) != NULL; i++) {
		refcount_trigger();
	}

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
void remove_input_trigger_from_datapipe(datapipe_struct *const datapipe,
					void (*trigger)(gconstpointer data))
{
	void (*refcount_trigger)(void);
	guint oldlen;
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"remove_input_trigger_from_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"remove_input_trigger_from_datapipe() called "
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

	for (i = 0; (refcount_trigger = g_slist_nth_data(datapipe->refcount_triggers, i)) != NULL; i++) {
		refcount_trigger();
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
void append_output_trigger_to_datapipe(datapipe_struct *const datapipe,
				       void (*trigger)(gconstpointer data))
{
	void (*refcount_trigger)(void);
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"append_output_trigger_to_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"append_output_trigger_to_datapipe() called "
			"without a valid trigger");
		goto EXIT;
	}

	datapipe->output_triggers = g_slist_append(datapipe->output_triggers,
						   trigger);

	for (i = 0; (refcount_trigger = g_slist_nth_data(datapipe->refcount_triggers, i)) != NULL; i++) {
		refcount_trigger();
	}

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
void remove_output_trigger_from_datapipe(datapipe_struct *const datapipe,
					 void (*trigger)(gconstpointer data))
{
	void (*refcount_trigger)(void);
	guint oldlen;
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"remove_output_trigger_from_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"remove_output_trigger_from_datapipe() called "
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

	for (i = 0; (refcount_trigger = g_slist_nth_data(datapipe->refcount_triggers, i)) != NULL; i++) {
		refcount_trigger();
	}

EXIT:
	return;
}

/**
 * Append a reference count trigger to an existing datapipe
 *
 * @param datapipe The datapipe to manipulate
 * @param trigger The trigger to add to the datapipe
 */
void append_refcount_trigger_to_datapipe(datapipe_struct *const datapipe,
					 void (*trigger)(void))
{
	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"append_refcount_trigger_to_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"append_refcount_trigger_to_datapipe() called "
			"without a valid trigger");
		goto EXIT;
	}

	datapipe->refcount_triggers = g_slist_append(datapipe->refcount_triggers, trigger);

EXIT:
	return;
}

/**
 * Remove a reference count trigger from an existing datapipe
 * Non-existing triggers are ignored
 *
 * @param datapipe The datapipe to manipulate
 * @param trigger The trigger to remove from the datapipe
 */
void remove_refcount_trigger_from_datapipe(datapipe_struct *const datapipe,
					   void (*trigger)(void))
{
	guint oldlen;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"remove_refcount_trigger_from_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"remove_refcount_trigger_from_datapipe() called "
			"without a valid trigger");
		goto EXIT;
	}

	oldlen = g_slist_length(datapipe->refcount_triggers);

	datapipe->refcount_triggers = g_slist_remove(datapipe->refcount_triggers, trigger);

	/* Did we remove any entry? */
	if (oldlen == g_slist_length(datapipe->refcount_triggers)) {
		mce_log(LL_DEBUG,
			"Trying to remove non-existing refcount trigger");
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
void setup_datapipe(datapipe_struct *const datapipe,
		    const read_only_policy_t read_only,
		    const cache_free_policy_t free_cache,
		    const gsize datasize, gpointer initial_data)
{
	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"setup_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	datapipe->filters = NULL;
	datapipe->input_triggers = NULL;
	datapipe->output_triggers = NULL;
	datapipe->refcount_triggers = NULL;
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
void free_datapipe(datapipe_struct *const datapipe)
{
	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"free_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	/* Warn about still registered filters/triggers */
	if (datapipe->filters != NULL) {
		mce_log(LL_INFO,
			"free_datapipe() called on a datapipe that "
			"still has registered filter(s)");
	}

	if (datapipe->input_triggers != NULL) {
		mce_log(LL_INFO,
			"free_datapipe() called on a datapipe that "
			"still has registered input_trigger(s)");
	}

	if (datapipe->output_triggers != NULL) {
		mce_log(LL_INFO,
			"free_datapipe() called on a datapipe that "
			"still has registered output_trigger(s)");
	}

	if (datapipe->refcount_triggers != NULL) {
		mce_log(LL_INFO,
			"free_datapipe() called on a datapipe that "
			"still has registered refcount_trigger(s)");
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
	setup_datapipe(&system_state_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_STATE_UNDEF));
	setup_datapipe(&master_radio_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&call_state_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(CALL_STATE_NONE));
	setup_datapipe(&call_type_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(NORMAL_CALL));
	setup_datapipe(&alarm_ui_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_ALARM_UI_INVALID_INT32));
	setup_datapipe(&submode_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_NORMAL_SUBMODE));
	setup_datapipe(&display_state_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_DISPLAY_UNDEF));
	setup_datapipe(&display_state_req_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_DISPLAY_UNDEF));
	setup_datapipe(&display_state_next_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(MCE_DISPLAY_UNDEF));
	setup_datapipe(&exception_state_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(UIEXC_NONE));
	setup_datapipe(&display_brightness_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(3));
	setup_datapipe(&led_brightness_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&lpm_brightness_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&led_pattern_activate_pipe, READ_ONLY, FREE_CACHE,
		       0, NULL);
	setup_datapipe(&led_pattern_deactivate_pipe, READ_ONLY, FREE_CACHE,
		       0, NULL);
	setup_datapipe(&user_activity_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, NULL);
	setup_datapipe(&key_backlight_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&keypress_pipe, READ_ONLY, FREE_CACHE,
		       sizeof (struct input_event), NULL);
	setup_datapipe(&touchscreen_pipe, READ_ONLY, FREE_CACHE,
		       sizeof (struct input_event), NULL);
	setup_datapipe(&device_inactive_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&lockkey_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&keyboard_slide_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&lid_cover_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(COVER_OPEN));
	setup_datapipe(&lens_cover_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&proximity_sensor_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(COVER_OPEN));
	setup_datapipe(&ambient_light_sensor_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(400));
	setup_datapipe(&orientation_sensor_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&tk_lock_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(LOCK_UNDEF));
	setup_datapipe(&charger_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&battery_status_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(BATTERY_STATUS_UNDEF));
	setup_datapipe(&battery_level_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(100));
	setup_datapipe(&camera_button_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(CAMERA_BUTTON_UNDEF));
	setup_datapipe(&inactivity_timeout_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(DEFAULT_INACTIVITY_TIMEOUT));
	setup_datapipe(&audio_route_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(AUDIO_ROUTE_UNDEF));
	setup_datapipe(&usb_cable_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&jack_sense_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&power_saving_mode_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&thermal_state_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(THERMAL_STATE_UNDEF));
	setup_datapipe(&heartbeat_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(0));
	setup_datapipe(&lipstick_available_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&dsme_available_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&packagekit_locked_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&update_mode_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&device_lock_active_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&touch_grab_wanted_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&touch_grab_active_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&keypad_grab_wanted_pipe, READ_WRITE, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&keypad_grab_active_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&music_playback_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
	setup_datapipe(&proximity_blank_pipe, READ_ONLY, DONT_FREE_CACHE,
		       0, GINT_TO_POINTER(FALSE));
}

/** Free all datapipes
 */
void mce_datapipe_quit(void)
{
	free_datapipe(&thermal_state_pipe);
	free_datapipe(&power_saving_mode_pipe);
	free_datapipe(&jack_sense_pipe);
	free_datapipe(&usb_cable_pipe);
	free_datapipe(&audio_route_pipe);
	free_datapipe(&inactivity_timeout_pipe);
	free_datapipe(&battery_level_pipe);
	free_datapipe(&battery_status_pipe);
	free_datapipe(&charger_state_pipe);
	free_datapipe(&tk_lock_pipe);
	free_datapipe(&proximity_sensor_pipe);
	free_datapipe(&ambient_light_sensor_pipe);
	free_datapipe(&orientation_sensor_pipe);
	free_datapipe(&lens_cover_pipe);
	free_datapipe(&lid_cover_pipe);
	free_datapipe(&keyboard_slide_pipe);
	free_datapipe(&lockkey_pipe);
	free_datapipe(&device_inactive_pipe);
	free_datapipe(&touchscreen_pipe);
	free_datapipe(&keypress_pipe);
	free_datapipe(&key_backlight_pipe);
	free_datapipe(&user_activity_pipe);
	free_datapipe(&led_pattern_deactivate_pipe);
	free_datapipe(&led_pattern_activate_pipe);
	free_datapipe(&led_brightness_pipe);
	free_datapipe(&lpm_brightness_pipe);
	free_datapipe(&display_brightness_pipe);
	free_datapipe(&display_state_pipe);
	free_datapipe(&display_state_req_pipe);
	free_datapipe(&display_state_next_pipe);
	free_datapipe(&exception_state_pipe);
	free_datapipe(&submode_pipe);
	free_datapipe(&alarm_ui_state_pipe);
	free_datapipe(&call_type_pipe);
	free_datapipe(&call_state_pipe);
	free_datapipe(&master_radio_pipe);
	free_datapipe(&system_state_pipe);
	free_datapipe(&heartbeat_pipe);
	free_datapipe(&lipstick_available_pipe);
	free_datapipe(&dsme_available_pipe);
	free_datapipe(&packagekit_locked_pipe);
	free_datapipe(&update_mode_pipe);
	free_datapipe(&device_lock_active_pipe);
	free_datapipe(&touch_grab_active_pipe);
	free_datapipe(&touch_grab_wanted_pipe);
	free_datapipe(&keypad_grab_active_pipe);
	free_datapipe(&keypad_grab_wanted_pipe);
	free_datapipe(&music_playback_pipe);
	free_datapipe(&proximity_blank_pipe);
}
