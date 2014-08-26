/**
 * @file mce-log.h
 * Headers for the logging functions for Mode Control Entity
 * <p>
 * Copyright © 2006-2007, 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 *
 * mce is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * mce is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _MCE_LOG_H_
#define _MCE_LOG_H_

#include <syslog.h>

#define MCE_LOG_SYSLOG			1	/**< Log to syslog */
#define MCE_LOG_STDERR			0	/**< Log to stderr */

/** Severity of loglevels (subset of syslog priorities) */
typedef enum {
	LL_NONE    = 0,			/**< No logging at all */
	LL_ALERT   = LOG_ALERT,		/**< Alert */
	LL_CRIT    = LOG_CRIT,		/**< Critical error */
	LL_ERR     = LOG_ERR,		/**< Error */
	LL_WARN    = LOG_WARNING,	/**< Warning */
	LL_NOTICE  = LOG_NOTICE,	/**< Normal but noteworthy */
	LL_INFO    = LOG_INFO,		/**< Informational message */
	LL_DEBUG   = LOG_DEBUG,		/**< Useful when debugging */

	LL_DEFAULT = LL_WARN,		/**< Default log level */

#ifdef ENABLE_DEVEL_LOGGING
	LL_DEVEL   = LL_CRIT,		/**< Log by default on devel build */
#else
	LL_DEVEL   = LL_NOTICE,		/**< Otherwise verbose mode needed */
#endif

} loglevel_t;

void mce_log_add_pattern(const char *pat);

#ifdef OSSOLOG_COMPILE
void mce_log_file(loglevel_t loglevel, const char *const file,
		  const char *const function, const char *const fmt, ...)
	__attribute__((format(printf, 4, 5)));
#define mce_log_raw(__loglevel, __fmt, __args...)	mce_log_file(__loglevel, NULL, NULL, __fmt , ## __args)
#define mce_log(__loglevel, __fmt, __args...) \
     do {\
	 if( mce_log_p(__loglevel) )\
	     mce_log_file(__loglevel, __FILE__, __FUNCTION__, __fmt , ## __args); \
     } while(0)

void mce_log_set_verbosity(const int verbosity);
void mce_log_open(const char *const name, const int facility, const int type);
void mce_log_close(void);
int mce_log_p_(const loglevel_t loglevel, const char *const file, const char *const function);
#define mce_log_p(LEV_) mce_log_p_(LEV_,__FILE__,__FUNCTION__)
#else
/** Dummy version used when logging is disabled at compile time */
#define mce_log(_loglevel, _fmt, ...)			do {} while (0)

/** Dummy version used when logging is disabled at compile time */
#define mce_log_set_verbosity(_verbosity)		do {} while (0)

/** Dummy version used when logging is disabled at compile time */
#define mce_log_open(_name, _facility, _type)		do {} while (0)

/** Dummy version used when logging is disabled at compile time */
#define mce_log_close()					do {} while (0)

/** Dummy version used when logging is disabled at compile time */
#define mce_log_p(_loglevel)				0
#endif /* OSSOLOG_COMPILE */
#endif /* _MCE_LOG_H_ */
