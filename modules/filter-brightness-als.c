/**
 * @file filter-brightness-als.c
 * Ambient Light Sensor level adjusting filter module
 * for display backlight, key backlight, and LED brightness
 * This file implements a filter module for MCE
 * <p>
 * Copyright © 2007-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright © 2012-2015 Jolla Ltd.
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

#include "../mce-log.h"
#include "../mce-io.h"
#include "../mce-conf.h"
#include "../mce-setting.h"
#include "../mce-dbus.h"
#include "../mce-sensorfw.h"
#include "../mce-wakelock.h"
#include "../tklock.h"

#include <string.h>

#include <mce/dbus-names.h>

#include <gmodule.h>

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** Module name */
#define MODULE_NAME                     "filter-brightness-als"

/** Maximum number of Lux->Brightness curves that can be defined in config
 *
 * Using odd number means the brightness setting values 1, 50 and 100 will
 * be mapped to the 1st, the middle and the last of the profiles available.
 *
 * Using 21 equals rouhgly: 1 automatic curve / 5 brightness setting steps.
 */
#define FBA_PROFILE_COUNT               21

/** Maximum number of steps Lux->Brightness curves can have
 *
 * Using 21 allows: Covering the 1-100% brightness range in < 5% steps.
 */
#define FBA_PROFILE_STEPS               21

/** Size of the median filtering window; code expects the value to be odd */
#define FBA_INPUTFLT_MEDIAN_SIZE        9

/** Duration of temporary ALS enable sessions */
#define FBA_SENSORPOLL_DURATION_MS      5000

/* ========================================================================= *
 * FUNCTIONALITY
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * MISC_UTILS
 * ------------------------------------------------------------------------- */

static int  fba_util_imin   (int a, int b);
static bool fba_util_streq  (const char *s1, const char *s2);

/* ------------------------------------------------------------------------- *
 * DYNAMIC_SETTINGS
 * ------------------------------------------------------------------------- */

static void fba_setting_cb    (GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);
static void fba_setting_init  (void);
static void fba_setting_quit  (void);

/* ------------------------------------------------------------------------- *
 * COLOR_PROFILE
 * ------------------------------------------------------------------------- */

static bool      fba_color_profile_exists      (const char *id);
static bool      fba_color_profile_set         (const gchar *id);

static void      fba_color_profile_init        (void);
static void      fba_color_profile_quit        (void);

/* ------------------------------------------------------------------------- *
 * SENSOR_INPUT_FILTERING
 * ------------------------------------------------------------------------- */

/** Hooks that an input filter backend needs to implement */
typedef struct
{
    const char *fi_name;

    void      (*fi_reset)(void);
    int       (*fi_filter)(int);
    bool      (*fi_stable)(void);
} fba_inputflt_t;

// INPUT_FILTER_BACKEND_DUMMY
static int      fba_inputflt_dummy_filter    (int add);
static bool     fba_inputflt_dummy_stable    (void);
static void     fba_inputflt_dummy_reset     (void);

// INPUT_FILTER_BACKEND_MEDIAN
static int      fba_inputflt_median_filter   (int add);
static bool     fba_inputflt_median_stable   (void);
static void     fba_inputflt_median_reset    (void);

// INPUT_FILTER_FRONTEND
static void     fba_inputflt_reset           (void);
static int      fba_inputflt_filter          (int lux);
static bool     fba_inputflt_stable          (void);
static void     fba_inputflt_select          (const char *name);
static void     fba_inputflt_flush_on_change (void);
static void     fba_inputflt_sampling_output (int lux);
static gboolean fba_inputflt_sampling_cb     (gpointer aptr);
static void     fba_inputflt_sampling_start  (void);
static void     fba_inputflt_sampling_stop   (void);

static int      fba_inputflt_sampling_time   (void);
static void     fba_inputflt_sampling_input  (int lux);

static void     fba_inputflt_init            (void);
static void     fba_inputflt_quit            (void);

/* ------------------------------------------------------------------------- *
 * ALS_FILTER
 * ------------------------------------------------------------------------- */

/** A step in ALS ramp */
typedef struct
{
    int lux; /**< upper lux limit */
    int val; /**< brightness percentage to use */
} fba_als_limit_t;

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
    int prof;

    /** Latest brightness percentage result */
    int val;

    /** Brightness percent from lux value look up table */
    fba_als_limit_t lut[FBA_PROFILE_COUNT][FBA_PROFILE_STEPS+1];
} fba_als_filter_t;

static void fba_als_filter_clear_threshold (fba_als_filter_t *self);
static bool fba_als_filter_load_profile    (fba_als_filter_t *self, const char *grp, int prof);
static void fba_als_filter_reset_profiles  (fba_als_filter_t *self);
static void fba_als_filter_load_profiles   (fba_als_filter_t *self);
static int  fba_als_filter_get_lux         (fba_als_filter_t *self, int prof, int slot);
static int  fba_als_filter_run             (fba_als_filter_t *self, int prof, int lux);

static void fba_als_filter_init            (void);

/* ------------------------------------------------------------------------- *
 * DATAPIPE_TRACKING
 * ------------------------------------------------------------------------- */

static gpointer fba_datapipe_display_brightness_filter  (gpointer data);
static gpointer fba_datapipe_led_brightness_filter      (gpointer data);
static gpointer fba_datapipe_lpm_brightness_filter      (gpointer data);
static gpointer fba_datapipe_key_backlight_brightness_filter(gpointer data);
static gpointer fba_datapipe_light_sensor_poll_request_filter(gpointer data);

static void     fba_datapipe_display_state_curr_trigger (gconstpointer data);
static void     fba_datapipe_display_state_next_trigger (gconstpointer data);

static void     fba_datapipe_execute_brightness_change  (void);

static void     fba_datapipe_init                       (void);
static void     fba_datapipe_quit                       (void);

/* ------------------------------------------------------------------------- *
 * DBUS_HANDLERS
 * ------------------------------------------------------------------------- */

static void     fba_dbus_send_current_color_profile  (DBusMessage *method_call);

static gboolean fba_dbus_get_color_profile_cb        (DBusMessage *const msg);
static gboolean fba_dbus_get_color_profiles_cb       (DBusMessage *const msg);
static gboolean fba_dbus_set_color_profile_cb        (DBusMessage *const msg);

static void     fba_dbus_init                        (void);
static void     fba_dbus_quit                        (void);

/* ------------------------------------------------------------------------- *
 * SENSOR_STATUS
 * ------------------------------------------------------------------------- */

static void fba_status_sensor_value_change_cb    (int lux);

static bool fba_status_sensor_is_needed          (void);
static void fba_status_rethink                   (void);

/* ------------------------------------------------------------------------- *
 * SENSOR_POLL
 * ------------------------------------------------------------------------- */

static gboolean fba_sensorpoll_timer_cb  (gpointer aptr);
static void     fba_sensorpoll_start     (void);
static void     fba_sensorpoll_stop      (void);
static void     fba_sensorpoll_rethink   (void);

/* ------------------------------------------------------------------------- *
 * LOAD_UNLOAD
 * ------------------------------------------------------------------------- */

G_MODULE_EXPORT const gchar *g_module_check_init (GModule *module);
G_MODULE_EXPORT void         g_module_unload     (GModule *module);

/** Flag for: The plugin is about to be unloaded */
static bool fba_module_unload = false;

/* ========================================================================= *
 * MISC_UTILS
 * ========================================================================= */

/** Integer minimum helper in style of fminf()
 *
 * @param a  integer value
 * @param b  integer value
 *
 * @return minimum of the two values
 */
static int fba_util_imin(int a, int b)
{
    return (a < b) ? a : b;
}

/* Null tolerant string equality predicate
 *
 * @param s1  string
 * @param s2  string
 *
 * @return true if both s1 and s2 are null or same string, false otherwise
 */
static bool fba_util_streq(const char *s1, const char *s2)
{
    return (s1 && s2) ? !strcmp(s1, s2) : (s1 == s2);
}

/* ========================================================================= *
 * DYNAMIC_SETTINGS
 * ========================================================================= */

/** Master ALS enabled setting */
static gboolean fba_setting_als_enabled = MCE_DEFAULT_DISPLAY_ALS_ENABLED;
static guint    fba_setting_als_enabled_id = 0;

/** Filter brightness through ALS setting */
static gboolean fba_setting_als_autobrightness = MCE_DEFAULT_DISPLAY_ALS_AUTOBRIGHTNESS;
static guint    fba_setting_als_autobrightness_id = 0;

/** ALS is used for LID filtering setting */
static gboolean fba_setting_filter_lid_with_als = MCE_DEFAULT_TK_FILTER_LID_WITH_ALS;
static guint    fba_setting_filter_lid_with_als_id = 0;

/** Input filter to use for ALS sensor - config value */
static gchar   *fba_setting_als_input_filter = 0;
static guint    fba_setting_als_input_filter_id = 0;

/** Sample time for ALS input filtering  - config value */
static gint     fba_setting_als_sample_time = MCE_DEFAULT_DISPLAY_ALS_SAMPLE_TIME;
static guint    fba_setting_als_sample_time_id = 0;

/** Currently active color profile (dummy implementation) */
static gchar   *fba_setting_color_profile = 0;
static guint    fba_setting_color_profile_id = 0;

/** Default color profile, set in ini-files */
static gchar   *fba_default_color_profile = 0;

/** GConf callback for powerkey related settings
 *
 * @param gcc    (not used)
 * @param id     Connection ID from gconf_client_notify_add()
 * @param entry  The modified GConf entry
 * @param data   (not used)
 */
static void
fba_setting_cb(GConfClient *const gcc, const guint id,
               GConfEntry *const entry, gpointer const data)
{
    (void)gcc;
    (void)data;

    const GConfValue *gcv = gconf_entry_get_value(entry);

    if( !gcv ) {
        mce_log(LL_DEBUG, "GConf Key `%s' has been unset",
                gconf_entry_get_key(entry));
        goto EXIT;
    }

    if( id == fba_setting_als_enabled_id ) {
        fba_setting_als_enabled = gconf_value_get_bool(gcv);
        fba_status_rethink();
    }
    else if( id == fba_setting_als_autobrightness_id ) {
        fba_setting_als_autobrightness = gconf_value_get_bool(gcv);
        fba_status_rethink();
    }
    else if( id == fba_setting_filter_lid_with_als_id ) {
        fba_setting_filter_lid_with_als = gconf_value_get_bool(gcv);
        fba_status_rethink();
    }
    else if( id == fba_setting_als_input_filter_id ) {
        const char *val = gconf_value_get_string(gcv);

        if( !fba_util_streq(fba_setting_als_input_filter, val) ) {
            g_free(fba_setting_als_input_filter);
            fba_setting_als_input_filter = g_strdup(val);
            fba_inputflt_select(fba_setting_als_input_filter);
        }
    }
    else if( id == fba_setting_als_sample_time_id ) {
        gint old = fba_setting_als_sample_time;
        fba_setting_als_sample_time = gconf_value_get_int(gcv);

        if( fba_setting_als_sample_time != old ) {
            mce_log(LL_NOTICE, "fba_setting_als_sample_time: %d -> %d",
                    old, fba_setting_als_sample_time);
            // NB: takes effect on the next sample timer restart
        }
    }
    else if (id == fba_setting_color_profile_id) {
        const gchar *val = gconf_value_get_string(gcv);
        mce_log(LL_NOTICE, "fba_setting_color_profile: '%s' -> '%s'",
                fba_setting_color_profile, val);
        fba_color_profile_set(val);
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }

EXIT:

    return;
}

/** Start tracking setting changes
 */
static void
fba_setting_init(void)
{
    /* ALS enabled settings */
    mce_setting_track_bool(MCE_SETTING_DISPLAY_ALS_ENABLED,
                           &fba_setting_als_enabled,
                           MCE_DEFAULT_DISPLAY_ALS_ENABLED,
                           fba_setting_cb,
                           &fba_setting_als_enabled_id);

    mce_setting_track_bool(MCE_SETTING_DISPLAY_ALS_AUTOBRIGHTNESS,
                           &fba_setting_als_autobrightness,
                           MCE_DEFAULT_DISPLAY_ALS_AUTOBRIGHTNESS,
                           fba_setting_cb,
                           &fba_setting_als_autobrightness_id);

    mce_setting_track_bool(MCE_SETTING_TK_FILTER_LID_WITH_ALS,
                           &fba_setting_filter_lid_with_als,
                           MCE_DEFAULT_TK_FILTER_LID_WITH_ALS,
                           fba_setting_cb,
                           &fba_setting_filter_lid_with_als_id);

    /* ALS input filter setting */
    mce_setting_track_string(MCE_SETTING_DISPLAY_ALS_INPUT_FILTER,
                             &fba_setting_als_input_filter,
                             MCE_DEFAULT_DISPLAY_ALS_INPUT_FILTER,
                             fba_setting_cb,
                             &fba_setting_als_input_filter_id);

    /* ALS sample time setting */
    mce_setting_track_int(MCE_SETTING_DISPLAY_ALS_SAMPLE_TIME,
                          &fba_setting_als_sample_time,
                          MCE_DEFAULT_DISPLAY_ALS_SAMPLE_TIME,
                          fba_setting_cb,
                          &fba_setting_als_sample_time_id);

    /* Color profile setting */
    mce_setting_notifier_add(MCE_SETTING_DISPLAY_PATH,
                             MCE_SETTING_DISPLAY_COLOR_PROFILE,
                             fba_setting_cb,
                             &fba_setting_color_profile_id);

    fba_color_profile_init();
}

/** Stop tracking setting changes
 */
static void
fba_setting_quit(void)
{
    mce_setting_notifier_remove(fba_setting_als_enabled_id),
        fba_setting_als_enabled_id = 0;

    mce_setting_notifier_remove(fba_setting_als_autobrightness_id),
        fba_setting_als_autobrightness_id = 0;

    mce_setting_notifier_remove(fba_setting_filter_lid_with_als_id),
        fba_setting_filter_lid_with_als_id = 0;

    mce_setting_notifier_remove(fba_setting_als_input_filter_id),
        fba_setting_als_input_filter_id = 0;

    mce_setting_notifier_remove(fba_setting_als_sample_time_id),
        fba_setting_als_sample_time_id = 0;

    mce_setting_notifier_remove(fba_setting_color_profile_id),
        fba_setting_color_profile_id = 0;

    fba_color_profile_quit();
}

/* ========================================================================= *
 * COLOR_PROFILE
 * ========================================================================= */

/** List of known color profiles (dummy implementation) */
static const char * const fba_color_profile_names[] =
{
    COLOR_PROFILE_ID_HARDCODED,
};

/** Check if color profile is supported
 *
 * @param id color profile name
 *
 * @return true if profile is supported, false otherwise
 */
static bool
fba_color_profile_exists(const char *id)
{
    if( !id || !*id )
        return false;

    if( !strcmp(id, COLOR_PROFILE_ID_HARDCODED) )
        return true;

    for( size_t i = 0; i < G_N_ELEMENTS(fba_color_profile_names); ++i ) {
        if( !strcmp(fba_color_profile_names[i], id) )
            return true;
    }

    return false;
}

/**
 * Set the current color profile according to requested
 *
 * @param id Name of requested color profile
 *
 * @return true on success, false on failure
 */
static bool
fba_color_profile_set(const gchar *id)
{
    // NOTE: color profile support dropped, this just a stub

    /* Transform "default" request into request for whatever is
     * the configured default color profile. */
    if( !g_strcmp0(id, COLOR_PROFILE_ID_DEFAULT) )
        id = fba_default_color_profile ?: COLOR_PROFILE_ID_HARDCODED;

    /* Succes is: The requested color profile exists */
    bool success = fba_color_profile_exists(id);

    /* Fall back to hadrcoded if requested profile is not supported */
    if( !success ) {
        if( id && *id )
            mce_log(LL_WARN, "%s: unsupported color profile", id);
        id = COLOR_PROFILE_ID_HARDCODED;
    }

    /* Check if the color profile does change */
    bool changed = g_strcmp0(id, fba_setting_color_profile);

    /* Update the cached value */
    if( changed ) {
        g_free(fba_setting_color_profile),
            fba_setting_color_profile = g_strdup(id);
    }

    /* Send a change indication if the value did change, or
     * if the requested value was not accepted as-is */
    if( changed || !success ) {
        fba_dbus_send_current_color_profile(0);
    }

    /* If we were to do something about the color profile,
     * it would happen here */
    if( changed ) {
        /* nop */
    }

    /* Always sync the settings cache */
    mce_setting_set_string(MCE_SETTING_DISPLAY_COLOR_PROFILE,
                           fba_setting_color_profile);

    return success;
}

/**
 * Initialization of saveed color profile during boot
 */
static void
fba_color_profile_init(void)
{
    /* Get the default value specified in static configuration.
     * This must be done before fba_color_profile_set() is called.
     */
    fba_default_color_profile =
        mce_conf_get_string(MCE_CONF_COMMON_GROUP,
                            MCE_CONF_DEFAULT_PROFILE_ID_KEY,
                            NULL);

    /* Apply the last value saved to dynamic settings / the default.
     */
    gchar *saved_color_profile = 0;
    mce_setting_get_string(MCE_SETTING_DISPLAY_COLOR_PROFILE,
                           &saved_color_profile);
    fba_color_profile_set(saved_color_profile);
    g_free(saved_color_profile);
}

static void
fba_color_profile_quit(void)
{
    g_free(fba_default_color_profile),
        fba_default_color_profile = 0;

    g_free(fba_setting_color_profile),
        fba_setting_color_profile = 0;
}

/* ========================================================================= *
 * SENSOR_INPUT_FILTERING
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * INPUT_FILTER_BACKEND_DUMMY
 * ------------------------------------------------------------------------- */

static int
fba_inputflt_dummy_filter(int add)
{
    return add;
}

static bool
fba_inputflt_dummy_stable(void)
{
    return true;
}

static void
fba_inputflt_dummy_reset(void)
{
}

/* ------------------------------------------------------------------------- *
 * INPUT_FILTER_BACKEND_MEDIAN
 * ------------------------------------------------------------------------- */

/** Moving window of ALS measurements */
static int  fba_inputflt_median_fifo[FBA_INPUTFLT_MEDIAN_SIZE] = {  };

/** Contents of fba_inputflt_median_fifo in ascending order */
static int  fba_inputflt_median_stat[FBA_INPUTFLT_MEDIAN_SIZE] = {  };;

static int
fba_inputflt_median_filter(int add)
{
    /* Adding negative sample values mean the sensor is not
     * in use and we should forget any history that exists */
    if( add < 0 ) {
        for( int i = 0; i < FBA_INPUTFLT_MEDIAN_SIZE; ++i )
            fba_inputflt_median_fifo[i] = fba_inputflt_median_stat[i] = -1;
        goto EXIT;
    }

    /* Value to be removed */
    int rem = fba_inputflt_median_fifo[0];

    /* If negative value gets shifted out, it means we do not
     * have history. Initialize with the value we have */
    if( rem < 0 ) {
        for( int i = 0; i < FBA_INPUTFLT_MEDIAN_SIZE; ++i )
            fba_inputflt_median_fifo[i] = fba_inputflt_median_stat[i] = add;
        goto EXIT;
    }

    /* Shift the new value to the sample window fifo */
    for( int i = 1; i < FBA_INPUTFLT_MEDIAN_SIZE; ++i )
        fba_inputflt_median_fifo[i-1] = fba_inputflt_median_fifo[i];
    fba_inputflt_median_fifo[FBA_INPUTFLT_MEDIAN_SIZE-1] = add;

    /* If we shift in the same value as what was shifted out,
     * the ordered statistics do not change */
    if( add == rem )
        goto EXIT;

    /* Do one pass filtering of ordered statistics:
     * Remove the value we shifted out of the fifo
     * and insert the new sample to correct place. */

    int src = 0, dst = 0, tmp;
    int stk = fba_inputflt_median_stat[src++];

    while( dst < FBA_INPUTFLT_MEDIAN_SIZE ) {
        if( stk < add )
            tmp = stk, stk = add, add = tmp;

        if( add == rem )
            rem = INT_MAX;
        else
            fba_inputflt_median_stat[dst++] = add;

        if( src < FBA_INPUTFLT_MEDIAN_SIZE )
            add = fba_inputflt_median_stat[src++];
        else
            add = INT_MAX;
    }

EXIT:
    mce_log(LL_DEBUG, "%d - %d - %d",
            fba_inputflt_median_stat[0],
            fba_inputflt_median_stat[FBA_INPUTFLT_MEDIAN_SIZE/2],
            fba_inputflt_median_stat[FBA_INPUTFLT_MEDIAN_SIZE-1]);

    /* Return median of the history window */
    return fba_inputflt_median_stat[FBA_INPUTFLT_MEDIAN_SIZE/2];
}

static bool
fba_inputflt_median_stable(void)
{
    /* The smallest and the largest values in the sorted
     * samples buffer are equal */
    int small = 0;
    int large = FBA_INPUTFLT_MEDIAN_SIZE-1;
    return fba_inputflt_median_stat[small] == fba_inputflt_median_stat[large];
}

static void
fba_inputflt_median_reset(void)
{
    fba_inputflt_median_filter(-1);
}

/* ------------------------------------------------------------------------- *
 * INPUT_FILTER_FRONTEND
 * ------------------------------------------------------------------------- */

/** Array of available input filter backendss */
static const fba_inputflt_t fba_inputflt_lut[] =
{
    // NOTE: "disabled" must be in the 1st slot
    {
        .fi_name   = "disabled",
        .fi_reset  = fba_inputflt_dummy_reset,
        .fi_filter = fba_inputflt_dummy_filter,
        .fi_stable = fba_inputflt_dummy_stable,
    },
    {
        .fi_name   = "median",
        .fi_reset  = fba_inputflt_median_reset,
        .fi_filter = fba_inputflt_median_filter,
        .fi_stable = fba_inputflt_median_stable,
    },
};

/** Currently used input filter backend */
static const fba_inputflt_t *fba_inputflt_cur = fba_inputflt_lut;

/** Latest Lux value fed in to the filter */
static gint fba_inputflt_input_lux = 0;

/** Latest Lux value emitted from the filter */
static gint fba_inputflt_output_lux = -1;

/** Flag for: forget history on the next als change */
static bool fba_inputflt_flush_history = true;

/** Timer ID for: ALS data sampling */
static guint fba_inputflt_sampling_id = 0;

/** Set input filter backend
 *
 * @param name  name of the backend to use
 */
static void
fba_inputflt_select(const char *name)
{
    fba_inputflt_reset();

    if( !name ) {
        fba_inputflt_cur = fba_inputflt_lut;
        goto EXIT;
    }

    for( size_t i = 0; ; ++i ) {
        if( i == G_N_ELEMENTS(fba_inputflt_lut) ) {
            mce_log(LL_WARN, "filter '%s' is unknown", name);
            fba_inputflt_cur = fba_inputflt_lut;
            break;
        }
        if( !strcmp(fba_inputflt_lut[i].fi_name, name) ) {
            fba_inputflt_cur = fba_inputflt_lut + i;
            break;
        }
    }
EXIT:
    mce_log(LL_NOTICE, "selected '%s' als filter",
            fba_inputflt_cur->fi_name);

    fba_inputflt_reset();
}

/** Reset history buffer
 *
 * Is used to clear stale history data when powering up the sensor.
 */
static void
fba_inputflt_reset(void)
{
    fba_inputflt_cur->fi_reset();
}

/** Apply filtering backend for als sensor value
 *
 * @param add  sample to shift into the filter history
 *
 * @return filtered value
 */
static int
fba_inputflt_filter(int lux)
{
    return fba_inputflt_cur->fi_filter(lux);
}

/** Check if the whole history buffer is filled with the same value
 *
 * Is used to determine when the pseudo sampling timer can be stopped.
 */
static bool
fba_inputflt_stable(void)
{
    return fba_inputflt_cur->fi_stable();
}

/** Request filter history to be flushed on the next ALS change
 */
static void
fba_inputflt_flush_on_change(void)
{
    fba_inputflt_flush_history = true;
}

/** Get filtered ALS state and feed it to datapipes
 *
 * @param lux  sensor reading, or -1 for no-data
 */
static void
fba_inputflt_sampling_output(int lux)
{
    lux = fba_inputflt_filter(lux);

    if( fba_inputflt_output_lux == lux )
        goto EXIT;

    mce_log(LL_DEBUG, "output: %d -> %d", fba_inputflt_output_lux, lux);

    fba_inputflt_output_lux = lux;

    fba_datapipe_execute_brightness_change();

    /* Feed filtered sensor data to datapipe */
    datapipe_exec_full(&light_sensor_filtered_pipe,
                       GINT_TO_POINTER(fba_inputflt_output_lux),
                       DATAPIPE_CACHE_INDATA);
EXIT:
    return;
}
/** Timer callback for: ALS data sampling
 *
 * @param aptr (unused user data pointer)
 *
 * @return TRUE to keep timer repeating, or FALSE to stop it
 */
static gboolean
fba_inputflt_sampling_cb(gpointer aptr)
{
    (void)aptr;

    if( !fba_inputflt_sampling_id )
        return FALSE;

    /* Drive the filter */
    fba_inputflt_sampling_output(fba_inputflt_input_lux);

    /* Keep timer active while changes come in */

    if( !fba_inputflt_stable() )
        return TRUE;

    /* Stop sampling activity */
    mce_log(LL_DEBUG, "stable");
    fba_inputflt_sampling_id = 0;
    return FALSE;
}

static int
fba_inputflt_sampling_time(void)
{
    return mce_clip_int(ALS_SAMPLE_TIME_MIN,
                        ALS_SAMPLE_TIME_MAX,
                        fba_setting_als_sample_time);
}

/** Start ALS sampling timer
 */
static void
fba_inputflt_sampling_start(void)
{
    // check if we need to flush history
    if( fba_inputflt_flush_history ) {
        fba_inputflt_flush_history = false;

        mce_log(LL_DEBUG, "reset");

        if( fba_inputflt_sampling_id ) {
            g_source_remove(fba_inputflt_sampling_id),
                fba_inputflt_sampling_id = 0;
        }
        fba_inputflt_reset();
    }

    // start collecting history
    if( !fba_inputflt_sampling_id ) {
        mce_log(LL_DEBUG, "start");
        fba_inputflt_sampling_id = g_timeout_add(fba_inputflt_sampling_time(),
                                             fba_inputflt_sampling_cb, 0);

        fba_inputflt_sampling_output(fba_inputflt_input_lux);
    }
}

/** Stop ALS sampling timer
 */
static void
fba_inputflt_sampling_stop(void)
{
    if( fba_inputflt_sampling_id ) {
        mce_log(LL_DEBUG, "stop");
        g_source_remove(fba_inputflt_sampling_id),
            fba_inputflt_sampling_id = 0;
    }

    fba_inputflt_sampling_output(fba_inputflt_input_lux);
}

/** Feed sensor input to filter
 *
 * @param lux  sensor reading, or -1 for no-data
 */
static void
fba_inputflt_sampling_input(int lux)
{
    if( fba_inputflt_input_lux == lux )
        goto EXIT;

    mce_log(LL_DEBUG, "input: %d -> %d", fba_inputflt_input_lux, lux);
    fba_inputflt_input_lux = lux;

    if( fba_inputflt_input_lux < 0 )
        fba_inputflt_sampling_stop();
    else
        fba_inputflt_sampling_start();

EXIT:
    return;
}

/** Initialize ALS filtering
 */
static void
fba_inputflt_init(void)
{
    fba_inputflt_select(fba_setting_als_input_filter);
}

/** De-initialize ALS filtering
 */
static void
fba_inputflt_quit(void)
{
    /* Stop sampling timer by feeding no-data to filter */
    fba_inputflt_sampling_input(-1);
}

/* ========================================================================= *
 * ALS_FILTER
 * ========================================================================= */

/** ALS filtering state for display backlight brightness */
static fba_als_filter_t lut_display =
{
    .id = "Display",
};

/** ALS filtering state for keypad backlight brightness */
static fba_als_filter_t lut_key =
{
    .id = "Keypad",
};

/** ALS filtering state for indication led brightness */
static fba_als_filter_t lut_led =
{
    .id = "Led",
};

/** ALS filtering state for low power mode display simulation */
static fba_als_filter_t lut_lpm =
{
    .id = "LPM",
};

/** Remove transition thresholds from filtering state
 *
 * Set thresholds so that any lux value will be out of bounds.
 *
 * @param self ALS filtering state data
 */
static void
fba_als_filter_clear_threshold(fba_als_filter_t *self)
{
    self->lux_lo = INT_MAX;
    self->lux_hi = 0;
}

/** Load ALS ramp into filtering state
 *
 * @param self ALS filtering state data
 * @param grp  Configuration group name
 * @param prof ALS profile id
 */
static bool
fba_als_filter_load_profile(fba_als_filter_t *self, const char *grp, int prof)
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

    if( lim_cnt >  FBA_PROFILE_STEPS ) {
        lim_cnt = FBA_PROFILE_STEPS;
        mce_log(LL_WARN, "[%s] %s: excess items",
                grp, lim_key);
    }
    else if( lim_cnt < FBA_PROFILE_STEPS ) {
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

/** Initialize ALS filtering state
 *
 * @param self ALS filtering state data
 */
static void
fba_als_filter_reset_profiles(fba_als_filter_t *self)
{
    /* Reset ramps to a state where any lux value will
     * yield 100% brightness */
    for( int i = 0; i < FBA_PROFILE_COUNT; ++i ) {
        for( int k = 0; k <= FBA_PROFILE_STEPS; ++k ) {
            self->lut[i][k].lux = INT_MAX;
            self->lut[i][k].val = 100;
        }
    }

    /* Default to 100% output */
    self->val  = 100;

    /* Invalidate thresholds */
    self->prof = -1;
    fba_als_filter_clear_threshold(self);
}

/** Load ALS ramps into filtering state
 *
 * @param self ALS filtering state data
 */
static void
fba_als_filter_load_profiles(fba_als_filter_t *self)
{
    fba_als_filter_reset_profiles(self);

    char grp[64];
    snprintf(grp, sizeof grp, "Brightness%s", self->id);

    if( !mce_conf_has_group(grp) ) {
        mce_log(LL_WARN, "[%s]: als config missing", grp);
        goto EXIT;
    }

    for( self->profiles = 0; self->profiles < FBA_PROFILE_COUNT; ++self->profiles ) {
        if( !fba_als_filter_load_profile(self, grp, self->profiles) )
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
static int
fba_als_filter_get_lux(fba_als_filter_t *self, int prof, int slot)
{
    if( slot < 0 )
        return 0;
    if( slot < FBA_PROFILE_STEPS )
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
static int
fba_als_filter_run(fba_als_filter_t *self, int prof, int lux)
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

    for( slot = 0; slot < FBA_PROFILE_STEPS; ++slot ) {
        if( lux < self->lut[prof][slot].lux )
            break;
    }

    self->prof = prof;

    if( slot < FBA_PROFILE_STEPS )
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

    int a = fba_als_filter_get_lux(self, prof, slot-2);
    int b = fba_als_filter_get_lux(self, prof, slot-1);
    int c = fba_als_filter_get_lux(self, prof, slot+0);

    self->lux_lo = b - fba_util_imin(b-a, c-b) / 10;
    self->lux_hi = c;

    mce_log(LL_DEBUG, "prof=%d, slot=%d, range=%d...%d",
            prof, slot, self->lux_lo, self->lux_hi);

EXIT:
    return self->val;
}

/** Setup ini-file based config items
 */
static void
fba_als_filter_init(void)
{
    /* Read lux ramps from configuration */
    fba_als_filter_load_profiles(&lut_display);
    fba_als_filter_load_profiles(&lut_led);
    fba_als_filter_load_profiles(&lut_key);
    fba_als_filter_load_profiles(&lut_lpm);
}

/* ========================================================================= *
 * DATAPIPE_TRACKING
 * ========================================================================= */

/** Cached display state; tracked via fba_datapipe_display_state_curr_trigger() */
static display_state_t fba_display_state_curr = MCE_DISPLAY_UNDEF;

/** Cached target display state; tracked via fba_datapipe_display_state_next_trigger() */
static display_state_t fba_display_state_curr_next = MCE_DISPLAY_UNDEF;

/** Cached als poll state; tracked via fba_datapipe_light_sensor_poll_request_filter() */
static bool fba_light_sensor_polling = false;

/**
 * Ambient Light Sensor filter for display brightness
 *
 * @param data The un-processed brightness setting (1-100) stored in a pointer
 * @return The processed brightness value (percentage) stored in a pointer
 */
static gpointer
fba_datapipe_display_brightness_filter(gpointer data)
{
    int setting = GPOINTER_TO_INT(data);

    int brightness = setting;

    if( !fba_setting_als_autobrightness || fba_inputflt_output_lux < 0 )
        goto EXIT;

    int max_prof = lut_display.profiles - 1;

    if( max_prof < 0 )
        goto EXIT;

    int prof = mce_xlat_int(1,100, 0,max_prof, setting);

    brightness = fba_als_filter_run(&lut_display, prof,
                                    fba_inputflt_output_lux);

EXIT:
    mce_log(LL_DEBUG, "in=%d -> out=%d", setting, brightness);
    return GINT_TO_POINTER(brightness);
}

/**
 * Ambient Light Sensor filter for LED brightness
 *
 * @param data The un-processed brightness setting (1-100) stored in a pointer
 * @return The processed brightness value
 */
static gpointer
fba_datapipe_led_brightness_filter(gpointer data)
{
    /* Startup default: LED brightness scale is unknown */
    static int prev_scale = -1;

    /* Default to: Brightness setting * 40 % */
    int brightness = GPOINTER_TO_INT(data);
    int curr_scale = 40;

    /* Check if LED brightness configuration exists */
    if( lut_led.profiles < 1 )
        goto EXIT;

    /* Forget cached output state if als master toggle or
     * autobrightness setting gets disabled */
    if(  !fba_setting_als_enabled ||
         !fba_setting_als_autobrightness ) {
        prev_scale = -1;
        goto EXIT;
    }

    if( fba_inputflt_output_lux >= 0 ) {
        /* Evaluate brightness scale based on available sensor data */
        curr_scale = fba_als_filter_run(&lut_led, 0, fba_inputflt_output_lux);
        prev_scale = curr_scale;
    }
    else if( prev_scale >= 0 ) {
        /* Use previously evaluated brightness scale */
        curr_scale = prev_scale;
    }

EXIT:
    return GINT_TO_POINTER(brightness * curr_scale / 100);
}

/** Ambient Light Sensor filter for LPM brightness
 *
 * @param data The un-processed brightness setting (1-100) stored in a pointer
 * @return The processed brightness value
 */
static gpointer
fba_datapipe_lpm_brightness_filter(gpointer data)
{
    int value = GPOINTER_TO_INT(data);

    if( !fba_setting_als_autobrightness || fba_inputflt_output_lux < 0 )
        goto EXIT;

    if( lut_lpm.profiles < 1 )
        goto EXIT;

    /* Note: Input value is ignored and output is
     *       determined only by the als config */
    value = fba_als_filter_run(&lut_lpm, 0, fba_inputflt_output_lux);

EXIT:
    return GINT_TO_POINTER(value);
}

/**
 * Ambient Light Sensor filter for keyboard backlight brightness
 *
 * @param data The un-processed brightness setting (1-100) stored in a pointer
 * @return The processed brightness value
 */
static gpointer
fba_datapipe_key_backlight_brightness_filter(gpointer data)
{
    int value = GPOINTER_TO_INT(data);
    int scale = 100;

    if( !fba_setting_als_autobrightness || fba_inputflt_output_lux < 0 )
        goto EXIT;

    if( lut_key.profiles < 1 )
        goto EXIT;

    scale = fba_als_filter_run(&lut_key, 0, fba_inputflt_output_lux);

EXIT:
    return GINT_TO_POINTER(value * scale / 100);
}

/** Ambient Light Sensor filter for temporary sensor enable
 *
 * @param data Requested sensor enable/disable bool (as void pointer)
 *
 * @return Granted  sensor enable/disable bool (as void pointer)
 */
static gpointer
fba_datapipe_light_sensor_poll_request_filter(gpointer data)
{
    bool prev = fba_light_sensor_polling;
    fba_light_sensor_polling = GPOINTER_TO_INT(data);

    if( !fba_setting_als_enabled )
        fba_light_sensor_polling = FALSE;

    if( fba_light_sensor_polling == prev )
        goto EXIT;

    mce_log(LL_DEVEL, "light_sensor_polling = %s",
            fba_light_sensor_polling ? "true" : "false");

    /* Sensor status is affected only if the value changes */
    fba_status_rethink();

EXIT:

    /* The termination timer must be renewed/stopped even
     * if the value does not change. */
    fba_sensorpoll_rethink();

    return GINT_TO_POINTER(fba_light_sensor_polling);
}

/**
 * Handle display state change
 *
 * @param data The display stated stored in a pointer
 */
static void
fba_datapipe_display_state_curr_trigger(gconstpointer data)
{
    display_state_t prev = fba_display_state_curr;
    fba_display_state_curr = GPOINTER_TO_INT(data);

    if( prev == fba_display_state_curr )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_curr: %s -> %s",
            display_state_repr(prev),
            display_state_repr(fba_display_state_curr));

    fba_status_rethink();

EXIT:
    return;
}

static void
fba_datapipe_display_state_next_trigger(gconstpointer data)
{
    display_state_t prev = fba_display_state_curr_next;
    fba_display_state_curr_next = GPOINTER_TO_INT(data);

    if( prev == fba_display_state_curr_next )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_next: %s -> %s",
            display_state_repr(prev),
            display_state_repr(fba_display_state_curr_next));

    fba_status_rethink();

EXIT:
    return;
}

static void
fba_datapipe_execute_brightness_change(void)
{
    /* Re-filter the brightness */
    datapipe_exec_full(&display_brightness_pipe,
                       datapipe_value(&display_brightness_pipe),
                       DATAPIPE_CACHE_NOTHING);
    datapipe_exec_full(&led_brightness_pipe,
                       datapipe_value(&led_brightness_pipe),
                       DATAPIPE_CACHE_NOTHING);
    datapipe_exec_full(&lpm_brightness_pipe,
                       datapipe_value(&lpm_brightness_pipe),
                       DATAPIPE_CACHE_NOTHING);
    datapipe_exec_full(&key_backlight_brightness_pipe,
                       datapipe_value(&key_backlight_brightness_pipe),
                       DATAPIPE_CACHE_NOTHING);
}

/** Array of datapipe handlers */
static datapipe_handler_t fba_datapipe_handlers[] =
{
    // input filters
    {
        .datapipe  = &display_brightness_pipe,
        .filter_cb = fba_datapipe_display_brightness_filter,
    },
    {
        .datapipe  = &led_brightness_pipe,
        .filter_cb = fba_datapipe_led_brightness_filter,
    },
    {
        .datapipe  = &lpm_brightness_pipe,
        .filter_cb = fba_datapipe_lpm_brightness_filter,
    },
    {
        .datapipe  = &key_backlight_brightness_pipe,
        .filter_cb = fba_datapipe_key_backlight_brightness_filter,
    },
    {
        .datapipe  = &light_sensor_poll_request_pipe,
        .filter_cb = fba_datapipe_light_sensor_poll_request_filter,
    },

    // output triggers
    {
        .datapipe  = &display_state_next_pipe,
        .output_cb = fba_datapipe_display_state_next_trigger,
    },
    {
        .datapipe  = &display_state_curr_pipe,
        .output_cb = fba_datapipe_display_state_curr_trigger,
    },

    // sentinel
    {
        .datapipe  = 0,
    }
};

static datapipe_bindings_t fba_datapipe_bindings =
{
    .module   = MODULE_NAME,
    .handlers = fba_datapipe_handlers,
};

/** Install datapipe triggers/filters
 */
static void
fba_datapipe_init(void)
{
    mce_datapipe_init_bindings(&fba_datapipe_bindings);
}

/** Remove datapipe triggers/filters
 */
static void
fba_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&fba_datapipe_bindings);
}

/* ========================================================================= *
 * DBUS_HANDLERS
 * ========================================================================= */

/**
 * Send the current profile id
 *
 * @param method_call A DBusMessage to reply to
 */
static void
fba_dbus_send_current_color_profile(DBusMessage *method_call)
{
    DBusMessage *msg = 0;
    const char  *val = fba_setting_color_profile;

    if( !fba_color_profile_exists(val) )
        val = COLOR_PROFILE_ID_HARDCODED;

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
}

/**
 * D-Bus callback for the get color profile method call
 *
 * @param msg The D-Bus message
 * @return TRUE
 */
static gboolean
fba_dbus_get_color_profile_cb(DBusMessage *const msg)
{
    mce_log(LL_DEVEL, "Received get color profile request from %s",
            mce_dbus_get_message_sender_ident(msg));

    if( dbus_message_get_no_reply(msg) )
        goto EXIT;

    fba_dbus_send_current_color_profile(msg);

EXIT:
    return TRUE;
}

/** D-Bus callback for the get color profile ids method call
 *
 * @param msg The D-Bus message
 * @return TRUE
 */
static gboolean
fba_dbus_get_color_profiles_cb(DBusMessage *const msg)
{
    mce_log(LL_DEVEL, "Received list color profiles request from %s",
            mce_dbus_get_message_sender_ident(msg));

    DBusMessage *rsp = 0;
    int cnt = sizeof fba_color_profile_names / sizeof *fba_color_profile_names;
    const char * const * vec = fba_color_profile_names;

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
static gboolean
fba_dbus_set_color_profile_cb(DBusMessage *const msg)
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
        mce_log(LL_ERR, "Failed to get argument from %s.%s: %s: %s",
                MCE_REQUEST_IF, MCE_COLOR_PROFILE_CHANGE_REQ,
                err.name, err.message);
    }
    else {
        if( fba_color_profile_set(val) )
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
        .name      = MCE_COLOR_PROFILE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = fba_dbus_get_color_profile_cb,
        .args      =
        "    <arg direction=\"out\" name=\"profile_name\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_COLOR_PROFILE_IDS_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = fba_dbus_get_color_profiles_cb,
        .args      =
        "    <arg direction=\"out\" name=\"profile_names\" type=\"as\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_COLOR_PROFILE_CHANGE_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = fba_dbus_set_color_profile_cb,
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
static void
fba_dbus_init(void)
{
    mce_dbus_handler_register_array(filter_brightness_dbus_handlers);
}

/** Remove dbus handlers
 */
static void
fba_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(filter_brightness_dbus_handlers);
}

/* ========================================================================= *
 * SENSOR_STATUS
 * ========================================================================= */

/** Raw lux value from the ALS */
static gint fba_status_sensor_lux = -1;

/** Handle lux value changed event
 *
 * @param lux ambient light value
 */
static void
fba_status_sensor_value_change_cb(int lux)
{
    if( !fba_setting_als_enabled )
        fba_status_sensor_lux = -1;
    else
        fba_status_sensor_lux = lux;

    mce_log(LL_DEBUG, "sensor: %d", fba_status_sensor_lux);

    /* Filter raw sensor data */
    fba_inputflt_sampling_input(fba_status_sensor_lux);

    /* Feed raw sensor data to datapipe */
    datapipe_exec_full(&light_sensor_actual_pipe,
                       GINT_TO_POINTER(fba_status_sensor_lux),
                       DATAPIPE_CACHE_INDATA);
}

static bool
fba_status_sensor_is_needed(void)
{
    bool need_als = false;

    switch( fba_display_state_curr_next ) {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
        if( fba_setting_als_autobrightness || fba_setting_filter_lid_with_als )
            need_als = true;
        break;

    default:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_UNDEF:
        break;
    }

    return need_als;
}

/** Check if ALS sensor should be enabled or disabled
 */
static void
fba_status_rethink(void)
{
    static int old_autobrightness = -1;

    static int enable_old = -1;
    bool       enable_new = false;

    if( fba_setting_als_enabled )
        enable_new = (fba_light_sensor_polling ||
                      fba_status_sensor_is_needed());

    if( fba_module_unload )
        enable_new = false;

    if( enable_old == enable_new )
        goto EXIT;

    mce_log(LL_DEBUG, "enabled=%d; autobright=%d; filter_lid=%d -> enable=%d",
            fba_setting_als_enabled, fba_setting_als_autobrightness, fba_setting_filter_lid_with_als, enable_new);

    enable_old = enable_new;

    if( enable_new ) {
        /* Enable sensor before attaching notification callback.
         * So that the last seen light sensor reading is made active
         * again and reported instead of the fallback/default value. */
        mce_sensorfw_als_enable();
        mce_sensorfw_als_set_notify(fba_status_sensor_value_change_cb);

        /* The sensor has been off for some time, so the
         * history needs to be forgotten when we get fresh
         * data */
        fba_inputflt_flush_on_change();
    }
    else {
        /* Disable change notifications */
        mce_sensorfw_als_set_notify(0);

        /* Force cached value re-evaluation */
        fba_status_sensor_value_change_cb(-1);

        mce_sensorfw_als_disable();

        /* Clear thresholds so that the next reading from
         * als will not be affected by previous state */
        fba_als_filter_clear_threshold(&lut_display);
        fba_als_filter_clear_threshold(&lut_led);
        fba_als_filter_clear_threshold(&lut_key);
        fba_als_filter_clear_threshold(&lut_lpm);
    }

EXIT:
    if( old_autobrightness != fba_setting_als_autobrightness ) {
        old_autobrightness = fba_setting_als_autobrightness;
        fba_datapipe_execute_brightness_change();
    }

    /* Block device from suspending while temporary ALS poll is active */
    if( enable_new && fba_light_sensor_polling )
        mce_wakelock_obtain("als_poll", -1);
    else
        mce_wakelock_release("als_poll");

    return;
}

/* ========================================================================= *
 * SENSOR_POLL
 * ========================================================================= */

/** Timer ID: Terminate temporary ALS enable */
static guint fba_sensorpoll_timer_id = 0;

/** Timer callback: Terminate temporary ALS enable
 *
 * @param aptr user data (unused)
 *
 * @return FALSE to stop timer from repeating
 */
static gboolean
fba_sensorpoll_timer_cb(gpointer aptr)
{
    (void)aptr;

    if( !fba_sensorpoll_timer_id )
        goto EXIT;

    mce_log(LL_DEBUG, "als poll: %s", "timeout");

    fba_sensorpoll_timer_id = 0;
    datapipe_exec_full(&light_sensor_poll_request_pipe,
                       GINT_TO_POINTER(false),
                       DATAPIPE_CACHE_OUTDATA);
EXIT:
    return FALSE;
}

/** Schedule temporary ALS enable termination
 *
 * If the timer is already active, the termination time
 * will be rescheduled.
 */
static void
fba_sensorpoll_start(void)
{
    if( fba_sensorpoll_timer_id )
        g_source_remove(fba_sensorpoll_timer_id);
    else
        mce_log(LL_DEBUG, "als poll: %s", "start");

    fba_sensorpoll_timer_id = g_timeout_add(FBA_SENSORPOLL_DURATION_MS,
                                            fba_sensorpoll_timer_cb, 0);
}

/** Cancel temporary ALS enable termination
 */
static void
fba_sensorpoll_stop(void)
{
    if( !fba_sensorpoll_timer_id )
        goto EXIT;

    mce_log(LL_DEBUG, "als poll: %s", "stop");

    g_source_remove(fba_sensorpoll_timer_id),
        fba_sensorpoll_timer_id = 0;

EXIT:
    return;
}

/** Evaluate need for temporary ALS enable termination
 */
static void fba_sensorpoll_rethink(void)
{
    if( fba_light_sensor_polling )
        fba_sensorpoll_start();
    else
        fba_sensorpoll_stop();
}

/* ========================================================================= *
 * LOAD_UNLOAD
 * ========================================================================= */

/** Init function for the ALS filter
 *
 * @param module (Unused)
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *
g_module_check_init(GModule *module)
{
    (void)module;

    fba_als_filter_init();

    fba_datapipe_init();

    fba_dbus_init();

    fba_setting_init();

    fba_inputflt_init();

    fba_status_rethink();

    return NULL;
}

/** Exit function for the ALS filter
 *
 * @param module (Unused)
 */
void
g_module_unload(GModule *module)
{
    (void)module;

    /* Mark that plugin is about to be unloaded */
    fba_module_unload = true;

    fba_setting_quit();

    fba_dbus_quit();

    fba_datapipe_quit();

    /* Final rethink to release wakelock & detach from sensorfw */
    fba_status_rethink();

    /* Make sure no timers with invalid callbacks are left active */
    fba_sensorpoll_stop();
    fba_inputflt_quit();

    g_free(fba_setting_als_input_filter),
        fba_setting_als_input_filter = 0;

    return;
}
