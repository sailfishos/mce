/**
 * @file mce-wltimer.h
 *
 * Mode Control Entity - Timers that block suspend until triggered
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

#ifndef MCE_WLTIMER_H_
# define MCE_WLTIMER_H_

# include <stdbool.h>
# include <glib.h>

# ifdef __cplusplus
extern "C" {
# endif

typedef struct mce_wltimer_t mce_wltimer_t;

mce_wltimer_t * mce_wltimer_create      (const char *name, int period, GSourceFunc notify, void *user_data);
void            mce_wltimer_delete      (mce_wltimer_t *self);

bool            mce_wltimer_is_active   (const mce_wltimer_t *self);
const char     *mce_wltimer_get_name    (const mce_wltimer_t *self);
void            mce_wltimer_set_period  (mce_wltimer_t *self, int period);

void            mce_wltimer_start       (mce_wltimer_t *self);
void            mce_wltimer_stop        (mce_wltimer_t *self);

void            mce_wltimer_dispatch    (void);

void            mce_wltimer_init        (void);
void            mce_wltimer_quit        (void);

# ifdef __cplusplus
};
# endif

#endif /* MCE_WLTIMER_H_ */
