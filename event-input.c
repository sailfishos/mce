/**
 * @file event-input.c
 * /dev/input event provider for the Mode Control Entity
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Ismo Laitinen <ismo.laitinen@nokia.com>
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

#include "event-input.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-io.h"
#include "mce-lib.h"
#include "mce-conf.h"
#ifdef ENABLE_DOUBLETAP_EMULATION
# include "mce-gconf.h"
#endif
#include "mce-sensorfw.h"
#include "evdev.h"

#include <linux/input.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>

#include <glib/gstdio.h>
#include <gio/gio.h>

#ifndef SW_CAMERA_LENS_COVER
/** Input layer code for the camera lens cover switch */
#define SW_CAMERA_LENS_COVER		0x09
#endif /* SW_CAMERA_LENS_COVER */
#ifndef SW_KEYPAD_SLIDE
/** Input layer code for the keypad slide switch */
#define SW_KEYPAD_SLIDE			0x0a
#endif /* SW_KEYPAD_SLIDE */
#ifndef SW_FRONT_PROXIMITY
/** Input layer code for the front proximity sensor switch */
#define SW_FRONT_PROXIMITY		0x0b
#endif /* SW_FRONT_PROXIMITY */
#ifndef KEY_CAMERA_FOCUS
/** Input layer code for the camera focus button */
#define KEY_CAMERA_FOCUS		0x0210
#endif /* KEY_CAMERA_FOCUS */

/** ID for keypress timeout source */
static guint keypress_repeat_timeout_cb_id = 0;

/** ID for misc timeout source */
static guint misc_io_monitor_timeout_cb_id = 0;

/* ========================================================================= *
 * TOUCH INPUT DEVICE MONITORING
 * ========================================================================= */

/** List of touchscreen input devices */
static GSList *touchscreen_dev_list = NULL;

/** Handle touch device iomon delete notification
 *
 * @param iomon I/O monitor that is about to get deleted
 */
static void touchscreen_dev_rem_cb(gconstpointer iomon)
{
	touchscreen_dev_list = g_slist_remove(touchscreen_dev_list, iomon);
}

/** Add touch device I/O monitor
 *
 * @param fd        File descriptor
 * @param path      File path
 * @param callback  Input event handler
 */
static void touchscreen_dev_add(int fd, const char *path, iomon_cb callback)
{
	gconstpointer iomon =
		mce_register_io_monitor_chunk(fd, path, MCE_IO_ERROR_POLICY_WARN,
					      FALSE, callback,
					      touchscreen_dev_rem_cb,
					      sizeof (struct input_event));
	if( iomon )
		touchscreen_dev_list = g_slist_prepend(touchscreen_dev_list,
						       (gpointer)iomon);
}

/** Remove all touch device I/O monitors
 */
static void touchscreen_dev_rem_all(void)
{
	mce_unregister_io_monitor_list(touchscreen_dev_list);
}

/* ========================================================================= *
 * KEYPAD INPUT DEVICE MONITORING
 * ========================================================================= */

/** List of keyboard input devices */
static GSList *keyboard_dev_list = NULL;

/** Handle keyboard device iomon delete notification
 *
 * @param iomon I/O monitor that is about to get deleted
 */
static void keyboard_dev_rem_cb(gconstpointer iomon)
{
	keyboard_dev_list = g_slist_remove(keyboard_dev_list, iomon);
}

/** Add keyboard device I/O monitor
 *
 * @param fd        File descriptor
 * @param path      File path
 * @param callback  Input event handler
 */
static void keyboard_dev_add(int fd, const char *path, iomon_cb callback)
{
	gconstpointer iomon =
		mce_register_io_monitor_chunk(fd, path, MCE_IO_ERROR_POLICY_WARN,
					      FALSE, callback,
					      keyboard_dev_rem_cb,
					      sizeof (struct input_event));
	if( iomon )
		keyboard_dev_list = g_slist_prepend(keyboard_dev_list,
						    (gpointer)iomon);
}

/** Remove all keyboard device I/O monitors
 */
static void keyboard_dev_rem_all(void)
{
	mce_unregister_io_monitor_list(keyboard_dev_list);
}

/* ========================================================================= *
 * VOLUMEKEY INPUT DEVICE MONITORING
 * ========================================================================= */

/** List of volume key input devices */
static GSList *volumekey_dev_list = NULL;

/** Handle volumekey device iomon delete notification
 *
 * @param iomon I/O monitor that is about to get deleted
 */
static void volumekey_dev_rem_cb(gconstpointer iomon)
{
	volumekey_dev_list = g_slist_remove(volumekey_dev_list, iomon);
}

/** Add volumekey device I/O monitor
 *
 * @param fd        File descriptor
 * @param path      File path
 * @param callback  Input event handler
 */
static void volumekey_dev_add(int fd, const char *path, iomon_cb callback)
{
	gconstpointer iomon =
		mce_register_io_monitor_chunk(fd, path, MCE_IO_ERROR_POLICY_WARN,
					      FALSE, callback,
					      volumekey_dev_rem_cb,
					      sizeof (struct input_event));
	if( iomon )
		volumekey_dev_list = g_slist_prepend(volumekey_dev_list,
						    (gpointer)iomon);
}

/** Remove all volumekey device I/O monitors
 */
static void volumekey_dev_rem_all(void)
{
	mce_unregister_io_monitor_list(volumekey_dev_list);
}

/* ========================================================================= *
 * MISC INPUT DEVICE MONITORING
 * ========================================================================= */

/** List of misc input devices */
static GSList *misc_dev_list = NULL;

/** Handle misc device iomon delete notification
 *
 * @param iomon I/O monitor that is about to get deleted
 */
static void misc_dev_rem_cb(gconstpointer iomon)
{
	misc_dev_list = g_slist_remove(misc_dev_list, iomon);
}

/** Add misc device I/O monitor
 *
 * @param fd        File descriptor
 * @param path      File path
 * @param callback  Input event handler
 */
static void misc_dev_add(int fd, const char *path, iomon_cb callback)
{
	gconstpointer iomon =
		mce_register_io_monitor_chunk(fd, path, MCE_IO_ERROR_POLICY_WARN,
					      FALSE, callback,
					      misc_dev_rem_cb,
					      sizeof (struct input_event));
	if( iomon )
		misc_dev_list = g_slist_prepend(misc_dev_list,
						(gpointer)iomon);
}

/** Remove all misc device I/O monitors
 */
static void misc_dev_rem_all(void)
{
	mce_unregister_io_monitor_list(misc_dev_list);
}

/** GFile pointer for the directory we monitor */
static GFile *dev_input_gfp = NULL;

/** GFileMonitor pointer for the directory we monitor */
static GFileMonitor *dev_input_gfmp = NULL;

/** The handler ID for the signal handler */
static gulong dev_input_handler_id = 0;

/** Time in milliseconds before the key press is considered long */
static gint longdelay = DEFAULT_HOME_LONG_DELAY;

/** Can GPIO key interrupts be disabled? */
static gboolean gpio_key_disable_exists = FALSE;

static void update_inputdevices(const gchar *device, gboolean add);

#ifndef FF_STATUS_CNT
# ifdef FF_STATUS_MAX
#  define FF_STATUS_CNT (FF_STATUS_MAX+1)
# else
#  define FF_STATUS_CNT 0
# endif
#endif

#ifndef PWR_CNT
# ifdef PWR_MAX
#  define PWR_CNT (PWR_MAX+1)
# else
#  define PWR_CNT 0
# endif
#endif

/** Calculate how many elements an array of longs bitmap needs to
 *  have enough space for bc items */
#define EVDEVBITS_LEN(bc) (((bc)+LONG_BIT-1)/LONG_BIT)

/** Supported codes for one evdev event type
 */
typedef struct
{
	/** event type */
	int type;
	/** event code count for this type */
	int cnt;
	/** bitmask of supported event codes */
	unsigned long bit[0];
} evdevbits_t;

/** Create empty event code bitmap for one evdev event type
 *
 * @param type evdev event type
 *
 * @return evdevbits_t object, or NULL for types not needed by mce
 */
static evdevbits_t *evdevbits_create(int type)
{
	evdevbits_t *self = 0;
	int cnt = 0;

	switch( type ) {
	case EV_SYN:       cnt = EV_CNT;        break;
	case EV_KEY:       cnt = KEY_CNT;       break;
	case EV_REL:       cnt = REL_CNT;       break;
	case EV_ABS:       cnt = ABS_CNT;       break;
	case EV_MSC:       cnt = MSC_CNT;       break;
	case EV_SW:        cnt = SW_CNT;        break;
#if 0
	case EV_LED:       cnt = LED_CNT;       break;
	case EV_SND:       cnt = SND_CNT;       break;
	case EV_REP:       cnt = REP_CNT;       break;
	case EV_FF:        cnt = FF_CNT;        break;
	case EV_PWR:       cnt = PWR_CNT;       break;
	case EV_FF_STATUS: cnt = FF_STATUS_CNT; break;
#endif
	default: break;
	}

	if( cnt > 0 ) {
		int len = EVDEVBITS_LEN(cnt);
		self = g_malloc0(sizeof *self + len * sizeof *self->bit);
		self->type = type;
		self->cnt  = cnt;
	}
	return self;
}

/** Delete evdev event code bitmap
 *
 * @param self evdevbits_t object, or NULL
 */
static void evdevbits_delete(evdevbits_t *self)
{
	g_free(self);
}

/** Clear bits in evdev event code bitmap
 *
 * @param self evdevbits_t object, or NULL
 */
static void evdevbits_clear(evdevbits_t *self)
{
	if( self ) {
		int len = EVDEVBITS_LEN(self->cnt);
		memset(self->bit, 0, len * sizeof *self->bit);
	}
}

/** Read supported codes from file descriptor
 *
 * @param self evdevbits_t object, or NULL
 * @param fd file descriptor to probe data from
 *
 * @return 0 on success, -1 on errors
 */
static int evdevbits_probe(evdevbits_t *self, int fd)
{
	int res = 0;
	if( self && ioctl(fd, EVIOCGBIT(self->type, self->cnt), self->bit) == -1 ) {
		mce_log(LL_WARN, "EVIOCGBIT(%s, %d): %m",
			evdev_get_event_type_name(self->type), self->cnt);
		evdevbits_clear(self);
		res = -1;
	}
	return res;
}

/** Test if evdev event code is set in bitmap
 *
 * @param self evdevbits_t object, or NULL
 * @param bit event code to check
 *
 * @return 1 if code is supported, 0 otherwise
 */
static int evdevbits_test(const evdevbits_t *self, int bit)
{
	int res = 0;
	if( self && (unsigned)bit < (unsigned)self->cnt ) {
		int i = bit / LONG_BIT;
		unsigned long m = 1ul << (bit % LONG_BIT);
		if( self->bit[i] & m ) res = 1;
	}
	return res;
}

/** Supported event types and codes for an evdev device node
 */
typedef struct
{
	/** Array of bitmasks for supported event types */
	evdevbits_t *mask[EV_CNT];
} evdevinfo_t;

/** Create evdev information object
 *
 * @return evdevinfo_t object
 */
static evdevinfo_t *evdevinfo_create(void)
{
	evdevinfo_t *self = g_malloc0(sizeof *self);

	for( int i = 0; i < EV_CNT; ++i )
		self->mask[i] = evdevbits_create(i);

	return self;
}

/** Delete evdev information object
 *
 * @param self evdevinfo_t object
 */
static void evdevinfo_delete(evdevinfo_t *self)
{
	if( self ) {
		for( int i = 0; i < EV_CNT; ++i )
			evdevbits_delete(self->mask[i]);
		g_free(self);
	}
}

/** Check if event type is supported
 *
 * @param self evdevinfo_t object
 * @param type evdev event type
 *
 * @return 1 if event type is supported, 0 otherwise
 */
static int evdevinfo_has_type(const evdevinfo_t *self, int type)
{
	int res = 0;
	if( (unsigned)type < EV_CNT )
		res = evdevbits_test(self->mask[0], type);
	return res;
}

/** Check if any of given event types are supported
 *
 * @param self evdevinfo_t object
 * @param types array of evdev event types
 *
 * @return 1 if at least on of the types is supported, 0 otherwise
 */
static int evdevinfo_has_types(const evdevinfo_t *self, const int *types)
{
	int res = 0;
	for( size_t i = 0; types[i] >= 0; ++i ) {
		if( (res = evdevinfo_has_type(self, types[i])) )
			break;
	}
	return res;
}

/** Check if event code is supported
 *
 * @param self evdevinfo_t object
 * @param type evdev event type
 * @param code evdev event code
 *
 * @return 1 if event code for type is supported, 0 otherwise
 */
static int evdevinfo_has_code(const evdevinfo_t *self, int type, int code)
{
	int res = 0;

	if( evdevinfo_has_type(self, type) )
		res = evdevbits_test(self->mask[type], code);
	return res;
}

/** Check if any of given event codes are supported
 *
 * @param self evdevinfo_t object
 * @param type evdev event type
 * @param code array of evdev event codes
 *
 * @return 1 if at least on of the event codes for type is supported, 0 otherwise
 */
static int evdevinfo_has_codes(const evdevinfo_t *self, int type, const int *codes)
{
	int res = 0;

	if( evdevinfo_has_type(self, type) ) {
		for( size_t i = 0; codes[i] != -1; ++i ) {
			if( (res = evdevbits_test(self->mask[type], codes[i])) )
				break;
		}
	}
	return res;
}

/** Helper for checking if array of integers contains a particular value
 *
 * @param list  array of ints, terminated with -1
 * @param entry value to check
 *
 * @return 1 if value is present in the list, 0 if not
 */
static int list_has_entry(const int *list, int entry)
{
	for( int i = 0; list[i] != -1; ++i ) {
		if( list[i] == entry )
			return 1;
	}
	return 0;
}

/** Check if all of the listed types and only the listed types are supported
 *
 * @param self  evdevinfo_t object
 * @param types array of evdev event types, terminated with -1
 *
 * @return 1 if all of types and only types are supported, 0 otherwise
 */
static int evdevinfo_match_types(const evdevinfo_t *self, const int *types)
{
	for( int etype = 1; etype < EV_CNT; ++etype ) {
		int have = evdevinfo_has_type(self, etype);
		int want = list_has_entry(types, etype);
		if( have != want )
			return 0;
	}
	return 1;
}

/** Check if all of the listed codes and only the listed codes are supported
 *
 * @param self  evdevinfo_t object
 * @param types evdev event type
 * @param codes array of evdev event codes, terminated with -1
 *
 * @return 1 if all of codes and only codes are supported, 0 otherwise
 */
static int evdevinfo_match_codes(const evdevinfo_t *self, int type, const int *codes)
{
	for( int ecode = 0; ecode < KEY_CNT; ++ecode ) {
		int have = evdevinfo_has_code(self, type, ecode);
		int want = list_has_entry(codes, ecode);
		if( have != want )
			return 0;
	}
	return 1;
}

/** Fill in evdev data by probing file descriptor
 *
 * @param self evdevinfo_t object
 * @param fd file descriptor to probe data from
 *
 * @return 0 on success, -1 on errors
 */
static int evdevinfo_probe(evdevinfo_t *self, int fd)
{
	int res = evdevbits_probe(self->mask[0], fd);

	for( int i = 1; i < EV_CNT; ++i ) {
		if( evdevbits_test(self->mask[0], i) )
			evdevbits_probe(self->mask[i], fd);
		else
			evdevbits_clear(self->mask[i]);
	}

	return res;
}

/** Types of use MCE can have for evdev input devices
 */
typedef enum {
	/** Sensors that might look like touch but should be ignored */
	EVDEV_REJECT,
	/** Touch screen to be tracked and processed */
	EVDEV_TOUCH,
	/** Keys etc that mce needs to track and process */
	EVDEV_INPUT,
	/** Keys etc that mce itself does not need, tracked only for
	 *  detecting user activity */
	EVDEV_ACTIVITY,
	/** The rest, mce does not track these */
	EVDEV_IGNORE,
	/** Button like touch device */
	EVDEV_DBLTAP,
	/** Proximity sensor */
	EVDEV_PS,
	/** Ambient light sensor */
	EVDEV_ALS,
	/** Volume key device */
	EVDEV_VOLKEY,

} evdev_type_t;

/** Human readable evdev classifications for debugging purposes
 */
static const char * const evdev_class[] =
{
	[EVDEV_REJECT]   = "REJECT",
	[EVDEV_TOUCH]    = "TOUCHSCREEN",
	[EVDEV_INPUT]    = "KEY, BUTTON or SWITCH",
	[EVDEV_ACTIVITY] = "USER ACTIVITY ONLY",
	[EVDEV_IGNORE]   = "IGNORE",
	[EVDEV_DBLTAP]   = "DOUBLE TAP",
	[EVDEV_PS]       = "PROXIMITY SENSOR",
	[EVDEV_ALS]      = "AMBIENT LIGHT SENSOR",
	[EVDEV_VOLKEY]   = "VOLUME KEYS",
};

/** Use heuristics to determine what mce should do with an evdev device node
 *
 * @param fd file descriptor to probe data from
 *
 * @return one of EVDEV_TOUCH, EVDEV_INPUT, ...
 */
static evdev_type_t get_evdev_type(int fd)
{
	int res = EVDEV_IGNORE;

	evdevinfo_t *feat = evdevinfo_create();

	evdevinfo_probe(feat, fd);

	/* EV_ABS probing arrays for ALS/PS detection */
	static const int abs_only[]  = { EV_ABS, -1 };
	static const int misc_only[] = { ABS_MISC, -1 };
	static const int dist_only[] = { ABS_DISTANCE, -1 };

	/* EV_KEY probing arrays for detecting input devices
	 * that report double tap gestures as power key events */
	static const int key_only[]   = { EV_KEY, -1 };
	static const int dbltap_lut[] = {
		KEY_POWER,
		KEY_MENU,
		KEY_BACK,
		KEY_HOMEPAGE,
		-1
	};
	/* Key events mce is interested in */
	static const int keypad_lut[] = {
		KEY_CAMERA,
		KEY_CAMERA_FOCUS,
		KEY_POWER,
		KEY_SCREENLOCK,
		KEY_VOLUMEDOWN,
		KEY_VOLUMEUP,
		-1
	};
	/* Volume key events */
	static const int volkey_lut[] = {
		KEY_VOLUMEDOWN,
		KEY_VOLUMEUP,
		-1
	};

	/* Switch events mce is interested in */
	static const int switch_lut[] = {
		SW_CAMERA_LENS_COVER,
		SW_FRONT_PROXIMITY,
		SW_HEADPHONE_INSERT,
		SW_KEYPAD_SLIDE,
		SW_LINEOUT_INSERT,
		SW_MICROPHONE_INSERT,
		SW_VIDEOOUT_INSERT,
		-1
	};

	/* Event classes that could be due to "user activity" */
	static const int misc_lut[] = {
		EV_KEY,
		EV_REL,
		EV_ABS,
		EV_MSC,
		EV_SW,
		-1
	};

	/* All event classes except EV_ABS */
	static const int all_but_abs_lut[] = {
		EV_KEY,
		EV_REL,
		EV_MSC,
		EV_SW,
		EV_LED,
		EV_SND,
		EV_REP,
		EV_FF,
		EV_PWR,
		EV_FF_STATUS,
		-1
	};

	/* Ambient light and proximity sensor inputs */
	if( evdevinfo_match_types(feat, abs_only) ) {
		if( evdevinfo_match_codes(feat, EV_ABS, misc_only) ) {
			// only EV_ABS:ABS_MISC -> ALS
			res = EVDEV_ALS;
			goto cleanup;
		}
		if( evdevinfo_match_codes(feat, EV_ABS, dist_only) ) {
			// only EV_ABS:ABS_DISTANCE -> PS
			res = EVDEV_PS;
			goto cleanup;
		}
	}

	/* MCE has no use for accelerometers etc */
	if( evdevinfo_has_code(feat, EV_KEY, BTN_Z) ||
	    evdevinfo_has_code(feat, EV_ABS, ABS_Z) ) {
		// 3d sensor like accelorometer/magnetometer
		res = EVDEV_REJECT;
		goto cleanup;
	}

	/* While MCE mostly uses touchscreen inputs only for
	 * "user activity" monitoring, the touch devices
	 * generate a lot of events and mce has mechanism in
	 * place to avoid processing all of them */
	if( evdevinfo_has_code(feat, EV_KEY, BTN_TOUCH) &&
	    evdevinfo_has_code(feat, EV_ABS, ABS_X)     &&
	    evdevinfo_has_code(feat, EV_ABS, ABS_Y) ) {
		// singletouch protocol
		res = EVDEV_TOUCH;
		goto cleanup;
	}
	if( evdevinfo_has_code(feat, EV_ABS, ABS_MT_POSITION_X) &&
	    evdevinfo_has_code(feat, EV_ABS, ABS_MT_POSITION_Y) ) {
		// multitouch protocol
		res = EVDEV_TOUCH;
		goto cleanup;
	}

	/* In SDK we might bump into mouse devices, track them
	 * as if they were touch screen devices */
	if( evdevinfo_has_code(feat, EV_KEY, BTN_MOUSE) &&
	    evdevinfo_has_code(feat, EV_REL, REL_X) &&
	    evdevinfo_has_code(feat, EV_REL, REL_Y) ) {
		// mouse
		res = EVDEV_TOUCH;
		goto cleanup;
	}

	/* Touchscreen that emits power key events on double tap */
	if( evdevinfo_match_types(feat, key_only) &&
	    evdevinfo_match_codes(feat, EV_KEY, dbltap_lut) ) {
		res = EVDEV_DBLTAP;
		goto cleanup;
	}

	/* Volume keys only input devices can be grabbed */
	if( evdevinfo_match_types(feat, key_only) &&
	    evdevinfo_match_codes(feat, EV_KEY, volkey_lut) ) {
		res = EVDEV_VOLKEY;
		goto cleanup;
	}

	/* Some keys and swithes are processed at mce level */
	if( evdevinfo_has_codes(feat, EV_KEY, keypad_lut ) ||
	    evdevinfo_has_codes(feat, EV_SW,  switch_lut ) ) {
		res = EVDEV_INPUT;
		goto cleanup;
	}

	/* Assume that: devices that support only ABS_DISTANCE are
	 * proximity sensors and devices that support only ABS_MISC
	 * are ambient light sensors that are handled via libhybris
	 * in more appropriate place and should not be used for
	 * "user activity" tracking. */
	if( evdevinfo_has_type(feat, EV_ABS) &&
	    !evdevinfo_has_types(feat, all_but_abs_lut) ) {
		int maybe_als = evdevinfo_has_code(feat, EV_ABS, ABS_MISC);
		int maybe_ps  = evdevinfo_has_code(feat, EV_ABS, ABS_DISTANCE);

		// supports one of the two, but not both ...
		if( maybe_als != maybe_ps ) {
			for( int code = 0; ; ++code ) {
				switch( code ) {
				case ABS_CNT:
					// ... and no other events supported
					res = EVDEV_REJECT;
					goto cleanup;

				case ABS_DISTANCE:
				case ABS_MISC:
					continue;

				default:
					break;
				}
				if( evdevinfo_has_code(feat, EV_ABS, code) )
					break;
			}
		}
	}

	/* Track events that can be considered as "user activity" */
	if( evdevinfo_has_types(feat, misc_lut) ) {
		res = EVDEV_ACTIVITY;
		goto cleanup;
	}

cleanup:

	evdevinfo_delete(feat);

	return res;
}

/**
 * Enable the specified GPIO key
 * non-existing or already enabled keys are silently ignored
 *
 * @param key The key to enable
 */
static void enable_gpio_key(guint16 key)
{
	gchar *disabled_keys = NULL;
	gulong *keylist = NULL;
	gsize keylistlen;
	gchar *tmp = NULL;

	if (mce_read_string_from_file(GPIO_KEY_DISABLE_PATH,
				      &disabled_keys) == FALSE)
		goto EXIT;

	keylistlen = (KEY_CNT / bitsize_of(*keylist)) +
		     ((KEY_CNT % bitsize_of(*keylist)) ? 1 : 0);
	keylist = g_malloc0(keylistlen * sizeof (*keylist));

	if (string_to_bitfield(disabled_keys, &keylist, keylistlen) == FALSE)
		goto EXIT;

	clear_bit(key, &keylist);

	if ((tmp = bitfield_to_string(keylist, keylistlen)) == NULL)
		goto EXIT;

	(void)mce_write_string_to_file(GPIO_KEY_DISABLE_PATH, tmp);

EXIT:
	g_free(disabled_keys);
	g_free(keylist);
	g_free(tmp);

	return;
}

/**
 * Disable the specified GPIO key/switch
 * non-existing or already disabled keys/switches are silently ignored
 *
 * @param key The key/switch to disable
 */
static void disable_gpio_key(guint16 key)
{
	gchar *disabled_keys = NULL;
	gulong *keylist = NULL;
	gsize keylistlen;
	gchar *tmp = NULL;

	if (mce_read_string_from_file(GPIO_KEY_DISABLE_PATH,
				      &disabled_keys) == FALSE)
		goto EXIT;

	keylistlen = (KEY_CNT / bitsize_of(*keylist)) +
		     ((KEY_CNT % bitsize_of(*keylist)) ? 1 : 0);
	keylist = g_malloc0(keylistlen * sizeof (*keylist));

	if (string_to_bitfield(disabled_keys, &keylist, keylistlen) == FALSE)
		goto EXIT;

	set_bit(key, &keylist);

	if ((tmp = bitfield_to_string(keylist, keylistlen)) == NULL)
		goto EXIT;

	(void)mce_write_string_to_file(GPIO_KEY_DISABLE_PATH, tmp);

EXIT:
	g_free(disabled_keys);
	g_free(keylist);
	g_free(tmp);

	return;
}

/**
 * Wrapper function to call mce_suspend_io_monitor() from g_slist_foreach()
 *
 * @param io_monitor The I/O monitor to suspend
 * @param user_data Unused
 */
static void suspend_io_monitor(gpointer io_monitor, gpointer user_data)
{
	(void)user_data;

	mce_suspend_io_monitor(io_monitor);
}

/**
 * Wrapper function to call mce_resume_io_monitor() from g_slist_foreach()
 *
 * @param io_monitor The I/O monitor to resume
 * @param user_data Unused
 */
static void resume_io_monitor(gpointer io_monitor, gpointer user_data)
{
	(void)user_data;

	mce_resume_io_monitor(io_monitor);
}

/* ========================================================================= *
 * GENERIC EVDEV INPUT GRAB STATE MACHINE
 * ========================================================================= */

typedef struct input_grab_t input_grab_t;

/** State information for generic input grabbing state machine */
struct input_grab_t
{
	/** State machine instance name */
	const char *ig_name;

	/** Currently touched/down */
	bool        ig_touching;

	/** Was touched/down, delaying release */
	bool        ig_touched;

	/** Input grab is wanted */
	bool        ig_want_grab;

	/** Input grab is active */
	bool        ig_have_grab;

	/** Delayed release timer */
	guint       ig_release_id;

	/** Delayed release delay */
	int         ig_release_ms;

	/** Callback for notifying grab status changes */
	void      (*ig_grab_changed_cb)(input_grab_t *self, bool have_grab);

	/** Callback for additional release polling */
	bool      (*ig_release_verify_cb)(input_grab_t *self);
};

static void     input_grab_reset(input_grab_t *self);
static gboolean input_grab_release_cb(gpointer aptr);
static void     input_grab_start_release_timer(input_grab_t *self);
static void     input_grab_cancel_release_timer(input_grab_t *self);
static void     input_grab_rethink(input_grab_t *self);
static void     input_grab_set_touching(input_grab_t *self, bool touching);
static void     input_grab_request_grab(input_grab_t *self, bool want_grab);
static void     input_grab_iomon_cb(gpointer data, gpointer user_data);

/** Reset input grab state machine
 *
 * Releases any dynamic resources held by the state machine
 */
static void input_grab_reset(input_grab_t *self)
{
	self->ig_touching = false;
	self->ig_touched  = false;

	if( self->ig_release_id )
		g_source_remove(self->ig_release_id),
		self->ig_release_id = 0;
}

/** Delayed release timeout callback
 *
 * Grab/ungrab happens from this function when touch/press ends
 */
static gboolean input_grab_release_cb(gpointer aptr)
{
	input_grab_t *self = aptr;
	gboolean repeat = FALSE;

	if( !self->ig_release_id )
		goto EXIT;

	if( self->ig_release_verify_cb && !self->ig_release_verify_cb(self) ) {
		mce_log(LL_DEBUG, "touching(%s) = holding", self->ig_name);
		repeat = TRUE;
		goto EXIT;
	}

	// timer no longer active
	self->ig_release_id = 0;

	// touch release delay has ended
	self->ig_touched = false;

	mce_log(LL_DEBUG, "touching(%s) = released", self->ig_name);

	// evaluate next state
	input_grab_rethink(self);

EXIT:
	return repeat;
}

/** Start delayed release timer if not already running
 */
static void input_grab_start_release_timer(input_grab_t *self)
{
	if( !self->ig_release_id )
		self->ig_release_id = g_timeout_add(self->ig_release_ms,
						       input_grab_release_cb,
						       self);
}

/** Cancel delayed release timer
 */
static void input_grab_cancel_release_timer(input_grab_t *self)
{
	if( self->ig_release_id )
		g_source_remove(self->ig_release_id),
		self->ig_release_id = 0;
}

/** Re-evaluate input grab state
 */
static void input_grab_rethink(input_grab_t *self)
{
	// no changes while active touch
	if( self->ig_touching ) {
		input_grab_cancel_release_timer(self);
		goto EXIT;
	}

	// delay after touch release
	if( self->ig_touched ) {
		input_grab_start_release_timer(self);
		goto EXIT;
	}

	// do we want to change state?
	if( self->ig_have_grab == self->ig_want_grab )
		goto EXIT;

	// make the transition
	self->ig_have_grab = self->ig_want_grab;

	// and report it
	if( self->ig_grab_changed_cb )
		self->ig_grab_changed_cb(self, self->ig_have_grab);

EXIT:
	return;
}

/** Feed touching/pressed state to input grab state machine
 */
static void input_grab_set_touching(input_grab_t *self, bool touching)
{
	if( self->ig_touching == touching )
		goto EXIT;

	mce_log(LL_DEBUG, "touching(%s) = %s", self->ig_name, touching ? "yes" : "no");

	if( (self->ig_touching = touching) )
		self->ig_touched = true;

	input_grab_rethink(self);

EXIT:
	return;
}

/** Feed desire to grab to input grab state machine
 */
static void input_grab_request_grab(input_grab_t *self, bool want_grab)
{
	if( self->ig_want_grab == want_grab )
		goto EXIT;

	self->ig_want_grab = want_grab;

	input_grab_rethink(self);

EXIT:
	return;
}

/** Callback for changing iomonitor input grab state
 */
static void input_grab_iomon_cb(gpointer data, gpointer user_data)
{
	gpointer iomon = data;
	int grab = GPOINTER_TO_INT(user_data);
	int fd   = mce_get_io_monitor_fd(iomon);

	if( fd == -1 )
		goto EXIT;

	const char *path = mce_get_io_monitor_name(iomon) ?: "unknown";

	if( ioctl(fd, EVIOCGRAB, grab) == -1 ) {
		mce_log(LL_ERR, "EVIOCGRAB(%s, %d): %m", path, grab);
		goto EXIT;
	}
	mce_log(LL_DEBUG, "%sGRABBED fd=%d path=%s",
		grab ? "" : "UN", fd, path);

EXIT:
	return;
}

/* ========================================================================= *
 * TOUCHSCREEN EVDEV INPUT GRAB
 * ========================================================================= */

/** Low level helper for input grab debug led pattern activate/deactivate
 */
static void ts_grab_set_led_raw(bool enabled)
{
	execute_datapipe_output_triggers(enabled ?
					 &led_pattern_activate_pipe :
					 &led_pattern_deactivate_pipe,
					 "PatternTouchInputBlocked",
					 USE_INDATA);
}

/** Handle delayed input grab led pattern activation
 */
static gboolean ts_grab_set_led_cb(gpointer aptr)
{
	guint *id = aptr;

	if( !*id )
		goto EXIT;

	*id = 0;
	ts_grab_set_led_raw(true);
EXIT:
	return FALSE;
}

/** Handle grab led pattern activation/deactivation
 *
 * Deactivation happens immediately.
 * Activation after brief delay
 */
static void ts_grab_set_led(bool enabled)
{
	static guint id = 0;

	static bool prev = false;

	if( prev == enabled )
		goto EXIT;

	if( id )
		g_source_remove(id), id = 0;

	if( enabled )
		id = g_timeout_add(200, ts_grab_set_led_cb, &id);
	else
		ts_grab_set_led_raw(false);

	prev = enabled;
EXIT:
	return;
}

/** Grab/ungrab all monitored touch input devices
 */
static void ts_grab_set_active(gboolean grab)
{
	static gboolean old_grab = FALSE;

	if( old_grab == grab )
		goto EXIT;

	old_grab = grab;
	g_slist_foreach(touchscreen_dev_list,
			input_grab_iomon_cb,
			GINT_TO_POINTER(grab));

	// STATE MACHINE -> OUTPUT DATAPIPE
	execute_datapipe(&touch_grab_active_pipe,
			 GINT_TO_POINTER(grab),
			 USE_INDATA, CACHE_INDATA);

	/* disable led pattern if grab ended */
	if( !grab )
		ts_grab_set_led(false);

EXIT:
	return;
}

/** Query palm detection state
 *
 * Used to keep touch input in unreleased state even if finger touch
 * events are not coming in.
 */
static bool ts_grab_poll_palm_detect(input_grab_t *ctrl)
{
	(void)ctrl;

	static const char path[] = "/sys/devices/i2c-3/3-0020/palm_status";

	bool released = true;

	int fd = -1;
	char buf[32];
	if( (fd = open(path, O_RDONLY)) == -1 ) {
		if( errno != ENOENT )
			mce_log(LL_ERR, "can't open %s: %m", path);
		goto EXIT;
	}

	int rc = read(fd, buf, sizeof buf - 1);
	if( rc < 0 ) {
		mce_log(LL_ERR, "can't read %s: %m", path);
		goto EXIT;
	}

	buf[rc] = 0;
	released = (strtol(buf, 0, 0) == 0);

EXIT:
	if( fd != -1 && close(fd) == -1 )
		mce_log(LL_WARN, "can't close %s: %m", path);

	return released;
}

/** Handle grab state notifications from generic input grab state machine
 */
static void ts_grab_changed(input_grab_t *ctrl, bool grab)
{
	(void)ctrl;

	ts_grab_set_active(grab);
}

enum
{
	TS_RELEASE_DELAY_DEFAULT = 100,
	TS_RELEASE_DELAY_BLANK   = 100,
	TS_RELEASE_DELAY_UNBLANK = 600,
};

/** State data for touch input grab state machine */
static input_grab_t ts_grab_state =
{
	.ig_name      = "ts",

	.ig_touching  = false,
	.ig_touched   = false,

	.ig_want_grab = false,
	.ig_have_grab = false,

	.ig_release_id = 0,
	.ig_release_ms = TS_RELEASE_DELAY_DEFAULT,

	.ig_grab_changed_cb = ts_grab_changed,
	.ig_release_verify_cb = ts_grab_poll_palm_detect,
};

/* Touch unblock delay from settings [ms] */
static gint ts_grab_release_delay = TS_RELEASE_DELAY_DEFAULT;

/** GConf notification ID for touch unblock delay */
static guint ts_grab_release_delay_id = 0;

/** Gconf notification callback for touch unblock delay
 *
 * @param client (not used)
 * @param id     (not used)
 * @param entry  GConf entry that changed
 * @param data   (not used)
 */
static void ts_grab_release_delay_cb(GConfClient *const client,
				     const guint id,
				     GConfEntry *const entry,
				     gpointer const data)
{
	(void)client; (void)id; (void)data;

	gint delay = ts_grab_release_delay;
	const GConfValue *value = 0;

	if( !entry )
		goto EXIT;

	if( !(value = gconf_entry_get_value(entry)) )
		goto EXIT;

	if( value->type == GCONF_VALUE_INT )
		delay = gconf_value_get_int(value);

	if( ts_grab_release_delay == delay )
		goto EXIT;

	mce_log(LL_NOTICE, "touch unblock delay changed: %d -> %d",
		ts_grab_release_delay, delay);

	ts_grab_release_delay = delay;

	// NB: currently active timer is not reprogrammed, change
	//     will take effect on the next unblank
EXIT:
	return;
}

/** Event filter for determining finger on screen state
 */
static void ts_grab_event_filter_cb(struct input_event *ev)
{
	static bool x = false, y = false, p = false, r = false;

	switch( ev->type ) {
	case EV_SYN:
		switch( ev->code ) {
		case SYN_MT_REPORT:
			r = true;
			break;

		case SYN_REPORT:
			if( r ) {
				input_grab_set_touching(&ts_grab_state,
							x && y && p);
				x = y = p = r = false;
			}
			break;

		default:
			break;
		}
		break;

	case EV_ABS:
		switch( ev->code ) {
		case ABS_MT_POSITION_X:
			x = true;
			break;

		case ABS_MT_POSITION_Y:
			y = true;
			break;

		case ABS_MT_TOUCH_MAJOR:
		case ABS_MT_PRESSURE:
			p = true;
			break;

		default:
			break;
		}
		break;

	default:
		break;
	}
}

/** Initialize touch screen grabbing state machine
 */
static void ts_grab_init(void)
{
	/* Get touch unblock delay */
	mce_gconf_notifier_add(MCE_GCONF_EVENT_INPUT_PATH,
			       MCE_GCONF_TOUCH_UNBLOCK_DELAY_PATH,
			       ts_grab_release_delay_cb,
			       &ts_grab_release_delay_id);

	mce_gconf_get_int(MCE_GCONF_TOUCH_UNBLOCK_DELAY_PATH,
			  &ts_grab_release_delay);

	mce_log(LL_NOTICE, "touch unblock delay config: %d",
		ts_grab_release_delay);

	ts_grab_state.ig_release_ms = ts_grab_release_delay;
}

/** De-initialize touch screen grabbing state machine
 */
static void ts_grab_quit(void)
{
	mce_gconf_notifier_remove(ts_grab_release_delay_id),
		ts_grab_release_delay_id = 0;

	input_grab_reset(&ts_grab_state);
}

/** feed desired touch grab state from datapipe to state machine
 *
 * @param data The grab wanted boolean as a pointer
 */
static void ts_grab_wanted_cb(gconstpointer data)
{
	bool required = GPOINTER_TO_INT(data);

	// INPUT DATAPIPE -> STATE MACHINE

	input_grab_request_grab(&ts_grab_state, required);
}

static void ts_grab_display_state_cb(gconstpointer data)
{
	static display_state_t prev = MCE_DISPLAY_UNDEF;

	display_state_t display_state = GPOINTER_TO_INT(data);

	mce_log(LL_DEBUG, "display_state=%d", display_state);

	switch( display_state ) {
	case MCE_DISPLAY_POWER_DOWN:
		/* Deactivate debug led pattern once we start to
		 * power off display and touch panel. */
		ts_grab_set_led(false);
		break;

	case MCE_DISPLAY_OFF:
		/* When display and touch are powered off the
		 * touch should get explicitly terminated by
		 * the kernel side. To keep the state machine
		 * sane in case that does not happen, fake a
		 * touch release anyway.
		 *
		 * Note: Display state machine keeps the device
		 * wakelocked for a second after reaching display
		 * off state -> short delays here work without
		 * explicit wakelocking. */
		ts_grab_state.ig_release_ms = TS_RELEASE_DELAY_BLANK;
		input_grab_set_touching(&ts_grab_state, false);
		break;

	case MCE_DISPLAY_POWER_UP:
		/* Fake a touch to keep statemachine from releasing
		 * the input grab before we have a change to get
		 * actual input from the touch panel. */
		ts_grab_state.ig_release_ms = TS_RELEASE_DELAY_UNBLANK;
		input_grab_set_touching(&ts_grab_state, true);

	case MCE_DISPLAY_ON:
	case MCE_DISPLAY_DIM:
		ts_grab_state.ig_release_ms = ts_grab_release_delay;
		if( prev != MCE_DISPLAY_ON && prev != MCE_DISPLAY_DIM ) {
			/* End the faked touch once the display is
			 * fully on. If there is a finger on the
			 * screen we will get more input events
			 * before the delay from artificial touch
			 * release ends. */
			input_grab_set_touching(&ts_grab_state, false);
		}
		/* Activate (delayed) debug led pattern if we reach
		 * display on with input grabbed */
		if( datapipe_get_gint(touch_grab_active_pipe) ) {
			ts_grab_set_led(true);
		}
		break;

	default:
	case MCE_DISPLAY_LPM_ON:
	case MCE_DISPLAY_UNDEF:
	case MCE_DISPLAY_LPM_OFF:
		break;
	}

	prev = display_state;
}

/* ========================================================================= *
 * KEYPAD IO GRAB
 * ========================================================================= */

/** Grab/ungrab all monitored volumekey input devices
 */
static void kp_grab_set_active(gboolean grab)
{
	static gboolean old_grab = FALSE;

	if( old_grab == grab )
		goto EXIT;

	old_grab = grab;
	g_slist_foreach(volumekey_dev_list,
			input_grab_iomon_cb,
			GINT_TO_POINTER(grab));

	// STATE MACHINE -> OUTPUT DATAPIPE
	execute_datapipe(&keypad_grab_active_pipe,
			 GINT_TO_POINTER(grab),
			 USE_INDATA, CACHE_INDATA);

EXIT:
	return;
}

/** Handle grab state notifications from generic input grab state machine
 */
static void kp_grab_changed(input_grab_t *ctrl, bool grab)
{
	(void)ctrl;

	kp_grab_set_active(grab);
}

/** State data for volumekey input grab state machine */
static input_grab_t kp_grab_state =
{
	.ig_name      = "kp",

	.ig_touching  = false,
	.ig_touched   = false,

	.ig_want_grab = false,
	.ig_have_grab = false,

	.ig_release_id = 0,
	.ig_release_ms = 200,

	.ig_grab_changed_cb = kp_grab_changed,
};

/** Event filter for determining volume key pressed state
 */
static void kp_grab_event_filter_cb(struct input_event *ev)
{
	static bool vol_up = false;
	static bool vol_dn = false;

	switch( ev->type ) {
	case EV_KEY:
		switch( ev->code ) {
		case KEY_VOLUMEUP:
			vol_up = (ev->value != 0);
			break;

		case KEY_VOLUMEDOWN:
			vol_dn = (ev->value != 0);
			break;

		default:
			break;
		}
		break;

	default:
		break;
	}

	input_grab_set_touching(&kp_grab_state, vol_up || vol_dn);
}

/** Feed desired volumekey grab state from datapipe to state machine
 *
 * @param data The grab wanted boolean as a pointer
 */
static void kp_grab_wanted_cb(gconstpointer data)
{
	bool required = GPOINTER_TO_INT(data);

	// INPUT DATAPIPE -> STATE MACHINE
	input_grab_request_grab(&kp_grab_state, required);
}

#ifdef ENABLE_DOUBLETAP_EMULATION

/** Fake doubletap policy */
static gboolean fake_doubletap_enabled = FALSE;

/** GConf callback ID for fake doubletap policy changes */
static guint fake_doubletap_id = 0;

/** Callback for handling changes to fake doubletap configuration
 *
 * @param client (not used)
 * @param id     (not used)
 * @param entry  GConf entry that changed
 * @param data   (not used)
 */
static void fake_doubletap_cb(GConfClient *const client, const guint id,
			      GConfEntry *const entry, gpointer const data)
{
	(void)client; (void)id; (void)data;

	gboolean enabled = fake_doubletap_enabled;
	const GConfValue *value = 0;

	if( entry && (value = gconf_entry_get_value(entry)) ) {
		if( value->type == GCONF_VALUE_BOOL )
			enabled = gconf_value_get_bool(value);
	}
	if( fake_doubletap_enabled != enabled ) {
		mce_log(LL_NOTICE, "use fake doubletap change: %d -> %d",
			fake_doubletap_enabled, enabled);
		fake_doubletap_enabled = enabled;
	}
}

/** Maximum time betweem 1st click and 2nd release, in milliseconds */
# define DOUBLETAP_TIME_LIMIT 500

/** Maximum distance between 1st and 2nd clicks, in pixels */
# define DOUBLETAP_DISTANCE_LIMIT 100

/** History data for emulating double tap */
typedef struct
{
	struct timeval time;
	int x,y;
	int click;
} doubletap_t;

/** Check if two double tap history points are close enough in time
 *
 * @param e1 event data from the 1st click
 * @param e2 event data from the 2nd release
 *
 * @return TRUE if e1 and e2 times are valid and close enough,
 *         or FALSE otherwise
 */
static int doubletap_time_p(const doubletap_t *e1, const doubletap_t *e2)
{
	static const struct timeval limit =
	{
		.tv_sec  = (DOUBLETAP_TIME_LIMIT / 1000),
		.tv_usec = (DOUBLETAP_TIME_LIMIT % 1000) * 1000,
	};

	struct timeval delta;

	/* Reject empty/reset slots */
	if( !timerisset(&e1->time) || !timerisset(&e2->time) )
		return 0;

	timersub(&e2->time, &e1->time, &delta);
	return timercmp(&delta, &limit, <);
}

/** Check if two double tap history points are close enough in pixels
 *
 * @param e1 event data from the 1st click
 * @param e2 event data from the 2nd click
 *
 * @return TRUE if e1 and e2 positions are close enough, or FALSE otherwise
 */
static int doubletap_dist_p(const doubletap_t *e1, const doubletap_t *e2)
{
	int x = e2->x - e1->x;
	int y = e2->y - e1->y;
	int r = DOUBLETAP_DISTANCE_LIMIT;

	return (x*x + y*y) < (r*r);
}

/** Accumulator steps for counting touch/mouse click events separately
 *
 *    2   2   2   1   1   0   0   0
 *    8   4   0   6   2   8   4   0
 * --------------------------------
 *                             mmmm [ 3: 0]  BTN_MOUSE
 *                         pppp     [ 7: 4]  ABS_MT_PRESSURE
 *                     tttt         [11: 8]  ABS_MT_TOUCH_MAJOR
 *                 iiii             [15:12]  ABS_MT_TRACKING_ID
 * aaaabbbbccccdddd                 [31:16]  (reserved)
 */
enum {

	SEEN_EVENT_MOUSE       = 1 <<  0,
	SEEN_EVENT_PRESSURE    = 1 <<  4,
	SEEN_EVENT_TOUCH_MAJOR = 1 <<  8,
	SEEN_EVENT_TRACKING_ID = 1 << 12,
};

/** Helper for probing no-touch vs single-touch vs multi-touch
 *
 * return 0 for no-touch, 1 for single touch, >1 for multi-touch
 */
static int doubletap_touch_points(const doubletap_t *e)
{
	/* The bit shuffling below calculates maximum number of mouse
	 * button click / touch point events accumulated to the history
	 * buffer to produce return value of
	 *
	 *   =0 -> no touch
	 *   =1 -> singletouch
	 *   >1 -> multitouch
	 *
	 * Note: If the event stream happens to report one ABS_MT_PRESSURE
	 * and two ABS_MT_TOUCH_MAJOR events / something similar it will
	 * be reported as "triple touch", but we do not need care as long
	 * as it is not "no touch" or "singletouch".
	 */

	unsigned m = e->click;
	m |= (m >> 16);
	m |= (m >>  8);
	m |= (m >>  4);
	return m & 15;
}

/** Process mouse input events to simulate double tap
 *
 * Maintain a crude state machine, that will detect double clicks
 * made with mouse when fed with evdev events from a mouse device.
 *
 * @param eve input event
 *
 * @return TRUE if double tap sequence was detected, FALSE otherwise
 */
static int doubletap_emulate(const struct input_event *eve)
{
	static doubletap_t hist[4]; // click/release ring buffer

	static unsigned i0       = 0; // current position
	static int      x_accum  = 0; // x delta accumulator
	static int      y_accum  = 0; // y delta accumulator
	static bool     skip_syn = true; // flag: ignore SYN_REPORT

	int result = FALSE; // assume: no doubletap

	unsigned i1, i2, i3; // 3 last positions

	switch( eve->type ) {
	case EV_REL:
		/* Accumulate X/Y position */
		switch( eve->code ) {
		case REL_X: x_accum += eve->value; break;
		case REL_Y: y_accum += eve->value; break;
		default: break;
		}
		break;

	case EV_KEY:
		if( eve->code == BTN_MOUSE ) {
			/* Store click/release and position */
			if( eve->value )
				hist[i0].click += SEEN_EVENT_MOUSE;
			hist[i0].x = x_accum;
			hist[i0].y = y_accum;

			/* We have a mouse click to process */
			skip_syn = false;
		}
		break;

	case EV_ABS:
		/* Do multitouch too while at it */
		switch( eve->code ) {
		case ABS_MT_PRESSURE:
			hist[i0].click += SEEN_EVENT_PRESSURE;
			skip_syn = false;
			break;
		case ABS_MT_TOUCH_MAJOR:
			hist[i0].click += SEEN_EVENT_TOUCH_MAJOR;
			skip_syn = false;
			break;
		case ABS_MT_TRACKING_ID:
			if( eve->value != -1 )
				hist[i0].click += SEEN_EVENT_TRACKING_ID;
			skip_syn = false;
			break;
		case ABS_MT_POSITION_X:
			hist[i0].x = eve->value;
			skip_syn = false;
			break;
		case ABS_MT_POSITION_Y:
			hist[i0].y = eve->value;
			skip_syn = false;
			break;
		default:
			break;
		}
		break;

	case EV_SYN:
		if( eve->code == SYN_MT_REPORT ) {
			/* We have a touch event to process */
			skip_syn = false;
			break;
		}

		if( eve->code != SYN_REPORT )
			break;

		/* Have we seen button events? */
		if( skip_syn )
			break;

		/* Next SYN_REPORT will be ignored unless something
		 * relevant is seen before that */
		skip_syn = true;

		/* Set timestamp from syn event */
		hist[i0].time = eve->time;

		/* Last event before current */
		i1 = (i0 + 3) & 3;

		int tp0 = doubletap_touch_points(hist+i0);
		int tp1 = doubletap_touch_points(hist+i1);

		if( tp0 != tp1 ) {
			/* 2nd and 3rd last events before current */
			i2 = (i0 + 2) & 3;
			i3 = (i0 + 1) & 3;

			int tp2 = doubletap_touch_points(hist+i2);
			int tp3 = doubletap_touch_points(hist+i3);

			/* Release after click after release after click,
			 * within the time and distance limits */
			if( tp0 == 0 && tp1 == 1 && tp2 == 0 && tp3 == 1 &&
			    doubletap_time_p(&hist[i3], &hist[i0]) &&
			    doubletap_dist_p(&hist[i3], &hist[i1]) ) {
				/* Reached DOUBLETAP state */
				result = TRUE;

				/* Reset history, so that triple click
				 * will not produce 2 double taps etc */
				memset(hist, 0, sizeof hist);
				x_accum = y_accum = 0;
			}

			/* Move to the next slot */
			i0 = (i0 + 1) & 3;
		}

		/* Reset the current position in the ring buffer */
		memset(&hist[i0], 0, sizeof *hist);
		break;

	default:
		break;
	}

	return result;
}

#endif /* ENABLE_DOUBLETAP_EMULATION */

/**
 * I/O monitor callback for the touchscreen
 *
 * @param data The new data
 * @param bytes_read The number of bytes read
 * @return FALSE to return remaining chunks (if any),
 *         TRUE to flush all remaining chunks
 */
static gboolean touchscreen_iomon_cb(gpointer data, gsize bytes_read)
{
	static time_t last_activity = 0;

	submode_t submode = mce_get_submode_int32();
	struct input_event *ev;
	gboolean flush = FALSE;

	ev = data;

	/* Don't process invalid reads */
	if (bytes_read != sizeof (*ev)) {
		goto EXIT;
	}

	mce_log(LL_DEBUG, "type: %s, code: %s, value: %d",
		evdev_get_event_type_name(ev->type),
		evdev_get_event_code_name(ev->type, ev->code),
		ev->value);

	ts_grab_event_filter_cb(ev);

	bool grabbed = datapipe_get_gint(touch_grab_active_pipe);

#ifdef ENABLE_DOUBLETAP_EMULATION
	if( grabbed || fake_doubletap_enabled ) {
		/* Note: In case we happen to be in middle of display
		 *       state transition the double tap simulation must
		 *       use the next stable display state rather than
		 *       the current - potentially transitional - state.
		 */
		display_state_t display_state_next =
			datapipe_get_gint(display_state_next_pipe);

		switch( display_state_next ) {
		case MCE_DISPLAY_OFF:
		case MCE_DISPLAY_LPM_OFF:
		case MCE_DISPLAY_LPM_ON:
			if( doubletap_emulate(ev) ) {
				mce_log(LL_DEVEL, "[doubletap] emulated from touch input");
				ev->type  = EV_MSC;
				ev->code  = MSC_GESTURE;
				ev->value = 0x4;
			}
			break;
		default:
		case MCE_DISPLAY_ON:
		case MCE_DISPLAY_DIM:
		case MCE_DISPLAY_UNDEF:
		case MCE_DISPLAY_POWER_UP:
		case MCE_DISPLAY_POWER_DOWN:
			break;
		}
	}
#endif

	/* Ignore unwanted events */
	if ((ev->type != EV_ABS) &&
	    (ev->type != EV_KEY) &&
	    (ev->type != EV_MSC)) {
		goto EXIT;
	}

	/* Do not generate activity if ts input is grabbed */
	if( !grabbed ) {
		/* For generic activity once/second is more than enough
		 * ... unless we need to disable event eater */
		if( last_activity != ev->time.tv_sec ||
		    (submode & MCE_EVEATER_SUBMODE) ) {
			last_activity = ev->time.tv_sec;

			/* Generate activity */
			execute_datapipe(&device_inactive_pipe,
					 GINT_TO_POINTER(FALSE),
					 USE_INDATA, CACHE_INDATA);
		}

		/* Signal actual non-synthetized user activity */
		execute_datapipe_output_triggers(&user_activity_pipe,
							 ev, USE_INDATA);
	}

	/* If the event eater is active, don't send anything */
	if( submode & MCE_EVEATER_SUBMODE )
		goto EXIT;

	/* Only send pressure and gesture events */
	if (((ev->type != EV_ABS) || (ev->code != ABS_PRESSURE)) &&
	    ((ev->type != EV_KEY) || (ev->code != BTN_TOUCH)) &&
	    ((ev->type != EV_MSC) || (ev->code != MSC_GESTURE))) {
		goto EXIT;
	}

	/* For now there's no reason to cache the value */
	execute_datapipe(&touchscreen_pipe, &ev,
			 USE_INDATA, DONT_CACHE_INDATA);

EXIT:
	return flush;
}

/** Proximity state enum to human readable string
 */
static const char *proximity_state_repr(cover_state_t state)
{
	const char *repr = "unknown";
	switch( state ) {
	case COVER_UNDEF:  repr = "undefined";   break;
	case COVER_CLOSED: repr = "covered";     break;
	case COVER_OPEN:   repr = "not covered"; break;
	default:
		break;
	}
	return repr;
}

static gboolean doubletap_iomon_cb(gpointer data, gsize bytes_read)
{
	struct input_event *ev = data;
	gboolean flush = FALSE;

	/* Don't process invalid reads */
	if (bytes_read != sizeof (*ev)) {
		goto EXIT;
	}

	/* Power key up event -> double tap gesture event */
	if( ev->type == EV_KEY &&
	    ev->code == KEY_POWER &&
	    ev->value == 0 ) {
		cover_state_t proximity_sensor_state =
			datapipe_get_gint(proximity_sensor_pipe);

		mce_log(LL_DEVEL, "[doubletap] while proximity=%s",
			proximity_state_repr(proximity_sensor_state));

		/* Mimic N9 style gesture event for which we
		 * already have logic in place. Possible filtering
		 * due to proximity state etc happens at tklock.c
		 */
		ev->type  = EV_MSC;
		ev->code  = MSC_GESTURE;
		ev->value = 0x4;
		touchscreen_iomon_cb(ev, sizeof *ev);
	}
EXIT:
	return flush;
}

/**
 * Timeout function for keypress repeats
 * @note Empty function; we check the callback id
 *       for 0 to know if we've had a timeout or not
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean keypress_repeat_timeout_cb(gpointer data)
{
	(void)data;

	keypress_repeat_timeout_cb_id = 0;

	return FALSE;
}

/**
 * Cancel timeout for keypress repeats
 */
static void cancel_keypress_repeat_timeout(void)
{
	if (keypress_repeat_timeout_cb_id != 0) {
		g_source_remove(keypress_repeat_timeout_cb_id);
		keypress_repeat_timeout_cb_id = 0;
	}
}

/**
 * Setup timeout for touchscreen I/O monitoring
 */
static void setup_keypress_repeat_timeout(void)
{
	cancel_keypress_repeat_timeout();

	/* Setup new timeout */
	keypress_repeat_timeout_cb_id =
		g_timeout_add_seconds(MONITORING_DELAY,
				      keypress_repeat_timeout_cb, NULL);
}

/**
 * I/O monitor callback for keypresses
 *
 * @param data The new data
 * @param bytes_read The number of bytes read
 * @return Always returns FALSE to return remaining chunks (if any)
 */
static gboolean keypress_iomon_cb(gpointer data, gsize bytes_read)
{
	submode_t submode = mce_get_submode_int32();
	struct input_event *ev;

	ev = data;

	/* Don't process invalid reads */
	if (bytes_read != sizeof (*ev)) {
		goto EXIT;
	}

	mce_log(LL_DEBUG, "type: %s, code: %s, value: %d",
		evdev_get_event_type_name(ev->type),
		evdev_get_event_code_name(ev->type, ev->code),
		ev->value);

	kp_grab_event_filter_cb(ev);

	/* Ignore non-keypress events */
	if ((ev->type != EV_KEY) && (ev->type != EV_SW)) {
		goto EXIT;
	}

	if (ev->type == EV_KEY) {
		if( datapipe_get_gint(keypad_grab_active_pipe) ) {
			switch( ev->code ) {
			case KEY_VOLUMEUP:
			case KEY_VOLUMEDOWN:
				mce_log(LL_DEVEL, "ignore volume key event");
				goto EXIT;
			default:
				break;
			}
		}
		if ((ev->code == KEY_SCREENLOCK) && (ev->value != 2)) {
			(void)execute_datapipe(&lockkey_pipe,
					       GINT_TO_POINTER(ev->value),
					       USE_INDATA, CACHE_INDATA);
		}

		/* For now there's no reason to cache the keypress
		 *
		 * If the event eater is active, and this is the press,
		 * don't send anything; never eat releases, otherwise
		 * the release event for a [power] press might get lost
		 * and the device shut down...  Not good(tm)
		 *
		 * Also, don't send repeat events, and don't send
		 * keypress events for the focus and screenlock keys
		 *
		 * Additionally ignore all key events if proximity locked
		 * during a call or alarm.
		 */
		if (((ev->code != KEY_CAMERA_FOCUS) &&
		     (ev->code != KEY_SCREENLOCK) &&
		     ((((submode & MCE_EVEATER_SUBMODE) == 0) &&
		       (ev->value == 1)) || (ev->value == 0))) &&
		    ((submode & MCE_PROXIMITY_TKLOCK_SUBMODE) == 0)) {
			(void)execute_datapipe(&keypress_pipe, &ev,
					       USE_INDATA, DONT_CACHE_INDATA);
		}
	}

	if (ev->type == EV_SW) {
		switch (ev->code) {
		case SW_CAMERA_LENS_COVER:
			if (ev->value != 2) {
				(void)execute_datapipe(&lens_cover_pipe, GINT_TO_POINTER(ev->value ? COVER_CLOSED : COVER_OPEN), USE_INDATA, CACHE_INDATA);
			}

			/* Don't generate activity on COVER_CLOSED */
			if (ev->value == 1)
				goto EXIT;

			break;

		case SW_KEYPAD_SLIDE:
			if (ev->value != 2) {
				(void)execute_datapipe(&keyboard_slide_pipe, GINT_TO_POINTER(ev->value ? COVER_CLOSED : COVER_OPEN), USE_INDATA, CACHE_INDATA);
			}

			/* Don't generate activity on COVER_CLOSED */
			if (ev->value == 1)
				goto EXIT;

			break;

		case SW_FRONT_PROXIMITY:
			if (ev->value != 2) {
				(void)execute_datapipe(&proximity_sensor_pipe, GINT_TO_POINTER(ev->value ? COVER_CLOSED : COVER_OPEN), USE_INDATA, CACHE_INDATA);
			}

			break;

		case SW_HEADPHONE_INSERT:
		case SW_MICROPHONE_INSERT:
		case SW_LINEOUT_INSERT:
		case SW_VIDEOOUT_INSERT:
			if (ev->value != 2) {
				(void)execute_datapipe(&jack_sense_pipe, GINT_TO_POINTER(ev->value ? COVER_CLOSED : COVER_OPEN), USE_INDATA, CACHE_INDATA);
			}

			break;

		/* Other switches do not have custom actions */
		default:
			break;
		}
	}

	/* Power key events ... */
	if( ev->type == EV_KEY && ev->code == KEY_POWER ) {
		/* .. count as actual non-synthetized user activity */
		execute_datapipe_output_triggers(&user_activity_pipe,
						 ev, USE_INDATA);

		/* but otherwise are handled in powerkey module */
		mce_log(LL_DEBUG, "ignore power key event");
		goto EXIT;
	}

	/* Generate activity:
	 * 0 - release (always)
	 * 1 - press (always)
	 * 2 - repeat (once a second)
	 */
	if ((ev->value == 0) || (ev->value == 1) ||
	    ((ev->value == 2) && (keypress_repeat_timeout_cb_id == 0))) {
		(void)execute_datapipe(&device_inactive_pipe,
				       GINT_TO_POINTER(FALSE),
				       USE_INDATA, CACHE_INDATA);

		if (ev->value == 2) {
			setup_keypress_repeat_timeout();
		}
	}

EXIT:
	return FALSE;
}

/**
 * Timeout callback for misc event monitoring reprogramming
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean misc_io_monitor_timeout_cb(gpointer data)
{
	(void)data;

	misc_io_monitor_timeout_cb_id = 0;

	/* Resume I/O monitors */
	g_slist_foreach(misc_dev_list, resume_io_monitor, 0);

	return FALSE;
}

/**
 * Cancel timeout for misc event I/O monitoring
 */
static void cancel_misc_io_monitor_timeout(void)
{
	if (misc_io_monitor_timeout_cb_id != 0) {
		g_source_remove(misc_io_monitor_timeout_cb_id);
		misc_io_monitor_timeout_cb_id = 0;
	}
}

/**
 * Setup timeout for misc event I/O monitoring
 */
static void setup_misc_io_monitor_timeout(void)
{
	cancel_misc_io_monitor_timeout();

	/* Setup new timeout */
	misc_io_monitor_timeout_cb_id =
		g_timeout_add_seconds(MONITORING_DELAY,
				      misc_io_monitor_timeout_cb, NULL);
}

/**
 * I/O monitor callback for misc /dev/input devices
 *
 * @param data Unused
 * @param bytes_read Unused
 * @return Always returns FALSE to return remaining chunks (if any)
 */
static gboolean misc_iomon_cb(gpointer data, gsize bytes_read)
{
	struct input_event *ev;

	ev = data;

	/* Don't process invalid reads */
	if (bytes_read != sizeof (*ev)) {
		goto EXIT;
	}

	mce_log(LL_DEBUG, "type: %s, code: %s, value: %d",
		evdev_get_event_type_name(ev->type),
		evdev_get_event_code_name(ev->type, ev->code),
		ev->value);

	/* Ignore synchronisation, force feedback, LED,
	 * and force feedback status
	 */
	switch (ev->type) {
	case EV_SYN:
	case EV_LED:
	case EV_SND:
	case EV_FF:
	case EV_FF_STATUS:
		goto EXIT;

	default:
		break;
	}

	/* ev->type for the jack sense is EV_SW */
	mce_log(LL_DEBUG, "ev->type: %d", ev->type);

	/* Generate activity */
	(void)execute_datapipe(&device_inactive_pipe, GINT_TO_POINTER(FALSE),
			       USE_INDATA, CACHE_INDATA);

	/* Suspend I/O monitors */
	g_slist_foreach(misc_dev_list, suspend_io_monitor, 0);

	/* Setup a timeout I/O monitor reprogramming */
	setup_misc_io_monitor_timeout();

EXIT:
	return FALSE;
}

/**
 * Check whether the fd in question supports the switches
 * we want information about -- if so, update their state
 */
static void get_switch_state(gpointer io_monitor, gpointer user_data)
{
	/* Get the fd of the I/O monitor */
	const gchar *filename = mce_get_io_monitor_name(io_monitor);
	int fd = mce_get_io_monitor_fd(io_monitor);
	gulong *featurelist = NULL;
	gulong *statelist = NULL;
	gsize featurelistlen;
	gint state;

	(void)user_data;

	featurelistlen = (KEY_CNT / bitsize_of(*featurelist)) +
			 ((KEY_CNT % bitsize_of(*featurelist)) ? 1 : 0);
	featurelist = g_malloc0(featurelistlen * sizeof (*featurelist));
	statelist = g_malloc0(featurelistlen * sizeof (*statelist));

	if (ioctl(fd, EVIOCGBIT(EV_SW, SW_MAX), featurelist) == -1) {
		mce_log(LL_ERR,
			"ioctl(EVIOCGBIT(EV_SW, SW_MAX)) failed on `%s'; %s",
			filename, g_strerror(errno));
		errno = 0;
		goto EXIT;
	}

	if (ioctl(fd, EVIOCGSW(SW_MAX), statelist) == -1) {
		mce_log(LL_ERR,
			"ioctl(EVIOCGSW(SW_MAX)) failed on `%s'; %s",
			filename, g_strerror(errno));
		errno = 0;
		goto EXIT;
	}

	if (test_bit(SW_CAMERA_LENS_COVER, featurelist) == TRUE) {
		state = test_bit(SW_CAMERA_LENS_COVER, statelist);

		(void)execute_datapipe(&lens_cover_pipe, GINT_TO_POINTER(state ? COVER_CLOSED : COVER_OPEN), USE_INDATA, CACHE_INDATA);
	}

	if (test_bit(SW_KEYPAD_SLIDE, featurelist) == TRUE) {
		state = test_bit(SW_KEYPAD_SLIDE, statelist);

		(void)execute_datapipe(&keyboard_slide_pipe, GINT_TO_POINTER(state ? COVER_CLOSED : COVER_OPEN), USE_INDATA, CACHE_INDATA);
	}

	if (test_bit(SW_FRONT_PROXIMITY, featurelist) == TRUE) {
		state = test_bit(SW_FRONT_PROXIMITY, statelist);

		(void)execute_datapipe(&proximity_sensor_pipe, GINT_TO_POINTER(state ? COVER_CLOSED : COVER_OPEN), USE_INDATA, CACHE_INDATA);
	}

	/* Need to consider more than one switch state when setting the
	 * initial value of the jack_sense_pipe */

	bool have = false;
	state = 0;

	if( test_bit(SW_HEADPHONE_INSERT, featurelist) )
		have = true, state |= test_bit(SW_HEADPHONE_INSERT, statelist);
	if( test_bit(SW_MICROPHONE_INSERT, featurelist) )
		have = true, state |= test_bit(SW_MICROPHONE_INSERT, statelist);
	if( test_bit(SW_LINEOUT_INSERT, featurelist) )
		have = true, state |= test_bit(SW_LINEOUT_INSERT, statelist);
	if( test_bit(SW_VIDEOOUT_INSERT, featurelist) )
		have = true, state |= test_bit(SW_VIDEOOUT_INSERT, statelist);

	if( have )
		execute_datapipe(&jack_sense_pipe,
				 GINT_TO_POINTER(state ? COVER_CLOSED : COVER_OPEN),
				 USE_INDATA, CACHE_INDATA);

EXIT:
	g_free(statelist);
	g_free(featurelist);

	return;
}

/**
 * Update switch states
 */
static void update_switch_states(void)
{
	g_slist_foreach(keyboard_dev_list, get_switch_state, 0);
	g_slist_foreach(volumekey_dev_list, get_switch_state, 0);
}

/**
 * Match and register I/O monitor
 */
static void match_and_register_io_monitor(const gchar *filename)
{
	int           fd    = -1;
	int           type  = -1;

	char  name[256];
	const gchar * const *black;

	/* If we cannot open the file, abort */
	if ((fd = open(filename, O_NONBLOCK | O_RDONLY)) == -1) {
		mce_log(LL_DEBUG,
			"Failed to open `%s', skipping",
			filename);
		goto EXIT;
	}

	/* Get name of the evdev node */
	if (ioctl(fd, EVIOCGNAME(sizeof name), name) < 0) {
		mce_log(LL_WARN,
			"ioctl(EVIOCGNAME) failed on `%s'",
			filename);
		goto EXIT;
	}

	/* Probe how mce could use the evdev node */
	type = get_evdev_type(fd);
	mce_log(LL_NOTICE, "%s: \"%s\", probe: %s", filename, name, evdev_class[type]);

	/* Check if the device is blacklisted by name in the config files */
	if( (black = mce_conf_get_blacklisted_event_drivers()) ) {
		for( size_t i = 0; black[i]; i++ ) {
			if( strcmp(name, black[i]) )
				continue;
			mce_log(LL_NOTICE, "%s: \"%s\", is blacklisted", filename, name);
			goto EXIT;
		}
	}

	switch( type ) {
	default:
	case EVDEV_IGNORE:
	case EVDEV_REJECT:
		break;

	case EVDEV_TOUCH:
		touchscreen_dev_add(fd, filename, touchscreen_iomon_cb), fd = -1;
		break;

	case EVDEV_DBLTAP:
		touchscreen_dev_add(fd, filename, doubletap_iomon_cb), fd = -1;
		break;

	case EVDEV_INPUT:
		keyboard_dev_add(fd, filename, keypress_iomon_cb), fd = -1;
		break;

	case EVDEV_VOLKEY:
		volumekey_dev_add(fd, filename, keypress_iomon_cb), fd = -1;
		break;

	case EVDEV_ACTIVITY:
		misc_dev_add(fd, filename, misc_iomon_cb), fd = -1;
		break;

	case EVDEV_ALS:
		/* Hook wakelockable ALS input source */
		mce_sensorfw_als_attach(fd), fd = -1;
		break;

	case EVDEV_PS:
		/* Hook wakelockable PS input source */
		mce_sensorfw_ps_attach(fd), fd = -1;
		break;
	}

EXIT:
	/* Close unmonitored file descriptors */
	if( fd != -1 && TEMP_FAILURE_RETRY(close(fd)) )
		mce_log(LL_ERR, "Failed to close `%s'; %m", filename);
}

/**
 * Update list of input devices
 * Remove the I/O monitor for the specified device (if existing)
 * and (re)open it if available
 *
 * @param device The device to add/remove
 * @param add TRUE if the device was added, FALSE if it was removed
 * @return TRUE on success, FALSE on failure
 */
static void update_inputdevices(const gchar *device, gboolean add)
{
	/* remove existing io monitor */
	mce_unregister_io_monitor_at_path(device);

	/* add new io monitor if so requested */
	if( add )
		match_and_register_io_monitor(device);
}

/**
 * Scan /dev/input for input event devices
 *
 * @todo Modify this function to use g_dir_open/g_dir_read_name/g_dir_close
 * @return TRUE on success, FALSE on failure
 */
static gboolean scan_inputdevices(void)
{
	gboolean       status = FALSE;

	DIR           *dir = NULL;
	struct dirent *de = NULL;

	static const char pfix[] = EVENT_FILE_PREFIX;

	if( !(dir = opendir(DEV_INPUT_PATH)) ) {
		mce_log(LL_ERR, "opendir() failed; %m");
		goto EXIT;
	}

	while( (de = readdir(dir)) != 0 ) {
		if( strncmp(de->d_name, pfix, sizeof pfix - 1) ) {
			mce_log(LL_DEBUG, "`%s/%s' skipped",
				DEV_INPUT_PATH, de->d_name);
		}
		else {
			gchar *path = g_strconcat(DEV_INPUT_PATH, "/",
						  de->d_name, NULL);
			match_and_register_io_monitor(path);
			g_free(path);
		}
	}

	status = TRUE;

EXIT:
	if( dir && closedir(dir) == -1 )
		mce_log(LL_ERR, "closedir() failed; %m");

	return status;
}

/**
 * Unregister monitors for input devices allocated by scan_inputdevices
 */
static void unregister_inputdevices(void)
{
	touchscreen_dev_rem_all();
	keyboard_dev_rem_all();
	volumekey_dev_rem_all();
	misc_dev_rem_all();
}

/**
 * Callback for directory changes
 *
 * @param monitor Unused
 * @param file The file that changed
 * @param other_file Unused
 * @param event_type The event that occured
 * @param user_data Unused
 */
static void dir_changed_cb(GFileMonitor *monitor,
			   GFile *file, GFile *other_file,
			   GFileMonitorEvent event_type, gpointer user_data)
{
	char *filename = g_file_get_basename(file);
	char *filepath = g_file_get_path(file);

	(void)monitor;
	(void)other_file;
	(void)user_data;

	if ((filename == NULL) || (filepath == NULL))
		goto EXIT;

	if (strncmp(filename,
		    EVENT_FILE_PREFIX, strlen(EVENT_FILE_PREFIX)) != 0)
		goto EXIT;

	switch (event_type) {
	case G_FILE_MONITOR_EVENT_CREATED:
		update_inputdevices(filepath, TRUE);
		break;

	case G_FILE_MONITOR_EVENT_DELETED:
		update_inputdevices(filepath, FALSE);
		break;

	default:
		break;
	}

EXIT:
	g_free(filepath);
	g_free(filename);

	return;
}

/**
 * Handle submode change
 *
 * @param data The submode stored in a pointer
 */
static void submode_trigger(gconstpointer data)
{
	static submode_t old_submode = MCE_NORMAL_SUBMODE;
	submode_t submode = GPOINTER_TO_INT(data);

	/* If the tklock is enabled, disable the camera focus interrupts,
	 * since we don't use them anyway
	 */
	if ((submode & MCE_TKLOCK_SUBMODE) != 0) {
		if ((old_submode & MCE_TKLOCK_SUBMODE) == 0) {
			if (gpio_key_disable_exists == TRUE)
				disable_gpio_key(KEY_CAMERA_FOCUS);
		}
	} else {
		if ((old_submode & MCE_TKLOCK_SUBMODE) != 0) {
			if (gpio_key_disable_exists == TRUE)
				enable_gpio_key(KEY_CAMERA_FOCUS);
		}
	}

	old_submode = submode;
}

/**
 * Init function for the /dev/input event component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_input_init(void)
{
	GError *error = NULL;
	gboolean status = FALSE;

	ts_grab_init();

#ifdef ENABLE_DOUBLETAP_EMULATION
	/* Get fake doubletap policy configuration & track changes */
	mce_gconf_notifier_add(MCE_GCONF_EVENT_INPUT_PATH,
			       MCE_GCONF_USE_FAKE_DOUBLETAP_PATH,
			       fake_doubletap_cb,
			       &fake_doubletap_id);
	mce_gconf_get_bool(MCE_GCONF_USE_FAKE_DOUBLETAP_PATH,
			   &fake_doubletap_enabled);
#endif

	/* Append triggers/filters to datapipes */
	append_output_trigger_to_datapipe(&submode_pipe,
					  submode_trigger);
	append_output_trigger_to_datapipe(&display_state_pipe,
					  ts_grab_display_state_cb);
	append_output_trigger_to_datapipe(&touch_grab_wanted_pipe,
					  ts_grab_wanted_cb);
	append_output_trigger_to_datapipe(&keypad_grab_wanted_pipe,
					  kp_grab_wanted_cb);

	/* Retrieve a GFile pointer to the directory to monitor */
	dev_input_gfp = g_file_new_for_path(DEV_INPUT_PATH);

	/* Monitor the directory */
	if ((dev_input_gfmp = g_file_monitor_directory(dev_input_gfp,
						       G_FILE_MONITOR_NONE,
						       NULL, &error)) == NULL) {
		mce_log(LL_ERR,
			"Failed to add monitor for directory `%s'; %s",
			DEV_INPUT_PATH, error->message);
		goto EXIT;
	}

	/* XXX: There is a race condition here; if a file (dis)appears
	 *      after this scan, but before we start monitoring,
	 *      then we'll miss that device.  The race is miniscule though,
	 *      and any workarounds are likely to be cumbersome
	 */
	/* Find the initial set of input devices */
	if ((status = scan_inputdevices()) == FALSE) {
		g_file_monitor_cancel(dev_input_gfmp);
		dev_input_gfmp = NULL;
		goto EXIT;
	}

	/* Connect "changed" signal for the directory monitor */
	dev_input_handler_id =
		g_signal_connect(G_OBJECT(dev_input_gfmp), "changed",
				 G_CALLBACK(dir_changed_cb), NULL);

	/* Get configuration options */
	longdelay = mce_conf_get_int(MCE_CONF_HOMEKEY_GROUP,
				     MCE_CONF_HOMEKEY_LONG_DELAY,
				     DEFAULT_HOME_LONG_DELAY);

	update_switch_states();

	gpio_key_disable_exists = (g_access(GPIO_KEY_DISABLE_PATH, W_OK) == 0);

EXIT:
	errno = 0;
	g_clear_error(&error);

	return status;
}

/**
 * Exit function for the /dev/input event component
 */
void mce_input_exit(void)
{
#ifdef ENABLE_DOUBLETAP_EMULATION
	/* Remove fake doubletap policy change notifier */
	mce_gconf_notifier_remove(fake_doubletap_id),
		fake_doubletap_id = 0;
#endif

	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&submode_pipe,
					    submode_trigger);
	remove_output_trigger_from_datapipe(&display_state_pipe,
					    ts_grab_display_state_cb);
	remove_output_trigger_from_datapipe(&touch_grab_wanted_pipe,
					    ts_grab_wanted_cb);
	remove_output_trigger_from_datapipe(&keypad_grab_wanted_pipe,
					    kp_grab_wanted_cb);

	if (dev_input_gfmp != NULL) {
		g_signal_handler_disconnect(G_OBJECT(dev_input_gfmp),
					    dev_input_handler_id);
		dev_input_handler_id = 0;
		g_file_monitor_cancel(dev_input_gfmp);
	}

	unregister_inputdevices();

	/* Remove all timer sources */
	cancel_keypress_repeat_timeout();
	cancel_misc_io_monitor_timeout();

	ts_grab_quit();
	input_grab_reset(&kp_grab_state);

	return;
}
