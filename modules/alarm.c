/**
 * @file alarm.c
 * Alarm interface module for the Mode Control Entity
 * <p>
 * Copyright © 2005-2009 Nokia Corporation and/or its subsidiary(-ies).
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

#include "../mce.h"
#include "../mce-log.h"
#include "../mce-dbus.h"
#include "../mce-wakelock.h"

#include <gmodule.h>

/* Alarm UI related D-Bus constants */
typedef enum {
    VISUAL_REMINDER_ON_SCREEN,
    VISUAL_REMINDER_NOT_ON_SCREEN,
    VISUAL_REMINDER_ON_SCREEN_NO_SOUND
} visual_reminders_status;

#define VISUAL_REMINDERS_SERVICE        "com.nokia.voland"
#define VISUAL_REMINDERS_SIGNAL_IF      "com.nokia.voland.signal"
#define VISUAL_REMINDERS_SIGNAL_PATH    "/com/nokia/voland/signal"
#define VISUAL_REMINDER_STATUS_SIG      "visual_reminders_status"

/* Timed alarm queue related D-Bus constants */
#define TIMED_DBUS_SERVICE              "com.nokia.time"
#define TIMED_DBUS_OBJECT               "/com/nokia/time"
#define TIMED_DBUS_INTERFACE            "com.nokia.time"
#define TIMED_QUEUE_STATUS_SIG          "next_bootup_event"

/** Module name */
#define MODULE_NAME                     "alarm"

/** Maximum number of alarm D-Bus objects requesting alarm mode */
#define ALARM_MAX_MONITORED             5

/** Pseudo-wakelock held while expecting alarm ui to start up */
#define ALARM_IMMINENT_WAKELOCK_NAME    "alarm_imminent"

/** Maximum time given for alarm ui to start up
 *
 * This needs to be long enough to allow timed to make at least one
 * retry after timeout from  alarm ui invocation D-Bus method call,
 * i.e. must be longer than 25 seconds.
 */
#define ALARM_IMMINENT_TIMEOUT_MS       (60*1000)

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Alarm UI D-Bus service monitor list */
static GSList *alarm_owner_monitor_list = NULL;

/** Alarm queue D-Bus service monitor list */
static GSList *queue_owner_monitor_list = NULL;

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
    /** Name of the module */
    .name = MODULE_NAME,
    /** Module provides */
    .provides = provides,
    /** Module priority */
    .priority = 250
};

/* Module functions */
static void     alarm_sync_state_to_datapipe(alarm_ui_state_t state);

static gboolean alarm_owner_monitor_dbus_cb (DBusMessage *const msg);
static void     setup_alarm_dbus_monitor    (const gchar *sender);

static gboolean queue_owner_monitor_dbus_cb (DBusMessage *const sig);
static void     queue_monitor_setup         (const char *sender, bool monitor);

static gboolean alarm_dialog_status_dbus_cb (DBusMessage *const msg);
static gboolean alarm_queue_status_dbus_cb  (DBusMessage *const sig);

static void     mce_alarm_init_dbus         (void);
static void     mce_alarm_quit_dbus         (void);

const gchar    *g_module_check_init         (GModule *module);
void            g_module_unload             (GModule *module);

static void alarm_sync_state_to_datapipe(alarm_ui_state_t state)
{
    if( datapipe_get_gint(alarm_ui_state_pipe) == state )
        goto EXIT;

    mce_log(LL_DEVEL, "alarm state = %s", alarm_state_repr(state));
    datapipe_exec_full(&alarm_ui_state_pipe,
                       GINT_TO_POINTER(state),
                       DATAPIPE_USE_INDATA, DATAPIPE_CACHE_INDATA);

EXIT:
    return;
}

/**
 * Alarm D-Bus service monitor callback.
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean alarm_owner_monitor_dbus_cb(DBusMessage *const msg)
{
    gboolean status = FALSE;
    const gchar *old_name;
    const gchar *new_name;
    const gchar *service;
    gssize retval;

    DBusError error = DBUS_ERROR_INIT;

    /* Extract result */
    if (dbus_message_get_args(msg, &error,
                              DBUS_TYPE_STRING, &service,
                              DBUS_TYPE_STRING, &old_name,
                              DBUS_TYPE_STRING, &new_name,
                              DBUS_TYPE_INVALID) == FALSE) {
        mce_log(LL_ERR,
                "Failed to get argument from %s.%s; %s",
                "org.freedesktop.DBus", "NameOwnerChanged",
                error.message);
        goto EXIT;
    }

    retval = mce_dbus_owner_monitor_remove(service, &alarm_owner_monitor_list);

    if (retval == 0) {
        /* We didn't get alarm off from the same service before it
         * unregistered (e.g. due crash), turn alarm state off so at
         * least powerkey works again.
         */
        mce_log(LL_DEBUG, "visual reminder service died, "
                "turning off alarm state");
        alarm_sync_state_to_datapipe(MCE_ALARM_UI_OFF_INT32);
    }

    status = TRUE;

EXIT:
    dbus_error_free(&error);
    return status;
}

/**
 * Install alarm D-Bus service monitor callback.
 *
 * @param sender sender D-Bus address
 */
static void setup_alarm_dbus_monitor(const gchar* sender)
{
    mce_log(LL_DEBUG, "adding dbus monitor for: '%s'" ,sender);
    /* No need to check return value, if it does not succeed, not much
     * we can do / fall back to
     */
    mce_dbus_owner_monitor_add(sender,
                               alarm_owner_monitor_dbus_cb,
                               &alarm_owner_monitor_list,
                               ALARM_MAX_MONITORED);
}

/** Callback for handling alarm queue name owner changed signals
 *
 * @param sig The D-Bus message
 *
 * @return TRUE
 */
static gboolean queue_owner_monitor_dbus_cb(DBusMessage *const sig)
{
    const char *name  = 0;
    const char *prev  = 0;
    const char *curr  = 0;
    DBusError   error = DBUS_ERROR_INIT;

    if( !dbus_message_get_args(sig, &error,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_STRING, &prev,
                               DBUS_TYPE_STRING, &curr,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to parse arguments: %s: %s",
                error.name, error.message);
        goto EXIT;
    }

    queue_monitor_setup(name, false);

EXIT:
    dbus_error_free(&error);
    return TRUE;

}

/** Install/remove alarm queue D-Bus name owner monitor
 *
 * @param sender   Private D-Bus name to monitor
 * @param monitor  true/false to start/stop monitoring
 */
static void queue_monitor_setup(const char *sender, bool monitor)
{
    if( monitor ) {
        gssize cnt = mce_dbus_owner_monitor_add(sender,
                                                queue_owner_monitor_dbus_cb,
                                                &queue_owner_monitor_list,
                                                ALARM_MAX_MONITORED);
        if( cnt != -1 ) {
            /* A owner monitor was added/renewed */
            mce_log(LL_DEVEL, "monitoring dbus name: %s", sender);
            mce_wakelock_obtain(ALARM_IMMINENT_WAKELOCK_NAME,
                                ALARM_IMMINENT_TIMEOUT_MS);
        }
    }
    else {
        gssize cnt = mce_dbus_owner_monitor_remove(sender,
                                                   &queue_owner_monitor_list);
        if( cnt == 0 ) {
            /* The last monitor was removed */
            mce_log(LL_DEVEL, "all dbus name monitors removed");
            mce_wakelock_release(ALARM_IMMINENT_WAKELOCK_NAME);
        }
    }
}

/**
 * D-Bus callback for the alarm dialog status signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean alarm_dialog_status_dbus_cb(DBusMessage *const msg)
{
    alarm_ui_state_t alarm_ui_state = MCE_ALARM_UI_INVALID_INT32;
    gboolean status = FALSE;
    const gchar *sender = dbus_message_get_sender(msg);
    DBusError error = DBUS_ERROR_INIT;
    dbus_int32_t dialog_status;

    mce_log(LL_DEVEL, "Received alarm dialog status signal from %s",
            mce_dbus_get_name_owner_ident(sender));

    if (dbus_message_get_args(msg, &error,
                              DBUS_TYPE_INT32, &dialog_status,
                              DBUS_TYPE_INVALID) == FALSE) {
        // XXX: should we return an error instead?
        mce_log(LL_CRIT,
                "Failed to get argument from %s.%s: %s",
                VISUAL_REMINDERS_SIGNAL_IF,
                VISUAL_REMINDER_STATUS_SIG,
                error.message);
        goto EXIT;
    }

    /* Convert alarm dialog status to to MCE alarm ui enum */
    switch (dialog_status) {
    case VISUAL_REMINDER_ON_SCREEN:
        setup_alarm_dbus_monitor(sender);
        alarm_ui_state = MCE_ALARM_UI_RINGING_INT32;
        break;

    case VISUAL_REMINDER_ON_SCREEN_NO_SOUND:
        setup_alarm_dbus_monitor(sender);
        alarm_ui_state = MCE_ALARM_UI_VISIBLE_INT32;
        break;

    case VISUAL_REMINDER_NOT_ON_SCREEN:
        mce_dbus_owner_monitor_remove(sender, &alarm_owner_monitor_list);
        alarm_ui_state = MCE_ALARM_UI_OFF_INT32;
        break;

    default:
        mce_log(LL_ERR,
                "Received invalid alarm dialog status; "
                "defaulting to OFF");
        alarm_ui_state = MCE_ALARM_UI_OFF_INT32;
        break;
    }

    alarm_sync_state_to_datapipe(alarm_ui_state);

    status = TRUE;

EXIT:
    dbus_error_free(&error);
    return status;
}

/** D-Bus callback for the alarm queue status signal
 *
 * @param sig  The D-Bus signal message
 *
 * @return TRUE
 */
static gboolean alarm_queue_status_dbus_cb(DBusMessage *const sig)
{
        dbus_int32_t  bootup = 0;
        dbus_int32_t  normal = 0;
        DBusError     error  = DBUS_ERROR_INIT;
        const gchar  *sender = dbus_message_get_sender(sig);

        mce_log(LL_DEVEL, "Received alarm queue status signal from %s",
                mce_dbus_get_name_owner_ident(sender));

        if( !dbus_message_get_args(sig, &error,
                                   DBUS_TYPE_INT32, &bootup,
                                   DBUS_TYPE_INT32, &normal,
                                   DBUS_TYPE_INVALID) ) {
                mce_log(LL_ERR, "Failed to parse arguments: %s: %s",
                        error.name, error.message);
                goto EXIT;
        }

        /* DSME makes sure the device wakes up from suspend at
         * the time when timed needs to trigger an alarm. MCE
         * needs to make sure device does not get back to suspend
         * before alarm ui has had sufficient time to start up
         * and signal alarm dialog state.
         *
         * Timeds sends alarm queue status signal where the "next
         * alarm time" has value of one when alarm has been triggered
         * and alarm ui will be started up.
         */
        queue_monitor_setup(sender, bootup == 1 || normal == 1);

EXIT:
        dbus_error_free(&error);
        return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t alarm_dbus_handlers[] =
{
    /* signals */
    {
        .interface = VISUAL_REMINDERS_SIGNAL_IF,
        .name      = VISUAL_REMINDER_STATUS_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = alarm_dialog_status_dbus_cb,
    },
    {
        .interface = TIMED_DBUS_INTERFACE,
        .name      = TIMED_QUEUE_STATUS_SIG,
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = alarm_queue_status_dbus_cb,
    },
    /* sentinel */
    {
        .interface = 0
    }
};

/** Add dbus handlers
 */
static void mce_alarm_init_dbus(void)
{
    mce_dbus_handler_register_array(alarm_dbus_handlers);
}

/** Remove dbus handlers
 */
static void mce_alarm_quit_dbus(void)
{
    mce_dbus_handler_unregister_array(alarm_dbus_handlers);
}

/**
 * Init function for the alarm interface module
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    /* Add dbus handlers */
    mce_alarm_init_dbus();

    return NULL;
}

/**
 * Exit function for the alarm interface module
 *
 * @todo D-Bus unregistration
 *
 * @param module Unused
 */
G_MODULE_EXPORT
void g_module_unload(GModule *module)
{
    (void)module;

    /* Remove name ownership monitors */
    mce_dbus_owner_monitor_remove_all(&alarm_owner_monitor_list);
    mce_dbus_owner_monitor_remove_all(&queue_owner_monitor_list);

    /* Remove dbus handlers */
    mce_alarm_quit_dbus();

    return;
}
