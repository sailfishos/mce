/** @file keypad.c
 *
 * Keypad module -- this handles the keypress logic for MCE
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * @author Matti Lehtimäki <matti.lehtimaki@gmail.com>
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

#include "keypad.h"

#include "../mce.h"
#include "../mce-conf.h"
#include "../mce-dbus.h"
#include "../mce-io.h"
#include "../mce-lib.h"
#include "../mce-log.h"
#include "../mce-setting.h"

#include "led.h"

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

#include <gmodule.h>

/* ========================================================================= *
 * Types
 * ========================================================================= */

typedef enum {
    KEYPAD_TYPE_SLIDE,
    KEYPAD_TYPE_FLIP,
    KEYPAD_TYPE_COUNT
} keypad_type_t;

/* ========================================================================= *
 * Configuration
 * ========================================================================= */

/** Module name */
#define MODULE_NAME "keypad"

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
    /** Name of the module */
    .name     = MODULE_NAME,
    /** Module provides */
    .provides = provides,
    /** Module priority */
    .priority = 100
};

/** Maximum number of concurrent button backlight mode clients */
#define KEYPAD_MAX_MODE_CLIENTS    1

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * UTILITY
 * ------------------------------------------------------------------------- */

static const char *bool_repr(int value);
static const char *mode_repr(int value);

/* ------------------------------------------------------------------------- *
 * KEYPAD_TYPE
 * ------------------------------------------------------------------------- */

static const char    *keypad_type_repr (keypad_type_t value);
static keypad_type_t  keypad_type_parse(const char *name);

/* ------------------------------------------------------------------------- *
 * KEYPAD
 * ------------------------------------------------------------------------- */

static void     keypad_set_mode                (int backlight_mode);
static void     keypad_set_brightness          (guint brightness);
static void     keypad_set_backlight_on        (void);
static void     keypad_set_backlight_off       (void);
static gboolean keypad_backlight_timeout_cb    (gpointer data);
static void     keypad_setup_backlight_timeout (void);
static void     keypad_cancel_backlight_timeout(void);
static bool     keypad_rethink_backlight_policy(void);

/* ------------------------------------------------------------------------- *
 * KEYPAD_DATAPIPE
 * ------------------------------------------------------------------------- */

static void keypad_datapipe_key_backlight_brightness_cb(gconstpointer data);
static void keypad_datapipe_system_state_cb            (gconstpointer data);
static void keypad_datapipe_display_state_curr_cb      (gconstpointer data);
static void keypad_datapipe_submode_cb                 (gconstpointer const data);
static void keypad_datapipe_interaction_expected_cb    (gconstpointer const data);
static void keypad_datapipe_device_inactive_cb         (gconstpointer const data);
static void keypad_datapipe_keyboard_slide_state_cb    (gconstpointer const data);
static void keypad_datapipe_lid_sensor_filtered_cb     (gconstpointer const data);
static void keypad_datapipe_alarm_ui_state_cb          (gconstpointer data);
static void keypad_datapipe_call_state_cb              (gconstpointer const data);
static void keypad_datapipe_battery_state_cb           (gconstpointer data);
static void keypad_datapipe_init                       (void);
static void keypad_datapipe_quit                       (void);

/* ------------------------------------------------------------------------- *
 * KEYPAD_DBUS
 * ------------------------------------------------------------------------- */

static gboolean keypad_dbus_mode_client_exit_cb       (DBusMessage *const sig);
static bool     keypad_dbus_add_mode_client           (const char *dbus_name, int backlight_mode);
static void     keypad_dbus_remove_mode_client        (const char *dbus_name);
static void     keypad_dbus_remove_all_mode_clients   (void);
static gboolean keypad_dbus_send_key_backlight_state  (DBusMessage *const method_call);
static gboolean keypad_dbus_get_key_backlight_state_cb(DBusMessage *const req);
static gboolean keypad_dbus_set_key_backlight_state_cb(DBusMessage *const req);
static void     keypad_dbus_init                      (void);
static void     keypad_dbus_quit                      (void);

/* ------------------------------------------------------------------------- *
 * KEYPAD_SETTINGS
 * ------------------------------------------------------------------------- */

static void keypad_settings_change_cb(GConfClient *const gcc, const guint id, GConfEntry *const entry, gpointer const data);
static void keypad_settings_init     (void);
static void keypad_settings_quit     (void);

/* ------------------------------------------------------------------------- *
 * KEYPAD_CONFIG
 * ------------------------------------------------------------------------- */

static bool keypad_config_probe_binary_control (void);
static bool keypad_config_probe_vanilla_control(void);
static void keypad_config_init                 (void);
static void keypad_config_quit                 (void);

/* ------------------------------------------------------------------------- *
 * G_MODULE
 * ------------------------------------------------------------------------- */

const gchar *g_module_check_init(GModule *module);
void         g_module_unload    (GModule *module);

/* ========================================================================= *
 * Data
 * ========================================================================= */

/** Select between SLIDE and FLIP type keypads
 *
 * SLIDE:
 * - uses keypad slide switch / sensor
 * - assumption: not used in lockscreen
 * - backlight is not enabled in lockscreen
 *
 * FLIP:
 * - uses (filtered) lid switch / sensor
 * - assumption: used for lockscreen navigation
 * - backlight is enabled in lockscreen
 */
static keypad_type_t keypad_type = KEYPAD_TYPE_SLIDE;

/** Backlight feature enabled in settings */
static gboolean keypad_backlight_enabled    = MCE_DEFAULT_KEYPADBACKLIGHT_ENABLED;
static guint    keypad_backlight_enabled_id = 0;

/** Backlight policy state */
static bool keypad_backlight_allowed = false;

/** Backlight physical state */
static bool keypad_backlight_active = false;

/** Policy override from e.g CSD test case */
static gint keypad_backlight_mode = MCE_KEYPAD_BACKLIGHT_MODE_POLICY;

/** Flag for: turn backlight on when charging (while keypad is closed) */
static bool keypad_enable_backlight_when_charging = DEFAULT_KEYPAD_ENABLE_BACKLIGHT_WHEN_CHARGING;

/** Timer id for turning backlight off */
static guint keypad_backlight_timeout_id = 0;

/** Configured delay for turning backlight off */
static gint keypad_backlight_timeout_seconds = DEFAULT_KEYPAD_BACKLIGHT_TIMEOUT;

/** Flag for: brightness control is binary
 *
 * Brightness control uses light sensor and that logic does not work when
 * value range is mere 0 and 1 -> In case of binary control we need to use
 * artificial range for ALS processing and then clamp the result to 0 or 1.
 */
static bool keypad_binary_brightness   = false;

/** Maximum backlight brightness, hw specific */
static gint keypad_max_brightness = DEFAULT_KEYPAD_BACKLIGHT_LEVEL;

/** File used to get maximum display brightness */
static gchar *keypad_max_brightness_path = NULL;

/** File used to set backlight brightness */
static output_state_t keypad_brightness_file =
{
    .path          = NULL,
    .context       = "keypad_backlight_brightness",
    .truncate_file = TRUE,
    .close_on_exit = FALSE,
};

/** List of monitored mode dbus clients */
static GSList *keypad_dbus_mode_clients = NULL;

/* Locally cached datapipe values */
static system_state_t   system_state         = MCE_SYSTEM_STATE_UNDEF;
static display_state_t  display_state_curr   = MCE_DISPLAY_UNDEF;
static submode_t        submode              = MCE_SUBMODE_INVALID;
static bool             interaction_expected = false;
static bool             device_inactive      = true;
static cover_state_t    keyboard_slide_state = COVER_UNDEF;
static cover_state_t    lid_sensor_filtered  = COVER_UNDEF;
static alarm_ui_state_t alarm_ui_state       = MCE_ALARM_UI_INVALID_INT32;
static call_state_t     call_state           = CALL_STATE_INVALID;
static battery_state_t  battery_state        = BATTERY_STATE_UNKNOWN;

/* ========================================================================= *
 * Code
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * UTILITY
 * ------------------------------------------------------------------------- */

static const char *
bool_repr(int value)
{
    return value < 0 ? "unset" : value ? "true" : "false";
}

static const char *
mode_repr(int value)
{
    const char *repr = "unknown";

    switch( value ) {
    case MCE_KEYPAD_BACKLIGHT_MODE_OFF:
        repr = "off";
        break;
    case MCE_KEYPAD_BACKLIGHT_MODE_ON:
        repr = "on";
        break;
    case MCE_KEYPAD_BACKLIGHT_MODE_POLICY:
        repr = "policy";
        break;
    default:
        break;
    }

    return repr;
}

/* ------------------------------------------------------------------------- *
 * KEYPAD_TYPE
 * ------------------------------------------------------------------------- */

static const char *
keypad_type_repr(keypad_type_t value)
{
    static const char * const lut[KEYPAD_TYPE_COUNT] = {
        [KEYPAD_TYPE_SLIDE] = "SLIDE",
        [KEYPAD_TYPE_FLIP]  = "FLIP",
    };
    const char *repr = (size_t)value < G_N_ELEMENTS(lut) ? lut[value] : NULL;
    return repr ?: "UNKNOWN";
}

static keypad_type_t
keypad_type_parse(const char *name)
{
    if( !name )
        return KEYPAD_TYPE_SLIDE;

    if( !strcasecmp(name, "FLIP") )
        return KEYPAD_TYPE_FLIP;

    if( strcasecmp(name, "SLIDE") )
        mce_log(LL_WARN, "unknown keypad type '%s'", name);

    return KEYPAD_TYPE_SLIDE;
}

/* ------------------------------------------------------------------------- *
 * KEYPAD
 * ------------------------------------------------------------------------- */

static void
keypad_set_mode(int backlight_mode)
{
    /* Normalize potentially received over D-Bus value */
    switch( backlight_mode ) {
    case MCE_KEYPAD_BACKLIGHT_MODE_OFF:
    case MCE_KEYPAD_BACKLIGHT_MODE_ON:
    case MCE_KEYPAD_BACKLIGHT_MODE_POLICY:
        break;
    default:
        backlight_mode = MCE_KEYPAD_BACKLIGHT_MODE_POLICY;
        break;
    }

    if( keypad_backlight_mode != backlight_mode ) {
        mce_log(LL_DEBUG, "backlight_mode: %s -> %s",
                mode_repr(keypad_backlight_mode),
                mode_repr(backlight_mode));

        keypad_backlight_mode = backlight_mode;

        keypad_cancel_backlight_timeout();
        if( keypad_rethink_backlight_policy() )
            keypad_setup_backlight_timeout();
    }
}

/** Key backlight brightness
 *
 * @param brightness Backlight brightness
 */
static void
keypad_set_brightness(guint brightness)
{
    if( keypad_brightness_file.path ) {
        if( !mce_write_number_string_to_file(&keypad_brightness_file, brightness) )
            mce_log(LL_ERR, "failed to write '%u' to %s", brightness, keypad_brightness_file.path);
        else
            keypad_backlight_active = brightness > 0;
    }
}

/** Turn backlight on
 */
static void
keypad_set_backlight_on(void)
{
    keypad_setup_backlight_timeout();

    if( datapipe_get_guint(key_backlight_brightness_pipe) == 0 )
        datapipe_exec_full(&key_backlight_brightness_pipe, GINT_TO_POINTER(keypad_max_brightness));
}

/** Turn backlight off
 */
static void
keypad_set_backlight_off(void)
{
    keypad_cancel_backlight_timeout();

    if( datapipe_get_guint(key_backlight_brightness_pipe) != 0 )
        datapipe_exec_full(&key_backlight_brightness_pipe, GINT_TO_POINTER(0));
}

/** Timeout callback for key backlight
 *
 * @param data Unused
 *
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean
keypad_backlight_timeout_cb(gpointer data)
{
    (void)data;

    mce_log(LL_DEBUG, "backlight timeout: triggered");

    keypad_backlight_timeout_id = 0;

    keypad_set_backlight_off();

    return G_SOURCE_REMOVE;
}

/** Setup key backlight timeout
 */
static void
keypad_setup_backlight_timeout(void)
{
    if( !keypad_backlight_timeout_id && keypad_backlight_mode == MCE_KEYPAD_BACKLIGHT_MODE_POLICY ) {
        mce_log(LL_DEBUG, "backlight timeout: scheduled");
        keypad_backlight_timeout_id = g_timeout_add_seconds(keypad_backlight_timeout_seconds,
                                                            keypad_backlight_timeout_cb, NULL);
    }
}

/** Cancel key backlight timeout
 */
static void
keypad_cancel_backlight_timeout(void)
{
    if( keypad_backlight_timeout_id ) {
        mce_log(LL_DEBUG, "backlight timeout: canceled");
        g_source_remove(keypad_backlight_timeout_id), keypad_backlight_timeout_id = 0;
    }
}

/** Policy based enabling of key backlight
 */
static bool
keypad_rethink_backlight_policy(void)
{
    /* Base policy: allow when in user mode, display is on, and ...
     */
    bool allow = true;
    bool closed = true;
    bool tklock = submode & MCE_SUBMODE_TKLOCK;

    if( system_state != MCE_SYSTEM_STATE_USER )
        allow = false;

    if( display_state_curr != MCE_DISPLAY_ON )
        allow = false;

    if( keypad_type == KEYPAD_TYPE_FLIP ) {
        if( lid_sensor_filtered == COVER_CLOSED )
            allow = false;
        else
            closed = false;
    }
    else {
        if( keyboard_slide_state != COVER_OPEN )
            allow = false;
        else
            closed = false;

        if( tklock && !interaction_expected )
            allow = false;
    }

    if( !tklock && device_inactive )
        allow = false;

    /* Policy exceptions: alarms, calls, ...
     */
    if( !closed ) {
        if( alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32 || alarm_ui_state == MCE_ALARM_UI_RINGING_INT32 )
            allow = true;

        if( call_state == CALL_STATE_RINGING || call_state == CALL_STATE_ACTIVE )
            allow = true;
    }

    if( closed && keypad_enable_backlight_when_charging )
        if( battery_state == BATTERY_STATE_CHARGING || battery_state == BATTERY_STATE_FULL)
            allow = true;

    /* User settings
     */
    if( !keypad_backlight_enabled )
        allow = false;

    /* Policy modes: CSD, ...
     */
    switch( keypad_backlight_mode ) {
    case MCE_KEYPAD_BACKLIGHT_MODE_OFF:
        allow = false;
        break;
    case MCE_KEYPAD_BACKLIGHT_MODE_ON:
        allow = true;
        break;
    default:
    case MCE_KEYPAD_BACKLIGHT_MODE_POLICY:
        break;
    }

    /* Execute policy decision
     */
    if( keypad_backlight_allowed != allow ) {
        mce_log(LL_DEBUG, "backlight_allowed: %s -> %s", bool_repr(keypad_backlight_allowed), bool_repr(allow));

        if( (keypad_backlight_allowed = allow) )
            keypad_set_backlight_on();
        else
            keypad_set_backlight_off();
    }

    return keypad_backlight_allowed;
}

/* ------------------------------------------------------------------------- *
 * KEYPAD_DATAPIPE
 * ------------------------------------------------------------------------- */

/** Set key backlight brightness
 *
 * @param data Backlight brightness passed as a gconstpointer
 */
static void
keypad_datapipe_key_backlight_brightness_cb(gconstpointer data)
{
    static gint backlight_input  = -1;
    static gint backlight_output = -1;

    gint prev_input = backlight_input;

    backlight_input = GPOINTER_TO_INT(data);

    if( prev_input != backlight_input ) {
        mce_log(LL_DEBUG, "backlight_input: %d -> %d", prev_input, backlight_input);

        gint prev_output = backlight_output;

        if( backlight_input <= 0 )
            backlight_output = 0;
        else if( keypad_binary_brightness )
            backlight_output = 1;
        else
            backlight_output = backlight_input;

        if( prev_output != backlight_output ) {
            mce_log(LL_DEBUG, "backlight_output: %d -> %d", prev_output, backlight_output);
            keypad_set_brightness(backlight_output);
        }
    }
}

/** Handle system state change
 *
 * @param data system_state_pipe status as a pointer
 */
static void
keypad_datapipe_system_state_cb(gconstpointer data)
{
    system_state_t prev = system_state;

    system_state = GPOINTER_TO_INT(data);

    if( prev != system_state ) {
        mce_log(LL_DEBUG, "system_state: %s -> %s", system_state_repr(prev), system_state_repr(system_state));
        keypad_rethink_backlight_policy();
    }
}

/** Datapipe trigger for display state
 *
 * @param data display_state_curr_pipe status as a pointer
 */
static void
keypad_datapipe_display_state_curr_cb(gconstpointer data)
{
    display_state_t prev = display_state_curr;

    display_state_curr = GPOINTER_TO_INT(data);

    if( prev != display_state_curr ) {
        mce_log(LL_DEBUG, "display_state_curr: %s -> %s", display_state_repr(prev), display_state_repr(display_state_curr));
        keypad_rethink_backlight_policy();
    }
}

/** Datapipe trigger for the submode
 *
 * @param data submode_pipe status as a pointer
 */
static void
keypad_datapipe_submode_cb(gconstpointer const data)
{
    submode_t prev = submode;

    submode = GPOINTER_TO_INT(data);

    if( prev != submode ) {
        mce_log(LL_DEBUG, "submode: %s", submode_change_repr(prev, submode));
        keypad_rethink_backlight_policy();
    }
}

/** Datapipe trigger for interaction_expected
 *
 * @param data interaction_expected_pipe status as a pointer
 */
static void
keypad_datapipe_interaction_expected_cb(gconstpointer const data)
{
    bool prev = interaction_expected;

    interaction_expected = GPOINTER_TO_INT(data);

    if( prev != interaction_expected ) {
        mce_log(LL_DEBUG, "interaction_expected: %s -> %s", bool_repr(prev), bool_repr(interaction_expected));
        keypad_rethink_backlight_policy();
    }
}

/** Datapipe trigger for device inactivity
 *
 * @param data device_inactive_pipe status as a pointer
 */
static void
keypad_datapipe_device_inactive_cb(gconstpointer const data)
{
    bool prev = device_inactive;

    device_inactive = GPOINTER_TO_INT(data);

    if( prev != device_inactive ) {
        /* Level triggeer: Inactivity state change */
        mce_log(LL_DEBUG, "device_inactive: %s -> %s", bool_repr(prev), bool_repr(device_inactive));
        keypad_rethink_backlight_policy();
    }
    else if( !device_inactive ) {
        /** Edge trigger: user activity event */
        mce_log(LL_DEBUG, "user activity");
        if( keypad_rethink_backlight_policy() ) {
            keypad_cancel_backlight_timeout();
            keypad_set_backlight_on();
        }
    }
}

/** Datapipe trigger for the keyboard slide
 *
 * @param data keyboard_slide_state_pipe status as a pointer
 */

static void
keypad_datapipe_keyboard_slide_state_cb(gconstpointer const data)
{
    cover_state_t prev = keyboard_slide_state;

    keyboard_slide_state = GPOINTER_TO_INT(data);

    if( prev != keyboard_slide_state ) {
        mce_log(LL_DEBUG, "keyboard_slide_state: %s -> %s", cover_state_repr(prev), cover_state_repr(keyboard_slide_state));
        keypad_rethink_backlight_policy();
    }
}

/** Datapipe trigger for the filtered lid sensor state
 *
 * @param data lid_sensor_filtered_pipe status as a pointer
 */
static void
keypad_datapipe_lid_sensor_filtered_cb(gconstpointer const data)
{
    cover_state_t prev = lid_sensor_filtered;

    lid_sensor_filtered = GPOINTER_TO_INT(data);

    if( prev != lid_sensor_filtered ) {
        mce_log(LL_DEBUG, "lid_sensor_filtered: %s -> %s", cover_state_repr(prev), cover_state_repr(lid_sensor_filtered));
        keypad_rethink_backlight_policy();
    }
}

/** Datapipe trigger for alarm ui state
 *
 * @param data alarm_ui_state_pipe status as a pointer
 */
static void
keypad_datapipe_alarm_ui_state_cb(gconstpointer data)
{
    alarm_ui_state_t prev = alarm_ui_state;

    alarm_ui_state = GPOINTER_TO_INT(data);

    if( prev != alarm_ui_state ) {
        mce_log(LL_DEBUG, "alarm_ui_state: %s -> %s", alarm_state_repr(prev), alarm_state_repr(alarm_ui_state));
        keypad_rethink_backlight_policy();
    }
}

/** Datapipe trigger for the call state
 *
 * @param data call_state_pipe status as a pointer
 */
static void
keypad_datapipe_call_state_cb(gconstpointer const data)
{
    call_state_t prev = call_state;

    call_state = GPOINTER_TO_INT(data);

    if( prev != call_state ) {
        mce_log(LL_DEBUG, "call_state: %s -> %s", call_state_repr(prev), call_state_repr(call_state));
        keypad_rethink_backlight_policy();
    }
}

/** Handle battery state change
 *
 * @param data battery_state_pipe status as a pointer
 */
static void
keypad_datapipe_battery_state_cb(gconstpointer data)
{
    battery_state_t prev = battery_state;

    battery_state = GPOINTER_TO_INT(data);

    if( prev != battery_state ) {
        mce_log(LL_DEBUG, "battery_state: %s -> %s", battery_state_repr(prev), battery_state_repr(battery_state));
        keypad_rethink_backlight_policy();
    }
}

/** Array of datapipe handlers */
static datapipe_handler_t mce_keypad_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &key_backlight_brightness_pipe,
        .output_cb = keypad_datapipe_key_backlight_brightness_cb,
    },
    {
        .datapipe  = &system_state_pipe,
        .output_cb = keypad_datapipe_system_state_cb,
    },
    {
        .datapipe  = &display_state_curr_pipe,
        .output_cb = keypad_datapipe_display_state_curr_cb,
    },
    {
        .datapipe  = &submode_pipe,
        .output_cb = keypad_datapipe_submode_cb,
    },
    {
        .datapipe  = &interaction_expected_pipe,
        .output_cb = keypad_datapipe_interaction_expected_cb,
    },
    {
        .datapipe  = &device_inactive_pipe,
        .output_cb = keypad_datapipe_device_inactive_cb,
    },
    {
        .datapipe  = &keyboard_slide_state_pipe,
        .output_cb = keypad_datapipe_keyboard_slide_state_cb,
    },
    {
        .datapipe  = &lid_sensor_filtered_pipe,
        .output_cb = keypad_datapipe_lid_sensor_filtered_cb,
    },
    {
        .datapipe  = &alarm_ui_state_pipe,
        .output_cb = keypad_datapipe_alarm_ui_state_cb,
    },
    {
        .datapipe  = &call_state_pipe,
        .output_cb = keypad_datapipe_call_state_cb,
    },
    {
        .datapipe  = &battery_state_pipe,
        .output_cb = keypad_datapipe_battery_state_cb,
    },
    // sentinel
    {
        .datapipe = NULL,
    }
};

static datapipe_bindings_t mce_keypad_datapipe_bindings =
{
    .module   = MODULE_NAME,
    .handlers = mce_keypad_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void
keypad_datapipe_init(void)
{
    mce_datapipe_init_bindings(&mce_keypad_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void
keypad_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&mce_keypad_datapipe_bindings);
}

/* ------------------------------------------------------------------------- *
 * KEYPAD_DBUS
 * ------------------------------------------------------------------------- */

/** External mode client exit callback
 *
 * If a process that has set external mode drops from SystemBus,
 * this gets treated as if they would have requested mode removal.
 *
 * @param sig NameOwnerChanged D-Bus signal
 *
 * @return TRUE
 */
static gboolean
keypad_dbus_mode_client_exit_cb(DBusMessage *const sig)
{
    DBusError   error     = DBUS_ERROR_INIT;
    const char *dbus_name = NULL;
    const char *old_owner = NULL;
    const char *new_owner = NULL;

    if( !dbus_message_get_args(sig, &error,
                               DBUS_TYPE_STRING, &dbus_name,
                               DBUS_TYPE_STRING, &old_owner,
                               DBUS_TYPE_STRING, &new_owner,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to parse NameOwnerChanged: %s: %s", error.name, error.message);
        goto EXIT;
    }

    if( !*new_owner )
        keypad_dbus_remove_mode_client(dbus_name);

EXIT:
    dbus_error_free(&error);
    return true;
}

/** Register external mode client
 *
 * @param dbus_name          Client private name
 * @param backlight_mode  Override requested by the client
 *
 * @return true if mode was accepted, false otherwise
 */
static bool
keypad_dbus_add_mode_client(const char *dbus_name, int backlight_mode)
{
    bool added = false;

    gssize rc = mce_dbus_owner_monitor_add(dbus_name, keypad_dbus_mode_client_exit_cb,
                                           &keypad_dbus_mode_clients, KEYPAD_MAX_MODE_CLIENTS);
    if( rc < 0 ) {
        mce_log(LL_WARN, "client %s ignored; KEYPAD_MAX_MODE_CLIENTS exceeded", dbus_name);
        goto EXIT;
    }

    if( rc > 0 )
        mce_log(LL_DEBUG, "mode client %s added for tracking", dbus_name);
    else
        mce_log(LL_DEBUG, "mode client %s already tracked", dbus_name);

    keypad_set_mode(backlight_mode);

    added = true;

EXIT:
    return added;
}

/** Unregister external mode client
 *
 * @param dbus_name Client private name
 */
static void
keypad_dbus_remove_mode_client(const char *dbus_name)
{
    gssize rc = mce_dbus_owner_monitor_remove(dbus_name, &keypad_dbus_mode_clients);

    if( rc < 0 )
        mce_log(LL_WARN, "client %s ignored; is not tracked",dbus_name);
    else
        mce_log(LL_DEBUG, "mode client %s removed from tracking", dbus_name);

    if( rc == 0 )
        keypad_set_mode(-1);
}

/** Unregister all external mode clients
 */
static void
keypad_dbus_remove_all_mode_clients(void)
{
    mce_dbus_owner_monitor_remove_all(&keypad_dbus_mode_clients);
    keypad_set_mode(-1);
}

/** Send a key backlight state reply
 *
 * @param method_call A DBusMessage to reply to
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean
keypad_dbus_send_key_backlight_state(DBusMessage *const method_call)
{
    DBusMessage *msg    = NULL;
    dbus_bool_t  state  = keypad_backlight_active;
    gboolean     status = FALSE;

    mce_log(LL_DEBUG, "Sending key backlight state: %d", state);

    msg = dbus_new_method_reply(method_call);

    /* Append the display status */
    if( dbus_message_append_args(msg, DBUS_TYPE_BOOLEAN, &state, DBUS_TYPE_INVALID) == FALSE ) {
        mce_log(LL_CRIT, "Failed to append reply argument to D-Bus message for %s.%s",
                MCE_REQUEST_IF, MCE_KEY_BACKLIGHT_STATE_GET);
        dbus_message_unref(msg);
        goto EXIT;
    }

    /* Send the message */
    status = dbus_send_message(msg);

EXIT:
    return status;
}

/** D-Bus callback for the get key backlight state method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean
keypad_dbus_get_key_backlight_state_cb(DBusMessage *const req)
{
    gboolean status = FALSE;

    mce_log(LL_DEVEL, "Received key backlight state get request from %s",
            mce_dbus_get_message_sender_ident(req));

    /* Try to send a reply that contains the current key backlight state */
    if( keypad_dbus_send_key_backlight_state(req) == FALSE )
        goto EXIT;

    status = TRUE;

EXIT:
    return status;
}

/** D-Bus callback for the set key backlight state method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean
keypad_dbus_set_key_backlight_state_cb(DBusMessage *const req)
{
    mce_log(LL_DEVEL, "Received key backlight state set request from %s", mce_dbus_get_message_sender_ident(req));

    const char  *cli = dbus_message_get_sender(req);
    dbus_int32_t arg = -1;
    DBusError    err = DBUS_ERROR_INIT;
    DBusMessage *rsp = NULL;

    if( !dbus_message_get_args(req, &err, DBUS_TYPE_INT32, &arg, DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "%s: %s: %s",  dbus_message_get_member(req), err.name, err.message);
        rsp = dbus_message_new_error(req, err.name, err.message);
    }
    else if( arg == -1 ) {
        keypad_dbus_remove_mode_client(cli);
    }
    else if( !keypad_dbus_add_mode_client(cli, arg) ) {
        rsp = dbus_message_new_error(req, DBUS_ERROR_LIMITS_EXCEEDED, "KEYPAD_MAX_MODE_CLIENTS exceeded");
    }

    if( !dbus_message_get_no_reply(req) ) {
        if( !rsp )
            rsp = dbus_message_new_method_return(req);
        if( rsp )
            dbus_send_message(rsp), rsp = NULL;
    }

    if( rsp )
        dbus_message_unref(rsp);

    return true;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t keypad_dbus_handlers[] =
{
    /* method calls */
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_KEY_BACKLIGHT_STATE_GET,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = keypad_dbus_get_key_backlight_state_cb,
        .args      =
            "    <arg direction=\"out\" name=\"backlight_state\" type=\"b\"/>\n"
    },
    {
        .interface = MCE_REQUEST_IF,
        .name      = MCE_KEYPAD_BACKLIGHT_MODE_REQ,
        .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
        .callback  = keypad_dbus_set_key_backlight_state_cb,
        .args      =
            "    <arg direction=\"in\" name=\"backlight_state\" type=\"i\"/>\n"
    },
    /* sentinel */
    {
        .interface = NULL
    }
};

/** Add dbus handlers
 */
static void
keypad_dbus_init(void)
{
    mce_dbus_handler_register_array(keypad_dbus_handlers);
}

/** Remove dbus handlers
 */
static void
keypad_dbus_quit(void)
{
    mce_dbus_handler_unregister_array(keypad_dbus_handlers);

    keypad_dbus_remove_all_mode_clients();
}

/* ========================================================================= *
 * KEYPAD_SETTINGS
 * ========================================================================= */

/** Setting changed callback
 *
 * @param gcc   Unused
 * @param id    Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data  Unused
 */
static void
keypad_settings_change_cb(GConfClient *const gcc, const guint id,
                          GConfEntry *const entry, gpointer const data)
{
    (void)gcc;
    (void)data;

    const GConfValue *gcv = gconf_entry_get_value(entry);

    if( !gcv ) {
        mce_log(LL_DEBUG, "GConf Key `%s' has been unset",
                gconf_entry_get_key(entry));
    }
    else if( id == keypad_backlight_enabled_id ) {
        gboolean enabled = gconf_value_get_bool(gcv);
        if( keypad_backlight_enabled != enabled ) {
            mce_log(LL_NOTICE, "backlight_enabled: %s -> %s", bool_repr(keypad_backlight_enabled), bool_repr(enabled));
            keypad_backlight_enabled = enabled;

            keypad_cancel_backlight_timeout();
            if( keypad_rethink_backlight_policy() )
                keypad_setup_backlight_timeout();
        }
    }
    else {
        mce_log(LL_WARN, "Spurious GConf value received; confused!");
    }
}

/** Get intial setting values and start tracking changes
 */
static void
keypad_settings_init(void)
{
    mce_setting_track_bool(MCE_SETTING_KEYPADBACKLIGHT_ENABLED,
                           &keypad_backlight_enabled,
                           MCE_DEFAULT_KEYPADBACKLIGHT_ENABLED,
                           keypad_settings_change_cb,
                           &keypad_backlight_enabled_id);

    mce_log(LL_DEBUG, "backlight enabled = %s", bool_repr(keypad_backlight_enabled));
}

/** Stop tracking setting changes
 */
static void
keypad_settings_quit(void)
{
    mce_setting_notifier_remove(keypad_backlight_enabled_id), keypad_backlight_enabled_id = 0;
}

/* ------------------------------------------------------------------------- *
 * KEYPAD_CONFIG
 * ------------------------------------------------------------------------- */

/** Check for configured on/off toggle file
 *
 * @return true if found, false otherwise
 */
static bool
keypad_config_probe_binary_control(void)
{
    bool found = false;

    gsize   cnt = 0;
    gchar **arr = mce_conf_get_string_list(MCE_CONF_KEYPAD_GROUP, MCE_CONF_KEYPAD_BACKLIGHT_BRIGHTNESS_FILE, &cnt);
    for( gsize i = 0; i < cnt; ++i ) {
        const char *path = arr[i];

        if( *path && access(path, W_OK) == 0 ) {
            keypad_brightness_file.path = g_strdup(path);
            keypad_binary_brightness    = true;
            keypad_max_brightness       = DEFAULT_KEYPAD_BACKLIGHT_LEVEL;

            found = true;
            break;
        }
    }
    g_strfreev(arr);

    return found;
}

/** Check for configured brightness and max_brightness files
 *
 * @return true if found, false otherwise
 */
static bool
keypad_config_probe_vanilla_control(void)
{
    bool found = false;

    /* Check if we have a configured brightness directory
     * that a) exists and b) contains both brightness and
     * max_brightness files */

    gsize   cnt = 0;
    gchar **arr = mce_conf_get_string_list(MCE_CONF_KEYPAD_GROUP, MCE_CONF_KEYPAD_BACKLIGHT_BRIGHTNESS_DIR, &cnt);
    for( size_t i = 0; i < cnt; ++i ) {
        const char *path = arr[i];
        if( !*path || access(path, F_OK) == -1 )
            continue;

        g_autofree gchar *set = g_strdup_printf("%s/brightness", path);
        g_autofree gchar *max = g_strdup_printf("%s/max_brightness", path);

        if( access(set, W_OK) == 0 && access(max, R_OK) == 0 ) {
            gulong tmp = 0;
            if( mce_read_number_string_from_file(max, &tmp, NULL, false, true) ) {
                keypad_brightness_file.path = set, set = NULL;
                keypad_max_brightness_path  = max, max = NULL;
                keypad_binary_brightness    = false;
                keypad_max_brightness       = (gint)tmp;

                found = true;
                break;
            }
        }
    }
    g_strfreev(arr);

    return found;
}

/** Initialize static configuration related variables
 */
static void
keypad_config_init(void)
{
    g_autofree gchar *type_setting = mce_conf_get_string(MCE_CONF_KEYPAD_GROUP, MCE_CONF_KEYPAD_TYPE, NULL);

    keypad_type = keypad_type_parse(type_setting);

    keypad_backlight_timeout_seconds =
        mce_conf_get_int(MCE_CONF_KEYPAD_GROUP, MCE_CONF_KEYPAD_BACKLIGHT_TIMEOUT, DEFAULT_KEYPAD_BACKLIGHT_TIMEOUT);

    keypad_enable_backlight_when_charging =
        mce_conf_get_bool(MCE_CONF_KEYPAD_GROUP, MCE_CONF_KEYPAD_ENABLE_BACKLIGHT_WHEN_CHARGING,
                          DEFAULT_KEYPAD_ENABLE_BACKLIGHT_WHEN_CHARGING);

    if( !keypad_config_probe_vanilla_control() && !keypad_config_probe_binary_control() )
        mce_log(LL_DEBUG, "keypad backlight controls not found");

    mce_log(LL_DEBUG, "keypad type = %s", keypad_type_repr(keypad_type));
    mce_log(LL_DEBUG, "brightness file = %s", keypad_brightness_file.path ?: "none");
    mce_log(LL_DEBUG, "max brightness file = %s",  keypad_max_brightness_path?: "none");
    mce_log(LL_DEBUG, "brightness range = 0 ... %d", keypad_binary_brightness ? 1 : keypad_max_brightness);
    mce_log(LL_DEBUG, "enable backlight when charging = %s", bool_repr(keypad_enable_backlight_when_charging));
    mce_log(LL_DEBUG, "backlight timeout = %d s", keypad_backlight_timeout_seconds);
}

/** Cleanup static configuration related variables
 */
static void
keypad_config_quit(void)
{
    keypad_set_brightness(0);

    g_free((void *)keypad_brightness_file.path), keypad_brightness_file.path = NULL;

    g_free(keypad_max_brightness_path), keypad_max_brightness_path = NULL;
}

/* ------------------------------------------------------------------------- *
 * G_MODULE
 * ------------------------------------------------------------------------- */

/** Init function for the keypad module
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 *
 * @return NULL on success, a string with an error message on failure
 */

G_MODULE_EXPORT const gchar *
g_module_check_init(GModule *module)
{
    (void)module;

    gchar *status = NULL;

    keypad_config_init();
    keypad_settings_init();
    keypad_datapipe_init();
    keypad_dbus_init();

    return status;
}

/** Exit function for the keypad module
 *
 * @param module Unused
 */

G_MODULE_EXPORT void
g_module_unload(GModule *module)
{
    (void)module;

    keypad_dbus_quit();
    keypad_datapipe_quit();
    keypad_settings_quit();
    keypad_config_quit();

    /* Do not leave timers behind */
    keypad_cancel_backlight_timeout();
}
