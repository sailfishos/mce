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

const char *evdev_get_event_code_name(int etype, int ecode);
const char *evdev_get_event_type_name(int etype);

int evdev_lookup_event_code(int etype, const char *ename);

int evdev_open_device(const char *path);
int evdev_identify_device(int fd);

#ifdef __cplusplus
};
#endif

#endif /* EVDEV_H_ */
