/**
 * @file event-input.c
 * /dev/input event provider for the Mode Control Entity
 * <p>
 * Copyright (c) 2004 - 2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (c) 2013 - 2020 Jolla Ltd.
 * Copyright (c) 2020 Open Mobile Platform LLC.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Ismo Laitinen <ismo.laitinen@nokia.com>
 * @author Tapio Rantala <ext-tapio.rantala@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Jukka Turunen <ext-jukka.t.turunen@nokia.com>
 * @author Mika Laitio <lamikr@pilppa.org>
 * @author Robin Burchell <robin+git@viroteck.net>
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

#include "event-input.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-io.h"
#include "mce-lib.h"
#include "mce-conf.h"
#include "mce-dbus.h"
#ifdef ENABLE_DOUBLETAP_EMULATION
# include "mce-setting.h"
#endif
#include "mce-sensorfw.h"
#include "multitouch.h"
#include "evdev.h"

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <glib/gstdio.h>
#include <gio/gio.h>

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

#ifndef SW_CAMERA_LENS_COVER
/** Input layer code for the camera lens cover switch */
# define SW_CAMERA_LENS_COVER           0x09
#endif

#ifndef SW_KEYPAD_SLIDE
/** Input layer code for the keypad slide switch */
# define SW_KEYPAD_SLIDE                0x0a
#endif

#ifndef SW_FRONT_PROXIMITY
/** Input layer code for the front proximity sensor switch */
# define SW_FRONT_PROXIMITY             0x0b
#endif

#ifndef KEY_CAMERA_FOCUS
/** Input layer code for the camera focus button */
# define KEY_CAMERA_FOCUS               0x0210
#endif

#ifndef FF_STATUS_CNT
# ifdef FF_STATUS_MAX
#  define FF_STATUS_CNT (FF_STATUS_MAX+1)
# else
#  define FF_STATUS_CNT 0
# endif
#endif

#ifndef PWR_CNT
# ifdef PWR_MAX
#  define PWR_CNT (PWR_MAX+1)
# else
#  define PWR_CNT 0
# endif
#endif

/* ========================================================================= *
 * DATA TYPES AND FUNCTION PROTOTYPES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * GPIO_KEYS  --  N900 camera focus key enable/disable policy
 * ------------------------------------------------------------------------- */

static void         evin_gpio_init                              (void);
static void         evin_gpio_key_enable                        (unsigned key);
static void         evin_gpio_key_disable                       (unsigned key);

/* ------------------------------------------------------------------------- *
 * EVENT_MAPPING  -- translate EV_SW events kernel sends to what mce expects
 * ------------------------------------------------------------------------- */

/** Structure for holding evdev event translation data
 */
typedef struct
{
    /** Event that kernel is emitting */
    struct input_event em_kernel_emits;

    /** Event mce is expecting to see */
    struct input_event em_mce_expects;
} evin_event_mapping_t;

static int          evin_event_mapping_guess_event_type         (const char *event_code_name);

static bool         evin_event_mapping_parse_event              (struct input_event *ev, const char *event_code_name);
static bool         evin_event_mapping_parse_config             (evin_event_mapping_t *self, const char *kernel_emits, const char *mce_expects);

static bool         evin_event_mapping_apply                    (const evin_event_mapping_t *self, struct input_event *ev);

static int          evin_event_mapper_rlookup_switch            (int expected_by_mce);
static void         evin_event_mapper_translate_event           (struct input_event *ev);

static void         evin_event_mapper_init                      (void);
static void         evin_event_mapper_quit                      (void);

/* ------------------------------------------------------------------------- *
 * EVDEVBITS
 * ------------------------------------------------------------------------- */

/** Calculate how many elements an array of longs bitmap needs to
 *  have enough space for bc items */
#define EVIN_EVDEVBITS_LEN(bc) (((bc)+LONG_BIT-1)/LONG_BIT)

/** Supported codes for one evdev event type
 */
typedef struct
{
    /** event type */
    int type;

    /** event code count for this type */
    int cnt;

    /** bitmask of supported event codes */
    unsigned long bit[0];
} evin_evdevbits_t;

static evin_evdevbits_t *evin_evdevbits_create                  (int type);
static void              evin_evdevbits_delete                  (evin_evdevbits_t *self);

static int               evin_evdevbits_probe                   (evin_evdevbits_t *self, int fd);

static void              evin_evdevbits_clear                   (evin_evdevbits_t *self);

static int               evin_evdevbits_test                    (const evin_evdevbits_t *self, int bit);

/* ------------------------------------------------------------------------- *
 * EVDEVINFO
 * ------------------------------------------------------------------------- */

/** Supported event types and codes for an evdev device node
 */
typedef struct
{
    /** Array of bitmasks for supported event types */
    evin_evdevbits_t *mask[EV_CNT];
} evin_evdevinfo_t;

static int               evin_evdevinfo_list_has_entry          (const int *list, int entry);

static evin_evdevinfo_t *evin_evdevinfo_create                  (void);
static void              evin_evdevinfo_delete                  (evin_evdevinfo_t *self);

static int               evin_evdevinfo_probe                   (evin_evdevinfo_t *self, int fd);

static int               evin_evdevinfo_has_type                (const evin_evdevinfo_t *self, int type);
static int               evin_evdevinfo_has_types               (const evin_evdevinfo_t *self, const int *types);

static int               evin_evdevinfo_has_code                (const evin_evdevinfo_t *self, int type, int code);
static int               evin_evdevinfo_has_codes               (const evin_evdevinfo_t *self, int type, const int *codes);

static int               evin_evdevinfo_match_types_ex          (const evin_evdevinfo_t *self, const int *types_req, const int *types_ign);
static int               evin_evdevinfo_match_types             (const evin_evdevinfo_t *self, const int *types);

static int               evin_evdevinfo_match_codes_ex          (const evin_evdevinfo_t *self, int type, const int *codes, const int *codes_ign);
static int               evin_evdevinfo_match_codes             (const evin_evdevinfo_t *self, int type, const int *codes);

static bool              evin_evdevinfo_is_volumekey_default    (const evin_evdevinfo_t *self);
static bool              evin_evdevinfo_is_volumekey_hammerhead (const evin_evdevinfo_t *self);
static bool              evin_evdevinfo_is_volumekey            (const evin_evdevinfo_t *self);
static bool              evin_evdevinfo_is_keyboard             (const evin_evdevinfo_t *self);

/* ------------------------------------------------------------------------- *
 * EVDEVTYPE
 * ------------------------------------------------------------------------- */

/** Types of use MCE can have for evdev input devices
 */
typedef enum {
    /** Sensors that might look like touch but should be ignored */
    EVDEV_REJECT,

    /** Touch screen to be tracked and processed */
    EVDEV_TOUCH,

    /** Mouse to be tracked and processed */
    EVDEV_MOUSE,

    /** Keys etc that mce needs to track and process */
    EVDEV_INPUT,

    /** Keys etc that mce itself does not need, tracked only for
     *  detecting user activity */
    EVDEV_ACTIVITY,

    /** The rest, mce does not track these */
    EVDEV_IGNORE,

    /** Button like touch device */
    EVDEV_DBLTAP,

    /** Proximity sensor */
    EVDEV_PS,

    /** Ambient light sensor */
    EVDEV_ALS,

    /** Volume key device */
    EVDEV_VOLKEY,

    /** Keyboard device */
    EVDEV_KEYBOARD,

    /** Device type was not explicitly set in configuration */
    EVDEV_UNKNOWN,
} evin_evdevtype_t;

static const char       *evin_evdevtype_repr                    (evin_evdevtype_t type);
static evin_evdevtype_t  evin_evdevtype_parse                   (const char *name);
static evin_evdevtype_t  evin_evdevtype_from_info               (evin_evdevinfo_t *info);

/* ------------------------------------------------------------------------- *
 * DOUBLETAP_EMULATION
 * ------------------------------------------------------------------------- */

#ifdef ENABLE_DOUBLETAP_EMULATION

static void         evin_doubletap_setting_cb                   (GConfClient *const client, const guint id, GConfEntry *const entry, gpointer const data);

#endif // ENABLE_DOUBLETAP_EMULATION

/* ------------------------------------------------------------------------- *
 * INI_FILE_HELPERS
 * ------------------------------------------------------------------------- */

static bool  evio_is_valid_key_char(int ch);
static char *evio_sanitize_key_name(const char *name);

/* ------------------------------------------------------------------------- *
 * EVDEV_IO_MONITORING
 * ------------------------------------------------------------------------- */

/** Cached capabilities and type of monitored evdev input device */
typedef struct
{
    /** Device name as reported by the driver */
    char             *ex_name;

    /** Cached device node capabilities */
    evin_evdevinfo_t *ex_info;

    /** Device type from mce point of view */
    evin_evdevtype_t  ex_type;

    /** Name of device that provides keypad slide state*/
    gchar             *ex_sw_keypad_slide;

    /** State data for multitouch/mouse input devices */
    mt_state_t        *ex_mt_state;
} evin_iomon_extra_t;

static void                evin_iomon_extra_delete_cb           (void *aptr);
static evin_iomon_extra_t *evin_iomon_extra_create              (int fd, const char *name);

// common rate limited activity generation

static void         evin_iomon_generate_activity                (struct input_event *ev, bool cooked, bool raw);

// event handling by device type

static bool         evin_iomon_sw_gestures_allowed              (void);
static gboolean     evin_iomon_touchscreen_cb                   (mce_io_mon_t *iomon, gpointer data, gsize bytes_read);
static gboolean     evin_iomon_evin_doubletap_cb                (mce_io_mon_t *iomon, gpointer data, gsize bytes_read);
static gboolean     evin_iomon_keypress_cb                      (mce_io_mon_t *iomon, gpointer data, gsize bytes_read);
static gboolean     evin_iomon_activity_cb                      (mce_io_mon_t *iomon, gpointer data, gsize bytes_read);

// add/remove devices

static void         evin_iomon_device_delete_cb                 (mce_io_mon_t *iomon);
static void         evin_iomon_device_rem_all                   (void);
static void         evin_iomon_device_add                       (const gchar *path);
static void         evin_iomon_device_update                    (const gchar *path, gboolean add);
static mce_io_mon_t*evin_iomon_lookup_device                    (const char *name);
static void         evin_iomon_device_iterate                   (evin_evdevtype_t type, GFunc func, gpointer data);

// check initial switch event states

static void         evin_iomon_switch_states_update_iter_cb     (gpointer io_monitor, gpointer user_data);
static void         evin_iomon_switch_states_update             (void);

static void         evin_iomon_keyboard_state_update_iter_cb    (gpointer io_monitor, gpointer user_data);
static void         evin_iomon_keyboard_state_update            (void);

static void         evin_iomon_mouse_state_update_iter_cb       (gpointer io_monitor, gpointer user_data);
static void         evin_iomon_mouse_state_update               (void);

// start/stop io monitoring

static bool         evin_iomon_init                             (void);
static void         evin_iomon_quit                             (void);

/* ------------------------------------------------------------------------- *
 * EVDEV_DIRECTORY_MONITORING
 * ------------------------------------------------------------------------- */

static void         evin_devdir_monitor_changed_cb              (GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data);

static bool         evin_devdir_monitor_init                    (void);
static void         evin_devdir_monitor_quit                    (void);

/* ------------------------------------------------------------------------- *
 * TOUCHSTATE_MONITORING
 * ------------------------------------------------------------------------- */

static void     evin_touchstate_iomon_iter_cb   (gpointer data, gpointer user_data);
static gboolean evin_touchstate_update_cb       (gpointer aptr);
static void     evin_touchstate_cancel_update   (void);
static void     evin_touchstate_schedule_update (void);

/* ------------------------------------------------------------------------- *
 * INPUT_GRAB  --  GENERIC EVDEV INPUT GRAB STATE MACHINE
 * ------------------------------------------------------------------------- */

/** Event input policy state */
typedef enum
{
    /** Initial value */
    EVIN_STATE_UNKNOWN  = 0,
    /** Input events can be processed normally */
    EVIN_STATE_ENABLED  = 1,
     /** Input events should be ignored */
    EVIN_STATE_DISABLED = 2,
} evin_state_t;

static const char *evin_state_repr(evin_state_t state);
static const char *evin_state_to_dbus(evin_state_t state);

typedef struct evin_input_grab_t evin_input_grab_t;

/** State information for generic input grabbing state machine */
struct evin_input_grab_t
{
    /** State machine instance name */
    const char  *ig_name;

    /** Current policy decision */
    evin_state_t ig_state;

    /** Currently touched/down */
    bool         ig_touching;

    /** Was touched/down, delaying release */
    bool         ig_touched;

    /** Input grab is wanted */
    bool         ig_want_grab;

    /** Input grab is allowed */
    bool         ig_allow_grab;

    /** Input grab should be active */
    bool         ig_have_grab;

    /** Input grab is active */
    bool         ig_real_grab;

    /** Delayed release timer */
    guint        ig_release_id;

    /** Delayed release delay */
    int          ig_release_ms;

    /** Callback for notifying grab status changes */
    void       (*ig_grab_changed_cb)(evin_input_grab_t *self, bool have_grab);

    /** Callback for additional release polling */
    bool       (*ig_release_verify_cb)(evin_input_grab_t *self);

    /** Callback for broadcasting policy changes */
    void       (*ig_state_changed_cb)(evin_input_grab_t *self);
};

static void         evin_input_grab_reset                       (evin_input_grab_t *self);
static gboolean     evin_input_grab_release_cb                  (gpointer aptr);
static void         evin_input_grab_start_release_timer         (evin_input_grab_t *self);
static void         evin_input_grab_cancel_release_timer        (evin_input_grab_t *self);
static void         evin_input_grab_rethink                     (evin_input_grab_t *self);
static void         evin_input_grab_set_touching                (evin_input_grab_t *self, bool touching);
static void         evin_input_grab_request_grab                (evin_input_grab_t *self, bool want_grab);
static void         evin_input_grab_allow_grab                  (evin_input_grab_t *self, bool allow_grab);
static void         evin_input_grab_iomon_cb                    (gpointer data, gpointer user_data);

/* ------------------------------------------------------------------------- *
 * TS_GRAB  --  TOUCHSCREEN EVDEV INPUT GRAB STATE MACHINE
 * ------------------------------------------------------------------------- */

static void         evin_ts_grab_set_led_raw                    (bool enabled);
static gboolean     evin_ts_grab_set_led_cb                     (gpointer aptr);
static void         evin_ts_grab_set_led                        (bool enabled);
static void         evin_ts_grab_rethink_led                    (void);

static void         evin_ts_grab_set_active                     (gboolean grab);

static bool         evin_ts_grab_poll_palm_detect               (evin_input_grab_t *ctrl);

static void         evin_ts_grab_changed                        (evin_input_grab_t *ctrl, bool grab);
static void         evin_ts_policy_changed                      (evin_input_grab_t *ctrl);

static void         evin_ts_grab_setting_cb                     (GConfClient *const client, const guint id, GConfEntry *const entry, gpointer const data);

static void         evin_ts_grab_init                           (void);
static void         evin_ts_grab_quit                           (void);

/* ------------------------------------------------------------------------- *
 * KP_GRAB  --  KEYPAD EVDEV INPUT GRAB STATE MACHINE
 * ------------------------------------------------------------------------- */

static void         evin_kp_grab_set_active                     (gboolean grab);
static void         evin_kp_grab_changed                        (evin_input_grab_t *ctrl, bool grab);
static void         evin_kp_policy_changed                      (evin_input_grab_t *ctrl);
static void         evin_kp_grab_event_filter_cb                (struct input_event *ev);

/* ------------------------------------------------------------------------- *
 * DYNAMIC_SETTINGS
 * ------------------------------------------------------------------------- */

static void         evin_setting_input_grab_rethink             (void);
static void         evin_setting_cb                             (GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);
static void         evin_setting_init                           (void);
static void         evin_setting_quit                           (void);

/* ------------------------------------------------------------------------- *
 * DBUS_HOOKS
 * ------------------------------------------------------------------------- */

static void         evin_dbus_send_keypad_input_policy      (DBusMessage *const req);
static gboolean     evin_dbus_keypad_input_policy_get_req_cb(DBusMessage *const msg);
static void         evin_dbus_send_touch_input_policy       (DBusMessage *const req);
static gboolean     evin_dbus_touch_input_policy_get_req_cb (DBusMessage *const msg);
static void         evin_dbus_init                         (void);
static void         evin_dbus_quit                         (void);

/* ------------------------------------------------------------------------- *
 * EVIN_DATAPIPE
 * ------------------------------------------------------------------------- */

static void  evin_datapipe_submode_cb                (gconstpointer data);
static void  evin_datapipe_touch_grab_wanted_cb      (gconstpointer data);
static void  evin_datapipe_touch_detected_cb         (gconstpointer data);
static void  evin_datapipe_display_state_curr_cb     (gconstpointer data);
static void  evin_datapipe_keypad_grab_wanted_cb     (gconstpointer data);
static void  evin_datapipe_display_state_next_cb     (gconstpointer data);
static void  evin_datapipe_proximity_sensor_actual_cb(gconstpointer data);
static void  evin_datapipe_lid_sensor_filtered_cb    (gconstpointer data);
static void  evin_datapipe_topmost_window_pid_cb     (gconstpointer data);
static void  evin_datapipe_call_state_cb             (gconstpointer data);
static void  evin_datapipe_interaction_expected_cb   (gconstpointer data);
static void  evin_datapipe_alarm_ui_state_cb         (gconstpointer data);
static void  evin_datapipe_init                      (void);
static void  evin_datapipe_quit                      (void);

/* ------------------------------------------------------------------------- *
 * MODULE_INIT
 * ------------------------------------------------------------------------- */

gboolean            mce_input_init                              (void);
void                mce_input_exit                              (void);

/* ========================================================================= *
 * EVIN_DATAPIPE
 * ========================================================================= */

/** Cached submode: Initialized to invalid placeholder value */
static submode_t submode = MCE_SUBMODE_INVALID;

/** Cached current display state */
static display_state_t display_state_curr = MCE_DISPLAY_UNDEF;

/** Cached target display state */
static display_state_t display_state_next = MCE_DISPLAY_UNDEF;

/** Cached touch input policy state */
static bool touch_grab_wanted = false;

/** Cached keypad input policy state */
static bool keypad_grab_wanted = false;

/** Cached finger on touchscreen state */
static bool touch_detected = false;

/** Cached (raw) proximity sensor state */
static cover_state_t proximity_sensor_actual = COVER_UNDEF;

/** Cached (filtered) lid sensor state */
static cover_state_t lid_sensor_filtered = COVER_UNDEF;

/** Cached PID of process owning the topmost window on UI */
static int topmost_window_pid = -1;

/** Cached alarm state; assume no active alarms */
static alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_OFF_INT32;

/** Cached call state */
static call_state_t call_state = CALL_STATE_INVALID;

/** Cached Interaction expected state */
static bool interaction_expected = false;

/* ========================================================================= *
 * GPIO_KEYS
 * ========================================================================= */

/** Can GPIO key interrupts be disabled? */
static gboolean evin_gpio_key_disable_exists = FALSE;

/** Check if enable/disable controls for gpio keys exist
 */
static void
evin_gpio_init(void)
{
    evin_gpio_key_disable_exists = (g_access(GPIO_KEY_DISABLE_PATH, W_OK) == 0);
}

/** Enable the specified GPIO key
 *
 * Non-existing or already enabled keys are silently ignored
 *
 * @param key The key to enable
 */
static void
evin_gpio_key_enable(unsigned key)
{
    gchar  *disabled_keys_old = NULL;
    gchar  *disabled_keys_new = NULL;
    gulong *bitmask           = NULL;
    gsize   bitmasklen;

    if( !mce_read_string_from_file(GPIO_KEY_DISABLE_PATH, &disabled_keys_old) )
        goto EXIT;

    bitmasklen = (KEY_CNT / bitsize_of(*bitmask)) +
        ((KEY_CNT % bitsize_of(*bitmask)) ? 1 : 0);
    bitmask = g_malloc0(bitmasklen * sizeof (*bitmask));

    if( !string_to_bitfield(disabled_keys_old, &bitmask, bitmasklen) )
        goto EXIT;

    clear_bit(key, &bitmask);

    if( !(disabled_keys_new = bitfield_to_string(bitmask, bitmasklen)) )
        goto EXIT;

    mce_write_string_to_file(GPIO_KEY_DISABLE_PATH, disabled_keys_new);

EXIT:
    g_free(disabled_keys_old);
    g_free(bitmask);
    g_free(disabled_keys_new);

    return;
}

/** Disable the specified GPIO key/switch
 *
 * non-existing or already disabled keys/switches are silently ignored
 *
 * @param key The key/switch to disable
 */
static void
evin_gpio_key_disable(unsigned key)
{
    gchar  *disabled_keys_old = NULL;
    gchar  *disabled_keys_new = NULL;
    gulong *bitmask           = NULL;
    gsize   bitmasklen;

    if( !mce_read_string_from_file(GPIO_KEY_DISABLE_PATH, &disabled_keys_old) )
        goto EXIT;

    bitmasklen = (KEY_CNT / bitsize_of(*bitmask)) +
        ((KEY_CNT % bitsize_of(*bitmask)) ? 1 : 0);
    bitmask = g_malloc0(bitmasklen * sizeof (*bitmask));

    if( !string_to_bitfield(disabled_keys_old, &bitmask, bitmasklen) )
        goto EXIT;

    set_bit(key, &bitmask);

    if( !(disabled_keys_new = bitfield_to_string(bitmask, bitmasklen)) )
        goto EXIT;

    mce_write_string_to_file(GPIO_KEY_DISABLE_PATH, disabled_keys_new);

EXIT:
    g_free(disabled_keys_old);
    g_free(bitmask);
    g_free(disabled_keys_new);

    return;
}

/** Disable/enable gpio keys based on submode changes
 *
 * @param data The submode stored in a pointer
 */
static void
evin_datapipe_submode_cb(gconstpointer data)
{
    submode_t prev = submode;
    submode = GPOINTER_TO_INT(data);

    if( prev != submode )
        mce_log(LL_DEBUG, "submode: %s", submode_change_repr(prev, submode));

    /* If the tklock is enabled, disable the camera focus interrupts,
     * since we don't use them anyway
     */
    if( evin_gpio_key_disable_exists ) {
        submode_t tklock_prev = (prev & MCE_SUBMODE_TKLOCK);
        submode_t tklock_curr = (submode & MCE_SUBMODE_TKLOCK);

        if( tklock_prev != tklock_curr ) {
            if( tklock_curr )
                evin_gpio_key_disable(KEY_CAMERA_FOCUS);
            else
                evin_gpio_key_enable(KEY_CAMERA_FOCUS);
        }
    }
}

/* ========================================================================= *
 * EVENT_MAPPING
 * ========================================================================= */

/** Guess event type from name of the event code
 *
 * @param event_code_name name of the event e.g. "SW_KEYPAD_SLIDE"
 *
 * @return event type id e.g. EV_SW, or -1 if unknown
 */
static int
evin_event_mapping_guess_event_type(const char *event_code_name)
{
    int etype = -1;

    if( !event_code_name )
        goto EXIT;

    /* We are interested only in EV_KEY and EV_SW events */

    if( !strncmp(event_code_name, "KEY_", 4) )
        etype = EV_KEY;
    else if( !strncmp(event_code_name, "BTN_", 4) )
        etype = EV_KEY;
    else if( !strncmp(event_code_name, "SW_", 3) )
        etype = EV_SW;

EXIT:
    return etype;
}

/** Fill in event type and code based on name of the event code
 *
 * @param event_code_name name of the event e.g. "SW_KEYPAD_SLIDE"
 *
 * @return true on success, or false on failure
 */
static bool
evin_event_mapping_parse_event(struct input_event *ev, const char *event_code_name)
{
    bool success = false;

    int etype = evin_event_mapping_guess_event_type(event_code_name);
    if( etype < 0 )
        goto EXIT;

    int ecode = evdev_lookup_event_code(etype, event_code_name);
    if( ecode < 0 )
        goto EXIT;

    ev->type = etype;
    ev->code = ecode;

    success  = true;

EXIT:
    return success;
}

/** Fill in event mapping structure from source and target event code names
 *
 * @param self pointer to a mapping structure to fill in
 * @param kernel_emits name of the event kernel will be emitting
 * @param mce_expects  mame of the event mce is expecting instead
 *
 * @return true on success, or false on failure
 */
static bool
evin_event_mapping_parse_config(evin_event_mapping_t *self,
                                const char *kernel_emits,
                                const char *mce_expects)
{
    bool success = false;

    if( !evin_event_mapping_parse_event(&self->em_kernel_emits, kernel_emits) )
        goto EXIT;

    if( !evin_event_mapping_parse_event(&self->em_mce_expects, mce_expects) )
        goto EXIT;

    success  = true;

EXIT:
    return success;
}

/** Translate event if applicable
 *
 * @param self pointer to a mapping structure to use
 * @param ev   input event to modify
 *
 * @param true if event was modified, false otherwise
 */
static bool
evin_event_mapping_apply(const evin_event_mapping_t *self, struct input_event *ev)
{
        bool applied = false;

    if( self->em_kernel_emits.type != ev->type )
        goto EXIT;

    if( self->em_kernel_emits.code != ev->code )
        goto EXIT;

    mce_log(LL_DEBUG, "map: %s:%s -> %s:%s",
            evdev_get_event_type_name(self->em_kernel_emits.type),
            evdev_get_event_code_name(self->em_kernel_emits.type,
                                      self->em_kernel_emits.code),
            evdev_get_event_type_name(self->em_mce_expects.type),
            evdev_get_event_code_name(self->em_mce_expects.type,
                                      self->em_mce_expects.code));

    ev->type = self->em_mce_expects.type;
    ev->code = self->em_mce_expects.code;

    applied = true;

EXIT:
    return applied;
}

/** Lookup table for translatable events */
static evin_event_mapping_t *evin_event_mapper_lut = 0;

/** Number of entries in evin_event_mapper_lut */
static size_t           evin_event_mapper_cnt = 0;

/** Reverse lookup switch kernel is emitting from switch mce is expecting
 *
 * Note: For use from event switch initial state evaluation only.
 *
 * @param expected_by_mce event code of SW_xxx kind mce expect to see
 *
 * @return event code of SW_xxx kind kernel might be sending
 */
static int
evin_event_mapper_rlookup_switch(int expected_by_mce)
{
    /* Assume kernel emits events mce is expecting to see */
    int emitted_by_kernel = expected_by_mce;

    /* If emitted_by_kernel -> expected_by_mce mapping exist, use it */
    for( size_t i = 0; i < evin_event_mapper_cnt; ++i ) {
        evin_event_mapping_t *map = evin_event_mapper_lut + i;

        if( map->em_kernel_emits.type != EV_SW )
            continue;

        if( map->em_mce_expects.type != EV_SW )
            continue;

        if( map->em_mce_expects.code != expected_by_mce )
            continue;

        emitted_by_kernel = map->em_kernel_emits.code;
        goto EXIT;
    }

    /* But if there is rule for mapping the event for something
     * else, it should be ignored instead of used as is */
    for( size_t i = 0; i < evin_event_mapper_cnt; ++i ) {
        evin_event_mapping_t *map = evin_event_mapper_lut + i;

        if( map->em_kernel_emits.type != EV_SW )
            continue;

        if( map->em_mce_expects.type != EV_SW )
            continue;

        if( map->em_kernel_emits.code != expected_by_mce )
            continue;

        /* Assumption: SW_MAX is valid index for ioctl() probing,
         *             but is not an alias for anything that kernel
         *             would report.
         */
        emitted_by_kernel = SW_MAX;
        goto EXIT;
    }

EXIT:
    return emitted_by_kernel;
}

/** Translate event emitted by kernel to something mce is expecting to see
 *
 * @param ev Input event to translate
 */
static void
evin_event_mapper_translate_event(struct input_event *ev)
{
    if( !ev )
        goto EXIT;

    /* Skip if there is no translation lookup table */
    if( !evin_event_mapper_lut )
        goto EXIT;

    /* We want to process key and switch events, but under all
     * potentially high frequency events should be skipped */
    switch( ev->type ) {
    case EV_KEY:
    case EV_SW:
        break;

    default:
        goto EXIT;
    }

    /* Try to find suitable mapping from lookup table */
    for( size_t i = 0; i < evin_event_mapper_cnt; ++i ) {
        evin_event_mapping_t *map = evin_event_mapper_lut + i;

        if( evin_event_mapping_apply(map, ev) )
            break;
    }

EXIT:
    return;
}

/** Initialize event translation lookup table
 */
static void
evin_event_mapper_init(void)
{
    static const char grp[] = MCE_CONF_EVDEV_GROUP;

    gchar **keys  = 0;
    gsize   count = 0;
    gsize   valid = 0;

    if( !mce_conf_has_group(grp) )
        goto EXIT;

    keys = mce_conf_get_keys(grp, &count);

    if( !keys || !count )
        goto EXIT;

    evin_event_mapper_lut = calloc(count, sizeof *evin_event_mapper_lut);

    for( gsize i = 0; i < count; ++i ) {
        evin_event_mapping_t *map = evin_event_mapper_lut + valid;
        const gchar     *key = keys[i];
        gchar           *val = mce_conf_get_string(grp, key, 0);

        if( val && evin_event_mapping_parse_config(map, key, val) )
            ++valid;

        g_free(val);
    }

    evin_event_mapper_cnt = valid;

EXIT:
    /* Remove also lookup table pointer if there are no entries */
    if( !evin_event_mapper_cnt )
        evin_event_mapper_quit();

    g_strfreev(keys);

    mce_log(LL_DEBUG, "EVDEV MAPS: %zd", evin_event_mapper_cnt);

    return;
}

/** Release event translation lookup table
 */
static void
evin_event_mapper_quit(void)
{
    free(evin_event_mapper_lut),
        evin_event_mapper_lut = 0,
        evin_event_mapper_cnt = 0;
}

/* ------------------------------------------------------------------------- *
 * EVDEVBITS
 * ------------------------------------------------------------------------- */

/** Create empty event code bitmap for one evdev event type
 *
 * @param type evdev event type
 *
 * @return evin_evdevbits_t object, or NULL for types not needed by mce
 */
static evin_evdevbits_t *
evin_evdevbits_create(int type)
{
    evin_evdevbits_t *self = 0;
    int cnt = 0;

    switch( type ) {
    case EV_SYN:       cnt = EV_CNT;        break;
    case EV_KEY:       cnt = KEY_CNT;       break;
    case EV_REL:       cnt = REL_CNT;       break;
    case EV_ABS:       cnt = ABS_CNT;       break;
    case EV_MSC:       cnt = MSC_CNT;       break;
    case EV_SW:        cnt = SW_CNT;        break;
#if 0
    case EV_LED:       cnt = LED_CNT;       break;
    case EV_SND:       cnt = SND_CNT;       break;
    case EV_REP:       cnt = REP_CNT;       break;
    case EV_FF:        cnt = FF_CNT;        break;
    case EV_PWR:       cnt = PWR_CNT;       break;
    case EV_FF_STATUS: cnt = FF_STATUS_CNT; break;
#endif
    default: break;
    }

    if( cnt > 0 ) {
        int len = EVIN_EVDEVBITS_LEN(cnt);
        self = g_malloc0(sizeof *self + len * sizeof *self->bit);
        self->type = type;
        self->cnt  = cnt;
    }
    return self;
}

/** Delete evdev event code bitmap
 *
 * @param self evin_evdevbits_t object, or NULL
 */
static void
evin_evdevbits_delete(evin_evdevbits_t *self)
{
    g_free(self);
}

/** Clear bits in evdev event code bitmap
 *
 * @param self evin_evdevbits_t object, or NULL
 */
static void
evin_evdevbits_clear(evin_evdevbits_t *self)
{
    if( self ) {
        int len = EVIN_EVDEVBITS_LEN(self->cnt);
        memset(self->bit, 0, len * sizeof *self->bit);
    }
}

/** Read supported codes from file descriptor
 *
 * @param self evin_evdevbits_t object, or NULL
 * @param fd file descriptor to probe data from
 *
 * @return 0 on success, -1 on errors
 */
static int
evin_evdevbits_probe(evin_evdevbits_t *self, int fd)
{
    int res = 0;
    if( self && ioctl(fd, EVIOCGBIT(self->type, self->cnt), self->bit) == -1 ) {
        mce_log(LL_WARN, "EVIOCGBIT(%s, %d): %m",
                evdev_get_event_type_name(self->type), self->cnt);
        evin_evdevbits_clear(self);
        res = -1;
    }
    return res;
}

/** Test if evdev event code is set in bitmap
 *
 * @param self evin_evdevbits_t object, or NULL
 * @param bit event code to check
 *
 * @return 1 if code is supported, 0 otherwise
 */
static int
evin_evdevbits_test(const evin_evdevbits_t *self, int bit)
{
    int res = 0;
    if( self && (unsigned)bit < (unsigned)self->cnt ) {
        int i = bit / LONG_BIT;
        unsigned long m = 1ul << (bit % LONG_BIT);
        if( self->bit[i] & m ) res = 1;
    }
    return res;
}

/* ------------------------------------------------------------------------- *
 * EVDEVINFO
 * ------------------------------------------------------------------------- */

/** Helper for checking if array of integers contains a particular value
 *
 * @param list  array of ints, terminated with -1
 * @param entry value to check
 *
 * @return 1 if value is present in the list, 0 if not
 */
static int
evin_evdevinfo_list_has_entry(const int *list, int entry)
{
    if( !list )
        return 0;

    for( int i = 0; list[i] != -1; ++i ) {
        if( list[i] == entry )
            return 1;
    }
    return 0;
}

/** Create evdev information object
 *
 * @return evin_evdevinfo_t object
 */
static evin_evdevinfo_t *
evin_evdevinfo_create(void)
{
    evin_evdevinfo_t *self = g_malloc0(sizeof *self);

    for( int i = 0; i < EV_CNT; ++i )
        self->mask[i] = evin_evdevbits_create(i);

    return self;
}

/** Delete evdev information object
 *
 * @param self evin_evdevinfo_t object
 */
static void
evin_evdevinfo_delete(evin_evdevinfo_t *self)
{
    if( self ) {
        for( int i = 0; i < EV_CNT; ++i )
            evin_evdevbits_delete(self->mask[i]);
        g_free(self);
    }
}

/** Check if event type is supported
 *
 * @param self evin_evdevinfo_t object
 * @param type evdev event type
 *
 * @return 1 if event type is supported, 0 otherwise
 */
static int
evin_evdevinfo_has_type(const evin_evdevinfo_t *self, int type)
{
    int res = 0;
    if( (unsigned)type < EV_CNT )
        res = evin_evdevbits_test(self->mask[0], type);
    return res;
}

/** Check if any of given event types are supported
 *
 * @param self evin_evdevinfo_t object
 * @param types array of evdev event types
 *
 * @return 1 if at least on of the types is supported, 0 otherwise
 */
static int
evin_evdevinfo_has_types(const evin_evdevinfo_t *self, const int *types)
{
    int res = 0;
    for( size_t i = 0; types[i] >= 0; ++i ) {
        if( (res = evin_evdevinfo_has_type(self, types[i])) )
            break;
    }
    return res;
}

/** Check if event code is supported
 *
 * @param self evin_evdevinfo_t object
 * @param type evdev event type
 * @param code evdev event code
 *
 * @return 1 if event code for type is supported, 0 otherwise
 */
static int
evin_evdevinfo_has_code(const evin_evdevinfo_t *self, int type, int code)
{
    int res = 0;

    if( evin_evdevinfo_has_type(self, type) )
        res = evin_evdevbits_test(self->mask[type], code);
    return res;
}

/** Check if any of given event codes are supported
 *
 * @param self evin_evdevinfo_t object
 * @param type evdev event type
 * @param code array of evdev event codes
 *
 * @return 1 if at least on of the event codes for type is supported, 0 otherwise
 */
static int
evin_evdevinfo_has_codes(const evin_evdevinfo_t *self, int type, const int *codes)
{
    int res = 0;

    if( evin_evdevinfo_has_type(self, type) ) {
        for( size_t i = 0; codes[i] != -1; ++i ) {
            if( (res = evin_evdevbits_test(self->mask[type], codes[i])) )
                break;
        }
    }
    return res;
}

/** Check if all of the listed types and only the listed types are supported
 *
 * @param self      evin_evdevinfo_t object
 * @param types_req array of required evdev event types, terminated with -1
 * @param types_ign array event types to ignore, terminated with -1; or null
 *
 * @return 1 if all of types and only types are supported, 0 otherwise
 */
static int
evin_evdevinfo_match_types_ex(const evin_evdevinfo_t *self,
                              const int *types_req, const int *types_ign)
{
    for( int etype = 1; etype < EV_CNT; ++etype ) {
        if( evin_evdevinfo_list_has_entry(types_ign, etype) )
            continue;

        int have = evin_evdevinfo_has_type(self, etype);
        int want = evin_evdevinfo_list_has_entry(types_req, etype);
        if( have != want )
            return 0;
    }
    return 1;
}

/** Check if all of the listed types and only the listed types are supported
 *
 * @param self  evin_evdevinfo_t object
 * @param types array of evdev event types, terminated with -1
 *
 * @return 1 if all of types and only types are supported, 0 otherwise
 */
static int
evin_evdevinfo_match_types(const evin_evdevinfo_t *self, const int *types)
{
    return evin_evdevinfo_match_types_ex(self, types, 0);
}

/** Check if all of the listed codes and only the listed codes are supported
 *
 * @param self      evin_evdevinfo_t object
 * @param types     evdev event type
 * @param codes     array of evdev event codes, terminated with -1
 * @param codes_ign array of dontcare evdev event codes, terminated with -1
 *
 * @return 1 if all of codes and only codes are supported, 0 otherwise
 */
static int
evin_evdevinfo_match_codes_ex(const evin_evdevinfo_t *self, int type,
                              const int *codes, const int *codes_ign)
{
    for( int ecode = 0; ecode < KEY_CNT; ++ecode ) {
        if( evin_evdevinfo_list_has_entry(codes_ign, ecode) )
            continue;

        int have = evin_evdevinfo_has_code(self, type, ecode);
        int want = evin_evdevinfo_list_has_entry(codes, ecode);
        if( have != want )
            return 0;
    }
    return 1;
}

/** Check if all of the listed codes and only the listed codes are supported
 *
 * @param self  evin_evdevinfo_t object
 * @param types evdev event type
 * @param codes array of evdev event codes, terminated with -1
 *
 * @return 1 if all of codes and only codes are supported, 0 otherwise
 */
static int
evin_evdevinfo_match_codes(const evin_evdevinfo_t *self, int type, const int *codes)
{
    return evin_evdevinfo_match_codes_ex(self, type, codes, 0);
}

#ifndef  KEY_CAMERA_SNAPSHOT
# define KEY_CAMERA_SNAPSHOT 0x02fe
#endif

/** Test if input device sends only volume key events
 *
 * @param self  evin_evdevinfo_t object
 *
 * @return true if info matches volume keys only device, false otherwise
 */
static bool
evin_evdevinfo_is_volumekey_default(const evin_evdevinfo_t *self)
{
    /* Emits volume key events, and only volume key events */
    static const int wanted_types[] = {
        EV_KEY,
        -1
    };

    static const int wanted_key_codes[] = {
        KEY_VOLUMEDOWN,
        KEY_VOLUMEUP,
        -1
    };
    static const int ignored_key_codes[] = {
        /* Getting some key blocked/unblocked based on
         * volume key policy is less harmful than leaving
         * the volume keys active all the time. */
        KEY_CAMERA_FOCUS,
        KEY_CAMERA_SNAPSHOT,
        KEY_CAMERA,

        /* Home key should be handled by mce and can be
         * ignored as well. */
        KEY_HOME,
        -1
    };

    /* Except we do not care if autorepeat controls are there or not */
    static const int ignored_types[] = {
        EV_REP,
        -1
    };

    return (evin_evdevinfo_match_types_ex(self, wanted_types, ignored_types) &&
            evin_evdevinfo_match_codes_ex(self, EV_KEY, wanted_key_codes,
                                          ignored_key_codes));
}

/** Test if input device is like volume key device in Nexus 5
 *
 * In addition to volume keys, the input device also reports
 * lid sensor state.
 *
 * @param self  evin_evdevinfo_t object
 *
 * @return true if info matches Nexus 5 volume key device, false otherwise
 */
static bool
evin_evdevinfo_is_volumekey_hammerhead(const evin_evdevinfo_t *self)
{
    static const int wanted_types[] = {
        EV_KEY,
        EV_SW,
        -1
    };

    static const int wanted_key_codes[] = {
        KEY_VOLUMEDOWN,
        KEY_VOLUMEUP,
        -1
    };

    static const int ignored_key_codes[] = {
        /* Getting camera focus blocked/unblocked based on
         * volume key policy is less harmful than leaving
         * the volume keys active all the time. */
        KEY_CAMERA_FOCUS,
        -1
    };

    static const int wanted_sw_codes[] = {
        SW_LID, // magnetic lid cover sensor
        -1
    };

    return (evin_evdevinfo_match_types(self, wanted_types) &&
            evin_evdevinfo_match_codes_ex(self, EV_KEY, wanted_key_codes,
                                          ignored_key_codes) &&
            evin_evdevinfo_match_codes(self, EV_SW, wanted_sw_codes));
}

/** Test if input device is grabbable volume key device
 *
 * @param self  evin_evdevinfo_t object
 *
 * @return true if info matches grabbable volume key device, false otherwise
 */
static bool
evin_evdevinfo_is_volumekey(const evin_evdevinfo_t *self)
{
    /* Note: If device node - in addition to volume keys - serves
     *       events that should always be made available to other
     *       processes too (KEY_POWER, SW_HEADPHONE_INSERT, etc),
     *       it should not be detected as grabbable volume key.
     */

    return (evin_evdevinfo_is_volumekey_default(self) ||
            evin_evdevinfo_is_volumekey_hammerhead(self));
}

static bool
evin_evdevinfo_is_keyboard(const evin_evdevinfo_t *self)
{
    return (evin_evdevinfo_has_type(self, EV_KEY) &&
            evin_evdevinfo_has_code(self, EV_KEY, KEY_Q) &&
            evin_evdevinfo_has_code(self, EV_KEY, KEY_P));
}

/** Fill in evdev data by probing file descriptor
 *
 * @param self evin_evdevinfo_t object
 * @param fd file descriptor to probe data from
 *
 * @return 0 on success, -1 on errors
 */
static int
evin_evdevinfo_probe(evin_evdevinfo_t *self, int fd)
{
    int res = evin_evdevbits_probe(self->mask[0], fd);

    for( int i = 1; i < EV_CNT; ++i ) {
        if( evin_evdevbits_test(self->mask[0], i) )
            evin_evdevbits_probe(self->mask[i], fd);
        else
            evin_evdevbits_clear(self->mask[i]);
    }

    return res;
}

/* ========================================================================= *
 * EVDEVTYPE
 * ========================================================================= */

/** Get Human readable evdev classifications for debugging purposes
 */
static const char *
evin_evdevtype_repr(evin_evdevtype_t type)
{
    static const char * const lut[] =
    {
        [EVDEV_REJECT]   = "REJECT",
        [EVDEV_TOUCH]    = "TOUCHSCREEN",
        [EVDEV_MOUSE]    = "MOUSE",
        [EVDEV_INPUT]    = "KEY, BUTTON or SWITCH",
        [EVDEV_ACTIVITY] = "USER ACTIVITY ONLY",
        [EVDEV_IGNORE]   = "IGNORE",
        [EVDEV_DBLTAP]   = "DOUBLE TAP",
        [EVDEV_PS]       = "PROXIMITY SENSOR",
        [EVDEV_ALS]      = "AMBIENT LIGHT SENSOR",
        [EVDEV_VOLKEY]   = "VOLUME KEYS",
        [EVDEV_KEYBOARD] = "KEYBOARD",
        [EVDEV_UNKNOWN]  = "UNKNOWN",
    };

    return lut[type];
}

/** Convert textual evdev classification from config file to enum value
 *
 * @param name evdev device type from configuration file
 *
 * @return Return corresponding device type id, or EVDEV_UNKNOWN
 */
static evin_evdevtype_t
evin_evdevtype_parse(const char *name)
{
    static const struct
    {
        const char *key;
        int         val;
    } lut[] =
    {
        { "REJECT",               EVDEV_REJECT,    },
        { "TOUCH",                EVDEV_TOUCH,     },
        { "MOUSE",                EVDEV_MOUSE,     },
        { "INPUT",                EVDEV_INPUT,     },
        { "ACTIVITY",             EVDEV_ACTIVITY,  },
        { "IGNORE",               EVDEV_IGNORE,    },
        { "DOUBLE_TAP",           EVDEV_DBLTAP,    },
        { "DBLTAP",               EVDEV_DBLTAP,    },
        { "PS",                   EVDEV_PS,        },
        { "PROXIMITY_SENSOR",     EVDEV_PS,        },
        { "ALS",                  EVDEV_ALS,       },
        { "LIGHT_SENSOR",         EVDEV_ALS,       },
        { "VOLKEY",               EVDEV_VOLKEY,    },
        { "VOLUME_KEYS",          EVDEV_VOLKEY,    },
        { "KEYBOARD",             EVDEV_KEYBOARD,  },

        /* Note: EVDEV_UNKNOWN is left out on purpose as it
         *       signifies parsing error and thus is not
         *       meant to be used in configuration files. */
    };

    evin_evdevtype_t type = EVDEV_UNKNOWN;

    if( !name )
        goto EXIT;

    for( size_t i = 0; i < G_N_ELEMENTS(lut); ++i ) {
        if( strcmp(lut[i].key, name) )
            continue;

        type = lut[i].val;
        break;
    }

EXIT:
    return type;
}

/** Use heuristics to determine what mce should do with an evdev device node
 *
 * @param info  Event types and codes emitted by a evdev device
 *
 * @return one of EVDEV_TOUCH, EVDEV_INPUT, ...
 */
static evin_evdevtype_t
evin_evdevtype_from_info(evin_evdevinfo_t *info)
{
    /* EV_ABS probing arrays for ALS/PS detection */
    static const int abs_only[]  = { EV_ABS, -1 };
    static const int misc_only[] = { ABS_MISC, -1 };
    static const int dist_only[] = { ABS_DISTANCE, -1 };

    /* EV_KEY probing arrays for detecting input devices
     * that report double tap gestures as power key events */
    static const int key_only[]   = { EV_KEY, -1 };
    static const int dbltap_lut[] = {
        KEY_POWER,
        KEY_MENU,
        KEY_BACK,
        KEY_HOMEPAGE,
        -1
    };
    /* Key events mce is interested in */
    static const int keypad_lut[] = {
        KEY_CAMERA,
        KEY_CAMERA_FOCUS,
        KEY_POWER,
        KEY_SCREENLOCK,
        KEY_VOLUMEDOWN,
        KEY_VOLUMEUP,
        KEY_WAKEUP,
        -1
    };

    /* Switch events mce is interested in */
    static const int switch_lut[] = {
        SW_CAMERA_LENS_COVER,
        SW_FRONT_PROXIMITY,
        SW_HEADPHONE_INSERT,
        SW_KEYPAD_SLIDE,
        SW_LID,
        SW_LINEOUT_INSERT,
        SW_MICROPHONE_INSERT,
        SW_VIDEOOUT_INSERT,
        -1
    };

    /* Event classes that could be due to "user activity" */
    static const int misc_lut[] = {
        EV_KEY,
        EV_REL,
        EV_ABS,
        EV_MSC,
        EV_SW,
        -1
    };

    /* All event classes except EV_ABS */
    static const int all_but_abs_lut[] = {
        EV_KEY,
        EV_REL,
        EV_MSC,
        EV_SW,
        EV_LED,
        EV_SND,
        EV_REP,
        EV_FF,
        EV_PWR,
        EV_FF_STATUS,
        -1
    };

    int res = EVDEV_IGNORE;

    /* Ambient light and proximity sensor inputs */
    if( evin_evdevinfo_match_types(info, abs_only) ) {
        if( evin_evdevinfo_match_codes(info, EV_ABS, misc_only) ) {
            // only EV_ABS:ABS_MISC -> ALS
            res = EVDEV_ALS;
            goto cleanup;
        }
        if( evin_evdevinfo_match_codes(info, EV_ABS, dist_only) ) {
            // only EV_ABS:ABS_DISTANCE -> PS
            res = EVDEV_PS;
            goto cleanup;
        }
    }

    /* MCE has no use for accelerometers etc */
    if( evin_evdevinfo_has_code(info, EV_KEY, BTN_Z) ||
        evin_evdevinfo_has_code(info, EV_REL, REL_Z) ||
        evin_evdevinfo_has_code(info, EV_ABS, ABS_Z) ) {
        // 3d sensor like accelorometer/magnetometer
        res = EVDEV_REJECT;
        goto cleanup;
    }

    /* While MCE mostly uses touchscreen inputs only for
     * "user activity" monitoring, the touch devices
     * generate a lot of events and mce has mechanism in
     * place to avoid processing all of them */
    if( evin_evdevinfo_has_code(info, EV_KEY, BTN_TOUCH) &&
        evin_evdevinfo_has_code(info, EV_ABS, ABS_X)     &&
        evin_evdevinfo_has_code(info, EV_ABS, ABS_Y) ) {
        // singletouch protocol
        res = EVDEV_TOUCH;
        goto cleanup;
    }
    if( evin_evdevinfo_has_code(info, EV_ABS, ABS_MT_POSITION_X) &&
        evin_evdevinfo_has_code(info, EV_ABS, ABS_MT_POSITION_Y) ) {
        // multitouch protocol
        res = EVDEV_TOUCH;
        goto cleanup;
    }

    /* In SDK we might bump into mouse devices, track them
     * as if they were touch screen devices */
    if( evin_evdevinfo_has_code(info, EV_KEY, BTN_MOUSE) &&
        evin_evdevinfo_has_code(info, EV_REL, REL_X) &&
        evin_evdevinfo_has_code(info, EV_REL, REL_Y) ) {
        // mouse
        res = EVDEV_MOUSE;
        goto cleanup;
    }

    /* Touchscreen that emits power key events on double tap */
    if( evin_evdevinfo_match_types(info, key_only) &&
        evin_evdevinfo_match_codes(info, EV_KEY, dbltap_lut) ) {
        res = EVDEV_DBLTAP;
        goto cleanup;
    }

    /* Presense of keyboard devices needs to be signaled */
    if( evin_evdevinfo_is_keyboard(info) ) {
        res = EVDEV_KEYBOARD;
        goto cleanup;
    }

    /* Volume keys only input devices can be grabbed */
    if( evin_evdevinfo_is_volumekey(info) ) {
        res = EVDEV_VOLKEY;
        goto cleanup;
    }

    /* Some keys and swithes are processed at mce level */
    if( evin_evdevinfo_has_codes(info, EV_KEY, keypad_lut ) ||
        evin_evdevinfo_has_codes(info, EV_SW,  switch_lut ) ) {
        res = EVDEV_INPUT;
        goto cleanup;
    }

    /* Also gesture events from an input device that does not
     * emit touch events need to be handled as double taps etc. */
    if( evin_evdevinfo_has_code(info, EV_MSC, MSC_GESTURE) ) {
        res = EVDEV_DBLTAP;
        goto cleanup;
    }

    /* Assume that: devices that support only ABS_DISTANCE are
     * proximity sensors and devices that support only ABS_MISC
     * are ambient light sensors that are handled via libhybris
     * in more appropriate place and should not be used for
     * "user activity" tracking. */
    if( evin_evdevinfo_has_type(info, EV_ABS) &&
        !evin_evdevinfo_has_types(info, all_but_abs_lut) ) {
        int maybe_als = evin_evdevinfo_has_code(info, EV_ABS, ABS_MISC);
        int maybe_ps  = evin_evdevinfo_has_code(info, EV_ABS, ABS_DISTANCE);

        // supports one of the two, but not both ...
        if( maybe_als != maybe_ps ) {
            for( int code = 0; ; ++code ) {
                switch( code ) {
                case ABS_CNT:
                    // ... and no other events supported
                    res = EVDEV_REJECT;
                    goto cleanup;

                case ABS_DISTANCE:
                case ABS_MISC:
                    continue;

                default:
                    break;
                }
                if( evin_evdevinfo_has_code(info, EV_ABS, code) )
                    break;
            }
        }
    }

    /* Ignore devices that emit only X or Y values */
    if( (evin_evdevinfo_has_code(info, EV_KEY, BTN_X) !=
         evin_evdevinfo_has_code(info, EV_KEY, BTN_Y)) ||
        (evin_evdevinfo_has_code(info, EV_REL, REL_X) !=
         evin_evdevinfo_has_code(info, EV_REL, REL_Y)) ||
        (evin_evdevinfo_has_code(info, EV_ABS, ABS_X) !=
         evin_evdevinfo_has_code(info, EV_ABS, ABS_Y)) ) {
        // assume unknown 1d sensor like als/proximity
        res = EVDEV_REJECT;
        goto cleanup;
    }

    /* Track events that can be considered as "user activity" */
    if( evin_evdevinfo_has_types(info, misc_lut) ) {
        res = EVDEV_ACTIVITY;
        goto cleanup;
    }

cleanup:

    return res;
}

/* ------------------------------------------------------------------------- *
 * DOUBLETAP_EMULATION
 * ------------------------------------------------------------------------- */

#ifdef ENABLE_DOUBLETAP_EMULATION

/** Fake doubletap policy */
static gboolean evin_doubletap_emulation_enabled = MCE_DEFAULT_USE_FAKE_DOUBLETAP;
static guint    evin_doubletap_emulation_enabled_setting_id = 0;

/** Callback for handling changes to fake doubletap configuration
 *
 * @param client (not used)
 * @param id     (not used)
 * @param entry  GConf entry that changed
 * @param data   (not used)
 */
static void
evin_doubletap_setting_cb(GConfClient *const client, const guint id,
                          GConfEntry *const entry, gpointer const data)
{
    (void)client; (void)id; (void)data;

    gboolean enabled = evin_doubletap_emulation_enabled;
    const GConfValue *value = 0;

    if( entry && (value = gconf_entry_get_value(entry)) ) {
        if( value->type == GCONF_VALUE_BOOL )
            enabled = gconf_value_get_bool(value);
    }
    if( evin_doubletap_emulation_enabled != enabled ) {
        mce_log(LL_NOTICE, "use fake doubletap change: %d -> %d",
                evin_doubletap_emulation_enabled, enabled);
        evin_doubletap_emulation_enabled = enabled;
    }
}

#endif /* ENABLE_DOUBLETAP_EMULATION */

/* ========================================================================= *
 * INI_FILE_HELPERS
 * ========================================================================= */

/** Predicate for: character can be used in glib keyfile key keyname
 *
 * @param ch character
 *
 * @return true if character can be used as is, false otherwise
 */
static bool
evio_is_valid_key_char(int ch)
{
    /* Skip negatives, ascii control chars / white space */
    if( ch <= 0x20 )
        return false;

    /* Keys must be utf-8 and we do not control what kernel
     * returns -> skip stuff that is not ascii-7 pure */
    if( ch >= 0x80 )
        return false;

    /* Square brackets are used for keyfile groups or
     * specifying language specific variant values */
    if( ch == '[' || ch == ']' )
        return false;

    /* And '=' is used for separating keys from values */
    if( ch == '=' )
        return false;

    /* Assume everything else is ok */
    return true;
}

/** Sanitize c-string to "usable as ini file key" form
 *
 * Dynamically obtained strings - such as evdev device names queried
 * from kernel - might contain characters that are not allowed in
 * glib keyfile keys. This function performs one way transformation
 * that allows turning any c-string into a form that can be used
 * as key name.
 *
 * Leading and trailing illegal characters are skipped altogether.
 *
 * Sequences of mid-string illegal characters are squeezed into
 * single underscores.
 *
 * Caller must release the returned string via free().
 *
 * Examples:
 *   "gpio-keys" -> "gpio-keys" (no change)
 *   "  some thing [x=7]  " -> "some_thing_x_7"
 *
 * @param name Device name
 *
 * @return Device name without illegal characters, or NULL
 */
static char *
evio_sanitize_key_name(const char *name)
{
    char *key = 0;

    if( !name )
        goto EXIT;

    if( !(key = strdup(name)) )
        goto EXIT;

    char *src = key;
    char *dst = key;

    while( *src && !evio_is_valid_key_char(*src) )
        ++src;

    for( ;; ) {
        while( *src && evio_is_valid_key_char(*src) )
            *dst++ = *src++;

        while( *src && !evio_is_valid_key_char(*src) )
            ++src;

        if( !*src )
            break;

        *dst++ = '_';
    }

    *dst = 0;

EXIT:
    return key;
}

/* ========================================================================= *
 * EVDEV_IO_MONITORING
 * ========================================================================= */

static void
evin_iomon_extra_delete_cb(void *aptr)
{
    evin_iomon_extra_t *self = aptr;

    if( self ) {
        mt_state_delete(self->ex_mt_state),
            self->ex_mt_state = 0;

        evin_evdevinfo_delete(self->ex_info);
        g_free(self->ex_sw_keypad_slide);
        free(self->ex_name);
        free(self);
    }
}

static evin_iomon_extra_t *
evin_iomon_extra_create(int fd, const char *name)
{
    evin_iomon_extra_t *self      = calloc(1, sizeof *self);
    gchar              *config    = 0;
    char               *key       = 0;
    char               *id        = 0;
    struct input_id     info      = {};

    /* Initialize extra info to sane defaults */
    self->ex_name            = strdup(name);
    self->ex_info            = evin_evdevinfo_create();
    self->ex_type            = EVDEV_UNKNOWN;
    self->ex_sw_keypad_slide = 0;
    self->ex_mt_state        = 0;

    evin_evdevinfo_probe(self->ex_info, fd);

    /* Check if evdev device type has been set in the configuration
     *
     * First lookup using bus-vendor-product based name,
     * then as a fallback lookup using sanitized device name.
     */
    if( ioctl(fd, EVIOCGID, &info) < 0 ) {
        mce_log(LL_WARN, "EVIOCGID: N/A (%m)");
    }
    else {
        id = g_strdup_printf("b%04xv%04xp%04x",
                             info.bustype,
                             info.vendor,
                             info.product);
    }

    if( id ) {
        config = mce_conf_get_string(MCE_CONF_EVDEV_TYPE_GROUP, id, 0);
    }

    if( !config ) {
        key  = evio_sanitize_key_name(name);
        config = mce_conf_get_string(MCE_CONF_EVDEV_TYPE_GROUP, key, 0);
    }

    /* Heuristics based type detection */
    evin_evdevtype_t probed = evin_evdevtype_from_info(self->ex_info);

    /* Override based on configuration */
    if( config ) {
        /* RULE  := <TYPE_TO_USE>[':'<ON_PROBED_TYPE>[':'<RESERVED>]]
         * RULES := <RULE>[';'<RULE>]...
         */
        char *rules = config;
        for( char *rule; *(rule = mce_slice_token(rules, &rules, ";")); ) {
            const char *arg1 = mce_slice_token(rule, &rule, ":");
            const char *arg2 = mce_slice_token(rule, &rule, ":");

            evin_evdevtype_t configured = EVDEV_UNKNOWN;
            evin_evdevtype_t replaces   = EVDEV_UNKNOWN;

            if( *arg1 ) {
                if( (configured = evin_evdevtype_parse(arg1)) == EVDEV_UNKNOWN )
                    mce_log(LL_WARN, "unknown evdev device type '%s'", arg1);
            }

            if( *arg2 ) {
                if( (replaces = evin_evdevtype_parse(arg2)) == EVDEV_UNKNOWN )
                    mce_log(LL_WARN, "unknown evdev device type '%s'", arg2);
            }

            if( replaces == EVDEV_UNKNOWN || replaces == probed ) {
                /* Unconditional / condition matched
                 * -> use configured / keep probed type
                 */
                if( configured != EVDEV_UNKNOWN )
                    probed = configured;
                break;
            }
        }
    }

    self->ex_type = probed;

    /* Initialize type specific tracking data */

    if( self->ex_type == EVDEV_KEYBOARD ) {
        self->ex_sw_keypad_slide = mce_conf_get_string("SW_KEYPAD_SLIDE",
                                                       self->ex_name, 0);
    }

    if( self->ex_type == EVDEV_TOUCH ||
        self->ex_type == EVDEV_MOUSE ||
        self->ex_type == EVDEV_DBLTAP ) {
        bool protocol_b = evin_evdevinfo_has_code(self->ex_info,
                                                  EV_ABS, ABS_MT_SLOT);
        self->ex_mt_state = mt_state_create(protocol_b);
    }

    g_free(config);
    free(key);
    g_free(id);

    return self;
}

/** List of monitored evdev input devices */
static GSList *evin_iomon_device_list = NULL;

/** Handle touch device iomon delete notification
 *
 * @param iomon I/O monitor that is about to get deleted
 */
static void
evin_iomon_device_delete_cb(mce_io_mon_t *iomon)
{
    evin_iomon_device_list = g_slist_remove(evin_iomon_device_list, iomon);
}

/** Locate I/O monitor object by device name
 *
 * @param name Name of the device
 *
 * @return iomon object or NULL if not found
 */
static mce_io_mon_t *
evin_iomon_lookup_device(const char *name)
{
    mce_io_mon_t *res = 0;

    if( !name )
        goto EXIT;

    for( GSList *item = evin_iomon_device_list; item; item = item->next ) {
        mce_io_mon_t *iomon = item->data;

        if( !iomon )
            continue;

        evin_iomon_extra_t *extra = mce_io_mon_get_user_data(iomon);

        if( !extra )
            continue;

        if( strcmp(extra->ex_name, name) )
            continue;

        res = iomon;
        break;
    }

EXIT:
    return res;
}

static void
evin_iomon_device_iterate(evin_evdevtype_t type, GFunc func, gpointer data)
{
    GSList *item;

    for( item = evin_iomon_device_list; item; item = item->next ) {
        mce_io_mon_t *iomon = item->data;

        if( !iomon )
            continue;

        evin_iomon_extra_t *extra = mce_io_mon_get_user_data(iomon);

        if( !extra )
            continue;

        if( extra->ex_type == type )
            func(iomon, data);
    }
}

/** Remove all touch device I/O monitors
 */
static void
evin_iomon_device_rem_all(void)
{
    mce_io_mon_unregister_list(evin_iomon_device_list),
        evin_iomon_device_list = 0;
}

/** Handle emitting of generic and/or genuine user activity
 *
 * To avoid excessive timer reprogramming the activity signaling is
 * rate limited to occur once / second.
 *
 * @param ev       Input event that caused activity reporting
 * @param cooked   True, if generic activity should be sent
 * @param raw      True, if non-synthetized activity should be sent
 */
static void
evin_iomon_generate_activity(struct input_event *ev, bool cooked, bool raw)
{
    static time_t t_cooked = 0;
    static time_t t_raw    = 0;

    if( !ev )
        goto EXIT;

    time_t t = ev->input_event_sec;

    /* Actual, never synthetized user activity */
    if( raw ) {
        if( t_raw != t ) {
            t_raw = t;
            datapipe_exec_full(&user_activity_event_pipe, ev);
        }
    }

    /* Generic, possibly synthetized user activity */
    if( cooked ) {
        if( t_cooked != t || (submode & MCE_SUBMODE_EVEATER) ) {
            t_cooked = t;
            mce_datapipe_generate_activity();
        }
    }

EXIT:
    return;
}

/** Predicate for using touch input for sw gestures is allowed
 *
 * @returns true if gesture events can be injected, false otherwise
 */
static bool
evin_iomon_sw_gestures_allowed(void)
{
    bool gestures_allowed = false;

    /* No simulated gestures unless only mce is supposed to
     * handle touch input */
    bool grabbed = touch_grab_wanted;
    if( !grabbed )
        goto EXIT;

    /* The setting must be enabled */
    if( !evin_doubletap_emulation_enabled )
        goto EXIT;

    /* And the display must be firmly in logically off state */
    switch( display_state_next ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
        break;
    default:
        goto EXIT;
    }

    switch( display_state_curr ) {
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_OFF:
    case MCE_DISPLAY_LPM_ON:
        break;
    default:
        goto EXIT;
    }

    gestures_allowed = true;

EXIT:
    return gestures_allowed;
}

/** I/O monitor callback for handling touchscreen events
 *
 * @param data       The new data
 * @param bytes_read The number of bytes read
 *
 * @return FALSE to return remaining chunks (if any),
 *         TRUE to flush all remaining chunks
 */
static gboolean
evin_iomon_touchscreen_cb(mce_io_mon_t *iomon, gpointer data, gsize bytes_read)
{
    (void)iomon;

    gboolean flush = FALSE;
    struct input_event *ev = data;

    if( ev == 0 || bytes_read != sizeof *ev )
        goto EXIT;

    /* Map event before processing */
    evin_event_mapper_translate_event(ev);

    mce_log(LL_DEBUG, "type: %s, code: %s, value: %d",
            evdev_get_event_type_name(ev->type),
            evdev_get_event_code_name(ev->type, ev->code),
            ev->value);

    bool grabbed = touch_grab_wanted;

    bool doubletap = false;

    evin_iomon_extra_t *extra = mce_io_mon_get_user_data(iomon);
    if( extra && extra->ex_mt_state ) {
        bool touching_prev = mt_state_touching(extra->ex_mt_state);
        doubletap = mt_state_handle_event(extra->ex_mt_state, ev);
        bool touching_curr = mt_state_touching(extra->ex_mt_state);

        if( touching_prev != touching_curr )
            evin_touchstate_schedule_update();
    }

#ifdef ENABLE_DOUBLETAP_EMULATION
    if( doubletap && evin_iomon_sw_gestures_allowed() ) {
        mce_log(LL_DEVEL, "[doubletap] emulated from touch input");
        ev->type  = EV_MSC;
        ev->code  = MSC_GESTURE;
        ev->value = GESTURE_DOUBLETAP | GESTURE_SYNTHESIZED;
    }
#endif

    /* Power key up event from touch screen -> double tap gesture event */
    if( ev->type == EV_KEY && ev->code == KEY_POWER && ev->value == 0 ) {
        mce_log(LL_DEVEL, "[doubletap] as power key event; "
                "proximity=%s, lid=%s",
                proximity_state_repr(proximity_sensor_actual),
                proximity_state_repr(lid_sensor_filtered));

        /* Mimic N9 style gesture event for which we
         * already have logic in place. Possible filtering
         * due to proximity state etc happens at tklock.c
         */
        ev->type  = EV_MSC;
        ev->code  = MSC_GESTURE;
        ev->value = GESTURE_DOUBLETAP;
    }

    /* Ignore unwanted events */
    if( ev->type != EV_ABS &&
        ev->type != EV_KEY &&
        ev->type != EV_MSC )
        goto EXIT;

    /* Do not generate activity if ts input is grabbed */
    if( !grabbed )
        evin_iomon_generate_activity(ev, true, true);

    /* If the event eater is active, don't send anything */
    if( submode & MCE_SUBMODE_EVEATER )
        goto EXIT;

    if( ev->type == EV_MSC && ev->code == MSC_GESTURE ) {
        /* Gesture events count as actual non-synthetized
         * user activity. */
        evin_iomon_generate_activity(ev, false, true);

        /* But otherwise are handled in powerkey.c. */
        datapipe_exec_full(&keypress_event_pipe, &ev);
    }
    else if( (ev->type == EV_ABS && ev->code == ABS_PRESSURE) ||
             (ev->type == EV_KEY && ev->code == BTN_TOUCH ) ) {
        /* Only send pressure events */
        datapipe_exec_full(&touchscreen_event_pipe, &ev);
    }

EXIT:
    return flush;
}

/** I/O monitor callback for handling powerkey is doubletap events
 *
 * @param data       The new data
 * @param bytes_read The number of bytes read
 *
 * @return Always returns FALSE to return remaining chunks (if any)
 */
static gboolean
evin_iomon_evin_doubletap_cb(mce_io_mon_t *iomon, gpointer data, gsize bytes_read)
{
    struct input_event *ev = data;
    gboolean flush = FALSE;

    /* Don't process invalid reads */
    if( bytes_read != sizeof (*ev) )
        goto EXIT;

    if( ev->type == EV_MSC && ev->code == MSC_GESTURE ) {
        /* Feed gesture events to touchscreen handler as-is */
        evin_iomon_touchscreen_cb(iomon, ev, sizeof *ev);
    }
    else if( ev->type == EV_KEY && ev->code == KEY_POWER ) {
        /* Feed power key events to touchscreen handler for
         * possible double tap gesture event conversion */
        evin_iomon_touchscreen_cb(iomon, ev, sizeof *ev);
    }

EXIT:

    return flush;
}

/** I/O monitor callback for handling keypress events
 *
 * @param data       The new data
 * @param bytes_read The number of bytes read
 *
 * @return Always returns FALSE to return remaining chunks (if any)
 */
static gboolean
evin_iomon_keypress_cb(mce_io_mon_t *iomon, gpointer data, gsize bytes_read)
{
    (void)iomon;

    static bool key_fn_down  = false;
    static bool key_esc_down = false;

    struct input_event *ev;

    ev = data;

    /* Don't process invalid reads */
    if( bytes_read != sizeof (*ev) )
        goto EXIT;

    /* Map event before processing */
    evin_event_mapper_translate_event(ev);

    mce_log((ev->type == EV_SW && ev->code == SW_LID) ? LL_DEVEL : LL_DEBUG,
            "type: %s, code: %s, value: %d",
            evdev_get_event_type_name(ev->type),
            evdev_get_event_code_name(ev->type, ev->code),
            ev->value);

    evin_kp_grab_event_filter_cb(ev);

    /* Ignore non-keypress events */
    if ((ev->type != EV_KEY) && (ev->type != EV_SW)) {
        goto EXIT;
    }

    if (ev->type == EV_KEY) {
        if ((ev->code == KEY_SCREENLOCK) && (ev->value != 2)) {
            key_state_t key_state = ev->value ?
                KEY_STATE_PRESSED : KEY_STATE_RELEASED;
            datapipe_exec_full(&lockkey_state_pipe,
                               GINT_TO_POINTER(key_state));
        }
        else if( ev->code == KEY_FN || ev->code == KEY_LEFTMETA ) {
            key_fn_down = (ev->value != 0);
        }
        else if( ev->code == KEY_ESC ) {
            bool alarm_ringing = (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32 ||
                                  alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32);
            bool incoming_call = (call_state == CALL_STATE_RINGING);

            /* Trapping ESC key should be harmless when display
             * is off / when there is no active application that
             * might have input focus.
             *
             * While there is a slight chance of hiccups, also use
             * escape key for silencing alarms / calls without need
             * for pressing the meta key.
             */

            bool allow_trap = (key_fn_down || !interaction_expected ||
                               incoming_call || alarm_ringing);

            if( ev->value != 0 && allow_trap ) {
                /* Press / repeat event while trapping allowed */
                key_esc_down = true;
                ev->code = KEY_POWER;
            }
            else if( key_esc_down ) {
                /* Repeat / release event while already trapped */
                ev->code = KEY_POWER;
                key_esc_down = (ev->value != 0);
            }

            if( ev->code == KEY_POWER )
                mce_log(LL_DEBUG, "esc key -> power key %s",
                        key_esc_down ? "press" : "release");
        } else if( ev->code == KEY_WAKEUP ) {
            mce_log(LL_DEVEL, "[wakeup] as gesture event");

            ev->type  = EV_MSC;
            ev->code  = MSC_GESTURE;
            ev->value = GESTURE_DOUBLETAP;
            datapipe_exec_full(&keypress_event_pipe, &ev);
        }

        /* For now there's no reason to cache the keypress
         *
         * If the event eater is active, and this is the press,
         * don't send anything; never eat releases, otherwise
         * the release event for a [power] press might get lost
         * and the device shut down...  Not good(tm)
         *
         * Also, don't send repeat events, and don't send
         * keypress events for the focus and screenlock keys
         *
         * Additionally ignore all key events if proximity locked
         * during a call or alarm.
         */
        if ((ev->type == EV_KEY) && ((ev->code != KEY_CAMERA_FOCUS) &&
             (ev->code != KEY_SCREENLOCK) &&
             ((((submode & MCE_SUBMODE_EVEATER) == 0) &&
               (ev->value == 1)) || (ev->value == 0))) &&
            ((submode & MCE_SUBMODE_PROXIMITY_TKLOCK) == 0)) {
            datapipe_exec_full(&keypress_event_pipe, &ev);
        }
    }

    if (ev->type == EV_SW) {
        switch (ev->code) {
        case SW_CAMERA_LENS_COVER:
            if (ev->value != 2) {
                cover_state_t cover_state = ev->value ?
                    COVER_CLOSED : COVER_OPEN;
                datapipe_exec_full(&lens_cover_state_pipe,
                                   GINT_TO_POINTER(cover_state));
            }

            /* Don't generate activity on COVER_CLOSED */
            if (ev->value == 1)
                goto EXIT;

            break;

        case SW_KEYPAD_SLIDE:
            if (ev->value != 2) {
                cover_state_t cover_state = ev->value ?
                    COVER_CLOSED : COVER_OPEN;
                datapipe_exec_full(&keyboard_slide_state_pipe,
                                   GINT_TO_POINTER(cover_state));
                evin_iomon_keyboard_state_update();
            }

            /* Don't generate activity on COVER_CLOSED */
            if (ev->value == 1)
                goto EXIT;

            break;

        case SW_FRONT_PROXIMITY:
            if (ev->value != 2) {
                cover_state_t cover_state = ev->value ?
                    COVER_CLOSED : COVER_OPEN;
                datapipe_exec_full(&proximity_sensor_actual_pipe,
                                   GINT_TO_POINTER(cover_state));
            }

            break;

        case SW_HEADPHONE_INSERT:
        case SW_MICROPHONE_INSERT:
        case SW_LINEOUT_INSERT:
        case SW_VIDEOOUT_INSERT:
            if (ev->value != 2) {
                cover_state_t cover_state = ev->value ?
                    COVER_CLOSED : COVER_OPEN;
                datapipe_exec_full(&jack_sense_state_pipe,
                                   GINT_TO_POINTER(cover_state));
            }

            break;

        case SW_LID:
            /* hammerhead magnetic lid sensor; Feed in to the
             * same datapipe as N770 sliding cover uses */
            if( ev->value ) {
                datapipe_exec_full(&lid_sensor_actual_pipe,
                                   GINT_TO_POINTER(COVER_CLOSED));
            }
            else {
                datapipe_exec_full(&lid_sensor_actual_pipe,
                                   GINT_TO_POINTER(COVER_OPEN));
            }
            break;

            /* Other switches do not have custom actions */
        default:
            break;
        }
    }

    /* Power key press and release events count as actual non-synthetized
     *  user activity, but otherwise are handled in the powerkey module.
     */
    if(( ev->type == EV_KEY && ev->code == KEY_POWER ) || 
        (ev->type == EV_MSC && ev->code == MSC_GESTURE)) {
        if( ev->value != 2 )
            evin_iomon_generate_activity(ev, false, true);
        goto EXIT;
    }

    /* Generate activity - rate limited to once/second */
    evin_iomon_generate_activity(ev, true, false);

EXIT:
    return FALSE;
}

/** I/O monitor callback generatic activity from misc evdev events
 *
 * @param data       The new data
 * @param bytes_read The number of bytes read
 *
 * @return Always returns FALSE to return remaining chunks (if any)
 */
static gboolean
evin_iomon_activity_cb(mce_io_mon_t *iomon, gpointer data, gsize bytes_read)
{
    (void)iomon;

    struct input_event *ev = data;

    if( !ev || bytes_read != sizeof (*ev) )
        goto EXIT;

    /* Ignore synchronisation, force feedback, LED,
     * and force feedback status
     */
    switch (ev->type) {
    case EV_SYN:
    case EV_LED:
    case EV_SND:
    case EV_FF:
    case EV_FF_STATUS:
        goto EXIT;

    default:
        break;
    }

    mce_log(LL_DEBUG, "type: %s, code: %s, value: %d",
            evdev_get_event_type_name(ev->type),
            evdev_get_event_code_name(ev->type, ev->code),
            ev->value);

    /* Generate activity - rate limited to once/second */
    evin_iomon_generate_activity(ev, true, false);

EXIT:

    return FALSE;
}

/** Match and register I/O monitor
 *
 * @param path  Path to the device to add
 */
static void
evin_iomon_device_add(const gchar *path)
{
    int                   fd     = -1;
    mce_io_mon_notify_cb  notify = 0;
    evin_iomon_extra_t   *extra  = 0;
    mce_io_mon_t         *iomon  = 0;

    char  name[256];
    const gchar * const *black;

    /* If we cannot open the file, abort */
    if( (fd = open(path, O_NONBLOCK | O_RDONLY)) == -1 ) {
        mce_log(LL_WARN, "Failed to open `%s', skipping", path);
        goto EXIT;
    }

    /* Get name of the evdev node */
    if( ioctl(fd, EVIOCGNAME(sizeof name), name) < 0 ) {
        mce_log(LL_WARN, "ioctl(EVIOCGNAME) failed on `%s'", path);
        goto EXIT;
    }

    /* Check if the device is blacklisted by name in the config files */
    if( (black = mce_conf_get_blacklisted_event_drivers()) ) {
        for( size_t i = 0; black[i]; i++ ) {
            if( strcmp(name, black[i]) )
                continue;
            mce_log(LL_NOTICE, "%s: \"%s\", is blacklisted", path, name);
            goto EXIT;
        }
    }

    /* Probe device type */
    extra = evin_iomon_extra_create(fd, name);

    mce_log(LL_NOTICE, "%s: name='%s' type=%s", path, name,
            evin_evdevtype_repr(extra->ex_type));

    /* Choose notification callback function based on device type */
    switch( extra->ex_type ) {
    case EVDEV_TOUCH:
    case EVDEV_MOUSE:
        notify = evin_iomon_touchscreen_cb;
        break;

    case EVDEV_DBLTAP:
        notify = evin_iomon_evin_doubletap_cb;
        break;

    case EVDEV_INPUT:
    case EVDEV_KEYBOARD:
        notify = evin_iomon_keypress_cb;
        break;

    case EVDEV_VOLKEY:
        notify = evin_iomon_keypress_cb;
        break;

    case EVDEV_ACTIVITY:
        notify = evin_iomon_activity_cb;
        break;

    case EVDEV_ALS:
        /* Hook wakelockable ALS input source */
        mce_sensorfw_als_attach(fd), fd = -1;
        goto EXIT;

    case EVDEV_PS:
        /* Hook wakelockable PS input source */
        mce_sensorfw_ps_attach(fd), fd = -1;
        goto EXIT;

    case EVDEV_REJECT:
    case EVDEV_IGNORE:
    case EVDEV_UNKNOWN:
        goto EXIT;

    default:
        break;
    }

    if( !notify ) {
        mce_log(LL_ERR, "%s: no iomon notify callback assigned", path);
        goto EXIT;
    }

    /* Create io monitor for the device file descriptor */
    iomon = mce_io_mon_register_chunk(fd, path, MCE_IO_ERROR_POLICY_WARN,
                                      FALSE, notify,
                                      evin_iomon_device_delete_cb,
                                      sizeof (struct input_event));
    /* After mce_io_mon_register_chunk() returns the fd is either
     * attached to iomon or closed. */
    fd = -1;

    if( !iomon )
        goto EXIT;

    /* Attach device type information to the io monitor */
    mce_io_mon_set_user_data(iomon, extra, evin_iomon_extra_delete_cb),
        extra = 0;

    /* Add to list of evdev io monitors */
    evin_iomon_device_list = g_slist_prepend(evin_iomon_device_list, iomon);

EXIT:
    /* Release type data if it was not attached to io monitor */
    if( extra )
        evin_iomon_extra_delete_cb(extra);

    /* Close unmonitored file descriptors */
    if( fd != -1 && TEMP_FAILURE_RETRY(close(fd)) )
        mce_log(LL_ERR, "Failed to close `%s'; %m", path);
}

/** Update list of input devices
 *
 * Remove the I/O monitor for the specified device (if existing)
 * and (re)open it if available
 *
 * @param path  Path to the device to add/remove
 * @param add   TRUE to add device, FALSE to remove it
 */
static void
evin_iomon_device_update(const gchar *path, gboolean add)
{
    /* Try unregistering by device path; if io monitor exists
     * the delete callback is used to unlink it from device type
     * specific lists in this module. */
    mce_io_mon_unregister_at_path(path);

    /* add new io monitor if so requested */
    if( add )
        evin_iomon_device_add(path);

    evin_iomon_switch_states_update();
    evin_iomon_keyboard_state_update();
    evin_iomon_mouse_state_update();
}

/** Check whether the fd in question supports the switches
 * we want information about -- if so, update their state
 */
static void
evin_iomon_switch_states_update_iter_cb(gpointer io_monitor, gpointer user_data)
{
    (void)user_data;

    const mce_io_mon_t *iomon = io_monitor;

    const gchar *filename = mce_io_mon_get_path(iomon);
    int fd = mce_io_mon_get_fd(iomon);

    gulong *featurelist = NULL;
    gulong *statelist = NULL;
    gsize featurelistlen;
    gint state;
    int ecode;

    featurelistlen = (KEY_CNT / bitsize_of(*featurelist)) +
        ((KEY_CNT % bitsize_of(*featurelist)) ? 1 : 0);
    featurelist = g_malloc0(featurelistlen * sizeof (*featurelist));
    statelist = g_malloc0(featurelistlen * sizeof (*statelist));

    if (ioctl(fd, EVIOCGBIT(EV_SW, SW_MAX), featurelist) == -1) {
        mce_log(LL_ERR, "%s: EVIOCGBIT(EV_SW, SW_MAX) failed: %m",
                filename);
        goto EXIT;
    }

    if (ioctl(fd, EVIOCGSW(SW_MAX), statelist) == -1) {
        mce_log(LL_ERR, "%s: EVIOCGSW(SW_MAX) failed: %m",
                filename);
        goto EXIT;
    }

    /* Check initial camera lens cover state */
    ecode = evin_event_mapper_rlookup_switch(SW_CAMERA_LENS_COVER);
    if( test_bit(ecode, featurelist) ) {
        state = test_bit(ecode, statelist) ? COVER_CLOSED : COVER_OPEN;
        datapipe_exec_full(&lens_cover_state_pipe, GINT_TO_POINTER(state));
    }

    /* Check initial keypad slide state */
    ecode = evin_event_mapper_rlookup_switch(SW_KEYPAD_SLIDE);
    if( test_bit(ecode, featurelist) ) {
        state = test_bit(ecode, statelist) ? COVER_CLOSED : COVER_OPEN;
        datapipe_exec_full(&keyboard_slide_state_pipe, GINT_TO_POINTER(state));
    }

    /* Check initial front proximity state */
    ecode = evin_event_mapper_rlookup_switch(SW_FRONT_PROXIMITY);
    if( test_bit(ecode, featurelist) ) {
        state = test_bit(ecode, statelist) ? COVER_CLOSED : COVER_OPEN;
        datapipe_exec_full(&proximity_sensor_actual_pipe, GINT_TO_POINTER(state));
    }

    /* Check initial lid sensor state */
    ecode = evin_event_mapper_rlookup_switch(SW_LID);
    if( test_bit(ecode, featurelist) ) {
        state = test_bit(ecode, statelist) ? COVER_CLOSED : COVER_OPEN;
        mce_log(LL_DEVEL, "SW_LID initial state = %s",
                cover_state_repr(state));
        datapipe_exec_full(&lid_sensor_actual_pipe, GINT_TO_POINTER(state));
    }

    /* Need to consider more than one switch state when setting the
     * initial value of the jack_sense_state_pipe */

    bool have  = false;
    int  value = 0;

    ecode = evin_event_mapper_rlookup_switch(SW_HEADPHONE_INSERT);
    if( test_bit(ecode, featurelist) )
        have = true, value |= test_bit(ecode, statelist);

    ecode = evin_event_mapper_rlookup_switch(SW_MICROPHONE_INSERT);
    if( test_bit(ecode, featurelist) )
        have = true, value |= test_bit(ecode, statelist);

    ecode = evin_event_mapper_rlookup_switch(SW_LINEOUT_INSERT);
    if( test_bit(ecode, featurelist) )
        have = true, value |= test_bit(ecode, statelist);

    ecode = evin_event_mapper_rlookup_switch(SW_VIDEOOUT_INSERT);
    if( test_bit(ecode, featurelist) )
        have = true, value |= test_bit(ecode, statelist);

    if( have ) {
        state = value ? COVER_CLOSED : COVER_OPEN;
        datapipe_exec_full(&jack_sense_state_pipe, GINT_TO_POINTER(state));
    }

EXIT:
    g_free(statelist);
    g_free(featurelist);

    return;
}

/** Update switch states
 *
 * Go through monitored input devices and get current state of
 * switches mce has interest in.
 */
static void
evin_iomon_switch_states_update(void)
{
    evin_iomon_device_iterate(EVDEV_INPUT,
                              evin_iomon_switch_states_update_iter_cb,
                              0);

    evin_iomon_device_iterate(EVDEV_VOLKEY,
                              evin_iomon_switch_states_update_iter_cb,
                              0);
}

/** Iterator callback for evaluation availability of keyboard input devices
 *
 * Note: The iteration is peforming a logical OR operation, so the
 *       result variable must be modified only to set it true.
 *
 * @param io_monitor  io monitor as void pointer
 * @param user_data   pointer to bool available flag
 */
static void
evin_iomon_keyboard_state_update_iter_cb(gpointer io_monitor, gpointer user_data)
{
    const mce_io_mon_t *iomon = io_monitor;
    const mce_io_mon_t *slide = 0;
    evin_iomon_extra_t *extra = mce_io_mon_get_user_data(iomon);
    bool               *avail = (bool *)user_data;
    const char         *name  = extra->ex_name;

    /* Whether keypad slide state switch is SW_KEYPAD_SLIDE or something
     * else depends on configuration. */

    int ecode = evin_event_mapper_rlookup_switch(SW_KEYPAD_SLIDE);

    /** Check if another device node is supposed to provide slide status */
    if( (slide = evin_iomon_lookup_device(extra->ex_sw_keypad_slide)) ) {
        iomon = slide;
        extra = mce_io_mon_get_user_data(iomon);
        mce_log(LL_DEBUG, "'%s' gets slide state from '%s'",
                name, extra->ex_name);
    }

    /* Keyboard devices that do not  have keypad slide switch are
     * considered to be always available. */

    if( !evin_evdevinfo_has_code(extra->ex_info, EV_SW, ecode) ) {
        *avail = true;
        mce_log(LL_DEBUG, "'%s' is non-sliding keyboard", name);
        goto EXIT;
    }

    /* Keyboard devices that have keypad slide are considered available
     * only when the slider is in open state */

    int fd = mce_io_mon_get_fd(iomon);

    unsigned long bits[EVIN_EVDEVBITS_LEN(SW_MAX)];
    memset(bits, 0, sizeof bits);

    if( ioctl(fd, EVIOCGSW(SW_MAX), bits) == -1 ) {
        mce_log(LL_WARN, "%s: EVIOCGSW(SW_MAX) failed: %m",
                mce_io_mon_get_path(iomon));
        goto EXIT;
    }

    bool is_open = !test_bit(ecode, bits);

    if( is_open )
        *avail = true;

    mce_log(LL_DEBUG, "'%s' is sliding keyboard in %s position",
            name, is_open ? "open" : "closed");

EXIT:
    return;
}

/** Check if at least one keyboard device in usable state exists
 *
 * Iterate over monitored input devices to find normal keyboards
 * or slide in keyboards in open position.
 *
 * Update keyboard availablity state based on the scanning result.
 *
 * This function should be called when new devices are detected,
 * old ones disappear or SW_KEYPAD_SLIDE events are seen.
 */
static void
evin_iomon_keyboard_state_update(void)
{
    bool available = false;

    evin_iomon_device_iterate(EVDEV_KEYBOARD,
                              evin_iomon_keyboard_state_update_iter_cb,
                              &available);

    mce_log(LL_DEBUG, "available = %s", available ? "true" : "false");

    cover_state_t state = available ? COVER_OPEN : COVER_CLOSED;

    datapipe_exec_full(&keyboard_available_state_pipe,
                       GINT_TO_POINTER(state));
}

/** Iterator callback for evaluation availability of mouse input devices
 *
 * Note: The iteration is peforming a logical OR operation, so the
 *       result variable must be modified only to set it true.
 *
 * @param io_monitor  io monitor as void pointer
 * @param user_data   pointer to bool available flag
 */
static void
evin_iomon_mouse_state_update_iter_cb(gpointer io_monitor, gpointer user_data)
{
    (void)io_monitor;
    bool *available = user_data;

    /* As long as we are iterating devices of EVDEV_MOUSE
     * type, it is enough that we got here */
    *available = true;
    return;
}

/** Check if at least one mouse device in usable state exists
 *
 * Iterate over monitored input devices to find mouses.
 *
 * Update mouse availablity state based on the scanning result.
 *
 * This function should be called when new devices are detected,
 * or old ones disappear.
 */
static void
evin_iomon_mouse_state_update(void)
{
    bool available = false;

    evin_iomon_device_iterate(EVDEV_MOUSE,
                              evin_iomon_mouse_state_update_iter_cb,
                              &available);

    mce_log(LL_DEBUG, "available = %s", available ? "true" : "false");

    cover_state_t state = available ? COVER_OPEN : COVER_CLOSED;

    datapipe_exec_full(&mouse_available_state_pipe,
                       GINT_TO_POINTER(state));
}

/** Scan /dev/input for input event devices
 *
 * @return TRUE on success, FALSE on failure
 */
static bool
evin_iomon_init(void)
{
    static const char pfix[] = EVENT_FILE_PREFIX;

    bool  res = false;
    DIR  *dir = NULL;

    if( !(dir = opendir(DEV_INPUT_PATH)) ) {
        mce_log(LL_ERR, "opendir() failed; %m");
        goto EXIT;
    }

    struct dirent *de;

    while( (de = readdir(dir)) != 0 ) {
        if( strncmp(de->d_name, pfix, sizeof pfix - 1) ) {
            mce_log(LL_DEBUG, "`%s/%s' skipped", DEV_INPUT_PATH, de->d_name);
            continue;
        }

        gchar *path = g_strdup_printf("%s/%s", DEV_INPUT_PATH, de->d_name);
        evin_iomon_device_add(path);
        g_free(path);
    }

    res = true;

EXIT:
    if( dir && closedir(dir) == -1 )
        mce_log(LL_ERR, "closedir() failed; %m");

    return res;
}

/** Unregister io monitors for all input devices
 */
static void
evin_iomon_quit(void)
{
    evin_iomon_device_rem_all();
}

/* ========================================================================= *
 * EVDEV_DIRECTORY_MONITORING
 * ========================================================================= */

/** GFile pointer for /dev/input */
static GFile        *evin_devdir_directory          = NULL;

/** GFileMonitor for evin_devdir_directory */
static GFileMonitor *evin_devdir_monitor            = NULL;

/** Handler ID for the evin_devdir_monitor "changed" signal */
static gulong        evin_devdir_monitor_changed_id = 0;

/** Callback for /dev/input directory changes
 *
 * @param monitor     Unused
 * @param file        The file that changed
 * @param other_file  Unused
 * @param event_type  The event that occured
 * @param user_data   Unused
 */
static void
evin_devdir_monitor_changed_cb(GFileMonitor *monitor,
                               GFile *file, GFile *other_file,
                               GFileMonitorEvent event_type, gpointer user_data)
{
    (void)monitor;
    (void)other_file;
    (void)user_data;

    char *filename = g_file_get_basename(file);
    char *filepath = g_file_get_path(file);

    if( !filename || !filepath )
        goto EXIT;

    if( strncmp(filename, EVENT_FILE_PREFIX, strlen(EVENT_FILE_PREFIX)) )
        goto EXIT;

    switch (event_type) {
    case G_FILE_MONITOR_EVENT_CREATED:
        evin_iomon_device_update(filepath, TRUE);
        break;

    case G_FILE_MONITOR_EVENT_DELETED:
        evin_iomon_device_update(filepath, FALSE);
        break;

    default:
        break;
    }

EXIT:
    g_free(filepath);
    g_free(filename);

    return;
}

/** Start tracking changes in /dev/input directory
 *
 * @return TRUE if monitoring was started, FALSE otherwise
 */
static bool
evin_devdir_monitor_init(void)
{
    bool    success = false;
    GError *error   = NULL;

    /* Retrieve a GFile pointer to the directory to monitor */
    if( !(evin_devdir_directory = g_file_new_for_path(DEV_INPUT_PATH)) )
        goto EXIT;

    /* Monitor the directory */
    evin_devdir_monitor = g_file_monitor_directory(evin_devdir_directory,
                                                       G_FILE_MONITOR_NONE,
                                                       NULL, &error);
    if( !evin_devdir_monitor ) {
        mce_log(LL_ERR,
                "Failed to add monitor for directory `%s'; %s",
                DEV_INPUT_PATH, error->message);
        goto EXIT;
    }

    /* Connect "changed" signal for the directory monitor */
    evin_devdir_monitor_changed_id =
        g_signal_connect(G_OBJECT(evin_devdir_monitor), "changed",
                         G_CALLBACK(evin_devdir_monitor_changed_cb), NULL);

    if( !evin_devdir_monitor_changed_id ) {
        mce_log(LL_ERR, "Failed to connect to 'changed' signal"
                " for directory `%s'", DEV_INPUT_PATH);
        goto EXIT;
    }

    success = true;

EXIT:

    /* All or nothing */
    if( !success )
        evin_devdir_monitor_quit();

    g_clear_error(&error);

    return success;
}

/** Stop tracking changes in /dev/input directory
 */
static void
evin_devdir_monitor_quit(void)
{
    /* Remove directory monitor */
    if( evin_devdir_monitor ) {
        if( evin_devdir_monitor_changed_id ) {
            g_signal_handler_disconnect(G_OBJECT(evin_devdir_monitor),
                                        evin_devdir_monitor_changed_id);
            evin_devdir_monitor_changed_id = 0;
        }
        g_object_unref(evin_devdir_monitor),
            evin_devdir_monitor = 0;
    }

    /* Release directory file object */
    if( evin_devdir_directory ) {
        g_object_unref(evin_devdir_directory),
            evin_devdir_directory = 0;
    }
}

/* ========================================================================= *
 * TOUCHSTATE_MONITORING
 * ========================================================================= */

/** Iterator callback for finding touch devices in finger-on-screen state
 *
 * @param data       iomon object
 * @param user_data  pointer to bool flag to set if tracked device is touched
 */
static void
evin_touchstate_iomon_iter_cb(gpointer data, gpointer user_data)
{
    mce_io_mon_t *iomon    = data;
    bool         *touching = user_data;

    evin_iomon_extra_t *extra = mce_io_mon_get_user_data(iomon);

    if( extra && mt_state_touching(extra->ex_mt_state) )
        *touching = true;
}

/** Idle ID for delayed update of finger-on-screen state */
static guint  evin_touchstate_update_id = 0;

/** Idle cb function for delayed update of finger-on-screen state
 *
 * @param aptr User data pointer (unused)
 *
 * @return FALSE to keep idle callback from repeating
 */
static gboolean
evin_touchstate_update_cb(gpointer aptr)
{
    (void)aptr;

    if( !evin_touchstate_update_id )
        goto EXIT;

    evin_touchstate_update_id = 0;

    bool touching = false;

    evin_iomon_device_iterate(EVDEV_TOUCH,
                              evin_touchstate_iomon_iter_cb,
                              &touching);

    evin_iomon_device_iterate(EVDEV_MOUSE,
                              evin_touchstate_iomon_iter_cb,
                              &touching);

    if( touching == touch_detected )
        goto EXIT;

    mce_log(LL_DEBUG, "touch_detected=%s", touching ? "true" : "false");
    datapipe_exec_full(&touch_detected_pipe,
                       GINT_TO_POINTER(touching));

EXIT:
    return FALSE;
}

/** Cancel delayed update of finger-on-screen state
 */
static void
evin_touchstate_cancel_update(void)
{
    if( evin_touchstate_update_id ) {
        g_source_remove(evin_touchstate_update_id),
            evin_touchstate_update_id = 0;
    }
}

/** Schedule delayed update of finger-on-screen state
 */
static void
evin_touchstate_schedule_update(void)
{
    if( !evin_touchstate_update_id ) {
        evin_touchstate_update_id = g_idle_add(evin_touchstate_update_cb, 0);
    }
}

/* ========================================================================= *
 * INPUT_GRAB  --  GENERIC EVDEV INPUT GRAB STATE MACHINE
 * ========================================================================= */

/** Convert state enum to human readable string for purposes
 */
static const char *
evin_state_repr(evin_state_t state)
{
  const char *str = "EVIN_STATE_INVALID";

  switch( state ) {
  case EVIN_STATE_UNKNOWN:  str = "EVIN_STATE_UNKNOWN";  break;
  case EVIN_STATE_ENABLED:  str = "EVIN_STATE_ENABLED";  break;
  case EVIN_STATE_DISABLED: str = "EVIN_STATE_DISABLED"; break;
  default: break;
  }

  return str;
}

/** Convert state enum to string for use on dbus
 */
static const char *
evin_state_to_dbus(evin_state_t state)
{
    if( state == EVIN_STATE_DISABLED )
        return MCE_INPUT_POLICY_DISABLED;

    return MCE_INPUT_POLICY_ENABLED;
}

/** Reset input grab state machine
 *
 * Releases any dynamic resources held by the state machine
 */
static void
evin_input_grab_reset(evin_input_grab_t *self)
{
    self->ig_touching = false;
    self->ig_touched  = false;

    if( self->ig_release_id )
        g_source_remove(self->ig_release_id),
        self->ig_release_id = 0;
}

/** Delayed release timeout callback
 *
 * Grab/ungrab happens from this function when touch/press ends
 */
static gboolean
evin_input_grab_release_cb(gpointer aptr)
{
    evin_input_grab_t *self = aptr;
    gboolean repeat = FALSE;

    if( !self->ig_release_id )
        goto EXIT;

    if( self->ig_release_verify_cb && !self->ig_release_verify_cb(self) ) {
        mce_log(LL_DEBUG, "touching(%s) = holding", self->ig_name);
        repeat = TRUE;
        goto EXIT;
    }

    // timer no longer active
    self->ig_release_id = 0;

    // touch release delay has ended
    self->ig_touched = false;

    mce_log(LL_DEBUG, "touching(%s) = released", self->ig_name);

    // evaluate next state
    evin_input_grab_rethink(self);

EXIT:
    return repeat;
}

/** Start delayed release timer if not already running
 */
static void
evin_input_grab_start_release_timer(evin_input_grab_t *self)
{
    if( !self->ig_release_id )
        self->ig_release_id = g_timeout_add(self->ig_release_ms,
                                            evin_input_grab_release_cb,
                                            self);
}

/** Cancel delayed release timer
 */
static void
evin_input_grab_cancel_release_timer(evin_input_grab_t *self)
{
    if( self->ig_release_id )
        g_source_remove(self->ig_release_id),
        self->ig_release_id = 0;
}

/** Re-evaluate input grab state
 */
static void
evin_input_grab_rethink(evin_input_grab_t *self)
{
    // no changes while active touch
    if( self->ig_touching ) {
        evin_input_grab_cancel_release_timer(self);
        goto EXIT;
    }

    // delay after touch release
    if( self->ig_touched ) {
        evin_input_grab_start_release_timer(self);
        goto EXIT;
    }

    // do the transition
    self->ig_have_grab = self->ig_want_grab;

EXIT:
    ;
    // evaluate actual grab
    bool real = self->ig_have_grab && self->ig_allow_grab;
    if( self->ig_real_grab != real ) {
        self->ig_real_grab = real;

        if( self->ig_grab_changed_cb )
            self->ig_grab_changed_cb(self, self->ig_real_grab);
    }

    // evaluate policy change
    evin_state_t state = EVIN_STATE_ENABLED;
    if( self->ig_want_grab || self->ig_have_grab )
        state = EVIN_STATE_DISABLED;

    if( self->ig_state != state ) {
        mce_log(LL_DEBUG, "state(%s): %s -> %s", self->ig_name,
                evin_state_repr(self->ig_state),
                evin_state_repr(state));
        self->ig_state = state;
        if( self->ig_state_changed_cb )
            self->ig_state_changed_cb(self);
    }
    return;
}

/** Feed touching/pressed state to input grab state machine
 */
static void
evin_input_grab_set_touching(evin_input_grab_t *self, bool touching)
{
    if( self->ig_touching == touching )
        goto EXIT;

    mce_log(LL_DEBUG, "touching(%s) = %s", self->ig_name, touching ? "yes" : "no");

    if( (self->ig_touching = touching) )
        self->ig_touched = true;

    evin_input_grab_rethink(self);

EXIT:
    return;
}

/** Feed desire to grab to input grab state machine
 */
static void
evin_input_grab_request_grab(evin_input_grab_t *self, bool want_grab)
{
    if( self->ig_want_grab == want_grab )
        goto EXIT;

    self->ig_want_grab = want_grab;

    evin_input_grab_rethink(self);

EXIT:
    return;
}

/** Feed allow/deny grab control to input grab state machine
 */
static void
evin_input_grab_allow_grab(evin_input_grab_t *self, bool allow_grab)
{
    if( self->ig_allow_grab == allow_grab )
        goto EXIT;

    self->ig_allow_grab = allow_grab;

    evin_input_grab_rethink(self);

EXIT:
    return;
}

/** Callback for changing iomonitor input grab state
 */
static void
evin_input_grab_iomon_cb(gpointer data, gpointer user_data)
{
    gpointer iomon = data;
    int grab = GPOINTER_TO_INT(user_data);
    int fd   = mce_io_mon_get_fd(iomon);

    if( fd == -1 )
        goto EXIT;

    const char *path = mce_io_mon_get_path(iomon) ?: "unknown";

    if( ioctl(fd, EVIOCGRAB, grab) == -1 ) {
        mce_log(LL_ERR, "EVIOCGRAB(%s, %d): %m", path, grab);
        goto EXIT;
    }
    mce_log(LL_DEBUG, "%sGRABBED fd=%d path=%s",
            grab ? "" : "UN", fd, path);

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * TS_GRAB
 * ------------------------------------------------------------------------- */

/** State data for touch input grab state machine */
static evin_input_grab_t evin_ts_grab_state =
{
    .ig_name       = "ts",
    .ig_state      = EVIN_STATE_UNKNOWN,

    .ig_touching   = false,
    .ig_touched    = false,

    .ig_want_grab  = false,
    .ig_have_grab  = false,
    .ig_real_grab  = false,
    .ig_allow_grab = false,

    .ig_release_id = 0,
    .ig_release_ms = MCE_DEFAULT_TOUCH_UNBLOCK_DELAY,

    .ig_grab_changed_cb   = evin_ts_grab_changed,
    .ig_release_verify_cb = evin_ts_grab_poll_palm_detect,
    .ig_state_changed_cb  = evin_ts_policy_changed,
};

/* Touch unblock delay from settings [ms] */
static gint  evin_ts_grab_release_delay = MCE_DEFAULT_TOUCH_UNBLOCK_DELAY;
static guint evin_ts_grab_release_delay_setting_id = 0;

/** Low level helper for input grab debug led pattern activate/deactivate
 */
static void
evin_ts_grab_set_led_raw(bool enabled)
{
    datapipe_exec_full(enabled ?
                       &led_pattern_activate_pipe :
                       &led_pattern_deactivate_pipe,
                       MCE_LED_PATTERN_TOUCH_INPUT_BLOCKED);
}

/** Handle delayed input grab led pattern activation
 */
static gboolean
evin_ts_grab_set_led_cb(gpointer aptr)
{
    guint *id = aptr;

    if( !*id )
        goto EXIT;

    *id = 0;
    evin_ts_grab_set_led_raw(true);
EXIT:
    return FALSE;
}

/** Handle grab led pattern activation/deactivation
 *
 * Deactivation happens immediately.
 * Activation after brief delay
 */
static void
evin_ts_grab_set_led(bool enabled)
{
    static guint id = 0;

    static bool prev = false;

    if( prev == enabled )
        goto EXIT;

    if( id )
        g_source_remove(id), id = 0;

    if( enabled )
        id = g_timeout_add(200, evin_ts_grab_set_led_cb, &id);
    else
        evin_ts_grab_set_led_raw(false);

    prev = enabled;
EXIT:
    return;
}

/** Evaluate need for grab active led notification
 *
 * This should be called when display state or
 * touch screen grab state changes.
 */
static void
evin_ts_grab_rethink_led(void)
{
    bool enable = false;

    switch( display_state_curr )
    {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        if( evin_ts_grab_state.ig_state == EVIN_STATE_DISABLED )
            enable = true;
        break;
    default:
        break;
    }

    evin_ts_grab_set_led(enable);
}

/** Grab/ungrab all monitored touch input devices
 */
static void
evin_ts_grab_set_active(gboolean grab)
{
    static gboolean old_grab = FALSE;

    if( old_grab == grab )
        goto EXIT;

    old_grab = grab;

    evin_iomon_device_iterate(EVDEV_TOUCH,
                              evin_input_grab_iomon_cb,
                              GINT_TO_POINTER(grab));

    evin_iomon_device_iterate(EVDEV_MOUSE,
                              evin_input_grab_iomon_cb,
                              GINT_TO_POINTER(grab));

    // STATE MACHINE -> OUTPUT DATAPIPE
    datapipe_exec_full(&touch_grab_active_pipe,
                       GINT_TO_POINTER(grab));

EXIT:
    return;
}

/** Query palm detection state
 *
 * Used to keep touch input in unreleased state even if finger touch
 * events are not coming in.
 */
static bool
evin_ts_grab_poll_palm_detect(evin_input_grab_t *ctrl)
{
    (void)ctrl;

    static const char path[] = "/sys/devices/i2c-3/3-0020/palm_status";

    bool released = true;

    int fd = -1;
    char buf[32];
    if( (fd = open(path, O_RDONLY)) == -1 ) {
        if( errno != ENOENT )
            mce_log(LL_ERR, "can't open %s: %m", path);
        goto EXIT;
    }

    int rc = read(fd, buf, sizeof buf - 1);
    if( rc < 0 ) {
        mce_log(LL_ERR, "can't read %s: %m", path);
        goto EXIT;
    }

    buf[rc] = 0;
    released = (strtol(buf, 0, 0) == 0);

EXIT:
    if( fd != -1 && close(fd) == -1 )
        mce_log(LL_WARN, "can't close %s: %m", path);

    return released;
}

/** Handle grab state notifications from generic input grab state machine
 */
static void
evin_ts_grab_changed(evin_input_grab_t *ctrl, bool grab)
{
    (void)ctrl;

    evin_ts_grab_set_active(grab);
}

static void
evin_ts_policy_changed(evin_input_grab_t *ctrl)
{
    (void)ctrl;
    evin_ts_grab_rethink_led();
    evin_dbus_send_touch_input_policy(0);
}

enum
{
    TS_RELEASE_DELAY_BLANK   = 100,
    TS_RELEASE_DELAY_UNBLANK = 600,
};

/** Gconf notification callback for touch unblock delay
 *
 * @param client (not used)
 * @param id     (not used)
 * @param entry  GConf entry that changed
 * @param data   (not used)
 */
static void
evin_ts_grab_setting_cb(GConfClient *const client,
                        const guint id,
                        GConfEntry *const entry,
                        gpointer const data)
{
    (void)client; (void)id; (void)data;

    gint delay = evin_ts_grab_release_delay;
    const GConfValue *value = 0;

    if( !entry )
        goto EXIT;

    if( !(value = gconf_entry_get_value(entry)) )
        goto EXIT;

    if( value->type == GCONF_VALUE_INT )
        delay = gconf_value_get_int(value);

    if( evin_ts_grab_release_delay == delay )
        goto EXIT;

    mce_log(LL_NOTICE, "touch unblock delay changed: %d -> %d",
            evin_ts_grab_release_delay, delay);

    evin_ts_grab_release_delay = delay;

    // NB: currently active timer is not reprogrammed, change
    //     will take effect on the next unblank
EXIT:
    return;
}

/** Feed desired touch grab state from datapipe to state machine
 *
 * @param data The grab wanted boolean as a pointer
 */
static void
evin_datapipe_touch_grab_wanted_cb(gconstpointer data)
{
    bool prev = touch_grab_wanted;
    touch_grab_wanted = GPOINTER_TO_INT(data);

    if( prev != touch_grab_wanted )
        mce_log(LL_DEBUG, "touch_grab_wanted: %d -> %d",
                prev, touch_grab_wanted);

    // INPUT DATAPIPE -> STATE MACHINE
    evin_input_grab_request_grab(&evin_ts_grab_state, touch_grab_wanted);
}

/** Feed detected finger-on-screen state from datapipe to state machine
 *
 * @param data The touch detected boolean as a pointer
 */
static void
evin_datapipe_touch_detected_cb(gconstpointer data)
{
    bool prev = touch_detected;
    touch_detected = GPOINTER_TO_INT(data);

    if( prev != touch_detected )
        mce_log(LL_DEBUG, "touch_detected = %s",
                touch_detected ? "true" : "false");

    evin_input_grab_set_touching(&evin_ts_grab_state, touch_detected);
}

/** Take display state changes in account for touch grab state
 *
 * @param data Display state as void pointer
 */
static void
evin_datapipe_display_state_curr_cb(gconstpointer data)
{
    display_state_t prev = display_state_curr;
    display_state_curr = GPOINTER_TO_INT(data);

    if( display_state_curr == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_curr=%s", display_state_repr(display_state_curr));

    switch( display_state_curr ) {
    case MCE_DISPLAY_POWER_DOWN:
    case MCE_DISPLAY_OFF:
    case MCE_DISPLAY_LPM_ON:
    case MCE_DISPLAY_LPM_OFF:
        /* Assume UI can deal with losing touch input mid gesture
         * and grab touch input already when we just start to power
         * down the display. */
        evin_input_grab_reset(&evin_ts_grab_state);
        evin_input_grab_rethink(&evin_ts_grab_state);
        break;

    case MCE_DISPLAY_POWER_UP:
        /* Fake a touch to keep statemachine from releasing
         * the input grab before we have a change to get
         * actual input from the touch panel. */
        evin_ts_grab_state.ig_release_ms = TS_RELEASE_DELAY_UNBLANK;
        if( !touch_detected ) {
            evin_input_grab_set_touching(&evin_ts_grab_state, true);
            evin_input_grab_set_touching(&evin_ts_grab_state, false);
        }
        /* Fall through */

    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        evin_ts_grab_state.ig_release_ms = evin_ts_grab_release_delay;
        if( prev != MCE_DISPLAY_ON && prev != MCE_DISPLAY_DIM ) {
            /* End the faked touch once the display is
             * fully on. If there is a finger on the
             * screen we will get more input events
             * before the delay from artificial touch
             * release ends. */
            evin_input_grab_set_touching(&evin_ts_grab_state, touch_detected);
        }
        break;

    default:
    case MCE_DISPLAY_UNDEF:
        break;
    }

    evin_ts_grab_rethink_led();

EXIT:
    return;
}

/** Initialize touch screen grabbing state machine
 */
static void
evin_ts_grab_init(void)
{
    /* Get touch unblock delay */
    mce_setting_notifier_add(MCE_SETTING_EVENT_INPUT_PATH,
                             MCE_SETTING_TOUCH_UNBLOCK_DELAY,
                             evin_ts_grab_setting_cb,
                             &evin_ts_grab_release_delay_setting_id);

    mce_setting_get_int(MCE_SETTING_TOUCH_UNBLOCK_DELAY,
                        &evin_ts_grab_release_delay);

    mce_log(LL_INFO, "touch unblock delay config: %d",
            evin_ts_grab_release_delay);

    evin_ts_grab_state.ig_release_ms = evin_ts_grab_release_delay;
}

/** De-initialize touch screen grabbing state machine
 */
static void
evin_ts_grab_quit(void)
{
    mce_setting_notifier_remove(evin_ts_grab_release_delay_setting_id),
        evin_ts_grab_release_delay_setting_id = 0;

    evin_input_grab_reset(&evin_ts_grab_state);
}

/* ------------------------------------------------------------------------- *
 * KP_GRAB
 * ------------------------------------------------------------------------- */

/** Grab/ungrab all monitored volumekey input devices
 */
static void
evin_kp_grab_set_active(gboolean grab)
{
    static gboolean old_grab = FALSE;

    if( old_grab == grab )
        goto EXIT;

    old_grab = grab;
    evin_iomon_device_iterate(EVDEV_VOLKEY,
                              evin_input_grab_iomon_cb,
                              GINT_TO_POINTER(grab));

    // STATE MACHINE -> OUTPUT DATAPIPE
    datapipe_exec_full(&keypad_grab_active_pipe,
                       GINT_TO_POINTER(grab));

EXIT:
    return;
}

/** Handle grab state notifications from generic input grab state machine
 */
static void
evin_kp_grab_changed(evin_input_grab_t *ctrl, bool grab)
{
    (void)ctrl;

    evin_kp_grab_set_active(grab);
}

static void
evin_kp_policy_changed(evin_input_grab_t *ctrl)
{
    (void)ctrl;
    evin_dbus_send_keypad_input_policy(0);
}

/** State data for volumekey input grab state machine */
static evin_input_grab_t evin_kp_grab_state =
{
    .ig_name       = "kp",
    .ig_state      = EVIN_STATE_UNKNOWN,

    .ig_touching   = false,
    .ig_touched    = false,

    .ig_want_grab  = false,
    .ig_have_grab  = false,
    .ig_real_grab  = false,
    .ig_allow_grab = false,

    .ig_release_id = 0,
    .ig_release_ms = 200,

    .ig_grab_changed_cb  = evin_kp_grab_changed,
    .ig_state_changed_cb = evin_kp_policy_changed,
};

/** Event filter for determining volume key pressed state
 */
static void
evin_kp_grab_event_filter_cb(struct input_event *ev)
{
    static bool vol_up = false;
    static bool vol_dn = false;

    switch( ev->type ) {
    case EV_KEY:
        switch( ev->code ) {
        case KEY_VOLUMEUP:
            vol_up = (ev->value != 0);
            break;

        case KEY_VOLUMEDOWN:
            vol_dn = (ev->value != 0);
            break;

        default:
            break;
        }
        break;

    default:
        break;
    }

    evin_input_grab_set_touching(&evin_kp_grab_state, vol_up || vol_dn);
}

/** Feed desired volumekey grab state from datapipe to state machine
 *
 * @param data The grab wanted boolean as a pointer
 */
static void
evin_datapipe_keypad_grab_wanted_cb(gconstpointer data)
{
    bool prev = keypad_grab_wanted;
    keypad_grab_wanted = GPOINTER_TO_INT(data);

    if( prev != keypad_grab_wanted )
        mce_log(LL_DEBUG, "keypad_grab_wanted: %d -> %d",
                prev, keypad_grab_wanted);

    // INPUT DATAPIPE -> STATE MACHINE
    evin_input_grab_request_grab(&evin_kp_grab_state, keypad_grab_wanted);
}

/* ========================================================================= *
 * DYNAMIC_SETTINGS
 * ========================================================================= */

/** Flag: Input device types that can be grabbed */
static gint  evin_setting_input_grab_allowed = MCE_DEFAULT_INPUT_GRAB_ALLOWED;
static guint evin_setting_input_grab_allowed_setting_id = 0;

/** Handle changes to the list of grabbable input devices
 */
static void evin_setting_input_grab_rethink(void)
{
    bool ts = (evin_setting_input_grab_allowed & MCE_INPUT_GRAB_ALLOW_TS) != 0;
    bool kp = (evin_setting_input_grab_allowed & MCE_INPUT_GRAB_ALLOW_KP) != 0;

    evin_input_grab_allow_grab(&evin_ts_grab_state, ts);
    evin_input_grab_allow_grab(&evin_kp_grab_state, kp);
}

/** GConf callback for event input related settings
 *
 * @param gcc    Unused
 * @param id     Connection ID from gconf_client_notify_add()
 * @param entry  The modified GConf entry
 * @param data   Unused
 */
static void evin_setting_cb(GConfClient *const gcc, const guint id,
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

    if( id == evin_setting_input_grab_allowed_setting_id ) {
        gint old = evin_setting_input_grab_allowed;

        evin_setting_input_grab_allowed = gconf_value_get_int(gcv);

        mce_log(LL_NOTICE, "evin_setting_input_grab_allowed: %d -> %d",
                old, evin_setting_input_grab_allowed);
        evin_setting_input_grab_rethink();
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }

EXIT:
    return;
}

/** Get intial setting values and start tracking changes
 */
static void evin_setting_init(void)
{
    /* Bitmask of input devices that can be grabbed */
    mce_setting_track_int(MCE_SETTING_INPUT_GRAB_ALLOWED,
                          &evin_setting_input_grab_allowed,
                          MCE_DEFAULT_INPUT_GRAB_ALLOWED,
                          evin_setting_cb,
                          &evin_setting_input_grab_allowed_setting_id);

    evin_setting_input_grab_rethink();
}

/** Stop tracking setting changes
 */
static void evin_setting_quit(void)
{
    mce_setting_notifier_remove(evin_setting_input_grab_allowed_setting_id),
        evin_setting_input_grab_allowed_setting_id = 0;
}

/* ========================================================================= *
 * DBUS_HOOKS
 * ========================================================================= */

/** Send the keypad input policy
 *
 * @param req A method call message to be replied, or
 *            NULL to broadcast a keypad input policy signal
 */
static void
evin_dbus_send_keypad_input_policy(DBusMessage *const req)
{
    DBusMessage *rsp = 0;

    if( req )
        rsp = dbus_new_method_reply(req);
    else
        rsp = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_VOLKEY_INPUT_POLICY_SIG);
    if( !rsp )
        goto EXIT;

    const char *arg = evin_state_to_dbus(evin_kp_grab_state.ig_state);

    mce_log(LL_DEBUG, "send keypad input policy %s: %s",
            req ? "reply" : "signal", arg);

    if( !dbus_message_append_args(rsp,
                                  DBUS_TYPE_STRING, &arg,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    dbus_send_message(rsp), rsp = 0;

EXIT:
    if( rsp ) dbus_message_unref(rsp);
}

/** D-Bus callback for the get keypad input policy method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
evin_dbus_keypad_input_policy_get_req_cb(DBusMessage *const msg)
{
    mce_log(LL_DEVEL, "Received keypad input policy get request from %s",
            mce_dbus_get_message_sender_ident(msg));

    evin_dbus_send_keypad_input_policy(msg);

    return TRUE;
}

/** Send the touch input policy
 *
 * @param req A method call message to be replied, or
 *            NULL to broadcast a touch input policy signal
 */
static void
evin_dbus_send_touch_input_policy(DBusMessage *const req)
{
    DBusMessage *rsp = 0;

    if( req )
        rsp = dbus_new_method_reply(req);
    else
        rsp = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                              MCE_TOUCH_INPUT_POLICY_SIG);
    if( !rsp )
        goto EXIT;

    const char *arg = evin_state_to_dbus(evin_ts_grab_state.ig_state);

    mce_log(LL_DEBUG, "send touch input policy %s: %s",
            req ? "reply" : "signal", arg);

    if( !dbus_message_append_args(rsp,
                                  DBUS_TYPE_STRING, &arg,
                                  DBUS_TYPE_INVALID) )
        goto EXIT;

    dbus_send_message(rsp), rsp = 0;

EXIT:
    if( rsp ) dbus_message_unref(rsp);
}

/** D-Bus callback for the get touch input policy method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE
 */
static gboolean
evin_dbus_touch_input_policy_get_req_cb(DBusMessage *const msg)
{
    mce_log(LL_DEVEL, "Received keypad input policy get request from %s",
            mce_dbus_get_message_sender_ident(msg));

    evin_dbus_send_touch_input_policy(msg);

    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t evin_dbus_handlers[] =
{
    /* signals - outbound (for Introspect purposes only) */
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_VOLKEY_INPUT_POLICY_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"input_policy\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_SIGNAL_IF,
        .name      = MCE_TOUCH_INPUT_POLICY_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .args      =
            "    <arg name=\"input_policy\" type=\"s\"/>\n"
    },
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_VOLKEY_INPUT_POLICY_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = evin_dbus_keypad_input_policy_get_req_cb,
        .args      =
            "    <arg direction=\"out\" name=\"input_policy\" type=\"s\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_TOUCH_INPUT_POLICY_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = evin_dbus_touch_input_policy_get_req_cb,
        .args      =
            "    <arg direction=\"out\" name=\"input_policy\" type=\"s\"/>\n"
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Add dbus handlers
 */
static void evin_dbus_init(void)
{
    mce_dbus_handler_register_array(evin_dbus_handlers);
}

/** Remove dbus handlers
 */
static void evin_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(evin_dbus_handlers);
}

/* ========================================================================= *
 * EVIN_DATAPIPE
 * ========================================================================= */

/** Pre-change notifications for display_state_curr
 */
static void evin_datapipe_display_state_next_cb(gconstpointer data)
{
    display_state_t prev = display_state_next;
    display_state_next = GPOINTER_TO_INT(data);

    if( prev == display_state_next )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state_next = %s -> %s",
            display_state_repr(prev),
            display_state_repr(display_state_next));
EXIT:
    return;
}

/** Change notifications for proximity_sensor_actual
 */
static void evin_datapipe_proximity_sensor_actual_cb(gconstpointer data)
{
    cover_state_t prev = proximity_sensor_actual;
    proximity_sensor_actual = GPOINTER_TO_INT(data);

    if( proximity_sensor_actual == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "proximity_sensor_actual = %s -> %s",
            proximity_state_repr(prev),
            proximity_state_repr(proximity_sensor_actual));
EXIT:
    return;
}

/** Change notifications from lid_sensor_filtered_pipe
 */
static void evin_datapipe_lid_sensor_filtered_cb(gconstpointer data)
{
    cover_state_t prev = lid_sensor_filtered;
    lid_sensor_filtered = GPOINTER_TO_INT(data);

    if( lid_sensor_filtered == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "lid_sensor_filtered = %s -> %s",
            cover_state_repr(prev),
            cover_state_repr(lid_sensor_filtered));
EXIT:
    return;
}

/** Change notifications for topmost_window_pid_pipe
 */
static void
evin_datapipe_topmost_window_pid_cb(gconstpointer data)
{
    int prev = topmost_window_pid;
    topmost_window_pid = GPOINTER_TO_INT(data);

    if( prev == topmost_window_pid )
        goto EXIT;

    mce_log(LL_DEBUG, "topmost_window_pid: %d -> %d",
            prev, topmost_window_pid);

EXIT:
    return;
}

/** Change notifications for alarm_ui_state
 */
static void evin_datapipe_alarm_ui_state_cb(gconstpointer data)
{
    alarm_ui_state_t prev = alarm_ui_state;
    alarm_ui_state = GPOINTER_TO_INT(data);

    if( alarm_ui_state == MCE_ALARM_UI_INVALID_INT32 )
        alarm_ui_state = MCE_ALARM_UI_OFF_INT32;

    if( alarm_ui_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "alarm_ui_state = %s -> %s",
            alarm_state_repr(prev),
            alarm_state_repr(alarm_ui_state));

EXIT:
    return;
}

/** Change notifications for call_state
 */
static void evin_datapipe_call_state_cb(gconstpointer data)
{
    call_state_t prev = call_state;
    call_state = GPOINTER_TO_INT(data);

    if( call_state == CALL_STATE_INVALID )
        call_state = CALL_STATE_NONE;

    if( call_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "call_state = %s -> %s",
            call_state_repr(prev),
            call_state_repr(call_state));

EXIT:
    return;
}

/** Change notifications for interaction_expected_pipe
 */
static void evin_datapipe_interaction_expected_cb(gconstpointer data)
{
    bool prev = interaction_expected;
    interaction_expected = GPOINTER_TO_INT(data);

    if( prev == interaction_expected )
        goto EXIT;

    mce_log(LL_DEBUG, "interaction_expected: %d -> %d",
            prev, interaction_expected);

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t evin_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &submode_pipe,
        .output_cb = evin_datapipe_submode_cb,
    },
    {
        .datapipe  = &display_state_curr_pipe,
        .output_cb = evin_datapipe_display_state_curr_cb,
    },
    {
        .datapipe  = &touch_detected_pipe,
        .output_cb = evin_datapipe_touch_detected_cb,
    },
    {
        .datapipe  = &touch_grab_wanted_pipe,
        .output_cb = evin_datapipe_touch_grab_wanted_cb,
    },
    {
        .datapipe  = &keypad_grab_wanted_pipe,
        .output_cb = evin_datapipe_keypad_grab_wanted_cb,
    },
    {
        .datapipe  = &display_state_next_pipe,
        .output_cb = evin_datapipe_display_state_next_cb,
    },
    {
        .datapipe  = &proximity_sensor_actual_pipe,
        .output_cb = evin_datapipe_proximity_sensor_actual_cb,
    },
    {
        .datapipe  = &lid_sensor_filtered_pipe,
        .output_cb = evin_datapipe_lid_sensor_filtered_cb,
    },
    {
        .datapipe  = &topmost_window_pid_pipe,
        .output_cb = evin_datapipe_topmost_window_pid_cb,
    },
    {
        .datapipe  = &alarm_ui_state_pipe,
        .output_cb = evin_datapipe_alarm_ui_state_cb,
    },
    {
        .datapipe  = &call_state_pipe,
        .output_cb = evin_datapipe_call_state_cb,
    },
    {
        .datapipe  = &interaction_expected_pipe,
        .output_cb = evin_datapipe_interaction_expected_cb,
    },
    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t evin_datapipe_bindings =
{
    .module   = "mce_input",
    .handlers = evin_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void evin_datapipe_init(void)
{
    mce_datapipe_init_bindings(&evin_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void evin_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&evin_datapipe_bindings);
}

/* ========================================================================= *
 * MODULE_INIT
 * ========================================================================= */

/** Init function for the /dev/input event component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean
mce_input_init(void)
{
    gboolean status = FALSE;

    evin_gpio_init();

    evin_event_mapper_init();

    evin_dbus_init();

    evin_ts_grab_init();

    evin_setting_init();

#ifdef ENABLE_DOUBLETAP_EMULATION
    /* Get fake doubletap policy configuration & track changes */
    mce_setting_notifier_add(MCE_SETTING_EVENT_INPUT_PATH,
                             MCE_SETTING_USE_FAKE_DOUBLETAP,
                             evin_doubletap_setting_cb,
                             &evin_doubletap_emulation_enabled_setting_id);

    mce_setting_get_bool(MCE_SETTING_USE_FAKE_DOUBLETAP,
                         &evin_doubletap_emulation_enabled);
#endif

    /* Append triggers/filters to datapipes */
    evin_datapipe_init();

    /* Register input device directory monitor */
    if( !evin_devdir_monitor_init() )
        goto EXIT;

    /* Find the initial set of input devices */
    if( !evin_iomon_init() )
        goto EXIT;

    evin_iomon_switch_states_update();
    evin_iomon_keyboard_state_update();
    evin_iomon_mouse_state_update();

    status = TRUE;
EXIT:
    return status;
}

/** Exit function for the /dev/input event component
 */
void
mce_input_exit(void)
{
#ifdef ENABLE_DOUBLETAP_EMULATION
    /* Remove fake doubletap policy change notifier */
    mce_setting_notifier_remove(evin_doubletap_emulation_enabled_setting_id),
        evin_doubletap_emulation_enabled_setting_id = 0;
#endif

    /* Remove triggers/filters from datapipes */
    evin_datapipe_quit();

    /* Remove input device directory monitor */
    evin_devdir_monitor_quit();

    evin_setting_quit();

    evin_iomon_quit();

    /* Reset input grab state machines */
    evin_ts_grab_quit();
    evin_input_grab_reset(&evin_kp_grab_state);

    /* Release event mapping lookup tables */
    evin_event_mapper_quit();

    evin_touchstate_cancel_update();

    evin_dbus_quit();

    return;
}
