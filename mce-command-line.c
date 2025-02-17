/**
 * @file mce-command-line.c
 *
 * Command line parameter parsing module for the Mode Control Entity
 * <p>
 * Copyright (c) 2014 Jolla Ltd.
 * Copyright (c) 2025 Jolla Mobile Ltd
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

#include "mce-command-line.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** Maximum number of short options supported; A-Z a-z 0-9 */
#define FLAGS_MAX ('z' - 'a' + 1 + 'Z' - 'A' + 1 + '9' - '0' + 1)

/* ========================================================================= *
 * LOCAL FUNCTIONS
 * ========================================================================= */

// mce_opt_xxx
static bool   mce_opt_handle(const mce_opt_t *self, char *optarg);
static void   mce_opt_show(const mce_opt_t *self);

// mce_options_xxx
static size_t mce_options_get_count(const mce_opt_t *opts);
static int    mce_options_find_by_flag(const mce_opt_t *opts, int val);
static void   mce_options_reflow_lines(const char *text);
static void   mce_options_emit_long_help(const mce_opt_t *opts, const char *arg);
static void   mce_options_emit_short_help(const mce_opt_t *opts);
static void   mce_options_sanity_check(const mce_opt_t *opts);

/* ========================================================================= *
 * mce_opt_xxx
 * ========================================================================= */

static bool
mce_opt_handle(const mce_opt_t *self, char *arg)
{
    if( !arg ) {
        if( self->without_arg )
            return self->without_arg(NULL);
    }
    else {
        if( self->with_arg )
            return self->with_arg(arg);
    }
    return false;
}

static void
mce_opt_show(const mce_opt_t *self)
{
    if( self->flag )
        printf("  -%c,", self->flag);
    else
        printf("     ");

    printf(" --%s", self->name);
    if( self->with_arg ) {
        if( self->without_arg )
            putchar('[');
        printf("=<%s>", self->values ?: "???");
        if( self->without_arg )
            putchar(']');
    }

    printf("\n");
}

/* ========================================================================= *
 * mce_options_xxx
 * ========================================================================= */

static size_t
mce_options_get_count(const mce_opt_t *opts)
{
    size_t count = 0;

    while( opts[count].name ) ++count;

    return count;
}

static int
mce_options_find_by_flag(const mce_opt_t *opts, int val)
{
    for( size_t i = 0; ; ++i ) {
        if( !opts[i].name )
            return -1;

        if( opts[i].flag == val )
            return (int)i;
    }
}

static void
mce_options_reflow_lines(const char *text)
{
    if( !text )
        goto EXIT;

    for( ;; ) {
        size_t n = strcspn(text, "\n");
        printf("\t%.*s\n", (int)n, text);
        if( *(text += n) )
            ++text;
        if( !*text )
            break;
    }
    printf("\n");

EXIT:
    return;
}

static void
mce_options_emit_long_help(const mce_opt_t *opts, const char *arg)
{
    if( arg ) {
        while( *arg == '-' )
            ++arg;

        if( !*arg || !strcmp(arg, "all") )
            arg = 0;
    }

    for( const mce_opt_t *opt = opts; opt->name; ++opt ) {
        if( arg && !strstr(opt->name, arg) )
            continue;

        mce_opt_show(opt);
        mce_options_reflow_lines(opt->usage);
    }
}

static void
mce_options_emit_long_help_keys(const mce_opt_t *opts, char **keys)
{
    for( const mce_opt_t *opt = opts; opt->name; ++opt ) {
        for( size_t i = 0; keys[i]; ++i ) {
            if( !strstr(opt->name, keys[i]) )
                continue;
            mce_opt_show(opt);
            mce_options_reflow_lines(opt->usage);
            break;
        }
    }
}

static void
mce_options_emit_short_help(const mce_opt_t *opts)
{
    for( const mce_opt_t *opt = opts; opt->name; ++opt )
        mce_opt_show(opt);
}

static void
mce_options_sanity_check(const mce_opt_t *opts)
{
    for( const mce_opt_t *first = opts; first->name; ++first ) {
        for( const mce_opt_t *later = first + 1; later->name; ++later ) {
            if( !strcmp(first->name, later->name) ) {
                fprintf(stderr, "duplicate long option '%s'\n", first->name);
                exit(EXIT_FAILURE);
            }
            if( first->flag && first->flag == later->flag ) {
                fprintf(stderr, "duplicate short option '%c'\n", first->flag);
                exit(EXIT_FAILURE);
            }
        }
    }
}

/* ========================================================================= *
 * mce_command_line_xxx
 * ========================================================================= */

void
mce_command_line_usage_keys(const mce_opt_t *opts, char **keys)
{
    if( keys && *keys )
        mce_options_emit_long_help_keys(opts, keys);
}

/** Print usage information from provided look up table
 *
 * Helper function for implementing -h / --help handler.
 *
 * If NULL arg is passed, will print short list of supported options
 * and arguments that can be passed.
 *
 * If non NULL arg is passed will print full usage information for options
 * that have arg as substring of name property.
 *
 * As a special case passing "" or "all" will print full information for
 * all options.
 *
 * @param opts array of command line option information
 * @param arg  option argument from command line
 */
void
mce_command_line_usage(const mce_opt_t *opts, const char *arg)
{
    if( arg )
        mce_options_emit_long_help(opts, arg);
    else
        mce_options_emit_short_help(opts);
}

/** Parse command line options using the provided look up table
 *
 * Actual parsing happens via standard getopt_long() function. The
 * arrays needed by it are constructed from opts array given as input.
 *
 * If both with_arg and witout_arg callbacks are defined, providing
 * option argument is optional.
 *
 * Since getopt_long() is used, non-option arguments can be processed
 * after mce_command_line_parse() returns from argv[optind ... argc-1].
 *
 * @return true on success, false in case of errors
 */
bool
mce_command_line_parse(const mce_opt_t *opts, int argc, char **argv)
{
    bool           success = false;
    size_t         opt_cnt = mce_options_get_count(opts);
    struct option *opt_arr = calloc(opt_cnt + 1, sizeof *opt_arr);
    size_t         flg_cnt = 0;
    char           flg_arr[FLAGS_MAX * 3 + 1];

    /* Check that option look up table does not contain duplicates etc */

    mce_options_sanity_check(opts);

    /* Generate getopt_long() compatible look up tables */

    for( size_t i = 0; i < opt_cnt; ++i ) {
        const mce_opt_t *opt = opts + i;
        struct option   *std = opt_arr + i;

        /* Initialize long option entry to sane defaults */
        std->name    = opt->name;
        std->flag    = 0;
        std->val     = 0;
        std->has_arg = no_argument;

        /* Decide between no / optional / required arg */
        if( opt->with_arg ) {
            if( opt->without_arg )
                std->has_arg = optional_argument;
            else
                std->has_arg = required_argument;
        }

        /* Fill in short option array too */
        if( opt->flag ) {
            flg_arr[flg_cnt++] = (char)opt->flag;
            switch( std->has_arg ) {
            case optional_argument: flg_arr[flg_cnt++] = ':'; // fall through
            case required_argument: flg_arr[flg_cnt++] = ':'; // fall through
            default:                break;
            }
        }
    }
    opt_arr[opt_cnt].name = 0;
    flg_arr[flg_cnt] = 0;

    /* Do the actual command line argument handling with getopt_long() */

    for( ;; ) {
        int id = -1;
        int rc = getopt_long(argc, argv, flg_arr, opt_arr, &id);

        if( rc == -1 )
            break;

        if( rc == '?' )
            goto EXIT;

        if( rc != 0 )
            id = mce_options_find_by_flag(opts, rc);

        if( id == -1 )
            goto EXIT;

        if( !mce_opt_handle(opts + id, optarg) )
            goto EXIT;
    }

    success = true;

EXIT:
    free(opt_arr);

    return success;
}
