/**
 * @file evdev.h
 * Mode Control Entity - evdev input device handling
 * <p>
 * Copyright (c) 2012 - 2019 Jolla Ltd.
 * Copyright (c) 2026 Jolla Mobile Ltd
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

#include <linux/input.h>

#ifndef  KEY_CAMERA_SNAPSHOT
# define KEY_CAMERA_SNAPSHOT 0x02fe
#endif

#ifndef SW_CAMERA_LENS_COVER
/** Input layer code for the camera lens cover switch */
# define SW_CAMERA_LENS_COVER           0x09
#endif

#ifndef SW_KEYPAD_SLIDE
/** Input layer code for the keypad slide switch */
# define SW_KEYPAD_SLIDE                0x0a
#endif

#ifndef SW_FRONT_PROXIMITY
/** Input layer code for the front proximity sensor switch */
# define SW_FRONT_PROXIMITY             0x0b
#endif

#ifndef KEY_CAMERA_FOCUS
/** Input layer code for the camera focus button */
# define KEY_CAMERA_FOCUS               0x0210
#endif

#ifndef FF_STATUS_CNT
# ifdef FF_STATUS_MAX
#  define FF_STATUS_CNT (FF_STATUS_MAX+1)
# else
#  define FF_STATUS_CNT 0
# endif
#endif

#ifndef PWR_CNT
# ifdef PWR_MAX
#  define PWR_CNT (PWR_MAX+1)
# else
#  define PWR_CNT 0
# endif
#endif

#ifdef __cplusplus
extern "C" {
#elif 0
} /* fool JED indentation ... */
#endif

/* On 32-bit arches input_event->time is undefined when using a time64
 * kernel. These macros are used to normalize timestamp access in code.
 */
# ifndef input_event_sec
#  define input_event_sec time.tv_sec
# endif

# ifndef input_event_usec
#  define input_event_usec time.tv_usec
# endif

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
