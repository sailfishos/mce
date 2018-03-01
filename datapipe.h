/**
 * @file datapipe.h
 * Headers for the simple filter framework
 * <p>
 * Copyright © 2007 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _DATAPIPE_H_
#define _DATAPIPE_H_

#include <stdbool.h>
#include <glib.h>

/** Device lock states used in devicelock_state_pipe */
typedef enum
{
    /** Device lock is not active */
    DEVICELOCK_STATE_UNLOCKED  = 0,

    /** Device lock is active */
    DEVICELOCK_STATE_LOCKED    = 1,

    /** Initial startup value; from mce p.o.v. equals not active */
    DEVICELOCK_STATE_UNDEFINED = 2,
}  devicelock_state_t;

const char *devicelock_state_repr(devicelock_state_t state);

/**
 * Datapipe structure
 *
 * Only access this struct through the functions
 */
typedef struct {
	GSList *filters;		/**< The filters */
	GSList *input_triggers;		/**< Triggers called on indata */
	GSList *output_triggers;	/**< Triggers called on outdata */
	gpointer cached_data;		/**< Latest cached data */
	gsize datasize;			/**< Size of data; NULL == automagic */
	gboolean free_cache;		/**< Free the cache? */
	gboolean read_only;		/**< Datapipe is read only */
} datapipe_struct;

/**
 * Read only policy type
 */
typedef enum {
	READ_WRITE = FALSE,		/**< The pipe is read/write */
	READ_ONLY = TRUE		/**< The pipe is read only */
} read_only_policy_t;

/**
 * Policy used for the cache when freeing a datapipe
 */
typedef enum {
	DONT_FREE_CACHE = FALSE,	/**< Don't free the cache */
	FREE_CACHE = TRUE		/**< Free the cache */
} cache_free_policy_t;

/**
 * Policy for the data source
 */
typedef enum {
	USE_INDATA = FALSE,		/**< Use the indata as data source */
	USE_CACHE = TRUE		/**< Use the cache as data source */
} data_source_t;

/**
 * Policy used for caching indata
 */
typedef enum {
	DONT_CACHE_INDATA = 0,          /**< Do not cache the indata */
	CACHE_INDATA      = 1<<0,       /**< Cache the unfiltered indata */
	CACHE_OUTDATA     = 1<<1,       /**< Cache the filtered outdata */
} caching_policy_t;

/* Available datapipes */
extern datapipe_struct led_brightness_pipe;
extern datapipe_struct lpm_brightness_pipe;
extern datapipe_struct device_inactive_pipe;
extern datapipe_struct inactivity_event_pipe;
extern datapipe_struct led_pattern_activate_pipe;
extern datapipe_struct led_pattern_deactivate_pipe;
extern datapipe_struct resume_detected_event_pipe;
extern datapipe_struct user_activity_event_pipe;
extern datapipe_struct display_state_curr_pipe;
extern datapipe_struct display_state_request_pipe;
extern datapipe_struct display_state_next_pipe;
extern datapipe_struct uiexception_type_pipe;
extern datapipe_struct display_brightness_pipe;
extern datapipe_struct key_backlight_brightness_pipe;
extern datapipe_struct keypress_event_pipe;
extern datapipe_struct touchscreen_event_pipe;
extern datapipe_struct lockkey_state_pipe;
extern datapipe_struct keyboard_slide_state_pipe;
extern datapipe_struct keyboard_available_state_pipe;
extern datapipe_struct lid_sensor_is_working_pipe;
extern datapipe_struct lid_sensor_actual_pipe;
extern datapipe_struct lid_sensor_filtered_pipe;
extern datapipe_struct lens_cover_state_pipe;
extern datapipe_struct proximity_sensor_actual_pipe;
extern datapipe_struct light_sensor_actual_pipe;
extern datapipe_struct light_sensor_filtered_pipe;
extern datapipe_struct light_sensor_poll_request_pipe;
extern datapipe_struct orientation_sensor_actual_pipe;
extern datapipe_struct alarm_ui_state_pipe;
extern datapipe_struct system_state_pipe;
extern datapipe_struct master_radio_enabled_pipe;
extern datapipe_struct submode_pipe;
extern datapipe_struct call_state_pipe;
extern datapipe_struct ignore_incoming_call_event_pipe;
extern datapipe_struct call_type_pipe;
extern datapipe_struct tklock_request_pipe;
extern datapipe_struct interaction_expected_pipe;
extern datapipe_struct charger_state_pipe;
extern datapipe_struct battery_status_pipe;
extern datapipe_struct battery_level_pipe;
extern datapipe_struct camera_button_state_pipe;
extern datapipe_struct inactivity_delay_pipe;
extern datapipe_struct audio_route_pipe;
extern datapipe_struct usb_cable_state_pipe;
extern datapipe_struct jack_sense_state_pipe;
extern datapipe_struct power_saving_mode_active_pipe;
extern datapipe_struct thermal_state_pipe;
extern datapipe_struct heartbeat_event_pipe;
extern datapipe_struct compositor_service_state_pipe;
extern datapipe_struct lipstick_service_state_pipe;
extern datapipe_struct devicelock_service_state_pipe;
extern datapipe_struct usbmoded_service_state_pipe;
extern datapipe_struct ngfd_service_state_pipe;
extern datapipe_struct dsme_service_state_pipe;
extern datapipe_struct bluez_service_state_pipe;
extern datapipe_struct packagekit_locked_pipe;
extern datapipe_struct osupdate_running_pipe;
extern datapipe_struct shutting_down_pipe;
extern datapipe_struct devicelock_state_pipe;
extern datapipe_struct touch_detected_pipe;
extern datapipe_struct touch_grab_wanted_pipe;
extern datapipe_struct touch_grab_active_pipe;
extern datapipe_struct keypad_grab_wanted_pipe;
extern datapipe_struct keypad_grab_active_pipe;
extern datapipe_struct music_playback_ongoing_pipe;
extern datapipe_struct proximity_blanked_pipe;
extern datapipe_struct fpd_service_state_pipe;
extern datapipe_struct fpstate_pipe;
extern datapipe_struct enroll_in_progress_pipe;

/* Data retrieval */

/** Retrieve a gboolean from a datapipe */
#define datapipe_get_gbool(_datapipe)	(GPOINTER_TO_INT((_datapipe).cached_data))

/** Retrieve a gint from a datapipe */
#define datapipe_get_gint(_datapipe)	(GPOINTER_TO_INT((_datapipe).cached_data))

/** Retrieve a guint from a datapipe */
#define datapipe_get_guint(_datapipe)	(GPOINTER_TO_UINT((_datapipe).cached_data))

/** Retrieve a gsize from a datapipe */
#define datapipe_get_gsize(_datapipe)	(GPOINTER_TO_SIZE((_datapipe).cached_data))

/** Retrieve a gpointer from a datapipe */
#define datapipe_get_gpointer(_datapipe)	((_datapipe).cached_data)

/* Datapipe execution */
void datapipe_exec_input_triggers(datapipe_struct *const datapipe,
				  gpointer const indata,
				  const data_source_t use_cache);
gconstpointer datapipe_exec_filters(datapipe_struct *const datapipe,
				    gpointer indata,
				    const data_source_t use_cache);
void datapipe_exec_output_triggers(const datapipe_struct *const datapipe,
				   gconstpointer indata,
				   const data_source_t use_cache);
gconstpointer datapipe_exec_full(datapipe_struct *const datapipe,
				 gpointer indata,
				 const data_source_t use_cache,
				 const caching_policy_t cache_indata);

/* Filters */
void datapipe_add_filter(datapipe_struct *const datapipe,
			 gpointer (*filter)(gpointer data));
void datapipe_remove_filter(datapipe_struct *const datapipe,
			    gpointer (*filter)(gpointer data));

/* Input triggers */
void datapipe_add_input_trigger(datapipe_struct *const datapipe,
				void (*trigger)(gconstpointer data));
void datapipe_remove_input_trigger(datapipe_struct *const datapipe,
				   void (*trigger)(gconstpointer data));

/* Output triggers */
void datapipe_add_output_trigger(datapipe_struct *const datapipe,
				 void (*trigger)(gconstpointer data));
void datapipe_remove_output_trigger(datapipe_struct *const datapipe,
				    void (*trigger)(gconstpointer data));

void datapipe_init(datapipe_struct *const datapipe,
		   const read_only_policy_t read_only,
		   const cache_free_policy_t free_cache,
		   const gsize datasize, gpointer initial_data);
void datapipe_free(datapipe_struct *const datapipe);

/* Binding arrays */

typedef struct
{
    datapipe_struct *datapipe;
    void (*output_cb)(gconstpointer data);
    void (*input_cb)(gconstpointer data);
    gpointer (*filter_cb)(gpointer data);
    bool bound;
} datapipe_handler_t;

void datapipe_handlers_install(datapipe_handler_t *bindings);
void datapipe_handlers_remove(datapipe_handler_t *bindings);
void datapipe_handlers_execute(datapipe_handler_t *bindings);

typedef struct
{
    const char         *module;
    datapipe_handler_t *handlers;
    guint               execute_id;
} datapipe_bindings_t;

void datapipe_bindings_init(datapipe_bindings_t *self);
void datapipe_bindings_quit(datapipe_bindings_t *self);

/* Startup / exit */
void mce_datapipe_init(void);
void mce_datapipe_quit(void);

/* Helper for making display state requests
 *
 * This needs to be macro so that logging context stays
 * at the point of call.
 */
#define mce_datapipe_req_display_state(state_) do {\
    display_state_t cur_target = datapipe_get_gint(display_state_next_pipe);\
    display_state_t req_target = (display_state_t)(state_);\
    /* Use elevated logginng verbosity for requests that \
     * are likely to result in display power up. */ \
    int level = LL_DEBUG; \
    if( cur_target != req_target ) {\
	switch( req_target ) {\
	case MCE_DISPLAY_ON:\
	case MCE_DISPLAY_LPM_ON:\
	    level = LL_CRUCIAL;\
	    break;\
	default:\
	    break;\
	}\
    }\
    mce_log(level, "display state req: %s",\
	    display_state_repr(req_target));\
    /* But the request must always be fed to the datapipe \
     * because during already ongoing transition something \
     * else might be already queued up and we want't the \
     * last request to reach the queue to "win". */ \
    datapipe_exec_full(&display_state_request_pipe,\
		       GINT_TO_POINTER(req_target),\
		       USE_INDATA, CACHE_OUTDATA);\
} while(0)

#endif /* _DATAPIPE_H_ */
