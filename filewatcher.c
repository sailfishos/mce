/**
 * @file filewatcher.c
 * Mode Control Entity - flag file tracking
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

#include "filewatcher.h"

#include "mce-log.h"

#include <sys/inotify.h>

#include <unistd.h>
#include <string.h>
#include <errno.h>

#define DEBUG_INOTIFY_EVENTS 0

static inline void *lea(const void *base, int offs)
{
  return ((char *)base)+offs;
}

/* ------------------------------------------------------------------------- *
 * Inotify event debugging helpers
 * ------------------------------------------------------------------------- */

#if DEBUG_INOTIFY_EVENTS
/** Convert inotify event bitmask to human readable string
 *
 * @param mask event bit mask
 * @param buff where to construct the string
 * @param size how much space buff has (must be > 0)
 *
 * @return string with names of set bits separated with '+'
 */
static
const char *
inotify_mask_repr(uint32_t mask, char *buff, size_t size)
{
  static const struct
  {
    uint32_t    mask;
    const char *name;
  } lut[] =
  {
# define X(tag) { .mask = IN_##tag, .name = #tag },
    X(ACCESS)
    X(MODIFY)
    X(ATTRIB)
    X(CLOSE_WRITE)
    X(CLOSE_NOWRITE)
    X(OPEN)
    X(MOVED_FROM)
    X(MOVED_TO)
    X(CREATE)
    X(DELETE)
    X(DELETE_SELF)
    X(MOVE_SELF)
    X(UNMOUNT)
    X(Q_OVERFLOW)
    X(IGNORED)
    X(ONLYDIR)
    X(DONT_FOLLOW)
    X(EXCL_UNLINK)
    X(MASK_ADD)
    X(ISDIR)
    X(ONESHOT)
# undef X
    { .mask = 0, .name = 0 }
  };

  char *pos = buff;
  char *end = buff + size - 1;

  auto void adds(const char *s)
  {
    while( *s && pos < end) *pos++ = *s++;
  }

  for( size_t i = 0; lut[i].mask; ++i )
  {
    if( mask & lut[i].mask )
    {
      mask ^= lut[i].mask;
      if( pos > buff ) adds("+");
      adds(lut[i].name);
    }
  }
  if( mask )
  {
    char hex[32];
    snprintf(hex, sizeof hex, "0x%"PRIx32, mask);
    if( pos > buff ) adds("+");
    adds(hex);
  }

  return *pos = 0, buff;
}

/** Emit inotify event details
 *
 * @param eve inotify_event pointer
 */
static
void
inotify_event_debug(const struct inotify_event *eve)
{
  char temp[256];
  printf("wd=%d\n", eve->wd);
  printf("mask=%s\n", inotify_mask_repr(eve->mask, temp, sizeof temp));
  if( eve->len )
  {
    printf("name=\"%s\"\n", eve->name);
  }
  printf("\n");
}
#endif /* DEBUG_INOTIFY_EVENTS */

/* ------------------------------------------------------------------------- *
 * File content change tracking
 * ------------------------------------------------------------------------- */

/** Object for tracking file content in a directory */
struct filewatcher_t
{
  /** inotify file descriptor */
  int inotify_fd;

  /** inotify watch descriptor */
  int inotify_wd;

  /** glib input watch for inotify_fd */
  guint watch_id;

  /** the directory to watch over */
  char *watch_path;

  /** the file in the watch_path to track */
  char *watch_file;

  /** function to call when watch_path/watch_file changes */
  filewatcher_changed_fn changed_cb;

  /** user data to pass to changed_cb */
  gpointer user_data;

  /** how to delete user_data when filewatcher_t is deleted */
  GDestroyNotify delete_cb;
};

/* Initialize filewatcher_t object to a sane state
 *
 * @param self pointer to uninitialized filewatcher_t object
 */
static
void
filewatcher_ctor(filewatcher_t *self)
{
  self->inotify_fd = -1;
  self->inotify_wd = -1;
  self->watch_path = 0;
  self->watch_file = 0;

  self->watch_id   = 0;

  self->changed_cb = 0;

  self->delete_cb  = 0;
  self->user_data  = 0;
}

/* Release all dynamic data from filewatcher_t object
 *
 * @param self pointer to initialized filewatcher_t object
 */
static
void
filewatcher_dtor(filewatcher_t *self)
{
  /* detach user data */
  if( self->delete_cb )
  {
    self->delete_cb(self->user_data);
  }
  self->user_data = 0;

  /* detach glib io watch */
  if( self->watch_id )
  {
    g_source_remove(self->watch_id), self->watch_id = 0;
  }

  /* detach inotify fd */
  if( self->inotify_fd != -1 )
  {
    if( self->inotify_wd != -1 )
    {
      if( inotify_rm_watch(self->inotify_fd, self->inotify_wd) == -1 )
      {
        mce_log(LL_WARN, "inotify_rm_watch: %m");
      }
      self->inotify_wd = -1;
    }
    if( close(self->inotify_fd) == -1 )
    {
      mce_log(LL_WARN, "close inotify fd: %m");
    }
    self->inotify_fd = -1;
  }

  /* release strings */
  g_free(self->watch_path), self->watch_path = 0;
  g_free(self->watch_file), self->watch_file = 0;
}

/* Delete a filewatcher_t object
 *
 * @param self pointer to initialized filewatcher_t object, or NULL
 */
void
filewatcher_delete(filewatcher_t *self)
{
  if( self != 0 )
  {
    filewatcher_dtor(self);
    g_free(self);
  }
}

/** Process inotify events
 *
 * @param self pointer to filewatcher_t object
 *
 * @return TRUE on success, or FALSE if further processing is not possible
 */
static
gboolean
filewatcher_process_events(filewatcher_t *self)
{
  gboolean res = FALSE;
  gboolean flg = FALSE;

  char buf[2048];
  int todo, size;
  struct inotify_event *eve;

  if( !self || self->inotify_fd == -1 )
  {
    goto cleanup;
  }

  todo = read(self->inotify_fd, buf, sizeof buf);

  if( todo < 0 )
  {
    switch( errno )
    {
    case EAGAIN:
    case EINTR:
      res = TRUE;
      break;

    default:
      mce_log(LL_WARN, "read inotify events: %m");
      break;
    }
    goto cleanup;
  }

  if( todo == 0 )
  {
    mce_log(LL_WARN, "read inotify events: EOF");
    goto cleanup;
  }

#if DEBUG_INOTIFY_EVENTS
  printf("----\n");
#endif

  for( eve = lea(buf, 0); todo; todo -= size, eve = lea(eve, size))
  {
    if( todo < (int)sizeof *eve )
    {
      mce_log(LL_WARN, "partial inotify event received");
      goto cleanup;
    }

    size = sizeof *eve + eve->len;

    if( todo < size )
    {
      mce_log(LL_WARN, "oversized inotify event received");
      goto cleanup;
    }

#if DEBUG_INOTIFY_EVENTS
    inotify_event_debug(eve);
#endif

    if( eve->len && !strcmp(self->watch_file, eve->name) )
    {
      flg = TRUE;
    }

    if( eve->mask & IN_IGNORED )
    {
      mce_log(LL_ERR, "inotify watch went defunct");
      flg = TRUE;
      goto cleanup;
    }
  }

  res = TRUE;

cleanup:

  if( flg && self && self->changed_cb )
  {
    self->changed_cb(self->watch_path, self->watch_file, self->user_data);
  }

  return res;
}

/** Glib io glue for processing inotify event input
 *
 * @param source (not used)
 * @param condition (not used)
 * @param data pointer to filewatcher_t object (as void pointer)
 *
 * @return TRUE to keep the io watch alive, or
 *         FALSE if the io watch must be released
 */
static
gboolean
filewatcher_input_cb(GIOChannel *source,
                     GIOCondition condition,
                     gpointer data)
{
  (void)source;

  filewatcher_t *self = data;
  gboolean keep_going = TRUE;

  if( condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) )
  {
    keep_going = FALSE;
  }

  if( !filewatcher_process_events(self) )
  {
    keep_going = FALSE;
  }

  if( !keep_going )
  {
    /* Note: This /should/ never happen, but if it does
     *       we must not leave the io watch in a state
     *       where it gets triggered forever. */
    mce_log(LL_CRIT, "stopping inotify event io watch");
    self->watch_id = 0;
  }

  return keep_going;
}

/** Helper for setting up inotify file descriptor
 *
 * @note This function is meant to be called form
 *       filewatcher_create() function only!
 *
 * @param self pointer to filewatcher_t object
 *
 * @return TRUE on success, or FALSE on failure
 */
static
gboolean
filewatcher_setup_inotify(filewatcher_t *self)
{
  gboolean success = FALSE;

  uint32_t mask = (0
                   | IN_CREATE
                   | IN_DELETE
                   | IN_CLOSE_WRITE
                   | IN_MOVED_TO
                   | IN_MOVED_FROM
                   | IN_DONT_FOLLOW
                   | IN_ONLYDIR);

  self->inotify_fd = inotify_init1(IN_CLOEXEC);
  if( self->inotify_fd == -1 )
  {
    mce_log(LL_WARN, "inotify_init: %m");
    goto cleanup;
  }

  self->inotify_wd = inotify_add_watch(self->inotify_fd,
                                       self->watch_path,  mask);
  if( self->inotify_wd == -1 )
  {
    mce_log(LL_WARN, "%s: inotify_add_watch: %m", self->watch_path);
    goto cleanup;
  }

  success = TRUE;

cleanup:
  return success;
}

/** Helper for setting up glib io watch for inotify file descriptor
 *
 * @note This function is meant to be called form
 *       filewatcher_create() function only!
 *
 * @param self pointer to filewatcher_t object
 *
 * @return TRUE on success, or FALSE on failure
 */
static
gboolean
filewatcher_setup_iowatch(filewatcher_t *self)
{
  gboolean success = FALSE;

  GIOChannel *chan  = 0;
  GError     *err   = 0;

  if( !(chan = g_io_channel_unix_new(self->inotify_fd)) )
  {
    mce_log(LL_WARN, "%s: %m", "g_io_channel_unix_new");
    goto cleanup;
  }

  /* the channel does not own the fd  */
  g_io_channel_set_close_on_unref(chan, FALSE);

  /* Set to NULL encoding so that we can turn off the buffering */
  if( g_io_channel_set_encoding(chan, NULL, &err) != G_IO_STATUS_NORMAL )
  {
    mce_log(LL_WARN, "%s: %s", "g_io_channel_set_encoding",
            (err && err->message) ? err->message : "unknown");
  }
  g_io_channel_set_buffered(chan, FALSE);

  self->watch_id = g_io_add_watch(chan,
                                  G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                                  filewatcher_input_cb, self);

  if( !self->watch_id )
  {
    mce_log(LL_WARN, "%s: %m", "g_io_add_watch");
    goto cleanup;
  }

  success = TRUE;

cleanup:

  g_clear_error(&err);

  if( chan ) g_io_channel_unref(chan);

  return success;
}

/** Create an filewatcher_t object
 *
 * An inotify watcher is started for the given director/file.
 * A glib io watch is used to process the inotify events.
 * The change_cb is called when contents of the tracked file
 * are assumed to have changed.
 *
 * @note The change_cb function will not be called during the
 *       initialization. You can make initial state evaluation
 *       to happen by calling filewatcher_force_trigger() after
 *       succesfull filewatcher_create().
 *
 * @param dirpath directory to watch over
 * @param filename file to watch in dirpath
 * @param change_cb function to call when dirpath/filename changes
 * @param user_data extra parameter to pass to change_cb
 * @param delete_cb called on user_data when filewatcher_t itself is deleted
 *
 * @return pointer to filewatcher_t object, or NULL in case of errors
 */
filewatcher_t *
filewatcher_create(const char *dirpath,
                   const char *filename,
                   filewatcher_changed_fn change_cb,
                   gpointer user_data,
                   GDestroyNotify delete_cb)
{
  gboolean success = FALSE;

  filewatcher_t *self = g_malloc0(sizeof *self);
  filewatcher_ctor(self);

  self->watch_path = g_strdup(dirpath);
  self->watch_file = g_strdup(filename);

  self->changed_cb = change_cb;

  self->user_data  = user_data;
  self->delete_cb  = delete_cb;

  if( !filewatcher_setup_inotify(self) )
  {
    goto cleanup;
  }
  if( !filewatcher_setup_iowatch(self) )
  {
    goto cleanup;
  }

  success = TRUE;

cleanup:

  if( !success )
  {
    filewatcher_delete(self), self = 0;
  }

  return self;
}

/** Force calling the change notification callback
 *
 * This can be useful for example to feed initial
 * state of tracked file via the same mechanism as
 * the later changes get reported
 *
 * @param self pointer to filewatcher_t object
 */
void
filewatcher_force_trigger(filewatcher_t *self)
{
  if( self->changed_cb )
  {
    self->changed_cb(self->watch_path, self->watch_file, self->user_data);
  }
}
