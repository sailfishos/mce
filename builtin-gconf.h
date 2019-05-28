/**
 * @file builtin-gconf.h
 * Mode Control Entity - Build-in GConf compatible settings
 * <p>
 * Copyright (C) 2012-2019 Jolla Ltd.
 * <p>
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

#ifndef BUILTIN_GCONF_H_
# define BUILTIN_GCONF_H_

#include <stdbool.h>
#include <glib.h>

# ifdef __cplusplus
extern "C" {
# elif 0
} /* fool JED indentation ... */
# endif

/* ========================================================================= *
 *
 * TYPES
 *
 * ========================================================================= */

typedef enum
{
  GCONF_ERROR_SUCCESS              =  0,
  GCONF_ERROR_FAILED               =  1,

  GCONF_ERROR_NO_SERVER            =  2,
  GCONF_ERROR_NO_PERMISSION        =  3,
  GCONF_ERROR_BAD_ADDRESS          =  4,
  GCONF_ERROR_BAD_KEY              =  5,

  GCONF_ERROR_PARSE_ERROR          =  6,
  GCONF_ERROR_CORRUPT              =  7,
  GCONF_ERROR_TYPE_MISMATCH        =  8,
  GCONF_ERROR_IS_DIR               =  9,
  GCONF_ERROR_IS_KEY               = 10,
  GCONF_ERROR_OVERRIDDEN           = 11,
  GCONF_ERROR_OAF_ERROR            = 12,
  GCONF_ERROR_LOCAL_ENGINE         = 13,
  GCONF_ERROR_LOCK_FAILED          = 14,
  GCONF_ERROR_NO_WRITABLE_DATABASE = 15,
  GCONF_ERROR_IN_SHUTDOWN          = 16,
} GConfError;

typedef enum
{
  GCONF_VALUE_INVALID,
  GCONF_VALUE_STRING,
  GCONF_VALUE_INT,
  GCONF_VALUE_FLOAT,
  GCONF_VALUE_BOOL,
  GCONF_VALUE_SCHEMA,
  GCONF_VALUE_LIST,
  GCONF_VALUE_PAIR

} GConfValueType;

typedef struct GConfValue
{
  // public

  GConfValueType type;

  // private

  int refcount;

  union {
    gboolean b;
    gint     i;
    char    *s;
    double   f;
  } data;

  GConfValueType  list_type;
  GSList         *list_head;

} GConfValue;

typedef struct GConfEntry
{
  // public

  char *key;
  GConfValue *value;

  // private

  char *def;

  bool notify_entered; // already withing gconf_client_notify_change()
  bool notify_changed; // another round of notifications needed within gconf_client_notify_change()

} GConfEntry;

typedef struct GConfClient
{
  // public

  // (nothing)

  // private

  GSList  *entries;

  GSList  *notify_list;

} GConfClient;

typedef enum
{
  GCONF_CLIENT_PRELOAD_NONE,
  GCONF_CLIENT_PRELOAD_ONELEVEL,
  GCONF_CLIENT_PRELOAD_RECURSIVE
} GConfClientPreloadType;

typedef void (*GConfClientNotifyFunc)(GConfClient *client,
                                      guint cnxn_id,
                                      GConfEntry *entry,
                                      gpointer user_data);

typedef struct GConfClientNotify
{
  guint                 id;
  gchar                *namespace_section;
  GConfClientNotifyFunc func;
  gpointer              user_data;
  GFreeFunc             destroy_notify;

} GConfClientNotify;

/* ------------------------------------------------------------------------- *
 *
 * FUNCTIONS
 *
 * ------------------------------------------------------------------------- */

gchar *gconf_concat_dir_and_key(const gchar *dir, const gchar *key);
GConfValue *gconf_value_copy(const GConfValue *src);
GConfValue *gconf_value_new(GConfValueType type);
void gconf_value_free(GConfValue *self);
gboolean gconf_value_get_bool(const GConfValue *self);
bool gconf_value_set_bool(GConfValue *self, gboolean val);
int gconf_value_get_int(const GConfValue *self);
bool gconf_value_set_int(GConfValue *self, gint val);
double gconf_value_get_float(const GConfValue *self);
bool gconf_value_set_float(GConfValue *self, double val);
const char *gconf_value_get_string(const GConfValue *self);
bool gconf_value_set_string(GConfValue *self, const char *val);
GConfValueType gconf_value_get_list_type(const GConfValue *self);
void gconf_value_set_list_type(GConfValue *self, GConfValueType list_type);
GSList *gconf_value_get_list(const GConfValue *self);
bool gconf_value_set_list(GConfValue *self, GSList *list);
const char *gconf_entry_get_key(const GConfEntry *entry);
GConfValue *gconf_entry_get_value(const GConfEntry *entry);
GConfClient *gconf_client_get_default(void);
int gconf_client_reset_defaults(GConfClient *self, const char *keyish);
void gconf_client_add_dir(GConfClient *client, const gchar *dir, GConfClientPreloadType preload, GError **err);
GConfValue *gconf_client_get(GConfClient *self, const gchar *key, GError **err);
gboolean gconf_client_set_bool(GConfClient *client, const gchar *key, gboolean val, GError **err);
gboolean gconf_client_set_int(GConfClient *client, const gchar *key, gint val, GError **err);
gboolean gconf_client_set_float(GConfClient *client, const gchar *key, double val, GError **err);
gboolean gconf_client_set_string(GConfClient *client, const gchar *key, const gchar *val, GError **err);
gboolean gconf_client_set_list(GConfClient *client, const gchar *key, GConfValueType list_type, GSList *list, GError **err);
void gconf_client_suggest_sync(GConfClient *client, GError **err);
guint gconf_client_notify_add(GConfClient *client, const gchar *namespace_section, GConfClientNotifyFunc func, gpointer user_data, GFreeFunc destroy_notify, GError **err);
void gconf_client_notify_remove(GConfClient *client, guint cnxn);

# ifdef __cplusplus
};
# endif

#endif /* BUILTIN_GCONF_H_ */
