/* ------------------------------------------------------------------------- *
 * Copyright (C) 2012 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: LGPLv2
 * ------------------------------------------------------------------------- */

#ifndef LIBWAKELOCK_H_
# define LIBWAKELOCK_H_

# ifdef __cplusplus
extern "C" {
# elif 0
};
# endif

/** Suspend model supported by the kernel */
typedef enum {
    /* Not known yet */
    SUSPEND_TYPE_UNKN  = -1,

    /* Suspend not supported */
    SUSPEND_TYPE_NONE  =  0,

    /* Early suspend model */
    SUSPEND_TYPE_EARLY =  1,

    /* Autosleep model */
    SUSPEND_TYPE_AUTO  =  2,
} suspend_type_t;

void wakelock_lock  (const char *name, long long ns);
void wakelock_unlock(const char *name);

void wakelock_allow_suspend(void);
void wakelock_block_suspend(void);
void wakelock_block_suspend_until_exit(void);

void lwl_enable_logging(void);
suspend_type_t lwl_probe(void);

# ifdef __cplusplus
};
# endif

#endif /* LIBWAKELOCK_H_ */
