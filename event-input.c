/**
 * @file event-input.c
 * /dev/input event provider for the Mode Control Entity
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Ismo Laitinen <ismo.laitinen@nokia.com>
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
#ifdef ENABLE_DOUBLETAP_EMULATION
# include "mce-gconf.h"
#endif
#include "mce-sensorfw.h"
#include "evdev.h"

#include <linux/input.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>

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
static void         evin_gpio_submode_trigger                   (gconstpointer data);

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
static int               evin_evdevinfo_match_codes             (const evin_evdevinfo_t *self, int type, const int *codes);

static bool              evin_evdevinfo_is_volumekey_default    (const evin_evdevinfo_t *self);
static bool              evin_evdevinfo_is_volumekey_hammerhead (const evin_evdevinfo_t *self);
static bool              evin_evdevinfo_is_volumekey            (const evin_evdevinfo_t *self);

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

} evin_evdevtype_t;

static const char       *evin_evdevtype_repr                    (evin_evdevtype_t type);
static evin_evdevtype_t  evin_evdevtype_from_info               (evin_evdevinfo_t *info);

/* ------------------------------------------------------------------------- *
 * DOUBLETAP_EMULATION
 * ------------------------------------------------------------------------- */

#ifdef ENABLE_DOUBLETAP_EMULATION

/** Maximum time betweem 1st click and 2nd release, in milliseconds */
# define EVIN_DOUBLETAP_TIME_LIMIT 500

/** Maximum distance between 1st and 2nd clicks, in pixels */
# define EVIN_DOUBLETAP_DISTANCE_LIMIT 100

/** History data for emulating double tap */
typedef struct
{
    /** Timestamp from ending EV_SYN event */
    struct timeval dt_time;

    /** X coordinate accumulated from relative movements */
    int dt_x;

    /** Y coordinate accumulated from relative movements */
    int dt_y;

    /** Bitmask for accumulating touch points */
    int dt_click;
} evin_doubletap_t;

static void         evin_doubletap_gconf_changed_cb             (GConfClient *const client, const guint id, GConfEntry *const entry, gpointer const data);

static int          evin_doubletap_within_time_limit            (const evin_doubletap_t *e1, const evin_doubletap_t *e2);
static int          evin_doubletap_within_dist_limit            (const evin_doubletap_t *e1, const evin_doubletap_t *e2);

static int          evin_doubletap_active_touch_points          (const evin_doubletap_t *e);

static int          evin_doubletap_emulate                      (const struct input_event *eve);

#endif // ENABLE_DOUBLETAP_EMULATION

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

} evin_iomon_extra_t;

static void                evin_iomon_extra_delete_cb           (void *aptr);
static evin_iomon_extra_t *evin_iomon_extra_create              (int fd, const char *name);

// common rate limited activity generation

static void         evin_iomon_generate_activity                (struct input_event *ev, bool cooked, bool raw);

// event handling by device type

static gboolean     evin_iomon_touchscreen_cb                   (gpointer data, gsize bytes_read);
static gboolean     evin_iomon_evin_doubletap_cb                (gpointer data, gsize bytes_read);
static gboolean     evin_iomon_keypress_cb                      (gpointer data, gsize bytes_read);
static gboolean     evin_iomon_activity_cb                      (gpointer data, gsize bytes_read);

// add/remove devices

static void         evin_iomon_device_delete_cb                 (mce_io_mon_t *iomon);
static void         evin_iomon_device_rem_all                   (void);
static void         evin_iomon_device_add                       (const gchar *path);
static void         evin_iomon_device_update                    (const gchar *path, gboolean add);

// check initial switch event states

static void         evin_iomon_switch_states_update_iter_cb     (gpointer io_monitor, gpointer user_data);
static void         evin_iomon_switch_states_update             (void);

static void         evin_iomon_keyboard_state_update_iter_cb    (gpointer io_monitor, gpointer user_data);
static void         evin_iomon_keyboard_state_update            (void);

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
 * INPUT_GRAB  --  GENERIC EVDEV INPUT GRAB STATE MACHINE
 * ------------------------------------------------------------------------- */

typedef struct evin_input_grab_t evin_input_grab_t;

/** State information for generic input grabbing state machine */
struct evin_input_grab_t
{
    /** State machine instance name */
    const char *ig_name;

    /** Currently touched/down */
    bool        ig_touching;

    /** Was touched/down, delaying release */
    bool        ig_touched;

    /** Input grab is wanted */
    bool        ig_want_grab;

    /** Input grab is active */
    bool        ig_have_grab;

    /** Delayed release timer */
    guint       ig_release_id;

    /** Delayed release delay */
    int         ig_release_ms;

    /** Callback for notifying grab status changes */
    void      (*ig_grab_changed_cb)(evin_input_grab_t *self, bool have_grab);

    /** Callback for additional release polling */
    bool      (*ig_release_verify_cb)(evin_input_grab_t *self);
};

static void         evin_input_grab_reset                       (evin_input_grab_t *self);
static gboolean     evin_input_grab_release_cb                  (gpointer aptr);
static void         evin_input_grab_start_release_timer         (evin_input_grab_t *self);
static void         evin_input_grab_cancel_release_timer        (evin_input_grab_t *self);
static void         evin_input_grab_rethink                     (evin_input_grab_t *self);
static void         evin_input_grab_set_touching                (evin_input_grab_t *self, bool touching);
static void         evin_input_grab_request_grab                (evin_input_grab_t *self, bool want_grab);
static void         evin_input_grab_iomon_cb                    (gpointer data, gpointer user_data);

/* ------------------------------------------------------------------------- *
 * TS_GRAB  --  TOUCHSCREEN EVDEV INPUT GRAB STATE MACHINE
 * ------------------------------------------------------------------------- */

static void         evin_ts_grab_set_led_raw                    (bool enabled);
static gboolean     evin_ts_grab_set_led_cb                     (gpointer aptr);
static void         evin_ts_grab_set_led                        (bool enabled);

static void         evin_ts_grab_set_active                     (gboolean grab);

static bool         evin_ts_grab_poll_palm_detect               (evin_input_grab_t *ctrl);

static void         evin_ts_grab_changed                        (evin_input_grab_t *ctrl, bool grab);

static void         evin_ts_grab_event_filter_cb                (struct input_event *ev);

static void         evin_ts_grab_wanted_cb                      (gconstpointer data);
static void         evin_ts_grab_display_state_cb               (gconstpointer data);

static void         evin_ts_grab_gconf_changed_cb               (GConfClient *const client, const guint id, GConfEntry *const entry, gpointer const data);

static void         evin_ts_grab_init                           (void);
static void         evin_ts_grab_quit                           (void);

/* ------------------------------------------------------------------------- *
 * KP_GRAB  --  KEYPAD EVDEV INPUT GRAB STATE MACHINE
 * ------------------------------------------------------------------------- */

static void         evin_kp_grab_set_active                     (gboolean grab);
static void         evin_kp_grab_changed                        (evin_input_grab_t *ctrl, bool grab);
static void         evin_kp_grab_event_filter_cb                (struct input_event *ev);
static void         evin_kp_grab_wanted_cb                      (gconstpointer data);

/* ------------------------------------------------------------------------- *
 * MODULE_INIT
 * ------------------------------------------------------------------------- */

gboolean            mce_input_init                              (void);
void                mce_input_exit                              (void);

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
evin_gpio_submode_trigger(gconstpointer data)
{
    static submode_t old_submode = MCE_NORMAL_SUBMODE;
    submode_t submode = GPOINTER_TO_INT(data);

    /* If the tklock is enabled, disable the camera focus interrupts,
     * since we don't use them anyway
     */
    if( evin_gpio_key_disable_exists ) {
        submode_t tklock_prev = (old_submode & MCE_TKLOCK_SUBMODE);
        submode_t tklock_curr = (submode & MCE_TKLOCK_SUBMODE);

        if( tklock_prev != tklock_curr ) {
            if( tklock_curr )
                evin_gpio_key_disable(KEY_CAMERA_FOCUS);
            else
                evin_gpio_key_enable(KEY_CAMERA_FOCUS);
        }
    }

    old_submode = submode;
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
    static const char grp[] = "EVDEV";

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
 * @param self  evin_evdevinfo_t object
 * @param types evdev event type
 * @param codes array of evdev event codes, terminated with -1
 *
 * @return 1 if all of codes and only codes are supported, 0 otherwise
 */
static int
evin_evdevinfo_match_codes(const evin_evdevinfo_t *self, int type, const int *codes)
{
    for( int ecode = 0; ecode < KEY_CNT; ++ecode ) {
        int have = evin_evdevinfo_has_code(self, type, ecode);
        int want = evin_evdevinfo_list_has_entry(codes, ecode);
        if( have != want )
            return 0;
    }
    return 1;
}

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

    /* Except we do not care if autorepeat controls are there or not */
    static const int ignored_types[] = {
        EV_REP,
        -1
    };

    return (evin_evdevinfo_match_types_ex(self, wanted_types, ignored_types) &&
            evin_evdevinfo_match_codes(self, EV_KEY, wanted_key_codes));
}

/** Test if input device is exactly like volume key device in Nexus 5
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

    static const int wanted_sw_codes[] = {
        SW_LID, // magnetic lid cover sensor
        -1
    };

    return (evin_evdevinfo_match_types(self, wanted_types) &&
            evin_evdevinfo_match_codes(self, EV_KEY, wanted_key_codes) &&
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
        [EVDEV_INPUT]    = "KEY, BUTTON or SWITCH",
        [EVDEV_ACTIVITY] = "USER ACTIVITY ONLY",
        [EVDEV_IGNORE]   = "IGNORE",
        [EVDEV_DBLTAP]   = "DOUBLE TAP",
        [EVDEV_PS]       = "PROXIMITY SENSOR",
        [EVDEV_ALS]      = "AMBIENT LIGHT SENSOR",
        [EVDEV_VOLKEY]   = "VOLUME KEYS",
        [EVDEV_KEYBOARD] = "KEYBOARD",
    };

    return lut[type];
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
        res = EVDEV_TOUCH;
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
static gboolean fake_evin_doubletap_enabled = FALSE;

/** GConf callback ID for fake doubletap policy changes */
static guint fake_evin_doubletap_id = 0;

/** Callback for handling changes to fake doubletap configuration
 *
 * @param client (not used)
 * @param id     (not used)
 * @param entry  GConf entry that changed
 * @param data   (not used)
 */
static void
evin_doubletap_gconf_changed_cb(GConfClient *const client, const guint id,
                                GConfEntry *const entry, gpointer const data)
{
    (void)client; (void)id; (void)data;

    gboolean enabled = fake_evin_doubletap_enabled;
    const GConfValue *value = 0;

    if( entry && (value = gconf_entry_get_value(entry)) ) {
        if( value->type == GCONF_VALUE_BOOL )
            enabled = gconf_value_get_bool(value);
    }
    if( fake_evin_doubletap_enabled != enabled ) {
        mce_log(LL_NOTICE, "use fake doubletap change: %d -> %d",
                fake_evin_doubletap_enabled, enabled);
        fake_evin_doubletap_enabled = enabled;
    }
}

/** Check if two double tap history points are close enough in time
 *
 * @param e1 event data from the 1st click
 * @param e2 event data from the 2nd release
 *
 * @return TRUE if e1 and e2 times are valid and close enough,
 *         or FALSE otherwise
 */
static int
evin_doubletap_within_time_limit(const evin_doubletap_t *e1, const evin_doubletap_t *e2)
{
    static const struct timeval limit =
    {
        .tv_sec  = (EVIN_DOUBLETAP_TIME_LIMIT / 1000),
        .tv_usec = (EVIN_DOUBLETAP_TIME_LIMIT % 1000) * 1000,
    };

    struct timeval delta;

    /* Reject empty/reset slots */
    if( !timerisset(&e1->dt_time) || !timerisset(&e2->dt_time) )
        return 0;

    timersub(&e2->dt_time, &e1->dt_time, &delta);
    return timercmp(&delta, &limit, <);
}

/** Check if two double tap history points are close enough in pixels
 *
 * @param e1 event data from the 1st click
 * @param e2 event data from the 2nd click
 *
 * @return TRUE if e1 and e2 positions are close enough, or FALSE otherwise
 */
static int
evin_doubletap_within_dist_limit(const evin_doubletap_t *e1, const evin_doubletap_t *e2)
{
    int x = e2->dt_x - e1->dt_x;
    int y = e2->dt_y - e1->dt_y;
    int r = EVIN_DOUBLETAP_DISTANCE_LIMIT;

    return (x*x + y*y) < (r*r);
}

/** Accumulator steps for counting touch/mouse click events separately
 *
 *    2   2   2   1   1   0   0   0
 *    8   4   0   6   2   8   4   0
 * --------------------------------
 *                             mmmm [ 3: 0]  BTN_MOUSE
 *                         pppp     [ 7: 4]  ABS_MT_PRESSURE
 *                     tttt         [11: 8]  ABS_MT_TOUCH_MAJOR
 *                 iiii             [15:12]  ABS_MT_TRACKING_ID
 * aaaabbbbccccdddd                 [31:16]  (reserved)
 */
enum {

    SEEN_EVENT_MOUSE       = 1 <<  0,
    SEEN_EVENT_PRESSURE    = 1 <<  4,
    SEEN_EVENT_TOUCH_MAJOR = 1 <<  8,
    SEEN_EVENT_TRACKING_ID = 1 << 12,
};

/** Helper for probing no-touch vs single-touch vs multi-touch
 *
 * return 0 for no-touch, 1 for single touch, >1 for multi-touch
 */
static int
evin_doubletap_active_touch_points(const evin_doubletap_t *e)
{
    /* The bit shuffling below calculates maximum number of mouse
     * button click / touch point events accumulated to the history
     * buffer to produce return value of
     *
     *   =0 -> no touch
     *   =1 -> singletouch
     *   >1 -> multitouch
     *
     * Note: If the event stream happens to report one ABS_MT_PRESSURE
     * and two ABS_MT_TOUCH_MAJOR events / something similar it will
     * be reported as "triple touch", but we do not need care as long
     * as it is not "no touch" or "singletouch".
     */

    unsigned m = e->dt_click;
    m |= (m >> 16);
    m |= (m >>  8);
    m |= (m >>  4);
    return m & 15;
}

/** Process mouse input events to simulate double tap
 *
 * Maintain a crude state machine, that will detect double clicks
 * made with mouse when fed with evdev events from a mouse device.
 *
 * @param eve input event
 *
 * @return TRUE if double tap sequence was detected, FALSE otherwise
 */
static int
evin_doubletap_emulate(const struct input_event *eve)
{
    static evin_doubletap_t hist[4]; // click/release ring buffer

    static unsigned i0       = 0; // current position
    static int      x_accum  = 0; // x delta accumulator
    static int      y_accum  = 0; // y delta accumulator
    static bool     skip_syn = true; // flag: ignore SYN_REPORT

    int result = FALSE; // assume: no doubletap

    unsigned i1, i2, i3; // 3 last positions

    switch( eve->type ) {
    case EV_REL:
        /* Accumulate X/Y position */
        switch( eve->code ) {
        case REL_X: x_accum += eve->value; break;
        case REL_Y: y_accum += eve->value; break;
        default: break;
        }
        break;

    case EV_KEY:
        switch( eve->code ) {
        case BTN_MOUSE:
            /* Store click/release and position */
            if( eve->value )
                hist[i0].dt_click += SEEN_EVENT_MOUSE;
            hist[i0].dt_x = x_accum;
            hist[i0].dt_y = y_accum;

            /* We have a mouse click to process */
            skip_syn = false;
            break;

        case BTN_TOUCH:
            /* Start/end of touch - if these are emitted by the touch
             * driver, we must not expect to see SYN_MT_REPORT events
             * on touch release. */
            if( eve->value == 0 )
                skip_syn = false;
            break;

        default:
            break;
        }
        break;

    case EV_ABS:
        /* Do multitouch too while at it */
        switch( eve->code ) {
        case ABS_MT_PRESSURE:
            if( eve->value > 0 )
                hist[i0].dt_click += SEEN_EVENT_PRESSURE;
            skip_syn = false;
            break;
        case ABS_MT_TOUCH_MAJOR:
            if( eve->value > 0 )
                hist[i0].dt_click += SEEN_EVENT_TOUCH_MAJOR;
            skip_syn = false;
            break;
        case ABS_MT_TRACKING_ID:
            if( eve->value != -1 )
                hist[i0].dt_click += SEEN_EVENT_TRACKING_ID;
            skip_syn = false;
            break;
        case ABS_MT_POSITION_X:
            hist[i0].dt_x = eve->value;
            skip_syn = false;
            break;
        case ABS_MT_POSITION_Y:
            hist[i0].dt_y = eve->value;
            skip_syn = false;
            break;
        default:
            break;
        }
        break;

    case EV_SYN:
        if( eve->code == SYN_MT_REPORT ) {
            /* We have a touch event to process */
            skip_syn = false;
            break;
        }

        if( eve->code != SYN_REPORT )
            break;

        /* Have we seen button events? */
        if( skip_syn )
            break;

        /* Next SYN_REPORT will be ignored unless something
         * relevant is seen before that */
        skip_syn = true;

        /* Set timestamp from syn event */
        hist[i0].dt_time = eve->time;

        /* Last event before current */
        i1 = (i0 + 3) & 3;

        int tp0 = evin_doubletap_active_touch_points(hist+i0);
        int tp1 = evin_doubletap_active_touch_points(hist+i1);

        if( tp0 != tp1 ) {
            /* 2nd and 3rd last events before current */
            i2 = (i0 + 2) & 3;
            i3 = (i0 + 1) & 3;

            int tp2 = evin_doubletap_active_touch_points(hist+i2);
            int tp3 = evin_doubletap_active_touch_points(hist+i3);

            /* Release after click after release after click,
             * within the time and distance limits */
            if( tp0 == 0 && tp1 == 1 && tp2 == 0 && tp3 == 1 &&
                evin_doubletap_within_time_limit(&hist[i3], &hist[i0]) &&
                evin_doubletap_within_dist_limit(&hist[i3], &hist[i1]) ) {
                /* Reached DOUBLETAP state */
                result = TRUE;

                /* Reset history, so that triple click
                 * will not produce 2 double taps etc */
                memset(hist, 0, sizeof hist);
                x_accum = y_accum = 0;
            }

            /* Move to the next slot */
            i0 = (i0 + 1) & 3;
        }

        /* Reset the current position in the ring buffer */
        memset(&hist[i0], 0, sizeof *hist);
        break;

    default:
        break;
    }

    return result;
}

#endif /* ENABLE_DOUBLETAP_EMULATION */

/* ========================================================================= *
 * EVDEV_IO_MONITORING
 * ========================================================================= */

static void
evin_iomon_extra_delete_cb(void *aptr)
{
    evin_iomon_extra_t *self = aptr;

    if( self ) {
        evin_evdevinfo_delete(self->ex_info);
        g_free(self->ex_sw_keypad_slide);
        free(self->ex_name);
        free(self);
    }
}

static evin_iomon_extra_t *
evin_iomon_extra_create(int fd, const char *name)
{
    evin_iomon_extra_t *self = calloc(1, sizeof *self);

    self->ex_name = strdup(name);
    self->ex_info = evin_evdevinfo_create();

    evin_evdevinfo_probe(self->ex_info, fd);

    self->ex_type = evin_evdevtype_from_info(self->ex_info);

    self->ex_sw_keypad_slide = 0;

    if( self->ex_type == EVDEV_KEYBOARD ) {
        self->ex_sw_keypad_slide = mce_conf_get_string("SW_KEYPAD_SLIDE",
                                                       self->ex_name, 0);
    }

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

    time_t t = ev->time.tv_sec;

    /* Actual, never synthetized user activity */
    if( raw ) {
        if( t_raw != t ) {
            t_raw = t;
            execute_datapipe_output_triggers(&user_activity_pipe,
                                             ev, USE_INDATA);
        }
    }

    /* Generic, possibly synthetized user activity */
    if( cooked ) {
        submode_t submode = mce_get_submode_int32();

        if( t_cooked != t || (submode & MCE_EVEATER_SUBMODE) ) {
            t_cooked = t;
            execute_datapipe(&device_inactive_pipe,
                             GINT_TO_POINTER(FALSE),
                             USE_INDATA, CACHE_INDATA);
        }
    }

EXIT:
    return;
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
evin_iomon_touchscreen_cb(gpointer data, gsize bytes_read)
{
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

    evin_ts_grab_event_filter_cb(ev);

    bool grabbed = datapipe_get_gint(touch_grab_active_pipe);

#ifdef ENABLE_DOUBLETAP_EMULATION
    if( grabbed || fake_evin_doubletap_enabled ) {
        /* Note: In case we happen to be in middle of display
         *       state transition the double tap simulation must
         *       use the next stable display state rather than
         *       the current - potentially transitional - state.
         */
        display_state_t display_state_next =
            datapipe_get_gint(display_state_next_pipe);

        switch( display_state_next ) {
        case MCE_DISPLAY_OFF:
        case MCE_DISPLAY_LPM_OFF:
        case MCE_DISPLAY_LPM_ON:
            if( evin_doubletap_emulate(ev) ) {
                mce_log(LL_DEVEL, "[doubletap] emulated from touch input");
                ev->type  = EV_MSC;
                ev->code  = MSC_GESTURE;
                ev->value = 0x4;
            }
            break;
        default:
        case MCE_DISPLAY_ON:
        case MCE_DISPLAY_DIM:
        case MCE_DISPLAY_UNDEF:
        case MCE_DISPLAY_POWER_UP:
        case MCE_DISPLAY_POWER_DOWN:
            break;
        }
    }
#endif

    /* Power key up event from touch screen -> double tap gesture event */
    if( ev->type == EV_KEY && ev->code == KEY_POWER && ev->value == 0 ) {
        cover_state_t proximity_sensor_state =
            datapipe_get_gint(proximity_sensor_pipe);

        cover_state_t lid_cover_policy_state =
            datapipe_get_gint(lid_cover_policy_pipe);

        mce_log(LL_DEVEL, "[doubletap] as power key event; "
                "proximity=%s, lid=%s",
                proximity_state_repr(proximity_sensor_state),
                proximity_state_repr(lid_cover_policy_state));

        /* Mimic N9 style gesture event for which we
         * already have logic in place. Possible filtering
         * due to proximity state etc happens at tklock.c
         */
        ev->type  = EV_MSC;
        ev->code  = MSC_GESTURE;
        ev->value = 0x4;
    }

    /* Ignore unwanted events */
    if( ev->type != EV_ABS &&
        ev->type != EV_KEY &&
        ev->type != EV_MSC )
        goto EXIT;

    /* Do not generate activity if ts input is grabbed */
    if( !grabbed )
        evin_iomon_generate_activity(ev, true, true);

    submode_t submode = mce_get_submode_int32();

    /* If the event eater is active, don't send anything */
    if( submode & MCE_EVEATER_SUBMODE )
        goto EXIT;

    /* Only send pressure and gesture events */
    if( (ev->type == EV_ABS && ev->code == ABS_PRESSURE) ||
        (ev->type == EV_KEY && ev->code == BTN_TOUCH ) ||
        (ev->type == EV_MSC && ev->code == MSC_GESTURE ) ) {
        /* For now there's no reason to cache the value */
        execute_datapipe(&touchscreen_pipe, &ev,
                         USE_INDATA, DONT_CACHE_INDATA);
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
evin_iomon_evin_doubletap_cb(gpointer data, gsize bytes_read)
{
    struct input_event *ev = data;
    gboolean flush = FALSE;

    /* Don't process invalid reads */
    if( bytes_read != sizeof (*ev) )
        goto EXIT;

    /* Feed power key events to touchscreen handler for
     * possible double tap gesture event conversion */
    if( ev->type == EV_KEY && ev->code == KEY_POWER ) {
        evin_iomon_touchscreen_cb(ev, sizeof *ev);
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
evin_iomon_keypress_cb(gpointer data, gsize bytes_read)
{
    submode_t submode = mce_get_submode_int32();
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
        if( datapipe_get_gint(keypad_grab_active_pipe) ) {
            switch( ev->code ) {
            case KEY_VOLUMEUP:
            case KEY_VOLUMEDOWN:
                mce_log(LL_DEVEL, "ignore volume key event");
                goto EXIT;
            default:
                break;
            }
        }
        if ((ev->code == KEY_SCREENLOCK) && (ev->value != 2)) {
            (void)execute_datapipe(&lockkey_pipe,
                                   GINT_TO_POINTER(ev->value),
                                   USE_INDATA, CACHE_INDATA);
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
        if (((ev->code != KEY_CAMERA_FOCUS) &&
             (ev->code != KEY_SCREENLOCK) &&
             ((((submode & MCE_EVEATER_SUBMODE) == 0) &&
               (ev->value == 1)) || (ev->value == 0))) &&
            ((submode & MCE_PROXIMITY_TKLOCK_SUBMODE) == 0)) {
            (void)execute_datapipe(&keypress_pipe, &ev,
                                   USE_INDATA, DONT_CACHE_INDATA);
        }
    }

    if (ev->type == EV_SW) {
        switch (ev->code) {
        case SW_CAMERA_LENS_COVER:
            if (ev->value != 2) {
                (void)execute_datapipe(&lens_cover_pipe, GINT_TO_POINTER(ev->value ? COVER_CLOSED : COVER_OPEN), USE_INDATA, CACHE_INDATA);
            }

            /* Don't generate activity on COVER_CLOSED */
            if (ev->value == 1)
                goto EXIT;

            break;

        case SW_KEYPAD_SLIDE:
            if (ev->value != 2) {
                (void)execute_datapipe(&keyboard_slide_pipe, GINT_TO_POINTER(ev->value ? COVER_CLOSED : COVER_OPEN), USE_INDATA, CACHE_INDATA);
                evin_iomon_keyboard_state_update();
            }

            /* Don't generate activity on COVER_CLOSED */
            if (ev->value == 1)
                goto EXIT;

            break;

        case SW_FRONT_PROXIMITY:
            if (ev->value != 2) {
                (void)execute_datapipe(&proximity_sensor_pipe, GINT_TO_POINTER(ev->value ? COVER_CLOSED : COVER_OPEN), USE_INDATA, CACHE_INDATA);
            }

            break;

        case SW_HEADPHONE_INSERT:
        case SW_MICROPHONE_INSERT:
        case SW_LINEOUT_INSERT:
        case SW_VIDEOOUT_INSERT:
            if (ev->value != 2) {
                (void)execute_datapipe(&jack_sense_pipe, GINT_TO_POINTER(ev->value ? COVER_CLOSED : COVER_OPEN), USE_INDATA, CACHE_INDATA);
            }

            break;

        case SW_LID:
            /* hammerhead magnetic lid sensor; Feed in to the
             * same datapipe as N770 sliding cover uses */
            if( ev->value ) {
                execute_datapipe(&lid_cover_sensor_pipe,
                                 GINT_TO_POINTER(COVER_CLOSED),
                                 USE_INDATA, CACHE_INDATA);
            }
            else {
                execute_datapipe(&lid_cover_sensor_pipe,
                                 GINT_TO_POINTER(COVER_OPEN),
                                 USE_INDATA, CACHE_INDATA);
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
    if( ev->type == EV_KEY && ev->code == KEY_POWER ) {
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
evin_iomon_activity_cb(gpointer data, gsize bytes_read)
{
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

    evin_iomon_keyboard_state_update();
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
        execute_datapipe(&lens_cover_pipe, GINT_TO_POINTER(state),
                         USE_INDATA, CACHE_INDATA);
    }

    /* Check initial keypad slide state */
    ecode = evin_event_mapper_rlookup_switch(SW_KEYPAD_SLIDE);
    if( test_bit(ecode, featurelist) ) {
        state = test_bit(ecode, statelist) ? COVER_CLOSED : COVER_OPEN;
        execute_datapipe(&keyboard_slide_pipe, GINT_TO_POINTER(state),
                         USE_INDATA, CACHE_INDATA);
    }

    /* Check initial front proximity state */
    ecode = evin_event_mapper_rlookup_switch(SW_FRONT_PROXIMITY);
    if( test_bit(ecode, featurelist) ) {
        state = test_bit(ecode, statelist) ? COVER_CLOSED : COVER_OPEN;
        execute_datapipe(&proximity_sensor_pipe, GINT_TO_POINTER(state),
                         USE_INDATA, CACHE_INDATA);
    }

    /* Check initial lid sensor state */
    ecode = evin_event_mapper_rlookup_switch(SW_LID);
    if( test_bit(ecode, featurelist) ) {
        state = test_bit(ecode, statelist) ? COVER_CLOSED : COVER_OPEN;
        mce_log(LL_DEVEL, "SW_LID initial state = %s",
                cover_state_repr(state));
        execute_datapipe(&lid_cover_sensor_pipe, GINT_TO_POINTER(state),
                         USE_INDATA, CACHE_INDATA);
    }

    /* Need to consider more than one switch state when setting the
     * initial value of the jack_sense_pipe */

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
        execute_datapipe(&jack_sense_pipe, GINT_TO_POINTER(state),
                         USE_INDATA, CACHE_INDATA);
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

    execute_datapipe(&keyboard_available_pipe,
                     GINT_TO_POINTER(state),
                     USE_INDATA, CACHE_INDATA);
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
 * INPUT_GRAB  --  GENERIC EVDEV INPUT GRAB STATE MACHINE
 * ========================================================================= */

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

    // do we want to change state?
    if( self->ig_have_grab == self->ig_want_grab )
        goto EXIT;

    // make the transition
    self->ig_have_grab = self->ig_want_grab;

    // and report it
    if( self->ig_grab_changed_cb )
        self->ig_grab_changed_cb(self, self->ig_have_grab);

EXIT:
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

/** Low level helper for input grab debug led pattern activate/deactivate
 */
static void
evin_ts_grab_set_led_raw(bool enabled)
{
    execute_datapipe_output_triggers(enabled ?
                                     &led_pattern_activate_pipe :
                                     &led_pattern_deactivate_pipe,
                                     "PatternTouchInputBlocked",
                                     USE_INDATA);
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

    switch( datapipe_get_gint(display_state_pipe) )
    {
    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        enable = datapipe_get_gint(touch_grab_active_pipe);
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

    // STATE MACHINE -> OUTPUT DATAPIPE
    execute_datapipe(&touch_grab_active_pipe,
                     GINT_TO_POINTER(grab),
                     USE_INDATA, CACHE_INDATA);

    evin_ts_grab_rethink_led();

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

enum
{
    TS_RELEASE_DELAY_DEFAULT = 100,
    TS_RELEASE_DELAY_BLANK   = 100,
    TS_RELEASE_DELAY_UNBLANK = 600,
};

/** State data for touch input grab state machine */
static evin_input_grab_t evin_ts_grab_state =
{
    .ig_name      = "ts",

    .ig_touching  = false,
    .ig_touched   = false,

    .ig_want_grab = false,
    .ig_have_grab = false,

    .ig_release_id = 0,
    .ig_release_ms = TS_RELEASE_DELAY_DEFAULT,

    .ig_grab_changed_cb = evin_ts_grab_changed,
    .ig_release_verify_cb = evin_ts_grab_poll_palm_detect,
};

/* Touch unblock delay from settings [ms] */
static gint evin_ts_grab_release_delay = TS_RELEASE_DELAY_DEFAULT;

/** GConf notification ID for touch unblock delay */
static guint evin_ts_grab_release_delay_id = 0;

/** Gconf notification callback for touch unblock delay
 *
 * @param client (not used)
 * @param id     (not used)
 * @param entry  GConf entry that changed
 * @param data   (not used)
 */
static void
evin_ts_grab_gconf_changed_cb(GConfClient *const client,
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

/** Event filter for determining finger on screen state
 */
static void
evin_ts_grab_event_filter_cb(struct input_event *ev)
{
    static bool x = false, y = false, p = false, r = false;

    switch( ev->type ) {
    case EV_SYN:
        switch( ev->code ) {
        case SYN_MT_REPORT:
            r = true;
            break;

        case SYN_REPORT:
            if( r ) {
                evin_input_grab_set_touching(&evin_ts_grab_state,
                                             x && y && p);
                x = y = p = r = false;
            }
            break;

        default:
            break;
        }
        break;

    case EV_KEY:
        switch( ev->code ) {
        case BTN_TOUCH:
            if( ev->value == 0 )
                r = true;
            break;

        default:
            break;
        }
        break;

    case EV_ABS:
        switch( ev->code ) {
        case ABS_MT_POSITION_X:
            x = true;
            break;

        case ABS_MT_POSITION_Y:
            y = true;
            break;

        case ABS_MT_TOUCH_MAJOR:
        case ABS_MT_PRESSURE:
            if( ev->value > 0 )
                p = true;
            break;

        default:
            break;
        }
        break;

    default:
        break;
    }
}
/** Feed desired touch grab state from datapipe to state machine
 *
 * @param data The grab wanted boolean as a pointer
 */
static void
evin_ts_grab_wanted_cb(gconstpointer data)
{
    bool required = GPOINTER_TO_INT(data);

    // INPUT DATAPIPE -> STATE MACHINE

    evin_input_grab_request_grab(&evin_ts_grab_state, required);
}

/** Take display state changes in account for touch grab state
 *
 * @param data Display state as void pointer
 */
static void
evin_ts_grab_display_state_cb(gconstpointer data)
{
    static display_state_t display_state = MCE_DISPLAY_UNDEF;

    display_state_t prev = display_state;
    display_state = GPOINTER_TO_INT(data);

    if( display_state == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "display_state=%s", display_state_repr(display_state));

    switch( display_state ) {
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
        evin_input_grab_set_touching(&evin_ts_grab_state, true);

    case MCE_DISPLAY_ON:
    case MCE_DISPLAY_DIM:
        evin_ts_grab_state.ig_release_ms = evin_ts_grab_release_delay;
        if( prev != MCE_DISPLAY_ON && prev != MCE_DISPLAY_DIM ) {
            /* End the faked touch once the display is
             * fully on. If there is a finger on the
             * screen we will get more input events
             * before the delay from artificial touch
             * release ends. */
            evin_input_grab_set_touching(&evin_ts_grab_state, false);
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
    mce_gconf_notifier_add(MCE_GCONF_EVENT_INPUT_PATH,
                           MCE_GCONF_TOUCH_UNBLOCK_DELAY_PATH,
                           evin_ts_grab_gconf_changed_cb,
                           &evin_ts_grab_release_delay_id);

    mce_gconf_get_int(MCE_GCONF_TOUCH_UNBLOCK_DELAY_PATH,
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
    mce_gconf_notifier_remove(evin_ts_grab_release_delay_id),
        evin_ts_grab_release_delay_id = 0;

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
    execute_datapipe(&keypad_grab_active_pipe,
                     GINT_TO_POINTER(grab),
                     USE_INDATA, CACHE_INDATA);

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

/** State data for volumekey input grab state machine */
static evin_input_grab_t evin_kp_grab_state =
{
    .ig_name      = "kp",

    .ig_touching  = false,
    .ig_touched   = false,

    .ig_want_grab = false,
    .ig_have_grab = false,

    .ig_release_id = 0,
    .ig_release_ms = 200,

    .ig_grab_changed_cb = evin_kp_grab_changed,
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
evin_kp_grab_wanted_cb(gconstpointer data)
{
    bool required = GPOINTER_TO_INT(data);

    // INPUT DATAPIPE -> STATE MACHINE
    evin_input_grab_request_grab(&evin_kp_grab_state, required);
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

    evin_ts_grab_init();

#ifdef ENABLE_DOUBLETAP_EMULATION
    /* Get fake doubletap policy configuration & track changes */
    mce_gconf_notifier_add(MCE_GCONF_EVENT_INPUT_PATH,
                           MCE_GCONF_USE_FAKE_DOUBLETAP_PATH,
                           evin_doubletap_gconf_changed_cb,
                           &fake_evin_doubletap_id);

    mce_gconf_get_bool(MCE_GCONF_USE_FAKE_DOUBLETAP_PATH,
                       &fake_evin_doubletap_enabled);
#endif

    /* Append triggers/filters to datapipes */
    append_output_trigger_to_datapipe(&submode_pipe,
                                      evin_gpio_submode_trigger);
    append_output_trigger_to_datapipe(&display_state_pipe,
                                      evin_ts_grab_display_state_cb);
    append_output_trigger_to_datapipe(&touch_grab_wanted_pipe,
                                      evin_ts_grab_wanted_cb);
    append_output_trigger_to_datapipe(&keypad_grab_wanted_pipe,
                                      evin_kp_grab_wanted_cb);

    /* Register input device directory monitor */
    if( !evin_devdir_monitor_init() )
        goto EXIT;

    /* Find the initial set of input devices */
    if( !evin_iomon_init() )
        goto EXIT;

    evin_iomon_switch_states_update();
    evin_iomon_keyboard_state_update();

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
    mce_gconf_notifier_remove(fake_evin_doubletap_id),
        fake_evin_doubletap_id = 0;
#endif

    /* Remove triggers/filters from datapipes */
    remove_output_trigger_from_datapipe(&submode_pipe,
                                        evin_gpio_submode_trigger);
    remove_output_trigger_from_datapipe(&display_state_pipe,
                                        evin_ts_grab_display_state_cb);
    remove_output_trigger_from_datapipe(&touch_grab_wanted_pipe,
                                        evin_ts_grab_wanted_cb);
    remove_output_trigger_from_datapipe(&keypad_grab_wanted_pipe,
                                        evin_kp_grab_wanted_cb);

    /* Remove input device directory monitor */
    evin_devdir_monitor_quit();

    evin_iomon_quit();

    /* Reset input grab state machines */
    evin_ts_grab_quit();
    evin_input_grab_reset(&evin_kp_grab_state);

    /* Release event mapping lookup tables */
    evin_event_mapper_quit();

    return;
}
