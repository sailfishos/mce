/**
 * @file mce-hybris.c
 * Mode Control Entity - android hal access
 * <p>
 * Copyright (C) 2013-2019 Jolla Ltd.
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

/* ========================================================================= *
 * Most of the functions in this module are just thunks that load and call
 * the real functionality from hybris-plugin on demand. If the hybris plugin
 * is not installed or underlying android code does not support some hw
 * elements these functions turn in to "NOP and return failure".
 *
 * In addition to the above this module also:
 * - moves sensor input data via pipe from worker thread context to the
 *   thread that is running the glib mainloop.
 * - proxies diagnostic output from hybris-plugin to mce_log()
 * ========================================================================= */

#define MCE_HYBRIS_INTERNAL 1
#include "mce-hybris.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-conf.h"
#include "mce-modules.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>

static void mce_hybris_ps_set_hook(mce_hybris_ps_fn cb);
static void mce_hybris_als_set_hook(mce_hybris_als_fn cb);

/* ------------------------------------------------------------------------- *
 * Feeding sensor data via pipe to glib mainloop goes roughly as follows
 *
 * --- mce-libhybris-plugin worker thread --
 * 1) uses blocking poll_dev->poll() function to read sensor data
 * 2) uses a set of callbacks to write the data to a pipe
 * --- mce-libhybris-module --
 * 3) iowatch reads the data from pipe
 * 4) and passes the data to mce via another set of callbacks
 * --- mce sensor handling code --
 * 5) can act on the data in the context that runs gmainloop
 * ------------------------------------------------------------------------- */

/** Sensor enumeration for mux @ worker thread -> pipe -> demux @ mainloop */
enum
{
  EVEPIPE_ALS,
  EVEPIPE_PS,
};

/** Sensor data passed over pipe */
typedef struct
{
  int64_t time;  // time stamp from android side
  int32_t type;  // EVEPIPE_ALS or EVEPIPE_PS
  float   value; // sensor data from android side
} evepipe_t;

/** Initialize once flag for sensor data pipe */
static bool evepipe_done = false;

/** Callback for handling proximity data */
static mce_hybris_ps_fn  evepipe_ps_cb  = 0;

/** Callback for handling ambient light data */
static mce_hybris_als_fn evepipe_als_cb = 0;

/** The sensor data pipe */
static int               evepipe_fd[2]  = { -1, -1 };

/** I/O watch id for the sensor data pipe */
static guint             evepipe_id     = 0;

/** I/O watch callback for handling pipe input
 *
 * @param channel    (not used)
 * @param condition  (not used)
 * @param data       (not used)
 *
 * @return TRUE to keep the iowatch alive, or FALSE to remove it
 */
static gboolean evepipe_recv_cb(GIOChannel *channel,
                                GIOCondition condition,
                                gpointer data)
{
  /* we just want the cb ... */
  (void)channel; (void)condition; (void)data;

  gboolean keep_going = TRUE;

  evepipe_t eve[64];

  if( condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) )
  {
    keep_going = FALSE;
  }

  int rc = read(evepipe_fd[0], eve, sizeof eve);

  if( rc < 0 ) {
    switch( errno ) {
    case EINTR:
    case EAGAIN:
      break;

    default:
      mce_log(LL_ERR, "failed to read sensor events: %m");
      keep_going = FALSE;
      break;
    }
    goto cleanup;
  }

  rc /= sizeof *eve;

  for( int i = 0; i < rc; ++i ) {
    switch( eve[i].type ) {
    case EVEPIPE_PS:
      if( evepipe_ps_cb ) {
        evepipe_ps_cb(eve[i].time, eve[i].value);
      }
      break;

    case EVEPIPE_ALS:
      if( evepipe_als_cb ) {
        evepipe_als_cb(eve[i].time, eve[i].value);
      }
      break;

    default:
      break;
    }
  }

cleanup:

  if( !keep_going )  {
    mce_log(LL_CRIT, "disabling sensor event pipe iowatch");
    evepipe_id = 0;
  }

  return keep_going;
}

/** Write sensor data to the pipe
 *
 * @param timestamp nanoseconds
 * @param type      EVEPIPE_ALS or EVEPIPE_PS
 * @param data      sensor data
 */
static void evepipe_send(int64_t timestamp, int32_t type, float data)
{
  evepipe_t eve =
  {
    .time  = timestamp,
    .type  = type,
    .value = data,
  };

  int rc = TEMP_FAILURE_RETRY(write(evepipe_fd[1], &eve, sizeof eve));

  if( rc != sizeof eve ) {
    // TODO: since this happens from separate thread, we might want
    //       to do something bit more clever in case the sensor data
    //       overflows the pipe ...
    mce_abort();
  }
}

/** Write PS data to the sensor data pipe
 *
 * @param timestamp nanoseconds
 * @param distance  centimeters
 */
static void evepipe_send_ps(int64_t timestamp, float distance)
{
  evepipe_send(timestamp, EVEPIPE_PS, distance);
}

/** Write ALS data to the sensor data pipe
 * @param timestamp nanoseconds
 * @param ligt      lux
 */
static void evepipe_send_als(int64_t timestamp, float light)
{
  evepipe_send(timestamp, EVEPIPE_ALS, light);
}

/** Close sensor data pipe
 *
 * @param reset_done true if we wish to return to uninitialized
 *                   state, or false to preserve "already tried
 *                   but failed" state
 */
static void evepipe_quit(bool reset_done)
{
  /* remove io watch */
  if( evepipe_id ) g_source_remove(evepipe_id), evepipe_id = 0;

  /* close pipe file descriptors */
  if( evepipe_fd[1] != -1 ) close(evepipe_fd[1]), evepipe_fd[1] = -1;
  if( evepipe_fd[0] != -1 ) close(evepipe_fd[0]), evepipe_fd[0] = -1;

  if( reset_done ) evepipe_done = false;
}

/** Initialize sensor data pipe
 *
 * @return true on success, or false in case of errors
 */
static bool evepipe_init(void)
{
  GIOChannel *chn = 0;

  if( evepipe_done ) {
    goto EXIT;
  }

  evepipe_done = true;

  if( pipe(evepipe_fd) == -1 ) {
    goto EXIT;
  }

  if( !(chn = g_io_channel_unix_new(evepipe_fd[0])) ) {
    goto EXIT;
  }

  evepipe_id = g_io_add_watch(chn,
                              G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                              evepipe_recv_cb, 0);

  if( !evepipe_id ) {
    goto EXIT;
  }

EXIT:

  if( chn != 0 ) g_io_channel_unref(chn);

  if( !evepipe_id ) {
    evepipe_quit(false);
  }

  return evepipe_id != 0;
}

/** Callback for forwarding logging from hybris-plugin to mce_log()
 *
 * @param lev  syslog priority (=mce_log level) i.e. LOG_ERR etc
 * @param file source code path
 * @param func name of function within file
 * @param text diagnostic message to output
 */
static void log_cb(int lev, const char *file, const char *func,
                   const char *text)
{
  mce_log_file(lev, file, func, "%s", text);
}

/** INTERNAL Set up hybris-plugin -> mce_log() proxy
 *
 * @param base handle for hybris-plugin
 */
static void mce_hybris_set_logging_proxy(void *base)
{
  static const char name[] = "mce_hybris_set_log_hook";
  void (*func)(mce_hybris_log_fn cb)  = 0;

  if( (func = dlsym(base, name)) ) {
    func(log_cb);
  }
}

/** INTERNAL Lookup path to hybris plugin DSO
 *
 * @return path to DSO, or NULL in case of errors
 */
static char *mce_hybris_module_path(void)
{
  static const char module_name[] = "hybris.so";
  gchar *module_dir = 0;
  char *module_path = 0;

  module_dir = mce_conf_get_string(MCE_CONF_MODULES_GROUP,
                                   MCE_CONF_MODULES_PATH,
                                   DEFAULT_MCE_MODULE_PATH);
  if( !module_dir ) {
    goto EXIT;
  }

  if( asprintf(&module_path, "%s/%s", module_dir, module_name) < 0 ) {
    module_path = 0;
  }

EXIT:
  g_free(module_dir);

  return module_path;
}

/** Lookup function address from hybris plugin
 *
 * @name function name
 *
 * @return function address, or NULL in case of errors
 */
static void *mce_hybris_lookup_function(const char *name)
{
  static void *base   = 0;
  static bool  done   = false;

  void *addr = 0;

  if( !done ) {
    char *path = 0;

    done = true;

    if( !(path = mce_hybris_module_path()) ) {
      mce_log(LL_WARN, "could not locate hybris plugin");
    }
    else if( access(path, F_OK) == -1 && errno == ENOENT ) {
      mce_log(LL_NOTICE, "%s: not installed", path);
    }
    else if( !(base = dlopen(path, RTLD_NOW|RTLD_LOCAL|RTLD_DEEPBIND)) ) {
      mce_log(LL_WARN, "%s: failed to load: %s", path, dlerror());
    }
    else {
      mce_log(LL_NOTICE, "loaded hybris plugin");
      mce_hybris_set_logging_proxy(base);
    }
    free(path);
  }

  if( base ) {
    if( !(addr = dlsym(base, name)) ) {
      mce_log(LL_ERR, "%s: failed to lookup: %s", name, dlerror());
    }
  }

  return addr;
}

/** Glue macro to perform function address lookup once
 *
 * Assumes a local function pointer variable 'real' exists,
 * and the local function name is the same as the function
 * we want to lookup from the plugin.so
 */
#define RESOLVE do {\
  static bool done = false; \
  if( !done ) { \
    done = true;\
    real = mce_hybris_lookup_function(__FUNCTION__);\
  }\
} while(0);

/* Thunk functions that will either call the real functionality
 * from the hybris plugin, or fall back to NOP with appropriate
 * return value to signal failure.
 */

/** Release all resources allocated by this module */
void mce_hybris_quit(void)
{
  static void (*real)(void) = 0;
  RESOLVE;

  evepipe_quit(true);

  if( real ) real();
}

/* ------------------------------------------------------------------------- *
 * framebuffer device
 * ------------------------------------------------------------------------- */

/** Start using libhybris for frame buffer power control
 *
 * @return true if functionality supported, or false if not
 */
bool mce_hybris_framebuffer_init(void)
{
  static bool (*real)(void) = 0;
  RESOLVE;
  return !real ? false : real();
}

/** Stop using libhybris for frame buffer power control
 */
void mce_hybris_framebuffer_quit(void)
{
  static void (*real)(void) = 0;
  RESOLVE;
  if( real ) real();
}

/** Turn frame buffer power on/off via libhybris
 *
 * @param state true for power on, false for power off
 *
 * @return true on success, or false on failure
 */
bool mce_hybris_framebuffer_set_power(bool state)
{
  static bool (*real)(bool) = 0;
  RESOLVE;
  return !real ? false : real(state);
}

/* ------------------------------------------------------------------------- *
 * display backlight device
 * ------------------------------------------------------------------------- */

/** Start using libhybris for display backlight brightness control
 *
 * @return true if functionality supported, or false if not
 */
bool mce_hybris_backlight_init(void)
{
  static bool (*real)(void) = 0;
  RESOLVE;
  return !real ? false : real();
}

/** Stop using libhybris for display backlight brightness control
 */
void mce_hybris_backlight_quit(void)
{
  static void (*real)(void) = 0;
  RESOLVE;
  if( real ) real();
}

/** Set display backlight brightness via libhybris
 *
 * @param level 0 for off, ..., 255 for maximum brightness
 *
 * @return true on success, or false on failure
 */
bool mce_hybris_backlight_set_brightness(int level)
{
  static bool (*real)(int) = 0;
  RESOLVE;
  return !real ? false : real(level);
}

/* ------------------------------------------------------------------------- *
 * keypad backlight device
 * ------------------------------------------------------------------------- */

/** Start using libhybris for keypad backlight brightness control
 *
 * @return true if functionality supported, or false if not
 */
bool mce_hybris_keypad_init(void)
{
  static bool (*real)(void) = 0;
  RESOLVE;
  return !real ? false : real();
}

/** Stop using libhybris for keypad backlight brightness control
 */
void mce_hybris_keypad_quit(void)
{
  static void (*real)(void) = 0;
  RESOLVE;
  if( real ) real();
}

/** Set keypad backlight brightness via libhybris
 *
 * @param level 0 for off, ..., 255 for maximum brightness
 *
 * @return true on success, or false on failure
 */
bool mce_hybris_keypad_set_brightness(int level)
{
  static bool (*real)(int) = 0;
  RESOLVE;
  return !real ? false : real(level);
}

/* ------------------------------------------------------------------------- *
 * indicator led device
 * ------------------------------------------------------------------------- */

/** Start using libhybris for indicator led control
 *
 * @return true if functionality supported, or false if not
 */
bool mce_hybris_indicator_init(void)
{
  static bool (*real)(void) = 0;
  RESOLVE;
  return !real ? false : real();
}

/** Stop using libhybris for indicator led control
 */
void mce_hybris_indicator_quit(void)
{
  static void (*real)(void) = 0;
  RESOLVE;
  if( real ) real();
}

/** Set indicator led pattern via libhybris
 *
 * @param r     red intensity 0 ... 255
 * @param g     green intensity 0 ... 255
 * @param b     blue intensity 0 ... 255
 * @param ms_on milliseconds to keep the led on, or 0 for no flashing
 * @param ms_on milliseconds to keep the led off, or 0 for no flashing
 *
 * @return true on success, or false on failure
 */
bool mce_hybris_indicator_set_pattern(int r, int g, int b, int ms_on, int ms_off)
{
  static bool (*real)(int,int,int,int,int) = 0;
  RESOLVE;
  return !real ? false : real(r,g,b, ms_on, ms_off);
}

/** Query if currently active led backend can support breathing
 *
 * @return true if breathing can be requested, false otherwise
 */
bool
mce_hybris_indicator_can_breathe(void)
{
  static bool (*real)(void) = 0;
  RESOLVE;

  /* If the plugin does not have this method, err on the safe side
   * and assume that breathing is not ok */

  return !real ? false : real();
}

/** Enable/disable timer based led breathing
 *
 * @param enable true for smooth sw transitions, false for hw blinking only
 */
void mce_hybris_indicator_enable_breathing(bool enable)
{
  static void (*real)(bool) = 0;
  RESOLVE;
  if( real ) real(enable);
}

/** Set indicator led brightness
 *
 * @param level 1=minimum, 255=maximum
 *
 * @return true on success, or false on failure
 */
bool mce_hybris_indicator_set_brightness(int level)
{
  static bool (*real)(int) = 0;
  RESOLVE;
  return !real ? false : real(level);
}

/* ------------------------------------------------------------------------- *
 * proximity sensor
 * ------------------------------------------------------------------------- */

/** Start using libhybris for proximity sensor input
 *
 * @return true if functionality supported, or false if not
 */
bool mce_hybris_ps_init(void)
{
  bool (*real)(void) = 0;
  RESOLVE;
  return !real ? false : real();
}

/** Stop using libhybris for proximity sensor input
 */
void mce_hybris_ps_quit(void)
{
  static void (*real)(void) = 0;
  RESOLVE;
  evepipe_ps_cb = 0;
  if( real ) real();
}

/** Enable/disable proximity sensor events via libhybris
 *
 * @param state true for enabling events, false for disabling
 *
 * @return true on success, or false on failure
 */
bool mce_hybris_ps_set_active(bool state)
{
  static bool (*real)(bool) = 0;
  RESOLVE;
  return !real ? false : real(state);
}

/** INTERNAL Set hybris-plugin -> hybris-module PS event callback
 *
 * Note: the callback will be called from worker thread context
 *
 * @param cb callback plugin should use to send events to module side
 */
static void mce_hybris_ps_set_hook(mce_hybris_ps_fn cb)
{
  static void (*real)(mce_hybris_ps_fn) = 0;
  RESOLVE;
  if( real ) real(cb);
}

/** Set proximity sensor event reporting callback
 *
 * Note: the callback will be called from the same context where
 *       glib mainloop is running
 *
 * @param cb callback plugin should use to send events to application code
 *
 * @return true on success, or false on failure
 */
bool mce_hybris_ps_set_callback(mce_hybris_ps_fn cb)
{
  bool res = true;

  if( (evepipe_ps_cb = cb) ) {
    mce_hybris_ps_set_hook(evepipe_send_ps);
    res = evepipe_init();
  }
  else {
    mce_hybris_ps_set_hook(0);
  }

  return res;
}

/* ------------------------------------------------------------------------- *
 * ambient light sensor
 * ------------------------------------------------------------------------- */

/** Start using libhybris for ambient light sensor input
 *
 * @return true if functionality supported, or false if not
 */
bool mce_hybris_als_init(void)
{
  bool (*real)(void) = 0;
  RESOLVE;
  return !real ? false : real();
}

/** Stop using libhybris for ambient light sensor input
 */
void mce_hybris_als_quit(void)
{
  static void (*real)(void) = 0;
  RESOLVE;
  evepipe_als_cb = 0;
  if( real ) real();
}

/** Enable/disable ambient light sensor events via libhybris
 *
 * @param state true for enabling events, false for disabling
 *
 * @return true on success, or false on failure
 */
bool mce_hybris_als_set_active(bool state)
{
  static bool (*real)(bool) = 0;
  RESOLVE;
  return !real ? false : real(state);
}

/** INTERNAL Set hybris-plugin -> hybris-module PS event callback
 *
 * Note: the callback will be called from worker thread context
 *
 * @param cb callback plugin should use to send events to module side
 */
static void mce_hybris_als_set_hook(mce_hybris_als_fn cb)
{
  static void (*real)(mce_hybris_als_fn) = 0;
  RESOLVE;
  if( real ) real(cb);
}

/** Set ambient light sensor event reporting callback
 *
 * Note: the callback will be called from the same context where
 *       glib mainloop is running
 *
 * @param cb callback plugin should use to send events to application code
 *
 * @return true on success, or false on failure
 */
bool mce_hybris_als_set_callback(mce_hybris_als_fn cb)
{
  bool res = true;

  if( (evepipe_als_cb = cb) ) {
    mce_hybris_als_set_hook(evepipe_send_als);
    res = evepipe_init();
  }
  else {
    mce_hybris_als_set_hook(0);
  }

  return res;
}
