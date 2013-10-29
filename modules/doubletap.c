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

/** Path to doubletap wakeup control file */
static char *dbltap_ctrl_path = 0;

/** String to write when enabling double tap wakeups */
static char *dbltap_enable_val = 0;

/** String to write when disabling double tap wakeups */
static char *dbltap_disable_val = 0;

/** Enable/disable doubletap wakeups
 *
 * @param enable true to enable wakeups, false to disable
 */
static void dbltap_enable(bool enable)
{
        static int was_enabled = -1;

        if( was_enabled == enable )
                goto EXIT;

        was_enabled = enable;

        mce_log(LL_DEBUG, "%s double tap wakeups",
                enable ? "enable" : "disable");

        const char *val = enable ? dbltap_enable_val : dbltap_disable_val;
        mce_write_string_to_file(dbltap_ctrl_path, val);
EXIT:
        return;
}

/** Re-evaluate whether doubletap wakeups should be enabled or not
 */
static void dbltap_rethink(void)
{
        bool enable = false;

        switch( dbltap_mode ) {
        default:
        case DBLTAP_ENABLE_ALWAYS:
                enable = true;
                break;

        case DBLTAP_ENABLE_NEVER:
                break;

        case DBLTAP_ENABLE_NO_PROXIMITY:
                switch( dbltap_ps_state ) {
                case COVER_CLOSED:
                        break;

                default:
                case COVER_OPEN:
                case COVER_UNDEF:
                        enable = true;
                        break;
                }
                break;
        }
        dbltap_enable(enable);
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

        /* Free config strings */
        g_free(dbltap_ctrl_path);
        g_free(dbltap_enable_val);
        g_free(dbltap_disable_val);

        return;
}
