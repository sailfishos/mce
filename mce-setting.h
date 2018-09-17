/**
 * @file mce-setting.h
 * Headers for the runtime setting handling code for the Mode Control Entity
 * <p>
 * Copyright © 2004-2007 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright © 2014-2016 Jolla Ltd.
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
#ifndef MCE_SETTING_H_
# define MCE_SETTING_H_

# include "builtin-gconf.h"

/* ========================================================================= *
 * Fingerprint Scanner Settings
 * ========================================================================= */

/** Fingerprint wakeup enable modes */
typedef enum
{
    /** Fingerprint wakeups disabled */
    FPWAKEUP_ENABLE_NEVER        = 0,

    /** Fingerprint wakeups always enabled */
    FPWAKEUP_ENABLE_ALWAYS       = 1,

    /** Fingerprint wakeups enabled when PS is not covered */
    FPWAKEUP_ENABLE_NO_PROXIMITY = 2,
} fpwakeup_mode_t;

/** Prefix for fingerprint setting keys */
# define MCE_SETTING_FINGERPRINT_PATH   "/system/osso/dsm/fingerprint"

/** When fingerprint wakeup is allowed */
# define MCE_SETTING_FPWAKEUP_MODE             MCE_SETTING_FINGERPRINT_PATH "/mode"
# define MCE_DEFAULT_FPWAKEUP_MODE             0 // = FPWAKEUP_ENABLE_NEVER

/** Delay between policy change and activating fingerprint daemon [ms] */
# define MCE_SETTING_FPWAKEUP_ALLOW_DELAY      MCE_SETTING_FINGERPRINT_PATH "/allow_delay"
# define MCE_DEFAULT_FPWAKEUP_ALLOW_DELAY      500

/** Delay between getting identified fingerprint and acting on it [ms] */
# define MCE_SETTING_FPWAKEUP_TRIGGER_DELAY    MCE_SETTING_FINGERPRINT_PATH "/trigger_delay"
# define MCE_DEFAULT_FPWAKEUP_TRIGGER_DELAY    100

/** Delay between ipc attempts with fingerprint daemon [ms] */
# define MCE_SETTING_FPWAKEUP_THROTTLE_DELAY   MCE_SETTING_FINGERPRINT_PATH "/throttle_delay"
# define MCE_DEFAULT_FPWAKEUP_THROTTLE_DELAY   250

/* ========================================================================= *
 * Functions
 * ========================================================================= */

gboolean      mce_setting_has_key           (const gchar *const key);
gboolean      mce_setting_set_int           (const gchar *const key, const gint value);
gboolean      mce_setting_set_string        (const gchar *const key, const gchar *const value);
gboolean      mce_setting_get_bool          (const gchar *const key, gboolean *value);
gboolean      mce_setting_get_int           (const gchar *const key, gint *value);
gboolean      mce_setting_get_int_list      (const gchar *const key, GSList **values);
gboolean      mce_setting_get_string        (const gchar *const key, gchar **value);
gboolean      mce_setting_notifier_add      (const gchar *path, const gchar *key, const GConfClientNotifyFunc callback, guint *cb_id);
void          mce_setting_notifier_remove   (guint id);
void          mce_setting_notifier_remove_cb(gpointer cb_id, gpointer user_data);
void          mce_setting_track_int         (const gchar *key, gint *val, gint def, GConfClientNotifyFunc cb, guint *cb_id);
void          mce_setting_track_bool        (const gchar *key, gboolean *val, gint def, GConfClientNotifyFunc cb, guint *cb_id);
void          mce_setting_track_string      (const gchar *key, gchar **val, const gchar *def, GConfClientNotifyFunc cb, guint *cb_id);

gboolean      mce_setting_init              (void);
void          mce_setting_exit              (void);

#endif /* MCE_SETTING_H_ */
