#include <check.h>
#include <dbus/dbus-glib-lowlevel.h>	/* dbus_connection_setup_with_g_main */
#include <glib.h>
#include <linux/input.h>
#include <stdbool.h>

#include "common.h"

/* Testing is done with stubs pretending presence of a generic display.
 * Disabling libhybris related code saves us from declaring dummy stubs for
 * hybris functions */
#ifdef ENABLE_HYBRIS
# undef ENABLE_HYBRIS
#endif

/* Tested module */
#include "../../modules/display.c"

/* Derived from get_display_type(), case DISPLAY_DISPLAY0 */
/* brightness_output.path */
#define STUB__BRIGHTNESS_OUTPUT_PATH                                \
        DISPLAY_BACKLIGHT_PATH DISPLAY_DISPLAY0                     \
        DISPLAY_CABC_BRIGHTNESS_FILE
/* max_brightness_file */
#define STUB__MAX_BRIGHTNESS_FILE                                   \
        DISPLAY_BACKLIGHT_PATH DISPLAY_DISPLAY0                     \
        DISPLAY_CABC_MAX_BRIGHTNESS_FILE
/* cabc_mode_file */
#define STUB__CABC_MODE_FILE                                        \
        DISPLAY_BACKLIGHT_PATH DISPLAY_DISPLAY0 "/device"           \
        DISPLAY_CABC_MODE_FILE
/* cabc_available_modes_file */
#define STUB__CABC_AVAILABLE_MODES_FILE                             \
        DISPLAY_BACKLIGHT_PATH DISPLAY_DISPLAY0 "/device"           \
        DISPLAY_CABC_AVAILABLE_MODES_FILE
/* hw_fading_output.path */
#define STUB__HW_FADING_OUTPUT_PATH                                 \
        DISPLAY_BACKLIGHT_PATH DISPLAY_DISPLAY0 DISPLAY_DEVICE_PATH \
        DISPLAY_HW_DIMMING_FILE
/* high_brightness_mode_output.path */
#define STUB__HIGH_BRIGHTNESS_MODE_OUTPUT_PATH                      \
        DISPLAY_BACKLIGHT_PATH DISPLAY_DISPLAY0 DISPLAY_DEVICE_PATH \
        DISPLAY_HBM_FILE
/* low_power_mode_file */
#define STUB__LOW_POWER_MODE_FILE                                   \
        DISPLAY_BACKLIGHT_PATH DISPLAY_DISPLAY0 DISPLAY_DEVICE_PATH \
        DISPLAY_LPM_FILE

/* ------------------------------------------------------------------------- *
 * EXTERN STUBS
 * ------------------------------------------------------------------------- */

/*
 * Note that the following modules are linked instead of providing stubs:
 *
 * 	- datapipe.c
 * 	- mce-lib.c
 * 	- modetransition.c (only submode manipulation helpers are used)
 */

/*
 * mce-conf.c stubs {{{1
 */

EXTERN_DUMMY_STUB (
gboolean, mce_conf_has_group, (const gchar *group));

typedef struct stub__mce_conf_get_int_item
{
	const gchar *group;
	const gchar *key;
	gint value;
} stub__mce_conf_get_int_item_t;
static stub__mce_conf_get_int_item_t *stub__mce_conf_get_int_items = NULL;

EXTERN_STUB (
gint, mce_conf_get_int, (const gchar *group, const gchar *key,
			 const gint defaultval))
{
	stub__mce_conf_get_int_item_t *const items =
		stub__mce_conf_get_int_items;

	for( int i = 0; items[i].key; ++i ) {
		if( strcmp(group, items[i].group) == 0
		    && strcmp(key, items[i].key) == 0 ) {
			return items[i].value != INT_MAX ? items[i].value : defaultval;
		}
	}

	ck_abort_msg("Key not handled: '%s'", key);

	return 0;
}

typedef struct stub__mce_conf_get_string_item
{
	const gchar *group;
	const gchar *key;
	const gchar *value;
} stub__mce_conf_get_string_item_t;
static stub__mce_conf_get_string_item_t *stub__mce_conf_get_string_items = NULL;

EXTERN_STUB (
gchar *, mce_conf_get_string, (const gchar *group, const gchar *key,
			       const gchar *defaultval))
{
	stub__mce_conf_get_string_item_t *const items =
		stub__mce_conf_get_string_items;

	for( int i = 0; items[i].key; ++i ) {
		if( strcmp(group, items[i].group) == 0
		    && strcmp(key, items[i].key) == 0 ) {
			return g_strdup(items[i].value ?: defaultval);
		}
	}

	ck_abort_msg("Key not handled: '%s'", key);

	return NULL;
}

EXTERN_DUMMY_STUB (
gchar **, mce_conf_get_string_list, (const gchar *group, const gchar *key,
				     gsize *length));

static void stub_mce_conf_setup_unchecked(void)
{
	static stub__mce_conf_get_int_item_t int_items[] = {
		{
			MCE_CONF_DISPLAY_GROUP,
			MCE_CONF_STEP_TIME_INCREASE,
			250, /* multiplied by 20 (steps) gives 5000ms for
				one-level brightness change (20% from 0-100
				range) */
		}, {
			MCE_CONF_DISPLAY_GROUP,
			MCE_CONF_STEP_TIME_DECREASE,
			250,
		}, {
			MCE_CONF_DISPLAY_GROUP,
			MCE_CONF_CONSTANT_TIME_INCREASE,
			5000,
		}, {
			MCE_CONF_DISPLAY_GROUP,
			MCE_CONF_CONSTANT_TIME_DECREASE,
			5000,
		}, {
			NULL,
			NULL,
			INT_MAX,
		}
	};
	stub__mce_conf_get_int_items = int_items;

	static stub__mce_conf_get_string_item_t string_items[] = {
		{
			MCE_CONF_DISPLAY_GROUP,
			MCE_CONF_BRIGHTNESS_INCREASE_POLICY,
			NULL,
		}, {
			MCE_CONF_DISPLAY_GROUP,
			MCE_CONF_BRIGHTNESS_DECREASE_POLICY,
			NULL,
		}, {
			NULL,
			NULL,
			NULL,
		}
	};
	stub__mce_conf_get_string_items = string_items;
}

static void stub_mce_conf_teardown_unchecked(void)
{
}

/*
 * mce-setting.c stubs {{{1
 */

typedef struct stub__mce_setting_notifier_data
{
	GConfClientNotifyFunc callback;
	guint cb_id;
} stub__mce_setting_notifier_data_t;

static GHashTable *stub__mce_setting_notifiers = NULL;

EXTERN_STUB (
gboolean, mce_setting_notifier_add, (const gchar *path, const gchar *key,
				   const GConfClientNotifyFunc callback,
				   guint *cb_id))
{
	(void)path;

	typedef stub__mce_setting_notifier_data_t notifier_data_t;
	GHashTable *const notifiers = stub__mce_setting_notifiers;

	ck_assert(g_hash_table_lookup(notifiers, key) == NULL);

	static guint max_id = 0;
	++max_id;

	notifier_data_t *const notifier = g_new(notifier_data_t, 1);
	notifier->callback = callback;
	notifier->cb_id = max_id;

	g_hash_table_insert(notifiers, g_strdup(key), notifier);

	*cb_id = notifier->cb_id;
	return TRUE;
}

EXTERN_STUB (
void, mce_setting_notifier_remove, (gpointer cb_id, gpointer user_data))
{
	(void)user_data;

	typedef stub__mce_setting_notifier_data_t notifier_data_t;
	GHashTable *const notifiers = stub__mce_setting_notifiers;

	gboolean match_cb_id(gpointer key, gpointer value, gpointer user_data_)
	{
		(void)key;
		(void)user_data_;

		notifier_data_t *const notifier = (notifier_data_t *)value;
		return notifier->cb_id == GPOINTER_TO_UINT(cb_id);
	}

	ck_assert_int_eq(1, g_hash_table_foreach_remove(notifiers, match_cb_id,
							NULL));
}

typedef struct stub__mce_setting_get_int_item
{
	const gchar *key;
	gint value;
} stub__mce_setting_get_int_item_t;
static stub__mce_setting_get_int_item_t *stub__mce_setting_get_int_items = NULL;

EXTERN_STUB (
gboolean, mce_setting_get_int, (const gchar *const key, gint *value))
{
	stub__mce_setting_get_int_item_t *const items =
		stub__mce_setting_get_int_items;

	for( int i = 0; items[i].key; ++i ) {
		if( strcmp(key, items[i].key) == 0 ) {
			*value = items[i].value;
			return TRUE;
		}
	}

	ck_abort_msg("Key not handled: '%s'", key);

	return FALSE;
}

static gboolean stub__mce_setting_set_int(const gchar *key, gint value)
{
	typedef stub__mce_setting_notifier_data_t notifier_data_t;
	typedef stub__mce_setting_get_int_item_t item_t;
	item_t *const items = stub__mce_setting_get_int_items;
	GHashTable *const notifiers = stub__mce_setting_notifiers;

	item_t *item = NULL;
	for( int i = 0; items[i].key; ++i ) {
		if( strcmp(key, items[i].key) == 0 ) {
			item = items + i;
			break;
		}
	}
	ck_assert_msg(item != NULL, "Key not handled: '%s'", key);

	item->value = value;

	notifier_data_t *const notifier = g_hash_table_lookup(notifiers, key);
	if( notifier != NULL ) {
		GConfValue *const entry_value = gconf_value_new(GCONF_VALUE_INT);
		gconf_value_set_int(entry_value, value);
		GConfEntry *const entry = gconf_entry_new(key, entry_value);

		(*notifier->callback)(NULL, notifier->cb_id, entry, NULL);

		gconf_entry_free(entry);
		gconf_value_free(entry_value);
	}

	ut_transition_recheck_schedule();

	return TRUE;
}

#define STUB__MCE_GCONF_GET_INT_LIST_MAX_ITEMS (10)
typedef struct stub__mce_setting_get_int_list_item
{
	const gchar *key;
	gint value[STUB__MCE_GCONF_GET_INT_LIST_MAX_ITEMS]; /* INT_MAX term. */
} stub__mce_setting_get_int_list_item_t;
static stub__mce_setting_get_int_list_item_t *stub__mce_setting_get_int_list_items = NULL;

EXTERN_STUB (
gboolean, mce_setting_get_int_list, (const gchar *const key, GSList **values))
{
	ck_assert(*values == NULL);

	stub__mce_setting_get_int_list_item_t *const items =
		stub__mce_setting_get_int_list_items;

	for( int i = 0; items[i].key; ++i ) {
		if( strcmp(key, items[i].key) != 0 )
			continue;

		for( int j = 0; items[i].value[j] != INT_MAX; ++j )
			*values = g_slist_append(*values,
					GINT_TO_POINTER(items[i].value[j]));
		return TRUE;
	}

	ck_abort_msg("Key not handled: '%s'", key);

	return FALSE;
}

typedef struct stub__mce_setting_get_bool_item
{
	const gchar *key;
	gboolean value;
} stub__mce_setting_get_bool_item_t;
static stub__mce_setting_get_bool_item_t *stub__mce_setting_get_bool_items = NULL;

EXTERN_STUB (
gboolean, mce_setting_get_bool, (const gchar *const key, gboolean *value))
{
	stub__mce_setting_get_bool_item_t *const items =
		stub__mce_setting_get_bool_items;

	for( int i = 0; items[i].key; ++i ) {
		if( strcmp(key, items[i].key) == 0 ) {
			*value = items[i].value;
			return TRUE;
		}
	}

	ck_abort_msg("Key not handled: '%s'", key);

	return FALSE;
}

static gboolean stub__mce_setting_set_bool(const gchar *key, gboolean value)
{
	typedef stub__mce_setting_notifier_data_t notifier_data_t;
	typedef stub__mce_setting_get_bool_item_t item_t;
	item_t *const items = stub__mce_setting_get_bool_items;
	GHashTable *const notifiers = stub__mce_setting_notifiers;

	item_t *item = NULL;
	for( int i = 0; items[i].key; ++i ) {
		if( strcmp(key, items[i].key) == 0 ) {
			item = items + i;
			break;
		}
	}
	ck_assert_msg(item != NULL, "Key not handled: '%s'", key);

	item->value = value;

	notifier_data_t *const notifier = g_hash_table_lookup(notifiers, key);
	if( notifier != NULL ) {
		GConfValue *const entry_value = gconf_value_new(GCONF_VALUE_BOOL);
		gconf_value_set_bool(entry_value, value);
		GConfEntry *const entry = gconf_entry_new(key, entry_value);

		(*notifier->callback)(NULL, notifier->cb_id, entry, NULL);

		gconf_entry_free(entry);
		gconf_value_free(entry_value);
	}

	ut_transition_recheck_schedule();

	return TRUE;
}

static void stub_mce_setting_setup_unchecked(void)
{
	stub__mce_setting_notifiers = g_hash_table_new_full(g_str_hash,
							  g_str_equal,
							  g_free, g_free);

	static stub__mce_setting_get_int_item_t int_items[] = {
		{
			MCE_SETTING_CPU_SCALING_GOVERNOR_PATH,
			/* governor_conf default */
			MCE_DEFAULT_CPU_SCALING_GOVERNOR,
		}, {
			MCE_SETTING_USE_AUTOSUSPEND_PATH,
			/* suspend_policy default */
			MCE_DEFAULT_USE_AUTOSUSPEND,
		}, {
			MCE_SETTING_DISPLAY_BRIGHTNESS_PATH,
			/* real_disp_brightness default */
			MCE_DEFAULT_DISPLAY_BRIGHTNESS,
		}, {
			MCE_SETTING_DISPLAY_BLANK_TIMEOUT_PATH,
			/* disp_blank_timeout default */
			MCE_DEFAULT_DISPLAY_BLANK_TIMEOUT,
		}, {
			MCE_SETTING_DISPLAY_NEVER_BLANK_PATH,
			/* disp_never_blank default */
			0,
		}, {
			MCE_SETTING_DISPLAY_ADAPTIVE_DIM_THRESHOLD_PATH,
			/* adaptive_dimming_threshold default */
			MCE_DEFAULT_DISPLAY_ADAPTIVE_DIM_THRESHOLD,
		}, {
			MCE_SETTING_DISPLAY_DIM_TIMEOUT_PATH,
			/* disp_dim_timeout default */
			MCE_DEFAULT_DISPLAY_DIM_TIMEOUT,
		}, {
			MCE_SETTING_BLANKING_INHIBIT_MODE_PATH,
			/* blanking_inhibit_mode default*/
			MCE_DEFAULT_BLANKING_INHIBIT_MODE,
		}, {
			NULL,
			0,
		}
	};
	stub__mce_setting_get_int_items = int_items;

	static stub__mce_setting_get_int_list_item_t int_list_items[] = {
		{
			MCE_SETTING_DISPLAY_DIM_TIMEOUT_LIST_PATH,
			{ 1, 5, 10, 15, 20, INT_MAX },
		}, {
			NULL,
			{ INT_MAX },
		}
	};
	stub__mce_setting_get_int_list_items = int_list_items;

	static stub__mce_setting_get_bool_item_t bool_items[] = {
		{
			MCE_SETTING_DISPLAY_ADAPTIVE_DIMMING_PATH,
			/* adaptive_dimming_enabled default */
			MCE_DEFAULT_DISPLAY_ADAPTIVE_DIMMING,
		}, {
			MCE_SETTING_USE_LOW_POWER_MODE_PATH,
			/* use_low_power_mode default */
			FALSE,
		}, {
			NULL,
			FALSE,
		}
	};
	stub__mce_setting_get_bool_items = bool_items;
}

static void stub_mce_setting_teardown_unchecked(void)
{
	g_hash_table_destroy(stub__mce_setting_notifiers),
		stub__mce_setting_notifiers = NULL;
}

/*
 * mce-io.c stubs {{{1
 */

#define STUB__MCE_FILE_IO_ITEM_DATA_MAX (64)

typedef struct stub__mce_io_item
{
	const gchar *file;
	gchar data[STUB__MCE_FILE_IO_ITEM_DATA_MAX];
	gint write_count;
} stub__mce_io_item_t;
static stub__mce_io_item_t *stub__mce_io_items = NULL;

EXTERN_STUB (
gboolean, mce_read_string_from_file, (const gchar *const file, gchar **string))
{
	stub__mce_io_item_t *const items =
		stub__mce_io_items;

	for( int i = 0; items[i].file; ++i ) {
		if( strcmp(file, items[i].file) == 0 ) {
			*string = g_strdup(items[i].data);
			return TRUE;
		}
	}

	ck_abort_msg("File not handled: '%s'", file);

	return FALSE;
}

EXTERN_STUB (
gboolean, mce_write_string_to_file, (const gchar *const file,
				     const gchar *const string))
{
	ck_assert(strlen(string) < STUB__MCE_FILE_IO_ITEM_DATA_MAX);

	stub__mce_io_item_t *const items =
		stub__mce_io_items;

	for( int i = 0; items[i].file; ++i ) {
		if( strcmp(file, items[i].file) == 0 ) {
			const int nwritten = snprintf(items[i].data,
					STUB__MCE_FILE_IO_ITEM_DATA_MAX,
					"%s", string);
			ck_assert(nwritten < STUB__MCE_FILE_IO_ITEM_DATA_MAX);

			++items[i].write_count;

			ut_transition_recheck_schedule();

			return TRUE;
		}
	}

	ck_abort_msg("File not handled: '%s'", file);

	return FALSE;
}

EXTERN_STUB (
gboolean, mce_read_number_string_from_file, (const gchar *const file,
					     gulong *number, FILE **fp,
					     gboolean rewind_file,
					     gboolean close_on_exit))
{
	ck_assert(fp == NULL);
	ck_assert(rewind_file == FALSE);
	ck_assert(close_on_exit == TRUE);

	stub__mce_io_item_t *const items =
		stub__mce_io_items;

	for( int i = 0; items[i].file; ++i ) {
		if( strcmp(file, items[i].file) == 0 ) {
			*number = atol(items[i].data);
			return TRUE;
		}
	}

	ck_abort_msg("File not handled: '%s'", file);

	return FALSE;
}

EXTERN_STUB (
gboolean, mce_write_number_string_to_file, (output_state_t *output,
					    const gulong number))
{
	ck_assert(output->truncate_file == TRUE);
	ck_assert(output->path != NULL);
	ck_assert(output->file == NULL);

	stub__mce_io_item_t *const items =
		stub__mce_io_items;

	for( int i = 0; items[i].file; ++i ) {
		if( strcmp(output->path, items[i].file) == 0 ) {
			const int nwritten = snprintf(items[i].data,
					STUB__MCE_FILE_IO_ITEM_DATA_MAX,
					"%lu", number);
			ck_assert(nwritten < STUB__MCE_FILE_IO_ITEM_DATA_MAX);

			++items[i].write_count;

			ut_transition_recheck_schedule();

			return TRUE;
		}
	}

	ck_abort_msg("File not handled: '%s'", output->path);

	return FALSE;
}

EXTERN_STUB (
void, mce_close_output, (output_state_t *output))
{
	output->file = 0;
}

static gint stub__mce_io_write_count(const gchar *file)
{
	stub__mce_io_item_t *const items =
		stub__mce_io_items;

	for( int i = 0; items[i].file; ++i ) {
		if( strcmp(file, items[i].file) == 0 ) {
			return items[i].write_count;
		}
	}

	ck_abort_msg("File not handled: '%s'", file);

	return 0;
}

static void stub_mce_io_setup_unchecked(void)
{
	static stub__mce_io_item_t items[] = {
		{
			/* cabc_available_modes_file */
			STUB__CABC_AVAILABLE_MODES_FILE,
			"",
			0,
		}, {
			/* cabc_mode_file */
			STUB__CABC_MODE_FILE,
			"",
			0,
		}, {
			/* max_brightness_file */
			STUB__MAX_BRIGHTNESS_FILE,
			"100",
			0,
		}, {
			/* brightness_output.path */
			STUB__BRIGHTNESS_OUTPUT_PATH,
			"20",
			0,
		}, {
			/* hw_fading_output.path */
			STUB__HW_FADING_OUTPUT_PATH,
			"",
			0,
		}, {
			/* high_brightness_mode_output.path */
			STUB__HIGH_BRIGHTNESS_MODE_OUTPUT_PATH,
			"",
			0,
		}, {
			/* low_power_mode_file */
			STUB__LOW_POWER_MODE_FILE,
			"",
			0,
		}, {
			NULL,
			"",
			0,
		}
	};
	stub__mce_io_items = items;
}

static void stub_mce_io_teardown_unchecked(void)
{
}

/*
 * mce-dbus.c stubs {{{1
 */

EXTERN_DUMMY_STUB (
DBusMessage *, dbus_new_method_reply, (DBusMessage *const message));

EXTERN_DUMMY_STUB (
DBusMessage *, dbus_new_signal, (const gchar *const path,
				 const gchar *const interface,
				 const gchar *const name));

EXTERN_DUMMY_STUB (
gboolean, dbus_send_message, (DBusMessage *const msg));

EXTERN_DUMMY_STUB (
gboolean, dbus_send, (const gchar *const service, const gchar *const path,
		      const gchar *const interface, const gchar *const name,
		      DBusPendingCallNotifyFunction callback,
		      int first_arg_type, ...));

static DBusConnection *stub__dbus_connection = NULL;

EXTERN_STUB (
DBusConnection *, dbus_connection_get, (void))
{
	if( !stub__dbus_connection ) {
		DBusBusType bus_type = DBUS_BUS_SYSTEM;
		DBusError error = DBUS_ERROR_INIT;
		if( (stub__dbus_connection = dbus_bus_get(bus_type,
							  &error)) == NULL ) {
			printf("%s: Failed to open connection to message bus; %s",
				G_STRFUNC, error.message);
			dbus_error_free(&error);
			abort();
		}

		dbus_connection_setup_with_g_main(stub__dbus_connection, NULL);
	}

	return dbus_connection_ref(stub__dbus_connection);
}

EXTERN_STUB (
gconstpointer, mce_dbus_handler_add, (const gchar *const interface,
				      const gchar *const name,
				      const gchar *const rules,
				      const guint type,
				      gboolean (*callback)(DBusMessage *const msg)))
{
	(void)interface;
	(void)name;
	(void)rules;
	(void)type;
	(void)callback;

	return (gconstpointer)1;
}

EXTERN_DUMMY_STUB (
gboolean, mce_dbus_is_owner_monitored, (const gchar *service,
					GSList *monitor_list));

EXTERN_STUB (
gssize, mce_dbus_owner_monitor_add, (const gchar *service,
				     gboolean (*callback)(DBusMessage *const msg),
				     GSList **monitor_list,
				     gssize max_num))
{
	(void)callback;

	gssize retval = -1;
	gssize num;

	/* If service or monitor_list is NULL, fail */
	ck_assert(service != NULL);
	ck_assert(monitor_list != NULL);

	/* If the service is already in the list, we're done */
	if( g_slist_find_custom(*monitor_list, service,
				(GCompareFunc)g_strcmp0) != NULL ) {
		retval = 0;
		goto EXIT;
	}

	/* If the service isn't in the list, and the list already
	 * contains max_num elements, bail out
	 */
	if( (num = g_slist_length(*monitor_list)) == max_num )
		goto EXIT;

	*monitor_list = g_slist_prepend(*monitor_list, g_strdup(service));
	retval = num + 1;

EXIT:
	return retval;
}

EXTERN_STUB (
gssize, mce_dbus_owner_monitor_remove, (const gchar *service,
					GSList **monitor_list))
{
	gssize retval = -1;
	GSList *tmp;

	/* If service or monitor_list is NULL, fail */
	ck_assert(service != NULL);
	ck_assert(monitor_list != NULL);

	/* If the service is not in the list, fail */
	if(  (tmp = g_slist_find_custom(*monitor_list, service,
					(GCompareFunc)g_strcmp0)) == NULL)
		goto EXIT;

	g_free(tmp->data);
	*monitor_list = g_slist_remove(*monitor_list, tmp->data);
	retval = g_slist_length(*monitor_list);

EXIT:
	return retval;
}

EXTERN_STUB (
void, mce_dbus_owner_monitor_remove_all, (GSList **monitor_list))
{
	void free_foreach(gpointer handler, gpointer user_data)
	{
		(void)user_data;
		g_free(handler);
	}

	if( (monitor_list != NULL) && (*monitor_list != NULL) ) {
		g_slist_foreach(*monitor_list, (GFunc)free_foreach, NULL);
		g_slist_free(*monitor_list);
		*monitor_list = NULL;
	}
}

/*
 * mce-sensorfw.c stubs {{{1
 */

EXTERN_STUB (
void, mce_sensorfw_suspend, (void))
{
}

EXTERN_STUB (
void, mce_sensorfw_resume, (void))
{
}

EXTERN_STUB (
void, mce_sensorfw_orient_enable, (void))
{
}

EXTERN_STUB (
void, mce_sensorfw_orient_disable, (void))
{
}

EXTERN_STUB (
void, mce_sensorfw_orient_set_notify, (void (*cb)(int state)))
{
	(void)cb;
}

/*
 * tklock.c stubs {{{1
 */

EXTERN_STUB (
void, mce_tklock_show_tklock_ui, (void))
{
	/* empty */
}

/*
 * libwakelock.c stubs {{{1
 */

static GHashTable *stub__wakelock_locks = NULL;

EXTERN_STUB (
void, wakelock_lock, (const char *name, long long ns))
{
	ck_assert(!g_hash_table_lookup_extended(stub__wakelock_locks, name,
						NULL, NULL));
	ck_assert_int_eq(ns, -1);

	g_hash_table_insert(stub__wakelock_locks,
			    g_strdup(name), NULL);

	ut_transition_recheck_schedule();
}

EXTERN_STUB (
void, wakelock_unlock, (const char *name))
{
	ck_assert(g_hash_table_lookup_extended(stub__wakelock_locks, name,
					       NULL, NULL));

	g_hash_table_remove(stub__wakelock_locks, name);

	ut_transition_recheck_schedule();
}

#if 0
static bool stub__wakelock_locked(const char *name)
{
	if( name == NULL )
		return g_hash_table_size(stub__wakelock_locks) != 0;
	else
		return g_hash_table_lookup_extended(stub__wakelock_locks, name,
						    NULL, NULL);
}
#endif

static gboolean stub__waitfb_event_cb(gpointer data);

EXTERN_STUB (
void, wakelock_allow_suspend, (void))
{
	g_idle_add(stub__waitfb_event_cb, GINT_TO_POINTER(TRUE));
}

EXTERN_STUB (
void, wakelock_block_suspend, (void))
{
	g_idle_add(stub__waitfb_event_cb, GINT_TO_POINTER(FALSE));
}

/* Stub init/cleanup */

static void stub_wakelock_setup_unchecked(void)
{
	stub__wakelock_locks = g_hash_table_new_full(g_str_hash, g_str_equal,
						     g_free, NULL);
}

static void stub_wakelock_teardown_unchecked(void)
{
	g_hash_table_destroy(stub__wakelock_locks), stub__wakelock_locks = NULL;
}

/*
 * filewatcher.c stubs {{{1
 */

EXTERN_DUMMY_STUB (
filewatcher_t *, filewatcher_create, (const char *dirpath,
				      const char *filename,
				      filewatcher_changed_fn change_cb,
				      gpointer user_data,
				      GDestroyNotify delete_cb));

EXTERN_DUMMY_STUB (
void, filewatcher_delete, (filewatcher_t *self));

EXTERN_DUMMY_STUB (
void, filewatcher_force_trigger, (filewatcher_t *self));

/*
 * }}}
 */

/* ------------------------------------------------------------------------- *
 * LOCAL STUBS
 * ------------------------------------------------------------------------- */

/*
 * Display HW related stubs {{{1
 */

LOCAL_DUMMY_STUB (
gboolean, get_brightness_controls, (const gchar *dirpath,
				    char **setpath, char **maxpath));

LOCAL_DUMMY_STUB (
gboolean, get_display_type_from_config, (display_type_t *display_type));

LOCAL_DUMMY_STUB (
gboolean, get_display_type_from_sysfs_probe, (display_type_t *display_type));

LOCAL_DUMMY_STUB (
gboolean, get_display_type_from_hybris, (display_type_t *display_type));

/*
 * Derived from get_display_type(), case DISPLAY_DISPLAY0
 */
LOCAL_STUB (
display_type_t, get_display_type, (void))
{
	static display_type_t display_type = DISPLAY_TYPE_UNSET;

	/* If we have the display type already, return it */
	if( display_type != DISPLAY_TYPE_UNSET )
		goto EXIT;

	display_type = DISPLAY_TYPE_DISPLAY0;

	brightness_output.path = g_strdup(STUB__BRIGHTNESS_OUTPUT_PATH);
	max_brightness_file = g_strdup(STUB__MAX_BRIGHTNESS_FILE);
	cabc_mode_file = g_strdup(STUB__CABC_MODE_FILE);
	cabc_available_modes_file = g_strdup(STUB__CABC_AVAILABLE_MODES_FILE);
	hw_fading_output.path = g_strdup(STUB__HW_FADING_OUTPUT_PATH);
	high_brightness_mode_output.path =
		g_strdup(STUB__HIGH_BRIGHTNESS_MODE_OUTPUT_PATH);
	low_power_mode_file = g_strdup(STUB__LOW_POWER_MODE_FILE);

	cabc_supported = TRUE;
	hw_fading_supported = TRUE;
	high_brightness_mode_supported = TRUE;
	low_power_mode_supported = TRUE;
	backlight_ioctl_hook = backlight_ioctl_default;

EXIT:
	return display_type;
}

#ifdef USE_LIBCAL
# error "FIXME: implement stub for update_display_timers()"
#endif

static int stub__backlight_ioctl_value_set = FB_BLANK_UNBLANK;

LOCAL_STUB (
gboolean, backlight_ioctl_default, (int value))
{
	stub__backlight_ioctl_value_set = value;

	ut_transition_recheck_schedule();

	return TRUE;
}

/*
 * CPU governor related stubs {{{1
 */

governor_setting_t *stub__governor_settings_default = NULL;
governor_setting_t *stub__governor_settings_interactive = NULL;

/* That one set with governor_apply_setting */
const governor_setting_t *stub__governor_settings_active = NULL;

LOCAL_STUB (
governor_setting_t *, governor_get_settings, (const char *tag))
{
	if( strcmp(tag, "Default") == 0 )
		return stub__governor_settings_default;
	else if( strcmp(tag, "Interactive") == 0 )
		return stub__governor_settings_interactive;
	else
		ck_abort_msg("Invalid tag: '%s'", tag);

	return NULL;
}

LOCAL_STUB (
void, governor_free_settings, (governor_setting_t *settings))
{
	ck_assert(settings == stub__governor_settings_default
		  || settings == stub__governor_settings_interactive);
}

LOCAL_DUMMY_STUB (
bool, governor_write_data, (const char *path, const char *data));

LOCAL_STUB (
void, governor_apply_setting, (const governor_setting_t *setting))
{
	ck_assert(setting == stub__governor_settings_default
		  || setting == stub__governor_settings_interactive);

	stub__governor_settings_active = setting;

	ut_transition_recheck_schedule();
}

static void stub_governor_setup_unchecked(void)
{
	static governor_setting_t default_setting[] = {
		{
			(char *)"/foo", /* safe */
			(char *)"default",
		}, {
			NULL,
			NULL,
		}
	};
	stub__governor_settings_default = default_setting;

	static governor_setting_t interactive_setting[] = {
		{
			(char *)"/foo",
			(char *)"interactive",
		}, {
			NULL,
			NULL,
		}
	};
	stub__governor_settings_interactive = interactive_setting;
}

static void stub_governor_teardown_unchecked(void)
{
}

/*
 * Device lock related stubs {{{1
 */

LOCAL_STUB (
void, inhibit_devicelock, (void))
{
	/* nothing */
}

/*
 * Lipstick related stubs {{{1
 */

static guint stub__renderer_set_state_id = 0;

static gboolean stub__renderer_set_state_cb(gpointer data)
{
	renderer_state_t state = GPOINTER_TO_INT(data);

	if( stub__renderer_set_state_id == 0 )
		return G_SOURCE_REMOVE;

	stub__renderer_set_state_id = 0;

	renderer_ui_state = state;

	stm_rethink_schedule();

	ut_transition_recheck_schedule();

	return G_SOURCE_REMOVE;
}

LOCAL_STUB (
void, renderer_cancel_state_set, (void))
{
	if( stub__renderer_set_state_id == 0 )
		return;

	g_source_remove(stub__renderer_set_state_id),
		stub__renderer_set_state_id = 0;
}

LOCAL_STUB (
gboolean, renderer_set_state, (renderer_state_t state))
{
	renderer_ui_state = RENDERER_UNKNOWN;

	stub__renderer_set_state_id = g_idle_add(stub__renderer_set_state_cb,
						 GINT_TO_POINTER(state));

	return TRUE;
}

/*
 * Display status {{{1
 */

LOCAL_STUB (
gboolean, send_display_status, (DBusMessage *const method_call))
{
	ck_assert(method_call == NULL);

	return TRUE;
}

/*
 * Initi-done tracking stubs {{{1
 */

static gboolean stub__desktop_ready_cb_proxy(gpointer user_data)
{
	gboolean res = desktop_ready_cb(user_data);

	ut_transition_recheck_schedule();

	return res;
}

LOCAL_STUB (
void, init_done_start_tracking, (void))
{
	init_done = TRUE;
	desktop_ready_id = g_timeout_add_seconds(1, stub__desktop_ready_cb_proxy, 0);
}

LOCAL_STUB (
void, init_done_stop_tracking, (void))
{
	if( desktop_ready_id ) {
		g_source_remove(desktop_ready_id);
		desktop_ready_id = 0;
	}
}

/*
 * DBus name owner tracking stubs {{{1
 */

LOCAL_STUB (
void, dbusname_init, (void))
{
	for( int i = 0; dbusname_lut[i].name; ++i ) {
		dbusname_owner_changed(dbusname_lut[i].name, NULL, "foo");
	}
}

LOCAL_STUB (
void, dbusname_quit, (void))
{
	/* empty */
}

/*
 * Waitfb related stubs {{{1
 */

LOCAL_STUB (
void, waitfb_cancel, (waitfb_t *self))
{
	ck_assert(self == &waitfb);

	self->thread = 0;
	self->finished = TRUE;
}

/* Invoked from wakelock_allow_suspend() / wakelock_block_suspend() stubs */
static gboolean stub__waitfb_event_cb(gpointer data)
{
	waitfb.suspended = GPOINTER_TO_INT(data);
	stm_rethink_schedule();
	ut_transition_recheck_schedule();
	return G_SOURCE_REMOVE;
}

LOCAL_STUB (
gboolean, waitfb_start, (waitfb_t *self))
{
	ck_assert(self == &waitfb);

	self->thread = (pthread_t)-1;
	self->finished = FALSE;

	return TRUE;
}

/*
 * Display brightness filtering {{{1
 */

/*
 * Based on filter-brightness-als.c. The value passed into
 * display_brightness_pipe is in range [1, 5]. During normal execution the value
 * is filtered -- converted to percentage -- by a filter-brightness module.
 * During test execution, the module is not loaded and so the filter is
 * implemented as part of stub code here.
 *
 * TODO: bug in filter-brightness-simple.c? display_state_curr is never changed.
 */
static gpointer stub__display_brightness_filter(gpointer data)
{
	int setting = GPOINTER_TO_INT(data);

	if( setting < 1 ) setting = 1; else
	if( setting > 5 ) setting = 5;

	int brightness = setting * 20;

	return GINT_TO_POINTER(brightness);
}

/*
 * }}}
 */

/*
 * Recheck transitions upon datapipe execution {{{1
 */

static void ut_display_state_pipe_trigger_transition_recheck(gconstpointer data)
{
	(void)data;
	ut_transition_recheck_schedule();
}

/*
 * Stub setup/teardown {{{1
 */

GMainLoop *stub__main_loop = NULL;

static void stub_setup_checked(void)
{
	g_type_init();

	stub__main_loop = g_main_loop_new(NULL, TRUE);

	/* Setup all datapipes - copy & paste from mce's main() */
	datapipe_init(&system_state_pipe, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(MCE_SYSTEM_STATE_UNDEF));
	datapipe_init(&master_radio_enabled_pipe, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&call_state_pipe, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(CALL_STATE_NONE));
	datapipe_init(&call_type_pipe, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(CALL_TYPE_NORMAL));
	datapipe_init(&alarm_ui_state_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(MCE_ALARM_UI_INVALID_INT32));
	datapipe_init(&submode_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(MCE_SUBMODE_NORMAL));
	datapipe_init(&display_state_curr_pipe, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(MCE_DISPLAY_UNDEF));
	datapipe_init(&display_state_request_pipe, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(MCE_DISPLAY_UNDEF));
	datapipe_init(&display_brightness_pipe, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&led_brightness_pipe, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&led_pattern_activate_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_DYNAMIC,
		      0, NULL);
	datapipe_init(&led_pattern_deactivate_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_DYNAMIC,
		      0, NULL);
	datapipe_init(&key_backlight_brightness_pipe, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&keypress_event_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_DYNAMIC,
		      sizeof (struct input_event), NULL);
	datapipe_init(&touchscreen_event_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_DYNAMIC,
		      sizeof (struct input_event), NULL);
	datapipe_init(&device_inactive_pipe, DATAPIPE_FILTERING_ALLOWED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(FALSE));
	datapipe_init(&lockkey_state_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&keyboard_slide_state_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&lid_sensor_actual_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&lens_cover_state_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&proximity_sensor_actual_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&tklock_request_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(TKLOCK_REQUEST_UNDEF));
	datapipe_init(&charger_state_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&battery_status_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(BATTERY_STATUS_UNDEF));
	datapipe_init(&battery_level_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(100));
	datapipe_init(&camera_button_state_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(CAMERA_BUTTON_UNDEF));
	datapipe_init(&inactivity_delay_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(DEFAULT_INACTIVITY_DELAY));
	datapipe_init(&audio_route_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(AUDIO_ROUTE_UNDEF));
	datapipe_init(&usb_cable_state_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&jack_sense_state_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&power_saving_mode_active_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));
	datapipe_init(&thermal_state_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(THERMAL_STATE_UNDEF));
	datapipe_init(&heartbeat_event_pipe, DATAPIPE_FILTERING_DENIED, DATAPIPE_DATA_LITERAL,
		      0, GINT_TO_POINTER(0));

	datapipe_add_filter(&display_brightness_pipe,
			    stub__display_brightness_filter);

	g_module_check_init(NULL);

	datapipe_add_output_trigger(&display_state_curr_pipe,
				    ut_display_state_pipe_trigger_transition_recheck);
}

static void stub_teardown_checked(void)
{
	datapipe_remove_output_trigger(&display_state_curr_pipe,
				       ut_display_state_pipe_trigger_transition_recheck);

	g_module_unload(NULL);

	datapipe_remove_filter(&display_brightness_pipe,
			       stub__display_brightness_filter);

	/* Free all datapipes - copy & paste from mce's main() */
	datapipe_free(&thermal_state_pipe);
	datapipe_free(&power_saving_mode_active_pipe);
	datapipe_free(&jack_sense_state_pipe);
	datapipe_free(&usb_cable_state_pipe);
	datapipe_free(&audio_route_pipe);
	datapipe_free(&inactivity_delay_pipe);
	datapipe_free(&battery_level_pipe);
	datapipe_free(&battery_status_pipe);
	datapipe_free(&charger_state_pipe);
	datapipe_free(&tklock_request_pipe);
	datapipe_free(&proximity_sensor_actual_pipe);
	datapipe_free(&lens_cover_state_pipe);
	datapipe_free(&lid_sensor_actual_pipe);
	datapipe_free(&keyboard_slide_state_pipe);
	datapipe_free(&lockkey_state_pipe);
	datapipe_free(&device_inactive_pipe);
	datapipe_free(&touchscreen_event_pipe);
	datapipe_free(&keypress_event_pipe);
	datapipe_free(&key_backlight_brightness_pipe);
	datapipe_free(&led_pattern_deactivate_pipe);
	datapipe_free(&led_pattern_activate_pipe);
	datapipe_free(&led_brightness_pipe);
	datapipe_free(&display_brightness_pipe);
	datapipe_free(&display_state_curr_pipe);
	datapipe_free(&submode_pipe);
	datapipe_free(&alarm_ui_state_pipe);
	datapipe_free(&call_type_pipe);
	datapipe_free(&call_state_pipe);
	datapipe_free(&master_radio_enabled_pipe);
	datapipe_free(&system_state_pipe);
	datapipe_free(&heartbeat_event_pipe);
}

/*
 * }}}
 */

/* ------------------------------------------------------------------------- *
 * TESTS
 * ------------------------------------------------------------------------- */

/*
 * Common state tests {{{1
 *
 * Implements ut_state_test_t.
 */

static gboolean ut_is_desktop_ready(gpointer user_data)
{
	(void)user_data;
	return desktop_ready_id == 0;
}

static display_state_t ut_trigerred_display_state = MCE_DISPLAY_UNDEF;
static void ut_store_trigerred_display_state_trigger(gconstpointer data)
{
	ut_trigerred_display_state = GPOINTER_TO_INT(data);
}

static gboolean ut_is_display_state_eq(gpointer user_data)
{
	display_state_t wanted = GPOINTER_TO_INT(user_data);

	return ut_trigerred_display_state == wanted;
}

static gboolean ut_is_sysfs_brightness_eq(gpointer user_data)
{
	guint wanted = GPOINTER_TO_INT(user_data);

	gulong current = -1;
	gboolean ok = mce_read_number_string_from_file(brightness_output.path,
						       &current,
						       NULL, FALSE, TRUE);
	ck_assert(ok);

	return current == wanted;
}

/* State tests setup/teardown */

static void ut_state_tests_setup_checked(void)
{
	datapipe_add_output_trigger(&display_state_curr_pipe,
				    ut_store_trigerred_display_state_trigger);
}

static void ut_state_tests_teardown_checked(void)
{
	datapipe_remove_output_trigger(&display_state_curr_pipe,
				       ut_store_trigerred_display_state_trigger);
}

/*
 * Utilies {{{1
 */

static gint ut_nth_possible_dim_timeout(int n)
{
	gpointer *nth = g_slist_nth_data(possible_dim_timeouts, n);
	return nth == NULL ? INT_MAX : GPOINTER_TO_INT(nth);
}

/*
 * Common prelude {{{1
 */

static void ut_run_to_user_state(void)
{
	datapipe_exec_full(&system_state_pipe, GINT_TO_POINTER(MCE_SYSTEM_STATE_USER),
			   DATAPIPE_CACHE_INDATA);

	ut_assert_transition(ut_is_desktop_ready, NULL);
	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_ON));
}

/*
 * }}}
 */

START_TEST (ut_check_basic_state_change_no_lpm)
{
	ut_run_to_user_state();

	struct state_change
	{
		display_state_t required;
		display_state_t expected;
	} state_changes[] = {
		{ MCE_DISPLAY_OFF,     MCE_DISPLAY_OFF   },
		{ MCE_DISPLAY_ON,      MCE_DISPLAY_ON    },
		{ MCE_DISPLAY_DIM,     MCE_DISPLAY_DIM   },
		{ MCE_DISPLAY_ON,      MCE_DISPLAY_ON    },
		{ MCE_DISPLAY_LPM_ON,  MCE_DISPLAY_OFF   },
		{ MCE_DISPLAY_ON,      MCE_DISPLAY_ON    },
		{ MCE_DISPLAY_LPM_OFF, MCE_DISPLAY_OFF   },
		{ MCE_DISPLAY_ON,      MCE_DISPLAY_ON    },
		{ MCE_DISPLAY_DIM,     MCE_DISPLAY_DIM   },
		{ MCE_DISPLAY_LPM_ON,  MCE_DISPLAY_OFF   },
		{ MCE_DISPLAY_LPM_OFF, MCE_DISPLAY_OFF   },
		{ MCE_DISPLAY_LPM_ON,  MCE_DISPLAY_OFF   },
		{ MCE_DISPLAY_OFF,     MCE_DISPLAY_OFF   },
		{ MCE_DISPLAY_ON,      MCE_DISPLAY_ON    },
		{ MCE_DISPLAY_OFF,     MCE_DISPLAY_OFF   },
		{ MCE_DISPLAY_LPM_ON,  MCE_DISPLAY_OFF   },
		{ MCE_DISPLAY_ON,      MCE_DISPLAY_ON    },
		{ MCE_DISPLAY_OFF,     MCE_DISPLAY_OFF   },
		{ MCE_DISPLAY_DIM,     MCE_DISPLAY_DIM   },
		{ MCE_DISPLAY_UNDEF,   MCE_DISPLAY_UNDEF },
	};

	for( int i = 0; state_changes[i].required != MCE_DISPLAY_UNDEF; ++i ) {
		gint current = display_state_get(); /* needed */
		mce_log(LL_DEBUG, "%d: %s -> %s, expect %s", i,
			display_state_name(current),
			display_state_name(state_changes[i].required),
			display_state_name(state_changes[i].expected));

		mce_datapipe_request_display_state(state_changes[i].required);
		ut_assert_transition(ut_is_display_state_eq,
				     GINT_TO_POINTER(state_changes[i].expected));
	}
}
END_TEST

START_TEST (ut_check_basic_state_change)
{
	stub__mce_setting_set_bool(MCE_SETTING_USE_LOW_POWER_MODE_PATH, TRUE);

	ut_run_to_user_state();

	struct state_change
	{
		display_state_t required;
		display_state_t expected;
	} state_changes[] = {
		{ MCE_DISPLAY_OFF,     MCE_DISPLAY_LPM_OFF },
		{ MCE_DISPLAY_ON,      MCE_DISPLAY_ON      },
		{ MCE_DISPLAY_DIM,     MCE_DISPLAY_DIM     },
		{ MCE_DISPLAY_ON,      MCE_DISPLAY_ON      },
		{ MCE_DISPLAY_LPM_ON,  MCE_DISPLAY_LPM_ON  },
		{ MCE_DISPLAY_ON,      MCE_DISPLAY_ON      },
		{ MCE_DISPLAY_LPM_OFF, MCE_DISPLAY_LPM_OFF },
		{ MCE_DISPLAY_ON,      MCE_DISPLAY_ON      },
		{ MCE_DISPLAY_DIM,     MCE_DISPLAY_DIM     },
		{ MCE_DISPLAY_LPM_ON,  MCE_DISPLAY_LPM_ON  },
		{ MCE_DISPLAY_LPM_OFF, MCE_DISPLAY_LPM_OFF },
		{ MCE_DISPLAY_LPM_ON,  MCE_DISPLAY_LPM_ON  },
		{ MCE_DISPLAY_OFF,     MCE_DISPLAY_LPM_OFF },
		{ MCE_DISPLAY_ON,      MCE_DISPLAY_ON      },
		{ MCE_DISPLAY_OFF,     MCE_DISPLAY_LPM_OFF },
		{ MCE_DISPLAY_LPM_ON,  MCE_DISPLAY_LPM_ON  },
		{ MCE_DISPLAY_ON,      MCE_DISPLAY_ON      },
		{ MCE_DISPLAY_OFF,     MCE_DISPLAY_LPM_OFF },
		{ MCE_DISPLAY_DIM,     MCE_DISPLAY_DIM     },
		{ MCE_DISPLAY_UNDEF,   MCE_DISPLAY_UNDEF   },
	};

	for( int i = 0; state_changes[i].required != MCE_DISPLAY_UNDEF; ++i ) {
		gint current = display_state_get(); /* needed */
		mce_log(LL_DEBUG, "%d: %s -> %s, expect %s", i,
			display_state_name(current),
			display_state_name(state_changes[i].required),
			display_state_name(state_changes[i].expected));

		mce_datapipe_request_display_state(state_changes[i].required);
		ut_assert_transition(ut_is_display_state_eq,
				     GINT_TO_POINTER(state_changes[i].expected));
	}
}
END_TEST

START_TEST (ut_check_auto_blank_no_lpm)
{
	const gint set_disp_blank_timeout = 2;
	stub__mce_setting_set_int(MCE_SETTING_DISPLAY_BLANK_TIMEOUT_PATH,
				set_disp_blank_timeout);

	ut_run_to_user_state();

	mce_datapipe_request_display_state(MCE_DISPLAY_DIM);
	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_DIM));

	ut_assert_transition_time_eq(ut_is_display_state_eq,
				     GINT_TO_POINTER(MCE_DISPLAY_OFF),
				     set_disp_blank_timeout);
}
END_TEST

START_TEST (ut_check_auto_blank)
{
	/* TODO: dead code?
	 *  - setup_blank_timeout() is only called from LPM_ON.
	 *  - DEFAULT_LPM_BLANK_TIMEOUT is 0 (disabled).
	 *  - no way to change disp_lpm_blank_timeout from outside
	 *    - no API, no setting
	 */
	stub__mce_setting_set_bool(MCE_SETTING_USE_LOW_POWER_MODE_PATH, TRUE);
	const gint set_disp_lpm_blank_timeout = 2;
	disp_lpm_blank_timeout = set_disp_lpm_blank_timeout;

	ut_run_to_user_state();

	mce_datapipe_request_display_state(MCE_DISPLAY_LPM_ON);
	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_LPM_ON));

	ut_assert_transition_time_eq(ut_is_display_state_eq,
				     GINT_TO_POINTER(MCE_DISPLAY_LPM_OFF),
				     set_disp_lpm_blank_timeout);
}
END_TEST

START_TEST (ut_check_auto_dim_not_adaptive)
{
	stub__mce_setting_set_bool(MCE_SETTING_DISPLAY_ADAPTIVE_DIMMING_PATH,
				 FALSE);

	const gint set_disp_dim_timeout = 2;
	stub__mce_setting_set_int(MCE_SETTING_DISPLAY_DIM_TIMEOUT_PATH,
				set_disp_dim_timeout);

	ut_run_to_user_state();

	ut_assert_transition_time_eq(ut_is_display_state_eq,
				     GINT_TO_POINTER(MCE_DISPLAY_DIM),
				     set_disp_dim_timeout);
}
END_TEST

START_TEST (ut_check_auto_dim)
{
	ck_assert(adaptive_dimming_enabled);

	ut_run_to_user_state();

	/* We want dim_timeout_index=forced_dti (points into
	 * possible_dim_timeouts[]).  dim_timeout_index is computed with
	 * find_dim_timeout_index() which finds index with value closest to
	 * dim_timeout.  That is why we set
	 * dim_timeout=possible_dim_timeouts[forced_dti] */
	const gint forced_dti = 1;
	stub__mce_setting_set_int(MCE_SETTING_DISPLAY_DIM_TIMEOUT_PATH,
				ut_nth_possible_dim_timeout(forced_dti));

	ck_assert(ut_is_display_state_eq(GINT_TO_POINTER(MCE_DISPLAY_ON)));

	/* At the begin dim_timeout_index=forced_dti (see above) and
	 * adaptive_dimming_index=0. Every time activity is generated
	 * adaptive_dimming_index should get incremented. We will verify it by
	 * meassuring time to re-enter DIM */
	const int n_times_activity_generated = 2;

	for( int i = 0; i <= n_times_activity_generated; ++i ) {
		/* Verify adaptive_dimming_index is incremented as expected by
		 * meassuring time to re-enter DIM */
		gdouble expected_dim_time =
			ut_nth_possible_dim_timeout(forced_dti + i);
		ut_assert_transition_time_eq(ut_is_display_state_eq,
					     GINT_TO_POINTER(MCE_DISPLAY_DIM),
					     expected_dim_time);

		/* Generate activity so adaptive_dimming_index gets inc. */
		datapipe_exec_full(&inactivity_event_pipe, GINT_TO_POINTER(FALSE),
				   DATAPIPE_CACHE_OUTDATA);
		ut_assert_transition(ut_is_display_state_eq,
				     GINT_TO_POINTER(MCE_DISPLAY_ON));
	}
}
END_TEST

START_TEST (ut_check_adaptive_dim_timeout)
{
	ck_assert(adaptive_dimming_enabled);

	gint expected_dim_time = 0;

	ut_run_to_user_state();

	/* We want dim_timeout_index=forced_dti (points into
	 * possible_dim_timeouts[]).  dim_timeout_index is computed with
	 * find_dim_timeout_index() which finds index with value closest to
	 * dim_timeout.  That is why we set
	 * dim_timeout=possible_dim_timeouts[forced_dti] */
	const gint forced_dti = 1;
	stub__mce_setting_set_int(MCE_SETTING_DISPLAY_DIM_TIMEOUT_PATH,
				ut_nth_possible_dim_timeout(forced_dti));

	/* Delay DIM -> OFF so it does not cancel_adaptive_dimming_timeout() and
	 * reset adaptive_dimming_index to 0 */
	stub__mce_setting_set_int(MCE_SETTING_DISPLAY_BLANK_TIMEOUT_PATH,
				adaptive_dimming_threshold / 1000 * 10);

	ck_assert(ut_is_display_state_eq(GINT_TO_POINTER(MCE_DISPLAY_ON)));

	/* At the begin dim_timeout_index=forced_dti (see above) and
	 * adaptive_dimming_index=0 */
	expected_dim_time = ut_nth_possible_dim_timeout(forced_dti + 0);
	ut_assert_transition_time_eq(ut_is_display_state_eq,
				     GINT_TO_POINTER(MCE_DISPLAY_DIM),
				     expected_dim_time);

	/* Generating activity the adaptive_dimming_index gets incremented */
	datapipe_exec_full(&inactivity_event_pipe, GINT_TO_POINTER(FALSE),
			   DATAPIPE_CACHE_OUTDATA);
	/* Verify adaptive_dimming_index=1 by meassuring rime to re-enter DIM */
	expected_dim_time = ut_nth_possible_dim_timeout(forced_dti + 1);
	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_ON));
	ut_assert_transition_time_eq(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_DIM),
			     expected_dim_time);

	/* Waiting less than adaptive_dimming_threshold, the
	 * adaptive_dimming_index should remain at 1 */
	ut_wait_seconds((gdouble)adaptive_dimming_threshold / 1000
			- UT_COMPARE_TIME_TRESHOLD);
	/* Verify adaptive_dimming_index=1 by meassuring time to re-enter DIM */
	expected_dim_time = ut_nth_possible_dim_timeout(forced_dti + 1);
	mce_datapipe_request_display_state(MCE_DISPLAY_ON);
	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_ON));
	ut_assert_transition_time_eq(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_DIM),
			     expected_dim_time);

	/* Waiting longer than adaptive_dimming_threshold, the
	 * adaptive_dimming_index should be reset to 0 */
	ut_wait_seconds((gdouble)adaptive_dimming_threshold / 1000
			+ UT_COMPARE_TIME_TRESHOLD);
	/* Verify adaptive_dimming_index=0 by meassuring time to re-enter DIM */
	expected_dim_time = ut_nth_possible_dim_timeout(forced_dti + 0);
	mce_datapipe_request_display_state(MCE_DISPLAY_ON);
	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_ON));
	ut_assert_transition_time_eq(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_DIM),
			     expected_dim_time);
}
END_TEST

START_TEST (ut_check_auto_dim_malf)
{
	stub__mce_setting_set_bool(MCE_SETTING_DISPLAY_ADAPTIVE_DIMMING_PATH,
				 FALSE);
	const gint set_disp_dim_timeout = 2;
	stub__mce_setting_set_int(MCE_SETTING_DISPLAY_DIM_TIMEOUT_PATH,
				set_disp_dim_timeout);

	ut_run_to_user_state();

	mce_add_submode_int32(MCE_SUBMODE_MALF);

	ut_assert_transition_time_eq(ut_is_display_state_eq,
				     GINT_TO_POINTER(MCE_DISPLAY_OFF),
				     set_disp_dim_timeout);
}
END_TEST

START_TEST (ut_check_auto_lpm)
{
	stub__mce_setting_set_bool(MCE_SETTING_USE_LOW_POWER_MODE_PATH, TRUE);

	/* disp_lpm_timeout == disp_blank_timeout */
	const gint set_disp_lpm_timeout = 2;
	stub__mce_setting_set_int(MCE_SETTING_DISPLAY_BLANK_TIMEOUT_PATH,
				set_disp_lpm_timeout);

	ut_run_to_user_state();

	mce_datapipe_request_display_state(MCE_DISPLAY_DIM);
	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_DIM));

	ut_assert_transition_time_eq(ut_is_display_state_eq,
				     GINT_TO_POINTER(MCE_DISPLAY_LPM_ON),
				     set_disp_lpm_timeout);
}
END_TEST

START_TEST (ut_check_brightness)
{
	ut_run_to_user_state();

	guint brightnesses[] = {
		1, 2, 3, 4, 5,
		4, 3, 2, 1,
		3, 5, 2, 4,
		1, 3, 1, 5, 1
	};
	gint nvalues = sizeof(brightnesses) / sizeof(brightnesses[0]);

	guint sysfs_brightness(guint brightness)
	{
		/* see stub__display_brightness_filter() */
		return (brightness * maximum_display_brightness) / 5;
	}

	for( int i = 0; i < nvalues; ++i ) {
		stub__mce_setting_set_int(MCE_SETTING_DISPLAY_BRIGHTNESS_PATH,
					brightnesses[i]);

		guint expected = sysfs_brightness(brightnesses[i]);
		ut_assert_transition(ut_is_sysfs_brightness_eq,
				     GINT_TO_POINTER(expected));
	}
}
END_TEST

#define DATA(display_state_curr) {   \
        "{ "#display_state_curr" }", \
        display_state_curr }
static struct ut_check_blanking_pause_data
{
	const gchar 	 *tag;

        display_state_t  initial_display_state;

} ut_check_blanking_pause_data[] = {
	/* TODO: Display is not turned on on request_display_blanking_pause() */
	/*DATA( MCE_DISPLAY_OFF     ),*/
	/*DATA( MCE_DISPLAY_LPM_OFF ),*/
	/*DATA( MCE_DISPLAY_LPM_ON  ),*/
	/*DATA( MCE_DISPLAY_DIM     ),*/
	DATA( MCE_DISPLAY_ON      ),
	/* vim: AlignCtrl =<><p010P100 [(,)] */
};
#undef DATA

static const int ut_check_blanking_pause_data_count =
	sizeof(ut_check_blanking_pause_data)
	/ sizeof(struct ut_check_blanking_pause_data);

START_TEST (ut_check_blanking_pause)
{
	ck_assert_int_lt(_i, ut_check_blanking_pause_data_count);

	struct ut_check_blanking_pause_data *const data =
		ut_check_blanking_pause_data;

	printf("data: %s\n", data[_i].tag);

	ut_run_to_user_state();

	const gint set_blank_prevent_timeout = 3;
	blank_prevent_timeout = set_blank_prevent_timeout;

	stub__mce_setting_set_bool(MCE_SETTING_DISPLAY_ADAPTIVE_DIMMING_PATH,
				 FALSE);

	const gint set_disp_dim_timeout = 2;
	stub__mce_setting_set_int(MCE_SETTING_DISPLAY_DIM_TIMEOUT_PATH,
				set_disp_dim_timeout);

	mce_datapipe_request_display_state(data[_i].initial_display_state);
	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(data[_i].initial_display_state));

	request_display_blanking_pause();

	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_ON));

	ut_assert_transition_time_eq(ut_is_display_state_eq,
				     GINT_TO_POINTER(MCE_DISPLAY_DIM),
				     set_blank_prevent_timeout
				     + set_disp_dim_timeout);
}
END_TEST

#define DATA(constant_time, change) {                           \
        "{ constant_time="#constant_time", change="#change" }", \
        constant_time, change }
static struct ut_check_sw_fading_data
{
	const gchar 	 *tag;

        gboolean constant_time;
	int change;

} ut_check_sw_fading_data[] = {
	DATA( TRUE ,  1 ),
	DATA( TRUE , -1 ),
	DATA( FALSE,  1 ),
	DATA( FALSE, -1 ),
	/* vim: AlignCtrl =<<><llrp0010P1100 [(,)] */
};
#undef DATA

static const int ut_check_sw_fading_data_count =
	sizeof(ut_check_sw_fading_data)
	/ sizeof(struct ut_check_sw_fading_data);

START_TEST (ut_check_sw_fading)
{
	ck_assert_int_lt(_i, ut_check_sw_fading_data_count);

	struct ut_check_sw_fading_data *const data =
		ut_check_sw_fading_data;

	printf("data: %s\n", data[_i].tag);

	ut_run_to_user_state();

	/* Set initial brightness to 60% so there is some space above/bellow */
	const gint start_brightness = 3;
	datapipe_exec_full(&display_brightness_pipe,
			   GINT_TO_POINTER(start_brightness),
			   DATAPIPE_CACHE_INDATA);
	ut_assert_transition(ut_is_sysfs_brightness_eq,
			     GINT_TO_POINTER(start_brightness * 20));

	/* Setup global state */
	gint expected_time;
	if( data[_i].change > 0 ) {
		if( data[_i].constant_time ) {
			brightness_increase_policy =
				BRIGHTNESS_CHANGE_CONSTANT_TIME;
			expected_time = brightness_increase_constant_time;
		} else {
			brightness_increase_policy =
				BRIGHTNESS_CHANGE_STEP_TIME;
			expected_time = brightness_increase_step_time * 20;
		}
	} else {
		if( data[_i].constant_time ) {
			brightness_decrease_policy =
				BRIGHTNESS_CHANGE_CONSTANT_TIME;
			expected_time = brightness_decrease_constant_time;
		} else {
			brightness_decrease_policy =
				BRIGHTNESS_CHANGE_STEP_TIME;
			expected_time = brightness_decrease_step_time * 20;
		}
	}

	/* Activate tested code path */
	hw_fading_supported = FALSE;

	/* Execute and evaluate brightness change */
	const gint start_brightness_write_count =
		stub__mce_io_write_count(STUB__BRIGHTNESS_OUTPUT_PATH);

	datapipe_exec_full(&display_brightness_pipe,
			   GINT_TO_POINTER(start_brightness + data[_i].change),
			   DATAPIPE_CACHE_INDATA);
	ut_assert_transition_time_eq(ut_is_sysfs_brightness_eq,
				     GINT_TO_POINTER(20 * (start_brightness +
							   data[_i].change)),
				     expected_time / 1000);

	const gint brightness_write_count =
		stub__mce_io_write_count(STUB__BRIGHTNESS_OUTPUT_PATH) -
		start_brightness_write_count;

	ck_assert_int_eq(brightness_write_count, 20);
}
END_TEST

START_TEST (ut_check_set_use_lpm_while_off)
{
	ut_run_to_user_state();

	mce_datapipe_request_display_state(MCE_DISPLAY_OFF);
	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_OFF));

	stub__mce_setting_set_bool(MCE_SETTING_USE_LOW_POWER_MODE_PATH, TRUE);

	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_LPM_ON));
}
END_TEST

#define DATA(on) { "{ lpm_on="#on" }", on }
struct ut_check_unset_use_lpm_while_lpm_data
{
	const gchar *tag;

	gboolean lpm_on;
} ut_check_unset_use_lpm_while_lpm_data[] = {
	DATA( FALSE ),
	DATA( TRUE  ),
};

static const int ut_check_unset_use_lpm_while_lpm_data_count =
	sizeof(ut_check_unset_use_lpm_while_lpm_data) /
	sizeof(struct ut_check_unset_use_lpm_while_lpm_data);

START_TEST (ut_check_unset_use_lpm_while_lpm)
{
	ck_assert_int_lt(_i, ut_check_unset_use_lpm_while_lpm_data_count);

	struct ut_check_unset_use_lpm_while_lpm_data *const data =
		ut_check_unset_use_lpm_while_lpm_data;

	printf("data: %s\n", data[_i].tag);

	ut_run_to_user_state();

	stub__mce_setting_set_bool(MCE_SETTING_USE_LOW_POWER_MODE_PATH, TRUE);

	display_type_t required_lpm_state = data[_i].lpm_on
		? MCE_DISPLAY_LPM_ON
		: MCE_DISPLAY_LPM_OFF;
	mce_datapipe_request_display_state(required_lpm_state);
	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(required_lpm_state));

	stub__mce_setting_set_bool(MCE_SETTING_USE_LOW_POWER_MODE_PATH, FALSE);

	ut_assert_transition(ut_is_display_state_eq,
			     GINT_TO_POINTER(MCE_DISPLAY_OFF));
}
END_TEST

static Suite *ut_display_suite (void)
{
	Suite *s = suite_create ("ut_display");

	TCase *tc_core = tcase_create ("core");
	tcase_set_timeout(tc_core, 300);
	tcase_add_unchecked_fixture(tc_core, stub_mce_conf_setup_unchecked,
				    stub_mce_conf_teardown_unchecked);
	tcase_add_unchecked_fixture(tc_core, stub_mce_setting_setup_unchecked,
				    stub_mce_setting_teardown_unchecked);
	tcase_add_unchecked_fixture(tc_core, stub_mce_io_setup_unchecked,
				    stub_mce_io_teardown_unchecked);
	tcase_add_unchecked_fixture(tc_core, stub_wakelock_setup_unchecked,
				    stub_wakelock_teardown_unchecked);
	tcase_add_unchecked_fixture(tc_core, stub_governor_setup_unchecked,
				    stub_governor_teardown_unchecked);
	tcase_add_checked_fixture(tc_core, stub_setup_checked,
				  stub_teardown_checked);
	tcase_add_checked_fixture(tc_core, ut_state_tests_setup_checked,
				  ut_state_tests_teardown_checked);

	tcase_add_test (tc_core, ut_check_basic_state_change_no_lpm);
	tcase_add_test (tc_core, ut_check_basic_state_change);
	tcase_add_test (tc_core, ut_check_auto_blank_no_lpm);
	tcase_add_test (tc_core, ut_check_auto_blank);
	tcase_add_test (tc_core, ut_check_auto_dim_not_adaptive);
	tcase_add_test (tc_core, ut_check_auto_dim);
	tcase_add_test (tc_core, ut_check_adaptive_dim_timeout);
	tcase_add_test (tc_core, ut_check_auto_dim_malf);
	tcase_add_test (tc_core, ut_check_auto_lpm);
	tcase_add_test (tc_core, ut_check_brightness);
	tcase_add_loop_test (tc_core, ut_check_blanking_pause,
			     0, ut_check_blanking_pause_data_count);
	tcase_add_loop_test (tc_core, ut_check_sw_fading,
			     0, ut_check_sw_fading_data_count);
	tcase_add_test (tc_core, ut_check_set_use_lpm_while_off);
	tcase_add_loop_test (tc_core, ut_check_unset_use_lpm_while_lpm,
			     0, ut_check_unset_use_lpm_while_lpm_data_count);

	suite_add_tcase (s, tc_core);

	return s;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	int number_failed;
	Suite *s = ut_display_suite ();
	SRunner *sr = srunner_create (s);
	srunner_run_all (sr, CK_NORMAL);
	number_failed = srunner_ntests_failed (sr);
	srunner_free (sr);
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* vim: set foldmethod=marker: */
