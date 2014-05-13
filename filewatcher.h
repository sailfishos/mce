/* ------------------------------------------------------------------------- *
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: LGPLv2
 * ------------------------------------------------------------------------- */

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
