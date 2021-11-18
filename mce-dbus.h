/**
 * @file mce-dbus.h
 * Headers for the D-Bus handling code for the Mode Control Entity
 * <p>
 * Copyright © 2004-2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2013-2019 Jolla Ltd.
 * Copyright (c) 2019 Open Mobile Platform LLC.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
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
#ifndef  MCE_DBUS_H_
# define MCE_DBUS_H_

# include "builtin-gconf.h"

# include <unistd.h>

# include <dbus/dbus.h>

/* ========================================================================= *
 * NON-PUBLIC mce dbus method calls
 * ========================================================================= */

# ifdef ENABLE_BATTERY_SIMULATION
/** Override current charger state
 *
 * Available in devel flavor mce only, and only to privileged applications.
 *
 * @since mce 1.99.0
 *
 * @param string: current charger state, one of:
 * - #MCE_CHARGER_STATE_UNKNOWN
 * - #MCE_CHARGER_STATE_ON
 * - #MCE_CHARGER_STATE_OFF
 *
 * @return boolean true if accepted, false / error reply otherwise
 */
#  define MCE_CHARGER_STATE_REQ                   "req_charger_state"

/** Override current charger type
 *
 * Available in devel flavor mce only, and only to privileged applications.
 *
 * @since mce 1.102.0
 *
 * @param string: current charger type, one of:
 * - #MCE_CHARGER_TYPE_NONE
 * - #MCE_CHARGER_TYPE_USB
 * - #MCE_CHARGER_TYPE_DCP
 * - #MCE_CHARGER_TYPE_HVDCP
 * - #MCE_CHARGER_TYPE_CDP
 * - #MCE_CHARGER_TYPE_WIRELESS
 * - #MCE_CHARGER_TYPE_OTHER
 *
 * @return boolean true if accepted, false / error reply otherwise
 */
#  define MCE_CHARGER_TYPE_REQ                   "req_charger_type"

/** Override current battery level
 *
 * Available in devel flavor mce only, and only to privileged applications.
 *
 * @since mce 1.99.0
 *
 * @param int32: battery level percent, or #MCE_BATTERY_LEVEL_UNKNOWN
 *
 * @return boolean true if accepted, false / error reply otherwise
 */
#  define MCE_BATTERY_LEVEL_REQ                   "req_battery_level"
# endif // ENABLE_BATTERY_SIMULATION

/* ========================================================================= *
 * DSME DBUS SERVICE
 * ========================================================================= */

/** Well known dbus name of dsme service */
# define DSME_DBUS_SERVICE                        "com.nokia.dsme"

/* ========================================================================= *
 * COMPOSITOR DBUS SERVICE
 * ========================================================================= */

/** Well known dbus name of compositor service */
# define COMPOSITOR_SERVICE                       "org.nemomobile.compositor"
# define COMPOSITOR_PATH                          "/"
# define COMPOSITOR_IFACE                         "org.nemomobile.compositor"

/* Enabling/disabling display updates via compositor service */
# define COMPOSITOR_SET_UPDATES_ENABLED           "setUpdatesEnabled"

/** Query owner of topmost ui window */
# define COMPOSITOR_GET_TOPMOST_WINDOW_PID        "privateTopmostWindowProcessId"

/** Change notification for owner of topmost ui window */
# define COMPOSITOR_TOPMOST_WINDOW_PID_CHANGED    "privateTopmostWindowProcessIdChanged"

/* ========================================================================= *
 * LIPSTICK DBUS SERVICE
 * ========================================================================= */

/** Well known dbus name of systemui service */
# define LIPSTICK_SERVICE                         "org.nemomobile.lipstick"
# define LIPSTICK_PATH                            "/"
# define LIPSTICK_IFACE                           "org.nemomobile.lipstick"

/* ========================================================================= *
 * USB_MODED DBUS SERVICE
 * ========================================================================= */

/** Well known service name for usb_moded service */
# define USB_MODED_DBUS_SERVICE                   "com.meego.usb_moded"

/** D-Bus interface name for usb_moded */
# define USB_MODED_DBUS_INTERFACE                 "com.meego.usb_moded"

/** D-Bus object name for usb_moded */
# define USB_MODED_DBUS_OBJECT                    "/com/meego/usb_moded"

/** Query current usb mode method call */
# define USB_MODED_QUERY_MODE_REQ                 "mode_request"

/** Current usb mode changed signal */
# define USB_MODED_MODE_CHANGED_SIG               "sig_usb_state_ind"

/* ========================================================================= *
 * FINGERPRINT_DAEMON_DBUS_SERVICE (API Version 1)
 * ========================================================================= */

# define FINGERPRINT1_DBUS_SERVICE               "org.sailfishos.fingerprint1"

# define FINGERPRINT1_DBUS_ROOT_OBJECT           "/org/sailfishos/fingerprint1"

# define FINGERPRINT1_DBUS_INTERFACE             "org.sailfishos.fingerprint1"

# define FINGERPRINT1_DBUS_REQ_ENROLL            "Enroll"
# define FINGERPRINT1_DBUS_REQ_IDENTIFY          "Identify"
# define FINGERPRINT1_DBUS_REQ_ABORT             "Abort"
# define FINGERPRINT1_DBUS_REQ_GET_ALL           "GetAll"
# define FINGERPRINT1_DBUS_REQ_REMOVE            "Remove"
# define FINGERPRINT1_DBUS_REQ_VERIFY            "Verify"
# define FINGERPRINT1_DBUS_REQ_GET_STATE         "GetState"

# define FINGERPRINT1_DBUS_SIG_ADDED             "Added"
# define FINGERPRINT1_DBUS_SIG_REMOVED           "Removed"
# define FINGERPRINT1_DBUS_SIG_IDENTIFIED        "Identified"
# define FINGERPRINT1_DBUS_SIG_ABORTED           "Aborted"
# define FINGERPRINT1_DBUS_SIG_FAILED            "Failed"
# define FINGERPRINT1_DBUS_SIG_VERIFIED          "Verified"

# define FINGERPRINT1_DBUS_SIG_STATE_CHANGED     "StateChanged"
# define FINGERPRINT1_DBUS_SIG_ERROR_INFO        "ErrorInfo"
# define FINGERPRINT1_DBUS_SIG_ACQUISITION_INFO  "AcquisitionInfo"
# define FINGERPRINT1_DBUS_SIG_ENROLL_PROGRESS   "EnrollProgressChanged"

/* ========================================================================= *
 * D-Bus connection and message handling
 * ========================================================================= */

/** Placeholder for any basic dbus data type */
typedef union
{
    dbus_int16_t   i16;
    dbus_int32_t   i32;
    dbus_int64_t   i64;

    dbus_uint16_t  u16;
    dbus_uint32_t  u32;
    dbus_uint64_t  u64;

    dbus_bool_t    b;
    unsigned char  o;
    const char    *s;
    double         d;

} dbus_any_t;

/** D-Bus message handler data
 *
 * For use with mce_dbus_handler_register() etc
 */
typedef struct
{
    const char *sender;
    const char *interface;
    const char *name;
    const char *rules;
    const char *args;
    int         type;
    gboolean  (*callback)(DBusMessage *const msg);
    bool        privileged;

    gconstpointer cookie;
} mce_dbus_handler_t;

DBusConnection *dbus_connection_get(void);

DBusMessage *dbus_new_signal(const gchar *const path,
                             const gchar *const interface,
                             const gchar *const name);

DBusMessage *dbus_new_method_call(const gchar *const service,
                                  const gchar *const path,
                                  const gchar *const interface,
                                  const gchar *const name);

DBusMessage *dbus_new_method_reply(DBusMessage *const message);

gboolean dbus_send_message(DBusMessage *const msg);

gboolean dbus_send(const gchar *const service, const gchar *const path,
                   const gchar *const interface, const gchar *const name,
                   DBusPendingCallNotifyFunction callback,
                   int first_arg_type, ...);

gboolean dbus_send_ex(const char *service,
                      const char *path,
                      const char *interface,
                      const char *name,
                      DBusPendingCallNotifyFunction callback,
                      void *user_data, DBusFreeFunction user_free,
                      DBusPendingCall **ppc,
                      int first_arg_type, ...);

gboolean dbus_send_ex2(const char *service,
                       const char *path,
                       const char *interface,
                       const char *name,
                       DBusPendingCallNotifyFunction callback,
                       int timeout,
                       void *user_data, DBusFreeFunction user_free,
                       DBusPendingCall **ppc,
                       int first_arg_type, ...);

void mce_dbus_send_config_notification(GConfEntry *entry);

const char *mce_dbus_type_repr(int type);
char *mce_dbus_message_repr(DBusMessage *const msg);
char *mce_dbus_message_iter_repr(DBusMessageIter *iter);

bool mce_dbus_iter_at_end(DBusMessageIter *iter);
bool mce_dbus_iter_req_type(DBusMessageIter *iter, int want);
bool mce_dbus_iter_get_basic(DBusMessageIter *iter, void *pval, int type);
bool mce_dbus_iter_get_object(DBusMessageIter *iter, const char **pval);
bool mce_dbus_iter_get_string(DBusMessageIter *iter, const char **pval);
bool mce_dbus_iter_get_bool(DBusMessageIter *iter, bool *pval);
bool mce_dbus_iter_get_int32(DBusMessageIter *iter, dbus_int32_t *pval);
bool mce_dbus_iter_get_uint32(DBusMessageIter *iter, dbus_uint32_t *val);
bool mce_dbus_iter_get_array(DBusMessageIter *iter, DBusMessageIter *sub);
bool mce_dbus_iter_get_struct(DBusMessageIter *iter, DBusMessageIter *sub);
bool mce_dbus_iter_get_entry(DBusMessageIter *iter, DBusMessageIter *sub);
bool mce_dbus_iter_get_variant(DBusMessageIter *iter, DBusMessageIter *sub);

void mce_dbus_handler_register(mce_dbus_handler_t *self);
void mce_dbus_handler_unregister(mce_dbus_handler_t *self);

void mce_dbus_handler_register_array(mce_dbus_handler_t *array);
void mce_dbus_handler_unregister_array(mce_dbus_handler_t *array);

void mce_dbus_pending_call_blocks_suspend(DBusPendingCall *pc);

/* ========================================================================= *
 * D-Bus client tracking
 * ========================================================================= */

/** D-Bus peer identity/availability tracking state */
typedef enum
{
    /** Freshly created */
    PEERSTATE_INITIAL,

    /** Pending org.freedesktop.DBus.GetNameOwner */
    PEERSTATE_QUERY_OWNER,

    /** Pending org.freedesktop.DBus.GetConnectionUnixProcessID */
    PEERSTATE_QUERY_PID,

    /** Pending org.sailfishos.sailjailed.Identify */
    PEERSTATE_IDENTIFY,

    /** Owner known and available on D-Bus */
    PEERSTATE_RUNNING,

    /** Owner possibly known, but no longer available on D-Bus
     *
     * The peer info is retained briefly so that it is still
     * available when for example logging client cleanup actions.
     */
    PEERSTATE_STOPPED,
} peerstate_t;

const char *peerstate_repr(peerstate_t state);

/** Opaque object type for holding D-Bus peer details */
typedef struct peerinfo_t peerinfo_t;

const char *peerinfo_name (const peerinfo_t *self);
peerstate_t peerinfo_get_state (const peerinfo_t *self);
pid_t peerinfo_get_owner_pid(const peerinfo_t *self);
uid_t peerinfo_get_owner_uid(const peerinfo_t *self);
gid_t peerinfo_get_owner_gid(const peerinfo_t *self);
const char *peerinfo_get_owner_cmd(const peerinfo_t *self);

/** Callback function type used by dbus name tracking functionality */
typedef void (*peernotify_fn)(const peerinfo_t *peerinfo, gpointer userdata);

void mce_dbus_name_tracker_add(const char     *name,
                               peernotify_fn   callback,
                               gpointer        userdata,
                               GDestroyNotify  userfree);

void mce_dbus_name_tracker_remove(const char     *name,
                                  peernotify_fn   callback,
                                  gpointer        userdata);

const char *mce_dbus_get_name_owner_ident(const char *name);
const char *mce_dbus_get_message_sender_ident(DBusMessage *msg);
const char *mce_dbus_nameowner_get(const char *name);

gssize mce_dbus_owner_monitor_add(const gchar *service,
                                  gboolean (*callback)(DBusMessage *const msg),
                                  GSList **monitor_list,
                                  gssize max_num);

gssize mce_dbus_owner_monitor_remove(const gchar *service,
                                     GSList **monitor_list);

void mce_dbus_owner_monitor_remove_all(GSList **monitor_list);

/* ========================================================================= *
 * Module init/quit
 * ========================================================================= */

gboolean mce_dbus_init(const gboolean systembus);
void     mce_dbus_exit(void);

#endif /* MCE_DBUS_H_ */
