/**
 * @file mce-conf.h
 * Headers for the configuration option handling for MCE
 * <p>
 * Copyright © 2006-2007 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2013-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
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
#ifndef _MCE_CONF_H_
#define _MCE_CONF_H_

#include <glib.h>

gboolean mce_conf_has_group(const gchar *group);
gboolean mce_conf_has_key(const gchar *group, const gchar *key);

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

/* ========================================================================= *
 * Constant related to button backlight configuration
 * ========================================================================= */

/** Name of the display backlight configuration group */
# define MCE_CONF_BUTTON_BACKLIGHT_GROUP                  "ButtonBacklight"

/** Path to button backlight control file */
# define MCE_CONF_BUTTON_BACKLIGHT_CONTROL_PATH           "ControlPath"

/** Value to write when enabling button backlight */
# define MCE_CONF_BUTTON_BACKLIGHT_CONTROL_VALUE_ENABLE   "ControlValueEnable"

/** Value to write when disabling button backlight */
# define MCE_CONF_BUTTON_BACKLIGHT_CONTROL_VALUE_DISABLE  "ControlValueDisable"

#endif /* _MCE_CONF_H_ */
