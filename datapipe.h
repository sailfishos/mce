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
#ifndef  _DATAPIPE_H_
# define _DATAPIPE_H_

# include <stdbool.h>
# include <glib.h>

/* ========================================================================= *
 * Types
 * ========================================================================= */

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
    GSList   *dp_filters;          /**< The filters */
    GSList   *dp_input_triggers;   /**< Triggers called on indata */
    GSList   *dp_output_triggers;  /**< Triggers called on outdata */
    gpointer  dp_cached_data;      /**< Latest cached data */
    gsize     dp_datasize;         /**< Size of data; NULL == automagic */
    gboolean  dp_free_cache;       /**< Free the cache? */
    gboolean  dp_read_only;        /**< Datapipe is read only */
} datapipe_t;

/**
 * Read only policy type
 */
typedef enum {
    DATAPIPE_FILTERING_ALLOWED = FALSE,  /**< The pipe is read/write */
    DATAPIPE_FILTERING_DENIED  = TRUE,   /**< The pipe is read only */
} datapipe_filtering_t;

/**
 * Policy used for the cache when freeing a datapipe
 */
typedef enum {
    DATAPIPE_DATA_LITERAL = FALSE,  /**< Don't free the cache */
    DATAPIPE_DATA_DYNAMIC = TRUE,   /**< Free the cache */
} datapipe_data_t;

/**
 * Policy for the data source
 */
typedef enum {
    DATAPIPE_USE_INDATA = FALSE,  /**< Use the indata as data source */
    DATAPIPE_USE_CACHED = TRUE,   /**< Use the cache as data source */
} datapipe_use_t;

/**
 * Policy used for caching indata
 */
typedef enum {
    DATAPIPE_CACHE_NOTHING = 0,     /**< Do not cache the indata */
    DATAPIPE_CACHE_INDATA  = 1<<0,  /**< Cache the unfiltered indata */
    DATAPIPE_CACHE_OUTDATA = 1<<1,  /**< Cache the filtered outdata */
} datapipe_cache_t;

typedef struct
{
    datapipe_t  *datapipe;
    void       (*output_cb)(gconstpointer data);
    void       (*input_cb)(gconstpointer data);
    gpointer   (*filter_cb)(gpointer data);
    bool         bound;
} datapipe_handler_t;

typedef struct
{
    const char         *module;
    datapipe_handler_t *handlers;
    guint               execute_id;
} datapipe_bindings_t;

/* ========================================================================= *
 * Functions
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * DATAPIPE
 * ------------------------------------------------------------------------- */

void           datapipe_exec_input_triggers  (datapipe_t *const datapipe, gpointer const indata, const datapipe_use_t use_cache);
gconstpointer  datapipe_exec_filters         (datapipe_t *const datapipe, gpointer indata, const datapipe_use_t use_cache);
void           datapipe_exec_output_triggers (const datapipe_t *const datapipe, gconstpointer indata, const datapipe_use_t use_cache);
gconstpointer  datapipe_exec_full            (datapipe_t *const datapipe, gpointer indata, const datapipe_use_t use_cache, const datapipe_cache_t cache_indata);
void           datapipe_add_filter           (datapipe_t *const datapipe, gpointer (*filter)(gpointer data));
void           datapipe_remove_filter        (datapipe_t *const datapipe, gpointer (*filter)(gpointer data));
void           datapipe_add_input_trigger    (datapipe_t *const datapipe, void (*trigger)(gconstpointer data));
void           datapipe_remove_input_trigger (datapipe_t *const datapipe, void (*trigger)(gconstpointer data));
void           datapipe_add_output_trigger   (datapipe_t *const datapipe, void (*trigger)(gconstpointer data));
void           datapipe_remove_output_trigger(datapipe_t *const datapipe, void (*trigger)(gconstpointer data));
void           datapipe_init                 (datapipe_t *const datapipe, const datapipe_filtering_t read_only, const datapipe_data_t free_cache, const gsize datasize, gpointer initial_data);
void           datapipe_free                 (datapipe_t *const datapipe);
void           datapipe_handlers_install     (datapipe_handler_t *bindings);
void           datapipe_handlers_remove      (datapipe_handler_t *bindings);
void           datapipe_handlers_execute     (datapipe_handler_t *bindings);
void           datapipe_bindings_init        (datapipe_bindings_t *self);
void           datapipe_bindings_quit        (datapipe_bindings_t *self);

/* ------------------------------------------------------------------------- *
 * MCE_DATAPIPE
 * ------------------------------------------------------------------------- */

void  mce_datapipe_init(void);
void  mce_datapipe_quit(void);

/* ========================================================================= *
 * Macros
 * ========================================================================= */

/** Retrieve a gboolean from a datapipe */
# define datapipe_get_gbool(_datapipe)  (GPOINTER_TO_INT((_datapipe).dp_cached_data))

/** Retrieve a gint from a datapipe */
# define datapipe_get_gint(_datapipe)   (GPOINTER_TO_INT((_datapipe).dp_cached_data))

/** Retrieve a guint from a datapipe */
# define datapipe_get_guint(_datapipe)  (GPOINTER_TO_UINT((_datapipe).dp_cached_data))

/** Retrieve a gsize from a datapipe */
# define datapipe_get_gsize(_datapipe)  (GPOINTER_TO_SIZE((_datapipe).dp_cached_data))

/** Retrieve a gpointer from a datapipe */
# define datapipe_get_gpointer(_datapipe)       ((_datapipe).dp_cached_data)

/* Helper for making display state requests
 *
 * This needs to be macro so that logging context stays
 * at the point of call.
 */
# define mce_datapipe_request_display_state(state_) do {\
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
                       DATAPIPE_USE_INDATA, DATAPIPE_CACHE_OUTDATA);\
} while(0)

/** Execute tklock request
 *
 * @param tklock_request Value from  tklock_request_t
 */
#define mce_datapipe_request_tklock(tklock_request) do {\
    mce_log(LL_DEBUG, "Requesting tklock=%s",\
            tklock_request_repr(tklock_request));\
    datapipe_exec_full(&tklock_request_pipe,\
                       GINT_TO_POINTER(tklock_request),\
                       DATAPIPE_USE_INDATA, DATAPIPE_CACHE_INDATA);\
}while(0)

/* ========================================================================= *
 * Datapipes
 * ========================================================================= */

/* Available datapipes */
extern datapipe_t led_brightness_pipe;
extern datapipe_t lpm_brightness_pipe;
extern datapipe_t device_inactive_pipe;
extern datapipe_t inactivity_event_pipe;
extern datapipe_t led_pattern_activate_pipe;
extern datapipe_t led_pattern_deactivate_pipe;
extern datapipe_t resume_detected_event_pipe;
extern datapipe_t user_activity_event_pipe;
extern datapipe_t display_state_curr_pipe;
extern datapipe_t display_state_request_pipe;
extern datapipe_t display_state_next_pipe;
extern datapipe_t uiexception_type_pipe;
extern datapipe_t display_brightness_pipe;
extern datapipe_t key_backlight_brightness_pipe;
extern datapipe_t keypress_event_pipe;
extern datapipe_t touchscreen_event_pipe;
extern datapipe_t lockkey_state_pipe;
extern datapipe_t init_done_pipe;
extern datapipe_t keyboard_slide_state_pipe;
extern datapipe_t keyboard_available_state_pipe;
extern datapipe_t lid_sensor_is_working_pipe;
extern datapipe_t lid_sensor_actual_pipe;
extern datapipe_t lid_sensor_filtered_pipe;
extern datapipe_t lens_cover_state_pipe;
extern datapipe_t proximity_sensor_actual_pipe;
extern datapipe_t light_sensor_actual_pipe;
extern datapipe_t light_sensor_filtered_pipe;
extern datapipe_t light_sensor_poll_request_pipe;
extern datapipe_t orientation_sensor_actual_pipe;
extern datapipe_t alarm_ui_state_pipe;
extern datapipe_t system_state_pipe;
extern datapipe_t master_radio_enabled_pipe;
extern datapipe_t submode_pipe;
extern datapipe_t call_state_pipe;
extern datapipe_t ignore_incoming_call_event_pipe;
extern datapipe_t call_type_pipe;
extern datapipe_t tklock_request_pipe;
extern datapipe_t interaction_expected_pipe;
extern datapipe_t charger_state_pipe;
extern datapipe_t battery_status_pipe;
extern datapipe_t battery_level_pipe;
extern datapipe_t topmost_window_pid_pipe;
extern datapipe_t camera_button_state_pipe;
extern datapipe_t inactivity_delay_pipe;
extern datapipe_t audio_route_pipe;
extern datapipe_t usb_cable_state_pipe;
extern datapipe_t jack_sense_state_pipe;
extern datapipe_t power_saving_mode_active_pipe;
extern datapipe_t thermal_state_pipe;
extern datapipe_t heartbeat_event_pipe;
extern datapipe_t compositor_service_state_pipe;
extern datapipe_t lipstick_service_state_pipe;
extern datapipe_t devicelock_service_state_pipe;
extern datapipe_t usbmoded_service_state_pipe;
extern datapipe_t ngfd_service_state_pipe;
extern datapipe_t ngfd_event_request_pipe;
extern datapipe_t dsme_service_state_pipe;
extern datapipe_t bluez_service_state_pipe;
extern datapipe_t packagekit_locked_pipe;
extern datapipe_t osupdate_running_pipe;
extern datapipe_t shutting_down_pipe;
extern datapipe_t devicelock_state_pipe;
extern datapipe_t touch_detected_pipe;
extern datapipe_t touch_grab_wanted_pipe;
extern datapipe_t touch_grab_active_pipe;
extern datapipe_t keypad_grab_wanted_pipe;
extern datapipe_t keypad_grab_active_pipe;
extern datapipe_t music_playback_ongoing_pipe;
extern datapipe_t proximity_blanked_pipe;
extern datapipe_t fpd_service_state_pipe;
extern datapipe_t fpstate_pipe;
extern datapipe_t enroll_in_progress_pipe;

#endif /* _DATAPIPE_H_ */
