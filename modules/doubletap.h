/* ------------------------------------------------------------------------- *
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: LGPLv2
 * ------------------------------------------------------------------------- */

#ifndef DOUBLETAP_H_
# define DOUBLETAP_H_

# ifdef __cplusplus
extern "C" {
# elif 0
} /* fool JED indentation ... */
# endif

/** Name of doubletap ini file configuration group */
# define MCE_CONF_DOUBLETAP_GROUP        "DoubleTap"

/** Name of the configuration key for doubletap wakeup control file */
# define MCE_CONF_DOUBLETAP_CONTROL_PATH "ControlPath"

/** Name of the configuration key for doubletap enable value */
# define MCE_CONF_DOUBLETAP_ENABLE_VALUE "EnableValue"

/** Name of the configuration key for doubletap disable, touch powered off value */
# define MCE_CONF_DOUBLETAP_DISABLE_VALUE "DisableValue"

/** Name of the configuration key for doubletap disable, touch powered on value */
# define MCE_CONF_DOUBLETAP_DISABLE_NO_SLEEP_VALUE "DisableNoSleepValue"

/** Path to the GConf settings for the doubletap module */
# define MCE_GCONF_DOUBLETAP_PATH       "/system/osso/dsm/doubletap"
/** Path to the doubletap  mode GConf setting */
# define MCE_GCONF_DOUBLETAP_MODE       MCE_GCONF_DOUBLETAP_PATH "/mode"

/** Double tap wakeup enable modes */
typedef enum
{
        /** Double tap wakeups disabled */
        DBLTAP_ENABLE_NEVER,

        /** Double tap wakeups always enabled */
        DBLTAP_ENABLE_ALWAYS,

        /** Double tap wakeups enabled when PS is not covered */
        DBLTAP_ENABLE_NO_PROXIMITY,

        DBLTAP_ENABLE_DEFAULT = DBLTAP_ENABLE_NO_PROXIMITY,
} dbltap_mode_t;

# ifdef __cplusplus
};
# endif

#endif /* DOUBLETAP_H_ */
