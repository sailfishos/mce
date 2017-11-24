#ifndef MEMNOTIFY_H_
# define MEMNOTIFY_H_

/* ========================================================================= *
 * Settings
 * ========================================================================= */

/** Prefix for memnotify setting keys */
# define MCE_SETTING_MEMNOTIFY_PATH             "/system/osso/dsm/memnotify"

/** Memnotify warning level configuration */
# define MCE_SETTING_MEMNOTIFY_WARNING_PATH     MCE_SETTING_MEMNOTIFY_PATH"/warning"

/** Warning threshold for used memory [pages] */
# define MCE_SETTING_MEMNOTIFY_WARNING_USED     MCE_SETTING_MEMNOTIFY_PATH"/warning/used"
# define MCE_DEFAULT_MEMNOTIFY_WARNING_USED     0 // = disabled

/** Warning threshold for active memory [pages] */
# define MCE_SETTING_MEMNOTIFY_WARNING_ACTIVE   MCE_SETTING_MEMNOTIFY_PATH"/warning/active"
# define MCE_DEFAULT_MEMNOTIFY_WARNING_ACTIVE   0 // = disabled

/** Memnotify critical level configuration */
# define MCE_SETTING_MEMNOTIFY_CRITICAL_PATH    MCE_SETTING_MEMNOTIFY_PATH"/critical"

/** Critical threshold for used memory [pages] */
# define MCE_SETTING_MEMNOTIFY_CRITICAL_USED    MCE_SETTING_MEMNOTIFY_PATH"/critical/used"
# define MCE_DEFAULT_MEMNOTIFY_CRITICAL_USED    0 // = disabled

/** Critical threshold for active memory [pages] */
# define MCE_SETTING_MEMNOTIFY_CRITICAL_ACTIVE  MCE_SETTING_MEMNOTIFY_PATH"/critical/active"
# define MCE_DEFAULT_MEMNOTIFY_CRITICAL_ACTIVE  0 // = disabled

#endif /* MEMNOTIFY_H_ */
