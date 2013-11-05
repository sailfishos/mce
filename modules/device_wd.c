/* ------------------------------------------------------------------------- *
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: LGPLv2.1
 * ------------------------------------------------------------------------- */

#include "device_wd.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-conf.h"
#include "mce-io.h"

#include <string.h>

#include <glib.h>
#include <gmodule.h>

#if 0 // DEBUG: make all logging from this module "critical"
# undef mce_log
# define mce_log(LEV, FMT, ARGS...) \
        mce_log_file(LL_CRIT, __FILE__, __FUNCTION__, FMT , ## ARGS)
#endif

/** Path to watchdog kick sysfs file */
static gchar *watchdog_kick_path   = 0;

/** Value to write to sysfs file */
static gchar *watchdog_kick_value  = 0;

/** Delay between writes to sysfs file */
static gint   watchdog_kick_period = 0;

/** Timer id for periodic watchdog kicking */
static guint watchdog_timer_id = 0;

/** Write to watchdog kick sysfs file
 */
static void watchdog_kick_write(void)
{
        mce_log(LL_DEBUG, "watchdog kick");
        mce_io_save_to_existing_file(watchdog_kick_path,
                                     watchdog_kick_value,
                                     strlen(watchdog_kick_value));
}

/** Timer callback for periodic watchdog kicking
 *
 * @param aptr (not used)
 *
 * @return TRUE to keep timer repeating, or FALSE to stop it
 */
static gboolean watchdog_timer_cb(gpointer aptr)
{
        (void)aptr;

        gboolean keep_going = FALSE;

        if( !watchdog_timer_id )
                goto EXIT;

        /* there are no notifications for the transient power up/down
         * states -> need to read from the data pipe */
        switch( datapipe_get_gint(display_state_pipe) ) {
        case MCE_DISPLAY_DIM:
        case MCE_DISPLAY_ON:
                break;

        default:
                goto EXIT;
        }

        watchdog_kick_write();
        keep_going = TRUE;

EXIT:
        if( !keep_going && watchdog_timer_id ) {
                watchdog_timer_id = 0;
                mce_log(LL_DEBUG, "watchdog kicking stopped");
        }

        return keep_going;
}

/** Start periodic watchdog kicking
 */
static void watchdog_start_kicking(void)
{
        if( watchdog_kick_path && !watchdog_timer_id ) {
                watchdog_timer_id = g_timeout_add(watchdog_kick_period,
                                                  watchdog_timer_cb, 0);
                mce_log(LL_DEBUG, "watchdog kicking started");
                watchdog_kick_write();
        }
}

/** Stop periodic watchdog kicking
 */
static void watchdog_cancel_kicking(void)
{
        if( watchdog_timer_id ) {
                g_source_remove(watchdog_timer_id), watchdog_timer_id = 0;
                mce_log(LL_DEBUG, "watchdog kicking cancelled");
        }
}

/** Handle display state change
 *
 * @param data The display state stored in a pointer
 */
static void display_state_trigger(gconstpointer data)
{
        static display_state_t cached_watchdog_state = MCE_DISPLAY_UNDEF;

        display_state_t watchdog_state = GPOINTER_TO_INT(data);

        if (cached_watchdog_state == watchdog_state)
                goto EXIT;

        /* Do periodic watchdog kicks while display is on */
        switch( watchdog_state ) {
        case MCE_DISPLAY_DIM:
        case MCE_DISPLAY_ON:
                watchdog_start_kicking();
                break;
        default:
                watchdog_cancel_kicking();
                break;
        }

EXIT:
        return;
}

/** Init function for the watchdog module
 *
 * @param module Unused
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
        (void)module;

        /* Get the watchdog kicking config */
        if( !mce_conf_has_group(MCE_CONF_DEVICEWD_GROUP) ) {
                mce_log(LL_NOTICE, "watchdog not configured");
                goto EXIT;
        }

        watchdog_kick_path = mce_conf_get_string(MCE_CONF_DEVICEWD_GROUP,
                                                 MCE_CONF_DEVICEWD_KICKPATH,
                                                 NULL);
        if( !watchdog_kick_path ) {
                mce_log(LL_WARN, "watchdog output path not defined");
                goto EXIT;
        }

        watchdog_kick_value = mce_conf_get_string(MCE_CONF_DEVICEWD_GROUP,
                                                  MCE_CONF_DEVICEWD_VALUE,
                                                  NULL);
        if( !watchdog_kick_value ) {
                mce_log(LL_WARN, "watchdog output value not defined");
                goto EXIT;
        }

        watchdog_kick_period = mce_conf_get_int(MCE_CONF_DEVICEWD_GROUP,
                                                MCE_CONF_DEVICEWD_PERIOD,
                                                -1);
        if( watchdog_kick_period <= 0 ) {
                mce_log(LL_WARN, "watchdog kick period not defined");
                goto EXIT;
        }

        mce_log(LL_NOTICE, "watchdog kick every %d ms", watchdog_kick_period);

        append_output_trigger_to_datapipe(&display_state_pipe,
                                          display_state_trigger);

EXIT:
        return 0;
}

/** Exit function for the watchdog module
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
        (void)module;

        remove_output_trigger_from_datapipe(&display_state_pipe,
                                            display_state_trigger);

        watchdog_cancel_kicking();
        g_free(watchdog_kick_path);
        g_free(watchdog_kick_value);
}
