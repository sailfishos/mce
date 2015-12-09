/**
 * @file fileusers.h
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

#ifndef FILEUSERS_H_
# define FILEUSERS_H_

# include <glib.h>

# ifdef __cplusplus
extern "C" {
# elif 0
} /* fool JED indentation ... */
# endif

/** Cached process identification data for open file */
typedef struct
{
    /** File path */
    char *path;

    /** Name of the process that has the file open */
    char *cmd;

    /** Pid of the process that has the file open */
    int   pid;

    /** File descriptor the process is using for the file */
    int   fd;
} fileuser_t;

GSList            *fileusers_get  (const char *path);
void               fileusers_init (void);
void               fileusers_quit (void);

# ifdef __cplusplus
};
# endif

#endif /* FILEUSERS_H_ */
