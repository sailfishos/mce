/**
 * @file mce-setting.c
 * Runtime setting handling code for the Mode Control Entity
 * <p>
 * Copyright © 2004-2009 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright © 2012-2016 Jolla Ltd.
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

#include "mce-setting.h"

#include "mce-log.h"

#include <string.h>

/** Pointer to the GConf client */
static GConfClient *gconf_client = NULL;
/** Is GConf disabled on purpose */
static gboolean gconf_disabled = FALSE;
/** List of GConf notifiers */
static GSList *gconf_notifiers = NULL;

/** Check if gconf-key exists
 *
 * @param key Name of value
 *
 * @return TRUE if value exits, FALSE otherwise
 */
gboolean mce_setting_has_key(const gchar *const key)
{
	gboolean    res = FALSE;
	GConfValue *val = 0;
	GError     *err = 0;

	if( gconf_disabled )
		goto EXIT;

	val = gconf_client_get(gconf_client, key, &err);
	res = (val != 0);

EXIT:
	g_clear_error(&err);
	gconf_value_free(val);

	return res;
}

/**Set an boolean GConf key to the specified value
 *
 * @param key   The GConf key to set the value of
 * @param value The value to set the key to
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_setting_set_bool(const gchar *const key, const gboolean value)
{
	gboolean status = FALSE;

	if( gconf_disabled ) {
		mce_log(LL_DEBUG, "blocked %s = %d", key, value);
		goto EXIT;
	}

	if( !gconf_client_set_bool(gconf_client, key, value, NULL) ) {
		mce_log(LL_WARN, "Failed to write %s to GConf", key);
		goto EXIT;
	}

	/* synchronise if possible, ignore errors */
	gconf_client_suggest_sync(gconf_client, NULL);

	status = TRUE;

EXIT:
	return status;
}

/**
 * Set an integer GConf key to the specified value
 *
 * @param key The GConf key to set the value of
 * @param value The value to set the key to
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_setting_set_int(const gchar *const key, const gint value)
{
	gboolean status = FALSE;

	if( gconf_disabled ) {
		mce_log(LL_DEBUG, "blocked %s = %d", key, value);
		goto EXIT;
	}

	if (gconf_client_set_int(gconf_client, key, value, NULL) == FALSE) {
		mce_log(LL_WARN, "Failed to write %s to GConf", key);
		goto EXIT;
	}

	/* synchronise if possible, ignore errors */
	gconf_client_suggest_sync(gconf_client, NULL);

	status = TRUE;

EXIT:
	return status;
}

/**
 * Set an string GConf key to the specified value
 *
 * @param key The GConf key to set the value of
 * @param value The value to set the key to
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_setting_set_string(const gchar *const key, const gchar *const value)
{
	gboolean status = FALSE;

	if( gconf_disabled ) {
		mce_log(LL_DEBUG, "blocked %s = \"%s\"", key, value);
		goto EXIT;
	}

	if (gconf_client_set_string(gconf_client, key, value, NULL) == FALSE) {
		mce_log(LL_WARN, "Failed to write %s to GConf", key);
		goto EXIT;
	}

	/* synchronise if possible, ignore errors */
	gconf_client_suggest_sync(gconf_client, NULL);

	status = TRUE;

EXIT:
	return status;
}

/**
 * Return a boolean from the specified GConf key
 *
 * @param key The GConf key to get the value from
 * @param[out] value Will contain the value on return, if successful
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_setting_get_bool(const gchar *const key, gboolean *value)
{
	gboolean status = FALSE;
	GError *error = NULL;
	GConfValue *gcv = 0;

	if( gconf_disabled ) {
		mce_log(LL_DEBUG, "blocked %s query", key);
		goto EXIT;
	}

	gcv = gconf_client_get(gconf_client, key, &error);

	if (gcv == NULL) {
		mce_log((error != NULL) ? LL_WARN : LL_INFO,
			"Could not retrieve %s from GConf; %s",
			key, (error != NULL) ? error->message : "Key not set");
		goto EXIT;
	}

	if (gcv->type != GCONF_VALUE_BOOL) {
		mce_log(LL_ERR,
			"GConf key %s should have type: %d, but has type: %d",
			key, GCONF_VALUE_BOOL, gcv->type);
		goto EXIT;
	}

	*value = gconf_value_get_bool(gcv);

	status = TRUE;

EXIT:
	if( gcv )
		gconf_value_free(gcv);
	g_clear_error(&error);

	return status;
}

/**
 * Return an integer from the specified GConf key
 *
 * @param key The GConf key to get the value from
 * @param[out] value Will contain the value on return
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_setting_get_int(const gchar *const key, gint *value)
{
	gboolean status = FALSE;
	GError *error = NULL;
	GConfValue *gcv = 0;

	if( gconf_disabled ) {
		mce_log(LL_DEBUG, "blocked %s query", key);
		goto EXIT;
	}

	gcv = gconf_client_get(gconf_client, key, &error);

	if (gcv == NULL) {
		mce_log((error != NULL) ? LL_WARN : LL_INFO,
			"Could not retrieve %s from GConf; %s",
			key, (error != NULL) ? error->message : "Key not set");
		goto EXIT;
	}

	if (gcv->type != GCONF_VALUE_INT) {
		mce_log(LL_ERR,
			"GConf key %s should have type: %d, but has type: %d",
			key, GCONF_VALUE_INT, gcv->type);
		goto EXIT;
	}

	*value = gconf_value_get_int(gcv);

	status = TRUE;

EXIT:
	if( gcv )
		gconf_value_free(gcv);
	g_clear_error(&error);

	return status;
}

/**
 * Return an integer list from the specified GConf key
 *
 * @param key The GConf key to get the values from
 * @param[out] values Will contain an GSList with the values on return
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_setting_get_int_list(const gchar *const key, GSList **values)
{
	gboolean status = FALSE;
	GError *error = NULL;
	GConfValue *gcv = 0;
	GConfValue *gcv2;
	GSList *list;
	gint i;

	if( gconf_disabled ) {
		mce_log(LL_DEBUG, "blocked %s query", key);
		goto EXIT;
	}

	gcv = gconf_client_get(gconf_client, key, &error);

	if (gcv == NULL) {
		mce_log((error != NULL) ? LL_WARN : LL_INFO,
			"Could not retrieve %s from GConf; %s",
			key, (error != NULL) ? error->message : "Key not set");
		goto EXIT;
	}

	if ((gcv->type != GCONF_VALUE_LIST) ||
	    (gconf_value_get_list_type(gcv) != GCONF_VALUE_INT)) {
		mce_log(LL_ERR,
			"GConf key %s should have type: %d<%d>, but has type: %d<%d>",
			key, GCONF_VALUE_LIST, GCONF_VALUE_INT,
			gcv->type, gconf_value_get_list_type(gcv));
		goto EXIT;
	}

	list = gconf_value_get_list(gcv);

	for (i = 0; (gcv2 = g_slist_nth_data(list, i)) != NULL; i++) {
		gint data;

		data = gconf_value_get_int(gcv2);

		/* Prepend is more efficient than append */
		*values = g_slist_prepend(*values, GINT_TO_POINTER(data));
	}

	/* Reverse the list, since we want the entries in the right order */
	*values = g_slist_reverse(*values);

	status = TRUE;

EXIT:
	if( gcv )
		gconf_value_free(gcv);
	g_clear_error(&error);

	return status;
}

/**
 * Return an string from the specified GConf key
 *
 * @param key The GConf key to get the values from
 * @param[out] value Will contain a newly allocated string with the value
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_setting_get_string(const gchar *const key, gchar **value)
{
	gboolean status = FALSE;
	GError *error = NULL;
	GConfValue *gcv = 0;

	if( gconf_disabled ) {
		mce_log(LL_DEBUG, "blocked %s query", key);
		goto EXIT;
	}

	gcv = gconf_client_get(gconf_client, key, &error);

	if (gcv == NULL) {
		mce_log((error != NULL) ? LL_WARN : LL_INFO,
			"Could not retrieve %s from GConf; %s",
			key, (error != NULL) ? error->message : "Key not set");
		goto EXIT;
	}

	if ((gcv->type != GCONF_VALUE_STRING)) {
		mce_log(LL_ERR,
			"GConf key %s should have type: %d, but has type: %d",
			key, GCONF_VALUE_STRING, gcv->type);
		goto EXIT;
	}

	*value = g_strdup(gconf_value_get_string(gcv));

	status = TRUE;

EXIT:
	if( gcv )
		gconf_value_free(gcv);
	g_clear_error(&error);

	return status;
}

/**
 * Add a GConf notifier
 *
 * @param path The GConf directory to watch
 * @param key The GConf key to add the notifier for
 * @param callback The callback function
 * @param[out] cb_id Will contain the callback ID or zero on return
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_setting_notifier_add(const gchar *path, const gchar *key,
				const GConfClientNotifyFunc callback,
				guint *cb_id)
{
	GError *error = NULL;
	gboolean status = FALSE;
	guint id = 0;

	if( gconf_disabled ) {
		mce_log(LL_DEBUG, "blocked %s notifier", key);

		/* Returning failure could result in termination
		 * of mce process -> return bogus success if we
		 * have disabled gconf on purpose. */
		status = TRUE;
		goto EXIT;
	}

	gconf_client_add_dir(gconf_client, path,
			     GCONF_CLIENT_PRELOAD_NONE, &error);

	if (error != NULL) {
		mce_log(LL_WARN,
			"Could not add %s to directories watched by "
			"GConf client setting from GConf; %s",
			path, error->message);
		goto EXIT;
	}

	id = gconf_client_notify_add(gconf_client, key, callback,
				     NULL, NULL, &error);
	if (error != NULL) {
		mce_log(LL_WARN,
			"Could not register notifier for %s; %s",
			key, error->message);
		goto EXIT;
	}

	if( !id )
		goto EXIT;

	gconf_notifiers = g_slist_prepend(gconf_notifiers,
					  GINT_TO_POINTER(id));

	status = TRUE;

EXIT:
	g_clear_error(&error);

	return *cb_id = id, status;
}

/**
 * Remove a GConf notifier
 *
 * Calling with zero id is allowed and does nothing
 *
 * @param id The ID of the notifier to remove, or zero
 */
void mce_setting_notifier_remove(guint id)
{
	if( gconf_disabled )
		goto EXIT;

	if( !id )
		goto EXIT;

	gconf_client_notify_remove(gconf_client, id);
	gconf_notifiers = g_slist_remove(gconf_notifiers, GINT_TO_POINTER(id));

EXIT:
	return;
}

/** Helper callback for removing GConf notifiers with g_slist_foreach
 *
 * @param cb_id The ID of the notifier to remove
 * @param user_data Unused
 */
void mce_setting_notifier_remove_cb(gpointer cb_id, gpointer user_data)
{
	(void)user_data;

	mce_setting_notifier_remove(GPOINTER_TO_INT(cb_id));
}

/** Helper for getting path of gconf key
 */
static gchar *mce_setting_get_path(const gchar *key)
{
	gchar       *res = 0;
	const gchar *end = strrchr(key, '/');

	if( end )
		res = g_strndup(key, end - key);

	return res;
}

/** Get initial value of integer setting and start tracking changes
 *
 * @param key   key name
 * @param val   where to store the initial value
 * @param def   default value to use if getting key value fails;
 *              or -1 to leave *val unmodified
 * @param cb    change notification callback
 * @param cb_id where to store notification callback id
 */
void mce_setting_track_int(const gchar *key, gint *val, gint def,
			 GConfClientNotifyFunc cb, guint *cb_id)
{
	gchar *path = mce_setting_get_path(key);

	if( path && cb && cb_id )
		mce_setting_notifier_add(path, key, cb, cb_id);

	if( !mce_setting_get_int(key, val) && def != -1 )
		*val = def;

	g_free(path);
}

/** Get initial value of boolean setting and start tracking changes
 *
 * @param key   key name
 * @param val   where to store the initial value
 * @param def   default value to use if getting key value fails;
 *              or -1 to leave *val unmodified
 * @param cb    change notification callback
 * @param cb_id where to store notification callback id
 */
void mce_setting_track_bool(const gchar *key, gboolean *val, gint def,
			  GConfClientNotifyFunc cb, guint *cb_id)
{
	gchar *path = mce_setting_get_path(key);

	if( path && cb && cb_id )
		mce_setting_notifier_add(path, key, cb, cb_id);

	if( !mce_setting_get_bool(key, val) && def != -1 )
		*val = (def != 0);

	g_free(path);
}

/** Get initial value of integer setting and start tracking changes
 *
 * Note: Caller must release returned string with g_free() when it
 *       is no longer needed.
 *
 * @param key   key name
 * @param val   where to store the initial value
 * @param def   default value to use if getting key value fails;
 *              or NULL to leave *val unmodified
 * @param cb    change notification callback
 * @param cb_id where to store notification callback id
 */
void mce_setting_track_string(const gchar *key, gchar **val, const gchar *def,
			    GConfClientNotifyFunc cb, guint *cb_id)
{
	gchar *path = mce_setting_get_path(key);

	if( path && cb && cb_id )
		mce_setting_notifier_add(path, key, cb, cb_id);

	if( !mce_setting_get_string(key, val) && def != 0 )
		*val = g_strdup(def);

	g_free(path);
}

/**
 * Init function for the mce-gconf component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_setting_init(void)
{
	gboolean status = FALSE;

	/* Get the default builtin-gconf client */
	if( !(gconf_client = gconf_client_get_default()) ) {
		mce_log(LL_CRIT, "Could not get default builtin-gconf client");
		goto EXIT;
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * Exit function for the mce-gconf component
 */
void mce_setting_exit(void)
{
	if( gconf_client ) {
		/* Free the list of GConf notifiers */
		g_slist_foreach(gconf_notifiers, mce_setting_notifier_remove_cb, 0);
		gconf_notifiers = 0;

		/* Forget builtin-gconf client reference */
		gconf_client = 0;
	}

	return;
}
