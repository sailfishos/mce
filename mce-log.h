/**
 * @file mce-log.h
 * Headers for the logging functions for Mode Control Entity
 * <p>
 * Copyright © 2006-2007, 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
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
#ifndef MCE_LOG_H_
# define MCE_LOG_H_

# include <syslog.h>
# include <stdarg.h>

# define MCE_LOG_SYSLOG		1	/**< Log to syslog */
# define MCE_LOG_STDERR		0	/**< Log to stderr */

/** Severity of loglevels (subset of syslog priorities) */
typedef enum {
	LL_NONE    = 0,			/**< No logging at all */
	LL_EXTRA   = LOG_ALERT,		/**< Placeholder for devel logging */
	LL_CRIT    = LOG_CRIT,		/**< Critical error */
	LL_ERR     = LOG_ERR,		/**< Error */
	LL_WARN    = LOG_WARNING,	/**< Warning */
	LL_NOTICE  = LOG_NOTICE,	/**< Normal but noteworthy */
	LL_INFO    = LOG_INFO,		/**< Informational message */
	LL_DEBUG   = LOG_DEBUG,		/**< Useful when debugging */

	LL_DEFAULT = LL_WARN,		/**< Default log level */

# ifdef ENABLE_DEVEL_LOGGING
	LL_DEVEL   = LL_EXTRA,		/**< Log by default on devel build */
# else
	LL_DEVEL   = LL_NOTICE,		/**< Otherwise verbose mode needed */
# endif

	LL_MAXIMUM = LOG_DEBUG,		/**< Minimum for bounds checking */
	LL_MINIMUM = LOG_EMERG,		/**< Maximum for bounds checking */

	LL_CRUCIAL = LOG_EMERG,		/**< Elevated priority for solving
					 *   problems that require logs from
					 *   the whole user base. Should be
					 *   downgraded as soon as possible. */
} loglevel_t;

# ifdef OSSOLOG_COMPILE
void mce_log_add_pattern(const char *pat);
void mce_log_set_verbosity(int verbosity);
int  mce_log_get_verbosity(void);

int  mce_log_p_(loglevel_t loglevel,
		const char *const file, const char *const function);

void mce_log_unconditional_va(loglevel_t loglevel, const char *const file, const char *const function, const char *const fmt, va_list va);

void mce_log_unconditional(loglevel_t loglevel, const char *const file,
		 const char *const function, const char *const fmt, ...)
		  __attribute__((format(printf, 4, 5)));

void mce_log_file(loglevel_t loglevel, const char *const file,
		  const char *const function, const char *const fmt, ...)
		  __attribute__((format(printf, 4, 5)));

void mce_log_open(const char *const name, const int facility, const int type);
void mce_log_close(void);

#  define mce_log_p(LEV_)\
	mce_log_p_(LEV_,__FILE__,__FUNCTION__)

#  define mce_log_raw(LEV_, FMT_, ARGS_...)\
	mce_log_file(LEV_, NULL, NULL, FMT_ , ## ARGS_)

#  define mce_log(LEV_, FMT_, ARGS_...)\
	do {\
		if( mce_log_p(LEV_) )\
			mce_log_file(LEV_, __FILE__, __FUNCTION__,\
				     FMT_ , ## ARGS_);\
	} while(0)

# else
/* Dummy versions used when logging is disabled at compile time */
#  define mce_log_add_pattern(PAT_)             do {} while (0)
#  define mce_log_set_verbosity(LEV_)           do {} while (0)
#  define mce_log_open(NAME_, FACILITY_, TYPE_) do {} while (0)
#  define mce_log_close()                       do {} while (0)
#  define mce_log_p(LEV_)                       0
#  define mce_log(LEV_, FMT_, ...)              do {} while (0)
#  define mce_log_raw(LEV_, FMT_, ARGS_...)     do {} while (0)
# endif /* OSSOLOG_COMPILE */
#endif /* MCE_LOG_H_ */
