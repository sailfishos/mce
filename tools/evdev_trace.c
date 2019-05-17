/**
 * @file evdev_trace.c
 * Mode Control Entity - cli utility for inspecting evdev input devices
 * <p>
 * Copyright (C) 2013-2019 Jolla Ltd.
 * <p>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * @author Mikko Harju <mikko.harju@jolla.com>
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

#include "../mce-log.h"
#include "../evdev.h"
#include "fileusers.h"

#include <linux/input.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <glob.h>
#include <getopt.h>

/** Flag for: emit event time stamps */
static bool emit_event_time  = true;

/** Flag for: emit time of day (of event read time) */
static bool emit_time_of_day = false;

/** Read and show input events
 *
 * @param fd   input device file descriptor to read from
 * @param title text to print before event details
 *
 * @return positive value on success, 0 on eof, -1 on errors
 */
static
int
process_events(int fd, const char *title)
{
  struct input_event eve[256];
  char tod[64], toe[64];

  errno = 0;
  int n = read(fd, eve, sizeof eve);
  if( n < 0 )
  {
    mce_log(LL_ERR, "%s: %m", title);
    return -1;
  }

  if( n == 0 )
  {
    mce_log(LL_ERR, "%s: EOF", title);
    return 0;
  }

  n /= sizeof *eve;

  *tod = 0;
  if( emit_time_of_day )
  {
    struct timeval tv;
    struct tm tm;
    memset(&tv, 0, sizeof tv);
    memset(&tm, 0, sizeof tm);

    /* Caveat emptor: time of day = event read time ... */
    gettimeofday(&tv, 0);
    localtime_r(&tv.tv_sec, &tm);

    snprintf(tod, sizeof tod, "%04d-%02d-%02d %02d:%02d:%02d.%03ld - ",
             tm.tm_year + 1900,
             tm.tm_mon + 1,
             tm.tm_mday,
             tm.tm_hour,
             tm.tm_min,
             tm.tm_sec,
             (long)(tv.tv_usec / 1000));
  }

  for( int i = 0; i < n; ++i )
  {
    struct input_event *e = &eve[i];

    *toe = 0;
    if( emit_event_time )
    {
      snprintf(toe, sizeof toe, "%ld.%03ld - ",
               (long)e->time.tv_sec,
               (long)e->time.tv_usec / 1000);
    }

    printf("%s: %s%s0x%02x/%s - 0x%03x/%s - %d\n",
           title, tod, toe,
           e->type,
           evdev_get_event_type_name(e->type),
           e->code,
           evdev_get_event_code_name(e->type, e->code),
           e->value);
  }
  return 1;
}

/** Mainloop for processing event input devices
 *
 * @param path  vector of input device paths
 * @param count number of paths in the path
 * @param identify if nonzero print input device information
 * @param trace stay in loop and print out events as they arrive
 */
static
void
mainloop(char **path, int count, int identify, int trace)
{
  struct pollfd pfd[count];

  int closed = 0;

  for( int i = 0; i < count; ++i )
  {
    if( (pfd[i].fd = evdev_open_device(path[i])) == -1 )
    {
      ++closed;
      continue;
    }

    if( identify )
    {
      printf("----====( %s )====----\n", path[i]);

      evdev_identify_device(pfd[i].fd);

      GSList *list = fileusers_get(path[i]);
      if( list ) {
        printf("Readers:\n");
        for( GSList *item = list; item; item = item->next )
        {
          fileuser_t *fu = item->data;
          printf("\t%s(pid=%d,fd=%d)\n", fu->cmd, fu->pid, fu->fd);
        }
        g_slist_free(list);
      }

      printf("\n");
    }
  }

  if( !trace )
  {
    goto cleanup;
  }

  while( closed < count )
  {
    for( int i = 0; i < count; ++i )
    {
      pfd[i].events = (pfd[i].fd < 0) ? 0 : POLLIN;
    }

    poll(pfd, count, -1);

    for( int i = 0; i < count; ++i )
    {
      if( pfd[i].revents )
      {
        if( process_events(pfd[i].fd, path[i]) <= 0 )
        {
          close(pfd[i].fd);
          pfd[i].fd = -1;
          ++closed;
        }
      }
    }
  }

cleanup:

  for( int i = 0; i < count; ++i )
  {
    if( pfd[i].fd != -1 ) close(pfd[i].fd);
  }
}

/** Configuration table for long command line options */
static struct option optL[] =
{
  { "help",          0, 0, 'h' },
  { "trace",         0, 0, 't' },
  { "identify",      0, 0, 'i' },
  { "show-readers",  0, 0, 'I' },
  { "emit-also-tod", 0, 0, 'e' },
  { "emit-only-tod", 0, 0, 'E' },
  { 0,0,0,0 }
};

/** Configuration string for short command line options */
static const char optS[] =
"h" // --help
"t" // --trace
"i" // --identify
"I" // --show-readers
"e" // --emit-also-tod
"E" // --emit-only-tod
;

/** Program name string */
static const char *progname = 0;

/** Compatibility with mce-log.h
 */
void
mce_log_file(loglevel_t loglevel,
             const char *const file,
             const char *const function,
             const char *const fmt, ...)
{
  const char *lev = "?";
  char       *msg = 0;
  va_list     va;

  (void)file, (void)function; // unused

  switch( loglevel )
  {
  case LL_CRIT:   lev = "C"; break;
  case LL_ERR:    lev = "E"; break;
  case LL_WARN:   lev = "W"; break;
  case LL_NOTICE: lev = "N"; break;
  case LL_INFO:   lev = "I"; break;
  case LL_DEBUG:  lev = "D"; break;
  default: break;
  }

  va_start(va, fmt);
  if( vasprintf(&msg, fmt, va) < 0 )
  {
    msg = 0;
  }
  va_end(va);

  fprintf(stderr, "%s: %s: %s\n", progname, lev, msg ?: "error");
  free(msg);
}

/** Stub for compatibility with mce-log.h
 */
int mce_log_p_(const loglevel_t loglevel,
               const char *const file,
               const char *const func)
{
  (void)loglevel;
  (void)file;
  (void)func;

  return true;
}
/** Provide runtime usage information
 */
static void usage(void)
{
  printf("USAGE\n"
         "  %s [options] [devicepath] ...\n"
         "\n"
         "OPTIONS\n"
         "  -h, --help           -- this help text\n"
         "  -i, --identify       -- identify input device\n"
         "  -t, --trace          -- trace input events\n"
         "  -e, --emit-also-tod  -- emit also time of day\n"
         "  -E, --emit-only-tod  -- emit only time of day\n"
         "  -I, --show-readers   -- identify processes using devices\n"
         "\n"
         "NOTES\n"
         "  If no device paths are given, /dev/input/event* is assumed.\n"
         "  \n"
         "  Full device path is not required, \"/dev/input/event1\" can\n"
         "  be shortened to \"event1\" or just \"1\".\n"
         "\n",
         progname);
}

/** Resolve device name given at command line to evdev path
 */

static char *get_device_path(const char *hint)
{
  char temp[256];

  // usable as is?
  if( access(hint, F_OK) == 0 )
  {
    goto cleanup;
  }

  // try couple of prefixes
  snprintf(temp, sizeof temp, "/dev/input/%s", hint);
  if( access(temp, F_OK) == 0 )
  {
    hint = temp;
    goto cleanup;
  }

  snprintf(temp, sizeof temp, "/dev/input/event%s", hint);
  if( access(temp, F_OK) == 0 )
  {
    hint = temp;
    goto cleanup;
  }

  // failure
  mce_log(LL_WARN, "%s: device file not found", hint);
  hint = 0;

cleanup:

  return hint ? strdup(hint) : 0;
}

/** Main entry point
 */
int
main(int argc, char **argv)
{
  int result = EXIT_FAILURE;

  int f_trace    = 0;
  int f_identify = 0;
  int f_readers  = 0;

  setlinebuf(stdout);

  glob_t gb;

  memset(&gb, 0, sizeof gb);

  progname = basename(*argv);

  for( ;; )
  {
    int opt = getopt_long(argc, argv, optS, optL, 0);

    if( opt < 0 )
    {
      break;
    }

    switch( opt )
    {
    case 'h':
      usage();
      exit(EXIT_SUCCESS);

    case 't':
      f_trace = 1;
      break;

    case 'i':
      f_identify = 1;
      break;

    case 'I':
      f_identify = f_readers = 1;
      break;

    case 'e':
      emit_time_of_day = true;
      break;

    case 'E':
      emit_time_of_day = true;
      emit_event_time  = false;
      break;

    case '?':
    case ':':
      goto cleanup;

    default:
      fprintf(stderr, "getopt() -> %d\n", opt);
      goto cleanup;
    }
  }

  if( !f_identify && !f_trace )
  {
    f_identify = 1;
  }

  if( f_readers )
  {
    fileusers_init();
  }

  if( optind < argc )
  {
    argc = 0;
    for( int i = optind; argv[i]; ++i )
    {
      char *path = get_device_path(argv[i]);
      if( path ) argv[argc++] = path;
    }
    mainloop(argv, argc, f_identify, f_trace);
    while( argc > 0 )
    {
      free(argv[--argc]);
    }
  }
  else
  {
    static const char pattern[] = "/dev/input/event*";

    if( glob(pattern, 0, 0, &gb) != 0 )
    {
      printf("%s: no matching files found\n", pattern);
      goto cleanup;
    }

    mainloop(gb.gl_pathv, gb.gl_pathc, f_identify, f_trace);
  }

  result = EXIT_SUCCESS;

cleanup:

  globfree(&gb);
  fileusers_quit();

  return result;
}
