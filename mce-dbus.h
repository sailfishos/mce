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

#include <glib.h>
#include <dbus/dbus.h>

#include <mce/dbus-names.h>

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
gboolean dbus_send_message_with_reply_handler(DBusMessage *const msg,
					      DBusPendingCallNotifyFunction callback);

gboolean dbus_send(const gchar *const service, const gchar *const path,
		   const gchar *const interface, const gchar *const name,
		   DBusPendingCallNotifyFunction callback,
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

#endif /* _MCE_DBUS_H_ */
