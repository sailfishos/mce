/**
 * @file mce-dbus.c
 * D-Bus handling code for the Mode Control Entity
 * <p>
 * Copyright © 2004-2009 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Ismo Laitinen <ismo.laitinen@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
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

#include "mce-dbus.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-lib.h"
#include "mce-wakelock.h"

#include "systemui/dbus-names.h"

#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <grp.h>
#include <pwd.h>

#include "dbus-gmain/dbus-gmain.h"

#include <mce/dbus-names.h>
#include <dsme/thermalmanager_dbus_if.h>

/* ========================================================================= *
 * TYPES & CONSTANTS
 * ========================================================================= */

/** How long to block late suspend when mce is sending dbus messages */
#define MCE_DBUS_SEND_SUSPEND_BLOCK_MS 1000

/** Placeholder value for invalid/unknown process id */
#define PEERINFO_NO_PID ((pid_t)-1)

/** Placeholder value for invalid/unknown user id */
#define PEERINFO_NO_UID ((uid_t)-1)

/** Placeholder value for invalid/unknown group id */
#define PEERINFO_NO_GID ((gid_t)-1)

/** Root user id */
#define PEERINFO_ROOT_UID ((uid_t)0)

/** Root group id */
#define PEERINFO_ROOT_GID ((gid_t)0)

/** D-Bus handler callback function */
typedef gboolean (*handler_callback_t)(DBusMessage *const msg);

/** D-Bus handler structure */
typedef struct
{
    handler_callback_t  callback;   /**< Handler callback */
    gchar              *sender;     /**< Service name to match */
    gchar              *interface;  /**< The interface to listen on */
    gchar              *rules;      /**< Additional matching rules */
    gchar              *name;       /**< Method call or signal name */
    gchar              *args;       /**< Introspect XML data */
    int                 type;       /**< DBUS_MESSAGE_TYPE */
    bool                privileged; /**< Allowed for privileged users only */
} handler_struct_t;

/** Possible values for "privileged" peer checks */
typedef enum
{
    /** Client/service is not available / details are not known yet */
    PRIVILEGED_UNKNOWN = -1,

    /** Client/service is on D-Bus and it is not a privileged process */
    PRIVILEGED_NO,

    /** Client/service is on D-Bus and it is a privileged process */
    PRIVILEGED_YES,
} privileged_t;

/** Notification details for client exit tracking */
typedef struct peerquit_t peerquit_t;

/** Callback function type for notifying client exits */
typedef gboolean  (*peerquit_fn)(DBusMessage *const msg);

struct peerquit_t
{
    peerinfo_t *pq_peerinfo;
    peerquit_fn pq_callback;
};

/** Notification details for client state tracking */
typedef struct peernotify_t peernotify_t;

struct peernotify_t
{
    peerinfo_t    *pn_peerinfo;
    peernotify_fn  pn_callback;
    gpointer       pn_userdata;
    GDestroyNotify pn_userfree;
    guint          pn_idle_id;
};

struct peerinfo_t
{
    /** Availability / property query IPC state */
    peerstate_t      pi_state;

    /** D-Bus name to track */
    gchar           *pi_name;

    /** Unique bus name of the name owner */
    gchar           *pi_owner_name;

    /** Signal match that has been sent to D-Bus daemon */
    gchar           *pi_rule;

    /** Process id of sandbox xdg-dbus-proxy */
    pid_t            pi_proxy_pid;

    /** Process id of the D-Bus name owner */
    pid_t            pi_owner_pid;

    /** Cached effective user id of the D-Bus name owner */
    uid_t            pi_owner_uid;

    /** Cached effective group id of the D-Bus name owner */
    gid_t            pi_owner_gid;

    /** Cached command line of the Bus name owner */
    gchar           *pi_owner_cmd;

    /** Optional datapipe to use for service availability signaling */
    datapipe_t      *pi_datapipe;

    /** Client exit notifications for this D-Bus name */
    GQueue           pi_quit_callbacks; // -> peerquit_t *

    /** Client stat notifications for this D-Bus name */
    GQueue           pi_notifications; // -> peernotify_t

    /** Queue of privileged method calls waiting for peer details */
    GQueue           pi_priv_methods; // -> DBusMessage *

    /** Pending org.freedesktop.DBus.GetNameOwner method call */
    DBusPendingCall *pi_name_owner_pc;

    /** Pending org.freedesktop.DBus.GetConnectionUnixProcessID method call */
    DBusPendingCall *pi_name_pid_pc;

    /** Pending org.sailfishos.sailjailed.Identify method call */
    DBusPendingCall *pi_identify_pc;

    /** Timer for delayed delete after hitting PEERSTATE_STOPPED */
    guint            pi_delete_id;

    /** Fixed size buffer for providing peer details for debugging purposes */
    char             pi_repr[128];
};

/* ========================================================================= *
 * FUNCTIONALITY
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * SUSPEND_PROOFING
 * ------------------------------------------------------------------------- */

static void               mdb_callgate_detach_cb               (void *aptr);
static void               mdb_callgate_attach                  (DBusPendingCall *pc);
void                      mce_dbus_pending_call_blocks_suspend (DBusPendingCall *pc);

/* ------------------------------------------------------------------------- *
 * DEBUG_HELPERS
 * ------------------------------------------------------------------------- */

static dbus_bool_t        mce_dbus_message_repr_any            (FILE *file, DBusMessageIter *iter);
char                     *mce_dbus_message_iter_repr           (DBusMessageIter *iter);
char                     *mce_dbus_message_repr                (DBusMessage *const msg);

/* ------------------------------------------------------------------------- *
 * HANDLER_STRUCT_T
 * ------------------------------------------------------------------------- */

static inline void        handler_struct_set_sender            (handler_struct_t *self, const char *val);
static inline void        handler_struct_set_type              (handler_struct_t *self, int val);
static inline void        handler_struct_set_interface         (handler_struct_t *self, const char *val);
static inline void        handler_struct_set_args              (handler_struct_t *self, const char *val);
static inline void        handler_struct_set_name              (handler_struct_t *self, const char *val);
static inline void        handler_struct_set_rules             (handler_struct_t *self, const char *val);
static inline void        handler_struct_set_callback          (handler_struct_t *self, handler_callback_t val);
static inline void        handler_struct_set_privileged        (handler_struct_t *self, bool val);

static void               handler_struct_delete                (handler_struct_t *self);
static handler_struct_t  *handler_struct_create                (void);

/* ------------------------------------------------------------------------- *
 * PEERSTATE_T
 * ------------------------------------------------------------------------- */

const char              *peerstate_repr                        (peerstate_t state);

/* ------------------------------------------------------------------------- *
 * PEERQUIT_T
 * ------------------------------------------------------------------------- */

static peerquit_t       *peerquit_create                       (peerinfo_t *parent, peerquit_fn callback);
static void              peerquit_delete                       (peerquit_t *self);

static void              peerquit_notify                       (peerquit_t *self, DBusMessage *msg);

/* ------------------------------------------------------------------------- *
 * PEERNOTIFY_T
 * ------------------------------------------------------------------------- */

static gboolean          peernotify_idle_cb                    (gpointer aptr);
static void              peernotify_schedule                   (peernotify_t *self);
static void              peernotify_unschedule                 (peernotify_t *self);
static void              peernotify_execute                    (peernotify_t *self);
static peernotify_t     *peernotify_create                     (peerinfo_t *peerinfo, peernotify_fn callback, gpointer userdata, GDestroyNotify userfree);
static void              peernotify_delete                     (peernotify_t *self);

/* ------------------------------------------------------------------------- *
 * PEERINFO_T
 * ------------------------------------------------------------------------- */

static gchar            *peerinfo_guess_cmd                    (pid_t pid);

static inline void       peerinfo_ctor                         (peerinfo_t *self, const char *name);
static inline void       peerinfo_dtor                         (peerinfo_t *self);
peerinfo_t              *peerinfo_create                       (const char *name);
void                     peerinfo_delete                       (peerinfo_t *self);
void                     peerinfo_delete_cb                    (void *self);

static void              peerinfo_enter_state                  (peerinfo_t *self);
static void              peerinfo_leave_state                  (peerinfo_t *self);
static void              peerinfo_set_state                    (peerinfo_t *self, peerstate_t state);
peerstate_t              peerinfo_get_state                    (const peerinfo_t *self);

static const char       *peerinfo_repr                         (peerinfo_t *self);
const char              *peerinfo_name                         (const peerinfo_t *self);
const char              *peerinfo_get_owner_name               (const peerinfo_t *self);
static bool              peerinfo_set_owner_name               (peerinfo_t *self, const char *name);
pid_t                    peerinfo_get_proxy_pid                (const peerinfo_t *self);
static void              peerinfo_set_proxy_pid                (peerinfo_t *self, pid_t pid);
pid_t                    peerinfo_get_owner_pid                (const peerinfo_t *self);
static void              peerinfo_set_owner_pid                (peerinfo_t *self, pid_t pid);
uid_t                    peerinfo_get_owner_uid                (const peerinfo_t *self);
static void              peerinfo_set_owner_uid                (peerinfo_t *self, uid_t uid);
gid_t                    peerinfo_get_owner_gid                (const peerinfo_t *self);
static void              peerinfo_set_owner_gid                (peerinfo_t *self, gid_t gid);
const char              *peerinfo_get_owner_cmd                (const peerinfo_t *self);
static void              peerinfo_set_owner_cmd                (peerinfo_t *self, const char *cmd);
static privileged_t      peerinfo_get_privileged               (const peerinfo_t *self, bool no_caching);
static void              peerinfo_set_datapipe                 (peerinfo_t *self, datapipe_t *datapipe);

static void              peerinfo_query_owner_ign              (peerinfo_t *self);
static void              peerinfo_query_owner_rsp              (DBusPendingCall *pc, void *aptr);
static void              peerinfo_query_owner_req              (peerinfo_t *self);

static void              peerinfo_query_pid_ign                (peerinfo_t *self);
static void              peerinfo_query_pid_rsp                (DBusPendingCall *pc, void *aptr);
static void              peerinfo_query_pid_req                (peerinfo_t *self);

static void              peerinfo_identify_ign                 (peerinfo_t *self);
static void              peerinfo_identify_rsp                 (DBusPendingCall *pc, void *aptr);
static void              peerinfo_identify_req                 (peerinfo_t *self);

static void              peerinfo_query_delete_ign             (peerinfo_t *self);
static gboolean          peerinfo_query_delete_tmo             (gpointer aptr);
static void              peerinfo_query_delete_req             (peerinfo_t *self);

static peerquit_t       *peerinfo_add_quit_callback            (peerinfo_t *self, peerquit_fn callback);
static void              peerinfo_remove_quit_callback         (peerinfo_t *self, peerquit_t *quit);
static void              peerinfo_execute_quit_callbacks       (peerinfo_t *self);
static void              peerinfo_flush_quit_callbacks         (peerinfo_t *self);

static GList            *peerinfo_find_notify_slot             (peerinfo_t *self, peernotify_fn callback, gpointer userdata);
static void              peerinfo_flush_notify_callbacks       (peerinfo_t *self);
static void              peerinfo_execute_notify_callbacks     (peerinfo_t *self);
static void              peerinfo_add_notify_callback          (peerinfo_t *self, peernotify_fn callback, gpointer userdata, GDestroyNotify userfree);
static void              peerinfo_remove_notify_callback       (peerinfo_t *self, peernotify_fn callback, gpointer userdata);

static void              peerinfo_queue_method                 (peerinfo_t *self, DBusMessage *req);
static void              peerinfo_flush_methods                (peerinfo_t *self);
static void              peerinfo_handle_methods               (peerinfo_t *self);

/* ------------------------------------------------------------------------- *
 * PEER_TRACKING
 * ------------------------------------------------------------------------- */

static DBusHandlerResult mce_dbus_peerinfo_filter_cb           (DBusConnection *con, DBusMessage *msg, void *user_data);
static void              mce_dbus_init_peerinfo                (void);
static void              mce_dbus_quit_peerinfo                (void);
peerinfo_t              *mce_dbus_get_peerinfo                 (const char *name);
peerinfo_t              *mce_dbus_add_peerinfo                 (const char *name);
void                     mce_dbus_update_peerinfo              (const char *name, const char *owner);
void                     mce_dbus_del_peerinfo                 (const char *name);
const char              *mce_dbus_get_peerdesc                 (const char *name);

/* ------------------------------------------------------------------------- *
 * MESSAGE_SENDING
 * ------------------------------------------------------------------------- */

DBusConnection          *dbus_connection_get                   (void);
DBusMessage             *dbus_new_signal                       (const gchar *const path, const gchar *const interface, const gchar *const name);
static DBusMessage      *dbus_new_error                        (DBusMessage *req, const char *err, const char *fmt, ...);
DBusMessage             *dbus_new_method_call                  (const gchar *const service, const gchar *const path, const gchar *const interface, const gchar *const name);
DBusMessage             *dbus_new_method_reply                 (DBusMessage *const message);
gboolean                 dbus_send_message                     (DBusMessage *const msg);
static gboolean          dbus_send_message_with_reply_handler  (DBusMessage *const msg, DBusPendingCallNotifyFunction callback, int timeout, void *user_data, DBusFreeFunction user_free, DBusPendingCall **ppc);
static gboolean          dbus_send_va                          (const char *service, const char *path, const char *interface, const char *name, DBusPendingCallNotifyFunction callback, int timeout, void *user_data, DBusFreeFunction user_free, DBusPendingCall **ppc, int first_arg_type, va_list va);
gboolean                 dbus_send_ex                          (const char *service, const char *path, const char *interface, const char *name, DBusPendingCallNotifyFunction callback, void *user_data, DBusFreeFunction user_free, DBusPendingCall **ppc, int first_arg_type, ...);
gboolean                 dbus_send_ex2                         (const char *service, const char *path, const char *interface, const char *name, DBusPendingCallNotifyFunction callback, int timeout, void *user_data, DBusFreeFunction user_free, DBusPendingCall **ppc, int first_arg_type, ...);
gboolean                 dbus_send                             (const gchar *const service, const gchar *const path, const gchar *const interface, const gchar *const name, DBusPendingCallNotifyFunction callback, int first_arg_type, ...);

/* ------------------------------------------------------------------------- *
 * METHOD_CALL_HANDLERS
 * ------------------------------------------------------------------------- */

static gboolean          version_get_dbus_cb                   (DBusMessage *const msg);
static gboolean          suspend_stats_get_dbus_cb             (DBusMessage *const req);
static gboolean          verbosity_get_dbus_cb                 (DBusMessage *const req);
static gboolean          config_get_dbus_cb                    (DBusMessage *const msg);
static gboolean          verbosity_set_dbus_cb                 (DBusMessage *const req);
static gboolean          config_get_all_dbus_cb                (DBusMessage *const req);
static gboolean          config_reset_dbus_cb                  (DBusMessage *const msg);
static gboolean          config_set_dbus_cb                    (DBusMessage *const msg);
static gboolean          introspect_dbus_cb                    (DBusMessage *const req);

/* ------------------------------------------------------------------------- *
 * CONFIG_VALUES
 * ------------------------------------------------------------------------- */

static const char      **string_array_from_gconf_value         (GConfValue *conf, int *pcount);
static dbus_int32_t     *int_array_from_gconf_value            (GConfValue *conf, int *pcount);
static dbus_bool_t      *bool_array_from_gconf_value           (GConfValue *conf, int *pcount);
static double           *float_array_from_gconf_value          (GConfValue *conf, int *pcount);
static const char       *type_signature                        (GConfValueType type);
static const char       *value_signature                       (GConfValue *conf);
static bool              append_gconf_value_to_dbus_iterator   (DBusMessageIter *iter, GConfValue *conf);
static bool              append_gconf_entry_to_dbus_iterator   (DBusMessageIter *iter, GConfEntry *entry);
static bool              append_gconf_entries_to_dbus_iterator (DBusMessageIter *iter, GSList *entries);
static bool              append_gconf_value_to_dbus_message    (DBusMessage *reply, GConfValue *conf);
void                     mce_dbus_send_config_notification     (GConfEntry *entry);
static void              value_list_free                       (GSList *list);
static GSList           *value_list_from_string_array          (DBusMessageIter *iter);
static GSList           *value_list_from_int_array             (DBusMessageIter *iter);
static GSList           *value_list_from_bool_array            (DBusMessageIter *iter);
static GSList           *value_list_from_float_array           (DBusMessageIter *iter);

/* ------------------------------------------------------------------------- *
 * MESSAGE_DISPATCH
 * ------------------------------------------------------------------------- */

static gboolean          check_rules                           (DBusMessage *const msg, const char *rules);
static gchar            *mce_dbus_build_signal_match           (const gchar *sender, const gchar *interface, const gchar *name, const gchar *rules);
static void              mce_dbus_squeeze_slist                (GSList **list);
static bool              mce_dbus_match                        (const char *msg_val, const char *hnd_val);
static DBusHandlerResult msg_handler                           (DBusConnection *const connection, DBusMessage *const msg, gpointer const user_data);
static gconstpointer     mce_dbus_handler_add_ex               (const gchar *const sender, const gchar *const interface, const gchar *const name, const gchar *const args, const gchar *const rules, const guint type, gboolean (*callback)(DBusMessage *const msg), bool privileged);
static void              mce_dbus_handler_remove               (gconstpointer cookie);
static void              mce_dbus_handler_remove_cb            (gpointer handler, gpointer user_data);
void                     mce_dbus_handler_register             (mce_dbus_handler_t *self);
void                     mce_dbus_handler_unregister           (mce_dbus_handler_t *self);
void                     mce_dbus_handler_register_array       (mce_dbus_handler_t *array);
void                     mce_dbus_handler_unregister_array     (mce_dbus_handler_t *array);

/* ------------------------------------------------------------------------- *
 * OWNER_MONITORING
 * ------------------------------------------------------------------------- */

static peerquit_t       *find_monitored_service                (const gchar *service, GSList *monitor_list);
gssize                   mce_dbus_owner_monitor_add            (const gchar *service, gboolean (*callback)(DBusMessage *const msg), GSList **monitor_list, gssize max_num);
gssize                   mce_dbus_owner_monitor_remove         (const gchar *service, GSList **monitor_list);
void                     mce_dbus_owner_monitor_remove_all     (GSList **monitor_list);

/* ------------------------------------------------------------------------- *
 * SERVICE_MANAGEMENT
 * ------------------------------------------------------------------------- */

static gboolean          dbus_acquire_services                 (void);
static void              dbus_quit_message_handler             (void);
static gboolean          dbus_init_message_handler             (void);

/* ------------------------------------------------------------------------- *
 * MESSAGE_ITER
 * ------------------------------------------------------------------------- */

const char              *mce_dbus_type_repr                    (int type);
bool                     mce_dbus_iter_at_end                  (DBusMessageIter *iter);
bool                     mce_dbus_iter_req_type                (DBusMessageIter *iter, int want);
bool                     mce_dbus_iter_get_basic               (DBusMessageIter *iter, void *pval, int type);
bool                     mce_dbus_iter_get_object              (DBusMessageIter *iter, const char **pval);
bool                     mce_dbus_iter_get_string              (DBusMessageIter *iter, const char **pval);
bool                     mce_dbus_iter_get_bool                (DBusMessageIter *iter, bool *pval);
bool                     mce_dbus_iter_get_int32               (DBusMessageIter *iter, dbus_int32_t *pval);
bool                     mce_dbus_iter_get_uint32              (DBusMessageIter *iter, dbus_uint32_t *pval);
static bool              mce_dbus_iter_get_container           (DBusMessageIter *iter, DBusMessageIter *sub, int type);
bool                     mce_dbus_iter_get_array               (DBusMessageIter *iter, DBusMessageIter *sub);
bool                     mce_dbus_iter_get_struct              (DBusMessageIter *iter, DBusMessageIter *sub);
bool                     mce_dbus_iter_get_entry               (DBusMessageIter *iter, DBusMessageIter *sub);
bool                     mce_dbus_iter_get_variant             (DBusMessageIter *iter, DBusMessageIter *sub);

/* ------------------------------------------------------------------------- *
 * PEER_IDENTITY
 * ------------------------------------------------------------------------- */

const char              *mce_dbus_get_name_owner_ident         (const char *name);
const char              *mce_dbus_get_message_sender_ident     (DBusMessage *msg);

/* ------------------------------------------------------------------------- *
 * INTROSPECT_SUPPORT
 * ------------------------------------------------------------------------- */

static void              introspect_add_methods                (FILE *file, const char *interface);
static void              introspect_add_signals                (FILE *file, const char *interface);
static void              introspect_add_defaults               (FILE *file);
static void              introspect_com_nokia_mce_request      (FILE *file);
static void              introspect_com_nokia_mce_signal       (FILE *file);
static void              introspect_com_nokia_mce              (FILE *file);
static void              introspect_com_nokia                  (FILE *file);
static void              introspect_com                        (FILE *file);
static void              introspect_root                       (FILE *file);
static bool              introspectable_signal                 (const char *interface, const char *member);

/* ------------------------------------------------------------------------- *
 * DBUS_NAME_OWNER_TRACKING
 * ------------------------------------------------------------------------- */

const char              *mce_dbus_nameowner_get                (const char *name);
static char             *mce_dbus_nameowner_watch              (const char *name);
static void              mce_dbus_nameowner_unwatch            (char *rule);

/* ------------------------------------------------------------------------- *
 * MODULE_INIT_QUIT
 * ------------------------------------------------------------------------- */

DBusConnection          *dbus_bus_get                          (DBusBusType type, DBusError *err);
DBusConnection          *dbus_bus_get_private                  (DBusBusType type, DBusError *err);
static void              mce_dbus_init_privileged_uid          (void);
static void              mce_dbus_init_privileged_gid          (void);
gboolean                 mce_dbus_init                         (const gboolean systembus);
void                     mce_dbus_exit                         (void);

/* ========================================================================= *
 * DATA
 * ========================================================================= */

/** Pointer to the DBusConnection */
static DBusConnection *dbus_connection = NULL;

/** List of all D-Bus handlers */
static GSList *dbus_handlers = NULL; // -> handler_struct_t *

/** Cached UID for "privileged" user; assume root only */
static uid_t mce_dbus_privileged_uid = PEERINFO_ROOT_UID;

/** Cached GID for "privileged" group; assume root only */
static gid_t mce_dbus_privileged_gid = PEERINFO_ROOT_GID;

/* ========================================================================= *
 * SUSPEND_PROOFING
 * ========================================================================= */

/** Pending call callgate slot destry callback function
 *
 * Called when pending call ref count drops to zero.
 *
 * Releases the ultiplexed wakelock that has been attached
 * to the pending call object via mdb_callgate_attach().
 *
 * @param aptr Name of the attached wakelock
 */
static void mdb_callgate_detach_cb(void  *aptr)
{
	char *name = aptr;

	mce_log(LL_DEBUG, "detach %s", name);
	mce_wakelock_release(name);
	g_free(name);
}

/** Block suspend while mce is waiting for a reply to a method call
 *
 * Take advantage of the fact that custom data attached to a pending
 * call object gets destroyed along with the object itself and create
 * unique wakelock that is used for blocking the device from entering
 * suspend until:
 *
 * a) the wait for pending call is canceled
 * b) mce has received and processed the reply message
 *
 * @param pc Pending call object to suspend proof
 */
static void mdb_callgate_attach(DBusPendingCall *pc)
{
	static dbus_int32_t  slot = -1;
	static unsigned      uniq = 0;

	gchar               *name = 0;

	if( !pc )
		goto EXIT;

	if( slot == -1 && !dbus_pending_call_allocate_data_slot(&slot) )
		goto EXIT;

	if( !(name = g_strdup_printf("dbus_call_%u", ++uniq)) )
		goto EXIT;

	mce_log(LL_DEBUG, "attach %s", name);

	if( dbus_pending_call_set_data(pc, slot, name, mdb_callgate_detach_cb) ) {
		mce_wakelock_obtain(name, -1);
		name = 0;
	}

EXIT:
	g_free(name);
}

/** Public function for making dbus method calls suspend proof
 *
 * NOTE: We would not need this function if all of the mce code base
 *       would use dbus_send_ex() based method call handling.
 *
 * FIXME: Fix all code that uses dbus_connection_send_with_reply()
 *        so that dbus_send_ex() is used instead.
 *
 * @param pc  Pending call object to protect
 */
void mce_dbus_pending_call_blocks_suspend(DBusPendingCall *pc)
{
	mdb_callgate_attach(pc);
}

/* ========================================================================= *
 * DEBUG_HELPERS
 * ========================================================================= */

/** Emit one iterm from dbus message iterator to file
 *
 * @param file output file
 * @param iter dbus message parse position
 *
 * @return TRUE if more items can be parsed, FALSE otherwise
 */
static dbus_bool_t
mce_dbus_message_repr_any(FILE *file, DBusMessageIter *iter)
{
	dbus_any_t val = { .u64 = 0 };
	DBusMessageIter sub;

	switch( dbus_message_iter_get_arg_type(iter) ) {
	case DBUS_TYPE_INVALID:
		return FALSE;
	default:
	case DBUS_TYPE_UNIX_FD:
		fprintf(file, " ???");
		return FALSE;
	case DBUS_TYPE_BYTE:
		dbus_message_iter_get_basic(iter, &val.o);
		fprintf(file, " byte:%d", val.o);
		break;
	case DBUS_TYPE_BOOLEAN:
		dbus_message_iter_get_basic(iter, &val.b);
		fprintf(file, " bool:%d", val.b);
		break;
	case DBUS_TYPE_INT16:
		dbus_message_iter_get_basic(iter, &val.i16);
		fprintf(file, " i16:%d", val.i16);
		break;
	case DBUS_TYPE_INT32:
		dbus_message_iter_get_basic(iter, &val.i32);
		fprintf(file, " i32:%d", val.i32);
		break;
	case DBUS_TYPE_INT64:
		dbus_message_iter_get_basic(iter, &val.i64);
		fprintf(file, " i64:%lld", (long long)val.i64);
		break;
	case DBUS_TYPE_UINT16:
		dbus_message_iter_get_basic(iter, &val.u16);
		fprintf(file, " u16:%u", val.u16);
		break;
	case DBUS_TYPE_UINT32:
		dbus_message_iter_get_basic(iter, &val.u32);
		fprintf(file, " u32:%u", val.u32);
		break;
	case DBUS_TYPE_UINT64:
		dbus_message_iter_get_basic(iter, &val.u64);
		fprintf(file, " u64:%llu", (unsigned long long)val.u64);
		break;
	case DBUS_TYPE_DOUBLE:
		dbus_message_iter_get_basic(iter, &val.d);
		fprintf(file, " dbl:%g", val.d);
		break;
	case DBUS_TYPE_STRING:
		dbus_message_iter_get_basic(iter, &val.s);
		fprintf(file, " str:\"%s\"", val.s);
		break;
	case DBUS_TYPE_OBJECT_PATH:
		dbus_message_iter_get_basic(iter, &val.s);
		fprintf(file, " obj:\"%s\"", val.s);
		break;
	case DBUS_TYPE_SIGNATURE:
		dbus_message_iter_get_basic(iter, &val.s);
		fprintf(file, " sgn:\"%s\"", val.s);
		break;
	case DBUS_TYPE_ARRAY:
		dbus_message_iter_recurse(iter, &sub);
		fprintf(file, " [");
		while( mce_dbus_message_repr_any(file, &sub) ) {}
		fprintf(file, " ]");
		break;
	case DBUS_TYPE_VARIANT:
		dbus_message_iter_recurse(iter, &sub);
		fprintf(file, " var");
		mce_dbus_message_repr_any(file, &sub);
		break;
	case DBUS_TYPE_STRUCT:
		dbus_message_iter_recurse(iter, &sub);
		fprintf(file, " {");
		while( mce_dbus_message_repr_any(file, &sub) ) {}
		fprintf(file, " }");
		break;
	case DBUS_TYPE_DICT_ENTRY:
		dbus_message_iter_recurse(iter, &sub);
		fprintf(file, " key");
		mce_dbus_message_repr_any(file, &sub);
		fprintf(file, " val");
		mce_dbus_message_repr_any(file, &sub);
		break;
	}

	return dbus_message_iter_next(iter);
}

/** Convert dbus message read iterator to string
 *
 * Caller must release returned string with free().
 *
 * @param iter dbus message iterator
 *
 * @returns representation of the iterator, or NULL
 */
char *
mce_dbus_message_iter_repr(DBusMessageIter *iter)
{
	size_t  size = 0;
	char   *data = 0;
	FILE   *file = open_memstream(&data, &size);

	if( !iter )
		goto EXIT;

	while( mce_dbus_message_repr_any(file, iter) ) {}
EXIT:
	fclose(file);
	return data;
}

/** Convert dbus message to string
 *
 * Caller must release returned string with free().
 *
 * @param msg dbus message
 *
 * @returns representation of the dbus message, or NULL
 */
char *
mce_dbus_message_repr(DBusMessage *const msg)
{
	size_t  size = 0;
	char   *data = 0;
	FILE   *file = open_memstream(&data, &size);

	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);
	int type = dbus_message_get_type(msg);
	const char *tname = dbus_message_type_to_string(type);
	const char *sender = dbus_message_get_sender(msg);

	fprintf(file, "%s", tname);

	if( sender ) fprintf(file, " from %s", sender);
	if( iface )  fprintf(file, " %s", iface);
	if( member ) fprintf(file, " %s", member);

	DBusMessageIter iter;
	dbus_message_iter_init(msg, &iter);
	if( dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INVALID )
		goto EXIT;

	fprintf(file, ":");

	while( mce_dbus_message_repr_any(file, &iter) ) {}
EXIT:
	fclose(file);
	return data;
}

/* ========================================================================= *
 * HANDLER_STRUCT_T
 * ========================================================================= */

/** Set sender for D-Bus handler structure */
static inline void handler_struct_set_sender(handler_struct_t *self, const char *val)
{
	g_free(self->sender), self->sender = val ? g_strdup(val) : 0;
}

/** Set handler type for D-Bus handler structure */
static inline void handler_struct_set_type(handler_struct_t *self, int val)
{
	self->type = val;
}

/** Set interface name for D-Bus handler structure */
static inline void handler_struct_set_interface(handler_struct_t *self, const char *val)
{
	g_free(self->interface), self->interface = val ? g_strdup(val) : 0;
}

/** Set introspect args for D-Bus handler structure */
static inline void handler_struct_set_args(handler_struct_t *self, const char *val)
{
	g_free(self->args), self->args = val ? g_strdup(val) : 0;
}

/** Set member name for D-Bus handler structure */
static inline void handler_struct_set_name(handler_struct_t *self, const char *val)
{
	g_free(self->name), self->name = val ? g_strdup(val) : 0;
}

/** Set custom rules for D-Bus handler structure */
static inline void handler_struct_set_rules(handler_struct_t *self, const char *val)
{
	g_free(self->rules), self->rules = val ? g_strdup(val) : 0;
}

/** Set callback function for D-Bus handler structure */
static inline void handler_struct_set_callback(handler_struct_t *self,
					       handler_callback_t val)
{
	self->callback = val;
}

/** Set privileged requirement for D-Bus handler structure */
static inline void handler_struct_set_privileged(handler_struct_t *self, bool val)
{
	self->privileged = val;
}

/** Release D-Bus handler structure */
static void handler_struct_delete(handler_struct_t *self)
{
	if( !self )
		goto EXIT;

	g_free(self->args);
	g_free(self->name);
	g_free(self->rules);
	g_free(self->interface);
	g_free(self->sender);
	g_free(self);

EXIT:
	return;
}

/** Allocate D-Bus handler structure */
static handler_struct_t *handler_struct_create(void)
{
	handler_struct_t *self = g_malloc0(sizeof *self);

	self->sender     = 0;
	self->interface  = 0;
	self->rules      = 0;
	self->name       = 0;
	self->args       = 0;
	self->type       = DBUS_MESSAGE_TYPE_INVALID;
	self->privileged = false;

	return self;
}

/* ========================================================================= *
 * PEERSTATE_T
 * ========================================================================= */

const char *
peerstate_repr(peerstate_t state)
{
    const char *repr = "PEERSTATE_INVALID";

    switch( state ) {
    case PEERSTATE_INITIAL:       repr = "PEERSTATE_INITIAL";     break;
    case PEERSTATE_QUERY_OWNER:   repr = "PEERSTATE_QUERY_OWNER"; break;
    case PEERSTATE_QUERY_PID:     repr = "PEERSTATE_QUERY_PID";   break;
    case PEERSTATE_IDENTIFY:      repr = "PEERSTATE_IDENTIFY";    break;
    case PEERSTATE_RUNNING:       repr = "PEERSTATE_RUNNING";     break;
    case PEERSTATE_STOPPED:       repr = "PEERSTATE_STOPPED";     break;
    default: break;
    }

    return repr;
}

/* ========================================================================= *
 * PEERQUIT_T
 * ========================================================================= */

static peerquit_t *
peerquit_create(peerinfo_t *parent, peerquit_fn callback)
{
    peerquit_t *self = g_malloc0(sizeof *self);

    self->pq_peerinfo = parent;
    self->pq_callback = callback;

    mce_log(LL_DEBUG, "create quit notify: %s -> %p",
	    peerinfo_name(self->pq_peerinfo),
	    self->pq_callback);

    return self;
}

static void
peerquit_delete(peerquit_t *self)
{
    if( self )
    {
	mce_log(LL_DEBUG, "delete quit notify: %s -> %p",
		peerinfo_name(self->pq_peerinfo),
		self->pq_callback);

	g_free(self);
    }
}

static void
peerquit_notify(peerquit_t *self, DBusMessage *msg)
{
    mce_log(LL_DEBUG, "execute quit notify: %s -> %p",
	    peerinfo_name(self->pq_peerinfo),
	    self->pq_callback);
    self->pq_callback(msg);
}

/* ========================================================================= *
 * PEERNOTIFY_T
 * ========================================================================= */

/** Callback for Notifying initial peer state
 *
 * @param aptr  peernotify_t object as void pointer
 *
 * @return FALSE to stop idle callback from being repeated
 */
static gboolean
peernotify_idle_cb(gpointer aptr)
{
    peernotify_t *self = aptr;

    if( self->pn_idle_id ) {
	self->pn_idle_id = 0;
	peernotify_execute(self);
    }

    return FALSE;
}

/** Schedule initial state notification for freshly added tracker
 *
 * Is called as a consequence of peerinfo_add_notify_callback() call.
 *
 * @param self  peernotify_t object
 */
static void
peernotify_schedule(peernotify_t *self)
{
    if( !self->pn_idle_id ) {
	self->pn_idle_id  = g_idle_add(peernotify_idle_cb, self);
    }
}

/** Cancel initial state notification for freshly added tracker
 *
 * Is called a part of notify object cleanup / if the tracking
 * state change notification gets emitted before idle callback
 * gets a chance to get dispatched.
 *
 * @param self  peernotify_t object
 */
static void
peernotify_unschedule(peernotify_t *self)
{
    if( self->pn_idle_id ) {
	g_source_remove(self->pn_idle_id), self->pn_idle_id = 0;
    }
}

/** Notify peer tracking state change
 *
 * @param self  peernotify_t object
 */
static void
peernotify_execute(peernotify_t *self)
{
    peernotify_unschedule(self);

    if( self->pn_callback ) {
	self->pn_callback(self->pn_peerinfo, self->pn_userdata);
    }
}

/** Create peer state tracking object
 *
 * @param peerinfo  D-Bus client being tracked
 * @param callback  Notification function to call on state change
 * @param userdata  A pointer that should be passed to notification callback
 * @param userfree  free() like function to call on userdata on cleanup
 *
 * @param self  peernotify_t object
 */
static peernotify_t *
peernotify_create(peerinfo_t       *peerinfo,
		  peernotify_fn     callback,
		  gpointer          userdata,
		  GDestroyNotify    userfree)
{
    peernotify_t *self = calloc(1, sizeof *self);

    self->pn_peerinfo = peerinfo;
    self->pn_callback = callback;
    self->pn_userdata = userdata;
    self->pn_userfree = userfree;
    self->pn_idle_id  = 0;

    peernotify_schedule(self);

    return self;
}

/** Delete peer state tracking object
 *
 * @param self  peernotify_t object, or NULL
 */
static void
peernotify_delete(peernotify_t *self)
{
    if( self != 0 )
    {
	peernotify_unschedule(self);

	if( self->pn_userdata && self->pn_userfree )
	    self->pn_userfree(self->pn_userdata);

	self->pn_peerinfo = 0;
	self->pn_callback = 0;
	self->pn_userdata = 0;
	self->pn_userfree = 0;

	free(self);
    }
}

/* ========================================================================= *
 * PEERINFO_T
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * misc helpers
 * ------------------------------------------------------------------------- */

static gchar *
peerinfo_guess_cmd(pid_t pid)
{
    gchar *cmd  = 0;
    int    file = -1;

    char path[256];
    unsigned char text[64];

    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    if( (file = open(path, O_RDONLY)) == -1 ) {
	mce_log(LL_ERR, "%s: open: %m", path);
	goto EXIT;
    }

    int n = read(file, text, sizeof text - 1);
    if( n == -1 ) {
	mce_log(LL_ERR, "%s: read: %m", path);
	goto EXIT;
    }
    text[n] = 0;

    for( int i = 0; i < n; ++i ) {
	if( text[i] < 32 )
	    text[i] = ' ';
    }

    cmd = g_strdup((char *)text);

EXIT:
    if( file != -1 ) close(file);

    return cmd;
}

/* ------------------------------------------------------------------------- *
 * create / delete
 * ------------------------------------------------------------------------- */

static inline void
peerinfo_ctor(peerinfo_t *self, const char *name)
{
    self->pi_state         = PEERSTATE_INITIAL;
    self->pi_name          = g_strdup(name);
    self->pi_owner_name    = 0;
    self->pi_proxy_pid     = PEERINFO_NO_PID;
    self->pi_owner_pid     = PEERINFO_NO_PID;
    self->pi_owner_uid     = PEERINFO_NO_UID;
    self->pi_owner_gid     = PEERINFO_NO_GID;
    self->pi_owner_cmd     = 0;
    self->pi_datapipe      = 0;
    self->pi_name_owner_pc = 0;
    self->pi_name_pid_pc   = 0;
    self->pi_identify_pc   = 0;
    self->pi_delete_id     = 0;

    g_queue_init(&self->pi_quit_callbacks);
    g_queue_init(&self->pi_notifications);
    g_queue_init(&self->pi_priv_methods);

    mce_log(LL_DEBUG, "[%s] create", peerinfo_name(self));

    self->pi_rule = mce_dbus_nameowner_watch(peerinfo_name(self));

    peerinfo_set_state(self, PEERSTATE_QUERY_OWNER);
}

static inline void
peerinfo_dtor(peerinfo_t *self)
{
    mce_log(LL_DEBUG, "[%s] delete", peerinfo_name(self));

    mce_dbus_nameowner_unwatch(self->pi_rule),
	self->pi_rule = 0;

    peerinfo_flush_quit_callbacks(self);
    peerinfo_flush_notify_callbacks(self);
    peerinfo_flush_methods(self);

    peerinfo_set_owner_name(self, 0);
    peerinfo_set_proxy_pid(self, PEERINFO_NO_PID);
    peerinfo_set_owner_pid(self, PEERINFO_NO_PID);
    peerinfo_set_owner_uid(self, PEERINFO_NO_UID);
    peerinfo_set_owner_gid(self, PEERINFO_NO_GID);

    peerinfo_query_owner_ign(self);
    peerinfo_query_pid_ign(self);
    peerinfo_identify_ign(self);
    peerinfo_query_delete_ign(self);

    g_free(self->pi_name),
	self->pi_name = 0;
}

peerinfo_t *
peerinfo_create(const char *name)
{
    peerinfo_t *self = g_malloc0(sizeof *self);
    peerinfo_ctor(self, name);
    return self;
}

void
peerinfo_delete(peerinfo_t *self)
{
    if( self != 0 )
    {
	peerinfo_dtor(self);
	g_free(self);
    }
}

void
peerinfo_delete_cb(void *self)
{
    peerinfo_delete(self);
}

/* ------------------------------------------------------------------------- *
 * state management
 * ------------------------------------------------------------------------- */

static void
peerinfo_enter_state(peerinfo_t *self)
{
    switch( self->pi_state ) {
    case PEERSTATE_INITIAL:
	break;

    case PEERSTATE_QUERY_OWNER:
	if( *peerinfo_name(self) == ':' ) {
	    peerinfo_set_owner_name(self, peerinfo_name(self));
	    peerinfo_set_state(self, PEERSTATE_QUERY_PID);
	}
	else {
	    peerinfo_set_owner_name(self, 0);
	    peerinfo_query_owner_req(self);
	}
	break;

    case PEERSTATE_QUERY_PID:
	peerinfo_set_proxy_pid(self, PEERINFO_NO_PID);
	peerinfo_set_owner_pid(self, PEERINFO_NO_PID);
	peerinfo_set_owner_uid(self, PEERINFO_NO_UID);
	peerinfo_set_owner_gid(self, PEERINFO_NO_GID);
	peerinfo_query_pid_req(self);
	break;

    case PEERSTATE_IDENTIFY:
	peerinfo_identify_req(self);
	break;

    case PEERSTATE_RUNNING:
	/* Make sure any previously logged ipc without process
	 * details can be mapped to something useful when
	 * debugging -> emit details now that we have them. */
	mce_log(LL_DEVEL, "%s", peerinfo_repr(self));

	if( self->pi_datapipe ) {
	    datapipe_exec_full(self->pi_datapipe,
			       GINT_TO_POINTER(SERVICE_STATE_RUNNING));
	}
	peerinfo_handle_methods(self);
	break;

    case PEERSTATE_STOPPED:
	if( self->pi_datapipe ) {
	    datapipe_exec_full(self->pi_datapipe,
			       GINT_TO_POINTER(SERVICE_STATE_STOPPED));
	}
	peerinfo_flush_methods(self);
	peerinfo_execute_quit_callbacks(self);

	/* Private names do not come back, delete from cache */
	if( *peerinfo_name(self) == ':' )
	    peerinfo_query_delete_req(self);
	break;

    default:
	mce_abort();
    }
}

static void
peerinfo_leave_state(peerinfo_t *self)
{
    switch( self->pi_state ) {
    case PEERSTATE_INITIAL:
	break;

    case PEERSTATE_QUERY_OWNER:
	peerinfo_query_owner_ign(self);
	break;

    case PEERSTATE_QUERY_PID:
	peerinfo_query_pid_ign(self);
	break;

    case PEERSTATE_IDENTIFY:
	peerinfo_identify_ign(self);
	break;

    case PEERSTATE_RUNNING:
	break;

    case PEERSTATE_STOPPED:
	peerinfo_query_delete_ign(self);
	break;

    default:
	mce_abort();
    }
}

static void
peerinfo_set_state(peerinfo_t *self, peerstate_t state)
{
    if( self->pi_state == state )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] state: %s -> %s",
	    peerinfo_name(self),
	    peerstate_repr(self->pi_state),
	    peerstate_repr(state));

    peerinfo_leave_state(self);
    self->pi_state = state;
    peerinfo_enter_state(self);

    peerinfo_execute_notify_callbacks(self);

EXIT:
    return;
}

peerstate_t
peerinfo_get_state(const peerinfo_t *self)
{
    return self->pi_state;
}

/* ------------------------------------------------------------------------- *
 * property accessors
 * ------------------------------------------------------------------------- */

static const char *
peerinfo_repr(peerinfo_t *self)
{
    const char *desc = 0;

    if( !self )
	goto EXIT;

    snprintf(self->pi_repr, sizeof self->pi_repr,
	     "name=%s owner=%s pid=%d uid=%d gid=%d priv=%d cmd=%s",
	     peerinfo_name (self),
	     peerinfo_get_owner_name(self) ?: "NULL",
	     (int)peerinfo_get_owner_pid(self),
	     (int)peerinfo_get_owner_uid(self),
	     (int)peerinfo_get_owner_gid(self),
	     peerinfo_get_privileged(self, false),
	     peerinfo_get_owner_cmd(self) ?: "NULL");

    desc = self->pi_repr;

EXIT:
    return desc;
}

const char *
peerinfo_name(const peerinfo_t *self)
{
    return self->pi_name;
}

const char *
peerinfo_get_owner_name(const peerinfo_t *self)
{
    return self->pi_owner_name;
}

static bool
peerinfo_set_owner_name(peerinfo_t *self, const char *name)
{
    bool changed = false;

    if( name && !*name )
	name = 0;

    if( !g_strcmp0(self->pi_owner_name, name) )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] owner name: %s -> %s",
	    peerinfo_name(self),
	    self->pi_owner_name ?: "NULL",
	    name          ?: "NULL");

    g_free(self->pi_owner_name),
	self->pi_owner_name = name ? g_strdup(name) : 0;

    changed = true;

EXIT:
    return changed;
}

pid_t
peerinfo_get_proxy_pid(const peerinfo_t *self)
{
    return self->pi_proxy_pid;
}

static void
peerinfo_set_proxy_pid(peerinfo_t *self, pid_t pid)
{
    if( self->pi_proxy_pid == pid )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] proxy pid: %d -> %d",
	    peerinfo_name(self),
	    (int)self->pi_proxy_pid,
	    (int)pid);

    self->pi_proxy_pid = pid;

EXIT:
    return;
}

pid_t
peerinfo_get_owner_pid(const peerinfo_t *self)
{
    return self->pi_owner_pid;
}

static void
peerinfo_set_owner_pid(peerinfo_t *self, pid_t pid)
{
    if( self->pi_owner_pid == pid )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] owner pid: %d -> %d",
	    peerinfo_name(self),
	    (int)self->pi_owner_pid,
	    (int)pid);

    self->pi_owner_pid = pid;

    gchar *cmd = 0;
    if( pid != PEERINFO_NO_PID )
	cmd = peerinfo_guess_cmd(pid);
    peerinfo_set_owner_cmd(self, cmd);
    g_free(cmd);

    if( pid != PEERINFO_NO_PID ) {
	char path[256];
	snprintf(path, sizeof path, "/proc/%d", (int)pid);

	struct stat st;
	memset(&st, 0, sizeof st);
	if( stat(path, &st) == 0 ) {
	    peerinfo_set_owner_uid(self, st.st_uid);
	    peerinfo_set_owner_gid(self, st.st_gid);
	}
    }

EXIT:
    return;
}

uid_t
peerinfo_get_owner_uid(const peerinfo_t *self)
{
    return self->pi_owner_uid;
}

static void
peerinfo_set_owner_uid(peerinfo_t *self, uid_t uid)
{
    if( self->pi_owner_uid == uid )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] owner uid: %d -> %d",
	    peerinfo_name(self),
	    (int)self->pi_owner_uid,
	    (int)uid);

    self->pi_owner_uid = uid;

EXIT:
    return;
}

gid_t
peerinfo_get_owner_gid(const peerinfo_t *self)
{
    return self->pi_owner_gid;
}

static void
peerinfo_set_owner_gid(peerinfo_t *self, gid_t gid)
{
    if( self->pi_owner_gid == gid )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] owner gid: %d -> %d",
	    peerinfo_name(self),
	    (int)self->pi_owner_gid,
	    (int)gid);

    self->pi_owner_gid = gid;

EXIT:
    return;
}

const char *
peerinfo_get_owner_cmd(const peerinfo_t *self)
{
    return self->pi_owner_cmd;
}

static void
peerinfo_set_owner_cmd(peerinfo_t *self, const char *cmd)
{
    if( !g_strcmp0(self->pi_owner_cmd, cmd) )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] owner cmd: %s -> %s",
	    peerinfo_name(self),
	    self->pi_owner_cmd ?: "NULL",
	    cmd                ?: "NULL");

    g_free(self->pi_owner_cmd),
	self->pi_owner_cmd = cmd ? g_strdup(cmd) : 0;

EXIT:
    return;
}

static privileged_t
peerinfo_get_privileged(const peerinfo_t *self, bool no_caching)
{
    privileged_t privileged = PRIVILEGED_NO;

    if( !self )
	goto EXIT;

    pid_t pid = peerinfo_get_owner_pid(self);
    if( pid == PEERINFO_NO_PID ) {
	privileged = PRIVILEGED_UNKNOWN;
	goto EXIT;
    }

    uid_t uid = PEERINFO_NO_UID;
    gid_t gid = PEERINFO_NO_GID;

    if( no_caching ) {

	/* The owner / group of /proc/PID directory reflects
	 * the current euid / egid of the process. */

	char path[256];
	struct stat st;

	memset(&st, 0, sizeof st);
	snprintf(path, sizeof path, "/proc/%d", (int)pid);

	if( stat(path, &st) == -1 )
	    goto EXIT;

	uid = st.st_uid;
	gid = st.st_gid;
    }
    else {
	uid = peerinfo_get_owner_uid(self);
	gid = peerinfo_get_owner_gid(self);
    }

    if( uid == PEERINFO_ROOT_UID ||
	uid == mce_dbus_privileged_uid ||
	gid == mce_dbus_privileged_gid ) {
	privileged = PRIVILEGED_YES;
    }

EXIT:
    return privileged;
}

static void
peerinfo_set_datapipe(peerinfo_t *self, datapipe_t *datapipe)
{
    mce_log(LL_DEBUG, "[%s] datapipe: %p -> %p",
	    peerinfo_name(self),
	    self->pi_datapipe,
	    datapipe);

    if( (self->pi_datapipe = datapipe) ) {
	/* Report immediately if the state is already known */
	switch( peerinfo_get_state(self) ) {
	case PEERSTATE_RUNNING:
	    datapipe_exec_full(self->pi_datapipe,
			       GINT_TO_POINTER(SERVICE_STATE_RUNNING));
	    break;
	case PEERSTATE_STOPPED:
	    datapipe_exec_full(self->pi_datapipe,
			       GINT_TO_POINTER(SERVICE_STATE_STOPPED));
	    break;
	default:
	    break;
	}
    }
}

/* ------------------------------------------------------------------------- *
 * async owner name query
 * ------------------------------------------------------------------------- */

static void
peerinfo_query_owner_ign(peerinfo_t *self)
{
    if( !self->pi_name_owner_pc )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] owner query canceled", peerinfo_name(self));

    dbus_pending_call_cancel(self->pi_name_owner_pc);
    dbus_pending_call_unref(self->pi_name_owner_pc),
	self->pi_name_owner_pc = 0;

EXIT:
    return;
}

static void
peerinfo_query_owner_rsp(DBusPendingCall *pc, void *aptr)
{
    peerinfo_t  *self  = aptr;
    const char  *owner = 0;
    DBusMessage *rsp   = 0;
    DBusError    err   = DBUS_ERROR_INIT;

    mce_log(LL_DEBUG, "[%s] owner query replied", peerinfo_name(self));

    if( self->pi_name_owner_pc != pc )
	goto EXIT;

    dbus_pending_call_unref(self->pi_name_owner_pc),
	self->pi_name_owner_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
	mce_log(LL_WARN, "null reply");
    }
    else if( dbus_set_error_from_message(&err, rsp) ) {
	if( strcmp(err.name, DBUS_ERROR_NAME_HAS_NO_OWNER) ) {
	    mce_log(LL_WARN, "error reply: %s: %s", err.name, err.message);
	}
    }
    else if( !dbus_message_get_args(rsp, &err,
				    DBUS_TYPE_STRING, &owner,
				    DBUS_TYPE_INVALID) ) {
	mce_log(LL_WARN, "parse error: %s: %s", err.name, err.message);
    }

    if( owner ) {
	peerinfo_set_owner_name(self, owner);
	peerinfo_set_state(self, PEERSTATE_QUERY_PID);
    }
    else {
	peerinfo_set_state(self, PEERSTATE_STOPPED);
    }

EXIT:
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

static void
peerinfo_query_owner_req(peerinfo_t *self)
{
    if( peerinfo_get_state(self) != PEERSTATE_QUERY_OWNER )
	goto EXIT;

    if( self->pi_name_owner_pc )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] owner query ...", peerinfo_name(self));

    const char *name = peerinfo_name(self);

    dbus_send_ex(DBUS_SERVICE_DBUS,
		 DBUS_PATH_DBUS,
		 DBUS_INTERFACE_DBUS,
		 "GetNameOwner",
		 peerinfo_query_owner_rsp,
		 self, 0,
		 &self->pi_name_owner_pc,
		 DBUS_TYPE_STRING, &name,
		 DBUS_TYPE_INVALID);

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * async owner pid query
 * ------------------------------------------------------------------------- */

static void
peerinfo_query_pid_ign(peerinfo_t *self)
{
    if( !self->pi_name_pid_pc )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] pid query canceled", peerinfo_name(self));

    dbus_pending_call_cancel(self->pi_name_pid_pc);
    dbus_pending_call_unref(self->pi_name_pid_pc),
	self->pi_name_pid_pc = 0;

EXIT:
    return;
}

static void
peerinfo_query_pid_rsp(DBusPendingCall *pc, void *aptr)
{
    peerinfo_t  *self  = aptr;
    dbus_uint32_t pid  = 0;
    DBusMessage *rsp   = 0;
    DBusError    err   = DBUS_ERROR_INIT;

    mce_log(LL_DEBUG, "[%s] pid query replied", peerinfo_name(self));

    if( self->pi_name_pid_pc != pc )
	goto EXIT;

    dbus_pending_call_unref(self->pi_name_pid_pc),
	self->pi_name_pid_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
	mce_log(LL_WARN, "null reply");
    }
    else if( dbus_set_error_from_message(&err, rsp) ) {
	mce_log(LL_WARN, "error reply: %s: %s", err.name, err.message);
    }
    else if( !dbus_message_get_args(rsp, &err,
			       DBUS_TYPE_UINT32, &pid,
			       DBUS_TYPE_INVALID) ) {
	mce_log(LL_WARN, "parse error: %s: %s", err.name, err.message);
    }

    if( pid ) {
	gchar *exe_path = g_strdup_printf("/proc/%d/exe", pid);
	gchar *exe_target = g_file_read_link(exe_path, NULL);
	if( !g_strcmp0(exe_target, "/usr/bin/xdg-dbus-proxy") ) {
	    peerinfo_set_proxy_pid(self, pid);
	    peerinfo_set_state(self, PEERSTATE_IDENTIFY);
	}
	else {
	    peerinfo_set_owner_pid(self, pid);
	    peerinfo_set_state(self, PEERSTATE_RUNNING);
	}
	g_free(exe_target);
	g_free(exe_path);
    }
    else {
	peerinfo_set_state(self, PEERSTATE_STOPPED);
    }

EXIT:
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

static void
peerinfo_query_pid_req(peerinfo_t *self)
{
    if( peerinfo_get_state(self) != PEERSTATE_QUERY_PID )
	goto EXIT;

    if( self->pi_name_pid_pc )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] pid query send", peerinfo_name(self));

    const char *name = peerinfo_name(self);

    dbus_send_ex(DBUS_SERVICE_DBUS,
		 DBUS_PATH_DBUS,
		 DBUS_INTERFACE_DBUS,
		 "GetConnectionUnixProcessID",
		 peerinfo_query_pid_rsp,
		 self, 0,
		 &self->pi_name_pid_pc,
		 DBUS_TYPE_STRING, &name,
		 DBUS_TYPE_INVALID);

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * async sandboxed client identify query
 * ------------------------------------------------------------------------- */

static void
peerinfo_identify_ign(peerinfo_t *self)
{
    if( !self->pi_identify_pc )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] identify query canceled", peerinfo_name(self));

    dbus_pending_call_cancel(self->pi_identify_pc);
    dbus_pending_call_unref(self->pi_identify_pc),
	self->pi_identify_pc = 0;

EXIT:
    return;
}

static void
peerinfo_identify_rsp(DBusPendingCall *pc, void *aptr)
{
    peerinfo_t  *self = aptr;
    DBusMessage *rsp  = 0;
    DBusError    err  = DBUS_ERROR_INIT;
    dbus_int32_t pid  = PEERINFO_NO_PID;

    mce_log(LL_DEBUG, "[%s] identify query replied", peerinfo_name(self));

    if( self->pi_identify_pc != pc )
	goto EXIT;

    dbus_pending_call_unref(self->pi_identify_pc),
	self->pi_identify_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
	mce_log(LL_WARN, "null reply");
    }
    else if( dbus_set_error_from_message(&err, rsp) ) {
	mce_log(LL_WARN, "error reply: %s: %s", err.name, err.message);
    }
    else {
	DBusMessageIter body_iter;
	dbus_message_iter_init(rsp, &body_iter);
	if( dbus_message_iter_get_arg_type(&body_iter) == DBUS_TYPE_ARRAY ) {
	    DBusMessageIter array_iter;
	    dbus_message_iter_recurse(&body_iter, &array_iter);
	    dbus_message_iter_next(&body_iter);
	    while( dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_DICT_ENTRY ) {
		DBusMessageIter dict_iter;
		dbus_message_iter_recurse(&array_iter, &dict_iter);
		dbus_message_iter_next(&array_iter);
		if( dbus_message_iter_get_arg_type(&dict_iter) != DBUS_TYPE_STRING )
		    continue;
		const char *key = NULL;
		dbus_message_iter_get_basic(&dict_iter, &key);
		dbus_message_iter_next(&dict_iter);
		if( dbus_message_iter_get_arg_type(&dict_iter) != DBUS_TYPE_VARIANT )
		    continue;
		DBusMessageIter variant_iter;
		dbus_message_iter_recurse(&dict_iter, &variant_iter);
		dbus_message_iter_next(&dict_iter);
		if( !g_strcmp0(key, "pid") ) {
		    if( dbus_message_iter_get_arg_type(&variant_iter) == DBUS_TYPE_INT32 )
			dbus_message_iter_get_basic(&variant_iter, &pid);
                    break;
		}
	    }
	}
    }

    /* In case of failure, fall back to using pid of proxy */
    if( pid == PEERINFO_NO_PID )
	pid = peerinfo_get_proxy_pid(self);

    peerinfo_set_owner_pid(self, pid);
    peerinfo_set_state(self, PEERSTATE_RUNNING);

EXIT:
    if( rsp )
	dbus_message_unref(rsp);
    dbus_error_free(&err);
}

static void
peerinfo_identify_req(peerinfo_t *self)
{
    if( peerinfo_get_state(self) != PEERSTATE_IDENTIFY )
	goto EXIT;

    if( self->pi_identify_pc )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] identify query send", peerinfo_name(self));

    const char *name = peerinfo_name(self);

    dbus_send_ex(name, "/", "org.sailfishos.sailjailed", "Identify",
		 peerinfo_identify_rsp,
		 self, 0,
		 &self->pi_identify_pc,
		 DBUS_TYPE_INVALID);

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * delete timer
 * ------------------------------------------------------------------------- */

static void
peerinfo_query_delete_ign(peerinfo_t *self)
{
    if( !self->pi_delete_id )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] delete timer canceled", peerinfo_name(self));

    g_source_remove(self->pi_delete_id),
	self->pi_delete_id = 0;

EXIT:
    return;
}

static gboolean
peerinfo_query_delete_tmo(gpointer aptr)
{
    peerinfo_t  *self  = aptr;

    mce_log(LL_DEBUG, "[%s] delete timer triggered", peerinfo_name(self));

    if( !self->pi_delete_id )
	goto EXIT;

    self->pi_delete_id = 0;

    /* Execute any quit callbacks that might have been queued
     * while peerinfo is already in PEERSTATE_STOPPED state.
     */
    peerinfo_execute_quit_callbacks(self);

    mce_dbus_del_peerinfo(peerinfo_name(self));

EXIT:
    return FALSE;
}

static void
peerinfo_query_delete_req(peerinfo_t *self)
{
    if( peerinfo_get_state(self) != PEERSTATE_STOPPED )
	goto EXIT;

    if( self->pi_delete_id )
	goto EXIT;

    mce_log(LL_DEBUG, "[%s] delete timer scheduled", peerinfo_name(self));

    self->pi_delete_id = g_timeout_add(500, peerinfo_query_delete_tmo, self);

EXIT:

    return;
}

/* ------------------------------------------------------------------------- *
 * quit notifications
 * ------------------------------------------------------------------------- */

static peerquit_t *
peerinfo_add_quit_callback(peerinfo_t *self, peerquit_fn callback)
{
    peerquit_t *quit = peerquit_create(self, callback);
    g_queue_push_tail(&self->pi_quit_callbacks, quit);
    return quit;
}

static void
peerinfo_remove_quit_callback(peerinfo_t *self, peerquit_t *quit)
{
    if( g_queue_remove(&self->pi_quit_callbacks, quit) )
	peerquit_delete(quit);
}

static void
peerinfo_execute_quit_callbacks(peerinfo_t *self)
{
    const char  *name  = peerinfo_name(self);
    const char  *owner = "";
    DBusMessage *faked = 0;

    peerquit_t *quit;

    mce_log(LL_DEBUG, "[%s] run quit notifications", peerinfo_name(self));

    while( (quit = g_queue_pop_head(&self->pi_quit_callbacks)) ) {
	if( !faked ) {
	    faked = dbus_message_new_signal("/org/freedesktop/DBus",
					    "org.freedesktop.DBus",
					    "NameOwnerChanged");
	    if( !faked )
		break;

	    dbus_message_append_args(faked,
				     DBUS_TYPE_STRING, &name,
				     DBUS_TYPE_STRING, &name,
				     DBUS_TYPE_STRING, &owner,
				     DBUS_TYPE_INVALID);
	}
	peerquit_notify(quit, faked);
	peerquit_delete(quit);
    }

    if( faked )
	dbus_message_unref(faked);
}

static void
peerinfo_flush_quit_callbacks(peerinfo_t *self)
{
    peerquit_t *quit;

    while( (quit = g_queue_pop_head(&self->pi_quit_callbacks)) )
	peerquit_delete(quit);
}

/* ------------------------------------------------------------------------- *
 * state change notifications
 * ------------------------------------------------------------------------- */

/** Find a peernotify_t slot related to a given notification callback
 *
 * Can be used both as a predicate for notification callback already
 * exist and accessing/removing the existing peernotify_t object.
 *
 * @param self      peerinfo_t object
 * @param callback  tracking state change notification callback
 * @param userdata  user data pointer that is passed to the callback
 *
 * @return GSlist entry that has notification data, or NULL if not present
 */
static GList *
peerinfo_find_notify_slot(peerinfo_t     *self,
			  peernotify_fn   callback,
			  gpointer        userdata)
{
    GList *slot = 0;

    for( GList *item = self->pi_notifications.head; item; item = item->next ) {
	peernotify_t *peernotify = item->data;
	if( !peernotify )
	    continue;
	if( peernotify->pn_callback != callback )
	    continue;
	if( peernotify->pn_userdata != userdata )
	    continue;
	slot = item;
	break;
    }

    return slot;
}

/** Remove all tracking state callbacks installed to peerinfo_t object
 *
 * @param self peerinfo_t object
 */
static void
peerinfo_flush_notify_callbacks(peerinfo_t *self)
{
    while( self->pi_notifications.head ) {
	peernotify_t *peernotify = g_queue_pop_head(&self->pi_notifications);
	peernotify_delete(peernotify);
    }
}

/** Execute all tracking state callbacks installed to peerinfo_t object
 *
 * @param self peerinfo_t object
 */
static void
peerinfo_execute_notify_callbacks(peerinfo_t *self)
{
    mce_log(LL_DEBUG, "[%s] run state notifications", peerinfo_name(self));

    bool flush = false;

    for( GList *item = self->pi_notifications.head; item; item = item->next ) {
	peernotify_t *peernotify = item->data;
	if( peernotify )
	    peernotify_execute(peernotify);

	/* Note: The callback might have removed itself */
	if( !item->data )
	    flush = true;
    }

    if( flush ) {
	g_queue_remove_all(&self->pi_notifications, 0);
    }
}

/** Add tracking state callback to peerinfo_t object
 *
 * @param self      peerinfo_t object
 * @param callback  Notification function to call on state change
 * @param userdata  A pointer that should be passed to notification callback
 * @param userfree  free() like function to call on userdata on cleanup
 */
static void
peerinfo_add_notify_callback(peerinfo_t     *self,
			     peernotify_fn   callback,
			     gpointer        userdata,
			     GDestroyNotify  userfree)
{
    GList *slot = peerinfo_find_notify_slot(self, callback, userdata);

    if( slot )
	goto EXIT;

    peernotify_t *peernotify = peernotify_create(self, callback,
						 userdata, userfree);

    g_queue_push_tail(&self->pi_notifications, peernotify);

EXIT:
    return;
}

/** Remove tracking state callback from peerinfo_t object
 *
 * @param self      peerinfo_t object
 * @param callback  Notification function to call on state change
 * @param userdata  A pointer that should be passed to notification callback
 */
static void
peerinfo_remove_notify_callback(peerinfo_t    *self,
				peernotify_fn  callback,
				gpointer       userdata)
{
    GList *slot = peerinfo_find_notify_slot(self, callback, userdata);

    if( !slot )
	goto EXIT;

    /* Note: To allow safe removal from notification callback,
     *       we just sever the item from queue without modifying
     *       the queue itself. The null links are left behind to
     *       be cleaned when peerinfo_execute_notify_callbacks()
     *       gets called the next time.
     */

    peernotify_t *peernotify = slot->data;
    slot->data = 0;
    peernotify_delete(peernotify);

EXIT:
    return;
}

/* ------------------------------------------------------------------------- *
 * waiting priviledged calls
 * ------------------------------------------------------------------------- */

static void
peerinfo_queue_method(peerinfo_t *self, DBusMessage *req)
{
    if( req ) {
	mce_log(LL_DEBUG, "queue %s call from %s",
		dbus_message_get_member(req),
		peerinfo_repr(self));
	g_queue_push_tail(&self->pi_priv_methods, dbus_message_ref(req));
    }
}

static void
peerinfo_flush_methods(peerinfo_t *self)
{
    DBusMessage *req;

    while( (req = g_queue_pop_head(&self->pi_priv_methods)) ) {
	mce_log(LL_WARN, "dropped %s call from %s",
		dbus_message_get_member(req),
		peerinfo_repr(self));
	dbus_message_unref(req);
    }
}

static void
peerinfo_handle_methods(peerinfo_t *self)
{
    DBusMessage *req;

    while( (req = g_queue_pop_head(&self->pi_priv_methods)) ) {
	mce_log(LL_DEBUG, "retry %s call from %s",
		dbus_message_get_member(req),
		peerinfo_repr(self));
	msg_handler(0, req, 0);
	dbus_message_unref(req);
    }
}

/* ========================================================================= *
 * PEER_TRACKING
 * ========================================================================= */

/** Lookup table of D-Bus names to watch */
static struct
{
    const char  *name;
    datapipe_t  *datapipe;

} mce_dbus_nameowner_lut[] =
{
    {
	.name     = DSME_DBUS_SERVICE,
	.datapipe = &dsme_service_state_pipe,
    },
    {
	.name     = thermalmanager_service,
	.datapipe = &thermalmanager_service_state_pipe,
    },
    {
	.name     = "org.bluez",
	.datapipe = &bluez_service_state_pipe,
    },
    {
	.name     = COMPOSITOR_SERVICE,
	.datapipe = &compositor_service_state_pipe,
    },
    {
	/* Note: due to lipstick==compositor assumption lipstick
	 *       service name must be probed after compositor */
	.name     = LIPSTICK_SERVICE,
	.datapipe = &lipstick_service_state_pipe,
    },
    {
	.name     = DEVICELOCK_SERVICE,
	.datapipe = &devicelock_service_state_pipe,
    },
    {
	.name     = USB_MODED_DBUS_SERVICE,
	.datapipe = &usbmoded_service_state_pipe,
    },
    {
	.name     = FINGERPRINT1_DBUS_SERVICE,
	.datapipe = &fpd_service_state_pipe,
    },
    {
	.name     = "com.nokia.NonGraphicFeedback1.Backend",
	.datapipe = &ngfd_service_state_pipe,
    },
    {
	.name = 0,
    }
};

/** D-Bus message filter for handling NameOwnerChanged signals
 *
 * @param con       (not used)
 * @param msg       message to be acted upon
 * @param user_data (not used)
 *
 * @return DBUS_HANDLER_RESULT_NOT_YET_HANDLED (other filters see the msg too)
 */
static
DBusHandlerResult
mce_dbus_peerinfo_filter_cb(DBusConnection *con, DBusMessage *msg, void *user_data)
{
    (void)user_data;
    (void)con;

    DBusHandlerResult res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *name = 0;
    const char *prev = 0;
    const char *curr = 0;

    DBusError err = DBUS_ERROR_INIT;

    if( !dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS,
				"NameOwnerChanged") )
	goto EXIT;

    if( !dbus_message_get_args(msg, &err,
			       DBUS_TYPE_STRING, &name,
			       DBUS_TYPE_STRING, &prev,
			       DBUS_TYPE_STRING, &curr,
			       DBUS_TYPE_INVALID) ) {
	mce_log(LL_WARN, "%s: %s", err.name, err.message);
	goto EXIT;
    }

    mce_dbus_update_peerinfo(name, curr);

EXIT:
    dbus_error_free(&err);
    return res;
}

/** D-Bus name to peerinfo_t object look up table */
static GHashTable *mce_dbus_peerinfo_lut = 0;

/** Initialize peerinfo lookup table and seed it with essential services
 */
static void
mce_dbus_init_peerinfo(void)
{
    if( !mce_dbus_peerinfo_lut ) {
	mce_dbus_peerinfo_lut = g_hash_table_new_full(g_str_hash, g_str_equal,
						      g_free, peerinfo_delete_cb);

	dbus_connection_add_filter(dbus_connection,
				   mce_dbus_peerinfo_filter_cb, 0, 0);

	for( int i = 0; mce_dbus_nameowner_lut[i].name; ++i ) {
	    peerinfo_t *info = mce_dbus_add_peerinfo(mce_dbus_nameowner_lut[i].name);
	    peerinfo_set_datapipe(info, mce_dbus_nameowner_lut[i].datapipe);
	}
    }
}

/** Cleanup peerinfo lookup table
 */
static void
mce_dbus_quit_peerinfo(void)
{
    if( mce_dbus_peerinfo_lut ) {
	dbus_connection_remove_filter(dbus_connection,
				      mce_dbus_peerinfo_filter_cb, 0);

	g_hash_table_unref(mce_dbus_peerinfo_lut),
	    mce_dbus_peerinfo_lut = 0;
    }
}

/** Lookup already existing peerinfo based on D-Bus name
 */
peerinfo_t *
mce_dbus_get_peerinfo(const char *name)
{
    peerinfo_t *info = 0;

    if( !mce_dbus_peerinfo_lut )
	goto EXIT;

    info = g_hash_table_lookup(mce_dbus_peerinfo_lut, name);

EXIT:
    return info;
}

/** Lookup / create peerinfo based on D-Bus name
 */
peerinfo_t *
mce_dbus_add_peerinfo(const char *name)
{
    peerinfo_t *info = 0;

    if( !mce_dbus_peerinfo_lut )
	goto EXIT;

    if( !(info = g_hash_table_lookup(mce_dbus_peerinfo_lut, name)) ) {
	info = peerinfo_create(name);
	g_hash_table_replace(mce_dbus_peerinfo_lut,
			     g_strdup(name), info);
    }

EXIT:
    return info;
}

/** Update name owner in already existing peerinfo object
 */
void
mce_dbus_update_peerinfo(const char *name, const char *owner)
{
    peerinfo_t *info = mce_dbus_get_peerinfo(name);

    if( !info )
	goto EXIT;

    if( peerinfo_set_owner_name(info, owner) ) {
	peerinfo_set_state(info, PEERSTATE_STOPPED);

	if( peerinfo_get_owner_name(info) ) {
	    peerinfo_set_state(info, PEERSTATE_QUERY_PID);
	}
    }

EXIT:
    return;
}

/** Remove peerinfo based on D-Bus name
 */
void
mce_dbus_del_peerinfo(const char *name)
{
    if( !mce_dbus_peerinfo_lut )
	goto EXIT;

    g_hash_table_remove(mce_dbus_peerinfo_lut, name);

EXIT:
    return;
}

/** Get useful-for-debugging description of process owning D-Bus name
 */
const char *
mce_dbus_get_peerdesc(const char *name)
{
    return peerinfo_repr(mce_dbus_add_peerinfo(name));
}

/* ========================================================================= *
 * MESSAGE_SENDING
 * ========================================================================= */

/** Return reference to dbus connection cached at mce-dbus module
 *
 * For use in situations where the abstraction provided by mce-dbus
 * makes things too complicated.
 *
 * Caller must release non-null return values with dbus_connection_unref().
 *
 * @return DBusConnection, or NULL if mce has no dbus connection
 */
DBusConnection *dbus_connection_get(void)
{
	if( !dbus_connection ) {
		mce_log(LL_WARN, "no dbus connection");
		return NULL;
	}
	return dbus_connection_ref(dbus_connection);
}

/**
 * Create a new D-Bus signal, with proper error checking
 * will exit the mainloop if an error occurs
 *
 * @param path The signal path
 * @param interface The signal interface
 * @param name The name of the signal to send
 * @return A new DBusMessage
 */
DBusMessage *dbus_new_signal(const gchar *const path,
			     const gchar *const interface,
			     const gchar *const name)
{
	DBusMessage *msg = dbus_message_new_signal(path, interface, name);

	if( !msg )
		mce_abort();

	if( !introspectable_signal(interface, name) ) {
		mce_log(LL_ERR, "sending non-introspectable signal: %s.%s",
			interface, name);
	}

	return msg;
}

/**
 * Create a new D-Bus error message, with proper error checking
 * will exit the mainloop if an error occurs
 *
 * @param req  Method call message for which to create an error reply
 * @param err  D-Bus error name
 * @param fmt  Error message format string
 * @param ...  Arguments required by the format string
 *
 * @return A new DBusMessage
 */

static DBusMessage *dbus_new_error(DBusMessage *req,
				   const char *err,
				   const char *fmt,
				   ...) __attribute__((format(printf, 3, 4)));

static DBusMessage *dbus_new_error(DBusMessage *req,
				   const char *err,
				   const char *fmt,
				   ...)
{
	char *msg = 0;

	va_list va;
	va_start(va, fmt);
	if( vasprintf(&msg, fmt, va) < 0 )
		msg = 0;
	va_end(va);

	DBusMessage *rsp = dbus_message_new_error(req, err, msg);

	if( !rsp )
		mce_abort();

	free(msg);
	return rsp;
}

/**
 * Create a new D-Bus method call, with proper error checking
 * will exit the mainloop if an error occurs
 *
 * @param service The method call service
 * @param path The method call path
 * @param interface The method call interface
 * @param name The name of the method to call
 * @return A new DBusMessage
 */
DBusMessage *dbus_new_method_call(const gchar *const service,
				  const gchar *const path,
				  const gchar *const interface,
				  const gchar *const name)
{
	DBusMessage *msg = dbus_message_new_method_call(service, path,
							interface, name);
	if( !msg )
		mce_abort();

	return msg;
}

/**
 * Create a new D-Bus method call reply, with proper error checking
 * will exit the mainloop if an error occurs
 *
 * @param message The DBusMessage to reply to
 * @return A new DBusMessage
 */
DBusMessage *dbus_new_method_reply(DBusMessage *const message)
{
	DBusMessage *msg = dbus_message_new_method_return(message);

	if( !msg )
		mce_abort();

	return msg;
}

/**
 * Send a D-Bus message
 * Side-effects: frees msg
 *
 * @param msg The D-Bus message to send
 * @return TRUE on success, FALSE on out of memory
 */
gboolean dbus_send_message(DBusMessage *const msg)
{
	gboolean status = FALSE;

	mce_wakelock_obtain("dbus_send", MCE_DBUS_SEND_SUSPEND_BLOCK_MS);

	if (dbus_connection_send(dbus_connection, msg, NULL) == FALSE) {
		mce_log(LL_CRIT,
			"Out of memory when sending D-Bus message");
		goto EXIT;
	}

	status = TRUE;

EXIT:
	dbus_message_unref(msg);

	return status;
}

/**
 * Send a D-Bus message and setup a reply callback
 * Side-effects: frees msg
 *
 * @param msg The D-Bus message to send
 * @param callback The reply callback
 * @param user_data Data to pass to callback
 * @param user_free Delete callback for user_data
 * @return TRUE on success, FALSE on failure
 */
static gboolean
dbus_send_message_with_reply_handler(DBusMessage *const msg,
				     DBusPendingCallNotifyFunction callback,
				     int timeout,
				     void *user_data,
				     DBusFreeFunction user_free,
				     DBusPendingCall **ppc)
{
	gboolean         status = FALSE;
	DBusPendingCall *pc     = 0;

	if( !msg )
		goto EXIT;

	if( !dbus_connection_send_with_reply(dbus_connection, msg, &pc,
					     timeout) ) {
		mce_log(LL_CRIT, "Out of memory when sending D-Bus message");
		goto EXIT;
	}

	if( !pc ) {
		mce_log(LL_ERR, "D-Bus connection disconnected");
		goto EXIT;
	}

	mdb_callgate_attach(pc);

	if( !dbus_pending_call_set_notify(pc, callback,
					  user_data, user_free) ) {
		mce_log(LL_CRIT, "Out of memory when sending D-Bus message");
		goto EXIT;
	}

	/* If caller asked for pending call handle, increase
	 * the refcount by one */
	if( ppc )
		*ppc = dbus_pending_call_ref(pc);

	/* Ownership of user_data passed on */
	user_free = 0, user_data = 0;

	status = TRUE;

EXIT:
	/* Release user_data if the ownership was not passed on */
	if( user_free )
		user_free(user_data);

	/* If notification was set succesfully above, it will hold
	 * one reference to the pending call until a) the callback function
	 * gets called, or b) the pending call is canceled */
	if( pc )
		dbus_pending_call_unref(pc);

	if( msg )
		dbus_message_unref(msg);

	return status;
}

static gboolean dbus_send_va(const char *service,
			     const char *path,
			     const char *interface,
			     const char *name,
			     DBusPendingCallNotifyFunction callback,
			     int         timeout,
			     void *user_data, DBusFreeFunction user_free,
			     DBusPendingCall **ppc,
			     int first_arg_type, va_list va)
{
	gboolean     res = FALSE;
	DBusMessage *msg = 0;

	/* Method call or signal? */
	if( service ) {
		msg = dbus_new_method_call(service, path, interface, name);

		if( !callback )
			dbus_message_set_no_reply(msg, TRUE);
	}
	else if( callback ) {
		mce_log(LL_ERR, "Programmer snafu! "
			"dbus_send() called with a DBusPending "
			"callback for a signal.  Whoopsie!");
		goto EXIT;
	}
	else {
		msg = dbus_new_signal(path, interface, name);
	}

	/* Append the arguments, if any */
	if( first_arg_type != DBUS_TYPE_INVALID  &&
	    !dbus_message_append_args_valist(msg, first_arg_type, va)) {
		mce_log(LL_CRIT, "Failed to append arguments to D-Bus message "
			"for %s.%s", interface, name);
		goto EXIT;
	}

	/* Send the signal / call the method */
	if( !callback ) {
		res = dbus_send_message(msg);
		msg = 0;
	}
	else {
		res = dbus_send_message_with_reply_handler(msg,
							   callback,
							   timeout,
							   user_data,
							   user_free,
							   ppc);
		msg = 0;

		/* Ownership of user_data passed on */
		user_data = 0, user_free = 0;
	}

EXIT:
	/* Release user_data if the ownership was not passed on */
	if( user_free )
		user_free(user_data);

	if( msg ) dbus_message_unref(msg);

	return res;
}

/** Generic function to send D-Bus messages and signals
 * to send a signal, call dbus_send with service == NULL
 *
 * @todo Make it possible to send D-Bus replies as well
 *
 * @param service        D-Bus service; for signals, set to NULL
 * @param path           D-Bus path
 * @param interface      D-Bus interface
 * @param name The       D-Bus method or signal name to send to
 * @param callback       A reply callback, or NULL to set no reply;
 *                       for signals, this is unused, but please use NULL
 *                       for consistency
 * @param user_data      Data to pass to callback
 * @param user_free      Data release callback for user_data
 * @param ppc            Where to store pending call handle, or NULL
 * @param first_arg_type The DBUS_TYPE of the first argument in the list
 * @param ...            The arguments to append to the D-Bus message;
 *                       terminate with DBUS_TYPE_INVALID
 *                       Note: the arguments MUST be passed by reference
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean dbus_send_ex(const char *service,
		      const char *path,
		      const char *interface,
		      const char *name,
		      DBusPendingCallNotifyFunction callback,
		      void *user_data, DBusFreeFunction user_free,
		      DBusPendingCall **ppc,
		      int first_arg_type, ...)
{
	va_list va;
	va_start(va, first_arg_type);
	gboolean res = dbus_send_va(service, path, interface, name,
				    callback, -1, user_data, user_free,
				    ppc, first_arg_type, va);
	va_end(va);
	return res;
}

/** Generic function to send D-Bus messages and signals
 * to send a signal, call dbus_send with service == NULL
 *
 * @todo Make it possible to send D-Bus replies as well
 *
 * @param service        D-Bus service; for signals, set to NULL
 * @param path           D-Bus path
 * @param interface      D-Bus interface
 * @param name The       D-Bus method or signal name to send to
 * @param callback       A reply callback, or NULL to set no reply;
 *                       for signals, this is unused, but please use NULL
 *                       for consistency
 * @param timeout        Milliseconds to wait for reply, or -1 for default
 * @param user_data      Data to pass to callback
 * @param user_free      Data release callback for user_data
 * @param ppc            Where to store pending call handle, or NULL
 * @param first_arg_type The DBUS_TYPE of the first argument in the list
 * @param ...            The arguments to append to the D-Bus message;
 *                       terminate with DBUS_TYPE_INVALID
 *                       Note: the arguments MUST be passed by reference
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean dbus_send_ex2(const char *service,
		       const char *path,
		       const char *interface,
		       const char *name,
		       DBusPendingCallNotifyFunction callback,
		       int timeout,
		       void *user_data, DBusFreeFunction user_free,
		       DBusPendingCall **ppc,
		       int first_arg_type, ...)
{
	va_list va;
	va_start(va, first_arg_type);
	gboolean res = dbus_send_va(service, path, interface, name,
				    callback, timeout, user_data, user_free,
				    ppc, first_arg_type, va);
	va_end(va);
	return res;
}

/**
 * Generic function to send D-Bus messages and signals
 * to send a signal, call dbus_send with service == NULL
 *
 * @todo Make it possible to send D-Bus replies as well
 *
 * @param service D-Bus service; for signals, set to NULL
 * @param path D-Bus path
 * @param interface D-Bus interface
 * @param name The D-Bus method or signal name to send to
 * @param callback A reply callback, or NULL to set no reply;
 *                 for signals, this is unused, but please use NULL
 *                 for consistency
 * @param first_arg_type The DBUS_TYPE of the first argument in the list
 * @param ... The arguments to append to the D-Bus message;
 *            terminate with DBUS_TYPE_INVALID
 *            Note: the arguments MUST be passed by reference
 * @return TRUE on success, FALSE on failure
 */
gboolean dbus_send(const gchar *const service, const gchar *const path,
		   const gchar *const interface, const gchar *const name,
		   DBusPendingCallNotifyFunction callback,
		   int first_arg_type, ...)
{
	va_list va;
	va_start(va, first_arg_type);
	gboolean res = dbus_send_va(service, path, interface, name,
				    callback, -1, 0, 0, 0, first_arg_type, va);
	va_end(va);
	return res;
}

/* ========================================================================= *
 * METHOD_CALL_HANDLERS
 * ========================================================================= */

/**
 * D-Bus callback for the version get method call
 *
 * @param msg The D-Bus message to reply to
 * @return TRUE on success, FALSE on failure
 */
static gboolean version_get_dbus_cb(DBusMessage *const msg)
{
	static const gchar *const versionstring = G_STRINGIFY(PRG_VERSION);
	DBusMessage *reply = NULL;
	gboolean status = FALSE;

	mce_log(LL_DEBUG, "Received version information request");

	/* Create a reply */
	reply = dbus_new_method_reply(msg);

	/* Append the version information */
	if (dbus_message_append_args(reply,
				     DBUS_TYPE_STRING, &versionstring,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append reply argument to D-Bus message "
			"for %s.%s",
			MCE_REQUEST_IF, MCE_VERSION_GET);
		dbus_message_unref(reply);
		goto EXIT;
	}

	/* Send the message */
	status = dbus_send_message(reply);

EXIT:
	return status;
}

/** D-Bus callback for the get suspend time statistics method call
 *
 * @param req The D-Bus message to reply to
 *
 * @return TRUE
 */
static gboolean suspend_stats_get_dbus_cb(DBusMessage *const req)
{
	DBusMessage *rsp = 0;

	mce_log(LL_DEVEL, "suspend info request from %s",
		mce_dbus_get_message_sender_ident(req));

	/* Get time values - in an order that is less
	 * likely to produce negative values on subtract.
	 */
	dbus_int64_t active_ms  = mce_lib_get_mono_tick();
	dbus_int64_t uptime_ms  = mce_lib_get_boot_tick();
	dbus_int64_t suspend_ms = uptime_ms - active_ms;

	/* create and send reply message */
	rsp = dbus_new_method_reply(req);

	if( !dbus_message_append_args(rsp,
				      DBUS_TYPE_INT64, &uptime_ms,
				      DBUS_TYPE_INT64, &suspend_ms,
				      DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "Failed to append arguments");
		goto EXIT;
	}

	dbus_send_message(rsp), rsp = 0;

EXIT:
	if( rsp )
		dbus_message_unref(rsp);

	return TRUE;
}

/** D-Bus callback for: get mce verbosity method call
 *
 * @param req The D-Bus message to reply to
 *
 * @return TRUE
 */
static gboolean verbosity_get_dbus_cb(DBusMessage *const req)
{
	DBusMessage *rsp = 0;

	mce_log(LL_DEVEL, "verbosity get from %s",
		mce_dbus_get_message_sender_ident(req));

	dbus_int32_t verbosity = mce_log_get_verbosity();

	rsp = dbus_new_method_reply(req);

	if( !dbus_message_append_args(rsp,
				      DBUS_TYPE_INT32, &verbosity,
				      DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "Failed to append arguments");
		goto EXIT;
	}

	dbus_send_message(rsp), rsp = 0;

EXIT:
	if( rsp )
		dbus_message_unref(rsp);

	return TRUE;
}

/** D-Bus callback for: set mce verbosity method call
 *
 * @param req The D-Bus message to reply to
 *
 * @return TRUE
 */
static gboolean verbosity_set_dbus_cb(DBusMessage *const req)
{
	dbus_bool_t  ack = false;
	DBusError    err = DBUS_ERROR_INIT;

	mce_log(LL_DEVEL, "verbosity set from %s",
		mce_dbus_get_message_sender_ident(req));

	dbus_int32_t verbosity = LL_WARN;

	if( !dbus_message_get_args(req, &err,
				   DBUS_TYPE_INT32, &verbosity,
				   DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "%s: %s", err.name, err.message);
		goto EXIT;
	}

	mce_log_set_verbosity(verbosity);
	ack = true;

EXIT:
	if( !dbus_message_get_no_reply(req) ) {
		DBusMessage *rsp = dbus_new_method_reply(req);
		if( !dbus_message_append_args(rsp,
					      DBUS_TYPE_BOOLEAN, &ack,
					      DBUS_TYPE_INVALID) ) {
			mce_log(LL_ERR, "Failed to append arguments");
			dbus_message_unref(rsp), rsp = 0;
		}
		else {
			dbus_send_message(rsp), rsp = 0;
		}
	}

	dbus_error_free(&err);

	return TRUE;
}

/* ========================================================================= *
 * CONFIG_VALUES
 * ========================================================================= */

/** Helper for appending gconf string list to dbus message
 *
 * @param conf GConfValue of string list type
 * @param pcount number of items in the returned array is stored here
 * @return array of string pointers that can be easily added to DBusMessage
 */
static const char **string_array_from_gconf_value(GConfValue *conf, int *pcount)
{
	const char **array = 0;
	int    count = 0;

	GSList *list, *item;

	if( conf->type != GCONF_VALUE_LIST )
		goto EXIT;

	if( gconf_value_get_list_type(conf) != GCONF_VALUE_STRING )
		goto EXIT;

	list = gconf_value_get_list(conf);

	for( item = list; item; item = item->next )
		++count;

	array = g_malloc_n(count, sizeof *array);
	count = 0;

	for( item = list; item; item = item->next )
		array[count++] = gconf_value_get_string(item->data);

EXIT:
	return *pcount = count, array;
}

/** Helper for appending gconf int list to dbus message
 *
 * @param conf GConfValue of int list type
 * @param pcount number of items in the returned array is stored here
 * @return array of integers that can be easily added to DBusMessage
 */
static dbus_int32_t *int_array_from_gconf_value(GConfValue *conf, int *pcount)
{
	dbus_int32_t *array = 0;
	int           count = 0;

	GSList *list, *item;

	if( conf->type != GCONF_VALUE_LIST )
		goto EXIT;

	if( gconf_value_get_list_type(conf) != GCONF_VALUE_INT )
		goto EXIT;

	list = gconf_value_get_list(conf);

	for( item = list; item; item = item->next )
		++count;

	array = g_malloc_n(count, sizeof *array);
	count = 0;

	for( item = list; item; item = item->next )
		array[count++] = gconf_value_get_int(item->data);

EXIT:
	return *pcount = count, array;
}

/** Helper for appending gconf bool list to dbus message
 *
 * @param conf GConfValue of bool list type
 * @param pcount number of items in the returned array is stored here
 * @return array of booleans that can be easily added to DBusMessage
 */
static dbus_bool_t *bool_array_from_gconf_value(GConfValue *conf, int *pcount)
{
	dbus_bool_t *array = 0;
	int          count = 0;

	GSList *list, *item;

	if( conf->type != GCONF_VALUE_LIST )
		goto EXIT;

	if( gconf_value_get_list_type(conf) != GCONF_VALUE_BOOL )
		goto EXIT;

	list = gconf_value_get_list(conf);

	for( item = list; item; item = item->next )
		++count;

	array = g_malloc_n(count, sizeof *array);
	count = 0;

	for( item = list; item; item = item->next )
		array[count++] = gconf_value_get_bool(item->data);

EXIT:
	return *pcount = count, array;
}

/** Helper for appending gconf float list to dbus message
 *
 * @param conf GConfValue of float list type
 * @param pcount number of items in the returned array is stored here
 * @return array of doubles that can be easily added to DBusMessage
 */
static double *float_array_from_gconf_value(GConfValue *conf, int *pcount)
{
	double *array = 0;
	int     count = 0;

	GSList *list, *item;

	if( conf->type != GCONF_VALUE_LIST )
		goto EXIT;

	if( gconf_value_get_list_type(conf) != GCONF_VALUE_FLOAT )
		goto EXIT;

	list = gconf_value_get_list(conf);

	for( item = list; item; item = item->next )
		++count;

	array = g_malloc_n(count, sizeof *array);
	count = 0;

	for( item = list; item; item = item->next )
		array[count++] = gconf_value_get_float(item->data);

EXIT:
	return *pcount = count, array;
}

/** Helper for deducing what kind of array signature we need for a list value
 *
 * @param type Non-complex gconf value type
 *
 * @return D-Bus signature needed for adding given type to a container
 */
static const char *type_signature(GConfValueType type)
{
	switch( type ) {
	case GCONF_VALUE_STRING: return DBUS_TYPE_STRING_AS_STRING;
	case GCONF_VALUE_INT:    return DBUS_TYPE_INT32_AS_STRING;
	case GCONF_VALUE_FLOAT:  return DBUS_TYPE_DOUBLE_AS_STRING;
	case GCONF_VALUE_BOOL:   return DBUS_TYPE_BOOLEAN_AS_STRING;
	default: break;
	}
	return 0;
}

/** Helper for deducing what kind of variant signature we need for a value
 *
 * @param conf GConf value
 *
 * @return D-Bus signature needed for adding given value to a container
 */
static const char *value_signature(GConfValue *conf)
{
	if( conf->type != GCONF_VALUE_LIST ) {
		return type_signature(conf->type);
	}

	switch( gconf_value_get_list_type(conf) ) {
	case GCONF_VALUE_STRING:
		return DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_STRING_AS_STRING;
	case GCONF_VALUE_INT:
		return DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_INT32_AS_STRING;
	case GCONF_VALUE_FLOAT:
		return DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_DOUBLE_AS_STRING;
	case GCONF_VALUE_BOOL:
		return DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BOOLEAN_AS_STRING;
	default: break;
	}

	return 0;
}

/** Helper for appending GConfValue to dbus message
 *
 * @param iter  Append iterator for DBusMessage under construction
 * @param conf  GConfValue to be added at the iterator
 *
 * @return true if the value was succesfully appended, or false on failure
 */
static bool append_gconf_value_to_dbus_iterator(DBusMessageIter *iter, GConfValue *conf)
{
	const char *sig = 0;

	DBusMessageIter variant, array;

	if( !(sig = value_signature(conf)) ) {
		goto bailout_message;
	}

	if( !dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
					      sig, &variant) ) {
		goto bailout_message;
	}

	switch( conf->type ) {
	case GCONF_VALUE_STRING:
		{
			const char *arg = gconf_value_get_string(conf) ?: "";
			dbus_message_iter_append_basic(&variant,
						       DBUS_TYPE_STRING,
						       &arg);
		}
		break;

	case GCONF_VALUE_INT:
		{
			dbus_int32_t arg = gconf_value_get_int(conf);
			dbus_message_iter_append_basic(&variant,
						       DBUS_TYPE_INT32,
						       &arg);
		}
		break;

	case GCONF_VALUE_FLOAT:
		{
			double arg = gconf_value_get_float(conf);
			dbus_message_iter_append_basic(&variant,
						       DBUS_TYPE_DOUBLE,
						       &arg);
		}
		break;

	case GCONF_VALUE_BOOL:
		{
			dbus_bool_t arg = gconf_value_get_bool(conf);
			dbus_message_iter_append_basic(&variant,
						       DBUS_TYPE_BOOLEAN,
						       &arg);
		}
		break;

	case GCONF_VALUE_LIST:
		if( !(sig = type_signature(gconf_value_get_list_type(conf))) ) {
			goto bailout_variant;
		}

		if( !dbus_message_iter_open_container(&variant,
						      DBUS_TYPE_ARRAY,
						      sig, &array) ) {
			goto bailout_variant;
		}

		switch( gconf_value_get_list_type(conf) ) {
		case GCONF_VALUE_STRING:
			{
				int          cnt = 0;
				const char **arg = string_array_from_gconf_value(conf, &cnt);
				for( int i = 0; i < cnt; ++i ) {
					const char *str = arg[i];
					dbus_message_iter_append_basic(&array,
								       DBUS_TYPE_STRING,
								       &str);
				}
				g_free(arg);
			}
			break;
		case GCONF_VALUE_INT:
			{
				int           cnt = 0;
				dbus_int32_t *arg = int_array_from_gconf_value(conf, &cnt);
				dbus_message_iter_append_fixed_array(&array,
								     DBUS_TYPE_INT32,
								     &arg, cnt);
				g_free(arg);
			}
			break;
		case GCONF_VALUE_FLOAT:
			{
				int     cnt = 0;
				double *arg = float_array_from_gconf_value(conf, &cnt);
				dbus_message_iter_append_fixed_array(&array,
								     DBUS_TYPE_DOUBLE,
								     &arg, cnt);
				g_free(arg);
			}
			break;
		case GCONF_VALUE_BOOL:
			{
				int          cnt = 0;
				dbus_bool_t *arg = bool_array_from_gconf_value(conf, &cnt);
				dbus_message_iter_append_fixed_array(&array,
								     DBUS_TYPE_BOOLEAN,
								     &arg, cnt);
				g_free(arg);
			}
			break;

		default:
			goto bailout_array;
		}

		if( !dbus_message_iter_close_container(&variant, &array) ) {
			goto bailout_variant;
		}
		break;

	default:
		goto bailout_variant;
	}

	if( !dbus_message_iter_close_container(iter, &variant) ) {
		goto bailout_message;
	}
	return true;

bailout_array:
	dbus_message_iter_abandon_container(&variant, &array);

bailout_variant:
	dbus_message_iter_abandon_container(iter, &variant);

bailout_message:
	return false;
}

/** Helper for appending GConfEntry to dbus message
 *
 * @param iter   Append iterator for DBusMessage under construction
 * @param entry  GConfEntry to be added at the iterator
 *
 * @return true if the value was succesfully appended, or false on failure
 */
static bool append_gconf_entry_to_dbus_iterator(DBusMessageIter *iter, GConfEntry *entry)
{
	DBusMessageIter sub;

	if( !dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY,
					      0, &sub) ) {
		goto bailout_message;
	}

	if( !dbus_message_iter_append_basic(&sub,
					    DBUS_TYPE_STRING, &entry->key) ) {
		goto bailout_container;
	}

	if( !append_gconf_value_to_dbus_iterator(&sub, entry->value) ) {
		goto bailout_container;
	}

	if( !dbus_message_iter_close_container(iter, &sub) ) {
		goto bailout_message;
	}

	return true;

bailout_container:
	dbus_message_iter_abandon_container(iter, &sub);

bailout_message:
	return false;
}

/** Helper for appending list of GConfEntry objects to dbus message
 *
 * @param iter     Append iterator for DBusMessage under construction
 * @param entries  GSList containing GConfEntry objects
 *
 * @return true if the value was succesfully appended, or false on failure
 */
static bool append_gconf_entries_to_dbus_iterator(DBusMessageIter *iter, GSList *entries)
{
	DBusMessageIter sub;

	if( !dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					      DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					      DBUS_TYPE_STRING_AS_STRING
					      DBUS_TYPE_VARIANT_AS_STRING
					      DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
					      &sub) ) {
		goto bailout_message;
	}

	for( GSList *item = entries; item; item = item->next )
	{
		GConfEntry *entry = item->data;

		if( !append_gconf_entry_to_dbus_iterator(&sub, entry) )
			goto bailout_container;
	}

	if( !dbus_message_iter_close_container(iter, &sub) ) {
		goto bailout_message;
	}

	return true;

bailout_container:
	dbus_message_iter_abandon_container(iter, &sub);

bailout_message:
	return false;
}

/** Helper for appending GConfValue to dbus message
 *
 * @param reply DBusMessage under construction
 * @param conf GConfValue to be added to the reply
 *
 * @return true if the value was succesfully appended, or false on failure
 */
static bool append_gconf_value_to_dbus_message(DBusMessage *reply, GConfValue *conf)
{
	DBusMessageIter body;

	dbus_message_iter_init_append(reply, &body);

	return append_gconf_value_to_dbus_iterator(&body, conf);
}

/**
 * D-Bus callback for the config get method call
 *
 * @param msg The D-Bus message to reply to
 *
 * @return TRUE if reply message was successfully sent, FALSE on failure
 */
static gboolean config_get_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;
	DBusMessage *reply = NULL;
	const char *key = NULL;
	GError *err = NULL;
	GConfValue *conf = 0;

	DBusMessageIter body;

	mce_log(LL_DEBUG, "Received configuration query request");

	dbus_message_iter_init(msg, &body);

	/* HACK: The key used to be object path, not string.
	 *       Allow clients to use either one. */
	switch( dbus_message_iter_get_arg_type(&body) ) {
	case DBUS_TYPE_OBJECT_PATH:
	case DBUS_TYPE_STRING:
		dbus_message_iter_get_basic(&body, &key);
		dbus_message_iter_next(&body);
		break;

	default:
		reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
					       "expected string/object path");
		goto EXIT;
	}

	if( !(conf = gconf_client_get(gconf_client_get_default(), key, &err)) ) {
		reply = dbus_message_new_error(msg,
					       "com.nokia.mce.GConf.Error",
					       err->message ?: "unknown");
		goto EXIT;
	}

	if( !(reply = dbus_new_method_reply(msg)) )
		goto EXIT;

	if( !append_gconf_value_to_dbus_message(reply, conf) ) {
		dbus_message_unref(reply);
		reply = dbus_message_new_error(msg,
					       "com.nokia.mce.GConf.Error",
					       "constructing reply failed");
	}

EXIT:
	/* Send a reply if we have one */
	if( reply ) {
		if( dbus_message_get_no_reply(msg) ) {
			dbus_message_unref(reply), reply = 0;
			status = TRUE;
		}
		else {
			/* dbus_send_message unrefs the reply message */
			status = dbus_send_message(reply), reply = 0;
		}
	}

	if( conf )
		gconf_value_free(conf);

	g_clear_error(&err);

	return status;
}

/** D-Bus callback for the config get all -method call
 *
 * @param msg The D-Bus message to reply to
 *
 * @return TRUE
 */
static gboolean config_get_all_dbus_cb(DBusMessage *const req)
{
	GConfClient *cli = 0;
	DBusMessage *rsp = 0;

	mce_log(LL_DEBUG, "Received configuration query request");

	if( !(cli = gconf_client_get_default()) )
		goto EXIT;

	if( !(rsp = dbus_new_method_reply(req)) )
		goto EXIT;

	DBusMessageIter body;

	dbus_message_iter_init_append(rsp, &body);

	if( !append_gconf_entries_to_dbus_iterator(&body, cli->entries) ) {
		dbus_message_unref(rsp), rsp = 0;
	}

EXIT:

	if( !dbus_message_get_no_reply(req) ) {
		if( !rsp )
			rsp = dbus_message_new_error(req, "com.nokia.mce.GConf.Error",
						     "unknown");
		if( rsp )
			dbus_send_message(rsp), rsp = 0;
	}

	if( rsp )
		dbus_message_unref(rsp);

	return TRUE;
}

/** Send configuration changed notification signal
 *
 * @param entry changed setting
 */
void mce_dbus_send_config_notification(GConfEntry *entry)
{
	const char  *key = 0;
	GConfValue  *val = 0;
	DBusMessage *sig = 0;

	if( !entry )
		goto EXIT;

	if( !(key = gconf_entry_get_key(entry)) )
		goto EXIT;

	if( !(val = gconf_entry_get_value(entry)) )
		goto EXIT;

	mce_log(LL_DEBUG, "%s: changed", key);

	sig = dbus_new_signal(MCE_SIGNAL_PATH,
			      MCE_SIGNAL_IF,
			      MCE_CONFIG_CHANGE_SIG);

	dbus_message_append_args(sig,
				 DBUS_TYPE_STRING, &key,
				 DBUS_TYPE_INVALID);

	append_gconf_value_to_dbus_message(sig, val);

	dbus_send_message(sig), sig = 0;

EXIT:

	if( sig ) dbus_message_unref(sig);

	return;
}

/** Release GSList of GConfValue objects
 *
 * @param list GSList where item->data members are pointers to GConfValue
 */
static void value_list_free(GSList *list)
{
	g_slist_free_full(list, (GDestroyNotify)gconf_value_free);
}

/** Convert D-Bus string array into GSList of GConfValue objects
 *
 * @param iter D-Bus message iterator at DBUS_TYPE_ARRAY
 * @return GSList where item->data members are pointers to GConfValue
 */
static GSList *value_list_from_string_array(DBusMessageIter *iter)
{
	GSList *res = 0;

	DBusMessageIter subiter;

	dbus_message_iter_recurse(iter, &subiter);

	int i = 0;
	while ( dbus_message_iter_get_arg_type(&subiter) == DBUS_TYPE_STRING ) {
		const char *tmp = 0;
		dbus_message_iter_get_basic(&subiter, &tmp);
		dbus_message_iter_next(&subiter);

		mce_log(LL_INFO, "arr[%d] = string:%s", i++, tmp);

		GConfValue *value = gconf_value_new(GCONF_VALUE_STRING);
		gconf_value_set_string(value, tmp);
		res = g_slist_prepend(res, value);
	}

	res = g_slist_reverse(res);

	return res;
}

/** Convert D-Bus int32 array into GSList of GConfValue objects
 *
 * @param iter D-Bus message iterator at DBUS_TYPE_ARRAY
 * @return GSList where item->data members are pointers to GConfValue
 */
static GSList *value_list_from_int_array(DBusMessageIter *iter)
{
	GSList *res = 0;

	DBusMessageIter subiter;

	dbus_message_iter_recurse(iter, &subiter);

	int i = 0;
	while ( dbus_message_iter_get_arg_type(&subiter) == DBUS_TYPE_INT32 ) {
		dbus_int32_t tmp = 0;
		dbus_message_iter_get_basic(&subiter, &tmp);
		dbus_message_iter_next(&subiter);

		mce_log(LL_INFO, "arr[%d] = int:%d", i++, tmp);

		GConfValue *value = gconf_value_new(GCONF_VALUE_INT);
		gconf_value_set_int(value, tmp);
		res = g_slist_prepend(res, value);
	}

	res = g_slist_reverse(res);

	return res;
}

/** Convert D-Bus bool array into GSList of GConfValue objects
 *
 * @param iter D-Bus message iterator at DBUS_TYPE_ARRAY
 * @return GSList where item->data members are pointers to GConfValue
 */
static GSList *value_list_from_bool_array(DBusMessageIter *iter)
{
	GSList *res = 0;

	DBusMessageIter subiter;

	dbus_message_iter_recurse(iter, &subiter);

	int i = 0;
	while ( dbus_message_iter_get_arg_type(&subiter) == DBUS_TYPE_BOOLEAN ) {
		dbus_bool_t tmp = 0;
		dbus_message_iter_get_basic(&subiter, &tmp);
		dbus_message_iter_next(&subiter);

		mce_log(LL_INFO, "arr[%d] = bool:%s", i++, tmp ? "true" : "false");

		GConfValue *value = gconf_value_new(GCONF_VALUE_BOOL);
		gconf_value_set_bool(value, tmp);
		res = g_slist_prepend(res, value);
	}

	res = g_slist_reverse(res);

	return res;
}

/** Convert D-Bus double array into GSList of GConfValue objects
 *
 * @param iter D-Bus message iterator at DBUS_TYPE_ARRAY
 * @return GSList where item->data members are pointers to GConfValue
 */
static GSList *value_list_from_float_array(DBusMessageIter *iter)
{
	GSList *res = 0;

	DBusMessageIter subiter;

	dbus_message_iter_recurse(iter, &subiter);

	int i = 0;
	while ( dbus_message_iter_get_arg_type(&subiter) == DBUS_TYPE_DOUBLE ) {
		double tmp = 0;
		dbus_message_iter_get_basic(&subiter, &tmp);
		dbus_message_iter_next(&subiter);

		mce_log(LL_INFO, "arr[%d] = float:%g", i++, tmp);

		GConfValue *value = gconf_value_new(GCONF_VALUE_FLOAT);
		gconf_value_set_float(value, tmp);
		res = g_slist_prepend(res, value);
	}

	res = g_slist_reverse(res);

	return res;
}

/**
 * D-Bus callback for the config reset method call
 *
 * @param msg The D-Bus message to reply to
 *
 * @return TRUE
 */
static gboolean config_reset_dbus_cb(DBusMessage *const msg)
{
	GConfClient  *client = 0;
	DBusError     error  = DBUS_ERROR_INIT;
	const char   *keyish = 0;
	dbus_int32_t  count  = -1;
	DBusMessage  *reply  = 0;

	mce_log(LL_DEVEL, "Received configuration reset request");

	if( !(client = gconf_client_get_default()) )
		goto EXIT;

	if( !dbus_message_get_args(msg, &error,
				   DBUS_TYPE_STRING, &keyish,
				   DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "%s: %s", error.name, error.message);
		goto EXIT;
	}

	count = gconf_client_reset_defaults(client, keyish);

	/* sync to disk if we changed something */
	if( count > 0 ) {
		GError *err = 0;
		gconf_client_suggest_sync(client, &err);
		if( err )
			mce_log(LL_ERR, "gconf_client_suggest_sync: %s",
				err->message);
		g_clear_error(&err);
	}

EXIT:
	if( dbus_message_get_no_reply(msg) )
		goto NOREPLY;

	if( !(reply = dbus_new_method_reply(msg)) )
		goto NOREPLY;

	dbus_message_append_args(reply,
				 DBUS_TYPE_INT32, &count,
				 DBUS_TYPE_INVALID);

	dbus_send_message(reply), reply = 0;

NOREPLY:
	dbus_error_free(&error);
	return TRUE;
}

/**
 * D-Bus callback for the config set method call
 *
 * @param msg The D-Bus message to reply to
 *
 * @return TRUE if reply message was successfully sent, FALSE on failure
 */
static gboolean config_set_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;
	DBusMessage *reply = NULL;
	const char *key = NULL;
	GError *err = NULL;
	GConfClient *client = 0;
	GSList *list = 0;

	DBusError error = DBUS_ERROR_INIT;
	DBusMessageIter body, iter;

	mce_log(LL_DEBUG, "Received configuration change request");

	if( !(client = gconf_client_get_default()) )
		goto EXIT;

	dbus_message_iter_init(msg, &body);

	/* HACK: The key used to be object path, not string.
	 *       Allow clients to use either one. */
	switch( dbus_message_iter_get_arg_type(&body) ) {
	case DBUS_TYPE_OBJECT_PATH:
	case DBUS_TYPE_STRING:
		dbus_message_iter_get_basic(&body, &key);
		dbus_message_iter_next(&body);
		break;

	default:
		reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
					       "expected string/object path");
		goto EXIT;
	}

	if( dbus_message_iter_get_arg_type(&body) == DBUS_TYPE_VARIANT ) {
		dbus_message_iter_recurse(&body, &iter);
		dbus_message_iter_next(&body);
	}
	else if( dbus_message_iter_get_arg_type(&body) == DBUS_TYPE_ARRAY ) {
		/* HACK: dbus-send does not know how to handle nested
		 * containers,  so it can't be used to send variant
		 * arrays 'variant:array:int32:1,2,3', so we allow array
		 * requrest without variant too ... */
		iter = body;
	}
	else {
		reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
					       "expected variant");
		goto EXIT;
	}

	switch( dbus_message_iter_get_arg_type(&iter) ) {
	case DBUS_TYPE_BOOLEAN:
		{
			dbus_bool_t arg = 0;
			dbus_message_iter_get_basic(&iter, &arg);
			gconf_client_set_bool(client, key, arg, &err);
		}
		break;
	case DBUS_TYPE_INT32:
		{
			dbus_int32_t arg = 0;
			dbus_message_iter_get_basic(&iter, &arg);
			gconf_client_set_int(client, key, arg, &err);
		}
		break;
	case DBUS_TYPE_DOUBLE:
		{
			double arg = 0;
			dbus_message_iter_get_basic(&iter, &arg);
			gconf_client_set_float(client, key, arg, &err);
		}
		break;
	case DBUS_TYPE_STRING:
		{
			const char *arg = 0;
			dbus_message_iter_get_basic(&iter, &arg);
			gconf_client_set_string(client, key, arg, &err);
		}
		break;

	case DBUS_TYPE_ARRAY:
		switch( dbus_message_iter_get_element_type(&iter) ) {
		case DBUS_TYPE_BOOLEAN:
			list = value_list_from_bool_array(&iter);
			gconf_client_set_list(client, key, GCONF_VALUE_BOOL, list, &err);
			break;
		case DBUS_TYPE_INT32:
			list = value_list_from_int_array(&iter);
			gconf_client_set_list(client, key, GCONF_VALUE_INT, list, &err);
			break;
		case DBUS_TYPE_DOUBLE:
			list = value_list_from_float_array(&iter);
			gconf_client_set_list(client, key, GCONF_VALUE_FLOAT, list, &err);
			break;
		case DBUS_TYPE_STRING:
			list = value_list_from_string_array(&iter);
			gconf_client_set_list(client, key, GCONF_VALUE_STRING, list, &err);
			break;
		default:
			reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
						       "unexpected value array type");
			goto EXIT;

		}
		break;

	default:
		reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
					       "unexpected value type");
		goto EXIT;
	}

	if( err )
	{
		/* some of the above gconf_client_set_xxx() calls failed */
		reply = dbus_message_new_error(msg,
					       "com.nokia.mce.GConf.Error",
					       err->message ?: "unknown");
		goto EXIT;
	}

	/* we changed something */
	gconf_client_suggest_sync(client, &err);
	if( err ) {
		mce_log(LL_ERR, "gconf_client_suggest_sync: %s", err->message);
	}

	if( !(reply = dbus_new_method_reply(msg)) )
		goto EXIT;

	/* it is either error reply or true, and we got here... */
	{
		dbus_bool_t arg = TRUE;
		dbus_message_append_args(reply,
					 DBUS_TYPE_BOOLEAN, &arg,
					 DBUS_TYPE_INVALID);
	}

EXIT:
	value_list_free(list);

	/* Send a reply if we have one */
	if( reply ) {
		if( dbus_message_get_no_reply(msg) ) {
			dbus_message_unref(reply), reply = 0;
			status = TRUE;
		}
		else {
			/* dbus_send_message unrefs the reply message */
			status = dbus_send_message(reply), reply = 0;
		}
	}

	g_clear_error(&err);
	dbus_error_free(&error);

	return status;
}

/* ========================================================================= *
 * MESSAGE_DISPATCH
 * ========================================================================= */

/**
 * D-Bus rule checker
 *
 * @param msg The D-Bus message being checked
 * @param rules The rule string to check against
 * @return TRUE if message matches the rules,
 *	   FALSE if not
 */
static gboolean check_rules(DBusMessage *const msg,
			    const char *rules)
{
	if (rules == NULL)
		return TRUE;
	rules += strspn(rules, " ");;

	while (*rules != '\0') {
		const char *eq;
		const char *value;
		const char *value_end;
		const char *val = NULL;
		gboolean quot = FALSE;

		if ((eq = strchr(rules, '=')) == NULL)
			return FALSE;
		eq += strspn(eq, " ");

		if (eq[1] == '\'') {
			value = eq + 2;
			value_end = strchr(value, '\'');
			quot = TRUE;
		} else {
			value = eq + 1;
			value_end = strchrnul(value, ',');
		}

		if (value_end == NULL)
			return FALSE;

		if (strncmp(rules, "arg", 3) == 0) {
			int fld = atoi(rules + 3);

			DBusMessageIter iter;

			if (dbus_message_iter_init(msg, &iter) == FALSE)
				return FALSE;

			for (; fld; fld--) {
				if (dbus_message_iter_has_next(&iter) == FALSE)
					return FALSE;
				dbus_message_iter_next(&iter);
			}

			if (dbus_message_iter_get_arg_type(&iter) !=
			    DBUS_TYPE_STRING)
				return FALSE;
			dbus_message_iter_get_basic(&iter, &val);

		} else if (strncmp(rules, "path", 4) == 0) {
			val = dbus_message_get_path(msg);
		}

		if (((value_end != NULL) &&
		     ((strncmp(value, val, value_end - value) != 0) ||
		      (val[value_end - value] != '\0'))) ||
		    ((value_end == NULL) &&
		     (strcmp(value, val) != 0)))
			return FALSE;

		if (value_end == NULL)
			break;

		rules = value_end + (quot == TRUE ? 1 : 0);
		rules += strspn(rules, " ");;

		if (*rules == ',')
			rules++;
		rules += strspn(rules, " ");;
	}

	return TRUE;
}

/** Build a dbus signal match string
 *
 * For use from mce_dbus_handler_add_ex() and mce_dbus_handler_remove()
 */
static gchar *mce_dbus_build_signal_match(const gchar *sender,
					  const gchar *interface,
					  const gchar *name,
					  const gchar *rules)
{
	gchar *match = 0;

	gchar *match_sender = 0;
	gchar *match_member = 0;
	gchar *match_iface  = 0;
	gchar *match_extra  = 0;

	if( sender )
		match_sender = g_strdup_printf(",sender='%s'", sender);

	if( name )
		match_member = g_strdup_printf(",member='%s'", name);

	if( interface )
		match_iface = g_strdup_printf(",interface='%s'", interface);

	if( rules )
		match_extra = g_strdup_printf(",%s", rules);

	match = g_strdup_printf("type='signal'%s%s%s%s",
				match_sender ?: "",
				match_iface  ?: "",
				match_member ?: "",
				match_extra  ?: "");
	g_free(match_extra);
	g_free(match_iface);
	g_free(match_member);
	g_free(match_sender);

	return match;
}

/** Remove links with NULL data from GSList */
static void mce_dbus_squeeze_slist(GSList **list)
{
	GSList *now, *zen;

	if( !list || !*list )
		goto EXIT;

	/* Move null links to trash, keep the rest in the original order
	 *
	 * Note: This is now one pass O(N) complexity. Using the singly
	 *       linked list api from glib would not allow that. */

	GSList *trash = 0;

	for( now = *list; now; now = zen ) {
		zen = now->next;
		if( now->data )
			*list = now, list = &now->next;
		else
			now->next = trash, trash = now;
	}
	*list = 0;

	/* Release the empty slices */
	g_slist_free(trash);

EXIT:

	return;
}

/** Check if message attribute value matches handler attribute value
 *
 * @return true on match, false otherwise
 */
static bool mce_dbus_match(const char *msg_val, const char *hnd_val)
{
	/* Special case 1: If message attribute has null value,
	 *                 no handler value can be a match */
	if( !msg_val )
		return false;

	/* Special case 2: If handler attribyte has null value,
	 *                 it mathes any non-null message value. */
	if( !hnd_val )
		return true;

	/* Normally we just test for string equality */
	return !strcmp(msg_val, hnd_val);
}

/**
 * D-Bus message handler
 *
 * @param connection Unused
 * @param msg The D-Bus message received
 * @param user_data Unused
 * @return DBUS_HANDLER_RESULT_HANDLED for handled messages
 *         DBUS_HANDLER_RESULT_NOT_HANDLED for unhandled messages
 */
static DBusHandlerResult msg_handler(DBusConnection *const connection,
				     DBusMessage *const msg,
				     gpointer const user_data)
{
	(void)user_data;

	mce_wakelock_obtain("dbus_recv", -1);

	guint status = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	int   type   = dbus_message_get_type(msg);

	const char *interface = dbus_message_get_interface(msg);
	const char *member    = dbus_message_get_member(msg);
	const char *sender    = dbus_message_get_sender(msg);

	peerinfo_t *peerinfo  = 0;

	if( sender )
		peerinfo = mce_dbus_add_peerinfo(sender);

	for( GSList *now = dbus_handlers; now; now = now->next ) {

		handler_struct_t *handler = now->data;

		/* Skip half removed handlers */
		if( !handler )
			continue;

		/* Skip introspect only entries */
		if( !handler->callback )
			continue;

		/* Skip not applicable handlers */
		if( handler->type != type )
			continue;

		switch( handler->type ) {
		case DBUS_MESSAGE_TYPE_METHOD_CALL:
			if( !mce_dbus_match(interface, handler->interface) )
				break;

			if( !mce_dbus_match(member, handler->name) )
				break;

			status = DBUS_HANDLER_RESULT_HANDLED;

			if( !handler->privileged ) {
				handler->callback(msg);
				goto EXIT;
			}

			switch( peerinfo_get_privileged(peerinfo, true) ) {
			case PRIVILEGED_YES:
				handler->callback(msg);
				break;

			case PRIVILEGED_UNKNOWN:
				/* We do not yet know if the client is
				 * privileged or not -> queue message to
				 * be handled when we know.
				 *
				 * Null connection arg => assume the message
				 * is fed from peerinfo_handle_methods() and
				 * must not be queued again. */
				if( connection != 0 ) {
					peerinfo_queue_method(peerinfo, msg);
					break;
				}
				/* fall through */

			default:
			case PRIVILEGED_NO:
				mce_log(LL_WARN, "method %s is reserved for privileged users; denied from: %s",
					member, peerinfo_repr(peerinfo));
				dbus_send_message(dbus_new_error(msg, DBUS_ERROR_AUTH_FAILED,
								 "method %s is reserved for privileged users",
								 member));
				break;

			}
			goto EXIT;

		case DBUS_MESSAGE_TYPE_ERROR:
			if( !mce_dbus_match(member, handler->name) )
				break;

			handler->callback(msg);
			break;

		case DBUS_MESSAGE_TYPE_SIGNAL:
			if( !mce_dbus_match(interface, handler->interface) )
				break;

			if( !mce_dbus_match(member, handler->name) )
				break;

			if( !check_rules(msg, handler->rules) )
				break;

			handler->callback(msg);
			break;

		default:
			mce_log(LL_ERR, "There's a bug somewhere in MCE; something "
				"has registered an invalid D-Bus handler");
			break;
		}
	}

	/* Purge half removed handlers */
	mce_dbus_squeeze_slist(&dbus_handlers);

EXIT:

	mce_wakelock_release("dbus_recv");

	return status;
}

/**
 * Register a D-Bus signal or method handler
 *
 * @param interface The interface to listen on
 * @param name The signal/method call to listen for
 * @param rules Additional matching rules
 * @param type DBUS_MESSAGE_TYPE
 * @param callback The callback function
 * @return A D-Bus handler cookie on success, NULL on failure
 */
static
gconstpointer mce_dbus_handler_add_ex(const gchar *const sender,
				      const gchar *const interface,
				      const gchar *const name,
				      const gchar *const args,
				      const gchar *const rules,
				      const guint type,
				      gboolean (*callback)(DBusMessage *const msg),
				      bool privileged)
{
	handler_struct_t *handler = 0;
	gchar            *match   = 0;

	if( !interface ) {
		mce_log(LL_CRIT, "D-Bus handler must specify interface");
		goto EXIT;
	}

	switch( type ) {
	case DBUS_MESSAGE_TYPE_SIGNAL:
		match = mce_dbus_build_signal_match(sender, interface, name, rules);
		if( !match ) {
			mce_log(LL_CRIT, "Failed to allocate memory for match");
			goto EXIT;
		}
		break;

	case DBUS_MESSAGE_TYPE_METHOD_CALL:
		if( !name ) {
			mce_log(LL_CRIT, "D-Bus method call handler must specify name");
			goto EXIT;
		}
		break;

	default:
		mce_log(LL_CRIT, "There's definitely a programming error somewhere; "
			"MCE is trying to register an invalid message type");
		goto EXIT;
	}

	handler = handler_struct_create();
	handler_struct_set_sender(handler, sender);
	handler_struct_set_type(handler, type);
	handler_struct_set_interface(handler, interface);
	handler_struct_set_name(handler, name);
	handler_struct_set_args(handler, args);
	handler_struct_set_rules(handler, rules);
	handler_struct_set_callback(handler, callback);
	handler_struct_set_privileged(handler, privileged);

	/* Only register D-Bus matches for inbound signals */
	if( match && callback )
		dbus_bus_add_match(dbus_connection, match, 0);

	dbus_handlers = g_slist_prepend(dbus_handlers, handler);

EXIT:
	g_free(match);

	return handler;
}

/**
 * Unregister a D-Bus signal or method handler
 *
 * @param cookie A D-Bus handler cookie for
 *               the handler that should be removed
 */
static
void mce_dbus_handler_remove(gconstpointer cookie)
{
	handler_struct_t *handler = (handler_struct_t *)cookie;
	gchar            *match   = 0;
	GSList           *item    = 0;

	if( !handler )
		goto EXIT;

	if( !(item = g_slist_find(dbus_handlers, handler)) ) {
		mce_log(LL_CRIT, "removing unregistered dbus handler");
	}
	else {
		/* Detach from containing list. The list itself is
		 * not modified so that possible ongoing iteration
		 * is not adversely affected. List cleanup happens
		 * at msg_handler() and mce_dbus_exit().
		 */
		item->data = 0;
	}

	if( handler->type == DBUS_MESSAGE_TYPE_SIGNAL ) {
		match = mce_dbus_build_signal_match(handler->sender,
						    handler->interface,
						    handler->name,
						    handler->rules);

		if( !match ) {
			mce_log(LL_CRIT, "Failed to allocate memory for match");
		}
		else if( dbus_connection_get_is_connected(dbus_connection) ) {
			dbus_bus_remove_match(dbus_connection, match, 0);
		}
	}
	else if( handler->type != DBUS_MESSAGE_TYPE_METHOD_CALL ) {
		mce_log(LL_ERR, "There's definitely a programming error somewhere; "
			"MCE is trying to unregister an invalid message type");
		/* Don't abort here, since we want to unregister it anyway */
	}

	handler_struct_delete(handler);

EXIT:
	g_free(match);
}

/**
 * Unregister a D-Bus signal or method handler;
 * to be used with g_slist_foreach()
 *
 * @param handler A pointer to the handler struct that should be removed
 * @param user_data Unused
 */
static void mce_dbus_handler_remove_cb(gpointer handler, gpointer user_data)
{
	(void)user_data;

	mce_dbus_handler_remove(handler);
}

/* ========================================================================= *
 * OWNER_MONITORING
 * ========================================================================= */

/**
 * Locate the specified D-Bus service in the monitor list
 *
 * @param service The service to check for
 * @param monitor_list The monitor list check
 * @return A pointer to the entry if the entry is in the list,
 *         NULL if the entry is not in the list
 */
static peerquit_t *
find_monitored_service(const gchar *service, GSList *monitor_list)
{
    peerquit_t *hit = 0;

    if( !service )
	goto EXIT;

    for( GSList *item = monitor_list; item; item = item->next ) {
	peerquit_t *quit = item->data;

	if( !quit )
	    continue;

	peerinfo_t *info = quit->pq_peerinfo;

	if( strcmp(peerinfo_name(info), service) )
	    continue;

	hit = quit;
	break;
    }

EXIT:
    return hit;
}

/**
 * Add a service to a D-Bus owner monitor list
 *
 * @param service The service to monitor
 * @param callback A D-Bus monitor callback
 * @param monitor_list The list of monitored services
 * @param max_num The maximum number of monitored services;
 *                keep this number low, for performance
 *                and memory usage reasons
 * @return -1 if the amount of monitored services would be exceeded;
 *            if either of service or monitor_list is NULL,
 *            or if adding a D-Bus monitor fails
 *          0 if the service is already monitored
 *         >0 represents the number of monitored services after adding
 *            this service
 */
gssize mce_dbus_owner_monitor_add(const gchar *service,
				  gboolean (*callback)(DBusMessage *const msg),
				  GSList **monitor_list,
				  gssize max_num)
{
    gssize retval = -1;
    gssize num;

    if( !service ) {
	mce_log(LL_CRIT, "A programming error occured; "
		"mce_dbus_owner_monitor_add() called with "
		"service == NULL");
	goto EXIT;
    }

    if( !monitor_list ) {
	mce_log(LL_CRIT, "A programming error occured; "
		"mce_dbus_owner_monitor_add() called with "
		"monitor_list == NULL");
	goto EXIT;
    }

    /* If the service is already in the list, we're done */
    if( find_monitored_service(service, *monitor_list) ) {
	retval = 0;
	goto EXIT;
    }

    /* If the service isn't in the list, and the list already
     * contains max_num elements, bail out
     */
    if( (num = g_slist_length(*monitor_list)) >= max_num )
	goto EXIT;

    /* Attach callback to peerinfo tracker */
    peerinfo_t *info = mce_dbus_add_peerinfo(service);
    peerquit_t *quit = peerinfo_add_quit_callback(info, callback);
    *monitor_list = g_slist_prepend(*monitor_list, quit);
    retval = num + 1;

EXIT:

    return retval;
}

/**
 * Remove a service from a D-Bus owner monitor list
 *
 * @param service The service to remove from the monitor list
 * @param monitor_list The monitor list to remove the service from
 * @return The new number of monitored connections;
 *         -1 if the service was not monitored,
 *            if removing monitoring failed,
 *            or if either of service or monitor_list is NULL
 */
gssize mce_dbus_owner_monitor_remove(const gchar *service,
				     GSList **monitor_list)
{
    gssize retval = -1;

    if( !service || !monitor_list )
	goto EXIT;

    peerquit_t *quit = find_monitored_service(service, *monitor_list);

    if( !quit )
	goto EXIT;

    peerinfo_t *info = quit->pq_peerinfo;

    *monitor_list = g_slist_remove(*monitor_list, quit);
    peerinfo_remove_quit_callback(info, quit);
    retval = g_slist_length(*monitor_list);

EXIT:
    return retval;
}

/**
 * Remove all monitored service from a D-Bus owner monitor list
 *
 * @param monitor_list The monitor list to remove the service from
 */
void mce_dbus_owner_monitor_remove_all(GSList **monitor_list)
{
    if( !monitor_list )
	goto EXIT;

    for( GSList *item = *monitor_list; item; item = item->next ) {
	peerquit_t *quit = item->data;

	if( !quit )
	    continue;

	item->data = 0;

	peerinfo_t *info = quit->pq_peerinfo;
	peerinfo_remove_quit_callback(info, quit);
    }
    g_slist_free(*monitor_list);
    *monitor_list = 0;

EXIT:
    return;
}

/** Start tracking state of a D-Bus name
 *
 * @param name      D-Bus name
 * @param callback  Notification function to call on state change
 * @param userdata  A pointer that should be passed to notification callback
 * @param userfree  free() like function to call on userdata on cleanup
 */
void
mce_dbus_name_tracker_add(const char      *name,
			  peernotify_fn   callback,
			  gpointer        userdata,
			  GDestroyNotify  userfree)
{
    peerinfo_t *info = mce_dbus_add_peerinfo(name);
    if( info )
	peerinfo_add_notify_callback(info, callback, userdata, userfree);
}

/** Stop tracking state of a D-Bus name
 *
 * @param name      D-Bus name
 * @param callback  Notification function to call on state change
 * @param userdata  A pointer that should be passed to notification callback
 */
void
mce_dbus_name_tracker_remove(const char     *name,
			     peernotify_fn   callback,
			     gpointer        userdata)
{
    peerinfo_t *info = mce_dbus_get_peerinfo(name);
    if( info )
	peerinfo_remove_notify_callback(info, callback, userdata);
}

/* ========================================================================= *
 * SERVICE_MANAGEMENT
 * ========================================================================= */

/**
 * Acquire D-Bus services
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbus_acquire_services(void)
{
	gboolean status = FALSE;
	DBusError error = DBUS_ERROR_INIT;
	int ret;

	ret = dbus_bus_request_name(dbus_connection, MCE_SERVICE, 0, &error);

	if( ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ) {
		mce_log(LL_CRIT, "Cannot acquire service %s: %s: %s",
			MCE_SERVICE, error.name, error.message);
		goto EXIT;
	}

	mce_log(LL_DEBUG, "Service %s acquired", MCE_SERVICE);

	status = TRUE;

EXIT:
	dbus_error_free(&error);
	return status;
}

/** Remove the message handler used by MCE
 */
static void dbus_quit_message_handler(void)
{
	if( dbus_connection ) {
		dbus_connection_remove_filter(dbus_connection,
					      msg_handler, 0);
	}
}

/**
 * Initialise the message handler used by MCE
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbus_init_message_handler(void)
{
	gboolean status = FALSE;

	if (dbus_connection_add_filter(dbus_connection, msg_handler,
				       NULL, NULL) == FALSE) {
		mce_log(LL_CRIT, "Failed to add D-Bus filter");
		goto EXIT;
	}

	status = TRUE;

EXIT:
	return status;
}

/* ========================================================================= *
 * MESSAGE_ITER
 * ========================================================================= */

/** Get dbus data type to human readable form
 *
 * @param type type id (DBUS_TYPE_BOOLEAN etc)
 *
 * @return name of the type, without the DBUS_TYPE_ prefix
 */
const char *
mce_dbus_type_repr(int type)
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

/** End of iterator reached predicate
 *
 * @param iter dbus message iterator
 *
 * @return true if no more iterms can be read, false otherwise
 */
bool
mce_dbus_iter_at_end(DBusMessageIter *iter)
{
	return dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_INVALID;
}

/** Iterator points to expected data type predicate
 *
 * If not, error diagnostic is emitted
 *
 * @param iter dbus message iterator
 * @param want dbus type id
 *
 * @return true if iterator points to expected type, false otherwise
 */
bool
mce_dbus_iter_req_type(DBusMessageIter *iter, int want)
{
	int have = dbus_message_iter_get_arg_type(iter);

	if( have == want ) {
		return true;
	}

	mce_log(LL_ERR, "expected: %s, got: %s",
		mce_dbus_type_repr(want),
		mce_dbus_type_repr(have));

	return false;
}

/** Iterator points to expected data type predicate
 *
 * If not, error diagnostic is emitted
 *
 * @param iter dbus message iterator
 * @param want dbus type id
 *
 * @return true if iterator points to expected type, false otherwise
 */
bool
mce_dbus_iter_get_basic(DBusMessageIter *iter, void *pval, int type)
{
	if( !dbus_type_is_basic(type) ) {
		mce_log(LL_ERR, "%s: is not basic dbus type",
			mce_dbus_type_repr(type));
		return false;
	}

	if( !mce_dbus_iter_req_type(iter, type) )
		return false;

	dbus_message_iter_get_basic(iter, pval);
	dbus_message_iter_next(iter);
	return true;
}

/** Get object path string from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param pval where to store the result
 *
 * @return true if *pval was modified, false otherwise
 */
bool
mce_dbus_iter_get_object(DBusMessageIter *iter, const char **pval)
{
	return mce_dbus_iter_get_basic(iter, pval, DBUS_TYPE_OBJECT_PATH);
}

/** Get string from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param pval where to store the result
 *
 * @return true if *pval was modified, false otherwise
 */
bool
mce_dbus_iter_get_string(DBusMessageIter *iter, const char **pval)
{
	return mce_dbus_iter_get_basic(iter, pval, DBUS_TYPE_STRING);
}

/** Get bool from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param pval where to store the result
 *
 * @return true if *pval was modified, false otherwise
 */
bool
mce_dbus_iter_get_bool(DBusMessageIter *iter, bool *pval)
{
	dbus_bool_t val = 0;
	bool        res = mce_dbus_iter_get_basic(iter, &val,
						  DBUS_TYPE_BOOLEAN);
	if( res )
		*pval = (val != 0);
	return res;
}

/** Get int32 from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param pval where to store the result
 *
 * @return true if *pval was modified, false otherwise
 */
bool
mce_dbus_iter_get_int32(DBusMessageIter *iter, dbus_int32_t *pval)
{
	return mce_dbus_iter_get_basic(iter, pval, DBUS_TYPE_INT32);
}

/** Get uint32 from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param pval where to store the result
 *
 * @return true if *pval was modified, false otherwise
 */
bool
mce_dbus_iter_get_uint32(DBusMessageIter *iter, dbus_uint32_t *pval)
{
	return mce_dbus_iter_get_basic(iter, pval, DBUS_TYPE_UINT32);
}

/** Get sub iterator from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param sub dbus message iterator
 * @param type expected container type id
 *
 * @return true if sub was modified, false otherwise
 */
static bool
mce_dbus_iter_get_container(DBusMessageIter *iter, DBusMessageIter *sub,
			    int type)
{
	if( !dbus_type_is_container(type) ) {
		mce_log(LL_ERR, "%s: is not container dbus type",
			mce_dbus_type_repr(type));
		return false;
	}

	if( !mce_dbus_iter_req_type(iter, type) )
		return false;

	dbus_message_iter_recurse(iter, sub);
	dbus_message_iter_next(iter);
	return true;
}

/** Get array sub iterator from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param sub dbus message iterator
 *
 * @return true if sub was modified, false otherwise
 */
bool
mce_dbus_iter_get_array(DBusMessageIter *iter, DBusMessageIter *sub)
{
	return mce_dbus_iter_get_container(iter, sub, DBUS_TYPE_ARRAY);
}

/** Get struct sub iterator from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param sub dbus message iterator
 *
 * @return true if sub was modified, false otherwise
 */
bool
mce_dbus_iter_get_struct(DBusMessageIter *iter, DBusMessageIter *sub)
{
	return mce_dbus_iter_get_container(iter, sub, DBUS_TYPE_STRUCT);
}

/** Get dict entry sub iterator from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param sub dbus message iterator
 *
 * @return true if sub was modified, false otherwise
 */
bool
mce_dbus_iter_get_entry(DBusMessageIter *iter, DBusMessageIter *sub)
{
	return mce_dbus_iter_get_container(iter, sub, DBUS_TYPE_DICT_ENTRY);
}

/** Get variant sub iterator from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param sub dbus message iterator
 *
 * @return true if sub was modified, false otherwise
 */
bool
mce_dbus_iter_get_variant(DBusMessageIter *iter, DBusMessageIter *sub)
{
	return mce_dbus_iter_get_container(iter, sub, DBUS_TYPE_VARIANT);
}

/** Register D-Bus message handler
 *
 * @param self handler data
 */
void
mce_dbus_handler_register(mce_dbus_handler_t *self)
{
	if( !self->cookie ) {
		self->cookie = mce_dbus_handler_add_ex(self->sender,
						       self->interface,
						       self->name,
						       self->args,
						       self->rules,
						       self->type,
						       self->callback,
						       self->privileged);
		if( !self->cookie )
			mce_log(LL_ERR, "%s.%s: failed to add handler",
				self->interface, self->name);
	}
}

/** Unregister D-Bus message handler
 *
 * @param self handler data
 */
void
mce_dbus_handler_unregister(mce_dbus_handler_t *self)
{
	if( self->cookie )
		mce_dbus_handler_remove(self->cookie), self->cookie = 0;
}

/** Register an array of D-Bus message handlers
 *
 * @param array handler data array, terminated with .interface=NULL
 */
void
mce_dbus_handler_register_array(mce_dbus_handler_t *array)
{
	for( ; array && array->interface; ++array )
		mce_dbus_handler_register(array);
}

/** Unregister an array of D-Bus message handlers
 *
 * @param array handler data array, terminated with .interface=NULL
 */
void
mce_dbus_handler_unregister_array(mce_dbus_handler_t *array)
{
	for( ; array && array->interface; ++array )
		mce_dbus_handler_unregister(array);
}

/* ========================================================================= *
 * PEER_IDENTITY
 * ========================================================================= */

/** Get identification string of D-Bus name owner
 */
const char *mce_dbus_get_name_owner_ident(const char *name)
{
	const char *res = mce_dbus_get_peerdesc(name);
	return res ?: "unknown";
}

/** Get identification string of sender of D-Bus message
 */
const char *mce_dbus_get_message_sender_ident(DBusMessage *msg)
{
	const char *name = 0;
	if( msg )
		name = dbus_message_get_sender(msg);
	return mce_dbus_get_name_owner_ident(name);
}

/* ========================================================================= *
 * INTROSPECT_SUPPORT
 * ========================================================================= */

/** Format string for Introspect XML prologue */
#define INTROSPECT_PROLOG_FMT \
"<!DOCTYPE node PUBLIC" \
" \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"" \
" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n" \
"<node name=\"%s\">\n"

/** Format string for Introspect XML epilogue */
#define INTROSPECT_EPILOG_FMT \
"</node>\n"

/** Emit Introspect XML for registered method call handlers
 */
static void introspect_add_methods(FILE *file, const char *interface)
{
	fprintf(file, "<interface name=\"%s\">\n", interface);
	for( GSList *now = dbus_handlers; now; now = now->next ) {

		handler_struct_t *handler = now->data;

		/* Skip half removed handlers */
		if( !handler )
			continue;

		/* Skip not applicable handlers */
		if( handler->type != DBUS_MESSAGE_TYPE_METHOD_CALL )
			continue;
		if( !handler->interface || !handler->name )
			continue;
		if( strcmp(handler->interface, interface) )
			continue;

		fprintf(file, "  <method name=\"%s\">\n", handler->name);
		if( handler->args )
			fprintf(file, "%s", handler->args);
		else
			fprintf(file, "    <!-- NOT DEFINED -->\n");
		fprintf(file, "  </method>\n");
	}
	fprintf(file, "</interface>\n");
}

/** Emit Introspect XML for registered outbound signals
 */
static void introspect_add_signals(FILE *file, const char *interface)
{
	fprintf(file, "<interface name=\"%s\">\n", interface);
	for( GSList *now = dbus_handlers; now; now = now->next ) {

		handler_struct_t *handler = now->data;

		/* Skip half removed handlers */
		if( !handler )
			continue;

		/* Skip not applicable handlers */
		if( handler->type != DBUS_MESSAGE_TYPE_SIGNAL )
			continue;
		if( handler->callback )
			continue;
		if( !handler->interface || !handler->name )
			continue;
		if( strcmp(handler->interface, interface) )
			continue;

		fprintf(file, "  <signal name=\"%s\">\n", handler->name);
		if( handler->args )
			fprintf(file, "%s", handler->args);
		else
			fprintf(file, "    <!-- NOT DEFINED -->\n");
		fprintf(file, "  </signal>\n");
	}
	fprintf(file, "</interface>\n");
}

/** Emit Introspect XML for standard Introspectable and Peer interfaces
 */
static void introspect_add_defaults(FILE *file)
{
	/* Custom handler for Introspect() is provided */
	fprintf(file, "%s",
		"  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
		"    <method name=\"Introspect\">\n"
		"      <arg direction=\"out\" name=\"data\" type=\"s\"/>\n"
		"    </method>\n"
		"  </interface>\n");

	/* The Ping() and GetMachineId() method calls are implicitly
	 * handled in libdbus as long as dbus config for mce allows
	 * them to be made */
	fprintf(file, "%s",
		"  <interface name=\"org.freedesktop.DBus.Peer\">\n"
		"    <method name=\"Ping\"/>\n"
		"    <method name=\"GetMachineId\">\n"
		"      <arg direction=\"out\" name=\"machine_uuid\" type=\"s\" />\n"
		"    </method>\n"
		"  </interface>\n");

}

/** Check if outbound D-Bus signal has been registered for Introspection
 */
static bool introspectable_signal(const char *interface, const char *member)
{
	for( GSList *now = dbus_handlers; now; now = now->next ) {

		handler_struct_t *handler = now->data;

		/* Skip half removed handlers */
		if( !handler )
			continue;

		/* Needs to be signal handler without a callback function */
		if( handler->callback )
			continue;
		if( handler->type != DBUS_MESSAGE_TYPE_SIGNAL )
			continue;
		if( !handler->interface || !handler->name )
			continue;
		if( strcmp(handler->interface, interface) )
			continue;
		if( strcmp(handler->name, member) )
			continue;

		return true;
	}
	return false;
}

static void introspect_com_nokia_mce_request(FILE *file)
{
	introspect_add_methods(file, MCE_REQUEST_IF);
}

static void introspect_com_nokia_mce_signal(FILE *file)
{
	introspect_add_signals(file, MCE_SIGNAL_IF);
}

static void introspect_com_nokia_mce(FILE *file)
{
	fprintf(file, "  <node name=\"request\"/>\n");
	fprintf(file, "  <node name=\"signal\"/>\n");
}

static void introspect_com_nokia(FILE *file)
{
	fprintf(file, "  <node name=\"mce\"/>\n");
}

static void introspect_com(FILE *file)
{
	fprintf(file, "  <node name=\"nokia\"/>\n");
}

static void introspect_root(FILE *file)
{
	fprintf(file, "  <node name=\"com\"/>\n");
}

static const struct
{
	const char *path;
	void (*func)(FILE *);
} introspect_lut[] =
{
	{ "/",                      introspect_root                  },
	{ "/com",                   introspect_com                   },
	{ "/com/nokia",             introspect_com_nokia             },
	{ "/com/nokia/mce",         introspect_com_nokia_mce         },
	{ "/com/nokia/mce/request", introspect_com_nokia_mce_request },
	{ "/com/nokia/mce/signal",  introspect_com_nokia_mce_signal  },
	{ 0, 0 }
};

/** D-Bus callback for org.freedesktop.DBus.Introspectable.Introspect
 *
 * @param msg The D-Bus message to reply to
 *
 * @return TRUE
 */
static gboolean introspect_dbus_cb(DBusMessage *const req)
{
	DBusMessage *rsp  = NULL;
	FILE        *file = 0;
	char        *data = 0;
	size_t       size = 0;

	mce_log(LL_DEBUG, "Received introspect request");

	const char  *path = dbus_message_get_path(req);

	if( !path ) {
		/* Should not really be possible, but ... */
		rsp = dbus_new_error(req, DBUS_ERROR_INVALID_ARGS,
				     "object path not specified");
		goto EXIT;
	}

	if( !(file = open_memstream(&data, &size)) )
		goto EXIT;

	for( size_t i = 0; ; ++i ) {
		if( introspect_lut[i].path == 0 ) {
			rsp = dbus_new_error(req, DBUS_ERROR_UNKNOWN_OBJECT,
					     "%s is not a valid object path",
					     path);
			goto EXIT;
		}
		if( !strcmp(introspect_lut[i].path, path) ) {
			fprintf(file, INTROSPECT_PROLOG_FMT, path);
			introspect_add_defaults(file);
			introspect_lut[i].func(file);
			fprintf(file, INTROSPECT_EPILOG_FMT);
			break;
		}
	}

	// the 'data' pointer gets updated at fclose
	fclose(file), file = 0;

	if( !data ) {
		rsp = dbus_new_error(req, DBUS_ERROR_FAILED,
				     "failed to generate introspect xml data");
		goto EXIT;
	}

	/* Create a reply */
	rsp = dbus_new_method_reply(req);

	if( !dbus_message_append_args(rsp,
				      DBUS_TYPE_STRING, &data,
				      DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "Failed to append reply argument to D-Bus"
			" message for %s.%s",
			DBUS_INTERFACE_INTROSPECTABLE,
			"Introspect");
	}

EXIT:
	if( file ) fclose(file);
	free(data);
	if( rsp ) dbus_send_message(rsp);

	return TRUE;
}

/* ========================================================================= *
 * DBUS_NAME_OWNER_TRACKING
 * ========================================================================= */

/** Get current name owner for tracked services
 *
 * @param name Well known D-Bus name
 *
 * @return owner name or NULL
 */
const char *
mce_dbus_nameowner_get(const char *name)
{
    const char *owner = 0;

    peerinfo_t *info = mce_dbus_get_peerinfo(name);
    if( info )
	owner = peerinfo_get_owner_name(info);

    return owner;
}

/** Create a match rule and add it to D-Bus daemon side
 *
 * Use mce_dbus_nameowner_unwatch() to cancel.
 *
 * @param name D-Bus name that changed owner
 *
 * @return rule that was sent to the D-Bus daemon
 */
static gchar *
mce_dbus_nameowner_watch(const char *name)
{
    static const char fmt[] =
	"type='signal'"
	",interface='"DBUS_INTERFACE_DBUS"'"
	",member='NameOwnerChanged'"
	",arg0='%s'";

    gchar *rule = g_strdup_printf(fmt, name);
    dbus_bus_add_match(dbus_connection, rule, 0);
    return rule;
}

/** Remove a match rule from D-Bus daemon side and free it
 *
 * @param rule obtained from mce_dbus_nameowner_watch()
 */
static void mce_dbus_nameowner_unwatch(gchar *rule)
{
    if( rule ) {
	dbus_bus_remove_match(dbus_connection, rule, 0);
	g_free(rule);
    }
}

/* ========================================================================= *
 * MODULE_INIT_QUIT
 * ========================================================================= */

/** Array of dbus message handlers */
static mce_dbus_handler_t mce_dbus_handlers[] =
{
	/* Outbound signals (for Introspect support only) */
	{
		.interface = MCE_SIGNAL_IF,
		.name      = MCE_CONFIG_CHANGE_SIG,
		.type      = DBUS_MESSAGE_TYPE_SIGNAL,
		.args      =
			"    <arg name=\"key_name\" type=\"s\"/>\n"
			"    <arg name=\"key_value\" type=\"v\"/>\n"
	},
	/* method calls */
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_VERSION_GET,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = version_get_dbus_cb,
		.args      =
			"    <arg direction=\"out\" name=\"version\" type=\"s\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_CONFIG_GET,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = config_get_dbus_cb,
		.args      =
			"    <arg direction=\"in\" name=\"key_name\" type=\"s\"/>\n"
			"    <arg direction=\"out\" name=\"key_value\" type=\"v\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_CONFIG_GET_ALL,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = config_get_all_dbus_cb,
		.args      =
			"    <arg direction=\"out\" name=\"values\" type=\"a{sv}\"/>\n"
			"    <annotation name=\"org.qtproject.QtDBus.QtTypeName.Out0\" value=\"QVariantMap\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_CONFIG_SET,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = config_set_dbus_cb,
		.args      =
			"    <arg direction=\"in\" name=\"key_name\" type=\"s\"/>\n"
			"    <arg direction=\"in\" name=\"key_value\" type=\"v\"/>\n"
			"    <arg direction=\"out\" name=\"success\" type=\"b\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_CONFIG_RESET,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = config_reset_dbus_cb,
		.args      =
			"    <arg direction=\"in\" name=\"key_part\" type=\"s\"/>\n"
			"    <arg direction=\"out\" name=\"count\" type=\"i\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_SUSPEND_STATS_GET,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = suspend_stats_get_dbus_cb,
		.args      =
			"    <arg direction=\"out\" name=\"uptime_ms\" type=\"x\"/>\n"
			"    <arg direction=\"out\" name=\"suspend_ms\" type=\"x\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_VERBOSITY_GET,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = verbosity_get_dbus_cb,
		.args      =
			"    <arg direction=\"out\" name=\"level\" type=\"i\"/>\n"
	},
	{
		.interface = MCE_REQUEST_IF,
		.name      = MCE_VERBOSITY_REQ,
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = verbosity_set_dbus_cb,
		.args      =
			"    <arg direction=\"in\" name=\"level\" type=\"i\"/>\n"
	},
	{
		.interface = DBUS_INTERFACE_INTROSPECTABLE,
		.name      = "Introspect",
		.type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
		.callback  = introspect_dbus_cb,
		.args      =
			"    <arg direction=\"out\" name=\"xml_data\" type=\"s\"/>\n"
	},
	/* sentinel */
	{
		.interface = 0
	}
};

/** Error string to use when abnormal dbus connects are detected */
static const char mce_dbus_init_bypass[] =
	"attempt to bypass mce dbus connection handling";

/** Flag for: DBus connect attempt is expected */
static bool mce_dbus_init_called = false;

/** Intercept libdbus dbus_bus_get() calls
 */
DBusConnection *
dbus_bus_get (DBusBusType type, DBusError *err)
{
	(void)type;
	dbus_set_error_const(err, DBUS_ERROR_NO_SERVER, mce_dbus_init_bypass);
	mce_log(LL_CRIT, "%s", mce_dbus_init_bypass);

	/* Note: getpid() never fails, it is used as a dummy test to keep
	 *       the compiler happy even though we never return from here. */
	if( getpid() != -1 )
		mce_abort();
	return 0;
}

/** Intercept libdbus dbus_bus_get_private() calls
 */
DBusConnection *
dbus_bus_get_private(DBusBusType type, DBusError *err)
{
	static DBusConnection *(*real)(DBusBusType, DBusError*) = 0;

	if( !real ) {
		if( !(real = dlsym(RTLD_NEXT, __FUNCTION__)) )
			mce_abort();
	}

	DBusConnection *con = 0;

	if( !mce_dbus_init_called || dbus_connection ) {
		dbus_set_error_const(err, DBUS_ERROR_NO_SERVER,
				     mce_dbus_init_bypass);
		mce_log(LL_CRIT, "%s", mce_dbus_init_bypass);
		mce_abort();
	}
	else {
		con = real(type, err);
	}

	return con;
}

/** Lookup id for "privileged" user
 */
static void
mce_dbus_init_privileged_uid(void)
{
    char  *data = 0;
    size_t size = sysconf(_SC_GETPW_R_SIZE_MAX);

    if( size < 0x1000 )
	size = 0x1000;

    if( !(data = malloc(size)) )
	goto EXIT;

    struct passwd pwd;
    struct passwd *pw = 0;

    if( getpwnam_r("privileged", &pwd, data, size, &pw) != 0 )
	goto EXIT;

    if( !pw )
	goto EXIT;

    mce_dbus_privileged_uid = pw->pw_uid;

EXIT:
    mce_log(LL_DEBUG, "privileged uid = %d", (int)mce_dbus_privileged_uid);

    free(data);
}

/** Lookup id for "privileged" group
 */
static void
mce_dbus_init_privileged_gid(void)
{
    char *data = 0;
    long  size = sysconf(_SC_GETGR_R_SIZE_MAX);

    if( size < 0x1000 )
	size = 0x1000;

    if( !(data = malloc(size)) )
	goto EXIT;

    struct group grp;
    struct group *gr = 0;

    if( getgrnam_r("privileged", &grp, data, size, &gr) != 0 )
	goto EXIT;

    if( !gr )
	goto EXIT;

    mce_dbus_privileged_gid = gr->gr_gid;

EXIT:
    mce_log(LL_DEBUG, "privileged gid = %d", (int)mce_dbus_privileged_gid);

    free(data);
}

/**
 * Init function for the mce-dbus component
 * Pre-requisites: glib mainloop registered
 *
 * Note: This is the only function that is allowed to make a D-Bus
 *       connection in context of the whole MCE process.
 *
 * @param systembus TRUE to use system bus, FALSE to use session bus
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_dbus_init(const gboolean systembus)
{
	gboolean    status   = FALSE;
	DBusBusType bus_type = DBUS_BUS_SYSTEM;
	DBusError   error    = DBUS_ERROR_INIT;

	mce_dbus_init_privileged_uid();
	mce_dbus_init_privileged_gid();

	if( !systembus )
		bus_type = DBUS_BUS_SESSION;

	mce_log(LL_DEBUG, "Establishing D-Bus connection");

	/* Establish D-Bus connection */
	mce_dbus_init_called = true;
	dbus_connection = dbus_bus_get_private(bus_type, &error);
	mce_dbus_init_called = false;

	if( !dbus_connection ) {
		mce_log(LL_CRIT, "DBus connect failed; %s",
			error.message);
		goto EXIT;
	}

	mce_log(LL_DEBUG, "Connecting D-Bus to the mainloop");

	/* Connect D-Bus to the mainloop */
	dbus_gmain_set_up_connection(dbus_connection, NULL);

	mce_log(LL_DEBUG, "Acquiring D-Bus service");

	/* Acquire D-Bus service */
	if( !dbus_acquire_services() )
		goto EXIT;

	/* Initialise message handlers */
	if( !dbus_init_message_handler() )
		goto EXIT;

	/* Start tracking essential services */
	mce_dbus_init_peerinfo();

	/* Register callbacks that are handled inside mce-dbus.c */
	mce_dbus_handler_register_array(mce_dbus_handlers);

	status = TRUE;

EXIT:
	dbus_error_free(&error);
	return status;
}

/**
 * Exit function for the mce-dbus component
 */
void mce_dbus_exit(void)
{
	/* Stop tracking services */
	mce_dbus_quit_peerinfo();

	/* Remove message handlers */
	dbus_quit_message_handler();

	/* Unregister callbacks that are handled inside mce-dbus.c */
	mce_dbus_handler_unregister_array(mce_dbus_handlers);

	/* Unregister remaining D-Bus handlers */
	if( dbus_handlers ) {
		g_slist_foreach(dbus_handlers, mce_dbus_handler_remove_cb, 0);
		g_slist_free(dbus_handlers);
		dbus_handlers = 0;
	}

	/* Disconnect from D-Bus */
	if (dbus_connection != NULL) {
		mce_log(LL_DEBUG, "closing dbus connection");
		dbus_connection_close(dbus_connection);
		dbus_connection_unref(dbus_connection),
			dbus_connection = NULL;
	}

	/* When debugging, tell libdbus to release all dynamic resouces */
	if( mce_in_valgrind_mode() ) {
		mce_log(LL_WARN, "dbus shutdown");
		dbus_shutdown();
	}

	return;
}
