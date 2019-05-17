/**
 * @file filewatcher.h
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

#ifndef FILEWATCHER_H_
# define FILEWATCHER_H_

# include <glib.h>

# ifdef __cplusplus
extern "C" {
# elif 0
} /* fool JED indentation ... */
# endif

typedef void (*filewatcher_changed_fn)(const char *path,
                                       const char *file,
                                       gpointer user_data);

typedef struct filewatcher_t filewatcher_t;

filewatcher_t *filewatcher_create(const char *dirpath,
                                  const char *filename,
                                  filewatcher_changed_fn change_cb,
                                  gpointer user_data,
                                  GDestroyNotify delete_cb);

void filewatcher_delete(filewatcher_t *self);

void filewatcher_force_trigger(filewatcher_t *self);

# ifdef __cplusplus
};
# endif

#endif /* FILEWATCHER_H_ */
