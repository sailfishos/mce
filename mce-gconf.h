/**
 * @file mce-gconf.h
 * Headers for the GConf handling code for the Mode Control Entity
 * <p>
 * Copyright © 2004-2007 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _MCE_GCONF_H_
#define _MCE_GCONF_H_

#include "builtin-gconf.h"

gboolean mce_gconf_has_key(const gchar *const key);
gboolean mce_gconf_set_int(const gchar *const key, const gint value);
gboolean mce_gconf_get_bool(const gchar *const key, gboolean *value);
gboolean mce_gconf_get_int(const gchar *const key, gint *value);
gboolean mce_gconf_get_int_list(const gchar *const key, GSList **values);
gboolean mce_gconf_get_string(const gchar *const key, gchar **value);
gboolean mce_gconf_set_string(const gchar *const key, const gchar *const value);
gboolean mce_gconf_notifier_add(const gchar *path, const gchar *key,
				const GConfClientNotifyFunc callback,
				guint *cb_id);
void mce_gconf_notifier_remove(guint id);
void mce_gconf_notifier_remove_cb(gpointer cb_id, gpointer user_data);

void mce_gconf_track_bool(const gchar *key, gboolean *val, gint def,
			  GConfClientNotifyFunc cb, guint *cb_id);
void mce_gconf_track_int(const gchar *key, gint *val, gint def,
			 GConfClientNotifyFunc cb, guint *cb_id);
void mce_gconf_track_string(const gchar *key, gchar **val, const gchar *def,
			    GConfClientNotifyFunc cb, guint *cb_id);

gboolean mce_gconf_init(void);
void mce_gconf_exit(void);

#endif /* _MCE_GCONF_H_ */
