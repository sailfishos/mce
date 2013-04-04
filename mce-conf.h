/**
 * @file mce-conf.h
 * Headers for the configuration option handling for MCE
 * <p>
 * Copyright © 2006-2007 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _MCE_CONF_H_
#define _MCE_CONF_H_

#include <glib.h>

gboolean mce_conf_get_bool(const gchar *group, const gchar *key,
			   const gboolean defaultval);
gint mce_conf_get_int(const gchar *group, const gchar *key,
		      const gint defaultval);
gint *mce_conf_get_int_list(const gchar *group, const gchar *key,
			    gsize *length);
gchar *mce_conf_get_string(const gchar *group, const gchar *key,
			   const gchar *defaultval);
gchar **mce_conf_get_string_list(const gchar *group, const gchar *key,
				 gsize *length);
gchar **mce_conf_get_keys(const gchar *group, gsize *length);

gboolean mce_conf_init(void);
void mce_conf_exit(void);

const gchar * const *mce_conf_get_touchscreen_event_drivers(void);
const gchar * const *mce_conf_get_keyboard_event_drivers(void);
const gchar * const *mce_conf_get_blacklisted_event_drivers(void);

#endif /* _MCE_CONF_H_ */
