/**
 * @file mce-worker.h
 *
 * Mode Control Entity - Offload blocking operations to a worker thread
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
#ifndef MCE_WORKER_H_
# define MCE_WORKER_H_

# include <stdbool.h>

# ifdef __cplusplus
extern "C" {
# elif 0
} /* fool JED indentation ... */
# endif

void  mce_worker_add_job    (const char *context, const char *name, void *(*handle)(void *), void (*notify)(void *, void *), void *param);

void  mce_worker_add_context(const char *context);
void  mce_worker_rem_context(const char *context);

void  mce_worker_quit       (void);
bool  mce_worker_init       (void);

# ifdef __cplusplus
};
# endif

#endif /* MCE_WORKER_H_ */
