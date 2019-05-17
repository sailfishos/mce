/**
 * @file mce-conf.c
 * Configuration option handling for MCE
 * <p>
 * Copyright © 2006-2009 Nokia Corporation and/or its subsidiary(-ies).
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

#include "mce-conf.h"

#include "mce.h"
#include "mce-log.h"
#include "modules/led.h"

#include <string.h>
#include <glob.h>

/** Pointer to the keyfile structure where config values are read from */
static gpointer keyfile = NULL;

/** Internal helper for insuring valid keyfile pointer is available
 *
 * @param keyfilepointer custom key file, or NULL to use the default one
 *
 * @returns non-null keyfile pointer, or aborts
 */
static gpointer mce_conf_get_keyfile(void)
{
	if( !keyfile ) {
		/* Earlier it was possible to have mce running with NULL
		 * keyfile. Now the only reasons that might happen are:
		 *   1) mce_conf_init() was not called yet
		 *   2) mce_conf_init() has failed
		 *   3) mce_conf_exit() has already been called
		 * i.e. critical logic errors somewhere */
		mce_log(LL_CRIT, "mce config subsystem used without "
			"properly initializing it");
		mce_abort();
	}
	return keyfile;
}

/** Check if configuration group is available
 *
 * @param group The configuration group
 *
 * @return TRUE if group is available, FALSE otherwise
 */
gboolean mce_conf_has_group(const gchar *group)
{
	gpointer keyfileptr = mce_conf_get_keyfile();
	return g_key_file_has_group(keyfileptr, group);
}

/** Check if configuration key is available
 *
 * @param group The configuration group
 * @param key The configuration key
 *
 * @return TRUE if key is available, FALSE otherwise
 */
gboolean mce_conf_has_key(const gchar *group, const gchar *key)
{
	gpointer keyfileptr = mce_conf_get_keyfile();
	GError *error = NULL;
	gboolean res = g_key_file_has_key(keyfileptr, group, key, &error);
	g_clear_error(&error);
	return res;
}

/**
 * Get a boolean configuration value
 *
 * @param group The configuration group to get the value from
 * @param key The configuration key to get the value of
 * @param defaultval The default value to use if the key isn't set
 * @param keyfileptr A keyfile pointer, or NULL to use the default keyfile
 * @return The configuration value on success, the default value on failure
 */
gboolean mce_conf_get_bool(const gchar *group, const gchar *key,
			   const gboolean defaultval)
{
	gboolean tmp = FALSE;
	GError *error = NULL;

	gpointer keyfileptr = mce_conf_get_keyfile();

	tmp = g_key_file_get_boolean(keyfileptr, group, key, &error);

	if (error != NULL) {
		mce_log(LL_DEBUG,
			"Could not get config key %s/%s; %s; "
			"defaulting to `%d'",
			group, key, error->message, defaultval);
		tmp = defaultval;
	}

	g_clear_error(&error);

	return tmp;
}

/**
 * Get an integer configuration value
 *
 * @param group The configuration group to get the value from
 * @param key The configuration key to get the value of
 * @param defaultval The default value to use if the key isn't set
 * @param keyfileptr A keyfile pointer, or NULL to use the default keyfile
 * @return The configuration value on success, the default value on failure
 */
gint mce_conf_get_int(const gchar *group, const gchar *key,
		      const gint defaultval)
{
	gint tmp = -1;
	GError *error = NULL;

	gpointer keyfileptr = mce_conf_get_keyfile();

	tmp = g_key_file_get_integer(keyfileptr, group, key, &error);

	if (error != NULL) {
		mce_log(LL_DEBUG,
			"Could not get config key %s/%s; %s; "
			"defaulting to `%d'",
			group, key, error->message, defaultval);
		tmp = defaultval;
	}

	g_clear_error(&error);

	return tmp;
}

/**
 * Get an integer list configuration value
 *
 * @param group The configuration group to get the value from
 * @param key The configuration key to get the value of
 * @param length The length of the list, or NULL if not needed
 * @param keyfileptr A keyfile pointer, or NULL to use the default keyfile
 * @return The configuration value on success, NULL on failure
 */
gint *mce_conf_get_int_list(const gchar *group, const gchar *key,
			    gsize *length)
{
	gint *tmp = NULL;
	GError *error = NULL;

	gpointer keyfileptr = mce_conf_get_keyfile();

	tmp = g_key_file_get_integer_list(keyfileptr, group, key,
					  length, &error);

	if (error != NULL) {
		mce_log(LL_DEBUG,
			"Could not get config key %s/%s; %s",
			group, key, error->message);
		if( length )
			*length = 0;
	}

	g_clear_error(&error);

	return tmp;
}

/**
 * Get a string configuration value
 *
 * @param group The configuration group to get the value from
 * @param key The configuration key to get the value of
 * @param defaultval The default value to use if the key isn't set
 * @param keyfileptr A keyfile pointer, or NULL to use the default keyfile
 * @return The configuration value on success, the default value on failure
 */
gchar *mce_conf_get_string(const gchar *group, const gchar *key,
			   const gchar *defaultval)
{
	gchar *tmp = NULL;
	GError *error = NULL;

	gpointer keyfileptr = mce_conf_get_keyfile();

	tmp = g_key_file_get_string(keyfileptr, group, key, &error);

	if (error != NULL) {
		mce_log(LL_DEBUG,
			"Could not get config key %s/%s; %s; %s%s%s",
			group, key, error->message,
			defaultval ? "defaulting to `" : "no default set",
			defaultval ? defaultval : "",
			defaultval ? "'" : "");

		if (defaultval != NULL)
			tmp = g_strdup(defaultval);
	}

	g_clear_error(&error);

	return tmp;
}

/**
 * Get a string list configuration value
 *
 * @param group The configuration group to get the value from
 * @param key The configuration key to get the value of
 * @param length The length of the list, or NULL if not needed
 * @param keyfileptr A keyfile pointer, or NULL to use the default keyfile
 * @return The configuration value on success, NULL on failure
 */
gchar **mce_conf_get_string_list(const gchar *group, const gchar *key,
				 gsize *length)
{
	gchar **tmp = NULL;
	GError *error = NULL;

	gpointer keyfileptr = mce_conf_get_keyfile();

	tmp = g_key_file_get_string_list(keyfileptr, group, key,
					 length, &error);

	if (error != NULL) {
		mce_log(LL_DEBUG,
			"Could not get config key %s/%s; %s",
			group, key, error->message);
		if( length )
			*length = 0;
	}

	g_clear_error(&error);

	return tmp;
}

gchar **mce_conf_get_keys(const gchar *group, gsize *length)
{
	gchar **tmp = NULL;
	GError *error = NULL;

	gpointer keyfileptr = mce_conf_get_keyfile();

	tmp = g_key_file_get_keys(keyfileptr, group, length, &error);

	if (error != NULL) {
		mce_log(LL_WARN,
			"Could not get config keys %s; %s",
			group, error->message);
		if( length )
			*length = 0;
	}

	g_clear_error(&error);

	return tmp;
}

/** Copy key value key value from one keyfile to another
 *
 * @param dest keyfile to modify
 * @param srce keyfile to copy from
 * @param grp  value group to copy
 * @param key  value key to copy
 */
static void mce_conf_override_key(GKeyFile *dest, GKeyFile *srce,
			       const char *grp, const char *key)
{
	gchar *val = g_key_file_get_value(srce, grp, key, 0);
	if( val ) {
		//mce_log(LL_NOTICE, "[%s] %s = %s", grp, key, val);
		g_key_file_set_value(dest, grp, key, val);
		g_free(val);
	}
}

/** Augment key value with data from another file
 *
 * @param dest keyfile to modify
 * @param srce keyfile to add from
 * @param grp  value group to add
 * @param key  value key to add
 */
static void mce_conf_append_key(GKeyFile *dest, GKeyFile *srce,
			       const char *grp, const char *key)
{
	gchar *val = g_key_file_get_value(srce, grp, key, 0);

	if( val ) {
		gchar *old = g_key_file_get_value(dest, grp, key, 0);
		gchar *tmp = 0;

		if( old && *old ) {
			tmp = g_strconcat(val, ";", old, NULL);
		}

		//mce_log(LL_NOTICE, "[%s] %s = %s", grp, key, tmp ?: val);
		g_key_file_set_value(dest, grp, key, tmp ?: val);

		g_free(tmp);
		g_free(old);
		g_free(val);
	}
}

/** Merge value from one keyfile to another
 *
 * Existing values will be overridden, except for values
 * in group [evdev] that are appended to existing data.
 *
 * @param dest keyfile to modify
 * @param srce keyfile to merge from
 * @param grp  value group to merge
 * @param key  value key to merge
 */
static void mce_conf_merge_key(GKeyFile *dest, GKeyFile *srce,
			       const char *grp, const char *key)
{
	/* groups/keys to append instead of overriding */
	static const struct {
		const gchar *grp;
		const gchar *key; // NULL == every key in the group
	} lut[] = {
		{
			.grp = "evdev",
		},
		{
			.grp = "modules/display",
		},
		{
			.grp = MCE_CONF_LED_GROUP,
			.key = MCE_CONF_LED_PATTERNS_REQUIRED,
		},
		{
			.grp = MCE_CONF_LED_GROUP,
			.key = MCE_CONF_LED_PATTERNS_DISABLED,
		},
		{
			.grp = NULL,
		}
	};

	for( size_t i = 0; ; ++i ) {
		if( lut[i].grp == NULL ) {
			mce_conf_override_key(dest, srce, grp, key);
			break;
		}
		if( strcmp(lut[i].grp, grp) )
			continue;
		if( !lut[i].key || !strcmp(lut[i].key, key) ) {
			mce_conf_append_key(dest, srce, grp, key);
			break;
		}
	}
}

/** Merge group of values from one keyfile to another
 *
 * @param dest keyfile to modify
 * @param srce keyfile to merge from
 * @param grp  value group to merge
 */
static void mce_conf_merge_group(GKeyFile *dest, GKeyFile *srce,
				 const char *grp)
{
	gchar **key = g_key_file_get_keys(srce, grp, 0, 0);
	if( key ) {
		for( size_t k = 0; key[k]; ++k )
			mce_conf_merge_key(dest, srce, grp, key[k]);
		g_strfreev(key);
	}
}

/** Merge all groups and values from one keyfile to another
 *
 * @param dest keyfile to modify
 * @param srce keyfile to merge from
 */
static void mce_conf_merge_file(GKeyFile *dest, GKeyFile *srce)
{
	gchar **grp = g_key_file_get_groups(srce, 0);

	if( grp ) {
		for( size_t g = 0; grp[g]; ++g )
			mce_conf_merge_group(dest, srce, grp[g]);
		g_strfreev(grp);
	}
}

/** Callback function for logging errors within glob()
 *
 * @param path path to file/dir where error occurred
 * @param err  errno that occurred
 *
 * @return 0 (= do not stop glob)
 */
static int mce_conf_glob_error_cb(const char *path, int err)
{
	mce_log(LL_WARN, "%s: glob: %s", path, g_strerror(err));
	return 0;
}

/** Process config data from /etc/mce/mce.d/xxx.ini files
 */
static GKeyFile *mce_conf_read_ini_files(void)
{
	static const char pattern[] = MCE_CONF_DIR"/[0-9][0-9]*.ini";

	GKeyFile *ini = g_key_file_new();
	glob_t    gb;

	memset(&gb, 0, sizeof gb);

	if( glob(pattern, 0, mce_conf_glob_error_cb, &gb) != 0 ) {
		mce_log(LL_WARN, "no mce configuration ini-files found");
		goto EXIT;
	}

	for( size_t i = 0; i < gb.gl_pathc; ++i ) {
		const char *path = gb.gl_pathv[i];
		GError     *err  = 0;
		GKeyFile   *tmp  = g_key_file_new();

		if( !g_key_file_load_from_file(tmp, path, 0, &err) ) {
			mce_log(LL_WARN, "%s: can't load: %s", path,
				err->message ?: "unknown");
		}
		else {
			mce_log(LL_NOTICE, "processing %s ...", path);
			mce_conf_merge_file(ini, tmp);
		}
		g_clear_error(&err);
		g_key_file_free(tmp);
	}

EXIT:
	globfree(&gb);

	return ini;
}

/* XXX:
 * We should probably use
 * /dev/input/keypad
 * /dev/input/gpio-keys
 * /dev/input/pwrbutton
 * /dev/input/ts
 * and add whitelist entries for misc devices instead
 */

/**
 * List of drivers that provide touchscreen events
 * XXX: If this is made case insensitive,
 *      we could search for "* touchscreen" instead
 */
static const gchar *const touch_builtin[] = {
	/** Input layer name for the Atmel mXT touchscreen */
	"Atmel mXT Touchscreen",

	/** Input layer name for the Atmel QT602240 touchscreen */
	"Atmel QT602240 Touchscreen",

	/** TSC2005 touchscreen */
	"TSC2005 touchscreen",

	/** TSC2301 touchscreen */
	"TSC2301 touchscreen",

	/** ADS784x touchscreen */
	"ADS784x touchscreen",

	/** No more entries */
	NULL
};

/**
 * List of drivers that provide keyboard events
 */
static const gchar *const keybd_builtin[] = {
	/** Input layer name for the TWL4030 keyboard/keypad */
	"TWL4030 Keypad",

	/** Legacy input layer name for the TWL4030 keyboard/keypad */
	"omap_twl4030keypad",

	/** Generic input layer name for keyboard/keypad */
	"Internal keyboard",

	/** Input layer name for the LM8323 keypad */
	"LM8323 keypad",

	/** Generic input layer name for keypad */
	"Internal keypad",

	/** Input layer name for the TSC2301 keypad */
	"TSC2301 keypad",

	/** Legacy generic input layer name for keypad */
	"omap-keypad",

	/** Input layer name for standard PC keyboards */
	"AT Translated Set 2 keyboard",

	/** Input layer name for the power button in various MeeGo devices */
	"msic_power_btn",

	/** Input layer name for the TWL4030 power button */
	"twl4030_pwrbutton",

	/** Input layer name for the Triton 2 power button */
	"triton2-pwrbutton",

	/** Input layer name for the Retu powerbutton */
	"retu-pwrbutton",

	/** Input layer name for the PC Power button */
	"Power Button",

	/** Input layer name for the PC Sleep button */
	"Sleep Button",

	/** Input layer name for the Thinkpad extra buttons */
	"Thinkpad Extra Buttons",

	/** Input layer name for ACPI virtual keyboard */
	"ACPI Virtual Keyboard Device",

	/** Input layer name for GPIO-keys */
	"gpio-keys",

	/** Input layer name for DFL-61/TWL4030 jack sense */
	"dfl61-twl4030 Jack",

	/** Legacy input layer name for TWL4030 jack sense */
	"rx71-twl4030 Jack",

	/** Input layer name for PC Lid switch */
	"Lid Switch",

	/** No more entries */
	NULL
};

/**
 * List of drivers that we should not monitor
 */
static const gchar *const black_builtin[] = {
	/** Input layer name for the AMI305 magnetometer */
	"ami305 magnetometer",

	/** Input layer name for the ST LIS3LV02DL accelerometer */
	"ST LIS3LV02DL Accelerometer",

	/** Input layer name for the ST LIS302DL accelerometer */
	"ST LIS302DL Accelerometer",

	/** Input layer name for the TWL4030 vibrator */
	"twl4030:vibrator",

	/** Input layer name for AV accessory */
	"AV Accessory",

	/** Input layer name for the video bus */
	"Video Bus",

	/** Input layer name for the PC speaker */
	"PC Speaker",

	/** Input layer name for the Intel HDA headphone */
	"HDA Intel Headphone",

	/** Input layer name for the Intel HDA microphone */
	"HDA Intel Mic",

	/** Input layer name for the UVC 17ef:4807 webcam in thinkpad X301 */
	"UVC Camera (17ef:4807)",

	/** Input layer name for the UVC 17ef:480c webcam in thinkpad X201si */
	"UVC Camera (17ef:480c)",

	/** No more entries */
	NULL
};

/** List of touchscreen event devices obtained from ini files */
static gchar **touch_cached = NULL;

/** List of keyboard event devices obtained from ini files */
static gchar **keybd_cached = NULL;

/** List of blacklisted event devices obtained from ini files */
static gchar **black_cached = NULL;

/**
 * Init function for the mce-conf component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_conf_init(void)
{
	gboolean status = FALSE;

	if( !(keyfile = mce_conf_read_ini_files()) )
		goto EXIT;

	touch_cached = g_key_file_get_string_list(keyfile,
						  "evdev",
						  "touch",
						  0, 0);

	keybd_cached = g_key_file_get_string_list(keyfile,
						  "evdev",
						  "keybd",
						  0, 0);

	black_cached = g_key_file_get_string_list(keyfile,
						  "evdev",
						  "black",
						  0, 0);
	status = TRUE;

EXIT:
	return status;
}

/**
 * Exit function for the mce-conf component
 */
void mce_conf_exit(void)
{
	g_strfreev(touch_cached), touch_cached = 0;
	g_strfreev(keybd_cached), keybd_cached = 0;
	g_strfreev(black_cached), black_cached = 0;

	if( keyfile ) g_key_file_free(keyfile), keyfile = 0;

	return;
}

const gchar * const *mce_conf_get_touchscreen_event_drivers(void)
{
	return (const gchar*const*)touch_cached ?: touch_builtin;
}

const gchar * const *mce_conf_get_keyboard_event_drivers(void)
{
	return (const gchar*const*)keybd_cached ?: keybd_builtin;
}

const gchar * const *mce_conf_get_blacklisted_event_drivers(void)
{
	return (const gchar*const*)black_cached ?: black_builtin;
}
