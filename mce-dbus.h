/**
 * @file mce-dbus.h
 * Headers for the D-Bus handling code for the Mode Control Entity
 * <p>
 * Copyright © 2004-2010 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _MCE_DBUS_H_
#define _MCE_DBUS_H_

#include "builtin-gconf.h"

#include <stdbool.h>

#include <dbus/dbus.h>

/* ========================================================================= *
 * DSME DBUS SERVICE
 * ========================================================================= */

/** Well known dbus name of dsme */
#define DSME_DBUS_SERVICE "com.nokia.dsme"

/* ========================================================================= *
 * COMPOSITOR DBUS SERVICE
 * ========================================================================= */

#define COMPOSITOR_SERVICE  "org.nemomobile.compositor"
#define COMPOSITOR_PATH     "/"
#define COMPOSITOR_IFACE    "org.nemomobile.compositor"

/* Enabling/disabling display updates via compositor service */
#define COMPOSITOR_SET_UPDATES_ENABLED "setUpdatesEnabled"

/* ========================================================================= *
 * LIPSTICK DBUS SERVICE
 * ========================================================================= */

#define LIPSTICK_SERVICE  "org.nemomobile.lipstick"
#define LIPSTICK_PATH     "/"
#define LIPSTICK_IFACE    "org.nemomobile.lipstick"

/* ========================================================================= *
 * USB_MODED DBUS SERVICE
 * ========================================================================= */

/** Well known service name for usb_moded */
#define USB_MODED_DBUS_SERVICE      "com.meego.usb_moded"

/** D-Bus interface name for usb_moded */
#define USB_MODED_DBUS_INTERFACE    "com.meego.usb_moded"

/** D-Bus object name for usb_moded */
#define USB_MODED_DBUS_OBJECT       "/com/meego/usb_moded"

/** Query current usb mode method call */
#define USB_MODED_QUERY_MODE_REQ    "mode_request"

/** Current usb mode changed signal */
#define USB_MODED_MODE_CHANGED_SIG  "sig_usb_state_ind"

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

gssize mce_dbus_owner_monitor_add(const gchar *service,
				  gboolean (*callback)(DBusMessage *const msg),
				  GSList **monitor_list,
				  gssize max_num);
gssize mce_dbus_owner_monitor_remove(const gchar *service,
				     GSList **monitor_list);
void mce_dbus_owner_monitor_remove_all(GSList **monitor_list);

gboolean mce_dbus_init(const gboolean systembus);
void mce_dbus_exit(void);

void mce_dbus_send_config_notification(GConfEntry *entry);

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

const char *mce_dbus_type_repr(int type);
bool mce_dbus_iter_at_end(DBusMessageIter *iter);
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

char *mce_dbus_message_repr(DBusMessage *const msg);
char *mce_dbus_message_iter_repr(DBusMessageIter *iter);

const char *mce_dbus_get_name_owner_ident(const char *name);
const char *mce_dbus_get_message_sender_ident(DBusMessage *msg);

const char *mce_dbus_nameowner_get(const char *name);

void mce_dbus_pending_call_blocks_suspend(DBusPendingCall *pc);

#endif /* _MCE_DBUS_H_ */
