/**
 * @file evdev.h
 * Mode Control Entity - evdev input device handling
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

#ifndef EVDEV_H_
#define EVDEV_H_

#ifdef __cplusplus
extern "C" {
#elif 0
} /* fool JED indentation ... */
#endif

/** Assumed EV_MSC:MSC_GESTURE event values
 *
 * Actual gestures / values depend on hw and kernel driver - these are
 * meant to be used mainly to improve code readability.
 *
 * However GESTURE_DOUBLETAP is a special case as it is assumed by
 * mce to always mean doubletap.
 */
typedef enum {
    /* Values 0-15 reserved for touchscreen gestures */
    GESTURE_SWIPE_FROM_LEFT   = 0,
    GESTURE_SWIPE_FROM_RIGHT  = 1,
    GESTURE_SWIPE_FROM_TOP    = 2,
    GESTURE_SWIPE_FROM_BOTTOM = 3,
    GESTURE_DOUBLETAP         = 4, /* To conform with value used in
                                    * Nokia N9 kernel driver */
    GESTURE_FPWAKEUP          = 16,

    /* Modifiers */
    GESTURE_SYNTHESIZED       = (1<<8),
} gesture_t;

const char *evdev_get_event_code_name(int etype, int ecode);
const char *evdev_get_event_type_name(int etype);

int evdev_lookup_event_code(int etype, const char *ename);

int evdev_open_device(const char *path);
int evdev_identify_device(int fd);

#ifdef __cplusplus
};
#endif

#endif /* EVDEV_H_ */
