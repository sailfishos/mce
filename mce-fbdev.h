/**
 * @file mce-fbdev.h
 * Frame buffer device handling code for the Mode Control Entity
 * <p>
 * Copyright 2015 Jolla Ltd.
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

#ifndef MCE_FBDEV_H_
# define MCE_FBDEV_H_

# include <stdbool.h>

# ifdef __cplusplus
extern "C" {
# endif

void mce_fbdev_init              (void);
void mce_fbdev_quit              (void);

bool mce_fbdev_open              (void);
void mce_fbdev_close             (void);
void mce_fbdev_reopen            (void);
bool mce_fbdev_is_open           (void);

void mce_fbdev_set_power         (bool power_on);

void mce_fbdev_linger_after_exit (int delay_ms);

# ifdef __cplusplus
};
# endif

#endif /* MCE_FBDEV_H_ */
