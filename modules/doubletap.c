/* ------------------------------------------------------------------------- *
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: LGPLv2
 * ------------------------------------------------------------------------- */

#include "doubletap.h"

#include "../mce.h"
#include "../mce-io.h"
#include "../mce-log.h"
#include "../mce-conf.h"
#include "../mce-gconf.h"

#include <stdbool.h>

#include <glib.h>
#include <gmodule.h>

/** Config value for doubletap enable mode */
static dbltap_mode_t dbltap_mode = DBLTAP_ENABLE_DEFAULT;

static guint dbltap_mode_gconf_id = 0;

/** Latest reported proximity sensor state */
static cover_state_t dbltap_ps_state = COVER_UNDEF;

/** Latest reported proximity blanking */
static bool dbltap_ps_blank = false;

/** Path to doubletap wakeup control file */
static char *dbltap_ctrl_path = 0;

/** String to write when enabling double tap wakeups */
static char *dbltap_enable_val = 0;

/** String to write when disabling double tap wakeups */
static char *dbltap_disable_val = 0;

/** String to write when disabling double tap wakeups,
 *  without powering off the touch detection [optional]
 */
static char *dbltap_nosleep_val = 0;

typedef enum {
        DT_UNDEF = -1,
        DT_DISABLED,
        DT_ENABLED,
        DT_DISABLED_NOSLEEP,
} dt_state_t;

static const char * const dt_state_name[] =
{
        "disabled",
        "enabled",
        "disabled-no-sleep",
};

/** Enable/disable doubletap wakeups
 *
 * @param state disable/enable/disable-without-powering-off
 */
static void dbltap_set_state(dt_state_t state)
{
        static dt_state_t prev_state = DT_UNDEF;

        if( prev_state == state )
                goto EXIT;

        prev_state = state;

        mce_log(LL_DEBUG, "double tap wakeups: %s", dt_state_name[state]);

        const char *val = 0;

        switch( state ) {
        case DT_DISABLED:
                val = dbltap_disable_val;
                break;
        case DT_ENABLED:
                val = dbltap_enable_val;
                break;
        case DT_DISABLED_NOSLEEP:
                val = dbltap_nosleep_val ?: dbltap_disable_val;
                break;
        default:
                break;
        }

        if( val )
                mce_write_string_to_file(dbltap_ctrl_path, val);
EXIT:
        return;
}

/** Re-evaluate whether doubletap wakeups should be enabled or not
 */
static void dbltap_rethink(void)
{
        dt_state_t state = DT_DISABLED;

        switch( dbltap_mode ) {
        default:
        case DBLTAP_ENABLE_ALWAYS:
                state = DT_ENABLED;
                break;

        case DBLTAP_ENABLE_NEVER:
                break;

        case DBLTAP_ENABLE_NO_PROXIMITY:
                switch( dbltap_ps_state ) {
                case COVER_CLOSED:
                        if( dbltap_ps_blank )
                                state = DT_DISABLED_NOSLEEP;
                        break;

                default:
                case COVER_OPEN:
                case COVER_UNDEF:
                        state = DT_ENABLED;
                        break;
                }
                break;
        }
        dbltap_set_state(state);
}

/** Set doubletap wakeup policy
 *
 * @param mode DBLTAP_ENABLE_ALWAYS|NEVER|NO_PROXIMITY
 */
static void dbltap_mode_set(dbltap_mode_t mode)
{
        if( dbltap_mode != mode ) {
                dbltap_mode = mode;
                dbltap_rethink();
        }
}

/** Proximity state changed callback
 *
 * @param data proximity state as void pointer
 */
static void dbltap_proximity_trigger(gconstpointer data)
{
        cover_state_t state = GPOINTER_TO_INT(data);

        if( dbltap_ps_state != state ) {
                dbltap_ps_state = state;
                dbltap_rethink();
        }
}

/** Proximity blank changed callback
 *
 * @param data proximity blank as void pointer
 */
static void dbltap_proximity_blank_trigger(gconstpointer data)
{
        cover_state_t state = GPOINTER_TO_INT(data);

        if( dbltap_ps_blank != state ) {
                dbltap_ps_blank = state;
                dbltap_rethink();
        }
}

/** GConf callback for doubletap mode setting
 *
 * @param gcc   (not used)
 * @param id    Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data  (not used)
 */
static void dbltap_mode_gconf_cb(GConfClient *const gcc, const guint id,
                                 GConfEntry *const entry, gpointer const data)
{
        (void)gcc; (void)data;

        const GConfValue *gcv;

        if( id != dbltap_mode_gconf_id )
                goto EXIT;

        gint mode = DBLTAP_ENABLE_DEFAULT;

        if( (gcv = gconf_entry_get_value(entry)) )
                mode = gconf_value_get_int(gcv);

        dbltap_mode_set(mode);
EXIT:
        return;
}

/** Init function for the doubletap module
 *
 * @param module (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
        (void)module;

        /* Config from ini-files */
        dbltap_ctrl_path = mce_conf_get_string(MCE_CONF_DOUBLETAP_GROUP,
                                               MCE_CONF_DOUBLETAP_CONTROL_PATH,
                                               NULL);

        dbltap_enable_val = mce_conf_get_string(MCE_CONF_DOUBLETAP_GROUP,
                                                MCE_CONF_DOUBLETAP_ENABLE_VALUE,
                                                "1");

        dbltap_disable_val = mce_conf_get_string(MCE_CONF_DOUBLETAP_GROUP,
                                                 MCE_CONF_DOUBLETAP_DISABLE_VALUE,
                                                 "0");

        dbltap_nosleep_val = mce_conf_get_string(MCE_CONF_DOUBLETAP_GROUP,
                                                 MCE_CONF_DOUBLETAP_DISABLE_NO_SLEEP_VALUE,
                                                 "2");

        if( !dbltap_ctrl_path || !dbltap_enable_val || !dbltap_disable_val ) {
                mce_log(LL_NOTICE, "no double tap wakeup controls defined");
                goto EXIT;
        }

        /* Runtime configuration settings */
        mce_gconf_notifier_add(MCE_GCONF_DOUBLETAP_PATH,
                               MCE_GCONF_DOUBLETAP_MODE,
                               dbltap_mode_gconf_cb,
                               &dbltap_mode_gconf_id);
        gint mode = DBLTAP_ENABLE_DEFAULT;
        mce_gconf_get_int(MCE_GCONF_DOUBLETAP_MODE, &mode);
        dbltap_mode = mode;

        /* Get initial state of datapipes */
        dbltap_ps_state = datapipe_get_gint(proximity_sensor_pipe);

        /* Append triggers/filters to datapipes */
        append_output_trigger_to_datapipe(&proximity_sensor_pipe,
                                          dbltap_proximity_trigger);

        dbltap_ps_blank = datapipe_get_gint(proximity_blank_pipe);
        append_output_trigger_to_datapipe(&proximity_blank_pipe,
                                          dbltap_proximity_blank_trigger);

        /* enable/disable double tap wakeups based on initial conditions */
        dbltap_rethink();
EXIT:
        return NULL;
}

/** Exit function for the doubletap module
 *
 * @param module (not used)
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
        (void)module;

        /* Remove triggers/filters from datapipes */
        remove_output_trigger_from_datapipe(&proximity_sensor_pipe,
                                            dbltap_proximity_trigger);
        remove_output_trigger_from_datapipe(&proximity_blank_pipe,
                                            dbltap_proximity_blank_trigger);

        /* Free config strings */
        g_free(dbltap_ctrl_path);
        g_free(dbltap_enable_val);
        g_free(dbltap_disable_val);
        g_free(dbltap_nosleep_val);

        return;
}
