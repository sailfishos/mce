/** @file mcetool.c
 * Tool to test and remote control the Mode Control Entity
 * <p>
 * Copyright (c) 2005 - 2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (c) 2012 - 2022 Jolla Ltd.
 * Copyright (c) 2019 - 2020 Open Mobile Platform LLC.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Victor Portnov <ext-victor.portnov@nokia.com>
 * @author Philippe De Swert <philippe.deswert@jollamobile.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * @author Vesa Halttunen <vesa.halttunen@jollamobile.com>
 * @author Kimmo Lindholm <kimmo.lindholm@eke.fi>
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

#include "../mce-command-line.h"
#include "../mce-dbus.h"
#include "../mce-setting.h"
#include "../tklock.h"
#include "../powerkey.h"
#include "../event-input.h"
#include "../evdev.h"
#include "../modules/display.h"
#include "../modules/doubletap.h"
#include "../modules/inactivity.h"
#include "../modules/powersavemode.h"
#include "../modules/proximity.h"
#include "../modules/memnotify.h"
#include "../modules/led.h"
#include "../modules/charging.h"
#include "../systemui/dbus-names.h"
#include "../systemui/tklock-dbus-names.h"

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <syslog.h>

#include <dbus/dbus.h>

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

/** Ram page size, used for human readable memory limits */
#define PAGE_SIZE ((unsigned long)sysconf(_SC_PAGESIZE))

/** Whether to enable development time debugging */
#define MCETOOL_ENABLE_EXTRA_DEBUG 0

/** Name shown by --help etc. */
#define PROG_NAME "mcetool"

/** Default padding for left column of status reports */
#define PAD1 "36"

/** Padding used for radio state bits */
#define PAD2 "28"

#if MCETOOL_ENABLE_EXTRA_DEBUG
# define debugf(FMT, ARGS...) fprintf(stderr, PROG_NAME": D: "FMT, ##ARGS)
#else
# define debugf(FMT, ARGS...) do { }while(0)
#endif

# define errorf(FMT, ARGS...) fprintf(stderr, PROG_NAME": E: "FMT, ##ARGS)

/* ========================================================================= *
 * Types
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * SYMBOL LOOKUP TABLES
 * ------------------------------------------------------------------------- */

/** Simple string key to integer value symbol */
typedef struct
{
        /** Name of the symbol, or NULL to mark end of symbol table */
        const char *key;

        /** Value of the symbol */
        int         val;
} symbol_t;

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * XDBUS
 * ------------------------------------------------------------------------- */

static DBusConnection  *xdbus_init   (void);
static void             xdbus_exit   (void);
static gboolean         xdbus_call_va(const gchar *const service, const gchar *const path, const gchar *const interface, const gchar *const name, DBusMessage **reply, int arg_type, va_list va);

/* ------------------------------------------------------------------------- *
 * XMCE
 * ------------------------------------------------------------------------- */

static gboolean      xmce_ipc_va                                       (const gchar *const name, DBusMessage **reply, int arg_type, va_list va);
#ifdef ENABLE_BATTERY_SIMULATION
static gboolean      xmce_ipc                                          (const gchar *const name, DBusMessage **reply, int arg_type, ...);
#endif
static gboolean      xmce_ipc_no_reply                                 (const gchar *const name, int arg_type, ...);
static gboolean      xmce_ipc_message_reply                            (const gchar *const name, DBusMessage **reply, int arg_type, ...);
static gboolean      xmce_ipc_string_reply                             (const gchar *const name, char **result, int arg_type, ...);
static gboolean      xmce_ipc_uint_reply                               (const gchar *const name, guint *result, int arg_type, ...);
static gboolean      xmce_ipc_int_reply                                (const gchar *const name, gint *result, int arg_type, ...);
static gboolean      xmce_ipc_bool_reply                               (const gchar *const name, gboolean *result, int arg_type, ...);
static DBusMessage  *xmce_setting_request                              (const gchar *const method);
static gboolean      xmce_setting_get_bool                             (const gchar *const key, gboolean *value);
static gboolean      xmce_setting_get_int                              (const gchar *const key, gint *value);
static gboolean      xmce_setting_get_string                           (const gchar *const key, gchar **value);
static gboolean      xmce_setting_get_int_array                        (const gchar *const key, gint **values, gint *count);
static gboolean      xmce_setting_set_bool                             (const gchar *const key, const gboolean value);
static gboolean      xmce_setting_set_int                              (const gchar *const key, const gint value);
static gboolean      xmce_setting_set_string                           (const gchar *const key, const char *value);
static gboolean      xmce_setting_set_int_array                        (const gchar *const key, const gint *values, gint count);
static int           xmce_parse_powerkeyevent                          (const char *args);
static unsigned      xmce_parse_radio_states                           (const char *args);
static gboolean      xmce_parse_enabled                                (const char *args);
static int           xmce_parse_integer                                (const char *args);
static int           xmce_parse_memory_limit                           (const char *args);
static double        xmce_parse_double                                 (const char *args);
static bool          xmce_set_verbosity                                (const char *arg);
static void          xmce_get_verbosity                                (void);
static bool          xmce_get_color_profile_ids                        (const char *arg);
static bool          xmce_set_color_profile                            (const char *args);
static void          xmce_get_color_profile                            (void);
#ifdef ENABLE_BATTERY_SIMULATION
static bool          xmce_set_charger_type                             (const char *type);
static bool          xmce_set_charger_state                            (const char *state);
static bool          xmce_set_battery_level                            (int level);
#endif
static void          xmce_get_cable_state                              (void);
static void          xmce_get_charger_state                            (void);
static void          xmce_get_battery_status                           (void);
static void          xmce_get_battery_state                            (void);
static void          xmce_get_battery_level                            (void);
static bool          xmce_set_charging_enable_limit                    (const char *args);
static void          xmce_get_charging_enable_limit                    (void);
static bool          xmce_set_charging_disable_limit                   (const char *args);
static void          xmce_get_charging_disable_limit                   (void);
static bool          xmce_set_charging_mode                            (const char *args);
static void          xmce_get_charging_mode                            (void);
static bool          xmce_set_forced_charging_mode                     (const char *args);
static void          xmce_get_forced_charging_mode                     (void);
static void          xmce_get_battery_info                             (void);
static void          xmce_parse_notification_args                      (const char *args, char **title, dbus_int32_t *delay, dbus_int32_t *renew);
static bool          xmce_notification_begin                           (const char *args);
static bool          xmce_notification_end                             (const char *args);
static bool          xmce_enable_radio                                 (const char *args);
static bool          xmce_disable_radio                                (const char *args);
static void          xmce_get_radio_states                             (void);
static bool          xmce_set_lpmui_triggering                         (const char *args);
static void          xmce_get_lpmui_triggering                         (void);
static bool          xmce_set_input_grab_allowed                       (const char *args);
static void          xmce_get_input_grab_allowed                       (void);
static bool          xmce_set_call_state                               (const char *args);
static void          xmce_get_call_state                               (void);
static bool          xmce_set_button_backlligut_off_delay              (const char *args);
static void          xmce_get_button_backlligut_off_delay              (void);
static bool          xmce_set_button_backlight_mode                    (const char *args);
static bool          xmce_set_button_backlight                         (const char *args);
static void          xmce_get_button_backlight                         (void);
static void          xmce_set_display_state                            (const char *state);
static void          xmce_get_display_state                            (void);
static bool          xmce_prevent_display_blanking                     (const char *arg);
static bool          xmce_allow_display_blanking                       (const char *arg);
static bool          xmce_set_blank_prevent_mode                       (const char *args);
static void          xmce_get_blank_prevent_mode                       (void);
static bool          xmce_set_display_brightness                       (const char *args);
static void          xmce_get_display_brightness                       (void);
static bool          xmce_set_dimmed_brightness_static                 (const char *args);
static void          xmce_get_dimmed_brightness_static                 (void);
static bool          xmce_set_dimmed_brightness_dynamic                (const char *args);
static void          xmce_get_dimmed_brightness_dynamic                (void);
static bool          xmce_set_compositor_dimming_hi                    (const char *args);
static bool          xmce_set_compositor_dimming_lo                    (const char *args);
static void          xmce_get_compositor_dimming                       (void);
static bool          xmce_set_cabc_mode                                (const char *args);
static void          xmce_get_cabc_mode                                (void);
static bool          xmce_reset_settings                               (const char *args);
static bool          xmce_set_dim_timeout                              (const char *args);
static void          xmce_get_dim_timeout                              (void);
static bool          xmce_set_dim_with_kbd_timeout                     (const char *args);
static void          xmce_get_dim_with_kbd_timeout                     (void);
static bool          xmce_set_dim_timeouts                             (const char *args);
static void          xmce_get_dim_timeouts                             (void);
static bool          xmce_set_adaptive_dimming_mode                    (const char *args);
static void          xmce_get_adaptive_dimming_mode                    (void);
static bool          xmce_set_adaptive_dimming_time                    (const char *args);
static void          xmce_get_adaptive_dimming_time                    (void);
static bool          xmce_set_exception_length_call_in                 (const char *args);
static bool          xmce_set_exception_length_call_out                (const char *args);
static bool          xmce_set_exception_length_alarm                   (const char *args);
static bool          xmce_set_exception_length_usb_connect             (const char *args);
static bool          xmce_set_exception_length_usb_dialog              (const char *args);
static bool          xmce_set_exception_length_charger                 (const char *args);
static bool          xmce_set_exception_length_battery                 (const char *args);
static bool          xmce_set_exception_length_jack_in                 (const char *args);
static bool          xmce_set_exception_length_jack_out                (const char *args);
static bool          xmce_set_exception_length_camera                  (const char *args);
static bool          xmce_set_exception_length_volume                  (const char *args);
static bool          xmce_set_exception_length_activity                (const char *args);
static void          xmce_get_exception_length                         (const char *tag, const char *key);
static void          xmce_get_exception_lengths                        (void);
static bool          xmce_set_filter_lid_with_als                      (const char *args);
static void          xmce_get_filter_lid_with_als                      (void);
static bool          xmce_set_filter_lid_als_limit                     (const char *args);
static void          xmce_get_filter_lid_als_limit                     (void);
static bool          xmce_set_lid_sensor_mode                          (const char *args);
static void          xmce_get_lid_sensor_mode                          (void);
static bool          xmce_set_lid_open_actions                         (const char *args);
static void          xmce_get_lid_open_actions                         (void);
static bool          xmce_set_lid_close_actions                        (const char *args);
static void          xmce_get_lid_close_actions                        (void);
static bool          xmce_set_kbd_slide_open_trigger                   (const char *args);
static void          xmce_get_kbd_slide_open_trigger                   (void);
static bool          xmce_set_kbd_slide_open_actions                   (const char *args);
static void          xmce_get_kbd_slide_open_actions                   (void);
static bool          xmce_set_kbd_slide_close_trigger                  (const char *args);
static void          xmce_get_kbd_slide_close_trigger                  (void);
static bool          xmce_set_kbd_slide_close_actions                  (const char *args);
static void          xmce_get_kbd_slide_close_actions                  (void);
static bool          xmce_set_orientation_sensor_mode                  (const char *args);
static void          xmce_get_orientation_sensor_mode                  (void);
static bool          xmce_set_orientation_change_is_activity           (const char *args);
static void          xmce_get_orientation_change_is_activity           (void);
static bool          xmce_set_flipover_gesture_detection               (const char *args);
static void          xmce_get_flipover_gesture_detection               (void);
static bool          xmce_set_ps_mode                                  (const char *args);
static void          xmce_get_ps_mode                                  (void);
static bool          xmce_set_ps_on_demand                             (const char *args);
static void          xmce_get_ps_on_demand                             (void);
static bool          xmce_set_ps_blocks_touch                          (const char *args);
static void          xmce_get_ps_blocks_touch                          (void);
static bool          xmce_set_ps_acts_as_lid                           (const char *args);
static void          xmce_get_ps_acts_as_lid                           (void);
static bool          xmce_set_ps_uncover_delay_sub                     (const char *key, const char *args);
static void          xmce_get_ps_uncover_delay_sub                     (const char *tag, const char *key);
static bool          xmce_set_default_ps_uncover_delay                 (const char *args);
static bool          xmce_set_incall_ps_uncover_delay                  (const char *args);
static void          xmce_get_ps_uncover_delay                         (void);
static bool          xmce_set_inactivity_shutdown_delay                (const char *args);
static void          xmce_get_inactivity_shutdown_delay                (void);
static bool          xmce_set_als_autobrightness                       (const char *args);
static void          xmce_get_als_autobrightness                       (void);
static bool          xmce_set_als_mode                                 (const char *args);
static void          xmce_get_als_mode                                 (void);
static bool          xmce_is_als_filter_name                           (const char *name);
static bool          xmce_set_als_input_filter                         (const char *args);
static void          xmce_get_als_input_filter                         (void);
static bool          xmce_set_als_sample_time                          (const char *args);
static void          xmce_get_als_sample_time                          (void);
static bool          xmce_set_autolock_mode                            (const char *args);
static void          xmce_get_autolock_mode                            (void);
static bool          xmce_set_autolock_delay                           (const char *args);
static void          xmce_get_autolock_delay                           (void);
static bool          xmce_set_devicelock_in_lockscreen                 (const char *args);
static void          xmce_get_devicelock_in_lockscreen                 (void);
static bool          xmce_set_lockscreen_unblank_animation             (const char *args);
static void          xmce_get_lockscreen_unblank_animation             (void);
static bool          xmce_set_blank_timeout                            (const char *args);
static bool          xmce_set_blank_from_lockscreen_timeout            (const char *args);
static bool          xmce_set_blank_from_lpm_on_timeout                (const char *args);
static bool          xmce_set_blank_from_lpm_off_timeout               (const char *args);
static void          xmce_get_blank_timeout_sub                        (const char *tag, const char *key);
static void          xmce_get_blank_timeout                            (void);
static bool          xmce_powerkey_event                               (const char *args);
static bool          xmce_set_powerkey_action                          (const char *args);
static void          xmce_get_powerkey_action                          (void);
static bool          xmce_set_powerkey_blanking                        (const char *args);
static void          xmce_get_powerkey_blanking                        (void);
static bool          xmce_set_powerkey_long_press_delay                (const char *args);
static void          xmce_get_powerkey_long_press_delay                (void);
static bool          xmce_set_powerkey_double_press_delay              (const char *args);
static void          xmce_get_powerkey_double_press_delay              (void);
static bool          xmce_is_powerkey_action                           (const char *name);
static bool          xmce_is_powerkey_action_mask                      (const char *names);
static bool          xmce_set_powerkey_action_mask                     (const char *key, const char *names);
static bool          xmce_set_powerkey_actions_while_display_off_single(const char *args);
static bool          xmce_set_powerkey_actions_while_display_off_double(const char *args);
static bool          xmce_set_powerkey_actions_while_display_off_long  (const char *args);
static bool          xmce_set_powerkey_actions_while_display_on_single (const char *args);
static bool          xmce_set_powerkey_actions_while_display_on_double (const char *args);
static bool          xmce_set_powerkey_actions_while_display_on_long   (const char *args);
static bool          xmce_set_touchscreen_gesture_action               (const char *args);
static void          xmce_get_powerkey_action_mask                     (const char *key, const char *tag);
static void          xmce_get_powerkey_action_masks                    (void);
static bool          xmce_is_powerkey_dbus_action                      (const char *conf);
static bool          xmce_set_powerkey_dbus_action                     (const char *args);
static void          xmce_get_powerkey_dbus_action                     (size_t action_id);
static void          xmce_get_powerkey_dbus_actions                    (void);
static bool          xmce_set_ps_override_count                        (const char *args);
static void          xmce_get_ps_override_count                        (void);
static bool          xmce_set_ps_override_timeout                      (const char *args);
static void          xmce_get_ps_override_timeout                      (void);
static bool          xmce_set_display_off_override                     (const char *args);
static void          xmce_get_display_off_override                     (void);
static bool          xmce_set_volkey_policy                            (const char *args);
static void          xmce_get_volkey_policy                            (void);
static bool          xmce_set_doubletap_mode                           (const char *args);
static bool          xmce_set_doubletap_wakeup                         (const char *args);
static void          xmce_get_doubletap_wakeup                         (void);
static bool          xmce_set_fingerprint_wakeup_mode                  (const char *args);
static void          xmce_get_fingerprint_wakeup_mode                  (void);
static bool          xmce_set_fingerprint_wakeup_allow_delay           (const char *args);
static void          xmce_get_fingerprint_wakeup_allow_delay           (void);
static bool          xmce_set_fingerprint_wakeup_trigger_delay         (const char *args);
static void          xmce_get_fingerprint_wakeup_trigger_delay         (void);
static bool          xmce_set_fingerprint_wakeup_throttle_delay        (const char *args);
static void          xmce_get_fingerprint_wakeup_throttle_delay        (void);
static bool          xmce_set_power_saving_mode                        (const char *args);
static void          xmce_get_power_saving_mode                        (void);
static bool          xmce_set_psm_threshold                            (const char *args);
static void          xmce_get_psm_threshold                            (void);
static bool          xmce_set_forced_psm                               (const char *args);
static void          xmce_get_forced_psm                               (void);
static bool          xmce_set_low_power_mode                           (const char *args);
static void          xmce_get_low_power_mode                           (void);
static bool          xmce_set_inhibit_mode                             (const char *args);
static void          xmce_get_inhibit_mode                             (void);
static bool          xmce_set_kbd_slide_inhibit_mode                   (const char *args);
static void          xmce_get_kbd_slide_inhibit_mode                   (void);
static bool          xmce_set_lipstick_core_delay                      (const char *args);
static void          xmce_get_lipstick_core_delay                      (void);
static bool          xmce_set_brightness_fade_default                  (const char *args);
static bool          xmce_set_brightness_fade_dimming                  (const char *args);
static bool          xmce_set_brightness_fade_als                      (const char *args);
static bool          xmce_set_brightness_fade_blank                    (const char *args);
static bool          xmce_set_brightness_fade_unblank                  (const char *args);
static void          xmce_get_brightness_fade_helper                   (const char *title, const char *key);
static void          xmce_get_brightness_fade                          (void);
static bool          xmce_set_memnotify_warning_used                   (const char *args);
static bool          xmce_set_memnotify_warning_active                 (const char *args);
static bool          xmce_set_memnotify_critical_used                  (const char *args);
static bool          xmce_set_memnotify_critical_active                (const char *args);
static void          xmce_get_memnotify_helper                         (const char *title, const char *key);
static void          xmce_get_memnotify_limits                         (void);
static void          xmce_get_memnotify_level                          (void);
static bool          xmce_set_input_policy_mode                        (const char *args);
static void          xmce_get_input_policy_mode                        (void);
static bool          xmce_set_touch_unblock_delay                      (const char *args);
static void          xmce_get_touch_unblock_delay                      (void);
static bool          xmce_set_cpu_scaling_governor                     (const char *args);
static void          xmce_get_cpu_scaling_governor                     (void);
static bool          xmce_set_never_blank                              (const char *args);
static void          xmce_get_never_blank                              (void);
static bool          xmce_set_suspend_policy                           (const char *args);
static void          xmce_get_suspend_policy                           (void);
static bool          xmce_get_suspend_stats                            (const char *args);
static bool          xmce_get_display_stats                            (const char *args);
static bool          xmce_set_fake_doubletap                           (const char *args);
static void          xmce_get_fake_doubletap                           (void);
static bool          xmce_tklock_open                                  (const char *args);
static bool          xmce_tklock_close                                 (const char *args);
static bool          xmce_tklock_callback                              (const char *args);
static bool          xmce_set_tklock_mode                              (const char *args);
static void          xmce_get_tklock_mode                              (void);
static bool          xmce_set_tklock_blank                             (const char *args);
static void          xmce_get_tklock_blank                             (void);
static void          xmce_get_version                                  (void);
static void          xmce_get_inactivity_state                         (void);
static void          xmce_get_keyboard_backlight_state                 (void);
static bool          xmce_get_status                                   (const char *args);
static bool          xmce_set_demo_mode                                (const char *args);

/* ------------------------------------------------------------------------- *
 * DBUSHELPER
 * ------------------------------------------------------------------------- */

static const char   *dbushelper_get_type_name      (int type);
static gboolean      dbushelper_require_type       (DBusMessageIter *iter, int want_type);
static gboolean      dbushelper_require_array_type (DBusMessageIter *iter, int want_type);
static DBusMessage  *dbushelper_call_method        (DBusMessage *req);
static gboolean      dbushelper_read_at_end        (DBusMessageIter *iter);
static gboolean      dbushelper_read_int           (DBusMessageIter *iter, gint *value);
static gboolean      dbushelper_read_int64         (DBusMessageIter *iter, int64_t *value);
static gboolean      dbushelper_read_string        (DBusMessageIter *iter, gchar **value);
static gboolean      dbushelper_read_boolean       (DBusMessageIter *iter, gboolean *value);
static gboolean      dbushelper_read_variant       (DBusMessageIter *iter, DBusMessageIter *sub);
static gboolean      dbushelper_read_array         (DBusMessageIter *iter, DBusMessageIter *sub);
static gboolean      dbushelper_read_dict          (DBusMessageIter *iter, DBusMessageIter *sub);
static gboolean      dbushelper_read_struct        (DBusMessageIter *iter, DBusMessageIter *sub);
static gboolean      dbushelper_read_int_array     (DBusMessageIter *iter, gint **value, gint *count);
static gboolean      dbushelper_init_read_iterator (DBusMessage *rsp, DBusMessageIter *iter);
static gboolean      dbushelper_init_write_iterator(DBusMessage *req, DBusMessageIter *iter);
static gboolean      dbushelper_write_int          (DBusMessageIter *iter, gint value);
static gboolean      dbushelper_write_string       (DBusMessageIter *iter, const char *value);
static gboolean      dbushelper_write_int_array    (DBusMessageIter *iter, const gint *value, gint count);
static gboolean      dbushelper_write_boolean      (DBusMessageIter *iter, gboolean value);
static gboolean      dbushelper_write_path         (DBusMessageIter *iter, const gchar *value);
static gboolean      dbushelper_push_variant       (DBusMessageIter **stack, const char *signature);
static gboolean      dbushelper_push_array         (DBusMessageIter **stack, const char *signature);
static gboolean      dbushelper_pop_container      (DBusMessageIter **stack);
static void          dbushelper_abandon_stack      (DBusMessageIter *stack, DBusMessageIter *iter);

/* ------------------------------------------------------------------------- *
 * UNGROUPED
 * ------------------------------------------------------------------------- */

static int          lookup                   (const symbol_t *stab, const char *key);
static const char  *rlookup                  (const symbol_t *stab, int val);
static int          parse_inhibitmode        (const char *args);
static const char  *repr_inhibitmode         (int value);
static gint        *parse_gint_array         (const char *text, gint *len);
static bool         is_configurable_pattern  (const char *pattern);
static bool         set_led_breathing_enabled(const char *args);
static void         get_led_breathing_enabled(void);
static bool         set_led_breathing_limit  (const char *args);
static void         get_led_breathing_limit  (void);
static bool         set_led_pattern_enabled  (const char *pattern, bool enable);
static bool         interactive_confirmation (const char *positive);
static char        *elapsed_time_repr        (char *buff, size_t size, int64_t t);
int                 main                     (int argc, char **argv);

/* ------------------------------------------------------------------------- *
 * MCETOOL
 * ------------------------------------------------------------------------- */

static gboolean   mcetool_parse_timspec     (struct timespec *ts, const char *args);
static char      *mcetool_parse_token       (char **ppos);
static char      *mcetool_format_bitmask    (const symbol_t *lut, int mask, char *buff, size_t size);
static unsigned   mcetool_parse_bitmask     (const symbol_t *lut, const char *args);
static bool       mcetool_show_led_patterns (const char *args);
static bool       mcetool_block             (const char *args);
static bool       mcetool_handle_common_args(const char *key, const char *args);
static bool       mcetool_reject_common_args(const char *args);

/* ------------------------------------------------------------------------- *
 * MCETOOL_DO
 * ------------------------------------------------------------------------- */

static bool  mcetool_do_enable_led         (const char *arg);
static bool  mcetool_do_disable_led        (const char *arg);
static bool  mcetool_do_enable_pattern     (const char *args);
static bool  mcetool_do_disable_led_pattern(const char *args);
static bool  mcetool_do_activate_pattern   (const char *args);
static bool  mcetool_do_deactivate_pattern (const char *args);
#ifdef ENABLE_BATTERY_SIMULATION
static bool  mcetool_do_set_charger_type(const char *arg);
static bool  mcetool_do_set_charger_state  (const char *arg);
static bool  mcetool_do_set_battery_level  (const char *arg);
#endif
static bool  mcetool_do_unblank_screen     (const char *arg);
static bool  mcetool_do_dim_screen         (const char *arg);
static bool  mcetool_do_blank_screen       (const char *arg);
static bool  mcetool_do_blank_screen_lpm   (const char *arg);
static bool  mcetool_do_version            (const char *arg);
static bool  mcetool_do_help               (const char *arg);
static bool  mcetool_do_long_help          (const char *arg);

/* ------------------------------------------------------------------------- *
 * GENERIC DBUS HELPERS
 * ------------------------------------------------------------------------- */

/** Cached D-Bus connection */
static DBusConnection *xdbus_con = NULL;

/** Initialize D-Bus system bus connection
 *
 * Makes a cached connection to system bus and checks if mce is present
 *
 * @return System bus connection on success, terminates on failure
 */
static DBusConnection *xdbus_init(void)
{
        if( !xdbus_con ) {
                DBusError err = DBUS_ERROR_INIT;
                DBusBusType bus_type = DBUS_BUS_SYSTEM;

                if( !(xdbus_con = dbus_bus_get(bus_type, &err)) ) {
                        errorf("Failed to open connection to message bus; %s: %s\n",
                               err.name, err.message);
                        dbus_error_free(&err);
                        exit(EXIT_FAILURE);
                }
                debugf("connected to system bus\n");

                if( !dbus_bus_name_has_owner(xdbus_con, MCE_SERVICE, &err) ) {
                        if( dbus_error_is_set(&err) ) {
                                errorf("%s: %s: %s\n", MCE_SERVICE,
                                        err.name, err.message);
                        }
                        errorf("MCE not running, terminating\n");
                        dbus_error_free(&err);
                        exit(EXIT_FAILURE);
                }

                debugf("mce is running\n");
        }
        return xdbus_con;
}

/** Disconnect from D-Bus system bus
 */
static void xdbus_exit(void)
{
        /* If there is an established D-Bus connection, unreference it */
        if (xdbus_con != NULL) {
                dbus_connection_unref(xdbus_con);
                xdbus_con = NULL;
                debugf("disconnected from system bus\n");
        }
}

/** Make sure the cached dbus connection is not used directly */
#define xdbus_con something_that_will_generate_error

/** Generic synchronous D-Bus method call wrapper function
 *
 * If reply pointer is NULL, the method call is sent without
 * waiting for method return message.
 *
 * @param service   [IN]  D-Bus service name
 * @param path      [IN]  D-Bus object path
 * @param interface [IN]  D-Bus interface name
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store method_return message, or NULL
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param va        [IN]  va_list of D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent (and optionally reply received),
 *         or FALSE in case of errors or if error reply is received
 */
static gboolean xdbus_call_va(const gchar *const service, const gchar *const path,
                              const gchar *const interface, const gchar *const name,
                              DBusMessage **reply, int arg_type, va_list va)
{
        debugf("%s(%s,%s,%s,%s)\n", __FUNCTION__, service, path, interface, name);
        gboolean        ack = FALSE;
        DBusMessage    *msg = 0;
        DBusMessage    *rsp = 0;
        DBusConnection *bus = xdbus_init();
        DBusError       err = DBUS_ERROR_INIT;

        msg = dbus_message_new_method_call(service, path, interface, name);

        if( !dbus_message_append_args_valist(msg, arg_type, va) ) {
                errorf("%s.%s: failed to construct message\n", interface, name);
                goto EXIT;
        }

        dbus_message_set_auto_start(msg, FALSE);

        if( reply ) {
                rsp = dbus_connection_send_with_reply_and_block(bus, msg, -1, &err);
                if( rsp == 0 ) {
                        errorf("%s.%s send message: %s: %s\n",
                               interface, name, err.name, err.message);
                        goto EXIT;
                }

                if( dbus_set_error_from_message(&err, rsp) ) {
                        errorf("%s.%s call failed: %s: %s\n",
                               interface, name, err.name, err.message);
                        dbus_message_unref(rsp), rsp = 0;
                        goto EXIT;
                }

        }
        else {
                dbus_message_set_no_reply(msg, TRUE);

                if( !dbus_connection_send(bus, msg, NULL) ) {
                        errorf("Failed to send method call\n");
                        goto EXIT;
                }
                dbus_connection_flush(bus);
        }

        ack = TRUE;

EXIT:
        if( reply ) *reply = rsp, rsp = 0;

        dbus_error_free(&err);

        if( rsp ) dbus_message_unref(rsp);
        if( msg ) dbus_message_unref(msg);

        return ack;
}

/* ------------------------------------------------------------------------- *
 * MCE DBUS IPC HELPERS
 * ------------------------------------------------------------------------- */

/** Wrapper for making synchronous D-Bus method calls to MCE
 *
 * If reply pointer is NULL, the method call is sent without
 * waiting for method return message.
 *
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store method_return message, or NULL
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param va        [IN]  va_list of D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent (and optionally reply received),
 *         or FALSE in case of errors
 */
static gboolean xmce_ipc_va(const gchar *const name, DBusMessage **reply,
                            int arg_type, va_list va)
{
        return xdbus_call_va(MCE_SERVICE,
                             MCE_REQUEST_PATH,
                             MCE_REQUEST_IF,
                             name, reply,
                             arg_type, va);
}

/** Wrapper for making synchronous D-Bus method calls to MCE
 *
 * If reply pointer is NULL, the method call is sent without
 * waiting for method return message.
 *
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store method_return message, or NULL
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param ...       [IN]  D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent (and optionally reply received),
 *         or FALSE in case of errors
 */
#ifdef ENABLE_BATTERY_SIMULATION
static gboolean xmce_ipc(const gchar *const name, DBusMessage **reply,
                         int arg_type, ...)
{
        va_list va;
        va_start(va, arg_type);
        gboolean ack = xdbus_call_va(MCE_SERVICE,
                                     MCE_REQUEST_PATH,
                                     MCE_REQUEST_IF,
                                     name, reply,
                                     arg_type, va);
        va_end(va);
        return ack;
}
#endif // ENABLE_BATTERY_SIMULATION

/** Wrapper for making MCE D-Bus method calls without waiting for reply
 *
 * @param name      [IN]  D-Bus method call name
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param ...       [IN]  D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent, or FALSE in case of errors
 */
static gboolean xmce_ipc_no_reply(const gchar *const name,
                                  int arg_type, ...)
{
        DBusMessage *reply = 0;

        va_list va;
        va_start(va, arg_type);
        gboolean ack = xmce_ipc_va(name, &reply, arg_type, va);
        va_end(va);

        if( reply )
                dbus_message_unref(reply);
        return ack;
}

/** Wrapper for making synchronous MCE D-Bus method calls
 *
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store method_return message
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param ...       [IN]  D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent and non-error reply received,
 *         or FALSE in case of errors
 */
static gboolean xmce_ipc_message_reply(const gchar *const name, DBusMessage **reply,
                                       int arg_type, ...)
{
        va_list va;
        va_start(va, arg_type);
        gboolean ack = xmce_ipc_va(name, reply, arg_type, va);
        va_end(va);
        return ack;
}

/** Wrapper for making synchronous MCE D-Bus method calls that return STRING
 *
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store string from method return
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param ...       [IN]  D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent and non-error reply received and parsed,
 *         or FALSE in case of errors
 */
static gboolean xmce_ipc_string_reply(const gchar *const name,
                                      char **result,
                                      int arg_type, ...)
{
        gboolean     ack = FALSE;
        DBusMessage *rsp = 0;
        DBusError    err = DBUS_ERROR_INIT;
        const char  *dta = 0;

        va_list va;
        va_start(va, arg_type);

        if( !xmce_ipc_va(name, &rsp, arg_type, va) )
                goto EXIT;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_STRING, &dta,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

        *result = strdup(dta);

        ack = TRUE;
EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", name, err.name, err.message);
                dbus_error_free(&err);
        }

        if( rsp ) dbus_message_unref(rsp);

        va_end(va);
        return ack;
}

/** Wrapper for making synchronous MCE D-Bus method calls that return UINT32
 *
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store uint from method return
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param ...       [IN]  D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent and non-error reply received and parsed,
 *         or FALSE in case of errors
 */
static gboolean xmce_ipc_uint_reply(const gchar *const name,
                                    guint *result,
                                    int arg_type, ...)
{
        gboolean      ack = FALSE;
        DBusMessage  *rsp = 0;
        DBusError     err = DBUS_ERROR_INIT;
        dbus_uint32_t dta = 0;

        va_list va;
        va_start(va, arg_type);

        if( !xmce_ipc_va(name, &rsp, arg_type, va) )
                goto EXIT;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_UINT32, &dta,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

        *result = dta;

        ack = TRUE;
EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", name, err.name, err.message);
                dbus_error_free(&err);
        }
        if( rsp ) dbus_message_unref(rsp);

        va_end(va);
        return ack;
}

/** Wrapper for making synchronous MCE D-Bus method calls that return INT32
 *
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store int from method return
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param ...       [IN]  D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent and non-error reply received and parsed,
 *         or FALSE in case of errors
 */
static gboolean xmce_ipc_int_reply(const gchar *const name,
                                   gint *result,
                                   int arg_type, ...)
{
        gboolean      ack = FALSE;
        DBusMessage  *rsp = 0;
        DBusError     err = DBUS_ERROR_INIT;
        dbus_int32_t  dta = 0;

        va_list va;
        va_start(va, arg_type);

        if( !xmce_ipc_va(name, &rsp, arg_type, va) )
                goto EXIT;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_INT32, &dta,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

        *result = dta;

        ack = TRUE;
EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", name, err.name, err.message);
                dbus_error_free(&err);
        }
        if( rsp ) dbus_message_unref(rsp);

        va_end(va);
        return ack;
}

/** Wrapper for making synchronous MCE D-Bus method calls that return BOOLEAN
 *
 * @param name      [IN]  D-Bus method call name
 * @param reply     [OUT] Where to store bool from method return
 * @param arg_type  [IN]  DBUS_TYPE_STRING etc, as with dbus_message_append_args()
 * @param ...       [IN]  D-Bus arguments, terminated with DBUS_TYPE_INVALID
 *
 * @return TRUE if call was successfully sent and non-error reply received and parsed,
 *         or FALSE in case of errors
 */
static gboolean xmce_ipc_bool_reply(const gchar *const name,
                                    gboolean *result,
                                    int arg_type, ...)
{
        gboolean      ack = FALSE;
        DBusMessage  *rsp = 0;
        DBusError     err = DBUS_ERROR_INIT;
        dbus_bool_t   dta = 0;

        va_list va;
        va_start(va, arg_type);

        if( !xmce_ipc_va(name, &rsp, arg_type, va) )
                goto EXIT;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_BOOLEAN, &dta,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

        *result = dta;

        ack = TRUE;
EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", name, err.name, err.message);
                dbus_error_free(&err);
        }

        if( rsp ) dbus_message_unref(rsp);

        va_end(va);
        return ack;
}

/* ------------------------------------------------------------------------- *
 * MCE IPC HELPERS
 * ------------------------------------------------------------------------- */

/** Helper for getting dbus data type as string
 *
 * @param type dbus data type (DBUS_TYPE_BOOLEAN etc)
 *
 * @return type name with out common prefix (BOOLEAN etc)
 */
static const char *dbushelper_get_type_name(int type)
{
        const char *res = "UNKNOWN";
        switch( type ) {
        case DBUS_TYPE_INVALID:     res = "INVALID";     break;
        case DBUS_TYPE_BYTE:        res = "BYTE";        break;
        case DBUS_TYPE_BOOLEAN:     res = "BOOLEAN";     break;
        case DBUS_TYPE_INT16:       res = "INT16";       break;
        case DBUS_TYPE_UINT16:      res = "UINT16";      break;
        case DBUS_TYPE_INT32:       res = "INT32";       break;
        case DBUS_TYPE_UINT32:      res = "UINT32";      break;
        case DBUS_TYPE_INT64:       res = "INT64";       break;
        case DBUS_TYPE_UINT64:      res = "UINT64";      break;
        case DBUS_TYPE_DOUBLE:      res = "DOUBLE";      break;
        case DBUS_TYPE_STRING:      res = "STRING";      break;
        case DBUS_TYPE_OBJECT_PATH: res = "OBJECT_PATH"; break;
        case DBUS_TYPE_SIGNATURE:   res = "SIGNATURE";   break;
        case DBUS_TYPE_UNIX_FD:     res = "UNIX_FD";     break;
        case DBUS_TYPE_ARRAY:       res = "ARRAY";       break;
        case DBUS_TYPE_VARIANT:     res = "VARIANT";     break;
        case DBUS_TYPE_STRUCT:      res = "STRUCT";      break;
        case DBUS_TYPE_DICT_ENTRY:  res = "DICT_ENTRY";  break;
        default: break;
        }
        return res;
}

/** Helper for testing that iterator points to expected data type
 *
 * @param iter D-Bus message iterator
 * @param want_type D-Bus data type
 *
 * @return TRUE if iterator points to give data type, FALSE otherwise
 */
static gboolean dbushelper_require_type(DBusMessageIter *iter,
                                        int want_type)
{
        int have_type = dbus_message_iter_get_arg_type(iter);

        if( want_type != have_type ) {
                errorf("expected DBUS_TYPE_%s, got %s\n",
                       dbushelper_get_type_name(want_type),
                       dbushelper_get_type_name(have_type));
                return FALSE;
        }

        return TRUE;
}

/** Helper for testing that iterator points to array of expected data type
 *
 * @param iter D-Bus message iterator
 * @param want_type D-Bus data type
 *
 * @return TRUE if iterator points to give data type, FALSE otherwise
 */
static gboolean dbushelper_require_array_type(DBusMessageIter *iter,
                                              int want_type)
{
        if( !dbushelper_require_type(iter, DBUS_TYPE_ARRAY) )
                return FALSE;

        int have_type = dbus_message_iter_get_element_type(iter);

        if( want_type != have_type ) {
                errorf("expected array of DBUS_TYPE_%s, got %s\n",
                       dbushelper_get_type_name(want_type),
                       dbushelper_get_type_name(have_type));
                return FALSE;
        }

        return TRUE;
}

/** Helper for making blocking D-Bus method calls
 *
 * @param req D-Bus method call message to send
 *
 * @return D-Bus method reply message, or NULL on failure
 */
static DBusMessage *dbushelper_call_method(DBusMessage *req)
{
        DBusMessage *rsp = 0;
        DBusError    err = DBUS_ERROR_INIT;

        rsp = dbus_connection_send_with_reply_and_block(xdbus_init(),
                                                        req, -1, &err);

        if( !rsp ) {
                errorf("%s.%s: %s: %s\n",
                       dbus_message_get_interface(req),
                       dbus_message_get_member(req),
                       err.name, err.message);
                goto EXIT;
        }

EXIT:
        dbus_error_free(&err);

        return rsp;
}

/** Helper for detecting end of data from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 *
 * @return TRUE if iterator points to DBUS_TYPE_INVALID, FALSE otherwise
 */
static gboolean dbushelper_read_at_end(DBusMessageIter *iter)
{
        int have_type = dbus_message_iter_get_arg_type(iter);
        return have_type == DBUS_TYPE_INVALID;
}

/** Helper for parsing int value from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param value Where to store the value (not modified on failure)
 *
 * @return TRUE if value could be read, FALSE on failure
 */
static gboolean dbushelper_read_int(DBusMessageIter *iter, gint *value)
{
        dbus_int32_t data = 0;

        if( !dbushelper_require_type(iter, DBUS_TYPE_INT32) )
                return FALSE;

        dbus_message_iter_get_basic(iter, &data);
        dbus_message_iter_next(iter);

        return *value = data, TRUE;
}

/** Helper for parsing int64 value from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param value Where to store the value (not modified on failure)
 *
 * @return TRUE if value could be read, FALSE on failure
 */
static gboolean dbushelper_read_int64(DBusMessageIter *iter, int64_t *value)
{
        dbus_int64_t data = 0;

        if( !dbushelper_require_type(iter, DBUS_TYPE_INT64) )
                return FALSE;

        dbus_message_iter_get_basic(iter, &data);
        dbus_message_iter_next(iter);

        return *value = data, TRUE;
}

/** Helper for parsing string value from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param value Where to store the value (not modified on failure)
 *
 * @return TRUE if value could be read, FALSE on failure
 */
static gboolean dbushelper_read_string(DBusMessageIter *iter, gchar **value)
{
        char *data = 0;

        if( !dbushelper_require_type(iter, DBUS_TYPE_STRING) )
                return FALSE;

        dbus_message_iter_get_basic(iter, &data);
        dbus_message_iter_next(iter);

        return *value = g_strdup(data ?: ""), TRUE;
}

/** Helper for parsing boolean value from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param value Where to store the value (not modified on failure)
 *
 * @return TRUE if value could be read, FALSE on failure
 */
static gboolean dbushelper_read_boolean(DBusMessageIter *iter, gboolean *value)
{
        dbus_bool_t data = 0;

        if( !dbushelper_require_type(iter, DBUS_TYPE_BOOLEAN) )
                return FALSE;

        dbus_message_iter_get_basic(iter, &data);
        dbus_message_iter_next(iter);

        return *value = data, TRUE;
}

/** Helper for entering variant container from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param sub  D-Bus message iterator for variant (not modified on failure)
 *
 * @return TRUE if container could be entered, FALSE on failure
 */
static gboolean dbushelper_read_variant(DBusMessageIter *iter, DBusMessageIter *sub)
{
        if( !dbushelper_require_type(iter, DBUS_TYPE_VARIANT) )
                return FALSE;

        dbus_message_iter_recurse(iter, sub);
        dbus_message_iter_next(iter);

        return TRUE;
}

/** Helper for entering array container from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param sub  D-Bus message iterator for array (not modified on failure)
 *
 * @return TRUE if container could be entered, FALSE on failure
 */
static gboolean dbushelper_read_array(DBusMessageIter *iter, DBusMessageIter *sub)
{
        if( !dbushelper_require_type(iter, DBUS_TYPE_ARRAY) )
                return FALSE;

        dbus_message_iter_recurse(iter, sub);
        dbus_message_iter_next(iter);

        return TRUE;
}

/** Helper for entering dict entry container from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param sub  D-Bus message iterator for dict entry (not modified on failure)
 *
 * @return TRUE if container could be entered, FALSE on failure
 */
static gboolean dbushelper_read_dict(DBusMessageIter *iter, DBusMessageIter *sub)
{
        if( !dbushelper_require_type(iter, DBUS_TYPE_DICT_ENTRY) )
                return FALSE;

        dbus_message_iter_recurse(iter, sub);
        dbus_message_iter_next(iter);

        return TRUE;
}

/** Helper for entering struct container from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param sub  D-Bus message iterator for struct (not modified on failure)
 *
 * @return TRUE if container could be entered, FALSE on failure
 */
static gboolean dbushelper_read_struct(DBusMessageIter *iter, DBusMessageIter *sub)
{
        if( !dbushelper_require_type(iter, DBUS_TYPE_STRUCT) )
                return FALSE;

        dbus_message_iter_recurse(iter, sub);
        dbus_message_iter_next(iter);

        return TRUE;
}

/** Helper for parsing int array from D-Bus message iterator
 *
 * @param iter D-Bus message iterator
 * @param value Where to store the array pointer (not modified on failure)
 * @param count Where to store the array length (not modified on failure)
 *
 * @return TRUE if value could be read, FALSE on failure
 */
static gboolean dbushelper_read_int_array(DBusMessageIter *iter,
                                          gint **value, gint *count)
{
        debugf("@%s()\n", __FUNCTION__);

        dbus_int32_t *arr_dbus = 0;
        gint         *arr_glib = 0;
        int           cnt = 0;
        DBusMessageIter tmp;

        if( !dbushelper_require_array_type(iter, DBUS_TYPE_INT32) )
                return FALSE;

        if( !dbushelper_read_array(iter, &tmp) )
                return FALSE;

        dbus_message_iter_get_fixed_array(&tmp, &arr_dbus, &cnt);
        dbus_message_iter_next(iter);

        arr_glib = g_malloc0(cnt * sizeof *arr_glib);
        for( gint i = 0; i < cnt; ++i )
                arr_glib[i] = arr_dbus[i];

        return *value = arr_glib, *count = cnt, TRUE;
}

/** Helper for initializing D-Bus message read iterator
 *
 * @param rsp  D-Bus message
 * @param iter D-Bus iterator for parsing message (not modified on failure)
 *
 * @return TRUE if read iterator could be initialized, FALSE on failure
 */
static gboolean dbushelper_init_read_iterator(DBusMessage *rsp,
                                              DBusMessageIter *iter)
{
        if( !dbus_message_iter_init(rsp, iter) ) {
                errorf("failed to initialize dbus read iterator\n");
                return FALSE;
        }
        return TRUE;
}

/** Helper for initializing D-Bus message write iterator
 *
 * @param rsp  D-Bus message
 * @param iter D-Bus iterator for appending message (not modified on failure)
 *
 * @return TRUE if append iterator could be initialized, FALSE on failure
 */
static gboolean dbushelper_init_write_iterator(DBusMessage *req,
                                               DBusMessageIter *iter)
{
        dbus_message_iter_init_append(req, iter);
        return TRUE;
}

/** Helper for adding int value to D-Bus iterator
 *
 * @param iter Write iterator where to add the value
 * @param value the value to add
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_write_int(DBusMessageIter *iter, gint value)
{
        dbus_int32_t data = value;
        int          type = DBUS_TYPE_INT32;

        if( !dbus_message_iter_append_basic(iter, type, &data) ) {
                errorf("failed to add %s data\n",
                       dbushelper_get_type_name(type));
                return FALSE;
        }

        return TRUE;
}

/** Helper for adding string value to D-Bus iterator
 *
 * @param iter Write iterator where to add the value
 * @param value the value to add
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_write_string(DBusMessageIter *iter, const char *value)
{
        const char *data = value ?: "";
        int         type = DBUS_TYPE_STRING;

        if( !dbus_message_iter_append_basic(iter, type, &data) ) {
                errorf("failed to add %s data\n",
                       dbushelper_get_type_name(type));
                return FALSE;
        }

        return TRUE;
}

/** Helper for adding int value array to D-Bus iterator
 *
 * @param iter Write iterator where to add the value
 * @param value the value to add
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_write_int_array(DBusMessageIter *iter,
                                           const gint *value, gint count)
{
        gboolean      res  = FALSE;
        int           type = DBUS_TYPE_INT32;
        dbus_int32_t *data = g_malloc0(count * sizeof *data);

        for( gint i = 0; i < count; ++i )
                data[i] = value[i];

        if( !dbus_message_iter_append_fixed_array(iter, type, &data, count) ) {
                errorf("failed to add array of %s data\n",
                       dbushelper_get_type_name(type));
                goto cleanup;
        }

        res = TRUE;

cleanup:
        g_free(data);

        return res;
}

/** Helper for adding boolean value to D-Bus iterator
 *
 * @param iter Write iterator where to add the value
 * @param value the value to add
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_write_boolean(DBusMessageIter *iter, gboolean value)
{
        dbus_bool_t data = value;
        int         type = DBUS_TYPE_BOOLEAN;

        if( !dbus_message_iter_append_basic(iter, type, &data) ) {
                errorf("failed to add %s data\n",
                       dbushelper_get_type_name(type));
                return FALSE;
        }

        return TRUE;
}

/** Helper for adding object path value to D-Bus iterator
 *
 * @param iter Write iterator where to add the value
 * @param value the value to add
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_write_path(DBusMessageIter *iter, const gchar *value)
{
        const char *data = value;
        int         type = DBUS_TYPE_OBJECT_PATH;

        if( !dbus_message_iter_append_basic(iter, type, &data) ) {
                errorf("failed to add %s data\n",
                       dbushelper_get_type_name(type));
                return FALSE;
        }

        return TRUE;
}

/** Helper for opening a variant container
 *
 * @param stack pointer to D-Bus message iterator pointer (not
 modified on failure)
 *
 * @param signature signature string of the data that will be added to the
 *                  variant container
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_push_variant(DBusMessageIter **stack,
                                        const char *signature)
{
        DBusMessageIter *iter = *stack;
        DBusMessageIter *sub  = iter + 1;

        if( !dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
                                              signature, sub) ) {
                errorf("failed to initialize variant write iterator\n");
                return FALSE;
        }

        *stack = sub;
        return TRUE;
}

/** Helper for opening a array container
 *
 * @param stack pointer to D-Bus message iterator pointer (not
 modified on failure)
 *
 * @param signature signature string of the data that will be added to the
 *                  variant container
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_push_array(DBusMessageIter **stack,
                                      const char *signature)
{
        DBusMessageIter *iter = *stack;
        DBusMessageIter *sub  = iter + 1;

        if( !dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
                                              signature, sub) ) {
                errorf("failed to initialize array write iterator\n");
                return FALSE;
        }

        *stack = sub;
        return TRUE;
}

/** Helper for closing a container
 *
 * @param stack pointer to D-Bus message iterator pointer
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbushelper_pop_container(DBusMessageIter **stack)
{
        DBusMessageIter *sub  = *stack;
        DBusMessageIter *iter = sub - 1;

        gboolean res = dbus_message_iter_close_container(iter, sub);

        *stack = iter;
        return res;
}

/** Helper for abandoning message iterator stack
 *
 * @param stack Start of iterator stack
 * @param iter  Current iterator within the stack
 */
static void dbushelper_abandon_stack(DBusMessageIter *stack,
                                     DBusMessageIter *iter)
{
        while( iter-- > stack )
                dbus_message_iter_abandon_container(iter, iter+1);
}

/* ------------------------------------------------------------------------- *
 * MCE SETTING IPC HELPERS
 * ------------------------------------------------------------------------- */

/** Helper for making MCE D-Bus method calls
 *
 * @param method name of the method in mce request interface
 * @param arg_type as with dbus_message_append_args()
 * @param ... must be terminated with DBUS_TYPE_INVALID
 */
static DBusMessage *xmce_setting_request(const gchar *const method)
{
        DBusMessage *req = 0;

        req = dbus_message_new_method_call(MCE_SERVICE,
                                           MCE_REQUEST_PATH,
                                           MCE_REQUEST_IF,
                                           method);
        if( !req ) {
                errorf("%s.%s: can't allocate method call\n",
                       MCE_REQUEST_IF, method);
                goto EXIT;
        }

        dbus_message_set_auto_start(req, FALSE);

EXIT:
        return req;
}

/** Return a boolean from the specified setting key
 *
 * @param key The setting key to get the value from
 * @param value Will contain the value on return
 * @return TRUE on success, FALSE on failure
 */
static gboolean xmce_setting_get_bool(const gchar *const key, gboolean *value)
{
        debugf("@%s(%s)\n", __FUNCTION__, key);

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter body, variant;

        if( !(req = xmce_setting_request(MCE_CONFIG_GET)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, &body) )
                goto EXIT;
        if( !dbushelper_write_path(&body, key) )
                goto EXIT;

        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;
        if( !dbushelper_init_read_iterator(rsp, &body) )
                goto EXIT;
        if( !dbushelper_read_variant(&body, &variant) )
                goto EXIT;

        res = dbushelper_read_boolean(&variant, value);

EXIT:
        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/** Return an integer from the specified setting key
 *
 * @param key The setting key to get the value from
 * @param value Will contain the value on return
 * @return TRUE on success, FALSE on failure
 */
static gboolean xmce_setting_get_int(const gchar *const key, gint *value)
{
        debugf("@%s(%s)\n", __FUNCTION__, key);

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter body, variant;

        if( !(req = xmce_setting_request(MCE_CONFIG_GET)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, &body) )
                goto EXIT;
        if( !dbushelper_write_path(&body, key) )
                goto EXIT;

        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;
        if( !dbushelper_init_read_iterator(rsp, &body) )
                goto EXIT;
        if( !dbushelper_read_variant(&body, &variant) )
                goto EXIT;

        res = dbushelper_read_int(&variant, value);

EXIT:
        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/** Return a string from the specified setting key
 *
 * @param key The setting key to get the value from
 * @param value Will contain the value on return
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean xmce_setting_get_string(const gchar *const key, gchar **value)
{
        debugf("@%s(%s)\n", __FUNCTION__, key);

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter body, variant;

        if( !(req = xmce_setting_request(MCE_CONFIG_GET)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, &body) )
                goto EXIT;
        if( !dbushelper_write_path(&body, key) )
                goto EXIT;

        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;
        if( !dbushelper_init_read_iterator(rsp, &body) )
                goto EXIT;
        if( !dbushelper_read_variant(&body, &variant) )
                goto EXIT;

        res = dbushelper_read_string(&variant, value);

EXIT:
        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/** Return an integer array from the specified setting key
 *
 * @param key The setting key to get the value from
 * @param values Will contain the array of values on return
 * @param count  Will contain the array size on return
 * @return TRUE on success, FALSE on failure
 */
static gboolean xmce_setting_get_int_array(const gchar *const key, gint **values, gint *count)
{
        debugf("@%s(%s)\n", __FUNCTION__, key);

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter body, variant;

        if( !(req = xmce_setting_request(MCE_CONFIG_GET)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, &body) )
                goto EXIT;
        if( !dbushelper_write_path(&body, key) )
                goto EXIT;

        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;

        if( !dbushelper_init_read_iterator(rsp, &body) )
                goto EXIT;
        if( !dbushelper_read_variant(&body, &variant) )
                goto EXIT;

        res = dbushelper_read_int_array(&variant, values, count);

EXIT:
        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/** Set a boolean setting key to the specified value
 *
 * @param key The setting key to set the value of
 * @param value The value to set the key to
 * @return TRUE on success, FALSE on failure
 */
static gboolean xmce_setting_set_bool(const gchar *const key,
                                      const gboolean value)
{
        debugf("@%s(%s, %d)\n", __FUNCTION__, key, value);

        static const char sig[] = DBUS_TYPE_BOOLEAN_AS_STRING;

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter stack[2];
        DBusMessageIter *wpos = stack;
        DBusMessageIter *rpos = stack;

        if( !(req = xmce_setting_request(MCE_CONFIG_SET)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, wpos) )
                goto EXIT;
        if( !dbushelper_write_path(wpos, key) )
                goto EXIT;
        if( !dbushelper_push_variant(&wpos, sig) )
                goto EXIT;
        if( !dbushelper_write_boolean(wpos, value) )
                goto EXIT;
        if( !dbushelper_pop_container(&wpos) )
                goto EXIT;
        if( wpos != stack )
                abort();

        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;
        if( !dbushelper_init_read_iterator(rsp, rpos) )
                goto EXIT;
        if( !dbushelper_read_boolean(rpos, &res) )
                res = FALSE;

EXIT:
        // make sure write iterator stack is collapsed
        dbushelper_abandon_stack(stack, wpos);

        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/** Set an integer setting key to the specified value
 *
 * @param key The setting key to set the value of
 * @param value The value to set the key to
 * @return TRUE on success, FALSE on failure
 */
static gboolean xmce_setting_set_int(const gchar *const key, const gint value)
{
        debugf("@%s(%s, %d)\n", __FUNCTION__, key, value);

        static const char sig[] = DBUS_TYPE_INT32_AS_STRING;

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter stack[2];
        DBusMessageIter *wpos = stack;
        DBusMessageIter *rpos = stack;

        // construct request
        if( !(req = xmce_setting_request(MCE_CONFIG_SET)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, wpos) )
                goto EXIT;
        if( !dbushelper_write_path(wpos, key) )
                goto EXIT;
        if( !dbushelper_push_variant(&wpos, sig) )
                goto EXIT;
        if( !dbushelper_write_int(wpos, value) )
                goto EXIT;
        if( !dbushelper_pop_container(&wpos) )
                goto EXIT;
        if( wpos != stack )
                abort();

        // get reply and process it
        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;
        if( !dbushelper_init_read_iterator(rsp, rpos) )
                goto EXIT;
        if( !dbushelper_read_boolean(rpos, &res) )
                res = FALSE;

EXIT:
        // make sure write iterator stack is collapsed
        dbushelper_abandon_stack(stack, wpos);

        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/** Set an string setting key to the specified value
 *
 * @param key The setting key to set the value of
 * @param value The value to set the key to
 * @return TRUE on success, FALSE on failure
 */
static gboolean xmce_setting_set_string(const gchar *const key, const char *value)
{
        debugf("@%s(%s, %d)\n", __FUNCTION__, key, value);

        static const char sig[] = DBUS_TYPE_STRING_AS_STRING;

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter stack[2];
        DBusMessageIter *wpos = stack;
        DBusMessageIter *rpos = stack;

        // construct request
        if( !(req = xmce_setting_request(MCE_CONFIG_SET)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, wpos) )
                goto EXIT;
        if( !dbushelper_write_path(wpos, key) )
                goto EXIT;
        if( !dbushelper_push_variant(&wpos, sig) )
                goto EXIT;
        if( !dbushelper_write_string(wpos, value) )
                goto EXIT;
        if( !dbushelper_pop_container(&wpos) )
                goto EXIT;
        if( wpos != stack )
                abort();

        // get reply and process it
        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;
        if( !dbushelper_init_read_iterator(rsp, rpos) )
                goto EXIT;
        if( !dbushelper_read_boolean(rpos, &res) )
                res = FALSE;

EXIT:
        // make sure write iterator stack is collapsed
        dbushelper_abandon_stack(stack, wpos);

        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/** Set an integer array setting key to the specified values
 *
 * @param key The setting key to set the value of
 * @param values The array of values to set the key to
 * @param count  The number of values in the array
 * @return TRUE on success, FALSE on failure
 */
static gboolean xmce_setting_set_int_array(const gchar *const key,
                                           const gint *values,
                                           gint count)
{
        debugf("@%s(%s, num x %d)\n", __FUNCTION__, key, count);

        static const char vsig[] = DBUS_TYPE_ARRAY_AS_STRING
                DBUS_TYPE_INT32_AS_STRING;
        static const char asig[] = DBUS_TYPE_INT32_AS_STRING;

        gboolean     res = FALSE;
        DBusMessage *req = 0;
        DBusMessage *rsp = 0;

        DBusMessageIter stack[3];
        DBusMessageIter *wpos = stack;
        DBusMessageIter *rpos = stack;

        // construct request
        if( !(req = xmce_setting_request(MCE_CONFIG_SET)) )
                goto EXIT;
        if( !dbushelper_init_write_iterator(req, wpos) )
                goto EXIT;
        if( !dbushelper_write_path(wpos, key) )
                goto EXIT;
        if( !dbushelper_push_variant(&wpos, vsig) )
                goto EXIT;
        if( !dbushelper_push_array(&wpos, asig) )
                goto EXIT;
        if( !dbushelper_write_int_array(wpos, values, count) )
                goto EXIT;
        if( !dbushelper_pop_container(&wpos) )
                goto EXIT;
        if( !dbushelper_pop_container(&wpos) )
                goto EXIT;
        if( wpos != stack )
                abort();

        // get reply and process it
        if( !(rsp = dbushelper_call_method(req)) )
                goto EXIT;
        if( !dbushelper_init_read_iterator(rsp, rpos) )
                goto EXIT;
        if( !dbushelper_read_boolean(rpos, &res) )
                res = FALSE;

EXIT:
        // make sure write iterator stack is collapsed
        dbushelper_abandon_stack(stack, wpos);

        if( rsp ) dbus_message_unref(rsp);
        if( req ) dbus_message_unref(req);

        return res;
}

/* ------------------------------------------------------------------------- *
 * SYMBOL LOOKUP TABLES
 * ------------------------------------------------------------------------- */

/** Lookup symbol by name and return value
 *
 * @param stab array of symbol_t objects
 * @param key name of the symbol to find
 *
 * @return Value matching the name. Or if not found, the
 *         value of the end-of-table marker symbol */
static int lookup(const symbol_t *stab, const char *key)
{
        for( ; ; ++stab ) {
                if( !stab->key || !strcmp(stab->key, key) )
                        return stab->val;
        }
}

/** Lookup symbol by value and return name
 *
 * @param stab array of symbol_t objects
 * @param val value of the symbol to find
 *
 * @return name of the first matching value, or NULL
 */
static const char *rlookup(const symbol_t *stab, int val)
{
        for( ; ; ++stab ) {
                if( !stab->key || stab->val == val )
                        return stab->key;
        }
}

/** Lookup table for gesture events
 */
static const symbol_t gesture_values[] = {
        { "doubletap",  GESTURE_DOUBLETAP },
        { "fpwakeup",   GESTURE_FPWAKEUP  },
        { NULL, -1 }
};

/** Lookup table for autosuspend policies
 *
 * @note These must match the hardcoded values in mce itself.
 */
static const symbol_t suspendpol_values[] = {
        { "disabled",           SUSPEND_POLICY_DISABLED            },
        { "enabled",            SUSPEND_POLICY_ENABLED             },
        { "early",              SUSPEND_POLICY_EARLY_ONLY          },
        { "disable_on_charger", SUSPEND_POLICY_DISABLE_ON_CHARGER  },
        { NULL, -1 }
};

/** Lookup table for cpu scaling governor overrides
 */
static const symbol_t governor_values[] = {
        { "automatic",    GOVERNOR_UNSET       },
        { "performance",  GOVERNOR_DEFAULT     },
        { "interactive",  GOVERNOR_INTERACTIVE },
        { NULL, -1 }
};

/** Lookup table for never blank options
 */
static const symbol_t never_blank_values[] = {
        { "enabled",   1 },
        { "disabled",  0 },
        { NULL, -1 }
};

/** Lookup table for button backlight options
 */
static const symbol_t button_backlight_values[] = {
        { "enabled",   1 },
        { "disabled",  0 },
        { NULL, -1 }
};

/** Lookup table for button backlight mode options
 */
static const symbol_t button_backlight_mode_values[] = {
        { "off",    MCE_BUTTON_BACKLIGHT_MODE_OFF    },
        { "on",     MCE_BUTTON_BACKLIGHT_MODE_ON     },
        { "policy", MCE_BUTTON_BACKLIGHT_MODE_POLICY },
        { NULL, -1 }
};

/** Lookup table for fake doubletap policies
 */
static const symbol_t fake_doubletap_values[] = {
        { "disabled",  0 },
        { "enabled",   1 },
        { NULL, -1 }
};

/** Lookup table for tklock autoblank policy values
 *
 * @note These must match the hardcoded values in mce itself.
 */
static const symbol_t tklockblank_values[] = {
        { "disabled",  1 },
        { "enabled",   0 },
        { NULL, -1 }
};

/** Lookup table for power key event values
 *
 * @note These must match the hardcoded values in mce itself.
 */
static const symbol_t powerkeyevent_lut[] =
{
        { "short",  MCE_POWERKEY_EVENT_SHORT_PRESS },
        { "long",   MCE_POWERKEY_EVENT_LONG_PRESS },
        { "double", MCE_POWERKEY_EVENT_DOUBLE_PRESS },
        { 0, -1 }
};

/** Convert power key event name to number passable to mce
 *
 * @param args string from user
 *
 * @return number passable to MCE, or terminate on error
 */
static int xmce_parse_powerkeyevent(const char *args)
{
        int res = lookup(powerkeyevent_lut, args);
        if( res < 0 ) {
                errorf("%s: not a valid power key event\n", args);
                exit(EXIT_FAILURE);
        }
        return res;
}

/** Lookup table for blanking inhibit modes
 *
 * @note These must match the hardcoded values in mce itself.
 */
static const symbol_t inhibitmode_lut[] =
{
        { "disabled",              0 },
        { "stay-on-with-charger",  1 },
        { "stay-dim-with-charger", 2 },
        { "stay-on",               3 },
        { "stay-dim",              4 },
        { 0, -1 }

};

/** Convert blanking inhibit mode name to number passable to MCE
 *
 * @param args string from user
 *
 * @return number passable to MCE, or terminate on error
 */
static int parse_inhibitmode(const char *args)
{
        int res = lookup(inhibitmode_lut, args);
        if( res < 0 ) {
                errorf("%s: not a valid inhibit mode value\n", args);
                exit(EXIT_FAILURE);
        }
        return res;
}

/** Convert blanking inhibit mode to human readable string
 *
 * @param value blanking inhibit mode from mce
 *
 * @return mode name, or NULL in case of errors
 */
static const char *repr_inhibitmode(int value)
{
        return rlookup(inhibitmode_lut, value);
}

/** Lookuptable for mce radio state bits */
static const symbol_t radio_states_lut[] =
{
        { "master",    MCE_RADIO_STATE_MASTER },
        { "cellular",  MCE_RADIO_STATE_CELLULAR },
        { "wlan",      MCE_RADIO_STATE_WLAN },
        { "bluetooth", MCE_RADIO_STATE_BLUETOOTH },
        { "nfc",       MCE_RADIO_STATE_NFC },
        { "fmtx",      MCE_RADIO_STATE_FMTX },
        { 0,           0 }
};

/** Convert comma separated list of radio state names into bitmask
 *
 * @param args radio state list from user
 *
 * @return bitmask passable to mce, or terminate on errors
 */
static unsigned xmce_parse_radio_states(const char *args)
{
        int   res = 0;
        char *tmp = strdup(args);
        int   bit;
        char *end;

        for( char *pos = tmp; pos; pos = end )
        {
                if( (end = strchr(pos, ',')) )
                        *end++ = 0;

                if( !(bit = lookup(radio_states_lut, pos)) ) {
                        errorf("%s: not a valid radio state\n", pos);
                        exit(EXIT_FAILURE);
                }

                res |= bit;
        }
        free(tmp);
        return (unsigned)res;
}

/** Lookuptable for enabled/disabled truth values */
static const symbol_t enabled_lut[] =
{
        { "enabled",   TRUE  },
        { "disabled",  FALSE },
        { "enable",    TRUE  },
        { "disable",   FALSE },
        { 0,           -1    }
};

/** Convert enable/disable string to boolean
 *
 * @param args string from user
 *
 * @return boolean passable to mce, or terminate on errors
 */
static gboolean xmce_parse_enabled(const char *args)
{
        int res = lookup(enabled_lut, args);
        if( res < 0 ) {
                errorf("%s: not a valid enable value\n", args);
                exit(EXIT_FAILURE);
        }
        return res != 0;
}

/** Convert string to integer
 *
 * @param args string from user
 *
 * @return integer number, or terminate on errors
 */
static int xmce_parse_integer(const char *args)
{
        char *end = 0;
        int   res = strtol(args, &end, 0);
        if( end <= args || *end != 0 ) {
                errorf("%s: not a valid integer value\n", args);
                exit(EXIT_FAILURE);
        }
        return res;
}

/** Convert string to memory limit page count
 *
 * @param args string from user
 *
 * @return integer number, or terminate on errors
 */
static int xmce_parse_memory_limit(const char *args)
{
        char     *end = 0;
        uint64_t  val = strtoull(args, &end, 0);
        if( end > args && *end != 0) {
                switch( *end ) {
                case 'k':
                        ++end;
                        val = (val << 10) / PAGE_SIZE;
                        break;
                case 'M':
                        ++end;
                        val = (val << 20) / PAGE_SIZE;
                        break;
                case 'G':
                        ++end;
                        val = (val << 30) / PAGE_SIZE;
                        break;
                default:
                        errorf("'%c' is not among supported modifiers 'kMG'\n", *end);
                        exit(EXIT_FAILURE);
                }
        }

        if( end <= args || *end != 0 ) {
                errorf("%s: not a valid integer value\n", args);
                exit(EXIT_FAILURE);
        }
        int res = (int)val;
        if( res < 0 || (uint64_t)res != val ) {
                errorf("limit range overflow\n");
                exit(EXIT_FAILURE);
        }
        return res;
}

/** Convert string to double
 *
 * @param args string from user
 *
 * @return double precision floating point number, or terminate on errors
 */
static double xmce_parse_double(const char *args)
{
        char   *end = 0;
        double  res = strtod(args, &end);
        if( end <= args || *end != 0 ) {
                errorf("%s: not a valid double value\n", args);
                exit(EXIT_FAILURE);
        }
        return res;
}

/** Convert a comma separated string in to gint array
 *
 * @param text string to split
 * @param len where to store number of elements in array
 *
 * @return array of gint type numbers
 */
static gint *parse_gint_array(const char *text, gint *len)
{
        gint   used = 0;
        gint   size = 0;
        gint  *data = 0;
        gchar *temp = 0;

        gchar *now, *zen;
        gint val;

        if( !text )
                goto EXIT;

        temp = g_strdup(text);
        size = 16;
        data = g_malloc(size * sizeof *data);

        for( now = zen = temp; *now; now = zen ) {
                val = strtol(now, &zen, 0);

                if( now == zen )
                        break;

                if( used == size )
                        data = g_realloc(data, (size *= 2) * sizeof *data);

                data[used++] = val;

                if( *zen == ',' )
                        ++zen;
        }

        size = used ? used : 1;
        data = g_realloc(data, size * sizeof *data);

EXIT:
        g_free(temp);

        return *len = used, data;
}

/** Convert string to struct timespec
 *
 * @param ts   [OUT] where to store time value
 * @param args [IN]  string from user
 *
 * @return TRUE if args was valid time value, or FALSE if not
 */
static gboolean mcetool_parse_timspec(struct timespec *ts, const char *args)
{
        gboolean ack = FALSE;
        double   tmp = 0;

        if( args && (tmp = strtod(args, 0)) > 0 ) {
                double s  = 0;
                double ns = modf(tmp, &s) * 1e9;
                ts->tv_sec  = (time_t)s;
                ts->tv_nsec = (long)ns;
                ack = TRUE;
        }

        return ack;
}

/** Parse comma separated value from give parse position
 *
 * The buffer parse position points to is cut from the
 * next comma (',') character. Parse position is updated
 * to the character following the comma, or to end of string.
 *
 * The parse position is neved moved beyond end of string,
 * thus the return value is always valid c-string.
 *
 * @param ppos pointer to parse position
 *
 * @return parse position
 */
static char *mcetool_parse_token(char **ppos)
{
        char *pos = *ppos;
        char *end = strchr(pos, ',');

        if( end )
                *end++ = 0;
        else
                end = strchr(pos, 0);

        *ppos = end;

        return pos;

}

/** Convert bitmap to human readable string via lookup table
 *
 * @param lut  array of symbol_t objects
 * @param mask mask of bits to convert
 * @param buff buffer to form string in
 * @param size size of buff
 *
 * @return buff, containing mask in human readable form
 */
static char *mcetool_format_bitmask(const symbol_t *lut, int mask,
                                    char *buff, size_t size)
{
        const char *none = rlookup(lut, 0) ?: "none";

        char *pos = buff;
        char *end = buff + size - 1;

        auto void add(const char *str)
        {
                if( pos > buff && pos < end )
                        *pos++ = ',';
                while( pos < end && *str )
                        *pos++ = *str++;
        }

        if( !mask ) {
                add(none);
                goto EXIT;
        }

        for( int bit = 1; bit > 0; bit <<= 1 ) {
                if( !(mask & bit) )
                        continue;

                const char *name = rlookup(lut, bit);
                if( name ) {
                        mask &= ~bit;
                        add(name);
                }
        }

        if( mask ) {
                char hex[32];
                snprintf(hex, sizeof hex, "0x%u", (unsigned)mask);
                add(hex);
        }
EXIT:
        *pos = 0;

        return buff;
}

/** Convert comma separated list of bit names into bitmask
 *
 * Note: the function will exit() if unknown bit names are given
 *
 * @param lut  array of symbol_t objects
 * @param args string with comma separated bit names
 *
 * @return bitmask of given bit names
 */
static unsigned mcetool_parse_bitmask(const symbol_t *lut, const char *args)
{
        const char *none = rlookup(lut, 0) ?: "none";

        unsigned  mask = 0;
        char     *work = 0;

        if( !args || !*args || !strcmp(args, none) )
                goto EXIT;

        if( !(work = strdup(args)) )
                goto EXIT;

        int   bit;
        char *end;

        for( char *pos = work; pos; pos = end )
        {
                if( (end = strpbrk(pos, ",|+")) )
                        *end++ = 0;

                if( !(bit = lookup(lut, pos)) ) {
                        errorf("%s: not a valid bit name\n", pos);
                        exit(EXIT_FAILURE);
                }

                mask |= bit;
        }

EXIT:
        free(work);

        return mask;
}

/** Common handling for special "values" for applicable --set-xxx options
 *
 * @return true if args was handled, false otherwise
 */
static bool mcetool_handle_common_args(const char *key, const char *args)
{
        bool handled = true;
        if( !g_strcmp0(args, "/reset") ) {
                if( !xmce_reset_settings(key) )
                        exit(EXIT_FAILURE);
        }
        else if( !g_strcmp0(args, "/key") ) {
                printf("%s\n", key);
        }
        else {
                handled = false;
        }
        return handled;
}

/** Common rejecting of special "values" for non-applicable --set-xxx options
 *
 * @return true if args was rejected, false otherwise
 */
static bool mcetool_reject_common_args(const char *args)
{
        if( !g_strcmp0(args, "/reset") || !g_strcmp0(args, "/key") ) {
                errorf("not a single persistent setting, '%s' is not applicable\n", args);
                return true;
        }
        return false;
}

/* ------------------------------------------------------------------------- *
 * leds
 * ------------------------------------------------------------------------- */

/** Array of led patterns that can be disabled/enabled */
static const char * const led_patterns[] =
{
        MCE_LED_PATTERN_BATTERY_CHARGING,
        MCE_LED_PATTERN_BATTERY_FULL,
        MCE_LED_PATTERN_COMMUNICATION_EVENT,
        MCE_LED_PATTERN_POWER_OFF,
        MCE_LED_PATTERN_POWER_ON,
        MCE_LED_PATTERN_CAMERA,
        MCE_LED_PATTERN_DEVICE_ON,
        MCE_LED_PATTERN_BATTERY_LOW,
        MCE_LED_PATTERN_COMMUNICATION_EVENT_BATTERY_FULL,
        MCE_LED_PATTERN_BATTERY_CHARGING_FLAT,
        MCE_LED_PATTERN_COMMON_NOTIFICATION,
        MCE_LED_PATTERN_COMMUNICATION_CALL,
        MCE_LED_PATTERN_COMMUNICATION_EMAIL,
        MCE_LED_PATTERN_COMMUNICATION_IM,
        MCE_LED_PATTERN_COMMUNICATION_SMS,
        MCE_LED_PATTERN_CSD_WHITE,
        MCE_LED_PATTERN_DISPLAY_BLANK_FAILED,
        MCE_LED_PATTERN_DISPLAY_UNBLANK_FAILED,
        MCE_LED_PATTERN_DISPLAY_SUSPEND_FAILED,
        MCE_LED_PATTERN_DISPLAY_RESUME_FAILED,
        MCE_LED_PATTERN_KILLING_LIPSTICK,
        MCE_LED_PATTERN_TOUCH_INPUT_BLOCKED,
        MCE_LED_PATTERN_DISPLAY_DIMMED,
        MCE_LED_PATTERN_SCANNING_FINGERPRINT,
        MCE_LED_PATTERN_FINGERPRINT_ACQUIRED,
        MCE_LED_PATTERN_PROXIMITY_COVERED,
        MCE_LED_PATTERN_PROXIMITY_UNCOVERING,
        MCE_LED_PATTERN_PROXIMITY_UNCOVERED,
        0
};

/** Predicate for: pattern can be disabled/enabled
 *
 * @param pattern  LED pattern name
 *
 * @return true if pattern can be enabled/disabled, false otherwise
 */
static bool is_configurable_pattern(const char *pattern)
{
        for( size_t i = 0; led_patterns[i]; ++i ) {
                if( !strcmp(led_patterns[i], pattern) )
                        return true;
        }
        return false;
}

/** Enable/Disable sw based led breathing
 */
static bool set_led_breathing_enabled(const char *args)
{
        const char *key = MCE_SETTING_LED_SW_BREATH_ENABLED;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Show current sw based led breathing enable setting
 */
static void get_led_breathing_enabled(void)
{
        gboolean    val = FALSE;
        const char *txt = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_LED_SW_BREATH_ENABLED, &val) )
                txt = val ? "enabled" : "disabled";

        printf("%-"PAD1"s %s\n", "Led breathing:", txt);
}

/** Set battery limit for sw based led breathing
 */
static bool set_led_breathing_limit(const char *args)
{
        const char *key = MCE_SETTING_LED_SW_BREATH_BATTERY_LIMIT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);

        if( val < 0 || val > 100 ) {
                errorf("%d: invalid battery limit value\n", val);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Show current battery limit for sw based led breathing
 */
static void get_led_breathing_limit(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_LED_SW_BREATH_BATTERY_LIMIT, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (%%)\n", "Led breathing battery limit:", txt);
}

/** Enable/Disable builtin mce led pattern
 *
 * @param pattern  The name of the pattern to activate/deactivate
 * @param activate true to activate pattern, false to deactivate pattern
 */
static bool set_led_pattern_enabled(const char *pattern, bool enable)
{
        char key[256];

        if( !is_configurable_pattern(pattern) ) {
                errorf("%s: not a configurable led pattern name\n", pattern);
                return false;
        }

        snprintf(key, sizeof key, MCE_SETTING_LED_PATH"/%s", pattern);
        return xmce_setting_set_bool(key, enable);
}

/** Enable LED feature
 */
static bool mcetool_do_enable_led(const char *arg)
{
        (void)arg;
        return xmce_ipc_no_reply(MCE_ENABLE_LED, DBUS_TYPE_INVALID);
}
/** Disable LED feature
 */
static bool mcetool_do_disable_led(const char *arg)
{
        (void)arg;
        return xmce_ipc_no_reply(MCE_DISABLE_LED, DBUS_TYPE_INVALID);
}

/** Enable a configurable LED pattern
 */
static bool mcetool_do_enable_pattern(const char *args)
{
        return set_led_pattern_enabled(args, true);
}

/** Disable a configurable LED pattern
 */
static bool mcetool_do_disable_led_pattern(const char *args)
{
        return set_led_pattern_enabled(args, false);
}

/** Show status of all configurable LED patterns
 */
static bool mcetool_show_led_patterns(const char *args)
{
        (void)args;
        char key[256];

        for( size_t i = 0; led_patterns[i]; ++i ) {
                snprintf(key, sizeof key, MCE_SETTING_LED_PATH"/%s",
                         led_patterns[i]);

                gboolean    val = FALSE;
                const char *txt = "unknown";

                if( xmce_setting_get_bool(key, &val) )
                        txt = val ? "enabled" : "disabled";
                printf("%-"PAD1"s %s\n", led_patterns[i], txt);
        }
        return true;
}

/** Activate a LED pattern
 */
static bool mcetool_do_activate_pattern(const char *args)
{
        return xmce_ipc_no_reply(MCE_ACTIVATE_LED_PATTERN,
                                 DBUS_TYPE_STRING, &args,
                                 DBUS_TYPE_INVALID);
}

/** Deactivate a LED pattern
 */
static bool mcetool_do_deactivate_pattern(const char *args)
{
        return xmce_ipc_no_reply(MCE_DEACTIVATE_LED_PATTERN,
                                 DBUS_TYPE_STRING, &args,
                                 DBUS_TYPE_INVALID);
}
/* ------------------------------------------------------------------------- *
 * mce verbosity
 * ------------------------------------------------------------------------- */

/** Lookup table for verbosity levels */
static const symbol_t verbosity_levels[] = {
        // official
        { "emerg",     LOG_EMERG   },
        { "alert",     LOG_ALERT   },
        { "crit",      LOG_CRIT    },
        { "err",       LOG_ERR     },
        { "warning",   LOG_WARNING },
        { "notice",    LOG_NOTICE  },
        { "info",      LOG_INFO    },
        { "debug",     LOG_DEBUG   },
        // aliases
        { "emergency", LOG_EMERG   },
        { "critical",  LOG_CRIT    },
        { "error",     LOG_ERR     },
        { "warn",      LOG_WARNING },
        { NULL,        -1          }
};

/** Set MCE verbosity level
 */
static bool xmce_set_verbosity(const char *arg)
{
        if( mcetool_reject_common_args(arg) )
                return false;

        dbus_int32_t val = lookup(verbosity_levels, arg);
        if( val < 0 ) {
                errorf("%s: invalid verbosity level\n", arg);
                return false;
        }

        return xmce_ipc_no_reply(MCE_VERBOSITY_REQ,
                                 DBUS_TYPE_INT32, &val,
                                 DBUS_TYPE_INVALID);
}

/** Show current MCE verbosity level
 */
static void xmce_get_verbosity(void)
{
        gint        val = 0;
        const char *txt = 0;

        if( xmce_ipc_int_reply(MCE_VERBOSITY_GET, &val, DBUS_TYPE_INVALID) )
                txt = rlookup(verbosity_levels, val);

        printf("%-"PAD1"s %s \n", "Verbosity level:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * color profile
 * ------------------------------------------------------------------------- */

/** Get and print available color profile ids
 */
static bool xmce_get_color_profile_ids(const char *arg)
{
        (void)arg;

        DBusMessage *rsp = NULL;
        DBusError    err = DBUS_ERROR_INIT;
        char       **arr = 0;
        int          len = 0;

        if( !xmce_ipc_message_reply(MCE_COLOR_PROFILE_IDS_GET, &rsp, DBUS_TYPE_INVALID) )
                goto EXIT;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &arr, &len,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

        printf("Available color profiles ids are: \n");
        for( int i = 0; i < len; ++i )
                printf("\t%s\n", arr[i]);

EXIT:

        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", MCE_COLOR_PROFILE_IDS_GET,
                        err.name, err.message);
                dbus_error_free(&err);
        }

        if( arr ) dbus_free_string_array(arr);
        if( rsp ) dbus_message_unref(rsp);

        return true;
}

/** Set color profile id
 *
 * Valid ids are printed by xmce_get_color_profile_ids(),
 * or --get-color-profile-ids option
 *
 * @param id The color profile id;
 */
static bool xmce_set_color_profile(const char *args)
{
        if( mcetool_reject_common_args(args) )
                return false;

        xmce_ipc_no_reply(MCE_COLOR_PROFILE_CHANGE_REQ,
                          DBUS_TYPE_STRING, &args,
                          DBUS_TYPE_INVALID);
        return true;
}

/** Get current color profile from mce and print it out
 */
static void xmce_get_color_profile(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_COLOR_PROFILE_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","Color profile:", str ?: "unknown");
        free(str);
}

/* ------------------------------------------------------------------------- *
 * battery stuff
 * ------------------------------------------------------------------------- */

#ifdef ENABLE_BATTERY_SIMULATION
static bool xmce_set_charger_type(const char *type)
{
        dbus_bool_t   ret = false;
        DBusError     err = DBUS_ERROR_INIT;
        DBusMessage  *rsp = 0;
        gboolean      ack = xmce_ipc(MCE_CHARGER_TYPE_REQ, &rsp,
                                     DBUS_TYPE_STRING, &type,
                                     DBUS_TYPE_INVALID);
        if( !ack || !rsp )
                goto EXIT;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_BOOLEAN, &ret,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("set %s: %s: %s\n", type, err.name, err.message);
                dbus_error_free(&err);
        }

        if( rsp ) dbus_message_unref(rsp);

        return ack && ret;
}

static bool xmce_set_charger_state(const char *state)
{
        dbus_bool_t   ret = false;
        DBusError     err = DBUS_ERROR_INIT;
        DBusMessage  *rsp = 0;
        gboolean      ack = xmce_ipc(MCE_CHARGER_STATE_REQ, &rsp,
                                     DBUS_TYPE_STRING, &state,
                                     DBUS_TYPE_INVALID);
        if( !ack || !rsp )
                goto EXIT;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_BOOLEAN, &ret,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("set %s: %s: %s\n", state, err.name, err.message);
                dbus_error_free(&err);
        }

        if( rsp ) dbus_message_unref(rsp);

        return ack && ret;
}

static bool xmce_set_battery_level(int level)
{
        dbus_bool_t   ret = false;
        DBusError     err = DBUS_ERROR_INIT;
        DBusMessage  *rsp = 0;
        dbus_int32_t  arg = level;
        gboolean      ack = xmce_ipc(MCE_BATTERY_LEVEL_REQ, &rsp,
                                     DBUS_TYPE_INT32, &arg,
                                     DBUS_TYPE_INVALID);
        if( !ack || !rsp )
                goto EXIT;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_BOOLEAN, &ret,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("set %d: %s: %s\n", level, err.name, err.message);
                dbus_error_free(&err);
        }

        if( rsp ) dbus_message_unref(rsp);

        return ack && ret;
}

static bool mcetool_do_set_charger_type(const char *arg)
{
        if( mcetool_reject_common_args(arg) )
                return false;

        const char * const lut[] = {
                MCE_CHARGER_TYPE_NONE,
                MCE_CHARGER_TYPE_USB,
                MCE_CHARGER_TYPE_DCP,
                MCE_CHARGER_TYPE_HVDCP,
                MCE_CHARGER_TYPE_CDP,
                MCE_CHARGER_TYPE_WIRELESS,
                MCE_CHARGER_TYPE_OTHER,
                0
        };

        for( size_t i = 0; ; ++i ) {
                if( !lut[i] ) {
                        errorf("%s: invalid charger type\n", arg);
                        return false;
                }
                if( !strcmp(lut[i], arg) )
                        break;
        }

        return xmce_set_charger_type(arg);
}

static bool mcetool_do_set_charger_state(const char *arg)
{
        if( mcetool_reject_common_args(arg) )
                return false;

        const char * const lut[] = {
                MCE_CHARGER_STATE_UNKNOWN,
                MCE_CHARGER_STATE_ON,
                MCE_CHARGER_STATE_OFF,
                0
        };

        for( size_t i = 0; ; ++i ) {
                if( !lut[i] ) {
                        errorf("%s: invalid charger state\n", arg);
                        return false;
                }
                if( !strcmp(lut[i], arg) )
                        break;
        }

        return xmce_set_charger_state(arg);
}

static bool mcetool_do_set_battery_level(const char *arg)
{
        if( mcetool_reject_common_args(arg) )
                return false;

        int level = xmce_parse_integer(arg);
        if( level < 0 ) {
                /* Battery simulation takes this as: unknown */
                level = -1;
        }
        else if( level > 100 ) {
                /* Battery simulation takes this as: 100% + Full */
                level = 101;
        }
        return xmce_set_battery_level(level);
}
#endif // ENABLE_BATTERY_SIMULATION

static void xmce_get_cable_state(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_USB_CABLE_STATE_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","Charger cable:", str ?: "unknown");
        free(str);
}

static void xmce_get_charger_state(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_CHARGER_STATE_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","Charger state:", str ?: "unknown");
        free(str);
}

static void xmce_get_battery_status(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_BATTERY_STATUS_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","Battery status:", str ?: "unknown");
        free(str);
}

static void xmce_get_battery_state(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_BATTERY_STATE_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","Battery state:", str ?: "unknown");
        free(str);
}

static void xmce_get_battery_level(void)
{
        gint num = -1;
        xmce_ipc_int_reply(MCE_BATTERY_LEVEL_GET, &num, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %d\n","Battery level:", num);
}

static bool xmce_set_charging_enable_limit(const char *args)
{
        const char *key = MCE_SETTING_CHARGING_LIMIT_ENABLE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        if( val < 0 || val > 100 ) {
                errorf("%d: invalid battery limit value\n", val);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

static void xmce_get_charging_enable_limit(void)
{
        gint val = 0;
        char txt[32];
        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_CHARGING_LIMIT_ENABLE, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (%%)\n", "Charging enable limit:", txt);
}

static bool xmce_set_charging_disable_limit(const char *args)
{
        const char *key = MCE_SETTING_CHARGING_LIMIT_DISABLE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        if( val < 0 || val > 100 ) {
                errorf("%d: invalid battery limit value\n", val);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

static void xmce_get_charging_disable_limit(void)
{
        gint val = 0;
        char txt[32];
        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_CHARGING_LIMIT_DISABLE, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (%%)\n", "Charging disable limit:", txt);
}

/** Lookuptable for enable/disable mode values */
static const symbol_t charging_mode_lut[] =
{
        { "disable",                     CHARGING_MODE_DISABLE },
        { "enable",                      CHARGING_MODE_ENABLE },
        { "apply-thresholds",            CHARGING_MODE_APPLY_THRESHOLDS },
        { "apply-thresholds-after-full", CHARGING_MODE_APPLY_THRESHOLDS_AFTER_FULL },
        { 0,                             -1    }
};

static bool xmce_set_charging_mode(const char *args)
{
        const char *key = MCE_SETTING_CHARGING_MODE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(charging_mode_lut, args);
        if( val == -1 ) {
                errorf("%s: invalid charging mode\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

static void xmce_get_charging_mode(void)
{
        gint val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_CHARGING_MODE, &val) )
                txt = rlookup(charging_mode_lut, val);
        printf("%-"PAD1"s %s\n", "Charging mode:", txt ?: "unknown");
}

static bool xmce_set_forced_charging_mode(const char *args)
{
        if( mcetool_reject_common_args(args) )
                return false;

        static const char * const lut[] = {
                MCE_FORCED_CHARGING_ENABLED,
                MCE_FORCED_CHARGING_DISABLED,
                NULL
        };
        for( size_t i = 0; ; ++i ) {
                if( lut[i] == NULL ) {
                        errorf("%s: invalid charging override value\n", args);
                        exit(EXIT_FAILURE);
                }
                if( !strcmp(lut[i], args) )
                        break;
        }
        xmce_ipc_no_reply(MCE_FORCED_CHARGING_REQ,
                          DBUS_TYPE_STRING, &args,
                          DBUS_TYPE_INVALID);
        return true;
}

static void xmce_get_forced_charging_mode(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_FORCED_CHARGING_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","Charging override:", str ?: "unknown");
        free(str);
}

static void xmce_get_battery_info(void)
{
        xmce_get_cable_state();
        xmce_get_charger_state();
        xmce_get_battery_level();
        xmce_get_battery_status();
        xmce_get_battery_state();
        xmce_get_charging_mode();
        xmce_get_forced_charging_mode();
        xmce_get_charging_enable_limit();
        xmce_get_charging_disable_limit();
}

/* ------------------------------------------------------------------------- *
 * notification states
 * ------------------------------------------------------------------------- */

/** Parse notification parameters from command line argument
 *
 * Expected input format is: "<delay>[,<name>[,<renew>]]"
 *
 * @param args   command line argument string
 * @param title  where to store name string
 * @param delay  where to store delay string converted to integer
 * @param renew  where to store delay string converted to integer, or NULL
 */
static void
xmce_parse_notification_args(const char   *args,
                             char        **title,
                             dbus_int32_t *delay,
                             dbus_int32_t *renew)
{
        char *work = 0;
        char *pos  = 0;
        char *arg;

        pos = work = strdup(args);

        arg = mcetool_parse_token(&pos);
        *title = strdup(*arg ? arg : "mcetool");

        arg = mcetool_parse_token(&pos);
        if( delay && *arg )
                *delay = xmce_parse_integer(arg);

        arg = mcetool_parse_token(&pos);
        if( renew && *arg )
                *renew = xmce_parse_integer(arg);

        free(work);
}

/** Start notification ui exception state
 */
static bool xmce_notification_begin(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);

        char         *title  = 0;
        dbus_int32_t  length = 2000;
        dbus_int32_t  renew  = -1;

        xmce_parse_notification_args(args, &title, &length, &renew);

        /* Note: length and limit ranges are enforced at mce side */

        xmce_ipc_no_reply(MCE_NOTIFICATION_BEGIN_REQ,
                          DBUS_TYPE_STRING, &title,
                          DBUS_TYPE_INT32 , &length,
                          DBUS_TYPE_INT32 , &renew,
                          DBUS_TYPE_INVALID);
        free(title);
        return true;
}

/** Stop notification ui exception state
 */
static bool xmce_notification_end(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);

        char         *title  = 0;
        dbus_int32_t  linger = 0;

        xmce_parse_notification_args(args, &title, &linger, 0);

        /* Note: linger range is enforced at mce side */

        xmce_ipc_no_reply(MCE_NOTIFICATION_END_REQ,
                          DBUS_TYPE_STRING, &title,
                          DBUS_TYPE_INT32 , &linger,
                          DBUS_TYPE_INVALID);
        free(title);
        return true;
}

/* ------------------------------------------------------------------------- *
 * radio states
 * ------------------------------------------------------------------------- */

/** Enable radios
 *
 * @param args string of comma separated radio state names
 */
static bool xmce_enable_radio(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        dbus_uint32_t mask = xmce_parse_radio_states(args);
        dbus_uint32_t data = mask;

        xmce_ipc_no_reply(MCE_RADIO_STATES_CHANGE_REQ,
                   DBUS_TYPE_UINT32, &data,
                   DBUS_TYPE_UINT32, &mask,
                   DBUS_TYPE_INVALID);

        return true;
}

/** Disable radios
 *
 * @param args string of comma separated radio state names
 */
static bool xmce_disable_radio(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        dbus_uint32_t mask = xmce_parse_radio_states(args);
        dbus_uint32_t data = 0;

        xmce_ipc_no_reply(MCE_RADIO_STATES_CHANGE_REQ,
                   DBUS_TYPE_UINT32, &data,
                   DBUS_TYPE_UINT32, &mask,
                   DBUS_TYPE_INVALID);

        return true;
}

/** Get current radio state from mce and print it out
 */
static void xmce_get_radio_states(void)
{
        guint mask = 0;

        if( !xmce_ipc_uint_reply(MCE_RADIO_STATES_GET, &mask, DBUS_TYPE_INVALID) ) {
                printf(" %-40s %s\n", "Radio states:", "unknown");
                return;
        }

        printf("Radio states:\n");

        printf("\t%-"PAD2"s %s\n", "Master:",
                mask & MCE_RADIO_STATE_MASTER ? "enabled (Online)" : "disabled (Offline)");

        printf("\t%-"PAD2"s %s\n",  "Cellular:",
                mask & MCE_RADIO_STATE_CELLULAR ? "enabled" : "disabled");

        printf("\t%-"PAD2"s %s\n", "WLAN:",
                mask & MCE_RADIO_STATE_WLAN ? "enabled" : "disabled");

        printf("\t%-"PAD2"s %s\n", "Bluetooth:",
                mask & MCE_RADIO_STATE_BLUETOOTH ? "enabled" : "disabled");

        printf("\t%-"PAD2"s %s\n", "NFC:",
                mask & MCE_RADIO_STATE_NFC ? "enabled" : "disabled");

        printf("\t%-"PAD2"s %s\n", "FM transmitter:",
                mask & MCE_RADIO_STATE_FMTX ? "enabled" : "disabled");
}

/* ------------------------------------------------------------------------- *
 * lpmui triggering
 * ------------------------------------------------------------------------- */

/** Lookuptable for mce radio state bits */
static const symbol_t lpmui_triggering_lut[] =
{
        { "from-pocket",  LPMUI_TRIGGERING_FROM_POCKET },
        { "hover-over",   LPMUI_TRIGGERING_HOVER_OVER  },
        { "disabled",     LPMUI_TRIGGERING_NONE        },
        { 0,              0                            }
};

/** Set automatic lpm ui triggering mode
 *
 * @param args string of comma separated lpm ui triggering names
 */
static bool xmce_set_lpmui_triggering(const char *args)
{
        const char *key = MCE_SETTING_TK_LPMUI_TRIGGERING;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int mask = mcetool_parse_bitmask(lpmui_triggering_lut, args);
        return xmce_setting_set_int(key, mask);
}

/** Get current lpm ui triggering mode from mce and print it out
 */
static void xmce_get_lpmui_triggering(void)
{
        gint mask = 0;
        char work[64] = "unknown";
        if( xmce_setting_get_int(MCE_SETTING_TK_LPMUI_TRIGGERING, &mask) )
                mcetool_format_bitmask(lpmui_triggering_lut, mask,
                                       work, sizeof work);

        printf("%-"PAD1"s %s\n", "LPM UI triggering:", work);
}
/* ------------------------------------------------------------------------- *
 * input_grab triggering
 * ------------------------------------------------------------------------- */

/** Lookuptable for mce radio state bits */
static const symbol_t input_grab_allowed_lut[] =
{
        { "ts",    MCE_INPUT_GRAB_ALLOW_TS   },
        { "kp",    MCE_INPUT_GRAB_ALLOW_KP   },
        { "none",  MCE_INPUT_GRAB_ALLOW_NONE },
        { 0,       0                         }
};

/** Set automatic lpm ui triggering mode
 *
 * @param args string of comma separated lpm ui triggering names
 */
static bool xmce_set_input_grab_allowed(const char *args)
{
        const char *key = MCE_SETTING_INPUT_GRAB_ALLOWED;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int mask = mcetool_parse_bitmask(input_grab_allowed_lut, args);
        return xmce_setting_set_int(key, mask);
}

/** Get current lpm ui triggering mode from mce and print it out
 */
static void xmce_get_input_grab_allowed(void)
{
        gint mask = 0;
        char work[64] = "unknown";
        if( xmce_setting_get_int(MCE_SETTING_INPUT_GRAB_ALLOWED, &mask) )
                mcetool_format_bitmask(input_grab_allowed_lut, mask,
                                       work, sizeof work);

        printf("%-"PAD1"s %s\n", "Input grab allowed:", work);
}

/* ------------------------------------------------------------------------- *
 * call state
 * ------------------------------------------------------------------------- */

/** Set call state
 *
 * Note: Faked call states get cancelled when mcetool exits. The
 *       --block option can be used keep mcetool connected to
 *       system bus.
 *
 * @param args string with callstate and calltype separated with ':'
 */
static bool xmce_set_call_state(const char *args)
{
        if( mcetool_reject_common_args(args) )
                return false;

        char *callstate = strdup(args);
        char *calltype  = strchr(callstate, ':');

        if( !calltype ) {
                errorf("%s: invalid call state value\n", args);
                exit(EXIT_FAILURE);
        }

        *calltype++ = 0;

        xmce_ipc_no_reply(MCE_CALL_STATE_CHANGE_REQ,
                          DBUS_TYPE_STRING, &callstate,
                          DBUS_TYPE_STRING, &calltype,
                          DBUS_TYPE_INVALID);

        free(callstate);
        return true;
}

/** Get current call state from mce and print it out
 */
static void xmce_get_call_state(void)
{
        const char  *callstate = 0;
        const char  *calltype  = 0;
        DBusMessage *rsp = NULL;
        DBusError    err = DBUS_ERROR_INIT;

        if( !xmce_ipc_message_reply(MCE_CALL_STATE_GET, &rsp, DBUS_TYPE_INVALID) )
                goto EXIT;

        dbus_message_get_args(rsp, &err,
                              DBUS_TYPE_STRING, &callstate,
                              DBUS_TYPE_STRING, &calltype,
                              DBUS_TYPE_INVALID);

EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", MCE_CALL_STATE_GET,
                       err.name, err.message);
                dbus_error_free(&err);
        }

        printf("%-"PAD1"s %s (%s)\n", "Call state (type):",
               callstate ?: "unknown",
               calltype ?:  "unknown");

        if( rsp ) dbus_message_unref(rsp);
}

/* ------------------------------------------------------------------------- *
 * button backlight
 * ------------------------------------------------------------------------- */

/** Set button backlight off delay
 *
 * @param args string that can be parsed to number
 */
static bool xmce_set_button_backlligut_off_delay(const char *args)
{
        const char *key = MCE_SETTING_BUTTONBACKLIGHT_OFF_DELAY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gint        val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Get current fingerprint wakeup allow delay
 */
static void xmce_get_button_backlligut_off_delay(void)
{
        const char *tag = "Button backlight off delay:";
        const char *key = MCE_SETTING_BUTTONBACKLIGHT_OFF_DELAY;
        gint        val = 0;
        char        txt[64];

        if( !xmce_setting_get_int(key, &val) )
                snprintf(txt, sizeof txt, "unknown");
        else
                snprintf(txt, sizeof txt, "%d [ms]", val);

        printf("%-"PAD1"s %s\n", tag, txt);
}

/** Set button backlight mode
 *
 * Note: The set mode gets cancelled when mcetool exits. The
 *       --block option can be used keep mcetool connected to
 *       system bus.
 *
 * @param args string with "off", "on", "policy"
 */
static bool xmce_set_button_backlight_mode(const char *args)
{
        if( mcetool_reject_common_args(args) )
                return false;

        dbus_int32_t val = lookup(button_backlight_mode_values, args);
        if( val < 0 ) {
                errorf("%s: invalid button backlight value\n", args);
                return false;
        }

        xmce_ipc_no_reply(MCE_BUTTON_BACKLIGHT_MODE_REQ,
                          DBUS_TYPE_INT32, &val,
                          DBUS_TYPE_INVALID);

        return true;
}

/** Set button backlight state
 *
 * Note: An enable gets cancelled when mcetool exits. The
 *       --block option can be used keep mcetool connected to
 *       system bus.
 *
 * @param args string with "enabled" / "disabled"
 */
static bool xmce_set_button_backlight(const char *args)
{
        if( mcetool_reject_common_args(args) )
                return false;

        int val = lookup(button_backlight_values, args);
        if( val < 0 ) {
                errorf("%s: invalid never blank value\n", args);
                return false;
        }

        dbus_bool_t enabled = (val != 0);

        xmce_ipc_no_reply(MCE_BUTTON_BACKLIGHT_CHANGE_REQ,
                          DBUS_TYPE_BOOLEAN, &enabled,
                          DBUS_TYPE_INVALID);

        return true;
}

/** Get current button backlight state from mce and print it out
 */
static void xmce_get_button_backlight(void)
{
        dbus_bool_t enabled = FALSE;
        DBusMessage *rsp = NULL;
        DBusError    err = DBUS_ERROR_INIT;

        if( !xmce_ipc_message_reply(MCE_BUTTON_BACKLIGHT_GET, &rsp, DBUS_TYPE_INVALID) )
                goto EXIT;

        dbus_message_get_args(rsp, &err,
                              DBUS_TYPE_BOOLEAN, &enabled,
                              DBUS_TYPE_INVALID);

EXIT:
        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", MCE_CALL_STATE_GET,
                       err.name, err.message);
                dbus_error_free(&err);
        }

        printf("%-"PAD1"s %s\n", "Button backlight:",
               rlookup(button_backlight_values, enabled));

        if( rsp ) dbus_message_unref(rsp);
}

/* ------------------------------------------------------------------------- *
 * display state
 * ------------------------------------------------------------------------- */

/** Set display state
 *
 * @param args display state; "on", "dim" or "off"
 */
static void xmce_set_display_state(const char *state)
{
        debugf("%s(%s)\n", __FUNCTION__, state);
        if( !strcmp(state, "on") )
                xmce_ipc_no_reply(MCE_DISPLAY_ON_REQ, DBUS_TYPE_INVALID);
        else if( !strcmp(state, "dim") )
                xmce_ipc_no_reply(MCE_DISPLAY_DIM_REQ, DBUS_TYPE_INVALID);
        else if( !strcmp(state, "off") )
                xmce_ipc_no_reply(MCE_DISPLAY_OFF_REQ, DBUS_TYPE_INVALID);
        else if( !strcmp(state, "lpm") )
                xmce_ipc_no_reply(MCE_DISPLAY_LPM_REQ, DBUS_TYPE_INVALID);
        else
                errorf("%s: invalid display state\n", state);
}

/** Get current display state from mce and print it out
 */
static void xmce_get_display_state(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_DISPLAY_STATUS_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","Display state:", str ?: "unknown");
        free(str);
}

/* ------------------------------------------------------------------------- *
 * display keepalive
 * ------------------------------------------------------------------------- */

/** Request display keepalive
 */
static bool xmce_prevent_display_blanking(const char *arg)
{
        (void) arg;

        debugf("%s()\n", __FUNCTION__);
        xmce_ipc_no_reply(MCE_PREVENT_BLANK_REQ, DBUS_TYPE_INVALID);
        return true;
}

/** Cancel display keepalive
 */
static bool xmce_allow_display_blanking(const char *arg)
{
        (void) arg;

        debugf("%s()\n", __FUNCTION__);
        xmce_ipc_no_reply(MCE_CANCEL_PREVENT_BLANK_REQ, DBUS_TYPE_INVALID);
        return true;
}

/** Lookup table for display blanking pause modes
 */
static const symbol_t blanking_pause_modes[] = {
        { "disabled",  BLANKING_PAUSE_MODE_DISABLED  },
        { "keep-on",   BLANKING_PAUSE_MODE_KEEP_ON   },
        { "allow-dim", BLANKING_PAUSE_MODE_ALLOW_DIM },
        { NULL,        -1                            }
};

/** Set display blank prevent mode setting
 *
 * @param args string that can be parsed to blank prevent mode
 */
static bool xmce_set_blank_prevent_mode(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_BLANKING_PAUSE_MODE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(blanking_pause_modes, args);
        if( val < 0 ) {
                errorf("%s: invalid display blank prevent mode\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current display blank prevent mode from mce and print it out
 */
static void xmce_get_blank_prevent_mode(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_DISPLAY_BLANKING_PAUSE_MODE, &val) )
                txt = rlookup(blanking_pause_modes, val);
        printf("%-"PAD1"s %s \n", "Display blank prevent mode:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * display brightness
 * ------------------------------------------------------------------------- */

/** Set display brightness
 *
 * @param args string that can be parsed to integer in [1 ... 5] range
 */
static bool xmce_set_display_brightness(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_BRIGHTNESS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);

        if( val < 1 || val > 100 ) {
                errorf("%d: invalid brightness value\n", val);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current display brightness from mce and print it out
 */
static void xmce_get_display_brightness(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_DISPLAY_BRIGHTNESS, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (1-100)\n", "Brightness:", txt);
}

/** Set statically defined dimmed display brightness
 *
 * @param args string that can be parsed to integer in [1 ... 100] range
 */
static bool xmce_set_dimmed_brightness_static(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_DIM_STATIC_BRIGHTNESS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);

        if( val < 1 || val > 100 ) {
                errorf("%d: invalid brightness value\n", val);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Show statically defined dimmed display brightness
 */
static void xmce_get_dimmed_brightness_static(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_DISPLAY_DIM_STATIC_BRIGHTNESS, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (1-100 percent of hw maximum)\n", "Dimmed brightness static:", txt);
}

/** Set dynamically defined dimmed display brightness
 *
 * @param args string that can be parsed to integer in [1 ... 100] range
 */
static bool xmce_set_dimmed_brightness_dynamic(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_DIM_DYNAMIC_BRIGHTNESS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);

        if( val < 1 || val > 100 ) {
                errorf("%d: invalid brightness value\n", val);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Show dynamically defined dimmed display brightness
 */
static void xmce_get_dimmed_brightness_dynamic(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_DISPLAY_DIM_DYNAMIC_BRIGHTNESS, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (1-100)\n", "Dimmed brightness maximum:", txt);
}

/** Set threshold for maximal dimming display via compositor
 *
 * @param args string that can be parsed to integer in [0 ... 100] range
 */
static bool xmce_set_compositor_dimming_hi(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_DIM_COMPOSITOR_HI;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);

        if( val < 0 || val > 100 ) {
                errorf("%d: invalid threshold value\n", val);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Set threshold for minimal dimming display via compositor
 *
 * @param args string that can be parsed to integer in [0 ... 100] range
 */
static bool xmce_set_compositor_dimming_lo(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_DIM_COMPOSITOR_LO;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);

        if( val < 0 || val > 100 ) {
                errorf("%d: invalid threshold value\n", val);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** show thresholds for dimming display via compositor
 */
static void xmce_get_compositor_dimming(void)
{
        gint hi = 0;
        gint lo = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_DISPLAY_DIM_COMPOSITOR_HI, &hi) )
                snprintf(txt, sizeof txt, "%d%s", (int)hi,
                         hi <= 0 ? "/disabled" : "");
        printf("%-"PAD1"s %s (0-100)\n",
               "Compositor dimming high threshold:", txt);

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_DISPLAY_DIM_COMPOSITOR_LO, &lo) )
                snprintf(txt, sizeof txt, "%d%s", (int)lo,
                         (lo <= hi) ? "/disabled" : "");
        printf("%-"PAD1"s %s (0-100)\n",
               "Compositor dimming low threshold:", txt);
}

/* ------------------------------------------------------------------------- *
 * cabc (content adaptive backlight control)
 * ------------------------------------------------------------------------- */

/** Set display brightness
 *
 * @param args cabc mode name
 */
static bool xmce_set_cabc_mode(const char *args)
{
        if( mcetool_reject_common_args(args) )
                return false;

        static const char * const lut[] = {
                MCE_CABC_MODE_OFF,
                MCE_CABC_MODE_UI,
                MCE_CABC_MODE_STILL_IMAGE,
                MCE_CABC_MODE_MOVING_IMAGE,
                0
        };

        for( size_t i = 0; ; ++i ) {
                if( !lut[i] ) {
                        errorf("%s: invalid cabc mode\n", args);
                        exit(EXIT_FAILURE);
                }
                if( !strcmp(lut[i], args) )
                        break;
        }

        xmce_ipc_no_reply(MCE_CABC_MODE_REQ,
                          DBUS_TYPE_STRING, &args,
                          DBUS_TYPE_INVALID);
        return true;
}

/** Get current cabc mode from mce and print it out
 */
static void xmce_get_cabc_mode(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_CABC_MODE_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","CABC mode:", str ?: "unknown");
        free(str);
}

/* ------------------------------------------------------------------------- *
 * config reset
 * ------------------------------------------------------------------------- */

static bool xmce_reset_settings(const char *args)
{
        if( !args )
                args = "/";

        xmce_ipc_no_reply(MCE_CONFIG_RESET,
                          DBUS_TYPE_STRING, &args,
                          DBUS_TYPE_INVALID);
        return true;
}

/* ------------------------------------------------------------------------- *
 * dim timeout
 * ------------------------------------------------------------------------- */

/** Set display dim timeout
 *
 * @param args string that can be parsed to integer
 */
static bool xmce_set_dim_timeout(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_DIM_TIMEOUT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Show current dim timeout
 */
static void xmce_get_dim_timeout(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_DISPLAY_DIM_TIMEOUT, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (seconds)\n", "Dim timeout:", txt);
}

/** Set display dim with kbd timeout
 *
 * @param args string that can be parsed to integer
 */
static bool xmce_set_dim_with_kbd_timeout(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_DIM_WITH_KEYBOARD_TIMEOUT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Show current dim with kbd timeout
 */
static void xmce_get_dim_with_kbd_timeout(void)
{
        gint val = 0;
        char txt[32] = "";

        if( !xmce_setting_get_int(MCE_SETTING_DISPLAY_DIM_WITH_KEYBOARD_TIMEOUT, &val) )
                strcpy(txt, "unknown");
        else if( val <= 0 )
                strcpy(txt, "use default");
        else
                snprintf(txt, sizeof txt, "%d", (int)val);

        printf("%-"PAD1"s %s (seconds)\n", "Dim with kbd timeout:", txt);
}

/** Set "allowed" display dim timeouts
 *
 * @param args string of comma separated integer numbers
 */

static bool xmce_set_dim_timeouts(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_DIM_TIMEOUT_LIST;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gint  len = 0;
        gint *arr = parse_gint_array(args, &len);

        if( len != 5 ) {
                errorf("%s: invalid dim timeout list\n", args);
                exit(EXIT_FAILURE);
        }
        for( gint i = 1; i < len; ++i ) {
                if( arr[i] <= arr[i-1] ) {
                        errorf("%s: dim timeout list not in ascending order\n", args);
                        exit(EXIT_FAILURE);
                }
        }

        bool ret = xmce_setting_set_int_array(key, arr, len);
        g_free(arr);
        return ret;
}

/** Get list of "allowed" dim timeouts from mce and print them out
 */
static void xmce_get_dim_timeouts(void)
{
        gint *vec = 0;
        gint  len = 0;

        xmce_setting_get_int_array(MCE_SETTING_DISPLAY_DIM_TIMEOUT_LIST,
                                   &vec, &len);
        printf("%-"PAD1"s [", "Allowed dim timeouts");
        for( gint i = 0; i < len; ++i )
                printf(" %d", vec[i]);
        printf(" ]\n");
        g_free(vec);
}

/* ------------------------------------------------------------------------- *
 * adaptive dimming timeout
 * ------------------------------------------------------------------------- */

/* Set adaptive dimming mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_adaptive_dimming_mode(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_ADAPTIVE_DIMMING;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Get current adaptive dimming mode from mce and print it out
 */
static void xmce_get_adaptive_dimming_mode(void)
{
        gboolean val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_bool(MCE_SETTING_DISPLAY_ADAPTIVE_DIMMING, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Adaptive dimming:", txt);
}

/** Set adaptive dimming time
 *
 * @param args string that can be parsed to integer
 */
static bool xmce_set_adaptive_dimming_time(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_ADAPTIVE_DIM_THRESHOLD;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Get current adaptive dimming time from mce and print it out
 */
static void xmce_get_adaptive_dimming_time(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_DISPLAY_ADAPTIVE_DIM_THRESHOLD, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (milliseconds)\n", "Adaptive dimming threshold:", txt);
}

/* ------------------------------------------------------------------------- *
 * exception lengths
 * ------------------------------------------------------------------------- */

static bool xmce_set_exception_length_call_in(const char *args)
{
        const char *key = MCE_SETTING_TK_EXCEPT_LEN_CALL_IN;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static bool xmce_set_exception_length_call_out(const char *args)
{
        const char *key = MCE_SETTING_TK_EXCEPT_LEN_CALL_OUT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static bool xmce_set_exception_length_alarm(const char *args)
{
        const char *key = MCE_SETTING_TK_EXCEPT_LEN_ALARM;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static bool xmce_set_exception_length_usb_connect(const char *args)
{
        const char *key = MCE_SETTING_TK_EXCEPT_LEN_USB_CONNECT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static bool xmce_set_exception_length_usb_dialog(const char *args)
{
        const char *key = MCE_SETTING_TK_EXCEPT_LEN_USB_DIALOG;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static bool xmce_set_exception_length_charger(const char *args)
{
        const char *key = MCE_SETTING_TK_EXCEPT_LEN_CHARGER;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static bool xmce_set_exception_length_battery(const char *args)
{
        const char *key = MCE_SETTING_TK_EXCEPT_LEN_BATTERY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static bool xmce_set_exception_length_jack_in(const char *args)
{
        const char *key = MCE_SETTING_TK_EXCEPT_LEN_JACK_IN;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static bool xmce_set_exception_length_jack_out(const char *args)
{
        const char *key = MCE_SETTING_TK_EXCEPT_LEN_JACK_OUT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static bool xmce_set_exception_length_camera(const char *args)
{
        const char *key = MCE_SETTING_TK_EXCEPT_LEN_CAMERA;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static bool xmce_set_exception_length_volume(const char *args)
{
        const char *key = MCE_SETTING_TK_EXCEPT_LEN_VOLUME;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static bool xmce_set_exception_length_activity(const char *args)
{
        const char *key = MCE_SETTING_TK_EXCEPT_LEN_ACTIVITY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static void xmce_get_exception_length(const char *tag, const char *key)
{
        gint val = 0;
        char txt[32];

        if( !xmce_setting_get_int(key, &val) )
                strcpy(txt, "unknown");
        else if( val <= 0 )
                strcpy(txt, "disabled");
        else
                snprintf(txt, sizeof txt, "%d ms", (int)val);
        printf("%-"PAD1"s %s\n", tag, txt);
}

static void xmce_get_exception_lengths(void)
{
        xmce_get_exception_length("Display on after incoming call",
                                  MCE_SETTING_TK_EXCEPT_LEN_CALL_IN);

        xmce_get_exception_length("Display on after outgoing call",
                                  MCE_SETTING_TK_EXCEPT_LEN_CALL_OUT);

        xmce_get_exception_length("Display on after alarm",
                                  MCE_SETTING_TK_EXCEPT_LEN_ALARM);

        xmce_get_exception_length("Display on at usb connect",
                                  MCE_SETTING_TK_EXCEPT_LEN_USB_CONNECT);

        xmce_get_exception_length("Display on at usb mode query",
                                  MCE_SETTING_TK_EXCEPT_LEN_USB_DIALOG);

        xmce_get_exception_length("Display on at charging start",
                                  MCE_SETTING_TK_EXCEPT_LEN_CHARGER);

        xmce_get_exception_length("Display on at battery full",
                                  MCE_SETTING_TK_EXCEPT_LEN_BATTERY);

        xmce_get_exception_length("Display on at jack insert",
                                  MCE_SETTING_TK_EXCEPT_LEN_JACK_IN);

        xmce_get_exception_length("Display on at jack remove",
                                  MCE_SETTING_TK_EXCEPT_LEN_JACK_OUT);

        xmce_get_exception_length("Display on at camera button",
                                  MCE_SETTING_TK_EXCEPT_LEN_CAMERA);

        xmce_get_exception_length("Display on at volume button",
                                  MCE_SETTING_TK_EXCEPT_LEN_VOLUME);

        xmce_get_exception_length("Display on activity extension",
                                  MCE_SETTING_TK_EXCEPT_LEN_ACTIVITY);
}

/* ------------------------------------------------------------------------- *
 * lid_sensor
 * ------------------------------------------------------------------------- */

/* Set filter lid with als mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_filter_lid_with_als(const char *args)
{
        const char *key = MCE_SETTING_TK_FILTER_LID_WITH_ALS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/* Get current filter lid with als mode from mce and print it out
 */
static void xmce_get_filter_lid_with_als(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_TK_FILTER_LID_WITH_ALS, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Filter lid with als:", txt);
}

/* Set limit for light als should report when lid is closed
 *
 * @param args string suitable for interpreting as lux value
 */
static bool xmce_set_filter_lid_als_limit(const char *args)
{
        const char *key = MCE_SETTING_TK_FILTER_LID_ALS_LIMIT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        if( val < 0 ) {
                errorf("%d: invalid lux value\n", val);
                return false;
        }
        return xmce_setting_set_int(key, val);
}

/* Get current filter lid als limit from mce and print it out
 */
static void xmce_get_filter_lid_als_limit(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_TK_FILTER_LID_ALS_LIMIT, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (lux)\n", "Lid closed als limit:", txt);
}

/* Set lid_sensor use mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_lid_sensor_mode(const char *args)
{
        const char *key = MCE_SETTING_TK_LID_SENSOR_ENABLED;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Get current lid_sensor mode from mce and print it out
 */
static void xmce_get_lid_sensor_mode(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_TK_LID_SENSOR_ENABLED, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Use lid sensor mode:", txt);
}

/** Lookup table for lid open actions
 */
static const symbol_t lid_open_actions[] = {
        { "disabled", LID_OPEN_ACTION_DISABLED },
        { "unblank",  LID_OPEN_ACTION_UNBLANK  },
        { "tkunlock", LID_OPEN_ACTION_TKUNLOCK },
        { NULL,       -1                       }
};

/** Set lid open actions
 *
 * @param args string that can be parsed to lid open actions
 */
static bool xmce_set_lid_open_actions(const char *args)
{
        const char *key = MCE_SETTING_TK_LID_OPEN_ACTIONS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(lid_open_actions, args);
        if( val < 0 ) {
                errorf("%s: invalid lid open actions\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current lid open actions from mce and print it out
 */
static void xmce_get_lid_open_actions(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_TK_LID_OPEN_ACTIONS, &val) )
                txt = rlookup(lid_open_actions, val);
        printf("%-"PAD1"s %s \n", "Lid open actions:", txt ?: "unknown");
}

/** Lookup table for lid close actions
 */
static const symbol_t lid_close_actions[] = {
        { "disabled", LID_CLOSE_ACTION_DISABLED },
        { "blank",    LID_CLOSE_ACTION_BLANK    },
        { "tklock",   LID_CLOSE_ACTION_TKLOCK   },
        { NULL,       -1                        }
};

/** Set lid close actions
 *
 * @param args string that can be parsed to lid close actions
 */
static bool xmce_set_lid_close_actions(const char *args)
{
        const char *key = MCE_SETTING_TK_LID_CLOSE_ACTIONS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(lid_close_actions, args);
        if( val < 0 ) {
                errorf("%s: invalid lid close actions\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current lid close actions from mce and print it out
 */
static void xmce_get_lid_close_actions(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_TK_LID_CLOSE_ACTIONS, &val) )
                txt = rlookup(lid_close_actions, val);
        printf("%-"PAD1"s %s \n", "Lid close actions:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * kbd slide
 * ------------------------------------------------------------------------- */

/** Lookup table for kbd slide open triggers
 */
static const symbol_t kbd_slide_open_triggers[] = {
        { "never",        KBD_OPEN_TRIGGER_NEVER        },
        { "always",       KBD_OPEN_TRIGGER_ALWAYS       },
        { "no-proximity", KBD_OPEN_TRIGGER_NO_PROXIMITY },
        { NULL,           -1                            }
};

/** Lookup table for kbd slide close triggers
 */
static const symbol_t kbd_slide_close_triggers[] = {
        { "never",        KBD_CLOSE_TRIGGER_NEVER      },
        { "always",       KBD_CLOSE_TRIGGER_ALWAYS     },
        { "after-open",   KBD_CLOSE_TRIGGER_AFTER_OPEN },
        { NULL,           -1                           }
};

/** Set kbd slide open trigger
 *
 * @param args trigger name
 */
static bool xmce_set_kbd_slide_open_trigger(const char *args)
{
        const char *key = MCE_SETTING_TK_KBD_OPEN_TRIGGER;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(kbd_slide_open_triggers, args);
        if( val < 0 ) {
                errorf("%s: invalid kbd slide open trigger\n", args);
                return false;
        }
        return xmce_setting_set_int(key, val);
}

/** Show current kbd slide open trigger
 */
static void xmce_get_kbd_slide_open_trigger(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_TK_KBD_OPEN_TRIGGER, &val) )
                txt = rlookup(kbd_slide_open_triggers, val);
        printf("%-"PAD1"s %s \n", "Kbd slide open trigger:", txt ?: "unknown");
}

/** Set kbd slide open actions
 *
 * @param args action name
 */
static bool xmce_set_kbd_slide_open_actions(const char *args)
{
        const char *key = MCE_SETTING_TK_KBD_OPEN_ACTIONS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(lid_open_actions, args);
        if( val < 0 ) {
                errorf("%s: invalid kbd slide open actions\n", args);
                return false;
        }
        return xmce_setting_set_int(key, val);
}

/** Show current kbd slide open actions
 */
static void xmce_get_kbd_slide_open_actions(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_TK_KBD_OPEN_ACTIONS, &val) )
                txt = rlookup(lid_open_actions, val);
        printf("%-"PAD1"s %s \n", "Kbd slide open actions:", txt ?: "unknown");
}

/** Set kbd slide close trigger
 *
 * @param args trigger name
 */
static bool xmce_set_kbd_slide_close_trigger(const char *args)
{
        const char *key = MCE_SETTING_TK_KBD_CLOSE_TRIGGER;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(kbd_slide_close_triggers, args);
        if( val < 0 ) {
                errorf("%s: invalid kbd slide close trigger\n", args);
                return false;
        }
        return xmce_setting_set_int(key, val);
}

/** Show current kbd slide close trigger
 */
static void xmce_get_kbd_slide_close_trigger(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_TK_KBD_CLOSE_TRIGGER, &val) )
                txt = rlookup(kbd_slide_close_triggers, val);
        printf("%-"PAD1"s %s \n", "Kbd slide close trigger:", txt ?: "unknown");
}

/** Set kbd slide close actions
 *
 * @param args string that can be parsed to kbd slide close actions
 */
static bool xmce_set_kbd_slide_close_actions(const char *args)
{
        const char *key = MCE_SETTING_TK_KBD_CLOSE_ACTIONS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(lid_close_actions, args);
        if( val < 0 ) {
                errorf("%s: invalid kbd slide close actions\n", args);
                return false;
        }
        return xmce_setting_set_int(key, val);
}

/** Show current kbd slide close actions
 */
static void xmce_get_kbd_slide_close_actions(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_TK_KBD_CLOSE_ACTIONS, &val) )
                txt = rlookup(lid_close_actions, val);
        printf("%-"PAD1"s %s \n", "Kbd slide close actions:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * orientation sensor
 * ------------------------------------------------------------------------- */

/** Set orientation sensor master toggle
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_orientation_sensor_mode(const char *args)
{
        const char *key = MCE_SETTING_ORIENTATION_SENSOR_ENABLED;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Show orientation sensor master toggle
 */
static void xmce_get_orientation_sensor_mode(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_ORIENTATION_SENSOR_ENABLED, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Use orientation sensor mode:", txt);
}

/** Set orientation change is activity toggle
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_orientation_change_is_activity(const char *args)
{
        const char *key = MCE_SETTING_ORIENTATION_CHANGE_IS_ACTIVITY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Show orientation change is activity toggle
 */
static void xmce_get_orientation_change_is_activity(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_ORIENTATION_CHANGE_IS_ACTIVITY, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Orientation change is activity:", txt);
}

/** Set flipover gesture detection toggle
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_flipover_gesture_detection(const char *args)
{
        const char *key = MCE_SETTING_FLIPOVER_GESTURE_ENABLED;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Show flipover gesture detection toggle
 */
static void xmce_get_flipover_gesture_detection(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_FLIPOVER_GESTURE_ENABLED, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Flipover gesture detection:", txt);
}

/* ------------------------------------------------------------------------- *
 * ps
 * ------------------------------------------------------------------------- */

/* Set ps use mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_ps_mode(const char *args)
{
        const char *key = MCE_SETTING_PROXIMITY_PS_ENABLED;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Get current ps mode from mce and print it out
 */
static void xmce_get_ps_mode(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_PROXIMITY_PS_ENABLED, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Use ps mode:", txt);
}

/* Set ps use mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_ps_on_demand(const char *args)
{
        const char *key = MCE_SETTING_PROXIMITY_ON_DEMAND;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Get current ps mode from mce and print it out
 */
static void xmce_get_ps_on_demand(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_PROXIMITY_ON_DEMAND, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Use ps on-demand:", txt);
}

/** Set ps can block touch input mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_ps_blocks_touch(const char *args)
{
        const char *key = MCE_SETTING_TK_PROXIMITY_BLOCKS_TOUCH;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Get current ps can block touch input mode and print it out
 */
static void xmce_get_ps_blocks_touch(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_TK_PROXIMITY_BLOCKS_TOUCH, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Touch can be blocked by ps:", txt);
}

/** Set ps acts as lid sensor mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_ps_acts_as_lid(const char *args)
{
        const char *key = MCE_SETTING_PROXIMITY_PS_ACTS_AS_LID;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Get current ps acts as lid mode and print it out
 */
static void xmce_get_ps_acts_as_lid(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_PROXIMITY_PS_ACTS_AS_LID, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "PS acts as LID sensor:", txt);
}

/* ------------------------------------------------------------------------- *
 * ps uncover delay
 * ------------------------------------------------------------------------- */

static bool xmce_set_ps_uncover_delay_sub(const char *key, const char *args)
{
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        if( val < MCE_MINIMUM_TK_PROXIMITY_DELAY ||
            val > MCE_MAXIMUM_TK_PROXIMITY_DELAY ) {
                errorf("%s: invalid proximity uncover delay\n", args);
                return false;
        }
        return xmce_setting_set_int(key, val);
}

static void xmce_get_ps_uncover_delay_sub(const char *tag, const char *key)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(key, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (ms)\n", tag, txt);
}

/** Set default proximity sensor uncover delay
 *
 * @param args string that can be parsed to integer
 */
static bool xmce_set_default_ps_uncover_delay(const char *args)
{
        return xmce_set_ps_uncover_delay_sub(MCE_SETTING_TK_PROXIMITY_DELAY_DEFAULT,
                                             args);
}

/** Set default proximity sensor uncover delay
 *
 * @param args string that can be parsed to integer
 */
static bool xmce_set_incall_ps_uncover_delay(const char *args)
{
        return xmce_set_ps_uncover_delay_sub(MCE_SETTING_TK_PROXIMITY_DELAY_INCALL,
                                             args);
}

/** Get proximity sensor uncover delays and print them out
 */
static void xmce_get_ps_uncover_delay(void)
{
        xmce_get_ps_uncover_delay_sub("Default ps uncover delay:",
                                      MCE_SETTING_TK_PROXIMITY_DELAY_DEFAULT);

        xmce_get_ps_uncover_delay_sub("In-call ps uncover delay:",
                                      MCE_SETTING_TK_PROXIMITY_DELAY_INCALL);
}

/* ------------------------------------------------------------------------- *
 * inactivity shutdown delay
 * ------------------------------------------------------------------------- */

/** Set inactivity shutdown delay
 *
 * @param args string that can be parsed to integer
 */
static bool xmce_set_inactivity_shutdown_delay(const char *args)
{
        const char *key = MCE_SETTING_INACTIVITY_SHUTDOWN_DELAY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);

        if( val != 0 && val < MCE_MINIMUM_INACTIVITY_SHUTDOWN_DELAY ) {
                errorf("%s: invalid inactivity shutdown delay\n", args);
                return false;
        }

        return xmce_setting_set_int(key, val);
}

/** Get inactivity shutdown delay and print it out
 */
static void xmce_get_inactivity_shutdown_delay(void)
{
        const char *key = MCE_SETTING_INACTIVITY_SHUTDOWN_DELAY;
        gint val = 0;
        char txt[32];

        if( !xmce_setting_get_int(key, &val) )
                strcpy(txt, "unknown");
        else if( val < MCE_MINIMUM_INACTIVITY_SHUTDOWN_DELAY )
                strcpy(txt, "disabled");
        else
                snprintf(txt, sizeof txt, "%d (s)", (int)val);

        printf("%-"PAD1"s %s\n", "Inactivity shutdown delay:", txt);
}

/* ------------------------------------------------------------------------- *
 * als
 * ------------------------------------------------------------------------- */

/* Set als autobrightness mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_als_autobrightness(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_ALS_AUTOBRIGHTNESS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Get current als autobrightness from mce and print it out
 */
static void xmce_get_als_autobrightness(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_DISPLAY_ALS_AUTOBRIGHTNESS, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Use als autobrightness:", txt);
}

/* Set als use mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_als_mode(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_ALS_ENABLED;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Get current als mode from mce and print it out
 */
static void xmce_get_als_mode(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_DISPLAY_ALS_ENABLED, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Use als mode:", txt);
}

/** Check that given ALS input filter name is valid
 */
static bool xmce_is_als_filter_name(const char *name)
{
        const char * const lut[] = {
                "disabled",
                "median",
        };

        for( size_t i = 0; i < G_N_ELEMENTS(lut); ++i ) {
                if( !strcmp(lut[i], name) )
                        return true;
        }

        fprintf(stderr, "%s: not a valid als input filter name", name);
        return false;
}

/* Set als input filter
 *
 * @param args string suitable for interpreting as filter name
 */
static bool xmce_set_als_input_filter(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_ALS_INPUT_FILTER;
        if( mcetool_handle_common_args(key, args) )
                return true;

        if( !xmce_is_als_filter_name(args) )
                return false;

        return xmce_setting_set_string(key, args);
}

/** Get current als input filter from mce and print it out
 */
static void xmce_get_als_input_filter(void)
{
        gchar *val = 0;
        char txt[32] = "unknown";
        if( xmce_setting_get_string(MCE_SETTING_DISPLAY_ALS_INPUT_FILTER, &val) )
                snprintf(txt, sizeof txt, "%s", val);
        printf("%-"PAD1"s %s\n", "Active als input filter:", txt);
        g_free(val);
}

/* Set als sample time
 *
 * @param args string suitable for interpreting as filter name
 */
static bool xmce_set_als_sample_time(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_ALS_SAMPLE_TIME;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);

        if( val < ALS_SAMPLE_TIME_MIN || val > ALS_SAMPLE_TIME_MAX ) {
                errorf("%d: invalid als sample time value\n", val);
                return false;
        }

        return xmce_setting_set_int(key, val);
}

/** Get current als sample time from mce and print it out
 */
static void xmce_get_als_sample_time(void)
{
        gint val = 0;
        char txt[32] = "unknown";
        if( xmce_setting_get_int(MCE_SETTING_DISPLAY_ALS_SAMPLE_TIME, &val) )
                snprintf(txt, sizeof txt, "%d", val);
        printf("%-"PAD1"s %s\n", "Sample time for als filtering:", txt);
}

/* ------------------------------------------------------------------------- *
 * autolock
 * ------------------------------------------------------------------------- */

/* Set autolock mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_autolock_mode(const char *args)
{
        const char *key = MCE_SETTING_TK_AUTOLOCK_ENABLED;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Get current autolock mode from mce and print it out
 */
static void xmce_get_autolock_mode(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_TK_AUTOLOCK_ENABLED, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Touchscreen/Keypad autolock:", txt);
}

/* Set autolock delay
 *
 * @param args string suitable for interpreting as time in msec
 */
static bool xmce_set_autolock_delay(const char *args)
{
        const char *key = MCE_SETTING_TK_AUTOLOCK_DELAY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gint val = (int)(xmce_parse_double(args) * 1000.0);

        if( val < MINIMUM_AUTOLOCK_DELAY || val > MAXIMUM_AUTOLOCK_DELAY ) {
                errorf("%d: invalid autolock delay\n", val);
                return false;
        }

        return xmce_setting_set_int(key, val);
}

/** Get current autolock delay from mce and print it out
 */
static void xmce_get_autolock_delay(void)
{
        gint val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_int(MCE_SETTING_TK_AUTOLOCK_DELAY, &val) )
                snprintf(txt, sizeof txt, "%g [s]", val / 1000.0);
        printf("%-"PAD1"s %s\n", "Touchscreen/Keypad autolock delay:", txt);
}

/* ------------------------------------------------------------------------- *
 * devicelock_in_lockscreen
 * ------------------------------------------------------------------------- */

static bool interactive_confirmation(const char *positive)
{
        if( !isatty(STDIN_FILENO) ) {
                printf("\nstdin is not a tty\n");
                return false;
        }

        char buff[64];

        fflush(stdout);

        if( !fgets(buff, sizeof buff, stdin) ) {
                printf("\n");
                return false;
        }

        buff[strcspn(buff, "\r\n")] = 0;

        return !strcmp(buff, positive);
}

/* Set devicelock_in_lockscreen mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_devicelock_in_lockscreen(const char *args)
{
        const char *key = MCE_SETTING_TK_DEVICELOCK_IN_LOCKSCREEN;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);

        /* Make it a bit more difficult to enable the setting
         * accidentally */
        if( val ) {
                printf("Setting devicelock-in-lockscreen=enabled can make\n"
                       "the device unlockabe via normal touch interaction\n"
                       "\n"
                       "Are you sure you want to continue (yes/NO): ");
                if( !interactive_confirmation("yes") ) {
                        printf("operation canceled\n");
                        return false;
                }
        }

        return xmce_setting_set_bool(key, val);
}

/** Get current devicelock_in_lockscreen mode from mce and print it out
 */
static void xmce_get_devicelock_in_lockscreen(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_TK_DEVICELOCK_IN_LOCKSCREEN, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Devicelock is in lockscreen:", txt);
}

/* ------------------------------------------------------------------------- *
 * lock screen animation
 * ------------------------------------------------------------------------- */

/* Set lock screen animation enabled/disabled
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_lockscreen_unblank_animation(const char *args)
{
        const char *key = MCE_SETTING_TK_LOCKSCREEN_ANIM_ENABLED;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/* Show current lock screen animation enabled/disabled
 */
static void xmce_get_lockscreen_unblank_animation(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_TK_LOCKSCREEN_ANIM_ENABLED, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Lockscreen unblank animations:", txt);
}

/* ------------------------------------------------------------------------- *
 * blank timeout
 * ------------------------------------------------------------------------- */

/** Set display blanking timeout
 *
 * @param args string that can be parsed to integer
 */
static bool xmce_set_blank_timeout(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_BLANK_TIMEOUT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Set display blanking from lockscreen timeout
 *
 * @param args string that can be parsed to integer
 */
static bool xmce_set_blank_from_lockscreen_timeout(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_BLANK_FROM_LOCKSCREEN_TIMEOUT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Set display blanking from lpm-on timeout
 *
 * @param args string that can be parsed to integer
 */
static bool xmce_set_blank_from_lpm_on_timeout(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_BLANK_FROM_LPM_ON_TIMEOUT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Set display blanking from lpm-off timeout
 *
 * @param args string that can be parsed to integer
 */
static bool xmce_set_blank_from_lpm_off_timeout(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_BLANK_FROM_LPM_OFF_TIMEOUT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Helper for outputting blank timeout settings
 */
static void xmce_get_blank_timeout_sub(const char *tag, const char *key)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(key, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (seconds)\n", tag, txt);
}

/** Get current blank timeouts from mce and print it out
 */
static void xmce_get_blank_timeout(void)
{
        xmce_get_blank_timeout_sub("Blank timeout:",
                                   MCE_SETTING_DISPLAY_BLANK_TIMEOUT);

        xmce_get_blank_timeout_sub("Blank from lockscreen:",
                                   MCE_SETTING_DISPLAY_BLANK_FROM_LOCKSCREEN_TIMEOUT);

        xmce_get_blank_timeout_sub("Blank from lpm-on:",
                                   MCE_SETTING_DISPLAY_BLANK_FROM_LPM_ON_TIMEOUT);

        xmce_get_blank_timeout_sub("Blank from lpm-off:",
                                   MCE_SETTING_DISPLAY_BLANK_FROM_LPM_OFF_TIMEOUT);
}

/* ------------------------------------------------------------------------- *
 * powerkey
 * ------------------------------------------------------------------------- */

/** Trigger a powerkey event
 *
 * @param type The type of event to trigger; valid types:
 *             "short", "double", "long"
 */
static bool xmce_powerkey_event(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = xmce_parse_powerkeyevent(args);
        if( val < 0 ) {
                errorf("%s: invalid power key event\n", args);
                exit(EXIT_FAILURE);
        }
        /* com.nokia.mce.request.req_trigger_powerkey_event */
        dbus_uint32_t data = val;
        xmce_ipc_no_reply(MCE_TRIGGER_POWERKEY_EVENT_REQ,
                          DBUS_TYPE_UINT32, &data,
                          DBUS_TYPE_INVALID);
        return true;
}

/** Lookup table for powerkey wakeup policies
 */
static const symbol_t powerkey_action[] = {
        { "never",      PWRKEY_ENABLE_NEVER         },
        { "always",     PWRKEY_ENABLE_ALWAYS        },
        { "proximity",  PWRKEY_ENABLE_NO_PROXIMITY  },
        { "proximity2", PWRKEY_ENABLE_NO_PROXIMITY2 },
        { NULL,         -1                          }
};

/** Set powerkey wakeup mode
 *
 * @param args string that can be parsed to powerkey wakeup mode
 */
static bool xmce_set_powerkey_action(const char *args)
{
        const char *key = MCE_SETTING_POWERKEY_MODE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(powerkey_action, args);
        if( val < 0 ) {
                errorf("%s: invalid powerkey policy value\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current powerkey wakeup mode from mce and print it out
 */
static void xmce_get_powerkey_action(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_POWERKEY_MODE, &val) )
                txt = rlookup(powerkey_action, val);
        printf("%-"PAD1"s %s \n", "Powerkey wakeup policy:", txt ?: "unknown");
}

/** Lookup table for powerkey blanking modess
 */
static const symbol_t powerkey_blanking[] = {
        { "off", PWRKEY_BLANK_TO_OFF },
        { "lpm", PWRKEY_BLANK_TO_LPM },
        { NULL,  -1                  }
};

/** Set powerkey wakeup mode
 *
 * @param args string that can be parsed to powerkey wakeup mode
 */
static bool xmce_set_powerkey_blanking(const char *args)
{
        const char *key = MCE_SETTING_POWERKEY_BLANKING_MODE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(powerkey_blanking, args);
        if( val < 0 ) {
                errorf("%s: invalid powerkey blanking value\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current powerkey wakeup mode from mce and print it out
 */
static void xmce_get_powerkey_blanking(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_POWERKEY_BLANKING_MODE, &val) )
                txt = rlookup(powerkey_blanking, val);
        printf("%-"PAD1"s %s \n", "Powerkey blanking mode:", txt ?: "unknown");
}

/** Set powerkey long press delay
 *
 * @param args string that can be parsed to number
 */
static bool xmce_set_powerkey_long_press_delay(const char *args)
{
        const char *key = MCE_SETTING_POWERKEY_LONG_PRESS_DELAY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gint        val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Get current powerkey long press delay
 */
static void xmce_get_powerkey_long_press_delay(void)
{
        const char *tag = "Powerkey long press delay:";
        const char *key = MCE_SETTING_POWERKEY_LONG_PRESS_DELAY;
        gint        val = 0;
        char        txt[64];

        if( !xmce_setting_get_int(key, &val) )
                snprintf(txt, sizeof txt, "unknown");
        else
                snprintf(txt, sizeof txt, "%d [ms]", val);

        printf("%-"PAD1"s %s\n", tag, txt);
}

/** Set powerkey double press delay
 *
 * @param args string that can be parsed to number
 */
static bool xmce_set_powerkey_double_press_delay(const char *args)
{
        const char *key = MCE_SETTING_POWERKEY_DOUBLE_PRESS_DELAY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gint        val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Get current powerkey double press delay
 */
static void xmce_get_powerkey_double_press_delay(void)
{
        const char *tag = "Powerkey double press delay:";
        const char *key = MCE_SETTING_POWERKEY_DOUBLE_PRESS_DELAY;
        gint        val = 0;
        char        txt[64];

        if( !xmce_setting_get_int(key, &val) )
                snprintf(txt, sizeof txt, "unknown");
        else
                snprintf(txt, sizeof txt, "%d [ms]", val);

        printf("%-"PAD1"s %s\n", tag, txt);
}

/** Action name is valid predicate
 */
static bool xmce_is_powerkey_action(const char *name)
{
        static const char * const lut[] =
        {
                "blank",
                "tklock",
                "devlock",
                "shutdown",
                "vibrate",
                "unblank",
                "tkunlock",
                "tkunlock2",
                "dbus1",
                "dbus2",
                "dbus3",
                "dbus4",
                "dbus5",
                "dbus6",
                "dbus7",
                "dbus8",
                "dbus9",
                "dbus10",
                "nop",
        };

        for( size_t i = 0; i < G_N_ELEMENTS(lut); ++i ) {
                if( !strcmp(lut[i], name) )
                        return true;
        }

        return false;
}

/** Comma separated list of action names is valid predicate
 */
static bool xmce_is_powerkey_action_mask(const char *names)
{
        bool valid = true;

        char *work = strdup(names);

        char *pos = work;

        while( *pos ) {
                char *name = mcetool_parse_token(&pos);
                if( xmce_is_powerkey_action(name) )
                        continue;
                fprintf(stderr, "invalid powerkey action: '%s'\n", name);
                valid = false;
        }

        free(work);

        return valid;
}

/** Helper for setting powerkey action mask settings
 */
static bool xmce_set_powerkey_action_mask(const char *key, const char *names)
{
        if( mcetool_handle_common_args(key, names) )
                return true;

        if( names && *names && !xmce_is_powerkey_action_mask(names) )
                exit(EXIT_FAILURE);

        return xmce_setting_set_string(key, names);
}

/** Set actions to perform on single power key press from display off
 */
static bool xmce_set_powerkey_actions_while_display_off_single(const char *args)
{
        return xmce_set_powerkey_action_mask(MCE_SETTING_POWERKEY_ACTIONS_SINGLE_OFF,
                                             args);
}

/** Set actions to perform on double power key press from display off
 */
static bool xmce_set_powerkey_actions_while_display_off_double(const char *args)
{
        return xmce_set_powerkey_action_mask(MCE_SETTING_POWERKEY_ACTIONS_DOUBLE_OFF,
                                             args);
}

/** Set actions to perform on long power key press from display off
 */
static bool xmce_set_powerkey_actions_while_display_off_long(const char *args)
{
        return xmce_set_powerkey_action_mask(MCE_SETTING_POWERKEY_ACTIONS_LONG_OFF,
                                             args);
}

/** Set actions to perform on single power key press from display on
 */
static bool xmce_set_powerkey_actions_while_display_on_single(const char *args)
{
        return xmce_set_powerkey_action_mask(MCE_SETTING_POWERKEY_ACTIONS_SINGLE_ON,
                                             args);
}

/** Set actions to perform on double power key press from display on
 */
static bool xmce_set_powerkey_actions_while_display_on_double(const char *args)
{
        xmce_set_powerkey_action_mask(MCE_SETTING_POWERKEY_ACTIONS_DOUBLE_ON,
                                      args);
        return true;
}

/** Set actions to perform on long power key press from display on
 */
static bool xmce_set_powerkey_actions_while_display_on_long(const char *args)
{
        xmce_set_powerkey_action_mask(MCE_SETTING_POWERKEY_ACTIONS_LONG_ON,
                                      args);
        return true;
}

static const char * const gesture_actions_key[POWERKEY_ACTIONS_GESTURE_COUNT] =
{
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE0,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE1,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE2,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE3,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE4,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE5,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE6,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE7,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE8,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE9,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE10,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE11,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE12,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE13,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE14,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE15,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE16,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE17,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE18,
        MCE_SETTING_POWERKEY_ACTIONS_GESTURE19,
};

/** Set actions to perform on touchscreen gestures
 */
static bool xmce_set_touchscreen_gesture_action(const char *args)
{
        char *work = strdup(args);
        char *conf = work;
        char *gesture = mcetool_parse_token(&conf);

        int id = lookup(gesture_values, gesture);
        if( id < 0 )
                id = xmce_parse_integer(gesture);

        if( id < 0 || id >= (int)G_N_ELEMENTS(gesture_actions_key) ) {
                fprintf(stderr, "invalid gesture id: '%s'\n", work);
                return false;
        }

        xmce_set_powerkey_action_mask(gesture_actions_key[id], conf);

        free(work);

        return true;
}

/** Helper for getting powerkey action mask settings
 */
static void xmce_get_powerkey_action_mask(const char *key, const char *tag)
{
        gchar *val = 0;
        xmce_setting_get_string(key, &val);
        printf("\t%-"PAD2"s %s\n", tag,
               val ? *val ? val : "(none)" : "unknown");
        g_free(val);
}

/* Show current powerkey action mask settings */
static void xmce_get_powerkey_action_masks(void)
{
        printf("Powerkey press from display on:\n");
        xmce_get_powerkey_action_mask(MCE_SETTING_POWERKEY_ACTIONS_SINGLE_ON,
                                      "single");
        xmce_get_powerkey_action_mask(MCE_SETTING_POWERKEY_ACTIONS_DOUBLE_ON,
                                      "double");
        xmce_get_powerkey_action_mask(MCE_SETTING_POWERKEY_ACTIONS_LONG_ON,
                                      "long");

        printf("Powerkey press from display off:\n");
        xmce_get_powerkey_action_mask(MCE_SETTING_POWERKEY_ACTIONS_SINGLE_OFF,
                                      "single");
        xmce_get_powerkey_action_mask(MCE_SETTING_POWERKEY_ACTIONS_DOUBLE_OFF,
                                      "double");
        xmce_get_powerkey_action_mask(MCE_SETTING_POWERKEY_ACTIONS_LONG_OFF,
                                      "long");

        printf("Touchscreen gestures:\n");
        for( size_t id = 0; id < G_N_ELEMENTS(gesture_actions_key); ++id) {
                char temp[32];
                const char *gesture = rlookup(gesture_values, (int)id);
                if( !gesture ) {
                        snprintf(temp, sizeof temp, "gesture%zu", id);
                        gesture = temp;
                }
                xmce_get_powerkey_action_mask(gesture_actions_key[id], gesture);
        }
}

/** Validate dbus action parameters given by the user
 */
static bool xmce_is_powerkey_dbus_action(const char *conf)
{
        bool valid = true;

        char *tmp = strdup(conf);
        char *pos = tmp;

        const char *arg = mcetool_parse_token(&pos);

        if( *arg && !*pos ) {
                // single item == argument to use for signal
        }
        else {
                const char *destination = arg;
                const char *object      = mcetool_parse_token(&pos);
                const char *interface   = mcetool_parse_token(&pos);
                const char *member      = mcetool_parse_token(&pos);

                // string argument is optional
                const char *argument    = mcetool_parse_token(&pos);

                /* NOTE: libdbus will call abort() if invalid parameters are
                 *       passed to  dbus_message_new_method_call() function.
                 *       We do not want values that can crash mce to
                 *       end up in persitently stored settings
                 */

                /* 1st try to validate given parameters ... */

                if( !dbus_validate_bus_name(destination, 0) ) {
                        fprintf(stderr, "invalid service name: '%s'\n",
                               destination);
                        valid = false;
                }
                if( !dbus_validate_path(object, 0) ) {
                        fprintf(stderr, "invalid object path: '%s'\n",
                               object);
                        valid = false;
                }
                if( !dbus_validate_interface(interface, 0) ) {
                        fprintf(stderr, "invalid interface: '%s'\n",
                               interface);
                        valid = false;
                }
                if( !dbus_validate_member(member, 0) ) {
                        fprintf(stderr, "invalid method name: '%s'\n",
                                member);
                        valid = false;
                }
                if( !dbus_validate_utf8(argument, 0) ) {
                        fprintf(stderr, "invalid argument string: '%s'\n",
                               argument);
                        valid = false;
                }

                /* ... then use the presumed safe parameters to create
                 * a dbus method call object -> if there is some
                 * reason for dbus_message_new_method_call() to abort,
                 * it happens within mcetool, not mce itself.
                 */

                if( valid ) {
                        DBusMessage *msg =
                                dbus_message_new_method_call(destination,
                                                             object,
                                                             interface,
                                                             member);
                        if( msg )
                                dbus_message_unref(msg);
                }
        }

        free(tmp);

        return valid;
}

static const char * const powerkey_dbus_action_key[] =
{
        MCE_SETTING_POWERKEY_DBUS_ACTION1,
        MCE_SETTING_POWERKEY_DBUS_ACTION2,
        MCE_SETTING_POWERKEY_DBUS_ACTION3,
        MCE_SETTING_POWERKEY_DBUS_ACTION4,
        MCE_SETTING_POWERKEY_DBUS_ACTION5,
        MCE_SETTING_POWERKEY_DBUS_ACTION6,
        MCE_SETTING_POWERKEY_DBUS_ACTION7,
        MCE_SETTING_POWERKEY_DBUS_ACTION8,
        MCE_SETTING_POWERKEY_DBUS_ACTION9,
        MCE_SETTING_POWERKEY_DBUS_ACTION10,
};

/** Helper for setting dbus action config
 */
static bool xmce_set_powerkey_dbus_action(const char *args)
{
        char *work = strdup(args);
        char *conf = work;

        size_t action_id = xmce_parse_integer(mcetool_parse_token(&conf)) - 1;

        if( action_id >= G_N_ELEMENTS(powerkey_dbus_action_key) ) {
                fprintf(stderr, "invalid dbus action id: '%s'\n", work);
                return false;
        }

        const char *key = powerkey_dbus_action_key[action_id];

        if( mcetool_handle_common_args(key, conf) )
                return true;

        if( conf && *conf && !xmce_is_powerkey_dbus_action(conf) )
                return false;

        bool ret = xmce_setting_set_string(key, conf);

        free(work);

        return ret;
}
/** Helper for showing current dbus action config
 */
static void xmce_get_powerkey_dbus_action(size_t action_id)
{
        gchar *val = 0;

        if( action_id >= G_N_ELEMENTS(powerkey_dbus_action_key) )
                goto cleanup;

        const char *key = powerkey_dbus_action_key[action_id];

        if( !xmce_setting_get_string(key, &val) )
                goto cleanup;

        if( !val )
                goto cleanup;

        char *pos = val;
        char *arg = mcetool_parse_token(&pos);

        char tmp[64];
        snprintf(tmp, sizeof tmp, "dbus%zu",
                 action_id + 1);

        if( *arg && !*pos ) {
                printf("\t%-"PAD2"s send signal with arg '%s'\n", tmp, arg);
        }
        else {
                const char *destination = arg;
                const char *object      = mcetool_parse_token(&pos);
                const char *interface   = mcetool_parse_token(&pos);
                const char *member      = mcetool_parse_token(&pos);
                const char *argument    = mcetool_parse_token(&pos);

                printf("\t%-"PAD2"s make method call:\n", tmp);
                printf("\t%-"PAD2"s   %s '%s'\n", "", "destination", destination);
                printf("\t%-"PAD2"s   %s '%s'\n", "", "object", object);
                printf("\t%-"PAD2"s   %s '%s'\n", "", "interface", interface);
                printf("\t%-"PAD2"s   %s '%s'\n", "", "member", member);
                printf("\t%-"PAD2"s   %s '%s'\n", "", "argument",
                       *argument ? argument : "N/A");;
        }

cleanup:
        g_free(val);
}

/** Show current configuration for powerkey dbus actions
 */
static void xmce_get_powerkey_dbus_actions(void)
{
        printf("Powerkey D-Bus actions:\n");
        size_t actions = G_N_ELEMENTS(powerkey_dbus_action_key);
        for( size_t action_id = 0; action_id < actions; ++action_id )
                xmce_get_powerkey_dbus_action(action_id);
}

/** Set powerkey proximity override press count
 *
 * @param args string that can be parsed to number
 */
static bool xmce_set_ps_override_count(const char *args)
{
        const char *key = MCE_SETTING_POWERKEY_PS_OVERRIDE_COUNT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gint        val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Get current powerkey proximity override press count
 */
static void xmce_get_ps_override_count(void)
{
        const char *tag = "Powerkey ps override count:";
        const char *key = MCE_SETTING_POWERKEY_PS_OVERRIDE_COUNT;
        gint        val = 0;
        char        txt[64];

        if( !xmce_setting_get_int(key, &val) )
                snprintf(txt, sizeof txt, "unknown");
        else if( val <= 0 )
                snprintf(txt, sizeof txt, "disabled");
        else
                snprintf(txt, sizeof txt, "%d", val);

        printf("%-"PAD1"s %s\n", tag, txt);
}

/** Set powerkey proximity override press timeout
 *
 * @param args string that can be parsed to number
 */
static bool xmce_set_ps_override_timeout(const char *args)
{
        const char *key = MCE_SETTING_POWERKEY_PS_OVERRIDE_TIMEOUT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gint        val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Get current powerkey proximity override press timeout
 */
static void xmce_get_ps_override_timeout(void)
{
        const char *tag = "Powerkey ps override timeout:";
        const char *key = MCE_SETTING_POWERKEY_PS_OVERRIDE_TIMEOUT;
        gint        val = 0;
        char        txt[64];

        if( !xmce_setting_get_int(key, &val) )
                snprintf(txt, sizeof txt, "unknown");
        else if( val <= 0 )
                snprintf(txt, sizeof txt, "disabled");
        else
                snprintf(txt, sizeof txt, "%d [ms]", val);

        printf("%-"PAD1"s %s\n", tag, txt);
}

/* ------------------------------------------------------------------------- *
 * display off request override
 * ------------------------------------------------------------------------- */

/** Lookup table for display off request override values
 */
static const symbol_t display_off_override[] = {
        { "disabled",   DISPLAY_OFF_OVERRIDE_DISABLED    },
        { "use-lpm",    DISPLAY_OFF_OVERRIDE_USE_LPM     },
        { "only-blank", DISPLAY_OFF_OVERRIDE_ONLY_BLANK  },
        { NULL,         -1                               }
};

/** Set display off override
 *
 * @param args string that can be parsed to display off override value
 */
static bool xmce_set_display_off_override(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_OFF_OVERRIDE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(display_off_override, args);
        if( val < 0 ) {
                errorf("%s: invalid display off override value\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current display off override from mce and print it out
 */
static void xmce_get_display_off_override(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_DISPLAY_OFF_OVERRIDE, &val) )
                txt = rlookup(display_off_override, val);
        printf("%-"PAD1"s %s \n", "Display off override mode:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * volkey input policy
 * ------------------------------------------------------------------------- */

/** Lookup table for volkey input policies
 */
static const symbol_t volkey_input_policies[] = {
        { "default",    VOLKEY_POLICY_DEFAULT    },
        { "media-only", VOLKEY_POLICY_MEDIA_ONLY },
        { NULL, -1 }
};

/** Set volkey input policy
 *
 * @param args input policy name
 */
static bool xmce_set_volkey_policy(const char *args)
{
        const char *key = MCE_SETTING_TK_VOLKEY_POLICY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(volkey_input_policies, args);
        if( val < 0 ) {
                errorf("%s: invalid volkey input policy value\n", args);
                return false;
        }
        return xmce_setting_set_int(key, val);
}

/** Show current volkey input policy
 */
static void xmce_get_volkey_policy(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_TK_VOLKEY_POLICY, &val) )
                txt = rlookup(volkey_input_policies, val);
        printf("%-"PAD1"s %s \n", "Volumekey input policy:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * doubletab
 * ------------------------------------------------------------------------- */

/** Set doubletap mode
 *
 * @param args string that can be parsed to doubletap mode
 */
static bool xmce_set_doubletap_mode(const char *args)
{
    char mode[256];

    if( !strcmp(args, "disabled") )
        args = "";
    else if( !strcmp(args, "show-unlock-screen") )
        args = "unblank";
    else if( !strcmp(args, "unlock") )
        args = "unblank,tkunlock";

    snprintf(mode, sizeof mode, "4,%s", args);
    return xmce_set_touchscreen_gesture_action(mode);
}

/** Lookup table for doubletap wakeup policies
 *
 * @note These must match the hardcoded values in mce itself.
 */
static const symbol_t doubletap_wakeup[] = {
        { "never",     DBLTAP_ENABLE_NEVER },
        { "always",    DBLTAP_ENABLE_ALWAYS },
        { "proximity", DBLTAP_ENABLE_NO_PROXIMITY },
        { NULL, -1 }
};

/** Set doubletap wakeup mode
 *
 * @param args string that can be parsed to doubletap wakeup mode
 */
static bool xmce_set_doubletap_wakeup(const char *args)
{
        const char *key = MCE_SETTING_DOUBLETAP_MODE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(doubletap_wakeup, args);
        if( val < 0 ) {
                errorf("%s: invalid doubletap policy value\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current doubletap wakeup mode from mce and print it out
 */
static void xmce_get_doubletap_wakeup(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_DOUBLETAP_MODE, &val) )
                txt = rlookup(doubletap_wakeup, val);
        printf("%-"PAD1"s %s \n", "Double-tap wakeup policy:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * fingerprint
 * ------------------------------------------------------------------------- */

/** Lookup table for fingerprint wakeup policies
 *
 * @note These must match the hardcoded values in mce itself.
 */
static const symbol_t fingerprint_wakeup[] = {
        { "never",     FPWAKEUP_ENABLE_NEVER },
        { "always",    FPWAKEUP_ENABLE_ALWAYS },
        { "proximity", FPWAKEUP_ENABLE_NO_PROXIMITY },
        { NULL, -1 }
};

/** Set fingerprint wakeup mode
 *
 * @param args string that can be parsed to fingerprint wakeup mode
 */
static bool xmce_set_fingerprint_wakeup_mode(const char *args)
{
        const char *key = MCE_SETTING_FPWAKEUP_MODE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(fingerprint_wakeup, args);
        if( val < 0 ) {
                errorf("%s: invalid fingerprint policy value\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current fingerprint wakeup mode from mce and print it out
 */
static void xmce_get_fingerprint_wakeup_mode(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_FPWAKEUP_MODE, &val) )
                txt = rlookup(fingerprint_wakeup, val);
        printf("%-"PAD1"s %s \n", "Fingerprint wakeup policy:",
               txt ?: "unknown");
}

/** Set fingerprint wakeup allow delay
 *
 * @param args string that can be parsed to number
 */
static bool xmce_set_fingerprint_wakeup_allow_delay(const char *args)
{
        const char *key = MCE_SETTING_FPWAKEUP_ALLOW_DELAY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gint        val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Get current fingerprint wakeup allow delay
 */
static void xmce_get_fingerprint_wakeup_allow_delay(void)
{
        const char *tag = "Fingerprint wakeup allow delay:";
        const char *key = MCE_SETTING_FPWAKEUP_ALLOW_DELAY;
        gint        val = 0;
        char        txt[64];

        if( !xmce_setting_get_int(key, &val) )
                snprintf(txt, sizeof txt, "unknown");
        else
                snprintf(txt, sizeof txt, "%d [ms]", val);

        printf("%-"PAD1"s %s\n", tag, txt);
}

/** Set fingerprint wakeup triggering delay
 *
 * @param args string that can be parsed to number
 */
static bool xmce_set_fingerprint_wakeup_trigger_delay(const char *args)
{
        const char *key = MCE_SETTING_FPWAKEUP_TRIGGER_DELAY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gint        val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Get current fingerprint wakeup triggering delay
 */
static void xmce_get_fingerprint_wakeup_trigger_delay(void)
{
        const char *tag = "Fingerprint wakeup triggering delay:";
        const char *key = MCE_SETTING_FPWAKEUP_TRIGGER_DELAY;
        gint        val = 0;
        char        txt[64];

        if( !xmce_setting_get_int(key, &val) )
                snprintf(txt, sizeof txt, "unknown");
        else
                snprintf(txt, sizeof txt, "%d [ms]", val);

        printf("%-"PAD1"s %s\n", tag, txt);
}

/** Set fingerprint wakeup throttle delay
 *
 * @param args string that can be parsed to number
 */
static bool xmce_set_fingerprint_wakeup_throttle_delay(const char *args)
{
        const char *key = MCE_SETTING_FPWAKEUP_THROTTLE_DELAY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gint        val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

/** Get current fingerprint wakeup throttle delay
 */
static void xmce_get_fingerprint_wakeup_throttle_delay(void)
{
        const char *tag = "Fingerprint wakeup throttle delay:";
        const char *key = MCE_SETTING_FPWAKEUP_THROTTLE_DELAY;
        gint        val = 0;
        char        txt[64];

        if( !xmce_setting_get_int(key, &val) )
                snprintf(txt, sizeof txt, "unknown");
        else
                snprintf(txt, sizeof txt, "%d [ms]", val);

        printf("%-"PAD1"s %s\n", tag, txt);
}

/* ------------------------------------------------------------------------- *
 * psm (power saving mode)
 * ------------------------------------------------------------------------- */

/* Set power saving mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_power_saving_mode(const char *args)
{
        const char *key = MCE_SETTING_EM_ENABLE_PSM;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Get current power saving mode from mce and print it out
 */
static void xmce_get_power_saving_mode(void)
{
        gboolean mode  = 0;
        gboolean state = 0;
        char txt1[32] = "unknown";
        char txt2[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_EM_ENABLE_PSM, &mode) )
                snprintf(txt1, sizeof txt1, "%s", mode ? "enabled" : "disabled");

        if( xmce_ipc_bool_reply(MCE_PSM_STATE_GET, &state, DBUS_TYPE_INVALID) )
                snprintf(txt2, sizeof txt2, "%s", state ? "active" : "inactive");

        printf("%-"PAD1"s %s (%s)\n", "Power saving mode:", txt1, txt2);
}

/** Set power saving mode threshold
 *
 * @param args string that can be parsed to integer
 */
static bool xmce_set_psm_threshold(const char *args)
{
        const char *key = MCE_SETTING_EM_PSM_THRESHOLD;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);

        if( val < 0 || val > 100 ) {
                errorf("%d: invalid psm threshold value\n", val);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current power saving threshold from mce and print it out
 */
static void xmce_get_psm_threshold(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_EM_PSM_THRESHOLD, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (%%)\n", "PSM threshold:", txt);
}

/* Set forced power saving mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_forced_psm(const char *args)
{
        const char *key = MCE_SETTING_EM_FORCED_PSM;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Get current forced power saving mode from mce and print it out
 */
static void xmce_get_forced_psm(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_EM_FORCED_PSM, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Forced power saving mode:", txt);
}

/* ------------------------------------------------------------------------- *
 * lpm (low power mode)
 * ------------------------------------------------------------------------- */

/* Set display low power mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_low_power_mode(const char *args)
{
        const char *key = MCE_SETTING_USE_LOW_POWER_MODE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/** Get current low power mode state from mce and print it out
 */
static void xmce_get_low_power_mode(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_USE_LOW_POWER_MODE, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Use low power mode:", txt);
}

/* ------------------------------------------------------------------------- *
 * blanking inhibit
 * ------------------------------------------------------------------------- */

static bool xmce_set_inhibit_mode(const char *args)
{
        const char *key = MCE_SETTING_BLANKING_INHIBIT_MODE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = parse_inhibitmode(args);
        return xmce_setting_set_int(key, val);
}

/** Get current blanking inhibit mode from mce and print it out
 */
static void xmce_get_inhibit_mode(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_BLANKING_INHIBIT_MODE, &val) )
                txt = repr_inhibitmode(val);
        printf("%-"PAD1"s %s \n", "Blank inhibit:", txt ?: "unknown");
}

/** Lookup table kbd slide inhibit modes */
static const symbol_t kbd_slide_inhibitmode_lut[] =
{
        { "disabled",           KBD_SLIDE_INHIBIT_OFF                },
        { "stay-on-when-open",  KBD_SLIDE_INHIBIT_STAY_ON_WHEN_OPEN  },
        { "stay-dim-when-open", KBD_SLIDE_INHIBIT_STAY_DIM_WHEN_OPEN },
        { 0,                    -1                                   }

};

/** Set kbd slide inhibit mode
 *
 * @param args name of inhibit mode
 */
static bool xmce_set_kbd_slide_inhibit_mode(const char *args)
{
        const char *key = MCE_SETTING_KBD_SLIDE_INHIBIT;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(kbd_slide_inhibitmode_lut, args);
        if( val < 0 ) {
                errorf("%s: Invalid kbd slide blank inhibit mode\n", args);
                return false;
        }

        return xmce_setting_set_int(key, val);
}

/** Show current kbd slide inhibit mode
 */
static void xmce_get_kbd_slide_inhibit_mode(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_KBD_SLIDE_INHIBIT, &val) )
                txt = rlookup(kbd_slide_inhibitmode_lut, val);
        printf("%-"PAD1"s %s \n", "Kbd slide blank inhibit:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * lipstick killer
 * ------------------------------------------------------------------------- */

static bool xmce_set_lipstick_core_delay(const char *args)
{
        const char *key = MCE_SETTING_LIPSTICK_CORE_DELAY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static void xmce_get_lipstick_core_delay(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_LIPSTICK_CORE_DELAY, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (seconds)\n", "Lipstick core delay:", txt);
}

/* ------------------------------------------------------------------------- *
 * brightness fade settings
 * ------------------------------------------------------------------------- */

static bool xmce_set_brightness_fade_default(const char *args)
{
        const char *key = MCE_SETTING_BRIGHTNESS_FADE_DEFAULT_MS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}
static bool xmce_set_brightness_fade_dimming(const char *args)
{
        const char *key = MCE_SETTING_BRIGHTNESS_FADE_DIMMING_MS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}
static bool xmce_set_brightness_fade_als(const char *args)
{
        const char *key = MCE_SETTING_BRIGHTNESS_FADE_ALS_MS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}
static bool xmce_set_brightness_fade_blank(const char *args)
{
        const char *key = MCE_SETTING_BRIGHTNESS_FADE_BLANK_MS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}
static bool xmce_set_brightness_fade_unblank(const char *args)
{
        const char *key = MCE_SETTING_BRIGHTNESS_FADE_UNBLANK_MS;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);
        return xmce_setting_set_int(key, val);
}

static void xmce_get_brightness_fade_helper(const char *title, const char *key)
{
        gint val = 0;
        char txt[32];
        strcpy(txt, "unknown");
        if( xmce_setting_get_int(key, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (milliseconds)\n", title, txt);
}

static void xmce_get_brightness_fade(void)
{
        xmce_get_brightness_fade_helper("Brightness fade [def]:",
                                        MCE_SETTING_BRIGHTNESS_FADE_DEFAULT_MS);
        xmce_get_brightness_fade_helper("Brightness fade [dim]:",
                                        MCE_SETTING_BRIGHTNESS_FADE_DIMMING_MS);
        xmce_get_brightness_fade_helper("Brightness fade [als]:",
                                        MCE_SETTING_BRIGHTNESS_FADE_ALS_MS);
        xmce_get_brightness_fade_helper("Brightness fade [blank]:",
                                        MCE_SETTING_BRIGHTNESS_FADE_BLANK_MS);
        xmce_get_brightness_fade_helper("Brightness fade [unblank]:",
                                        MCE_SETTING_BRIGHTNESS_FADE_UNBLANK_MS);
}

/* ------------------------------------------------------------------------- *
 * memnotify limit settings
 * ------------------------------------------------------------------------- */

static bool xmce_set_memnotify_warning_used(const char *args)
{
        const char *key = MCE_SETTING_MEMNOTIFY_WARNING_USED;
        if( mcetool_handle_common_args(key, args) )
                return true;

        return xmce_setting_set_int(key, xmce_parse_memory_limit(args));
}

static bool xmce_set_memnotify_warning_active(const char *args)
{
        const char *key = MCE_SETTING_MEMNOTIFY_WARNING_ACTIVE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        return xmce_setting_set_int(key, xmce_parse_memory_limit(args));
}

static bool xmce_set_memnotify_critical_used(const char *args)
{
        const char *key = MCE_SETTING_MEMNOTIFY_CRITICAL_USED;
        if( mcetool_handle_common_args(key, args) )
                return true;

        return xmce_setting_set_int(key, xmce_parse_memory_limit(args));
}

static bool xmce_set_memnotify_critical_active(const char *args)
{
        const char *key = MCE_SETTING_MEMNOTIFY_CRITICAL_ACTIVE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        return xmce_setting_set_int(key, xmce_parse_memory_limit(args));
}

static void xmce_get_memnotify_helper(const char *title, const char *key)
{
        gint val = 0;
        if( !xmce_setting_get_int(key, &val) )
                printf("%-"PAD1"s %s\n", title, "unknown");
        else if( val <= 0 )
                printf("%-"PAD1"s %s\n", title, "disabled");
        else {
                char txt[32];
                snprintf(txt, sizeof txt, "%d", (int)val);
                uint64_t bytes = PAGE_SIZE * (uint64_t)val;
                printf("%-"PAD1"s %s (ram pages = %.1f MB)\n",
                       title, txt, bytes / (1024.0 * 1024.0));
        }
}

static void xmce_get_memnotify_limits(void)
{
        xmce_get_memnotify_helper("Memory use warning [used]:",
                                  MCE_SETTING_MEMNOTIFY_WARNING_USED);

        xmce_get_memnotify_helper("Memory use warning [active]:",
                                  MCE_SETTING_MEMNOTIFY_WARNING_ACTIVE);

        xmce_get_memnotify_helper("Memory use critical [used]:",
                                  MCE_SETTING_MEMNOTIFY_CRITICAL_USED);

        xmce_get_memnotify_helper("Memory use critical [active]:",
                                  MCE_SETTING_MEMNOTIFY_CRITICAL_ACTIVE);
}

static void xmce_get_memnotify_level(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_MEMORY_LEVEL_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","Memory use level:", str ?: "unknown");
        free(str);
}

/* ------------------------------------------------------------------------- *
 * input policy
 * ------------------------------------------------------------------------- */

/* Set input policy mode
 *
 * @param args string suitable for interpreting as enabled/disabled
 */
static bool xmce_set_input_policy_mode(const char *args)
{
        const char *key = MCE_SETTING_TK_INPUT_POLICY_ENABLED;
        if( mcetool_handle_common_args(key, args) )
                return true;

        gboolean val = xmce_parse_enabled(args);
        return xmce_setting_set_bool(key, val);
}

/* Show input policy mode
 */
static void xmce_get_input_policy_mode(void)
{
        gboolean val = 0;
        char txt[32] = "unknown";

        if( xmce_setting_get_bool(MCE_SETTING_TK_INPUT_POLICY_ENABLED, &val) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n", "Input grab policy:", txt);
}

/* ------------------------------------------------------------------------- *
 * touch input unblocking
 * ------------------------------------------------------------------------- */

static bool xmce_set_touch_unblock_delay(const char *args)
{
        const char *key = MCE_SETTING_TOUCH_UNBLOCK_DELAY;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = xmce_parse_integer(args);

        if( val <= 0 ) {
                errorf("%d: invalid touch unblock delay\n", val);
                return false;
        }
        return xmce_setting_set_int(key, val);
}

static void xmce_get_touch_unblock_delay(void)
{
        gint val = 0;
        char txt[32];

        strcpy(txt, "unknown");
        if( xmce_setting_get_int(MCE_SETTING_TOUCH_UNBLOCK_DELAY, &val) )
                snprintf(txt, sizeof txt, "%d", (int)val);
        printf("%-"PAD1"s %s (milliseconds)\n", "Touch unblock delay:", txt);
}

/* ------------------------------------------------------------------------- *
 * cpu scaling governor override
 * ------------------------------------------------------------------------- */

static bool xmce_set_cpu_scaling_governor(const char *args)
{
        const char *key = MCE_SETTING_CPU_SCALING_GOVERNOR;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(governor_values, args);
        if( val < 0 ) {
                errorf("%s: invalid cpu scaling governor value\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current autosuspend policy from mce and print it out
 */
static void xmce_get_cpu_scaling_governor(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_CPU_SCALING_GOVERNOR, &val) )
                txt = rlookup(governor_values, val);
        printf("%-"PAD1"s %s \n", "CPU Scaling Governor:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * never blank
 * ------------------------------------------------------------------------- */

static bool xmce_set_never_blank(const char *args)
{
        const char *key = MCE_SETTING_DISPLAY_NEVER_BLANK;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(never_blank_values, args);
        if( val < 0 ) {
                errorf("%s: invalid never blank value\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

static void xmce_get_never_blank(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_DISPLAY_NEVER_BLANK, &val) )
                txt = rlookup(never_blank_values, val);
        printf("%-"PAD1"s %s \n", "Display never blank:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * autosuspend on display blank policy
 * ------------------------------------------------------------------------- */

static bool xmce_set_suspend_policy(const char *args)
{
        const char *key = MCE_SETTING_USE_AUTOSUSPEND;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(suspendpol_values, args);
        if( val < 0 ) {
                errorf("%s: invalid suspend policy value\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current autosuspend policy from mce and print it out
 */
static void xmce_get_suspend_policy(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_USE_AUTOSUSPEND, &val) )
                txt = rlookup(suspendpol_values, val);
        printf("%-"PAD1"s %s \n", "Autosuspend policy:", txt ?: "unknown");
}

/** Get current uptime and suspend time
 */
static bool xmce_get_suspend_stats(const char *args)
{
        (void)args;

        DBusMessage *rsp = NULL;
        DBusError    err = DBUS_ERROR_INIT;

        if( !xmce_ipc_message_reply(MCE_SUSPEND_STATS_GET, &rsp, DBUS_TYPE_INVALID) )
                goto EXIT;

        dbus_int64_t uptime_ms  = 0;
        dbus_int64_t suspend_ms = 0;

        if( !dbus_message_get_args(rsp, &err,
                                   DBUS_TYPE_INT64, &uptime_ms,
                                   DBUS_TYPE_INT64, &suspend_ms,
                                   DBUS_TYPE_INVALID) )
                goto EXIT;

        printf("uptime:       %.3f \n", uptime_ms  * 1e-3);
        printf("suspend_time: %.3f \n", suspend_ms * 1e-3);
EXIT:

        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", MCE_SUSPEND_STATS_GET, err.name, err.message);
                dbus_error_free(&err);
        }

        if( rsp ) dbus_message_unref(rsp);

        return true;
}

/* ------------------------------------------------------------------------- *
 * display state statistics
 * ------------------------------------------------------------------------- */

/** Helper for turning 64 bit ms count to human readable elapsed time
 */
static char *elapsed_time_repr(char *buff, size_t size, int64_t t)
{
        char days[32] = "";
        const char *sgn = "";
        if( t < 0 ) sgn="-", t = -t;

        int ms = (int)t % 1000; t /= 1000;
        int s  = (int)t %   60; t /=   60;
        int m  = (int)t %   60; t /=   60;
        int h  = (int)t %   24; t /=   24;

        if( t ) snprintf(days, sizeof days, "%"PRIi64"d ", t);

        snprintf(buff, size, "%s%s%02d:%02d:%02d.%03d",
                 sgn, days, h, m, s, ms);

        return buff;
}

/** Get display state statistics
 */
static bool xmce_get_display_stats(const char *args)
{
        bool human_readable = true;

        if( args ) {
                if( !strcmp(args, "machine") )
                        human_readable = false;
                else if( !strcmp(args, "human") )
                        human_readable = true;
                else {
                        errorf("unkown output mode: %s\n", args);
                        return false;
                }
        }

        DBusMessage *rsp  = NULL;
        DBusError    err  = DBUS_ERROR_INIT;
        gchar       *name = 0;

        DBusMessageIter body, array, dict, entry;

        if( !xmce_ipc_message_reply(MCE_DISPLAY_STATS_GET, &rsp, DBUS_TYPE_INVALID) )
                goto EXIT;

        if( !dbushelper_init_read_iterator(rsp, &body) )
                goto EXIT;

        if( !dbushelper_require_array_type(&body, DBUS_TYPE_DICT_ENTRY) )
                goto EXIT;

        if( !dbushelper_read_array(&body, &array) )
                goto EXIT;

        while( !dbushelper_read_at_end(&array) ) {
                g_free(name), name = 0;

                if( !dbushelper_read_dict(&array, &dict) )
                        goto EXIT;

                if( !dbushelper_read_string(&dict, &name) )
                        goto EXIT;

                if( !dbushelper_read_struct(&dict, &entry) )
                        goto EXIT;

                int64_t time_ms = 0;
                int64_t entries = 0;

                if( !dbushelper_read_int64(&entry, &time_ms) )
                        goto EXIT;

                if( !dbushelper_read_int64(&entry, &entries) )
                        goto EXIT;

                if( human_readable ) {
                        char tmp[64];
                        printf("%-10s %16s, %"PRIi64" times\n",
                               name,
                               elapsed_time_repr(tmp, sizeof tmp, time_ms),
                               entries);
                }
                else {
                        printf("%-10s %"PRIi64" %"PRIi64"\n",
                               name, time_ms, entries);
                }

        }
EXIT:
        g_free(name);

        if( dbus_error_is_set(&err) ) {
                errorf("%s: %s: %s\n", MCE_DISPLAY_STATS_GET, err.name, err.message);
                dbus_error_free(&err);
        }

        if( rsp ) dbus_message_unref(rsp);

        return true;
}

/* ------------------------------------------------------------------------- *
 * use mouse clicks to emulate touchscreen doubletap policy
 * ------------------------------------------------------------------------- */

#ifdef ENABLE_DOUBLETAP_EMULATION
static bool xmce_set_fake_doubletap(const char *args)
{
        const char *key = MCE_SETTING_USE_FAKE_DOUBLETAP;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(fake_doubletap_values, args);
        if( val < 0 ) {
                errorf("%s: invalid fake doubletap value\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_bool(key, val != 0);
}

/** Get current fake double tap policy from mce and print it out
 */
static void xmce_get_fake_doubletap(void)
{
        gboolean    val = 0;
        const char *txt = 0;
        if( xmce_setting_get_bool(MCE_SETTING_USE_FAKE_DOUBLETAP, &val) )
                txt = rlookup(fake_doubletap_values, val);
        printf("%-"PAD1"s %s \n", "Use fake doubletap:", txt ?: "unknown");
}
#endif /* ENABLE_DOUBLETAP_EMULATION */

/* ------------------------------------------------------------------------- *
 * tklock
 * ------------------------------------------------------------------------- */

/** Lookup table for tklock open values
 */
static const symbol_t tklock_open_values[] = {
#if 0 // DEPRECATED
        { "none",     TKLOCK_NONE },
        { "enable",   TKLOCK_ENABLE },
        { "help",     TKLOCK_HELP },
        { "select",   TKLOCK_SELECT },
#endif
        { "oneinput", TKLOCK_ONEINPUT },
        { "visual",   TKLOCK_ENABLE_VISUAL },
        { "lpm",      TKLOCK_ENABLE_LPM_UI },
        { "pause",    TKLOCK_PAUSE_UI },
        { NULL, -1 }
};

/** Simulate tklock open from mce to lipstick
 */
static bool xmce_tklock_open(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        int val = lookup(tklock_open_values, args);
        if( val < 0 ) {
                errorf("%s: invalid tklock open value\n", args);
                exit(EXIT_FAILURE);
        }

        DBusConnection *bus = xdbus_init();
        DBusMessage    *rsp = 0;
        DBusMessage    *req = 0;
        DBusError       err = DBUS_ERROR_INIT;

        const char   *cb_service   = MCE_SERVICE;
        const char   *cb_path      = MCE_REQUEST_PATH;
        const char   *cb_interface = MCE_REQUEST_IF;
        const char   *cb_method    = MCE_TKLOCK_CB_REQ;
        dbus_uint32_t mode         = (dbus_uint32_t)val;
        dbus_bool_t   silent       = TRUE;
        dbus_bool_t   flicker_key  = FALSE;

        req = dbus_message_new_method_call(SYSTEMUI_SERVICE,
                                           SYSTEMUI_REQUEST_PATH,
                                           SYSTEMUI_REQUEST_IF,
                                           SYSTEMUI_TKLOCK_OPEN_REQ);
        if( !req ) goto EXIT;

        dbus_message_append_args(req,
                                 DBUS_TYPE_STRING, &cb_service,
                                 DBUS_TYPE_STRING, &cb_path,
                                 DBUS_TYPE_STRING, &cb_interface,
                                 DBUS_TYPE_STRING, &cb_method,
                                 DBUS_TYPE_UINT32, &mode,
                                 DBUS_TYPE_BOOLEAN, &silent,
                                 DBUS_TYPE_BOOLEAN, &flicker_key,
                                 DBUS_TYPE_INVALID);

        rsp = dbus_connection_send_with_reply_and_block(bus, req, -1, &err);

        if( !req ) {
                errorf("no reply to %s; %s: %s\n", SYSTEMUI_TKLOCK_OPEN_REQ,
                       err.name, err.message);
                goto EXIT;
        }
        printf("got reply to %s\n", SYSTEMUI_TKLOCK_OPEN_REQ);

EXIT:
        if( rsp ) dbus_message_unref(rsp), rsp = 0;
        if( req ) dbus_message_unref(req), req = 0;
        dbus_error_free(&err);
        return true;
}

/** Simulate tklock close from mce to lipstick
 */
static bool xmce_tklock_close(const char *args)
{
        (void)args;

        debugf("%s(%s)\n", __FUNCTION__, args);

        DBusConnection *bus = xdbus_init();
        DBusMessage    *rsp = 0;
        DBusMessage    *req = 0;
        DBusError       err = DBUS_ERROR_INIT;

        dbus_bool_t silent = TRUE;

        req = dbus_message_new_method_call(SYSTEMUI_SERVICE,
                                           SYSTEMUI_REQUEST_PATH,
                                           SYSTEMUI_REQUEST_IF,
                                           SYSTEMUI_TKLOCK_CLOSE_REQ);
        if( !req ) goto EXIT;

        dbus_message_append_args(req,
                                 DBUS_TYPE_BOOLEAN, &silent,
                                 DBUS_TYPE_INVALID);

        rsp = dbus_connection_send_with_reply_and_block(bus, req, -1, &err);

        if( !req ) {
                errorf("no reply to %s; %s: %s\n", SYSTEMUI_TKLOCK_CLOSE_REQ,
                       err.name, err.message);
                goto EXIT;
        }
        printf("got reply to %s\n", SYSTEMUI_TKLOCK_CLOSE_REQ);

EXIT:
        if( rsp ) dbus_message_unref(rsp), rsp = 0;
        if( req ) dbus_message_unref(req), req = 0;
        dbus_error_free(&err);
        return true;
}

/** Lookup table for tklock callback values
 */
static const symbol_t tklock_callback_values[] = {
        { "unlock",  TKLOCK_UNLOCK  },
        { "retry",   TKLOCK_RETRY   },
        { "timeout", TKLOCK_TIMEOUT },
        { "closed",  TKLOCK_CLOSED  },
        { NULL, -1 }
};

/** Simulate tklock callback from lipstick to mce
 */
static bool xmce_tklock_callback(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args);
        dbus_int32_t val = lookup(tklock_callback_values, args);
        if( val < 0 ) {
                errorf("%s: invalidt klock callback value\n", args);
                exit(EXIT_FAILURE);
        }

        xmce_ipc_no_reply(MCE_TKLOCK_CB_REQ,
                          DBUS_TYPE_INT32, &val,
                          DBUS_TYPE_INVALID);
        return true;
}

/** Enable/disable the tklock
 *
 * @param mode The mode to change to; valid modes:
 *             "locked", "locked-dim", "locked-delay", "unlocked"
 */
static bool xmce_set_tklock_mode(const char *args)
{
        if( mcetool_reject_common_args(args) )
                return false;

        xmce_ipc_no_reply(MCE_TKLOCK_MODE_CHANGE_REQ,
                          DBUS_TYPE_STRING, &args,
                          DBUS_TYPE_INVALID);
        return true;
}

/** Get current tklock mode from mce and print it out
 */
static void xmce_get_tklock_mode(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_TKLOCK_MODE_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n", "Touchscreen/Keypad lock:", str ?: "unknown");
        free(str);
}

/* Set tklock blanking inhibit mode
 *
 * @param args string that can be parsed to inhibit mode
 */
static bool xmce_set_tklock_blank(const char *args)
{
        const char *key = MCE_SETTING_TK_AUTO_BLANK_DISABLE;
        if( mcetool_handle_common_args(key, args) )
                return true;

        int val = lookup(tklockblank_values, args);
        if( val < 0 ) {
                errorf("%s: invalid lockscreen blanking policy value\n", args);
                exit(EXIT_FAILURE);
        }
        return xmce_setting_set_int(key, val);
}

/** Get current tklock autoblank inhibit mode from mce and print it out
 */
static void xmce_get_tklock_blank(void)
{
        gint        val = 0;
        const char *txt = 0;
        if( xmce_setting_get_int(MCE_SETTING_TK_AUTO_BLANK_DISABLE, &val) )
                txt = rlookup(tklockblank_values, val);
        printf("%-"PAD1"s %s \n", "Tklock autoblank policy:", txt ?: "unknown");
}

/* ------------------------------------------------------------------------- *
 * misc
 * ------------------------------------------------------------------------- */

/** Get mce version from mce and print it out
 */
static void xmce_get_version(void)
{
        char *str = 0;
        xmce_ipc_string_reply(MCE_VERSION_GET, &str, DBUS_TYPE_INVALID);
        printf("%-"PAD1"s %s\n","MCE version:", str ?: "unknown");
        free(str);
}

/** Get inactivity state from mce and print it out
 */
static void xmce_get_inactivity_state(void)
{
        gboolean val = 0;
        char txt[32];
        strcpy(txt, "unknown");
        if( xmce_ipc_bool_reply(MCE_INACTIVITY_STATUS_GET, &val, DBUS_TYPE_INVALID) )
                snprintf(txt, sizeof txt, "%s", val ? "inactive" : "active");
        printf("%-"PAD1"s %s\n","Inactivity status:", txt);
}

/** Get keyboard backlight state from mce and print it out
 */
static void xmce_get_keyboard_backlight_state(void)
{
        gboolean val = 0;
        char txt[32];
        strcpy(txt, "unknown");
        if( xmce_ipc_bool_reply(MCE_KEY_BACKLIGHT_STATE_GET, &val, DBUS_TYPE_INVALID) )
                snprintf(txt, sizeof txt, "%s", val ? "enabled" : "disabled");
        printf("%-"PAD1"s %s\n","Keyboard backlight:", txt);
}

/** Obtain and print mce status information
 */
static bool xmce_get_status(const char *args)
{
        (void)args;
        printf("\n"
                "MCE status:\n"
                "-----------\n");

        xmce_get_version();
        xmce_get_verbosity();
        xmce_get_radio_states();
        xmce_get_call_state();
        xmce_get_button_backlight();
        xmce_get_display_state();
        xmce_get_color_profile();
        xmce_get_display_brightness();
        xmce_get_dimmed_brightness_static();
        xmce_get_dimmed_brightness_dynamic();
        xmce_get_compositor_dimming();
        xmce_get_cabc_mode();
        xmce_get_dim_timeout();
        xmce_get_dim_with_kbd_timeout();
        xmce_get_adaptive_dimming_mode();
        xmce_get_adaptive_dimming_time();
        xmce_get_never_blank();
        xmce_get_blank_timeout();
        xmce_get_inhibit_mode();
        xmce_get_kbd_slide_inhibit_mode();
        xmce_get_blank_prevent_mode();
        xmce_get_keyboard_backlight_state();
        xmce_get_inactivity_state();
        xmce_get_inactivity_shutdown_delay();
        xmce_get_power_saving_mode();
        xmce_get_forced_psm();
        xmce_get_psm_threshold();
        xmce_get_tklock_mode();
        xmce_get_autolock_mode();
        xmce_get_autolock_delay();
        xmce_get_devicelock_in_lockscreen();
        xmce_get_lockscreen_unblank_animation();
        xmce_get_doubletap_wakeup();
        xmce_get_fingerprint_wakeup_mode();
        xmce_get_fingerprint_wakeup_allow_delay();
        xmce_get_fingerprint_wakeup_trigger_delay();
        xmce_get_fingerprint_wakeup_throttle_delay();
        xmce_get_volkey_policy();
        xmce_get_powerkey_action();
        xmce_get_powerkey_blanking();
        xmce_get_powerkey_long_press_delay();
        xmce_get_powerkey_double_press_delay();
        xmce_get_powerkey_action_masks();
        xmce_get_powerkey_dbus_actions();
        xmce_get_ps_override_count();
        xmce_get_ps_override_timeout();
        xmce_get_display_off_override();
        xmce_get_low_power_mode();
        xmce_get_lpmui_triggering();
        xmce_get_als_mode();
        xmce_get_als_autobrightness();
        xmce_get_als_input_filter();
        xmce_get_als_sample_time();
        xmce_get_orientation_sensor_mode();
        xmce_get_orientation_change_is_activity();
        xmce_get_flipover_gesture_detection();
        xmce_get_ps_mode();
        xmce_get_ps_on_demand();
        xmce_get_ps_uncover_delay();
        xmce_get_ps_blocks_touch();
        xmce_get_ps_acts_as_lid();
        xmce_get_lid_sensor_mode();
        xmce_get_filter_lid_with_als();
        xmce_get_filter_lid_als_limit();
        xmce_get_lid_open_actions();
        xmce_get_lid_close_actions();
        xmce_get_kbd_slide_open_trigger();
        xmce_get_kbd_slide_open_actions();
        xmce_get_kbd_slide_close_trigger();
        xmce_get_kbd_slide_close_actions();
        xmce_get_dim_timeouts();
        xmce_get_brightness_fade();
        xmce_get_suspend_policy();
        xmce_get_cpu_scaling_governor();
#ifdef ENABLE_DOUBLETAP_EMULATION
        xmce_get_fake_doubletap();
#endif
        xmce_get_tklock_blank();
        xmce_get_lipstick_core_delay();
        xmce_get_input_policy_mode();
        xmce_get_input_grab_allowed();
        xmce_get_touch_unblock_delay();
        xmce_get_exception_lengths();

        get_led_breathing_enabled();
        get_led_breathing_limit();
        xmce_get_memnotify_limits();
        xmce_get_memnotify_level();
        xmce_get_button_backlligut_off_delay();

        xmce_get_battery_info();
        printf("\n");

        return true;
}

/* ------------------------------------------------------------------------- *
 * special
 * ------------------------------------------------------------------------- */

/** Handle --block command line option
 *
 * @param args optarg from command line, or NULL
 */
static bool mcetool_block(const char *args)
{
        debugf("%s(%s)\n", __FUNCTION__, args ?: "inf");
        struct timespec ts;

        if( mcetool_parse_timspec(&ts, args) )
                TEMP_FAILURE_RETRY(nanosleep(&ts, &ts));
        else
                pause();

        return true;
}

/** Handle --demo-mode command line option
 *
 * @param args optarg from command line
 */
static bool xmce_set_demo_mode(const char *args)
{
        if( mcetool_reject_common_args(args) )
                return false;

        bool res = true;
        if( !strcmp(args, "on") ) {
                // mcetool --unblank-screen
                //         --set-inhibit-mode=stay-on
                //         --set-tklock-mode=unlocked
                //         --set-tklock-blank=disabled
                xmce_set_display_state("on");
                xmce_set_inhibit_mode("stay-on");
                xmce_set_tklock_mode("unlocked");
                xmce_set_tklock_blank("disabled");
        }
        else if( !strcmp(args, "dim")) {
                // mcetool --dim-screen
                //         --set-inhibit-mode=stay-dim
                //         --set-tklock-mode=unlocked
                //         --set-tklock-blank=disabled
                xmce_set_display_state("dim");
                xmce_set_inhibit_mode("stay-dim");
                xmce_set_tklock_mode("unlocked");
                xmce_set_tklock_blank("disabled");
        }
        else if( !strcmp(args, "off")) {
                // mcetool --blank-screen
                //         --set-inhibit-mode=disabled
                //         --set-tklock-mode=locked
                //         --set-tklock-blank=enabled
                xmce_set_display_state("off");
                xmce_set_inhibit_mode("disabled");
                xmce_set_tklock_mode("locked");
                xmce_set_tklock_blank("enabled");
        }
        else {
                errorf("%s: invalid demo mode value\n", args);
                res = false;
        }

        return res;
}

/* ========================================================================= *
 * COMMAND LINE OPTIONS
 * ========================================================================= */

static bool mcetool_do_help(const char *arg);
static bool mcetool_do_long_help(const char *arg);
static bool mcetool_do_version(const char *arg);

static bool mcetool_do_unblank_screen(const char *arg)
{
        (void)arg;
        xmce_set_display_state("on");
        return true;
}
static bool mcetool_do_dim_screen(const char *arg)
{
        (void)arg;
        xmce_set_display_state("dim");
        return true;
}
static bool mcetool_do_blank_screen(const char *arg)
{
        (void)arg;
        xmce_set_display_state("off");
        return true;
}
static bool mcetool_do_blank_screen_lpm(const char *arg)
{
        (void)arg;
        xmce_set_display_state("lpm");
        return true;
}

// Unused short options left ....
// - - - - - - - - - - - - - - - - - - - - - - w x - -
// - - - - - - - - - - - - - - - - - - - - - - W X - -

static const mce_opt_t options[] =
{
        {
                .name        = "unblank-screen",
                .flag        = 'U',
                .without_arg = mcetool_do_unblank_screen,
                .usage       =
                        "send display on request\n"
        },
        {
                .name        = "dim-screen",
                .flag        = 'd',
                .without_arg = mcetool_do_dim_screen,
                .usage       =
                        "send display dim request\n"
        },
        {
                .name        = "blank-screen",
                .flag        = 'n',
                .without_arg = mcetool_do_blank_screen,
                .usage       =
                        "send display off request\n"
        },
        {
                .name        = "blank-screen-lpm",
                .without_arg = mcetool_do_blank_screen_lpm,
                .usage       =
                        "send display low power mode request\n"
        },
        {
                .name        = "get-display-stats",
                .without_arg = xmce_get_display_stats,
                .with_arg    = xmce_get_display_stats,
                .values      = "human|machine",
                .usage       =
                        "get time spent in various display states\n"
                        "\n"
                        "Note that uptime accumulated before the startup of\n"
                        "the currently running mce process gets accounted\n"
                        "as UNDEF.\n"
        },
        {
                .name        = "blank-prevent",
                .flag        = 'P',
                .without_arg = xmce_prevent_display_blanking,
                .usage       =
                        "send blank prevent request\n"
        },
        {
                .name        = "cancel-blank-prevent",
                .flag        = 'v',
                .without_arg = xmce_allow_display_blanking,
                .usage       =
                        "send cancel blank prevent request\n"
        },
        {
                .name        = "set-blank-prevent-mode",
                .with_arg    = xmce_set_blank_prevent_mode,
                .values      = "disabled|keep-on|allow-dim",
                .usage       =
                        "set blank prevent mode; valid modes are:\n"
                        "  'disabled'  all blank prevent requests are ignored\n"
                        "  'keep-on'   display is kept on as requested\n"
                        "  'allow-dim' display can be dimmed during blank prevent\n"
        },
        {
                .name        = "set-dim-timeout",
                .flag        = 'G',
                .with_arg    = xmce_set_dim_timeout,
                .values      = "secs",
                .usage       =
                        "set the automatic dimming timeout\n"
        },
        {
                .name        = "set-dim-with-kbdtimeout",
                .with_arg    = xmce_set_dim_with_kbd_timeout,
                .values      = "secs",
                .usage       =
                        "set the automatic dimming timeout when hw kbd is available\n"
        },
        {
                .name        = "set-dim-timeouts",
                .flag        = 'O',
                .with_arg    = xmce_set_dim_timeouts,
                .values      = "secs,secs,...",
                .usage       =
                        "set the allowed dim timeouts; valid list must\n"
                        "must have 5 entries, in ascending order\n"
        },
        {
                .name        = "set-adaptive-dimming-mode",
                .flag        = 'f',
                .with_arg    = xmce_set_adaptive_dimming_mode,
                .values      = "enabled|disabled",
                .usage       =
                        "set the adaptive dimming mode; valid modes are:\n"
                        "  'enabled' and 'disabled'\n"
        },
        {
                .name        = "set-adaptive-dimming-time",
                .flag        = 'J',
                .with_arg    = xmce_set_adaptive_dimming_time,
                .values      = "secs",
                .usage       =
                        "set the adaptive dimming threshold\n"
        },
        {
                .name        = "set-blank-timeout",
                .flag        = 'o',
                .with_arg    = xmce_set_blank_timeout,
                .values      = "secs",
                .usage       =
                        "set the default automatic blanking timeout\n"
        },
        {
                .name        = "set-blank-from-lockscreen-timeout",
                .with_arg    = xmce_set_blank_from_lockscreen_timeout,
                .values      = "secs",
                .usage       =
                        "set the automatic blanking timeout from lockscreen\n"
        },
        {
                .name        = "set-blank-from-lpm-on-timeout",
                .with_arg    = xmce_set_blank_from_lpm_on_timeout,
                .values      = "secs",
                .usage       =
                        "set the automatic blanking timeout from lpm-on\n"
        },
        {
                .name        = "set-blank-from-lpm-off-timeout",
                .with_arg    = xmce_set_blank_from_lpm_off_timeout,
                .values      = "secs",
                .usage       =
                        "set the automatic blanking timeout from lpm-off\n"
        },
        {
                .name        = "set-never-blank",
                .flag        = 'j',
                .with_arg    = xmce_set_never_blank,
                .values      = "enabled|disabled",
                .usage       =
                        "set never blank mode; valid modes are:\n"
                        "'disabled', 'enabled'\n"
        },
        {
                .name        = "set-autolock-mode",
                .flag        = 'K',
                .with_arg    = xmce_set_autolock_mode,
                .values      = "enabled|disabled",
                .usage       =
                        "set the autolock mode; valid modes are:\n"
                        "'enabled' and 'disabled'\n"
        },
        {
                .name        = "set-autolock-delay",
                .with_arg    = xmce_set_autolock_delay,
                .values      = "seconds[.fraction]",
                .usage       =
                        "set autolock delay after automatic display blanking\n"
        },
        {
                .name        = "set-devicelock-in-lockscreen",
                .with_arg    = xmce_set_devicelock_in_lockscreen,
                .values      = "READ THE LONG HELP",
                .usage       =
                        "DO NOT TOUCH THIS UNLESS YOU KNOWN WHAT YOU ARE DOING\n"
                        "\n"
                        "Enabling the toggle on devices where device unlocking\n"
                        "is not included in the lockscreen makes it impossible to\n"
                        "unlock the device via touch screen.\n"
                        "\n"
                        "Valid modes are: 'enabled' and 'disabled'\n"
        },
        {
                .name        = "set-lockscreen-animation",
                .with_arg    = xmce_set_lockscreen_unblank_animation,
                .values      = "enabled|disabled",
                .usage       =
                        "allow/deny animations during unblanking via powerkey / doubletap\n"
        },
        {
                .name        = "set-tklock-blank",
                .flag        = 't',
                .with_arg    = xmce_set_tklock_blank,
                .values      = "enabled|disabled",
                .usage       =
                        "set the touchscreen/keypad autoblank mode;\n"
                        "valid modes are: 'enabled' and 'disabled'\n"
        },
        {
                .name        = "set-inhibit-mode",
                .flag        = 'I',
                .with_arg    = xmce_set_inhibit_mode,
                .values      = "disabled|stay-on-with-charger|stay-on|stay-dim-with-charger|stay-dim",
                .usage       =
                        "set the blanking inhibit mode to MODE;\n"
                        "valid modes are:\n"
                        "'disabled',\n"
                        "'stay-on-with-charger', 'stay-on',\n"
                        "'stay-dim-with-charger', 'stay-dim'\n"
        },
        {
                .name        = "set-kbd-slide-inhibit-mode",
                .with_arg    = xmce_set_kbd_slide_inhibit_mode,
                .values      = "disabled|stay-on-when-open|stay-dim-when-open",
                .usage       =
                        "Set the kbd slide blanking inhibit mode:\n"
                        "  disabled            kbd slide status does not prevent blanking\n"
                        "  stay-on-when-open   prevent dimming while kbd slide is open\n"
                        "  stay-dim-when-open  prevent blanking while kbd slide is open\n"
        },
        {
                .name        = "set-tklock-mode",
                .flag        = 'k',
                .with_arg    = xmce_set_tklock_mode,
                .values      = "locked|locked-dim|locked-delay|unlocked",
                .usage       =
                        "set the touchscreen/keypad lock mode;\n"
                        "valid modes are:\n"
                        "'locked', 'locked-dim',\n"
                        "'locked-delay',\n"
                        "and 'unlocked'\n"
        },
        {
                .name        = "tklock-callback",
                .flag        = 'm',
                .with_arg    = xmce_tklock_callback,
                .values      = "unlock|retry|timeout|closed",
                .usage       =
                        "simulate tklock callback from systemui\n"
        },
        {
                .name        = "tklock-open",
                .flag        = 'q',
                .with_arg    = xmce_tklock_open,
                .values      = "oneinput|visual|lpm|pause",
                .usage       =
                        "simulate tklock open from mce\n"
        },
        {
                .name        = "tklock-close",
                .flag        = 'Q',
                .without_arg = xmce_tklock_close,
                .usage       =
                        "simulate tklock close from mce\n"
        },
        {
                .name        = "set-doubletap-mode",
                .flag        = 'M',
                .with_arg    = xmce_set_doubletap_mode,
                .values      = "disabled|show-unlock-screen|unlock",
                .usage       =
                        "set the doubletap mode (deprecated)\n"
                        "\n"
                        "This option is retained for convenience/backwards compatibility\n"
                        "and acts as alias for --set-touchscreen-gesture-action that can\n"
                        "be used for configuring also other touchscreen gestures in addition\n"
                        "to just double tap.\n"
                        "\n"
                        "Valid modes are:\n"
                        "  disabled - double tap events are not acted on\n"
                        "  show-unlock-screen - unblank screen\n"
                        "  unlock - unblank screen and deactivate lockscreen\n"
        },
        {
                .name        = "set-doubletap-wakeup",
                .flag        = 'z',
                .with_arg    = xmce_set_doubletap_wakeup,
                .values      = "never|always|proximity",
                .usage       =
                        "set the doubletap wakeup mode; valid modes are:\n"
                        "'never', 'always', 'proximity'\n"
                        "\n"
                        "Note: proximity setting applies for lid sensor too."
        },
        {
                .name        = "set-fingerprint-wakeup-mode",
                .with_arg    = xmce_set_fingerprint_wakeup_mode,
                .values      = "never|always|proximity",
                .usage       =
                        "set the fingerprint wakeup mode; valid modes are:\n"
                        "'never', 'always', 'proximity'\n"
                        "\n"
                        "Note: Additionally the device must be in a state where fingerprint\n"
                        "      acquisition does not interfere with authentication services\n"
                        "      and some sensible action can be taken on fingerprint identify.\n"
        },
        {
                .name        = "set-fingerprint-wakeup-allow-delay",
                .with_arg    = xmce_set_fingerprint_wakeup_allow_delay,
                .values      = "ms",
                .usage       =
                        "set delay for enabling fp wakeups on policy change.\n"
                        "\n"
                        "When policy decision makes denied->allowed transition, mce will\n"
                        "wait a bit before starting identification ipc - so that short\n"
                        "living policy changes do not cause unnecessary dbus ipc.\n"
                        "\n"
                        "On allowed->denied transitions mce will immediately let go of any\n"
                        "pending identification requests.\n"
        },
        {
                .name        = "set-fingerprint-wakeup-trigger-delay",
                .with_arg    = xmce_set_fingerprint_wakeup_trigger_delay,
                .values      = "ms",
                .usage       =
                        "set delay between identifying a finger and acting on it.\n"
                        "\n"
                        "Used for filtering out false positive fingerprints when user makes\n"
                        "an attempt to press power key on devices where fingerprint scanner\n"
                        "is located physically on power button.\n"
                        "\n"
                        "In devices that have dedicated fingerprint scanner it should be\n"
                        "safe to set this delay to zero.\n"
        },
        {
                .name        = "set-fingerprint-wakeup-throttle-delay",
                .with_arg    = xmce_set_fingerprint_wakeup_throttle_delay,
                .values      = "ms",
                .usage       =
                        "set delay between ipc attempts with fingerprint daemon.\n"
                        "\n"
                        "Used for both pacing dbus ipc to fingerprint daemon on failures, and\n"
                        "allowing authentication services some time to grab fingerprint sensor\n"
                        "when needed.\n"
        },
        {
                .name        = "set-volume-key-policy",
                .with_arg    = xmce_set_volkey_policy,
                .values      = "default|media-only",
                .usage       =
                        "set the volume key input policy; valid modes are:\n"
                        "\n"
                        "  default    - Volume keys are enabled when display is on\n"
                        "               or audio policy indicates music playback\n"
                        "\n"
                        "  media-only - Volume keys are enabled only when there is\n"
                        "               music playback.\n"
        },
        {
                .name        = "set-powerkey-action",
                .flag        = 'Z',
                .with_arg    = xmce_set_powerkey_action,
                .values      = "never|always|proximity|proximity2",
                .usage       =
                        "set the power key action mode; valid modes are:\n"
                        "  never       -  ignore power key presses\n"
                        "  always      -  always act\n"
                        "  proximity   -  act if proximity sensor is not covered\n"
                        "  proximity2  -  act if display is on or PS not covered\n"
                        "\n"
                        "Note: proximity settings apply for lid sensor too."
        },
        {
                .name        = "set-powerkey-blanking",
                .with_arg    = xmce_set_powerkey_blanking,
                .values      = "off|lpm",
                .usage       =
                        "set the doubletap blanking mode; valid modes are:\n"
                        "'off', 'lpm'\n"
        },
        {
                .name        = "set-powerkey-long-press-delay",
                .with_arg    = xmce_set_powerkey_long_press_delay,
                .values      = "ms",
                .usage       =
                        "set minimum length of \"long\" power key press.\n"
        },
        {
                .name        = "set-powerkey-double-press-delay",
                .with_arg    = xmce_set_powerkey_double_press_delay,
                .values      = "ms",
                .usage       =
                        "set maximum delay between \"double\" power key presses.\n"
        },
        {
                .name        = "set-display-on-single-powerkey-press-actions",
                .with_arg    = xmce_set_powerkey_actions_while_display_on_single,
                .values      = "actions",
                .usage       =
                        "set actions to execute on single power key press from display on state\n"
                        "\n"
                        "Valid actions are:\n"
                        "  blank     - turn display off\n"
                        "  tklock    - lock ui\n"
                        "  devlock   - lock device\n"
                        "  shutdown  - power off device\n"
                        "  vibrate   - play vibrate event via ngfd\n"
                        "  unblank   - turn display on\n"
                        "  tkunlock  - unlock ui / prompt devicelock code\n"
                        "  tkunlock2 - unlock ui if device is not locked\n"
                        "  dbus1     - send dbus signal or make method call\n"
                        "  dbus2     - send dbus signal or make method call\n"
                        "  dbus3     - send dbus signal or make method call\n"
                        "   ...\n"
                        "  dbus10    - send dbus signal or make method call\n"
                        "  nop       - dummy operation, for differentiating otherwise\n"
                        "              identical single/double press actions (mce does not\n"
                        "              wait for double presses double press actions are\n"
                        "              the same as for a single press)\n"
                        "\n"
                        "Comma separated list of actions can be used.\n"
        },
        {
                .name        = "set-display-on-double-powerkey-press-actions",
                .with_arg    = xmce_set_powerkey_actions_while_display_on_double,
                .values      = "actions",
                .usage       =
                        "set actions to execute on double power key press from display on state\n"
                        "\n"
                        "See --set-display-on-single-powerkey-press-actions for details\n"
        },
        {
                .name        = "set-display-on-long-powerkey-press-actions",
                .with_arg    = xmce_set_powerkey_actions_while_display_on_long,
                .values      = "actions",
                .usage       =
                        "set actions to execute on long power key press from display on state\n"
                        "\n"
                        "See --set-display-on-single-powerkey-press-actions for details\n"
        },
        {
                .name        = "set-display-off-single-powerkey-press-actions",
                .with_arg    = xmce_set_powerkey_actions_while_display_off_single,
                .values      = "actions",
                .usage       =
                        "set actions to execute on single power key press from display off state\n"
                        "\n"
                        "See --set-display-on-single-powerkey-press-actions for details\n"
        },
        {
                .name        = "set-display-off-double-powerkey-press-actions",
                .with_arg    = xmce_set_powerkey_actions_while_display_off_double,
                .values      = "actions",
                .usage       =
                        "set actions to execute on double power key press from display off state\n"
                        "\n"
                        "See --set-display-on-single-powerkey-press-actions for details\n"
        },
        {
                .name        = "set-display-off-long-powerkey-press-actions",
                .with_arg    = xmce_set_powerkey_actions_while_display_off_long,
                .values      = "actions",
                .usage       =
                        "set actions to execute on long power key press from display off state\n"
                        "\n"
                        "See --set-display-on-single-powerkey-press-actions for details\n"
        },
        {
                .name        = "set-touchscreen-gesture-actions",
                .with_arg    = xmce_set_touchscreen_gesture_action,
                .values      = "gesture_id|doubletap|fpwakeup,actions",
                .usage       =
                        "set actions to execute on touchscreen gestures\n"
                        "\n"
                        "Gesture id is a number in 0...19 range. The values are hw specific,\n"
                        "but 4 can be assumed to mean doubletap and 16 fingerprint wakeup.\n"
                        "\n"
                        "Actions are as with --set-display-on-single-powerkey-press-actions.\n"
        },
        {
                .name        = "set-powerkey-dbus-action",
                .with_arg    = xmce_set_powerkey_dbus_action,
                .values      = "action_id,signal_argument|method_call_details",
                .usage       =
                        "define dbus ipc taking place when dbus1 powerkey action is triggered\n"
                        "\n"
                        "action_id: <1..."G_STRINGIFY(POWEKEY_DBUS_ACTION_COUNT)">\n"
                        "\n"
                        "signal_argument: <argument>\n"
                        "  MCE will still send a dbus signal, but uses the given string as argument\n"
                        "  instead of using the built-in default.\n"
                        "\n"
                        "methdod_call_details: <service>,<object>,<interface>,<method>[,<argument>]\n"
                        "  Instead of sending a signal, MCE will make dbus method call as specified.\n"
                        "  The string argument for the method call is optional.\n"
        },
        {
                .name        = "set-powerkey-ps-override-count",
                .with_arg    = xmce_set_ps_override_count,
                .values      = "press-count",
                .usage       =
                        "set number of repeated power key presses needed to\n"
                        "override stuck proximity sensor; use 0 to disable\n"
        },
        {
                .name        = "set-powerkey-ps-override-timeout",
                .with_arg    = xmce_set_ps_override_timeout,
                .values      = "ms",
                .usage       =
                        "maximum delay between repeated power key presses that\n"
                        "can override stuck proximity sensor; use 0 to disable\n"
        },
        {
                .name        = "set-display-off-override",
                .with_arg    = xmce_set_display_off_override,
                .values      = "disabled|use-lpm|only-blank",
                .usage       =
                        "set the display off request override; valid modes are:\n"
                        "disabled    - blank screen and activate lockscreen\n"
                        "use-lpm     - activate display low power mode\n"
                        "only-blank  - just blank screen\n"
        },
        {
                .name        = "enable-radio",
                .flag        = 'r',
                .with_arg    = xmce_enable_radio,
                .values      = "master|cellular|wlan|bluetooth",
                .usage       =
                        "enable the specified radio; valid radios are:\n"
                        "'master', 'cellular',\n"
                        "'wlan' and 'bluetooth';\n"
                        "'master' affects all radios\n"
        },
        {
                .name        = "disable-radio",
                .flag        = 'R',
                .with_arg    = xmce_disable_radio,
                .values      = "master|cellular|wlan|bluetooth",
                .usage       =
                        "disable the specified radio; valid radios are:\n"
                        "'master', 'cellular',\n"
                        "'wlan' and 'bluetooth';\n"
                        "'master' affects all radios\n"
        },
        {
                .name        = "set-power-saving-mode",
                .flag        = 'p',
                .with_arg    = xmce_set_power_saving_mode,
                .values      = "enabled|disabled",
                .usage       =
                        "set the power saving mode; valid modes are:\n"
                        "'enabled' and 'disabled'\n"
        },
        {
                .name        = "set-psm-threshold",
                .flag        = 'T',
                .with_arg    = xmce_set_psm_threshold,
                .values      = "10|20|30|40|50",
                .usage       =
                        "set the threshold for the power saving mode;\n"
                        "valid values are:\n"
                        "10, 20, 30, 40, 50\n"
        },
        {
                .name        = "set-forced-psm",
                .flag        = 'F',
                .with_arg    = xmce_set_forced_psm,
                .values      = "enabled|disabled",
                .usage       =
                        "the forced power saving mode to MODE;\n"
                        "valid modes are:\n"
                        "'enabled' and 'disabled'\n"
        },
        {
                .name        = "set-low-power-mode",
                .flag        = 'E',
                .with_arg    = xmce_set_low_power_mode,
                .values      = "enabled|disabled",
                .usage       =
                        "set the low power mode; valid modes are:\n"
                        "'enabled' and 'disabled'\n"
        },
        {
                .name        = "set-lpmui-triggering",
                .with_arg    = xmce_set_lpmui_triggering,
                .values      = "bit1[,bit2][...]",
                .usage       =
                        "set the low power mode ui triggering; valid bits are:\n"
                        "'disabled', 'from-pocket' and 'hover-over'\n"
        },
        {
                .name        = "set-suspend-policy",
                .flag        = 's',
                .with_arg    = xmce_set_suspend_policy,
                .values      = "enabled|disabled|early|disable_on_charger",
                .usage       =
                        "set the autosuspend mode; valid modes are:\n"
                        "  enabled   Allow early and late suspend. This is the default mode.\n"
                        "  disabled  Deny suspending altogether. Also display and touch panel should\n"
                        "            stay powered on and allow sw double tap detection.\n"
                        "  early     Allow early suspend / display power off, but not late suspend\n"
                        "            so that (at least one) cpu stays online.\n"
                        "  disable_on_charger Switch between enabled and disabled based on charger\n"
                        "            availability when device is running in USER mode.\n"
        },
        {
                .name        = "get-suspend-stats",
                .without_arg = xmce_get_suspend_stats,
                .usage       =
                        "get device uptime and time spent in suspend\n"
        },
        {
                .name        = "set-cpu-scaling-governor",
                .flag        = 'S',
                .with_arg    = xmce_set_cpu_scaling_governor,
                .values      = "automatic|performance|interactive",
                .usage       =
                        "set the cpu scaling governor override; valid\n"
                        "modes are: 'automatic', 'performance',\n"
                        "'interactive'\n"
        },
#ifdef ENABLE_DOUBLETAP_EMULATION
        {
                .name        = "set-fake-doubletap",
                .flag        = 'i',
                .with_arg    = xmce_set_fake_doubletap,
                .values      = "enabled|disabled",
                .usage       =
                        "set the doubletap emulation mode; valid modes are:\n"
                        "  'enabled' and 'disabled'\n"
        },
#endif
        {
                .name        = "set-display-brightness",
                .flag        = 'b',
                .with_arg    = xmce_set_display_brightness,
                .values      = "1...100",
                .usage       =
                        "set the display brightness to BRIGHTNESS;\n"
                        "valid values are: 1-100\n"
        },

        {
                .name        = "set-dimmed-brightness-static",
                .with_arg    = xmce_set_dimmed_brightness_static,
                .values      = "1...100",
                .usage       =
                        "set the statically defined dimmed display brightness;\n"
                        "valid values are: 1-100 [%% of hw maximum level]\n"
                        "\n"
                        "The affective backlight level used when the display is in\n"
                        "dimmed state is minimum of dynamic and static levels.\n"
        },
        {
                .name        = "set-dimmed-brightness-dynamic",
                .with_arg    = xmce_set_dimmed_brightness_dynamic,
                .values      = "1...100",
                .usage       =
                        "set the maximum dimmed display brightness;\n"
                        "valid values are: 1-100 [%% of on brightness level]\n"
        },
        {
                .name        = "set-compositor-dimming-threshold-hi",
                .with_arg    = xmce_set_compositor_dimming_hi,
                .values      = "0...100",
                .usage       =
                        "set threshold for maximal dimming via compositor\n"
                        "valid values are: 0-100 [%% of hw maximum]\n"
                        "\n"
                        "If difference between on brightness and dimmed brightness\n"
                        "derived from default and maximum settings is smaller than\n"
                        "threshold, fade-to-blank on compositor side is used to make\n"
                        "the display dimming more visible to the user.\n"
        },
        {
                .name        = "set-compositor-dimming-threshold-lo",
                .with_arg    = xmce_set_compositor_dimming_lo,
                .values      = "0...100",
                .usage       =
                        "set threshold for minimal dimming via compositor\n"
                        "valid values are: 0-100 [%% of hw maximum]\n"
                        "\n"
                        "If difference between on brightness and dimmed brightness\n"
                        "derived from default and maximum settings is smaller than\n"
                        "this threshold, but still larger than the high threshold,\n"
                        "limited opacity dimming via compositor is used.\n"
                        "\n"
                        "If low threshold is set smaller than the high threshold,\n"
                        "the low threshold is ignored.\n"
        },
        {
                .name        = "set-als-mode",
                .flag        = 'g',
                .with_arg    = xmce_set_als_mode,
                .values      = "enabled|disabled",
                .usage       =
                        "set the als master mode; valid modes are:\n"
                        "'enabled' and 'disabled'\n"
                        "\n"
                        "If disabled, mce will never power up the ambient light\n"
                        "sensor. If enabled, ALS is used depending on device.\n"
                        "state and feature specific settings.\n"
        },
        {
                .name        = "set-als-autobrightness",
                .with_arg    = xmce_set_als_autobrightness,
                .values      = "enabled|disabled",
                .usage       =
                        "use the als for automatic brightness tuning; valid modes are:\n"
                        "'enabled' and 'disabled'\n"
                        "\n"
                        "When enabled, affects display, notification led and keypad\n"
                        "backlight brightness.\n"
        },
        {
                .name        = "set-als-input-filter",
                .with_arg    = xmce_set_als_input_filter,
                .values      = "disabled|median",
                .usage       =
                        "set the als input filter; valid filters are:\n"
                        "'disabled', 'median'\n"
        },
        {
                .name        = "set-als-sample-time",
                .with_arg    = xmce_set_als_sample_time,
                .values      = "50...1000",
                .usage       =
                        "set the sample slot size for als input filtering;\n"
                        "valid values are: 50-1000\n"
        },

        {
                .name        = "set-ps-mode",
                .flag        = 'u',
                .with_arg    = xmce_set_ps_mode,
                .values      = "enabled|disabled",
                        "set the ps mode; valid modes are:\n"
                        "'enabled'  sensor is disabled/enabled based on policy\n"
                        "           and other settings\n"
                        "'disabled' sensor is never powered on by mce\n"
        },
        {
                .name        = "set-ps-on-demand",
                .with_arg    = xmce_set_ps_on_demand,
                .values      = "enabled|disabled",
                        "set the ps on-demand mode; valid modes are:\n"
                        "'enabled'  sensor is powered up only when needed\n"
                        "'disabled' sensor is always kept powered on\n"
        },
        {
                .name        = "set-default-ps-uncover-delay",
                .with_arg    = xmce_set_default_ps_uncover_delay,
                .values      = "ms",
                        "set the default ps uncover delay in milliseconds\n"
        },
        {
                .name        = "set-incall-ps-uncover-delay",
                .with_arg    = xmce_set_incall_ps_uncover_delay,
                .values      = "ms",
                        "set the incall ps uncover delay in milliseconds\n"
        },
        {
                .name        = "set-ps-blocks-touch",
                .with_arg    = xmce_set_ps_blocks_touch,
                .values      = "enabled|disabled",
                        "allow ps to block touch input; valid modes are:\n"
                        "'enabled' and 'disabled'\n"
        },
        {
                .name        = "set-ps-acts-as-lid",
                .with_arg    = xmce_set_ps_acts_as_lid,
                .values      = "enabled|disabled",
                        "make ps act as lid sensor; valid modes are:\n"
                        "'enabled' and 'disabled'\n"
        },
        {
                .name        = "set-lid-sensor-mode",
                .with_arg    = xmce_set_lid_sensor_mode,
                .values      = "enabled|disabled",
                        "set the lid sensor mode; valid modes are:\n"
                        "'enabled' and 'disabled'\n"
        },
        {
                .name        = "set-lid-open-actions",
                .with_arg    = xmce_set_lid_open_actions,
                .values      = "disabled|unblank|tkunlock",
                        "set the lid open actions; valid modes are:\n"
                        "'disabled' ignore lid open\n"
                        "'unblank'  unblank (and show lockscreen)\n"
                        "'tkunlock' unblank and deactivate lockscreen (if possible)\n"
        },
        {
                .name        = "set-lid-close-actions",
                .with_arg    = xmce_set_lid_close_actions,
                .values      = "disabled|blank|tklock",
                        "set the lid close actions; valid modes are:\n"
                        "'disabled' ignore lid close\n"
                        "'blank'    blank display\n"
                        "'tklock'   blank display and activate lockscreen\n"
        },
        {
                .name        = "set-kbd-slide-open-trigger",
                .with_arg    = xmce_set_kbd_slide_open_trigger,
                .values      = "never|no-proximity|always",
                        "When to react to kbd slide opened event:\n"
                        "  never         never\n"
                        "  no-proximity  if proximity sensor is not covered\n"
                        "  always        always\n"
        },
        {
                .name        = "set-kbd-slide-open-actions",
                .with_arg    = xmce_set_kbd_slide_open_actions,
                .values      = "disabled|unblank|tkunlock",
                        "How to react to kbd slide opened event:\n"
                        "  disabled  do nothing\n"
                        "  unblank   unblank (and show lockscreen)\n"
                        "  tkunlock  unblank and deactivate lockscreen (if possible)\n"
        },
        {
                .name        = "set-kbd-slide-close-trigger",
                .with_arg    = xmce_set_kbd_slide_close_trigger,
                .values      = "never|after-open|always",
                        "When to react to kbd slide closed event:\n"
                        "  never       never\n"
                        "  after-open  if display was unblanked due to kbd slide open\n"
                        "  always      always\n"
                        "\n"
                        "Note: Display state policy can overrule this setting,\n"
                        "      so that for example display does not blank during\n"
                        "      alarms / incoming calls.\n"
        },
        {
                .name        = "set-kbd-slide-close-actions",
                .with_arg    = xmce_set_kbd_slide_close_actions,
                .values      = "disabled|blank|tklock",
                        "How to react to kbd slide closed event:\n"
                        "  disabled  do nothing\n"
                        "  blank     blank display\n"
                        "  tklock    blank display and activate lockscreen\n"
        },
        {
                .name        = "set-filter-lid-with-als",
                .with_arg    = xmce_set_filter_lid_with_als,
                .values      = "enabled|disabled",
                        "set filter lid close events with als mode; valid modes are:\n"
                        "'enabled' and 'disabled'\n"
                        "\n"
                        "When enabled, lid closed events are acted on only if they\n"
                        "happen in close proximity to light level drop.\n"
        },
        {
                .name        = "set-filter-lid-als-limit",
                .with_arg    = xmce_set_filter_lid_als_limit,
                .values      = "lux",
                        "set limit for how much light als can report when lid is closed\n"
                        "\n"
                        "Lid closed event is ignored unless it is associated with als\n"
                        "reporting lux>limit -> lux<=limit drop.\n"
                        "\n"
                        "Lid opened event is ignored unless it is associated with als\n"
                        "reporting lux<=limit -> lux>limit raise.\n"
        },
        {
                .name        = "set-orientation-sensor-mode",
                .with_arg    = xmce_set_orientation_sensor_mode,
                .values      = "enabled|disabled",
                        "set the orientation sensor master toggle; valid modes are:\n"
                        "  'enabled'  mce is allowed to use orientation sensor\n"
                        "  'disabled' all orientation sensor features are disabled\n"
        },

        {
                .name        = "set-orientation-change-is-activity",
                .with_arg    = xmce_set_orientation_change_is_activity,
                .values      = "enabled|disabled",
                        "set the orientation change cancels inactivity toggle; valid modes are:\n"
                        "  'enabled'  orientation changes keep display on etc\n"
                        "  'disabled' orientation changes do not affect inactivity state \n"
        },
        {
                .name        = "set-flipover-gesture-detection",
                .with_arg    = xmce_set_flipover_gesture_detection,
                .values      = "enabled|disabled",
                        "set the flipover gesture detection toggle; valid modes are:\n"
                        "  'enabled'  flipover gestures can be used to silence calls/alarms\n"
                        "  'disabled' turning device over does not affect calls/alarms\n"
        },
        {
                .name        = "get-color-profile-ids",
                .flag        = 'a',
                .without_arg = xmce_get_color_profile_ids,
                .usage       =
                        "get available color profile ids\n"
        },
        {
                .name        = "set-color-profile",
                .flag        = 'A',
                .with_arg    = xmce_set_color_profile,
                .values      = "ID",
                .usage       =
                        "set the color profile to ID; valid ID names\n"
                        "can be obtained with --get-color-profile-ids\n"
        },
        {
                .name        = "set-cabc-mode",
                .flag        = 'C',
                .with_arg    = xmce_set_cabc_mode,
                .values      = "off|ui|still-image|moving-image",
                .usage       =
                        "set the CABC mode\n"
                        "valid modes are:\n"
                        "'off', 'ui', 'still-image' and 'moving-image'\n"
        },
        {
                .name        = "set-call-state",
                .flag        = 'c',
                .with_arg    = xmce_set_call_state,
                .values      = "none|ringing|active|service>:<normal|emergency",
                .usage       =
                        "set the call state and type\n"
                        "Valid states are: none, ringing, active and service.\n"
                        "Valid types are: normal and emergency.\n"
        },
        {
                .name        = "set-button-backlight",
                .with_arg    = xmce_set_button_backlight,
                .values      = "enabled|disabled",
                .usage       =
                        "request button backlight state\n"
                        "Valid states are: enabled and disabled.\n"
        },
        {
                .name        = "set-button-backlight-mode",
                .with_arg    = xmce_set_button_backlight_mode,
                .values      = "off|on|policy",
                .usage       =
                        "request button backlight mode\n"
                        "Valid modes are: off|on|policy.\n"
        },
        {
                .name        = "set-button-backlight-off-delay",
                .with_arg    = xmce_set_button_backlligut_off_delay,
                .values      = "ms",
                .usage       =
                        "set delay for powering off button backlight.\n"
                        "\n"
                        "Set delay in ms for powering off the backlight for\n"
                        "menu/home/back buttons.\n"
                        "\n"
                        "Use zero to keep the buttons light as long as the\n"
                        "topmost application / system is prepared to handle\n"
                        "button presses.\n"
        },
        {
                .name        = "enable-led",
                .flag        = 'l',
                .without_arg = mcetool_do_enable_led,
                .usage       =
                        "enable LED framework\n"
        },
        {
                .name        = "disable-led",
                .flag        = 'L',
                .without_arg = mcetool_do_disable_led,
                .usage       =
                        "disable LED framework\n"
        },
        {
                .name        = "enable-led-pattern",
                .with_arg    = mcetool_do_enable_pattern,
                .values      = "PATTERN",
                .usage       =
                        "allow activating of a LED pattern\n"
        },
        {
                .name        = "disable-led-pattern",
                .with_arg    = mcetool_do_disable_led_pattern,
                .values      = "PATTERN",
                .usage       =
                        "deny activating of a LED pattern\n"
        },
        {
                .name        = "show-led-patterns",
                .without_arg = mcetool_show_led_patterns,
                .usage       =
                        "show status of LED patterns that can be disabled/enabled\n"
        },
        {
                .name        = "activate-led-pattern",
                .flag        = 'y',
                .with_arg    = mcetool_do_activate_pattern,
                .values      = "PATTERN",
                .usage       =
                        "activate a LED pattern\n"
        },
        {
                .name        = "deactivate-led-pattern",
                .flag        = 'Y',
                .with_arg    = mcetool_do_deactivate_pattern,
                .values      = "PATTERN",
                .usage       =
                        "deactivate a LED pattern\n"
        },
        {
                .name        = "set-sw-breathing",
                .with_arg    = set_led_breathing_enabled,
                .values      = "enabled|disabled",
                .usage       =
                        "Allow/deny using smooth timer based led transitions instead of just\n"
                        "HW based blinking. Note that enabling this feature means that the\n"
                        "device can't suspend while the led is breathing which will increase\n"
                        "the battery consumption significantly.\n"
        },
        {
                .name        = "set-sw-breathing-limit",
                .with_arg    = set_led_breathing_limit,
                .values      = "0 ... 100",
                .usage       =
                        "If charger is not connected, the led breathing is enabled only if\n"
                        "battery level is greater than the limit given. Setting limit to 100%\n"
                        "allows breathing only when charger is connected.\n"
        },
        {
                .name        = "powerkey-event",
                .flag        = 'e',
                .with_arg    = xmce_powerkey_event,
                .values      = "short|double|long",
                .usage       =
                        "trigger a powerkey event; valid types are:\n"
                        "'short', 'double' and 'long'\n"
        },
        {
                .name        = "set-demo-mode",
                .flag        = 'D',
                .with_arg    = xmce_set_demo_mode,
                .values      = "on|dim|off",
                .usage       =
                        "set the display blanking policy to demo mode\n"
                        "\n"
                        "Valid modes are:\n"
                        "  on  - display is kept on\n"
                        "  dim - display is allowed to dim, but not blank\n"
                        "  off - display is allowed to turn off\n"
        },
        {
                .name        = "set-brightness-fade-def",
                .with_arg    = xmce_set_brightness_fade_default,
                .values      = "msecs",
                .usage       =
                        "set the default brightness fade duration\n"
        },
        {
                .name        = "set-brightness-fade-dim",
                .with_arg    = xmce_set_brightness_fade_dimming,
                .values      = "msecs",
                .usage       =
                        "set the dim brightness fade duration\n"
        },
        {
                .name        = "set-brightness-fade-als",
                .with_arg    = xmce_set_brightness_fade_als,
                .values      = "msecs",
                .usage       =
                        "set the als brightness fade duration\n"
        },
        {
                .name        = "set-brightness-fade-blank",
                .with_arg    = xmce_set_brightness_fade_blank,
                .values      = "msecs",
                .usage       =
                        "set the blank brightness fade duration\n"
        },
        {
                .name        = "set-brightness-fade-unblank",
                .with_arg    = xmce_set_brightness_fade_unblank,
                .values      = "msecs",
                .usage       =
                        "set the unblank brightness fade duration\n"
        },
        {
                .name        = "set-lipstick-core-delay",
                .with_arg    = xmce_set_lipstick_core_delay,
                .values      = "secs",
                .usage       =
                        "set the delay for dumping core from unresponsive lipstick\n"
        },
        {
                .name        = "set-input-policy-mode",
                .with_arg    = xmce_set_input_policy_mode,
                .values      = "enabled|disabled",
                .usage       =
                        "allow/deny grabbing of input devices based on input policy\n"
                        "\n"
                        "Normally this should be always set to 'enabled'.\n"
                        "\n"
                        "Setting to 'disabled' can be useful when debugging things like\n"
                        "unresponsive touch screen: If the issue goes away when mce is\n"
                        "allowed to grab input device, problem is likely to reside in\n"
                        "mce policy logic. If the problem persists, the problem is more\n"
                        "likely to exist at the ui side input handling logic.\n"
        },
        {
                .name        = "set-input-grab-allowed",
                .with_arg    = xmce_set_input_grab_allowed,
                .values      = "bit1[,bit2][...]",
                .usage       =
                        "set the input devices that mce is allowed to grab based on policy\n"
                        "\n"
                        "Valid input device types to use are:\n"
                        "  none - no input files will be grabbed by mce\n"
                        "  ts   - allow mce to grab touch screen devices\n"
                        "  kp   - allow mce to grab keypad devices (with volkeys)\n"
        },
        {
                .name        = "set-touch-unblock-delay",
                .with_arg    = xmce_set_touch_unblock_delay,
                .values      = "msecs",
                .usage       =
                        "set the delay for ending touch blocking after unblanking\n"
        },
        {
                .name        = "begin-notification",
                .with_arg    = xmce_notification_begin,
                .without_arg = xmce_notification_begin,
                .values      = "name[,duration_ms[,renew_ms]]",
                .usage       =
                        "start notification ui exception\n"
        },
        {
                .name        = "end-notification",
                .with_arg    = xmce_notification_end,
                .without_arg = xmce_notification_end,
                .values      = "name[,linger_ms]",
                .usage       =
                        "end notification ui exception\n"
        },
        {
                .name        = "status",
                .flag        = 'N',
                .without_arg = xmce_get_status,
                .usage       =
                        "output MCE status\n"
        },
        {
                .name        = "block",
                .flag        = 'B',
                .with_arg    = mcetool_block,
                .without_arg = mcetool_block,
                .values      = "secs",
                .usage       =
                        "Block after executing commands\n"
                        "for D-Bus\n"
        },
        {
                .name        = "help",
                .flag        = 'h',
                .with_arg    = mcetool_do_help,
                .without_arg = mcetool_do_help,
                .values      = "OPTION|\"all\"",
                .usage       =
                        "display list of options and exit\n"
                        "\n"
                        "If the optional argument is given, more detailed information is\n"
                        "given about matching options. Using \"all\" lists all options\n"
        },
        {
                .name        = "long-help",
                .flag        = 'H',
                .with_arg    = mcetool_do_long_help,
                .without_arg = mcetool_do_long_help,
                .values      = "OPTION",
                .usage       =
                        "display full usage information  and exit\n"
                        "\n"
                        "If the optional argument is given, information is given only\n"
                        "about matching options.\n"
        },
        {
                .name        = "version",
                .flag        = 'V',
                .without_arg = mcetool_do_version,
                .usage       =
                        "output version information and exit\n"

        },
        {
                .name        = "set-verbosity",
                .with_arg    = xmce_set_verbosity,
                .values      = "emerg|alert|crit|err|warning|notice|info|debug",
                .usage       =
                        "set the mce verbosity level\n"
                        "\n"
                        "Valid levels conform to syslog standard and are:\n"
                        "  emerg   - Silent (not used by mce)\n"
                        "  alert   - Silent (not used by mce)\n"
                        "  crit    - Critical problems that can cause mce to exit\n"
                        "  err     - Unexpected operational failures\n"
                        "  warning - Tolerable operational failures\n"
                        "  notice  - Important status changes, external triggers\n"
                        "  info    - Status changes relevant in debugging\n"
                        "  debug   - Low importance changes/often occurring events\n"
        },
        {
                .name        = "set-memuse-warning-used",
                .with_arg    = xmce_set_memnotify_warning_used,
                .values      = "page_count",
                .usage       =
                        "set warning limit for used memory pages; zero=disabled\n"
                        "\n"
                        "Also more human readable values like 100M can be used.\n"
        },
        {
                .name        = "set-memuse-warning-active",
                .with_arg    = xmce_set_memnotify_warning_active,
                .values      = "page_count",
                .usage       =
                        "set warning limit for active memory pages; zero=disabled\n"
        },
        {
                .name        = "set-memuse-critical-used",
                .with_arg    = xmce_set_memnotify_critical_used,
                .values      = "page_count",
                .usage       =
                        "set critical limit for used memory pages; zero=disabled\n"
        },
        {
                .name        = "set-memuse-critical-active",
                .with_arg    = xmce_set_memnotify_critical_active,
                .values      = "page_count",
                .usage       =
                        "set critical limit for active memory pages; zero=disabled\n"
        },
        {
                .name        = "set-exception-length-call-in",
                .with_arg    = xmce_set_exception_length_call_in,
                .values      = "msec",
                .usage       =
                        "how long to keep display on after incoming call"
        },
        {
                .name        = "set-exception-length-call-out",
                .with_arg    = xmce_set_exception_length_call_out,
                .values      = "msec",
                .usage       =
                        "how long to keep display on after outgoing call"
        },
        {
                .name        = "set-exception-length-alarm",
                .with_arg    = xmce_set_exception_length_alarm,
                .values      = "msec",
                .usage       =
                        "how long to keep display on after alarm"
        },
        {
                .name        = "set-exception-length-usb-connect",
                .with_arg    = xmce_set_exception_length_usb_connect,
                .values      = "msec",
                .usage       =
                        "how long to keep display on at usb connect"
        },
        {
                .name        = "set-exception-length-usb-dialog",
                .with_arg    = xmce_set_exception_length_usb_dialog,
                .values      = "msec",
                .usage       =
                        "how long to keep display on at usb mode query"
        },
        {
                .name        = "set-exception-length-charger",
                .with_arg    = xmce_set_exception_length_charger,
                .values      = "msec",
                .usage       =
                        "how long to keep display on at charging start"
        },
        {
                .name        = "set-exception-length-battery",
                .with_arg    = xmce_set_exception_length_battery,
                .values      = "msec",
                .usage       =
                        "how long to keep display on at battery full"
        },
        {
                .name        = "set-exception-length-jack-in",
                .with_arg    = xmce_set_exception_length_jack_in,
                .values      = "msec",
                .usage       =
                        "how long to keep display on at jack insert"
        },
        {
                .name        = "set-exception-length-jack-out",
                .with_arg    = xmce_set_exception_length_jack_out,
                .values      = "msec",
                .usage       =
                        "how long to keep display on at jack remove"
        },
        {
                .name        = "set-exception-length-camera",
                .with_arg    = xmce_set_exception_length_camera,
                .values      = "msec",
                .usage       =
                        "how long to keep display on at camera button\n"
                        "\n"
                        "Note: this is unverified legacy feature.\n"
        },
        {
                .name        = "set-exception-length-volume",
                .with_arg    = xmce_set_exception_length_volume,
                .values      = "msec",
                .usage       =
                        "how long to keep display on at volume button"
        },
        {
                .name        = "set-exception-length-activity",
                .with_arg    = xmce_set_exception_length_activity,
                .values      = "msec",
                .usage       =
                        "how much user activity extends display on"
        },
        {
                .name        = "reset-settings",
                .without_arg = xmce_reset_settings,
                .with_arg    = xmce_reset_settings,
                .values      = "keyish",
                .usage       =
                        "reset matching settings back to configuration default.\n"
                        "\n"
                        "All settings whose key name contains the given subpart\n"
                        "will be reset to defaults set in /etc/mce/*.conf files.\n"
                        "If no keyish is given, all settings are reset.\n"
        },
        {
                .name        = "set-inactivity-shutdown-delay",
                .with_arg    = xmce_set_inactivity_shutdown_delay,
                .values      = "s",
                .usage       =
                        "set delay in seconds for automatic shutdown\n"
                        "\n"
                        "If the device is not in active use it will be\n"
                        "automatically powered off after the given delay.\n"
                        "\n"
                        "Mostly this is assumed to be useful for developing\n"
                        "purposes where juggling between several devices\n"
                        "running in some blanking inhibit mode easily means\n"
                        "that the device you need the next has empty battery.\n"
                        "\n"
                        "Using value smaller than "
                        G_STRINGIFY(MCE_MINIMUM_INACTIVITY_SHUTDOWN_DELAY)
                        " disables the feature.\n"
        },
#ifdef ENABLE_BATTERY_SIMULATION
        {
                .name        = "set-charger-type",
                .with_arg    = mcetool_do_set_charger_type,
                .values      =
                        MCE_CHARGER_TYPE_NONE "|"
                        MCE_CHARGER_TYPE_USB "|"
                        MCE_CHARGER_TYPE_DCP "|"
                        MCE_CHARGER_TYPE_HVDCP "|"
                        MCE_CHARGER_TYPE_CDP "|"
                        MCE_CHARGER_TYPE_WIRELESS "|"
                        MCE_CHARGER_TYPE_OTHER,
                .usage       =
                        "Override charger type for debugging purposes\n"
        },
        {
                .name        = "set-charger-state",
                .with_arg    = mcetool_do_set_charger_state,
                .values      =
                        MCE_CHARGER_STATE_UNKNOWN"|"
                        MCE_CHARGER_STATE_ON"|"
                        MCE_CHARGER_STATE_OFF,
                .usage       =
                        "Override charger state for debugging purposes\n"
        },
        {
                .name        = "set-battery-level",
                .with_arg    = mcetool_do_set_battery_level,
                .values      = "percent",
                .usage       =
                        "Override battery level for debugging purposes\n"
                        "\n"
                        "values < 0 signify: unknown level\n"
                        "values > 100 signify: 100% + battery full\n"
        },
#endif // ENABLE_BATTERY_SIMULATION
        {
                .name        = "set-charging-mode",
                .with_arg    = xmce_set_charging_mode,
                .values      = ""
                        "enable|"
                        "disable|"
                        "apply-thresholds|"
                        "apply-thresholds-after-full",
                .usage       =
                        "Set charging enable/disable mode\n"
                        "\n"
                        "Valid modes are:\n"
                        "  enable           - charging is always enabled (default)\n"
                        "  disable          - charging is always disabled\n"
                        "  apply-thresholds - charging is disabled when level reaches disable limit\n"
                        "                     and enabled when level drops to enable limit\n"
                        "  apply-thresholds-after-full - charging is enabled until battery full is\n"
                        "                     reached, then as with apply-thresholds\n"
        },
        {
                .name        = "set-forced-charging",
                .with_arg    = xmce_set_forced_charging_mode,
                .values      = ""
                        MCE_FORCED_CHARGING_ENABLED
                        "|" MCE_FORCED_CHARGING_DISABLED,
                .usage       =
                        "Set charging mode override\n"
                        "\n"
                        "Valid modes are:\n"
                        "  enabled          - charging mode is ignored and battery is charged\n"
                        "  disabled         - battery is charged according to charging mode\n"
                        "\n"
                        "Automatically disabled when battery gets full or charger is disconnected.\n"
        },
        {
                .name        = "set-charging-enable-limit",
                .with_arg    = xmce_set_charging_enable_limit,
                .values      = "percent",
                .usage       =
                        "Set charging enable limit\n"
                        "\n"
                        "Charging is enabled when battery level drops to enable limit or below.\n"

        },
        {
                .name        = "set-charging-disable-limit",
                .with_arg    = xmce_set_charging_disable_limit,
                .values      = "percent",
                .usage       =
                        "Set charging disabled limit\n"
                        "\n"
                        "Charging is disabled when battery level reaches disable limit or above.\n"
        },

        // sentinel
        {
                .name = 0
        }
};

/** Version information */
static const char version_text[] =
PROG_NAME" v"G_STRINGIFY(PRG_VERSION)"\n"
"Written by David Weinehall.\n"
"\n"
"Copyright (c) 2005 - 2011 Nokia Corporation.  All rights reserved.\n"
"Copyright (c) 2012 - 2022 Jolla Ltd.\n"
"Copyright (c) 2019 - 2020 Open Mobile Platform LLC.\n"
;

static __attribute__((__noreturn__)) bool mcetool_do_version(const char *arg)
{
        (void)arg;

        printf("%s\n", version_text);
        exit(EXIT_SUCCESS);
}

static __attribute__((__noreturn__)) bool mcetool_do_help(const char *arg)
{
        fprintf(stdout,
                "Mode Control Entity Tool\n"
                "\n"
                "USAGE\n"
                "\t"PROG_NAME" [OPTION] ...\n"
                "\n"
                "OPTIONS\n");

        mce_command_line_usage(options, arg);

        fprintf(stdout,
                "\n"
                "      --set-xxx=</key|/reset>\n"
                "      --set-touchscreen-gesture-actions=<gesture_id,/key|/reset>\n"
                "      --set-powerkey-dbus-action=<action_id,/key|/reset>\n%s",
                g_strcmp0(arg, "all") ? "" :
                "        All --set-xxx options that affect a signle\n"
                "        persistent setting accept the following values:\n"
                "          /key     Print setting key path\n"
                "          /reset   Reset setting to configured default\n"
                "\n"
                "        See also: --reset-settings\n");

        fprintf(stdout,
                "\n"
                "NOTES\n"
                "\tIf no options are specified, the status is output.\n"
                "\n"
                "\tIf non-option arguments are given, matching parts of long help\n"
                "\tis printed out.\n");

        fprintf(stdout,
                "\n"
                "REPORTING BUGS\n"
                "\tSend e-mail to: <simo.piiroinen@jollamobile.com>\n");

        exit(EXIT_SUCCESS);
}

static __attribute__((__noreturn__)) bool mcetool_do_long_help(const char *arg)
{
        mcetool_do_help(arg ?: "all");
}

/* ========================================================================= *
 * MCETOOL ENTRY POINT
 * ========================================================================= */

/** Main
 *
 * @param argc Number of command line arguments
 * @param argv Array with command line arguments
 * @return 0 on success, non-zero on failure
 */
int main(int argc, char **argv)
{
        int exitcode = EXIT_FAILURE;

        /* No args -> show mce status */
        if( argc < 2 )
                xmce_get_status(0);

        /* Parse the command-line options */
        if( !mce_command_line_parse(options, argc, argv) )
                goto EXIT;

        /* Non-flag arguments are quick help patterns */
        if( optind < argc ) {
                mce_command_line_usage_keys(options, argv + optind);
        }

        exitcode = EXIT_SUCCESS;

EXIT:
        xdbus_exit();

        return exitcode;
}
