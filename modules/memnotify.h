#ifndef MEMNOTIFY_H_
# define MEMNOTIFY_H_

# ifdef __cplusplus
extern "C" {
# elif 0
} /* fool JED indentation ... */
# endif

/** Base path for memnotify configuration */
# define MCE_GCONF_MEMNOTIFY_PATH "/system/osso/dsm/memnotify"

/** Memnotify warning level configuration */
# define MCE_GCONF_MEMNOTIFY_WARNING_PATH MCE_GCONF_MEMNOTIFY_PATH"/warning"

# define MCE_GCONF_MEMNOTIFY_WARNING_USED   MCE_GCONF_MEMNOTIFY_WARNING_PATH"/used"
# define MCE_GCONF_MEMNOTIFY_WARNING_ACTIVE MCE_GCONF_MEMNOTIFY_WARNING_PATH"/active"

/** Memnotify critical level configuration */
# define MCE_GCONF_MEMNOTIFY_CRITICAL_PATH MCE_GCONF_MEMNOTIFY_PATH"/critical"

# define MCE_GCONF_MEMNOTIFY_CRITICAL_USED   MCE_GCONF_MEMNOTIFY_CRITICAL_PATH"/used"
# define MCE_GCONF_MEMNOTIFY_CRITICAL_ACTIVE MCE_GCONF_MEMNOTIFY_CRITICAL_PATH"/active"

/** Signal that is sent when memory use level changes
 *
 * Has a string parameter: "normal", "warning" or "critical" (actual strings
 * are defined in the memnotify_limit[] array).
 */
# define MCE_MEMORY_LEVEL_SIG           "sig_memory_level_ind"

/** Query current memory level */
# define MCE_MEMORY_LEVEL_GET           "get_memory_level"

# ifdef __cplusplus
};
# endif

#endif /* MEMNOTIFY_H_ */
