/**
 * @file battery-statefs.c
 * Battery module -- this implements battery and charger logic for MCE
 * <p>
 * Copyright (C) 2013-2015 Jolla Ltd.
 * <p>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * <p>
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
 *
 * <p>
 * Rough diagram of data/control flow within this module
 * @verbatim
 *
 *           .------.      .-------.
 *           |SFSCTL|      |statefs|
 *           `------'      `-------'
 *              |              |
 *           .-------.    .--------.
 *           |TRACKER|.---|INPUTSET|
 *           `-------'|.  `--------'
 *            `-------'|
 *             `-------'
 *                |
 *             .------.
 *             |SFSBAT|
 *             `------'
 *                |
 *             .------.
 *             |MCEBAT|
 *             `------'
 *                |
 *           .---------.
 *           |datapipes|
 *           `---------'
 * @endverbatim
 */

#include "../mce.h"
#include "../mce-log.h"

#include <sys/epoll.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <gmodule.h>

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** Delay between re-open attempts while statefs entries are missing; [ms] */
#define START_DELAY  (5 * 1000)

/** Delay from 1st property change to forced property re-read; [ms]
 *
 * HACK: Depending on kernel & fuse versions there are varying problems
 *       with epoll wakeups. It is possible that we get woken up, but
 *       do not receive events identifying the input file with changed
 *       content. To overcome this we schedule forced re-read of all
 *       battery properties if we get any kind of wakeup from epoll fd.
 */
#define REREAD_DELAY 250

/** Delay from 1st property change to state machine update; [ms] */
#define UPDATE_DELAY (REREAD_DELAY + 50)

/** Whether to support legacy pattery low led pattern; nonzero for yes */
#define SUPPORT_BATTERY_LOW_LED_PATTERN 0

/** Enumeration of possible statefs Battery.State property values */
typedef enum
{
    STATEFS_BATTERY_STATE_UNKNOWN      = -1,
    STATEFS_BATTERY_STATE_EMPTY        =  0,
    STATEFS_BATTERY_STATE_LOW          =  1,
    STATEFS_BATTERY_STATE_DISCHARGING  =  2,
    STATEFS_BATTERY_STATE_CHARGING     =  3,
    STATEFS_BATTERY_STATE_FULL         =  4,
} sfsbat_state_t;

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * MISC_UTILS
 * ------------------------------------------------------------------------- */

static bool parse_state (const char *data, sfsbat_state_t *res);
static bool parse_int   (const char *data, int *res);
static bool parse_bool  (const char *data, bool *res);

static const char *repr_state (sfsbat_state_t state);
static const char *repr_bool  (bool val);

/* ------------------------------------------------------------------------- *
 * DATAPIPE_HANDLERS
 * ------------------------------------------------------------------------- */

static void     bsf_datapipe_shutting_down_cb(gconstpointer data);

static void     bsf_datapipe_init(void);
static void     bsf_datapipe_quit(void);

/* ------------------------------------------------------------------------- *
 * INPUTSET  --  generic epoll set as glib io watch input listener
 * ------------------------------------------------------------------------- */

static bool     inputset_init    (bool (*input_cb)(struct epoll_event *, int));
static void     inputset_quit    (void);
static bool     inputset_insert  (int fd, void *data);
static void     inputset_remove  (int fd);

static gboolean inputset_watch_cb(GIOChannel *srce, GIOCondition cond,
                                  gpointer data);

/* ------------------------------------------------------------------------- *
 * SFSBAT  --  battery data as available from statefs
 * ------------------------------------------------------------------------- */

/** Battery properties available via statefs */
typedef struct
{
    /** Battery is: charging, discharging, empty or full */
    sfsbat_state_t State;

    /** Device is drawing power from battery */
    bool           OnBattery;

    /** Low battery condition */
    bool           LowBattery;

    /** Charge level percentage */
    int            ChargePercentage;
} sfsbat_t;

static void        sfsbat_init(void);

/* ------------------------------------------------------------------------- *
 * MCEBAT  --  battery data in form expected by mce statemachines
 * ------------------------------------------------------------------------- */

/** Battery properties in mce statemachine compatible form */
typedef struct
{
    /** Battery charge percentage; for use with battery_level_pipe */
    int             level;

    /** Battery FULL/OK/LOW/EMPTY; for use with battery_status_pipe */
    int             status;

    /** Charger connected; for use with charger_state_pipe */
    charger_state_t charger;
} mcebat_t;

static void     mcebat_init               (void);
static void     mcebat_update_from_sfsbat (void);
static gboolean mcebat_update_cb          (gpointer user_data);
static void     mcebat_update_cancel      (void);
static void     mcebat_update_schedule    (void);

/* ------------------------------------------------------------------------- *
 * TRACKER  --  binds statefs file to sfsbat_t member
 * ------------------------------------------------------------------------- */

typedef struct tracker_t tracker_t;

/** Bind statefs file to member of sfsbat_t structure */
struct tracker_t
{
    /** Basename of the input file */
    const char *name;

    /** Path to input file, set at tracker_init() / sfsctl_init() */
    char       *path;

    /** Pointer to a sfsbat_t member */
    void       *value;

    /** Value type specific input parser hook */
    bool      (*update_cb)(tracker_t *, const char *);

    /** File descriptor for the input file */
    int         fd;

    /** For use with debugging with pipes instead of real statefs */
    bool        seekable;
};

static const char *tracker_propdir(void);

static void tracker_init        (tracker_t *self);
static void tracker_quit        (tracker_t *self);
static bool tracker_open        (tracker_t *self, bool *warned);
static void tracker_close       (tracker_t *self);
static bool tracker_start       (tracker_t *self, bool *warned);
static bool tracker_read_data   (tracker_t *self, char *data, size_t size);
static bool tracker_parse_int   (tracker_t *self, const char *data);
static bool tracker_parse_bool  (tracker_t *self, const char *data);
static bool tracker_update      (tracker_t *self);

/* ------------------------------------------------------------------------- *
 * SFSCTL  --  controls for statefs tracking
 * ------------------------------------------------------------------------- */

static void     sfsctl_init             (void);
static void     sfsctl_quit             (void);

static void     sfsctl_start            (void);
static bool     sfsctl_start_try                (void);
static gboolean sfsctl_start_cb         (gpointer aptr);

static bool     sfsctl_watch_cb         (struct epoll_event *eve, int cnt);

static void     sfsctl_schedule_reread  (void);
static void     sfsctl_cancel_reread    (void);
static gboolean sfsctl_reread_cb        (gpointer aptr);

/* ------------------------------------------------------------------------- *
 * MODULE_INIT_EXIT
 * ------------------------------------------------------------------------- */

G_MODULE_EXPORT const gchar *g_module_check_init (GModule *module);
G_MODULE_EXPORT void         g_module_unload     (GModule *module);

/* ========================================================================= *
 * MISC_UTILS
 * ========================================================================= */

/** Lookup table for sfsbat_state_t parsing */
static const struct {
    /** State name visible in statefs file */
    const char     *name;

    /** Enumeration ID used in code */
    sfsbat_state_t  state;
} state_lut[] =
{
    { "charging",    STATEFS_BATTERY_STATE_CHARGING    },
    { "discharging", STATEFS_BATTERY_STATE_DISCHARGING },
    { "empty",       STATEFS_BATTERY_STATE_EMPTY       },
    { "low",         STATEFS_BATTERY_STATE_LOW         },
    { "full",        STATEFS_BATTERY_STATE_FULL        },
    { "unknown",     STATEFS_BATTERY_STATE_UNKNOWN     },
    { "",            STATEFS_BATTERY_STATE_UNKNOWN     },
};

/** String to sfsbat_state_t helper
 *
 * @param data text to parse
 * @param res  where to store parsed value
 *
 * @return true if data could be parse and stored to res, false otherwise
 */
static bool
parse_state(const char *data, sfsbat_state_t *res)
{
    static bool lut_miss_reported = false;

    for( size_t i = 0; ; ++i ) {
        if( i == G_N_ELEMENTS(state_lut) ) {
            /* Value was not found in the lookup table - handle
             * as if "unknown" had been reported */
            *res = STATEFS_BATTERY_STATE_UNKNOWN;

            /* Emit warning, but only once to avoid repetitive reporting
             * due to forced property updates */
            if( !lut_miss_reported ) {
                lut_miss_reported = true;
                mce_log(LL_WARN, "unrecognized Battery.State value '%s';"
                        " assuming battery state is not known", data);
            }
            break;
        }

        if( !strcmp(state_lut[i].name, data) ) {
            /* Use the state from the lookup table */
            *res = state_lut[i].state;

            /* Enable reporting of lookup table misses again */
            lut_miss_reported = false;
            break;
        }
    }

    return true;
}

/** String to int helper
 *
 * @param data text to parse
 * @param res  where to store parsed value
 *
 * @return true if data could be parse and stored to res, false otherwise
 */
static bool
parse_int(const char *data, int *res)
{
    bool  ack = true;
    char *pos = (char *)data;
    int   val = strtol(pos, &pos, 0);

    if( pos > data && *pos == 0 )
        *res = val;
    else
        ack = false;

    return ack;
}

/** String to bool helper
 *
 * @param data text to parse
 * @param res  where to store parsed value
 *
 * @return true if data could be parse and stored to res, false otherwise
 */
static bool
parse_bool(const char *data, bool *res)
{
    bool ack = true;
    int  val = 0;

    if( parse_int(data, &val) )
        *res = (val != 0);
    else if( !strcmp(data, "true") )
        *res = true;
    else if( !strcmp(data, "false") )
        *res = false;
    else
        ack = false;

    return ack;
}

/** sfsbat_state_t to string helper
 *
 * @param state sfsbat_state_t enum value
 *
 * @return human readable state name
 */
static const char *
repr_state(sfsbat_state_t state)
{
    const char *res = "unknown";

    for( size_t i = 0; i < G_N_ELEMENTS(state_lut); ++i ) {
        if( state_lut[i].state != state )
            continue;

        res = state_lut[i].name;
        break;
    }

    return res;
}

/** Boolean to string helper
 *
 * @param val boolean value
 *
 * @return "true" or "false" depending on val
 */
static const char *
repr_bool(bool val)
{
    return val ? "true" : "false";
}

/* ========================================================================= *
 * DATAPIPE_HANDLERS
 * ========================================================================= */

/** Device is shutting down; assume false */
static bool shutting_down = false;

/** Change notifications for shutting_down
 */
static void bsf_datapipe_shutting_down_cb(gconstpointer data)
{
    bool prev = shutting_down;
    shutting_down = GPOINTER_TO_INT(data);

    if( shutting_down == prev )
        goto EXIT;

    mce_log(LL_DEBUG, "shutting_down = %d -> %d",
            prev, shutting_down);

    /* Loss of statefs files is expected during shutdown */

EXIT:
    return;
}

/** Array of datapipe handlers */
static datapipe_handler_t bsf_datapipe_handlers[] =
{
    // output triggers
    {
        .datapipe  = &shutting_down_pipe,
        .output_cb = bsf_datapipe_shutting_down_cb,
    },

    // sentinel
    {
        .datapipe = 0,
    }
};

static datapipe_bindings_t bsf_datapipe_bindings =
{
    .module   = "battery_statefs",
    .handlers = bsf_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void bsf_datapipe_init(void)
{
    mce_datapipe_init_bindings(&bsf_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void bsf_datapipe_quit(void)
{
    mce_datapipe_quit_bindings(&bsf_datapipe_bindings);
}

/* ========================================================================= *
 * INPUTSET
 * ========================================================================= */

/** epoll fd for tracking a set of input files */
static int inputset_epoll_fd = -1;

/** glib io watch for inputset_epoll_fd */
static guint inputset_watch_id = 0;

/** Handle statefs change notifications received via epoll set
 *
 * @param srce (not used)
 * @param cond wakeup reason
 * @param data event handler function as void pointer
 *
 * @return FALSE if the io watch must be disabled, TRUE otherwise
 */
static gboolean
inputset_watch_cb(GIOChannel *srce, GIOCondition cond, gpointer data)
{
    (void)srce;

    gboolean keep_going = TRUE;

    struct epoll_event eve[16];

    if( cond & ~G_IO_IN ) {
        mce_log(LL_ERR, "unexpected io cond: 0x%x", (unsigned)cond);
        keep_going = FALSE;
    }

    int rc = epoll_wait(inputset_epoll_fd, eve, G_N_ELEMENTS(eve), 0);

    if( rc == -1 ) {
        switch( errno ) {
        case EINTR:
        case EAGAIN:
            break;

        default:
            mce_log(LL_ERR, "statfs io wait: %m");
            keep_going = FALSE;
        }
        goto cleanup;
    }

    bool (*input_cb)(struct epoll_event *, int) = data;

    if( !input_cb(eve, rc) )
        keep_going = FALSE;

cleanup:

    if( !keep_going ) {
        mce_log(LL_CRIT, "disabling statfs io watch");
        inputset_watch_id = 0;
    }

    return keep_going;
}

/** Initialize epoll set and io watch for it
 *
 * @param input_cb event handler function
 *
 * @return true on success, false on failure
 */
static bool
inputset_init(bool (*input_cb)(struct epoll_event *, int))
{
    bool success = false;
    GIOChannel *chn = 0;

    if( (inputset_epoll_fd = epoll_create1(EPOLL_CLOEXEC)) == -1 ) {
        mce_log(LL_WARN, "epoll_create: %m");
        goto cleanup;
    }

    if( !(chn = g_io_channel_unix_new(inputset_epoll_fd)) )
        goto cleanup;

    g_io_channel_set_close_on_unref(chn, FALSE);

    inputset_watch_id = g_io_add_watch(chn, G_IO_IN,
                                       inputset_watch_cb, input_cb);
    if( !inputset_watch_id )
        goto cleanup;

    success = true;

cleanup:

    if( chn )
        g_io_channel_unref(chn);

    if( !success )
        inputset_quit();

    return success;
}

/** Remove epoll set and io watch for it
 */
static void
inputset_quit(void)
{
    if( inputset_watch_id )
        g_source_remove(inputset_watch_id), inputset_watch_id = 0;

    if( inputset_epoll_fd != -1 )
        close(inputset_epoll_fd), inputset_epoll_fd = -1;
}

/** Add tracking object to epoll set
 *
 * @param fd   input file descriptor
 * @param data data to associate with the fd
 *
 * @return true on success, false on failure
 */
static bool
inputset_insert(int fd, void *data)
{
    bool success = false;

    struct epoll_event eve;

    if( fd == -1 )
        goto cleanup;

    memset(&eve, 0, sizeof eve);
    eve.events = EPOLLIN;
    eve.data.ptr = data;

    int rc = epoll_ctl(inputset_epoll_fd, EPOLL_CTL_ADD, fd, &eve);

    if( rc == -1 ) {
        mce_log(LL_WARN, "EPOLL_CTL_ADD(%d): %m", fd);
        goto cleanup;
    }

    success = true;

cleanup:

    return success;
}

/** Remove tracking object from epoll set
 *
 * @param fd   input file descriptor
 */
static void
inputset_remove(int fd)
{
    if( fd == -1 )
        goto cleanup;

    if( epoll_ctl(inputset_epoll_fd, EPOLL_CTL_DEL, fd, 0) == -1 )
        mce_log(LL_WARN, "EPOLL_CTL_DEL(%d): %m", fd);

cleanup:
    return;
}

/* ========================================================================= *
 * SFSBAT
 * ========================================================================= */

/** Battery status, as available via statefs */
static sfsbat_t sfsbat;

/** Provide intial guess of statefs battery status
 */
static void
sfsbat_init(void)
{
    memset(&sfsbat, 0, sizeof sfsbat);

    sfsbat.State            = STATEFS_BATTERY_STATE_UNKNOWN;
    sfsbat.OnBattery        = true;
    sfsbat.LowBattery       = false;
    sfsbat.ChargePercentage = 50;
}

/* ========================================================================= *
 * MCEBAT
 * ========================================================================= */

/** Timer for processing battery status changes */
static guint mcebat_update_id = 0;

/** Current battery status in mce legacy compatible form */
static mcebat_t mcebat;

/** Provide intial guess of mce battery status
 */
static void
mcebat_init(void)
{
    memset(&mcebat, 0, sizeof mcebat);

    mcebat.level   = 50;
    mcebat.status  = BATTERY_STATUS_UNDEF;
    mcebat.charger = CHARGER_STATE_UNDEF;
}

/** Update mce battery status from statefs battery data
 */
static void
mcebat_update_from_sfsbat(void)
{
    mcebat.level = sfsbat.ChargePercentage;

    switch( sfsbat.State ) {
    case STATEFS_BATTERY_STATE_EMPTY:
        mcebat.status = BATTERY_STATUS_EMPTY;
        break;

    case STATEFS_BATTERY_STATE_LOW:
        mcebat.status = BATTERY_STATUS_LOW;
        break;

    case STATEFS_BATTERY_STATE_DISCHARGING:
        if( sfsbat.LowBattery )
            mcebat.status = BATTERY_STATUS_LOW;
        else
            mcebat.status = BATTERY_STATUS_OK;
        break;

    case STATEFS_BATTERY_STATE_CHARGING:
        mcebat.status = BATTERY_STATUS_OK;
        break;

    case STATEFS_BATTERY_STATE_FULL:
        mcebat.status = BATTERY_STATUS_FULL;
        break;

    default:
    case STATEFS_BATTERY_STATE_UNKNOWN:
        mcebat.status = BATTERY_STATUS_UNDEF;
        break;
    }

    if( sfsbat.OnBattery )
        mcebat.charger = CHARGER_STATE_OFF;
    else
        mcebat.charger = CHARGER_STATE_ON;
}

/** Process accumulated statefs battery status changes
 *
 * @param user_data (not used)
 *
 * @return FALSE (to stop timer from repeating)
 */
static gboolean
mcebat_update_cb(gpointer user_data)
{
    (void)user_data;

    if( !mcebat_update_id )
        goto cleanup;

    mcebat_update_id = 0;

    mce_log(LL_DEBUG, "update datapipes");

    /* Get a copy of current status */
    mcebat_t prev = mcebat;

    /* Update from statefs based information */
    mcebat_update_from_sfsbat();

    /* Process changes */
    if( mcebat.charger != prev.charger ) {
        mce_log(LL_NOTICE, "charger: %s -> %s",
                charger_state_repr(prev.charger),
                charger_state_repr(mcebat.charger));

        /* Charger connected state */
        datapipe_exec_full(&charger_state_pipe, GINT_TO_POINTER(mcebat.charger),
                           DATAPIPE_CACHE_INDATA);

        /* Charging led pattern */
        if( mcebat.charger == CHARGER_STATE_ON ) {
            datapipe_exec_output_triggers(&led_pattern_activate_pipe,
                                          MCE_LED_PATTERN_BATTERY_CHARGING);
        }
        else {
            datapipe_exec_output_triggers(&led_pattern_deactivate_pipe,
                                          MCE_LED_PATTERN_BATTERY_CHARGING);
        }

        /* Generate activity */
        datapipe_exec_full(&inactivity_event_pipe, GINT_TO_POINTER(FALSE),
                           DATAPIPE_CACHE_OUTDATA);
    }

    if( mcebat.status != prev.status ) {
        mce_log(LL_NOTICE, "status: %s -> %s",
                battery_status_repr(prev.status),
                battery_status_repr(mcebat.status));

        /* Battery full led pattern */
        if( mcebat.status == BATTERY_STATUS_FULL ) {
            datapipe_exec_output_triggers(&led_pattern_activate_pipe,
                                          MCE_LED_PATTERN_BATTERY_FULL);
        }
        else {
            datapipe_exec_output_triggers(&led_pattern_deactivate_pipe,
                                          MCE_LED_PATTERN_BATTERY_FULL);
        }

#if SUPPORT_BATTERY_LOW_LED_PATTERN
        /* Battery low led pattern */
        if( mcebat.status == BATTERY_STATUS_LOW ||
            mcebat.status == BATTERY_STATUS_EMPTY ) {
            datapipe_exec_output_triggers(&led_pattern_activate_pipe,
                                          MCE_LED_PATTERN_BATTERY_LOW);
        }
        else {
            datapipe_exec_output_triggers(&led_pattern_deactivate_pipe,
                                          MCE_LED_PATTERN_BATTERY_LOW);
        }
#endif /* SUPPORT_BATTERY_LOW_LED_PATTERN */

        /* Battery charge state */
        datapipe_exec_full(&battery_status_pipe,
                           GINT_TO_POINTER(mcebat.status),
                           DATAPIPE_CACHE_INDATA);

    }

    if( mcebat.level != prev.level ) {
        mce_log(LL_NOTICE, "level: %d -> %d", prev.level, mcebat.level);

        /* Battery charge percentage */
        datapipe_exec_full(&battery_level_pipe,
                           GINT_TO_POINTER(mcebat.level),
                           DATAPIPE_CACHE_INDATA);
    }

cleanup:
    return FALSE;
}

/** Cancel processing of statefs battery status changes
 */
static void
mcebat_update_cancel(void)
{
    if( mcebat_update_id )
        g_source_remove(mcebat_update_id), mcebat_update_id = 0;
}

/** Initiate delayed processing of statefs battery status changes
 */
static void
mcebat_update_schedule(void)
{
    if( !mcebat_update_id )
        mcebat_update_id = g_timeout_add(UPDATE_DELAY, mcebat_update_cb, 0);
}

/* ========================================================================= *
 * TRACKER
 * ========================================================================= */

/** Locate directory where battery properties are
 */
static const char *
tracker_propdir(void)
{
    // TODO: use statefs helper function to get the path
    static const char def[] = "/run/state/namespaces/Battery";

    static char *res = 0;

    if( !res ) {
        // TODO: debug stuff, remove later
        const char *env = getenv("BATTERY_BASEDIR");
        res = strdup(env ?: def);
    }
    return res;
}

/** Read string from statefs input file
 *
 * @param self statefs input file tracking object
 * @param data buffer where to read to
 * @param size length of the buffer
 *
 * @return true on success, or false in case of errors
 */
static bool
tracker_read_data(tracker_t *self, char *data, size_t size)
{
    bool  res = false;
    int   rc;

    if( self->fd == -1 )
        goto cleanup;

    /* Read the state data */
    if( (rc = read(self->fd, data, size-1)) == -1 ) {
        mce_log(LL_WARN, "%s: read: %m", self->path);
        goto cleanup;
    }

    /* Rewind to the start of the file */
    if( self->seekable && lseek(self->fd, 0, SEEK_SET) == -1 ) {
        mce_log(LL_WARN, "%s: rewind: %m", self->path);
        goto cleanup;
    }

    data[rc] = 0, res = true;

    data[strcspn(data, "\r\n")] = 0;

cleanup:

    return res;
}

/** Parse statefs file content to int value
 *
 * @param self statefs input file tracking object
 * @param data string to parse
 *
 * @return true if data could be read and the value changed, false otherwise
 */
static bool
tracker_parse_int(tracker_t *self, const char *data)
{
    bool  ack = false;
    int  *now = self->value;
    int   zen = *now;

    if( !parse_int(data, &zen) ) {
        mce_log(LL_WARN, "%s: can't convert '%s' to int", self->name, data);
        goto cleanup;
    }

    if( *now == zen )
        goto cleanup;

    mce_log(LL_INFO, "%s: %d -> %d", self->name, *now, zen);

    *now = zen, ack = true;

cleanup:
    return ack;
}

/** Parse statefs file content to bool value
 *
 * @param self statefs input file tracking object
 * @param data string to parse
 *
 * @return true if data could be read and the value changed, false otherwise
 */
static bool
tracker_parse_bool(tracker_t *self, const char *data)
{
    bool  ack = false;
    bool *now = self->value;
    bool  zen = *now;

    if( !parse_bool(data, &zen) ) {
        mce_log(LL_WARN, "%s: can't convert '%s' to bool", self->name, data);
        goto cleanup;
    }

    if( *now == zen )
        goto cleanup;

    mce_log(LL_INFO, "%s: %s -> %s", self->name,
            repr_bool(*now), repr_bool(zen));

    *now = zen, ack = true;

cleanup:
    return ack;
}

/** Parse statefs file content to battery state enum
 *
 * @param self statefs input file tracking object
 * @param data string to parse
 *
 * @return true if data could be read and the value changed, false otherwise
 */
static bool
tracker_parse_state(tracker_t *self, const char *data)
{
    bool            ack = false;
    sfsbat_state_t *now = self->value;
    sfsbat_state_t  zen = *now;

    if( !parse_state(data, &zen) ) {
        mce_log(LL_WARN, "%s: can't convert '%s' to battery state",
                self->name, data);
        goto cleanup;
    }

    if( *now == zen )
        goto cleanup;

    mce_log(LL_INFO, "%s: %s -> %s", self->name,
            repr_state(*now), repr_state(zen));

    *now = zen, ack = true;

cleanup:
    return ack;
}

/** Update value from statefs content and schedule state machine update
 *
 * @param self statefs input file tracking object
 *
 * @return true if io was successfull, but the value did not change;
 *         false otherwise
 */
static bool
tracker_update(tracker_t *self)
{
    bool dummy = false; // assume: io failed or value changed

    char data[64];

    if( !tracker_read_data(self, data, sizeof data) ) {
        tracker_close(self);
        goto cleanup;
    }

    if( self->update_cb(self, data) )
        mcebat_update_schedule();
    else
        dummy = true; // io succeesfull, but value did not change

cleanup:

    return dummy;
}

/** Open statefs file
 *
 * @param self   statefs input file tracking object
 * @param warned pointer to flag holding already-warned status
 *
 * @return true if file is open, false otherwise
 */
static bool
tracker_open(tracker_t *self, bool *warned)
{
    bool success = false;

    if( self->fd != -1 )
        goto cleanup_success;

    self->seekable = false;

    self->fd = open(self->path, O_RDONLY | O_DIRECT);
    if( self->fd == -1 ) {
        /* On shutdown it is expected that statefs files
         * become unaccessible. And to reduce journal
         * spamming on statefs restart, log only the
         * first file in the set that we fail to open.
         */

        int level = LL_WARN;

        if( shutting_down || *warned )
            level = LL_DEBUG;
        else
            *warned = true;

        mce_log(level, "%s: open: %m", self->path);

        goto cleanup_failure;
    }

    if( lseek(self->fd, 0, SEEK_CUR) == -1 )
        mce_log(LL_WARN, "%s: is not seekable", self->path);
    else
        self->seekable = true;

cleanup_success:
    mce_log(LL_DEBUG, "%s: opened", self->name);
    success = true;

cleanup_failure:
    return success;
}

/** Close statefs file
 *
 * @param self statefs input file tracking object
 */
static void
tracker_close(tracker_t *self)
{
    if( self->fd != -1 ) {
        mce_log(LL_DEBUG, "%s: closing", self->name);
        inputset_remove(self->fd);
        close(self->fd), self->fd = -1;
    }
}

/** Initialize tracker_t dynamic data
 *
 * @param self statefs input file tracking object
 */
static void
tracker_init(tracker_t *self)
{
    self->path = g_strdup_printf("%s/%s", tracker_propdir(), self->name);
}

/** Release dynamic resources associated with tracker_t
 *
 * @param self statefs input file tracking object
 */
static void
tracker_quit(tracker_t *self)
{
    tracker_close(self);

    free(self->path), self->path = 0;
}

/** Start tracking statefs property file
 *
 * @param self   statefs input file tracking object
 * @param warned pointer to flag holding already-warned status
 *
 * @return true if statefs file is open and tracked, false otherwise
 */
static bool
tracker_start(tracker_t *self, bool *warned)
{
    bool success = false;

    if( self->fd != -1 )
        goto cleanup_success;

    if( !tracker_open(self, warned) )
        goto cleanup_failure;

    tracker_update(self);

    if( !inputset_insert(self->fd, self) ) {
        tracker_close(self);
        goto cleanup_failure;
    }

cleanup_success:
    success = true;

cleanup_failure:
    return success;
}

/* ========================================================================= *
 * SFSCTL
 * ========================================================================= */

/** Initializer macro for int properties */
#define INIT_PROP_INT(NAME)\
     {\
         .name      = #NAME,\
         .path      = 0,\
         .value     = &sfsbat.NAME,\
         .update_cb = tracker_parse_int,\
         .fd        = -1,\
     }

/** Initializer macro for bool properties */
#define INIT_PROP_BOOL(NAME)\
     {\
        .name      = #NAME,\
        .path      = 0,\
        .value     = &sfsbat.NAME,\
        .update_cb = tracker_parse_bool,\
        .fd        = -1,\
    }

/** Lookup table for statefs based properties */
static tracker_t sfsctl_props[] =
{
    {
        .name      = "State",
        .path      = 0,
        .value     = &sfsbat.State,
        .update_cb = tracker_parse_state,
        .fd        = -1,
     },

    INIT_PROP_BOOL(OnBattery),
    INIT_PROP_BOOL(LowBattery),

    INIT_PROP_INT(ChargePercentage),

    // sentinel
    { .name = 0, }
};

/** timeout for waiting statefs to come available */
static guint sfsctl_start_id = 0;

/** timeout for handling missed epoll io notifications */
static guint sfsctl_reread_id = 0;

/** Initialize dynamic data for statefs tracking objects
 */
static void
sfsctl_init(void)
{
    for( tracker_t *prop = sfsctl_props; prop->name; ++prop )
        tracker_init(prop);
}

/** Stop statefs change tracking
 */
static void
sfsctl_quit(void)
{
    if( sfsctl_start_id )
        g_source_remove(sfsctl_start_id), sfsctl_start_id = 0;

    for( tracker_t *prop = sfsctl_props; prop->name; ++prop )
        tracker_quit(prop);
}

/** Helper for starting/restarting statefs change tracking
 *
 * @return true if all properties could be bound to statefs files
 */
static bool
sfsctl_start_try(void)
{
    bool success = true;
    bool warned  = false;

    mce_log(LL_NOTICE, "probe statefs files");

    for( tracker_t *prop = sfsctl_props; prop->name; ++prop ) {
        if( !tracker_start(prop, &warned) )
            success = false;
    }

    return success;
}

/** Timeout for retrying start of statefs change tracking
 *
 * @return FALSE on success (to stop the timer), or
 *         TRUE on failure (to keep timer repeating)
 */
static gboolean
sfsctl_start_cb(gpointer aptr)
{
    (void)aptr;

    gboolean keep_going = FALSE;

    if( !sfsctl_start_id )
        goto cleanup;

    if( !sfsctl_start_try() )
        keep_going = TRUE;
    else
        sfsctl_start_id = 0;

cleanup:
    return keep_going;
}

/** Start statefs change tracking
 *
 * If all properties are not available immediately, retry
 * timer will be started
 */
static void
sfsctl_start(void)
{
    /* Retry timer already active ? */
    if( sfsctl_start_id )
        goto cleanup;

    /* Attempt to start file tracking */
    if( sfsctl_start_try() )
        goto cleanup;

    /* Re-try again later */
    sfsctl_start_id = g_timeout_add(START_DELAY, sfsctl_start_cb, 0);

cleanup:
    return;
}

/** Handle statefs change notifications received via epoll set
 *
 * @param eve array of epoll events
 * @param cnt number of epoll events
 *
 * @return false if io watch should be disabled, otherwise true
 */
static bool
sfsctl_watch_cb(struct epoll_event *eve, int cnt)
{
    bool keep_going   = true;
    bool statefs_lost = false;

    mce_log(LL_DEBUG, "process %d statefs changes", cnt);

    for( int i = 0; i < cnt; ++i ) {
        tracker_t *prop = eve[i].data.ptr;

        if( eve[i].events & ~EPOLLIN )
            tracker_close(prop), statefs_lost = true;
        else
            tracker_update(prop);
    }

    /* HACK: Force all props to be reread before datapipe updates */
    sfsctl_schedule_reread();

    if( statefs_lost ) {
        /* ASSUME: Loss of inputs == statefs restart */

        /* Start timer based re-open attempts */
        sfsctl_start();

        /* Forced re-read makes no sense, cancel it */
        sfsctl_cancel_reread();
    }

    return keep_going;
}

/** Timeout for forced re-read of statefs properties
 *
 * @param aptr (not used)
 *
 * @return FALSE (to stop the timer from repeating)
 */
static gboolean
sfsctl_reread_cb(gpointer aptr)
{
    (void)aptr;

    if( !sfsctl_reread_id )
        goto cleanup;

    sfsctl_reread_id = 0;

    mce_log(LL_DEBUG, "forced update of all states files");

    for( tracker_t *prop = sfsctl_props; prop->name; ++prop )
        tracker_update(prop);

cleanup:
    return FALSE;
}

/** Cancel forced re-read of statefs properties
 */
static void
sfsctl_cancel_reread(void)
{
    if( sfsctl_reread_id )
        g_source_remove(sfsctl_reread_id), sfsctl_reread_id = 0;
}

/** Schedule forced re-read of statefs properties
 */
static void
sfsctl_schedule_reread(void)
{
    if( !sfsctl_reread_id )
        sfsctl_reread_id = g_timeout_add(REREAD_DELAY, sfsctl_reread_cb, 0);
}

/** Stop battery/charging tracking
 */
static void
battery_quit(void)
{
    /* stop statefs change tracking */
    sfsctl_quit();

    /* cancel pending state machine updates */
    mcebat_update_cancel();

    /* cancel pending property re-reads */
    sfsctl_cancel_reread();

    /* remove epoll input listener */
    inputset_quit();
}

/** Start battery/charging tracking
 *
 * @return true on success, false otherwise
 */
static bool
battery_init(void)
{
    bool success = false;

    /* initialize epoll input listener */
    if( !inputset_init(sfsctl_watch_cb) )
        goto cleanup;

    /* reset data used by the state machine */
    mcebat_init();

    /* reset data that should come from statefs */
    sfsbat_init();

    /* initialize statefs paths etc */
    sfsctl_init();

    /* start statefs change tracking */
    sfsctl_start();

    success = true;

cleanup:

    return success;
}

/* ========================================================================= *
 * MODULE_INIT_EXIT
 * ========================================================================= */

/** Module name */
#define MODULE_NAME "battery_statefs"

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info =
{
    /** Name of the module */
    .name = MODULE_NAME,
    /** Module provides */
    .provides = provides,
    /** Module priority */
    .priority = 100
};

/** Init function for the battery and charger module
 *
 * @param module (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *g_module_check_init(GModule *module)
{
    (void)module;

    bsf_datapipe_init();

    if( !battery_init() )
        mce_log(LL_WARN, MODULE_NAME" module initialization failed");
    else
        mce_log(LL_INFO, MODULE_NAME" module initialized ");

    return NULL;
}

/** Exit function for the battery and charger module
 *
 * @param module (not used)
 */
void g_module_unload(GModule *module)
{
    (void)module;

    bsf_datapipe_quit();

    battery_quit();
}
