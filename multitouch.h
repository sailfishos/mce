/**
 * @file multitouch.h
 *
 * Mode Control Entity - Tracking for evdev based multitouch devices
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

#ifndef MCE_MULTITOUCH_H_
# define MCE_MULTITOUCH_H_

# include <linux/input.h>
# include <stdbool.h>

typedef struct mt_state_t mt_state_t;

mt_state_t        *mt_state_create       (bool protocol_b);
void               mt_state_delete       (mt_state_t *self);
bool               mt_state_handle_event (mt_state_t *self, const struct input_event *ev);
bool               mt_state_touching     (const mt_state_t *self);

#endif /* MCE_MULTITOUCH_H_ */
