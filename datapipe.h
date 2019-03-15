/**
 * @file datapipe.h
 * Headers for the simple filter framework
 * <p>
 * Copyright © 2007 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2014-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author: Simo Piiroinen <simo.piiroinen@jollamobile.com>
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
 * Read only policy type
 */
typedef enum {
    DATAPIPE_FILTERING_ALLOWED = FALSE,  /**< The pipe is read/write */
    DATAPIPE_FILTERING_DENIED  = TRUE,   /**< The pipe is read only */
} datapipe_filtering_t;

/**
 * Policy used for caching indata
 */
typedef enum {
    /** Data is passed through datapipe, but is not cached
     *
     * Suitable for stateless impulse and event data.
     */
    DATAPIPE_CACHE_NOTHING = 0,

    /** Update cache with unfiltered input value
     *
     * The cached value is set to unfiltered input value.
     *
     * Suitable for datapipes designed to be re-run in mind - in practice
     * this would be brightness control pipes where the input is setting
     * value and output is light sensor filtered hw brightness level.
     */
    DATAPIPE_CACHE_INDATA  = 1<<0,

    /** Update cache with filtered input value
     *
     * The cached value is set to filtered input value.
     */
    DATAPIPE_CACHE_OUTDATA = 1<<1,

    /* Update cache both with unfiltered and filtered input value
     *
     * The cached value is updated both before and after filtering.
     *
     * As this is the least likely option to cause differences between
     * direct datapipe polling and following notifications, it should
     * be used unless there is some specific reason not to.
     */
    DATAPIPE_CACHE_DEFAULT = (DATAPIPE_CACHE_INDATA |
                              DATAPIPE_CACHE_OUTDATA),
} datapipe_cache_t;

/**
 * Datapipe structure
 *
 * Only access this struct through the functions
 */
typedef struct datapipe_t datapipe_t;

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

const char     *datapipe_name     (const datapipe_t *self);
gconstpointer   datapipe_value    (const datapipe_t *self);
gconstpointer   datapipe_exec_full_real(datapipe_t *self, gconstpointer indata,
                                   const char *file, const char *func);
void            datapipe_set_value(datapipe_t *self, gconstpointer data);

#define datapipe_exec_full(PIPE_,DATA_)\
   datapipe_exec_full_real(PIPE_,DATA_,__FILE__,__func__)

/* ------------------------------------------------------------------------- *
 * MCE_DATAPIPE
 * ------------------------------------------------------------------------- */

void  mce_datapipe_init         (void);
void  mce_datapipe_quit         (void);
void  mce_datapipe_init_bindings(datapipe_bindings_t *self);
void  mce_datapipe_quit_bindings(datapipe_bindings_t *self);

void  mce_datapipe_generate_activity   (void);
void  mce_datapipe_generate_inactivity (void);

/* ========================================================================= *
 * Macros
 * ========================================================================= */

/** Retrieve a gint from a datapipe */
# define datapipe_get_gint(_datapipe) ((gint)(void*)datapipe_value(&(_datapipe)))

/** Retrieve a guint from a datapipe */
# define datapipe_get_guint(_datapipe) ((guint)(void*)datapipe_value(&(_datapipe)))

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
                       GINT_TO_POINTER(req_target));\
} while(0)

/** Execute tklock request
 *
 * @param tklock_request Value from  tklock_request_t
 */
#define mce_datapipe_request_tklock(tklock_request) do {\
    mce_log(LL_DEBUG, "Requesting tklock=%s",\
            tklock_request_repr(tklock_request));\
    datapipe_exec_full(&tklock_request_pipe,\
                       GINT_TO_POINTER(tklock_request));\
}while(0)

/** Add reason -prefix for executing proximity_sensor_required_pipe request
 *
 * See #proximity_sensor_required_pipe for details.
 */
#define PROXIMITY_SENSOR_REQUIRED_ADD "+"

/** Remove reason -prefix for executing proximity_sensor_required_pipe request
 *
 * See #proximity_sensor_required_pipe for details.
 */
#define PROXIMITY_SENSOR_REQUIRED_REM "-"

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
extern datapipe_t proximity_sensor_effective_pipe;
extern datapipe_t proximity_sensor_required_pipe;
extern datapipe_t proximity_blanked_pipe;
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
extern datapipe_t thermalmanager_service_state_pipe;
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
extern datapipe_t fpd_service_state_pipe;
extern datapipe_t fpstate_pipe;
extern datapipe_t enroll_in_progress_pipe;

#endif /* _DATAPIPE_H_ */
