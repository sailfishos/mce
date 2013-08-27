/**
 * @file mce-log.c
 * Logging functions for Mode Control Entity
 * <p>
 * Copyright © 2006-2007, 2010 Nokia Corporation and/or its subsidiary(-ies).
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
#include <glib.h>
#include <glib/gprintf.h>

#ifdef OSSOLOG_COMPILE
#include <stdio.h>			/* fprintf() */
#include <stdarg.h>			/* va_start(), va_end(), vfprintf() */
#include <string.h>			/* strdup() */
#include <syslog.h>			/* openlog(), closelog(), vsyslog() */
#include <sys/time.h>
#include "mce-log.h"

static unsigned int logverbosity = LL_WARN;	/**< Log verbosity */
static int logtype = MCE_LOG_STDERR;		/**< Output for log messages */
static char *logname = NULL;

/** Get monotonic time as struct timeval */
static void monotime(struct timeval *tv)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	TIMESPEC_TO_TIMEVAL(tv, &ts);
}
static void timestamp(struct timeval *tv)
{
	static struct timeval start, prev;
	struct timeval diff;
	monotime(tv);
	timersub(tv, &prev, &diff);
	if( diff.tv_sec >= 4 ) start = *tv;
	prev = *tv;
	timersub(tv, &start, tv);
}

/** Make sure loglevel is in the supported range
 *
 * @param loglevel level to check
 *
 * @return log level clipped to LL_CRIT ... LL_DEBUG range
 */
static loglevel_t mce_log_level_normalize(loglevel_t loglevel)
{
	if( loglevel < LL_CRIT ) {
		loglevel = LL_CRIT;
	}
	else if( loglevel > LL_DEBUG ) {
		loglevel = LL_DEBUG;
	}
	return loglevel;
}

/** Get level indication tag to include in stderr logging
 *
 * @param loglevel level for the message
 *
 * @return level indication string
 */
static const char *mce_log_level_tag(loglevel_t loglevel)
{
	const char *res = "?";
	switch( loglevel ) {
        case LL_CRIT:   res = "C"; break;
        case LL_ERR:    res = "E"; break;
        case LL_WARN:   res = "W"; break;
        case LL_NOTICE: res = "N"; break;
        case LL_INFO:   res = "I"; break;
        case LL_DEBUG:  res = "D"; break;
        default: break;
        }
	return res;
}

/**
 * Log debug message with optional filename and function name attached
 *
 * @param loglevel The level of severity for this message
 * @param fmt The format string for this message
 * @param ... Input to the format string
 */
void mce_log_file(loglevel_t loglevel, const char *const file,
		  const char *const function, const char *const fmt, ...)
{
	va_list args;

	loglevel = mce_log_level_normalize(loglevel);

	if (logverbosity >= loglevel) {
		gchar *msg = 0;

		va_start(args, fmt);
		g_vasprintf(&msg, fmt, args);
		va_end(args);

		if( file && function ) {
			gchar *tmp = g_strconcat(file, ": ", function, "(): ",
						 msg, NULL);
			g_free(msg), msg = tmp;
		}

		if (logtype == MCE_LOG_STDERR) {
			struct timeval tv;
			timestamp(&tv);
			fprintf(stderr, "%s: T+%ld.%03ld %s: %s\n",
				logname,
				(long)tv.tv_sec, (long)(tv.tv_usec/1000),
				mce_log_level_tag(loglevel),
				msg);
		} else {
			/* loglevels are subset of syslog priorities, so
			 * we can use loglevel as is for syslog priority */
			syslog(loglevel, "%s", msg);
		}

		g_free(msg);
	}
}

/**
 * Set log verbosity
 * messages with loglevel higher than or equal to verbosity will be logged
 *
 * @param verbosity minimum level for log level
 */
void mce_log_set_verbosity(const int verbosity)
{
	logverbosity = verbosity;
}

/**
 * Open log
 *
 * @param name identifier to use for log messages
 * @param facility the log facility; normally LOG_USER or LOG_DAEMON
 * @param type log type to use; MCE_LOG_STDERR or MCE_LOG_SYSLOG
 */
void mce_log_open(const char *const name, const int facility, const int type)
{
	logtype = type;

	if (logtype == MCE_LOG_SYSLOG)
		openlog(name, LOG_PID | LOG_NDELAY, facility);
	else
		logname = g_strdup(name);
}

/**
 * Close log
 */
void mce_log_close(void)
{
	g_free(logname), logname = 0;

	if (logtype == MCE_LOG_SYSLOG)
		closelog();
}

/**
 * Log level testing predicate
 *
 * For testing whether given level of logging is allowed
 * before spending cpu time for gathering parameters etc
 *
 * @param loglevel level of logging we might do
 *
 * @return 1 if logging at givel level is enabled, 0 if not
 */
int mce_log_p(const loglevel_t loglevel)
{
	return logverbosity >= loglevel;
}

#endif /* OSSOLOG_COMPILE */
