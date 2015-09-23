/** @file mce-wakelock.h
 * Wakelock multiplexing code for the Mode Control Entity
 * <p>
 * Copyright (C) 2015 Jolla Ltd.
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

#ifndef MCE_WAKELOCK_H_
# define MCE_WAKELOCK_H_

# ifdef __cplusplus
extern "C" {
# elif 0
} /* fool JED indentation ... */
# endif

void                   mce_wakelock_obtain      (const char *name, int duration_ms);
void                   mce_wakelock_release     (const char *name);

void                   mce_wakelock_init        (void);
void                   mce_wakelock_quit        (void);
void                   mce_wakelock_abort       (void);

# ifdef __cplusplus
};
# endif

#endif /* MCE_WAKELOCK_H_ */
