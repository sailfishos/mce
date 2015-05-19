/**
 * @file mce-hbtimer.h
 *
 * Mode Control Entity - Suspend proof timer functionality
 *
 * <p>
 *
 * Copyright (C) 2014-2015 Jolla Ltd.
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

#ifndef MCE_HBTIMER_H_
# define MCE_HBTIMER_H_

# include <stdbool.h>
# include <glib.h>

# ifdef __cplusplus
extern "C" {
# endif

typedef struct mce_hbtimer_t mce_hbtimer_t;

mce_hbtimer_t * mce_hbtimer_create      (const char *name, int period, GSourceFunc notify, void *user_data);
void            mce_hbtimer_delete      (mce_hbtimer_t *self);

bool            mce_hbtimer_is_active   (const mce_hbtimer_t *self);
const char     *mce_hbtimer_get_name    (const mce_hbtimer_t *self);
void            mce_hbtimer_set_period  (mce_hbtimer_t *self, int period);

void            mce_hbtimer_start       (mce_hbtimer_t *self);
void            mce_hbtimer_stop        (mce_hbtimer_t *self);

void            mce_hbtimer_dispatch    (void);

void            mce_hbtimer_init        (void);
void            mce_hbtimer_quit        (void);

# ifdef __cplusplus
};
# endif

#endif /* MCE_HBTIMER_H_ */
