#ifndef MEMNOTIFY_H_
# define MEMNOTIFY_H_

/* ========================================================================= *
 * Settings
 * ========================================================================= */

/** Base path for memnotify configuration */
# define MCE_SETTING_MEMNOTIFY_PATH             "/system/osso/dsm/memnotify"

/** Memnotify warning level configuration */
# define MCE_SETTING_MEMNOTIFY_WARNING_PATH     MCE_SETTING_MEMNOTIFY_PATH"/warning"

# define MCE_SETTING_MEMNOTIFY_WARNING_USED     MCE_SETTING_MEMNOTIFY_PATH"/warning/used"
# define MCE_DEFAULT_MEMNOTIFY_WARNING_USED     0 // = disabled

# define MCE_SETTING_MEMNOTIFY_WARNING_ACTIVE   MCE_SETTING_MEMNOTIFY_PATH"/warning/active"
# define MCE_DEFAULT_MEMNOTIFY_WARNING_ACTIVE   0 // = disabled

/** Memnotify critical level configuration */
# define MCE_SETTING_MEMNOTIFY_CRITICAL_PATH    MCE_SETTING_MEMNOTIFY_PATH"/critical"

# define MCE_SETTING_MEMNOTIFY_CRITICAL_USED    MCE_SETTING_MEMNOTIFY_PATH"/critical/used"
# define MCE_DEFAULT_MEMNOTIFY_CRITICAL_USED    0 // = disabled

# define MCE_SETTING_MEMNOTIFY_CRITICAL_ACTIVE  MCE_SETTING_MEMNOTIFY_PATH"/critical/active"
# define MCE_DEFAULT_MEMNOTIFY_CRITICAL_ACTIVE  0 // = disabled

/* ========================================================================= *
 * D-Bus Constants
 * ========================================================================= */

/** Signal that is sent when memory use level changes
 *
 * Has a string parameter: "normal", "warning" or "critical" (actual strings
 * are defined in the memnotify_limit[] array).
 */
# define MCE_MEMORY_LEVEL_SIG           "sig_memory_level_ind"

/** Query current memory level */
# define MCE_MEMORY_LEVEL_GET           "get_memory_level"

#endif /* MEMNOTIFY_H_ */
