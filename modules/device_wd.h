/* ------------------------------------------------------------------------- *
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: LGPLv2.1
 * ------------------------------------------------------------------------- */

#ifndef DEVICE_WD_H_
# define DEVICE_WD_H_

# ifdef __cplusplus
extern "C" {
# elif 0
} /* fool JED indentation ... */
# endif

/** Name of device watchdog configuration group */
# define MCE_CONF_DEVICEWD_GROUP                "DeviceWD"

/** Name of device watchdog kick path entry */
# define MCE_CONF_DEVICEWD_KICKPATH             "KickPath"

/** Name of device watchdog kick value entry */
# define MCE_CONF_DEVICEWD_VALUE                "KickValue"

/** Name of device watchdog kick period entry */
# define MCE_CONF_DEVICEWD_PERIOD               "KickPeriod"

# ifdef __cplusplus
};
#endif

#endif /* DEVICE_WD_H_ */
