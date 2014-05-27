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
		      int first_arg_type, ...);

DBusMessage *dbus_send_with_block(const gchar *const service,
				  const gchar *const path,
				  const gchar *const interface,
				  const gchar *const name,
				  gint timeout, int first_arg_type, ...);
pid_t dbus_get_pid_from_bus_name(const gchar *const bus_name);

gconstpointer mce_dbus_handler_add(const gchar *const interface,
				   const gchar *const name,
				   const gchar *const rules,
				   const guint type,
				   gboolean (*callback)(DBusMessage *const msg));
void mce_dbus_handler_remove(gconstpointer cookie);
gboolean mce_dbus_is_owner_monitored(const gchar *service,
				     GSList *monitor_list);
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

/** D-Bus message handler data
 *
 * For use with mce_dbus_handler_register() etc
 */
typedef struct
{
  const char *interface;
  const char *name;
  const char *rules;
  int         type;
  gboolean (*callback)(DBusMessage *const msg);

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

typedef void (*mce_dbus_pid_notify_t)(const char *name, int pid);
void mce_dbus_get_pid_async(const char *name, mce_dbus_pid_notify_t cb);

#endif /* _MCE_DBUS_H_ */
