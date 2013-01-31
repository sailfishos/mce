/* ------------------------------------------------------------------------- *
 * Copyright (C) 2012-2013 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: GPLv2
 * ------------------------------------------------------------------------- */

#include "../evdev.h"
#include "../mce-log.h"
#include <linux/input.h>

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <glob.h>
#include <getopt.h>

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

  for( int i = 0; i < n; ++i )
  {
    struct input_event *e = &eve[i];

    printf("%s: %ld.%03ld - 0x%02x/%s - 0x%03x/%s - %d\n",
           title,
           (long)e->time.tv_sec,
           (long)e->time.tv_usec / 1000,
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
  { "help",     0, 0, 'h' },
  { "trace",    0, 0, 'i' },
  { "identify", 0, 0, 't' },
  { 0,0,0,0 }
};

/** Configuration string for short command line options */
static const char optS[] =
"h" // --help
"t" // --trace
"i" // --identify
;

/** Program name string */
static const char *progname = 0;

/** Compatibility with mce-log.h */
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

  fprintf(stderr, "%s: %s: %s", progname, lev, msg ?: "error");
  free(msg);
}

/** Provide runtime usage information
 */
static void usage(void)
{
  printf("USAGE\n"
         "  %s [options] [devicepath] ...\n"
         "\n"
         "OPTIONS\n"
         "  -h, --help      -- this help text\n"
         "  -i, --identify  -- identify input device\n"
         "  -t, --trace     -- trace input events\n"
         "\n",
         progname);
}

/** Main entry point */
int
main(int argc, char **argv)
{
  int result = EXIT_FAILURE;

  int f_trace    = 0;
  int f_identify = 0;

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

  if( optind < argc )
  {
    mainloop(argv+optind, argc-optind, f_identify, f_trace);
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

  return result;
}
