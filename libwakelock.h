/**
 * @file libwakelock.h
 * Mode Control Entity - wakelock management
 * <p>
 * Copyright (C) 2012-2019 Jolla Ltd.
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

#ifndef LIBWAKELOCK_H_
# define LIBWAKELOCK_H_

# ifdef __cplusplus
extern "C" {
# elif 0
};
# endif

/** Suspend model supported by the kernel */
typedef enum {
    /* Not known yet */
    SUSPEND_TYPE_UNKN  = -1,

    /* Suspend not supported */
    SUSPEND_TYPE_NONE  =  0,

    /* Early suspend model */
    SUSPEND_TYPE_EARLY =  1,

    /* Autosleep model */
    SUSPEND_TYPE_AUTO  =  2,
} suspend_type_t;

void wakelock_lock  (const char *name, long long ns);
void wakelock_unlock(const char *name);

void wakelock_allow_suspend(void);
void wakelock_block_suspend(void);
void wakelock_block_suspend_until_exit(void);

void lwl_enable_logging(void);
suspend_type_t lwl_probe(void);

# ifdef __cplusplus
};
# endif

#endif /* LIBWAKELOCK_H_ */
