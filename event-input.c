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

static time_t prev_handled_touchscreen_activity_seconds = 0;

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
 * I/O monitor callback for the touchscreen
 *
 * @param data The new data
 * @param bytes_read The number of bytes read
 * @return FALSE to return remaining chunks (if any),
 *         TRUE to flush all remaining chunks
 */
static gboolean touchscreen_iomon_cb(gpointer data, gsize bytes_read)
{
	submode_t submode = mce_get_submode_int32();
	struct input_event *ev;
	gboolean flush = FALSE;
	time_t time_now;

	ev = data;

	/* Don't process invalid reads */
	if (bytes_read != sizeof (*ev)) {
		goto EXIT;
	}

	/* Ignore unwanted events */
	if ((ev->type != EV_ABS) &&
	    (ev->type != EV_KEY) &&
	    (ev->type != EV_MSC)) {
		goto EXIT;
	}

	/* ignore all other tousch screen events except the first one happened at same second */
	if ((ev->time.tv_sec - prev_handled_touchscreen_activity_seconds) == 0) {
		goto EXIT;
	}
	prev_handled_touchscreen_activity_seconds	= ev->time.tv_sec;

	time(&time_now);
	if ((time_now - ev->time.tv_sec) > 2) {
		// ignore events that are more than 2 seconds old
		goto EXIT;
	}

	/* Generate activity */
	(void)execute_datapipe(&device_inactive_pipe, GINT_TO_POINTER(FALSE),
			       USE_INDATA, CACHE_INDATA);

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
 * Try to match /dev/input event file to a specific driver
 *
 * @param filename A string containing the name of the event file
 * @param drivers An array of driver names
 * @return An open file descriptor on success, -1 on failure
 */
static int match_event_file(const gchar *const filename,
			    const gchar *const *const drivers)
{
	static char name[256];
	int fd = -1;
	int i;

	/* If we cannot open the file, abort */
	if ((fd = open(filename, O_NONBLOCK | O_RDONLY)) == -1) {
		mce_log(LL_DEBUG,
			"Failed to open `%s', skipping",
			filename);

		/* Ignore error */
		errno = 0;
		goto EXIT;
	}

	for (i = 0; drivers[i] != NULL; i++) {
		if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0) {
			if (!strcmp(name, drivers[i])) {
				/* We found our event file */
				mce_log(LL_DEBUG,
					"`%s' is `%s'",
					filename, drivers[i]);
				break;
			}
		} else {
			mce_log(LL_WARN,
				"ioctl(EVIOCGNAME) failed on `%s'",
				filename);
		}
	}

	/* If the scan terminated with drivers[i] == NULL,
	 * we didn't find any match
	 */
	if (drivers[i] == NULL) {
		if (close(fd) == -1) {
			mce_log(LL_ERR,
				"Failed to close `%s'; %s",
				filename, g_strerror(errno));
			errno = 0;
		}

		fd = -1;
		goto EXIT;
	}

EXIT:
	return fd;
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
	int fd;

	if ((fd = match_event_file(filename,
				   driver_blacklist)) != -1) {
		/* If the driver for the event file is blacklisted, skip it */
		if (close(fd) == -1) {
			mce_log(LL_ERR,
				"Failed to close `%s'; %s",
				filename, g_strerror(errno));
			errno = 0;
		}

		fd = -1;
	} else if ((fd = match_event_file(filename,
					  touchscreen_event_drivers)) != -1) {
		gconstpointer iomon = NULL;

		iomon = mce_register_io_monitor_chunk(fd, filename, MCE_IO_ERROR_POLICY_WARN, G_IO_IN | G_IO_ERR, FALSE, touchscreen_iomon_cb, sizeof (struct input_event));

		/* If we fail to register an I/O monitor,
		 * don't leak the file descriptor,
		 * and don't add the device to the list
		 */
		if (iomon == NULL) {
			if (close(fd) == -1) {
				mce_log(LL_ERR,
					"Failed to close `%s'; %s",
					filename, g_strerror(errno));
				errno = 0;
			}
		} else {
			touchscreen_dev_list = g_slist_prepend(touchscreen_dev_list, (gpointer)iomon);
		}
	} else if ((fd = match_event_file(filename, keyboard_event_drivers)) != -1) {
		gconstpointer iomon = NULL;

		iomon = mce_register_io_monitor_chunk(fd, filename, MCE_IO_ERROR_POLICY_WARN, G_IO_IN | G_IO_ERR, FALSE, keypress_iomon_cb, sizeof (struct input_event));

		/* If we fail to register an I/O monitor,
		 * don't leak the file descriptor,
		 * and don't add the device to the list
		 */
		if (iomon == NULL) {
			if (close(fd) == -1) {
				mce_log(LL_ERR,
					"Failed to close `%s'; %s",
					filename, g_strerror(errno));
				errno = 0;
			}
		} else {
			keyboard_dev_list = g_slist_prepend(keyboard_dev_list, (gpointer)iomon);
		}
	} else {
		gconstpointer iomon = NULL;

		/* XXX: don't register a misc device unless it has
		 * EV_KEY, EV_REL, EV_ABS, EV_MSC or EV_SW events
		 */
		iomon = mce_register_io_monitor_chunk(-1, filename, MCE_IO_ERROR_POLICY_WARN, G_IO_IN | G_IO_ERR, FALSE, misc_iomon_cb, sizeof (struct input_event));

		/* If we fail to register an I/O monitor,
		 * don't add the device to the list
		 */
		if (iomon != NULL) {
			mce_set_io_monitor_err_cb(iomon, misc_err_cb);
			misc_dev_list = g_slist_prepend(misc_dev_list, (gpointer)iomon);
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
				     DEFAULT_HOME_LONG_DELAY,
				     NULL);

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
