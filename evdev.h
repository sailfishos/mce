/* ------------------------------------------------------------------------- *
 * Copyright (C) 2012-2013 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: GPLv2
 * ------------------------------------------------------------------------- */

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
    /* Values */
    GESTURE_SWIPE_FROM_LEFT   = 0,
    GESTURE_SWIPE_FROM_RIGHT  = 1,
    GESTURE_SWIPE_FROM_TOP    = 2,
    GESTURE_SWIPE_FROM_BOTTOM = 3,
    GESTURE_DOUBLETAP         = 4, /* To conform with value used in
                                    * Nokia N9 kernel driver */
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
