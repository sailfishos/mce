/**
 * @file fileusers.c
 *
 * Mode Control Entity - Get users of evdev input nodes
 *
 * <p>
 *
 * Copyright (C) 2015 Jolla Ltd.
 *
 * <p>
 *
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

#include "fileusers.h"
#include "../mce-log.h"

#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>

/* ========================================================================= *
 * TYPES & FUNCTIONS
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * GENERIC_UTILS
 * ------------------------------------------------------------------------- */

static void       *fu_malloc           (size_t size);
static void       *fu_calloc           (size_t nmemb, size_t size);
static char       *fu_strdup           (const char *str);

static bool        fu_ascii_digit_p    (int ch);
static char       *fu_read_content     (const char *path);
static char       *fu_get_command_name (int pid);

/* ------------------------------------------------------------------------- *
 * FILEUSER_OBJS
 * ------------------------------------------------------------------------- */

static fileuser_t *fileuser_create    (const char *path, const char *cmd, int pid, int fd);
static void        fileuser_delete    (fileuser_t *self);
static void        fileuser_delete_cb (void *self);

/* ------------------------------------------------------------------------- *
 * MODULE_API
 * ------------------------------------------------------------------------- */

static void        fileusers_scan_pid_files (int pid);
static void        fileusers_scan_pids      (void);

GSList            *fileusers_get  (const char *path);
void               fileusers_init (void);
void               fileusers_quit (void);

/* ========================================================================= *
 * GENERIC_UTILS
 * ========================================================================= */

/** Do or die malloc() helper
 */
static void *fu_malloc(size_t size)
{
    void *res = malloc(size);

    if( !res && size )
        abort();

    return res;
}

/** Do or die calloc() helper
 */
static void *fu_calloc(size_t nmemb, size_t size)
{
    void *res = calloc(nmemb, size);

    if( !res && nmemb && size )
        abort();

    return res;
}

/** Do or die strdup() helper
 */
static char *fu_strdup(const char *str)
{
    char *res = str ? strdup(str) : 0;

    if( !res && str )
        abort();

    return res;
}

/** Ascii digit character predicate function
 */
static bool
fu_ascii_digit_p(int ch)
{
    return '0' <= ch && ch <= '9';
}

/** Read file content as string
 *
 * Note: Content beyond 4k in size is ignored.
 */
static char *
fu_read_content(const char *path)
{
    char *res = 0;
    int   fd  = -1;

    if( (fd = open(path, O_RDONLY)) == -1 )
        goto EXIT;

    char buf[4<<10];

    int rc = read(fd, buf, sizeof buf);
    if( rc < 0 )
        goto EXIT;

    res = fu_malloc(rc + 1);
    memcpy(res, buf, rc);
    res[rc] = 0;

EXIT:
    if( fd != -1 )
        close(fd);

    return res;
}

/** Use heuristics to derive command name for process identifier
 */
static char *
fu_get_command_name(int pid)
{
    char *res = 0;

    char path[256];
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    char *tmp = fu_read_content(path);
    if( tmp )
        res = fu_strdup(basename(tmp));
    free(tmp);
    return res ?: fu_strdup("unknown");
}

/* ========================================================================= *
 * FILEUSER_OBJS
 * ========================================================================= */

/** Create fileuser object
 */
static fileuser_t *
fileuser_create(const char *path, const char *cmd, int pid, int fd)
{
    fileuser_t *self = fu_calloc(1, sizeof *self);

    self->path = fu_strdup(path ?: "unknown");
    self->cmd  = fu_strdup(cmd  ?: "unknown");
    self->pid  = pid;
    self->fd   = fd;

    return self;
}

/** Delete fileuser object
 */
static void
fileuser_delete(fileuser_t *self)
{
    if( self ) {
        free(self->cmd);
        free(self->path);
        free(self);
    }
}

/** Type agnostic fileuser object delete function for use as a callback
 */
static void
fileuser_delete_cb(void *self)
{
    fileuser_delete(self);
}

/* ========================================================================= *
 * MODULE_API
 * ========================================================================= */

/** Cache of evdev input files that at least one process has open */
static GSList *fileusers_list = 0;

/** Scan evdev input files that a process has open
 */
static void
fileusers_scan_pid_files(int pid)
{
    static const char   pfix_str[] = "/dev/input/event";
    static const size_t pfix_len   = sizeof pfix_str - 1;

    DIR  *dir = 0;
    char *cmd = 0;

    char base[256];

    snprintf(base, sizeof base, "/proc/%d/fd", pid);

    if( !(dir = opendir(base)) ) {
        mce_log(LL_WARN, "%s: can't scan dir: %m", base);
        goto EXIT;
    }

    struct dirent *de;

    int errno_prev = 0;

    while( (de = readdir(dir)) ) {
        if( de->d_type != DT_LNK )
            continue;

        if( !fu_ascii_digit_p(*de->d_name) )
            continue;

        char srce[256];
        snprintf(srce, sizeof srce, "%s/%s", base, de->d_name);

        char dest[256];
        ssize_t rc = readlink(srce, dest, sizeof dest - 1);

        if( rc < 0 ) {
            if( errno_prev != errno ) {
                errno_prev = errno;
                mce_log(LL_WARN, "%s: can't read link: %m", srce);
            }
            continue;
        }

        if( rc > (int)sizeof dest - 1 )
            continue;

        if( rc < (int)pfix_len )
            continue;

        if( memcmp(dest, pfix_str, pfix_len) )
            continue;

        dest[rc] = 0;

        if( !cmd )
            cmd = fu_get_command_name(pid);

        int fd = strtol(de->d_name, 0, 0);

        fileuser_t *fu = fileuser_create(dest, cmd, pid, fd);
        fileusers_list = g_slist_prepend(fileusers_list, fu);
    }

EXIT:
    free(cmd);

    if( dir )
        closedir(dir);
}

/** Scan processes that might have evdev input files open
 */
static void
fileusers_scan_pids(void)
{
    DIR *dir = 0;

    if( !(dir = opendir("/proc")) ) {
        mce_log(LL_WARN, "%s: can't scan dir: %m", "/proc");
        goto EXIT;
    }

    struct dirent *de;

    while( (de = readdir(dir)) ) {
        if( de->d_type != DT_DIR )
            continue;

        if( !fu_ascii_digit_p(*de->d_name) )
            continue;

        int pid = strtol(de->d_name, 0, 0);
        fileusers_scan_pid_files(pid);
    }

EXIT:
    if( dir )
        closedir(dir);
}

/** Initialize open-evdev-files cache
 */
void
fileusers_init(void)
{
    fileusers_scan_pids();
}

/** Flush open-evdev-files cache
 */
void
fileusers_quit(void)
{
    g_slist_free_full(fileusers_list, fileuser_delete_cb),
        fileusers_list = 0;
}

/** Get a list of open files for an evdev input file
 *
 * Note: While the entries in the returned list must not
 *       be freed or modified, the returned list itself
 *       must be released with g_slist_free().
 *
 * @param path  Canonical path to an evdev input device node
 *
 * @return list of fileuser objects; or NULL
 */
GSList *
fileusers_get(const char *path)
{
    GSList *list = 0;

    if( !path )
        goto EXIT;

    for( GSList *item = fileusers_list; item; item = item->next ) {
        fileuser_t *fu = item->data;

        if( !fu->path )
            continue;

        if( !strcmp(fu->path, path) )
            list = g_slist_prepend(list, fu);
    }

EXIT:
    return list;
}
