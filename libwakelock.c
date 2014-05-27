/* ------------------------------------------------------------------------- *
 * Copyright (C) 2012 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: LGPLv2
 * ------------------------------------------------------------------------- */

/* NOTE: Only async-signal-safe functions can be called from this
 *       module since we might need to use the functionality while
 *       handling non-recoverable signals!
 */

#include "libwakelock.h"

#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

/** Whether to write debug logging to stderr
 *
 * If not enabled, no diagnostics of any kind gets written.
 */
#ifndef LWL_ENABLE_LOGGING
# define LWL_ENABLE_LOGGING 01
#endif

/** Prefix used for log messages
 */
#define LWL_LOG_PFIX "LWL: "

/** Number to string helper
 *
 * @param buf work space
 * @param len size of work space
 * @param num number to convert
 * @return pointer to ascii representation of num
 */
static char *lwl_number(char *buf, size_t len, long long num)
{
	char              *pos = buf + len;
	int                sgn = (num < 0);
	unsigned long long val = sgn ? -num : num;

	auto void emit(int c) {
		if( pos > buf ) *--pos = c;
	}

	/* terminate at end of work space */
	emit(0);

	/* work backwards from least signigicant digits */
	do { emit('0' + val % 10); } while( val /= 10 );

	/* apply minus sign if needed */
	if( sgn ) emit('-');

	/* return pointer to 1st digit (not start of buffer) */
	return pos;
}

/** String concatenation helper
 *
 * @param buf work space
 * @param len size of work space
 * @param str first string to copy
 * @param ... NULL terminated list of more strings to copy
 * @return pointer to the start of the buffer
 */
static char *lwl_concat(char *buf, size_t len, const char *str, ...)
{
	char *pos = buf;
	char *end = buf + len - 1;
	va_list va;

	auto void emit(const char *s) {
		while( pos < end && *s ) *pos++ = *s++;
	}

	va_start(va, str);
	while( str )
		emit(str), str = va_arg(va, const char *);
	va_end(va);

	*pos = 0;
	return buf;
}

#if LWL_ENABLE_LOGGING
/** Flag for enabling wakelock debug logging */
static int lwl_debug_enabled = 0;

/** Logging functionality that can be configured out at compile time
 */
static void lwl_debug_(const char *m, ...)
{
	char buf[256];
	char *pos = buf;
	char *end = buf + sizeof buf - 1;
	va_list va;

	auto void emit(const char *s)
	{
		while( pos < end && *s ) *pos++ = *s++;
	}

	emit("LWL: ");
	va_start(va, m);
	while( m )
	{
		emit(m), m = va_arg(va, const char *);
	}
	va_end(va);

	if( write(STDERR_FILENO, buf, pos - buf) < 0 )
	{
		// do not care
	}
}

# define lwl_debug(MSG, MORE...) \
	do {\
		if( lwl_debug_enabled ) lwl_debug_(MSG, ##MORE); \
	} while( 0 )

#else
# define lwl_debug(MSG, MORE...) do { } while( 0 )
#endif

/** Flag that gets set once the process is about to exit */
static int        lwl_shutting_down = 0;

/** Sysfs entry for acquiring wakelocks */
static const char lwl_lock_path[]   = "/sys/power/wake_lock";

/** Sysfs entry for releasing wakelocks */
static const char lwl_unlock_path[] = "/sys/power/wake_unlock";

/** Sysfs entry for allow/block suspend */
static const char lwl_state_path[] = "/sys/power/state";

/** Helper for writing to sysfs files
 */
static void lwl_write_file(const char *path, const char *data)
{
	int file;

	lwl_debug(path, " << ", data, NULL);

	if( (file = TEMP_FAILURE_RETRY(open(path, O_WRONLY))) == -1 ) {
		lwl_debug(path, ": open: ", strerror(errno), "\n", NULL);
	} else {
		int size = strlen(data);
		errno = 0;
		if( TEMP_FAILURE_RETRY(write(file, data, size)) != size ) {
			lwl_debug(path, ": write: ", strerror(errno),
				  "\n", NULL);
		}
		TEMP_FAILURE_RETRY(close(file));
	}
}

/** Helper for checking if wakelock interface is supported
 */
static int lwl_enabled(void)
{
	static int checked = 0, enabled = 0;

	if( !checked ) {
		checked = 1, enabled = !access(lwl_lock_path, F_OK);
		lwl_debug(enabled ? "enabled" : "disabled", "\n", NULL);
	}

	return enabled;
}

/** Use sysfs interface to create and enable a wakelock.
 *
 * @param name The name of the wakelock to obtain
 * @param ns   Time in nanoseconds before the wakelock gets released
 *             automatically, or negative value for no timeout.
 */
void wakelock_lock(const char *name, long long ns)
{
	if( lwl_enabled() && !lwl_shutting_down ) {
		char tmp[64];
		char num[64];
		if( ns < 0 ) {
			lwl_concat(tmp, sizeof tmp, name, "\n", NULL);
		} else {
			lwl_concat(tmp, sizeof tmp, name, " ",
				   lwl_number(num, sizeof num, ns),
				   "\n", NULL);
		}
		lwl_write_file(lwl_lock_path, tmp);
	}
}

/** Use sysfs interface to disable a wakelock.
 *
 * @param name The name of the wakelock to release
 *
 * Note: This will not delete the wakelocks.
 */
void wakelock_unlock(const char *name)
{
	if( lwl_enabled() ) {
		char tmp[64];
		lwl_concat(tmp, sizeof tmp, name, "\n", NULL);
		lwl_write_file(lwl_unlock_path, tmp);
	}
}

/** Use sysfs interface to allow automatic entry to suspend
 *
 * After this call the device will enter suspend mode once all
 * the wakelocks have been released.
 *
 * Android kernels will enter early suspend (i.e. display is
 * turned off etc) even if there still are active wakelocks.
 */
void wakelock_allow_suspend(void)
{
	if( lwl_enabled() && !lwl_shutting_down ) {
		lwl_write_file(lwl_state_path, "mem\n");
	}
}

/** Use sysfs interface to block automatic entry to suspend
 *
 * The device will not enter suspend mode with or without
 * active wakelocks.
 */
void wakelock_block_suspend(void)
{
	if( lwl_enabled() ) {
		lwl_write_file(lwl_state_path, "on\n");
	}
}

/** Block automatic suspend without possibility to unblock it again
 *
 * For use on exit path. We want to do clean exit from mainloop and
 * that might that code that re-enables autosuspend gets triggered
 * while we're on exit path.
 *
 * By calling this function when initiating daemon shutdown we are
 * protected against this.
 */

void wakelock_block_suspend_until_exit(void)
{
	lwl_shutting_down = 1;
	wakelock_block_suspend();
}

/** Enable wakelock debug logging (if support compiled in)
 */
void lwl_enable_logging(void)
{
	lwl_debug_enabled = 1;
}
