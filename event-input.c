/**
 * @file event-input.c
 * /dev/input event provider for the Mode Control Entity
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Ismo Laitinen <ismo.laitinen@nokia.com>
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
#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>		/* g_access() */
#include <glib-object.h>		/* g_signal_connect(),
					 * G_OBJECT(),
					 * G_CALLBACK()
					 */

#include <errno.h>			/* errno */
#include <fcntl.h>			/* open() */
#include <dirent.h>			/* opendir(), readdir(), telldir() */
#include <string.h>			/* strcmp() */
#include <unistd.h>			/* close() */
#include <sys/ioctl.h>			/* ioctl() */
#include <sys/types.h>			/* DIR */
#include <linux/input.h>		/* struct input_event,
					 * EVIOCGNAME, EVIOCGBIT, EVIOCGSW,
					 * EV_ABS, EV_KEY, EV_SW,
					 * ABS_PRESSURE,
					 * SW_CAMERA_LENS_COVER,
					 * SW_KEYPAD_SLIDE,
					 * SW_FRONT_PROXIMITY,
					 * KEY_SCREENLOCK,
					 * KEY_CAMERA_FOCUS,
					 * KEY_CAMERA
					 */
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

#include "mce.h"
#include "event-input.h"

#include "mce-io.h"			/* mce_read_string_from_file(),
					 * mce_write_string_to_file(),
					 * mce_suspend_io_monitor(),
					 * mce_resume_io_monitor(),
					 * mce_register_io_monitor_chunk(),
					 * mce_unregister_io_monitor(),
					 * mce_get_io_monitor_name(),
					 * mce_get_io_monitor_fd()
					 */
#include "mce-lib.h"			/* bitsize_of(),
					 * set_bit(), clear_bit(), test_bit(),
					 * bitfield_to_string(),
					 * string_to_bitfield()
					 */
#include "mce-log.h"			/* mce_log(), LL_* */
#include "mce-conf.h"			/* mce_conf_get_int(),
					 * mce_conf_get_string()
					 */
#include "datapipe.h"			/* execute_datapipe() */
#include "evdev.h"
#ifdef ENABLE_DOUBLETAP_EMULATION
# include "mce-gconf.h"
#endif

/** ID for touchscreen I/O monitor timeout source */
static guint touchscreen_io_monitor_timeout_cb_id = 0;

/** ID for keypress timeout source */
static guint keypress_repeat_timeout_cb_id = 0;

/** ID for misc timeout source */
static guint misc_io_monitor_timeout_cb_id = 0;

/** List of touchscreen input devices */
static GSList *touchscreen_dev_list = NULL;
/** List of keyboard input devices */
static GSList *keyboard_dev_list = NULL;
/** List of misc input devices */
static GSList *misc_dev_list = NULL;

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

/**
 * Wrapper function to call mce_unregister_io_monitor() from g_slist_foreach()
 *
 * @param io_monitor The I/O monitor to unregister
 * @param user_data Unused
 */
static void unregister_io_monitor(gpointer io_monitor, gpointer user_data)
{
	const gchar *filename = mce_get_io_monitor_name(io_monitor);

	/* If we opened an fd to monitor, retrieve it to ensure
	 * that we can close it after unregistering the I/O monitor
	 */
	int fd = mce_get_io_monitor_fd(io_monitor);

	(void)user_data;

	mce_unregister_io_monitor(io_monitor);

	/* Close the fd if there is one */
	if (fd != -1) {
		if (close(fd) == -1) {
			mce_log(LL_ERR,
				"Failed to close `%s'; %s",
				filename, g_strerror(errno));
			errno = 0;
		}

		fd = -1;
	}
}

/**
 * Timeout function for touchscreen I/O monitor reprogramming
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean touchscreen_io_monitor_timeout_cb(gpointer data)
{
	(void)data;

	touchscreen_io_monitor_timeout_cb_id = 0;

	/* Resume I/O monitors */
	if (touchscreen_dev_list != NULL) {
		g_slist_foreach(touchscreen_dev_list,
				(GFunc)resume_io_monitor, NULL);
	}

	return FALSE;
}

/**
 * Cancel timeout for touchscreen I/O monitor reprogramming
 */
static void cancel_touchscreen_io_monitor_timeout(void)
{
	if (touchscreen_io_monitor_timeout_cb_id != 0) {
		g_source_remove(touchscreen_io_monitor_timeout_cb_id);
		touchscreen_io_monitor_timeout_cb_id = 0;
	}
}

/**
 * Setup timeout for touchscreen I/O monitor reprogramming
 */
static void setup_touchscreen_io_monitor_timeout(void)
{
	cancel_touchscreen_io_monitor_timeout();

	/* Setup new timeout */
	touchscreen_io_monitor_timeout_cb_id =
		g_timeout_add_seconds(MONITORING_DELAY,
				      touchscreen_io_monitor_timeout_cb, NULL);
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
	static int      skip_syn = FALSE; // flag: skip SYN_REPORTS

	int result = FALSE; // assume: no doubletap

	unsigned i1, i2, i3; // 3 last positions

	switch( eve->type ) {
	case EV_REL:
		/* Ignore EV_SYN unless we see EV_KEY too */
		skip_syn = TRUE;
		/* Accumulate X/Y position */
		switch( eve->code ) {
		case REL_X: x_accum += eve->value; break;
		case REL_Y: y_accum += eve->value; break;
		default: break;
		}
		break;

	case EV_KEY:
		/* Store click/release and position */
		skip_syn = FALSE;
		if( eve->code == BTN_MOUSE ) {
			hist[i0].click += eve->value;
			hist[i0].x = x_accum;
			hist[i0].y = y_accum;
		}
		break;

	case EV_ABS:
		/* Do multitouch too while at it */
		skip_syn = FALSE;
		switch( eve->code ) {
		case ABS_MT_TOUCH_MAJOR: hist[i0].click += 1; break;
		case ABS_MT_POSITION_X:  hist[i0].x = eve->value; break;
		case ABS_MT_POSITION_Y:  hist[i0].y = eve->value; break;
		default: break;
		}
		break;

	case EV_SYN:
		if( eve->code != SYN_REPORT )
			break;

		/* Have we seen button events? */
		if( skip_syn ) {
			skip_syn = FALSE;
			break;
		}
		/* Set timestamp from syn event */
		hist[i0].time = eve->time;

		/* Last event before current */
		i1 = (i0 + 3) & 3;

		if( hist[i1].click != hist[i0].click ) {
			/* 2nd and 3rd last events before current */
			i2 = (i0 + 2) & 3;
			i3 = (i0 + 1) & 3;

			/* Release after click after release after click,
			 * within the time and distance limits */
			if( hist[i0].click == 0 && hist[i1].click == 1 &&
			    hist[i2].click == 0 && hist[i3].click == 1 &&
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
		/* Unexpected events -> nothing to do at EV_SYN */
		skip_syn = TRUE;
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
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
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

#ifdef ENABLE_DOUBLETAP_EMULATION
	if( fake_doubletap_enabled ) {
		switch( datapipe_get_gint(display_state_pipe) ) {
		case MCE_DISPLAY_OFF:
		case MCE_DISPLAY_LPM_OFF:
		case MCE_DISPLAY_LPM_ON:
			if( doubletap_emulate(ev) ) {
				mce_log(LL_NOTICE, "EMULATING DOUBLETAP");
				ev->type  = EV_MSC;
				ev->code  = MSC_GESTURE;
				ev->value = 0x4;
			}
			break;
		default:
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

	/* Generate activity */
	(void)execute_datapipe(&device_inactive_pipe, GINT_TO_POINTER(FALSE),
			       USE_INDATA, CACHE_INDATA);

	/* If the display is on/dim and visual tklock is active
	 * or autorelock isn't active, suspend I/O monitors
	 */
	if (((display_state == MCE_DISPLAY_ON) ||
	     (display_state == MCE_DISPLAY_DIM)) &&
	    (((submode & MCE_VISUAL_TKLOCK_SUBMODE) != 0) ||
	     ((submode & MCE_AUTORELOCK_SUBMODE) == 0))) {
		if (touchscreen_dev_list != NULL) {
			g_slist_foreach(touchscreen_dev_list,
					(GFunc)suspend_io_monitor, NULL);
		}

		/* Setup a timeout I/O monitor reprogramming */
		setup_touchscreen_io_monitor_timeout();

		flush = TRUE;
	}

	/* Only send pressure and gesture events */
	if (((ev->type != EV_ABS) || (ev->code != ABS_PRESSURE)) &&
	    ((ev->type != EV_KEY) || (ev->code != BTN_TOUCH)) &&
	    ((ev->type != EV_MSC) || (ev->code != MSC_GESTURE))) {
		goto EXIT;
	}

	/* If we get a double tap gesture, flush the remaining data */
	if ((ev->type == EV_MSC) &&
	    (ev->code == MSC_GESTURE) &&
	    (ev->value == 0x4)) {
		flush = TRUE;
	}

	/* For now there's no reason to cache the value
	 *
	 * If the event eater is active, don't send anything
	 */
	if ((submode & MCE_EVEATER_SUBMODE) == 0) {
		(void)execute_datapipe(&touchscreen_pipe, &ev,
				       USE_INDATA, DONT_CACHE_INDATA);
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

	/* Ignore non-keypress events */
	if ((ev->type != EV_KEY) && (ev->type != EV_SW)) {
		goto EXIT;
	}

	if (ev->type == EV_KEY) {
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
	if (misc_dev_list != NULL) {
		g_slist_foreach(misc_dev_list,
				(GFunc)resume_io_monitor, NULL);
	}

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
	if (misc_dev_list != NULL) {
		g_slist_foreach(misc_dev_list,
				(GFunc)suspend_io_monitor, NULL);
	}

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

	if (keyboard_dev_list != NULL) {
		g_slist_foreach(keyboard_dev_list,
				(GFunc)get_switch_state, NULL);
	}
}

/**
 * Custom compare function used to find I/O monitor entries
 *
 * @param iomon_id An I/O monitor cookie
 * @param name The name to search for
 * @return Less than, equal to, or greater than zero depending
 *         whether the name of the I/O monitor with the id iomon_id
 *         is less than, equal to, or greater than name
 */
static gint iomon_name_compare(gconstpointer iomon_id,
			       gconstpointer name)
{
	const gchar *iomon_name = mce_get_io_monitor_name(iomon_id);

	return strcmp(iomon_name, name);
}

/**
 * I/O monitor error callback for misc devices. Removes device
 * if suitable error condition.
 *
 * @param iomon An I/O monitor cookie
 * @param condition I/O condition
 */
static void misc_err_cb(gpointer iomon, GIOCondition condition)
{
	if (condition == G_IO_HUP) {
		mce_log(LL_DEBUG, "removing monitor for misc device %s", mce_get_io_monitor_name(iomon));
		misc_dev_list = g_slist_remove(misc_dev_list, iomon);
		mce_unregister_io_monitor(iomon);
	}
}

/**
 * Match and register I/O monitor
 */
static void match_and_register_io_monitor(const gchar *filename)
{
	gconstpointer iomon = NULL;
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
		iomon = mce_register_io_monitor_chunk(fd, filename, MCE_IO_ERROR_POLICY_WARN,
						      G_IO_IN | G_IO_ERR, FALSE, touchscreen_iomon_cb,
						      sizeof (struct input_event));
		if( iomon )
			touchscreen_dev_list = g_slist_prepend(touchscreen_dev_list, (gpointer)iomon);
		break;

	case EVDEV_INPUT:
		iomon = mce_register_io_monitor_chunk(fd, filename, MCE_IO_ERROR_POLICY_WARN,
						      G_IO_IN | G_IO_ERR, FALSE, keypress_iomon_cb,
						      sizeof (struct input_event));
		if( iomon )
			keyboard_dev_list = g_slist_prepend(keyboard_dev_list, (gpointer)iomon);
		break;

	case EVDEV_ACTIVITY:
		iomon = mce_register_io_monitor_chunk(fd, filename, MCE_IO_ERROR_POLICY_WARN,
						      G_IO_IN | G_IO_ERR, FALSE, misc_iomon_cb,
						      sizeof (struct input_event));
		if( iomon ) {
			mce_set_io_monitor_err_cb(iomon, misc_err_cb);
			misc_dev_list = g_slist_prepend(misc_dev_list, (gpointer)iomon);
		}
		break;
	}

EXIT:
	/* Close unmonitored file descriptors */
	if( !iomon && fd != -1 ) {
		if(close(fd) == -1) {
			mce_log(LL_ERR,
				"Failed to close `%s'; %s",
				filename, g_strerror(errno));
		}
	}
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
	gconstpointer iomon_id = NULL;
	GSList *list_entry = NULL;

	/* Try to find a matching touchscreen I/O monitor */
	list_entry = g_slist_find_custom(touchscreen_dev_list, device,
					 iomon_name_compare);

	/* If we find one, obtain the iomon ID,
	 * remove the entry and finally unregister the I/O monitor
	 */
	if (list_entry != NULL) {
		iomon_id = list_entry->data;
		touchscreen_dev_list = g_slist_remove(touchscreen_dev_list,
						      iomon_id);
		mce_unregister_io_monitor(iomon_id);
	}

	/* Try to find a matching keyboard I/O monitor */
	list_entry = g_slist_find_custom(keyboard_dev_list, device,
					 iomon_name_compare);

	/* If we find one, obtain the iomon ID,
	 * remove the entry and finally unregister the I/O monitor
	 */
	if (list_entry != NULL) {
		iomon_id = list_entry->data;
		keyboard_dev_list = g_slist_remove(keyboard_dev_list,
						   iomon_id);
		mce_unregister_io_monitor(iomon_id);
	}

	/* Try to find a matching touchscreen I/O monitor */
	list_entry = g_slist_find_custom(misc_dev_list, device,
					 iomon_name_compare);

	/* If we find one, obtain the iomon ID,
	 * remove the entry and finally unregister the I/O monitor
	 */
	if (list_entry != NULL) {
		iomon_id = list_entry->data;
		misc_dev_list = g_slist_remove(misc_dev_list,
					       iomon_id);
		mce_unregister_io_monitor(iomon_id);
	}

	if (add == TRUE)
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
	DIR *dir = NULL;
	struct dirent *direntry = NULL;
	gboolean status = FALSE;

	if ((dir = opendir(DEV_INPUT_PATH)) == NULL) {
		mce_log(LL_ERR, "opendir() failed; %s", g_strerror(errno));
		errno = 0;
		goto EXIT;
	}

	for (direntry = readdir(dir);
	     (direntry != NULL && telldir(dir));
	     direntry = readdir(dir)) {
		gchar *filename = NULL;

		if (strncmp(direntry->d_name, EVENT_FILE_PREFIX,
			    strlen(EVENT_FILE_PREFIX)) != 0) {
			mce_log(LL_DEBUG,
				"`%s/%s' skipped",
				DEV_INPUT_PATH,
				direntry->d_name);
			continue;
		}

		filename = g_strconcat(DEV_INPUT_PATH, "/",
				       direntry->d_name, NULL);
		match_and_register_io_monitor(filename);
		g_free(filename);
	}

	if ((direntry == NULL) && (errno != 0)) {
		mce_log(LL_ERR,
			"readdir() failed; %s",
			g_strerror(errno));
		errno = 0;
	}

	/* Report, but ignore, errors when closing directory */
	if (closedir(dir) == -1) {
		mce_log(LL_ERR,
			"closedir() failed; %s",
			g_strerror(errno));
		errno = 0;
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * Unregister monitors for input devices allocated by scan_inputdevices
 */
static void unregister_inputdevices(void)
{
	if (touchscreen_dev_list != NULL) {
		g_slist_foreach(touchscreen_dev_list,
				(GFunc)unregister_io_monitor, NULL);
		g_slist_free(touchscreen_dev_list);
		touchscreen_dev_list = NULL;
	}

	if (keyboard_dev_list != NULL) {
		g_slist_foreach(keyboard_dev_list,
				(GFunc)unregister_io_monitor, NULL);
		g_slist_free(keyboard_dev_list);
		keyboard_dev_list = NULL;
	}

	if (misc_dev_list != NULL) {
		g_slist_foreach(misc_dev_list,
				(GFunc)unregister_io_monitor, NULL);
		g_slist_free(misc_dev_list);
		misc_dev_list = NULL;
	}
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
	if( fake_doubletap_id ) {
		mce_gconf_notifier_remove(GINT_TO_POINTER(fake_doubletap_id), 0);
		fake_doubletap_id = 0;
	}
#endif

	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&submode_pipe,
					    submode_trigger);

	if (dev_input_gfmp != NULL) {
		g_signal_handler_disconnect(G_OBJECT(dev_input_gfmp),
					    dev_input_handler_id);
		dev_input_handler_id = 0;
		g_file_monitor_cancel(dev_input_gfmp);
	}

	unregister_inputdevices();

	/* Remove all timer sources */
	cancel_touchscreen_io_monitor_timeout();
	cancel_keypress_repeat_timeout();
	cancel_misc_io_monitor_timeout();

	return;
}
