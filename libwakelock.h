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

void wakelock_lock  (const char *name, long long ns);
void wakelock_unlock(const char *name);

void wakelock_allow_suspend(void);
void wakelock_block_suspend(void);
void wakelock_block_suspend_until_exit(void);

# ifdef __cplusplus
};
# endif

#endif /* LIBWAKELOCK_H_ */
