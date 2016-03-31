/* ------------------------------------------------------------------------- *
 * Copyright (C) 2013-2014 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: LGPLv2
 * ------------------------------------------------------------------------- */

#ifndef DOUBLETAP_H_
# define DOUBLETAP_H_

/* ========================================================================= *
 * Configuration
 * ========================================================================= */

/** Name of doubletap ini file configuration group */
# define MCE_CONF_DOUBLETAP_GROUP        "DoubleTap"

/** Name of the configuration key for doubletap wakeup control file */
# define MCE_CONF_DOUBLETAP_CONTROL_PATH "ControlPath"

/** Name of the configuration key for doubletap enable value */
# define MCE_CONF_DOUBLETAP_ENABLE_VALUE "EnableValue"

/** Name of the configuration key for doubletap disable value */
# define MCE_CONF_DOUBLETAP_DISABLE_VALUE "DisableValue"

/** Name of touch panel ini file configuration group */
# define MCE_CONF_TPSLEEP_GROUP          "TouchPanelSleep"

/** Name of the configuration key for touch panel sleep control file */
# define MCE_CONF_TPSLEEP_CONTROL_PATH   "ControlPath"

/** Name of the configuration key for touch panel sleep allowed value */
# define MCE_CONF_TPSLEEP_ALLOW_VALUE    "AllowValue"

/** Name of the configuration key for touch panel sleep denied value */
# define MCE_CONF_TPSLEEP_DENY_VALUE     "DenyValue"

/* ========================================================================= *
 * Settings
 * ========================================================================= */

/** Double tap wakeup enable modes */
typedef enum
{
        /** Double tap wakeups disabled */
        DBLTAP_ENABLE_NEVER        = 0,

        /** Double tap wakeups always enabled */
        DBLTAP_ENABLE_ALWAYS       = 1,

        /** Double tap wakeups enabled when PS is not covered */
        DBLTAP_ENABLE_NO_PROXIMITY = 2,
} dbltap_mode_t;

/** Prefix for doubletap setting keys */
# define MCE_SETTING_DOUBLETAP_PATH      "/system/osso/dsm/doubletap"

/** When doubletap detection is enabled */
# define MCE_SETTING_DOUBLETAP_MODE      MCE_SETTING_DOUBLETAP_PATH "/mode"
# define MCE_DEFAULT_DOUBLETAP_MODE      2 // = DBLTAP_ENABLE_NO_PROXIMITY

#endif /* DOUBLETAP_H_ */
