/**
 * @file mce-command-line.h
 *
 * Command line parameter parsing module for the Mode Control Entity
 * <p>
 * Copyright © 2014 Jolla Ltd.
 * <p>
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

#ifndef MCE_COMMAND_LINE_H_
# define MCE_COMMAND_LINE_H_

# include <stdbool.h>

# ifdef __cplusplus
extern "C" {
# elif 0
} /* fool JED indentation ... */
# endif

/* ========================================================================= *
 * TYPES
 * ========================================================================= */

typedef struct mce_opt_t mce_opt_t;

/** Option handler callback
 *
 * Both with_arg and without_arg callbacks use this type. This allows
 * the same callback function to be used for handling both. The arg
 * is NULL only when without_arg handler is called.
 *
 * @param arg option arg or NULL
 *
 * @return false to stop command line parsing and return error from
 *         mce_command_line_parse(), or true to keep going
 */
typedef bool (*mce_opt_parser_fn)(const char *arg);

/** Information about command line option
 *
 * If both with_arg and witout_arg callbacks are defined, providing
 * option argument is optional.
 */
struct mce_opt_t
{
    /** Long option name; NULL for sentinel element */
    const char *name;

    /** Short option flag character; 0 for none */
    int         flag;

    /** Description text for option argument; NULL if not used */
    const char *values;

    /** Usage information text for the option */
    const char *usage;

    /** Callback to use when option argument is provided */
    mce_opt_parser_fn with_arg;

    /** Callback to use when no option argument is provided */
    mce_opt_parser_fn without_arg;
};

/* ========================================================================= *
 * INTERFACE FUNCTIONS
 * ========================================================================= */

void mce_command_line_usage(const mce_opt_t *opts, const char *arg);
void mce_command_line_usage_keys(const mce_opt_t *opts, char **keys);
bool mce_command_line_parse(const mce_opt_t *opts, int argc, char **argv);

# ifdef __cplusplus
};
# endif

#endif /* MCE_COMMAND_LINE_H_ */
