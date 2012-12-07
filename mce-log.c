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

#include "mce-log.h"

static unsigned int logverbosity = LL_WARN;	/**< Log verbosity */
static int logtype = MCE_LOG_STDERR;		/**< Output for log messages */
static char *logname = NULL;

/**
 * Log debug message with optional filename and function name attached
 *
 * @param loglevel The level of severity for this message
 * @param fmt The format string for this message
 * @param ... Input to the format string
 */
void mce_log_file(const loglevel_t loglevel, const char *const file,
		  const char *const function, const char *const fmt, ...)
{
	va_list args;

	va_start(args, fmt);

	if (logverbosity >= loglevel) {
		gchar *tmp;
		gchar *msg;

		g_vasprintf(&tmp, fmt, args);

		if ((file != NULL) && (function != NULL)) {
			msg = g_strconcat(file, ":",
					  function, "(): ",
					  tmp, NULL);
		} else {
			msg = g_strdup(tmp);
		}

		g_free(tmp);

		if (logtype == MCE_LOG_STDERR) {
			fprintf(stderr, "%s: %s\n", logname, msg);
		} else {
			int priority;

			switch (loglevel) {
			case LL_DEBUG:
				priority = LOG_DEBUG;
				break;

			case LL_ERR:
				priority = LOG_ERR;
				break;

			case LL_CRIT:
				priority = LOG_CRIT;
				break;

			case LL_INFO:
				priority = LOG_INFO;
				break;

			case LL_WARN:
			default:
				priority = LOG_WARNING;
				break;
			}

			syslog(priority, "%s", msg);
		}

		g_free(msg);
	}

	va_end(args);
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
	if (logname)
		g_free(logname);

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
