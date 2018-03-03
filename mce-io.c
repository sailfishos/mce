/**
 * @file mce-io.c
 * Generic I/O functionality for the Mode Control Entity
 * <p>
 * Copyright © 2006-2011 Nokia Corporation and/or its subsidiary(-ies).
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

#include "mce-io.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-lib.h"

#ifdef ENABLE_WAKELOCKS
# include "libwakelock.h"
#endif

#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <glib/gstdio.h>

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** Suffix used for temporary files */
#define TMP_SUFFIX				".tmp"

/* ========================================================================= *
 * TYPES
 * ========================================================================= */

/** I/O monitor type */
typedef enum {
	IOMON_UNSET  = -1,		/**< I/O monitor type unset */
	IOMON_STRING =  0,		/**< String I/O monitor */
	IOMON_CHUNK  =  1,		/**< Chunk I/O monitor */
} iomon_type;

/** I/O monitor structure */
struct mce_io_mon_t {
	gchar          *path;		/**< Monitored file */
	iomon_type      type;		/**< Monitor type */
	gulong          chunk_size;	/**< Read-chunk size */

	gboolean        seekable;	/**< is the I/O channel seekable */
	gboolean        suspended;	/**< Is the I/O monitor suspended? */

	GIOChannel     *iochan;		/**< I/O channel */
	guint           iowatch_id;	/**< GSource ID for input */

	mce_io_mon_notify_cb nofity_cb;	/**< Input handling callback */
	mce_io_mon_delete_cb delete_cb;	/**< Iomon delete callback */

	error_policy_t  error_policy;	/**< Error policy */
	gboolean        rewind_policy;	/**< Rewind policy */

	void              *user_data;   /**< Attached user data block */
	mce_io_mon_free_cb user_free_cb;/**< Callback for freeing user_data */
};

/* ========================================================================= *
 * STATE_DATA
 * ========================================================================= */

/** List of all file monitors */
static GSList *file_monitors = NULL;

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

// SUSPEND_DETECTION

static void    io_detect_resume  (void);

// GLIB_IO_HELPERS

static const char *io_condition_repr (GIOCondition cond);
static const char *io_status_name    (GIOStatus io_status);

// IO_MONITOR

static mce_io_mon_t *mce_io_mon_create                  (const char *path, mce_io_mon_delete_cb delete_cb);
static void          mce_io_mon_delete                  (mce_io_mon_t *self);
static void          mce_io_mon_probe_seekable          (mce_io_mon_t *self);

static gboolean      mce_io_mon_read_chunks             (GIOChannel *source, GIOCondition condition, gpointer data);
static gboolean      mce_io_mon_read_string             (GIOChannel *source, GIOCondition condition, gpointer data);
static gboolean      mce_io_mon_input_cb                (GIOChannel *source, GIOCondition condition, gpointer data);

static mce_io_mon_t *mce_io_mon_register                (gint fd, const gchar *path, error_policy_t error_policy, gboolean rewind_policy, mce_io_mon_notify_cb callback, mce_io_mon_delete_cb delete_cb);

mce_io_mon_t        *mce_io_mon_register_string         (const gint fd, const gchar *const file, error_policy_t error_policy, gboolean rewind_policy, mce_io_mon_notify_cb callback, mce_io_mon_delete_cb delete_cb);
mce_io_mon_t        *mce_io_mon_register_chunk          (const gint fd, const gchar *const file, error_policy_t error_policy, gboolean rewind_policy, mce_io_mon_notify_cb callback, mce_io_mon_delete_cb delete_cb, gulong chunk_size);

void                 mce_io_mon_unregister              (mce_io_mon_t *iomon);
void                 mce_io_mon_unregister_list         (GSList *list);
void                 mce_io_mon_unregister_at_path      (const char *path);

void                 mce_io_mon_suspend                 (mce_io_mon_t *iomon);
void                 mce_io_mon_resume                  (mce_io_mon_t *iomon);

const gchar         *mce_io_mon_get_path                (const mce_io_mon_t *iomon);
int                  mce_io_mon_get_fd                  (const mce_io_mon_t *iomon);

// MISC_UTILS

gboolean        mce_close_file                          (const gchar *const file, FILE **fp);
gboolean        mce_read_chunk_from_file                (const gchar *const file, void **data, gssize *len, int flags);
gboolean        mce_read_string_from_file               (const gchar *const file, gchar **string);
gboolean        mce_read_number_string_from_file        (const gchar *const file, gulong *number, FILE **fp, gboolean rewind_file, gboolean close_on_exit);
gboolean        mce_write_string_to_file                (const gchar *const file, const gchar *const string);
void            mce_close_output                        (output_state_t *output);
gboolean        mce_write_number_string_to_file         (output_state_t *output, const gulong number);
gboolean        mce_write_number_string_to_file_atomic  (const gchar *const file, const gulong number);
gboolean        mce_are_settings_locked                 (void);
gboolean        mce_unlock_settings                     (void);
static gboolean mce_io_read_all                         (int fd, void *buff, size_t size, size_t *pdone);
static gboolean mce_io_write_all                        (int fd, const void *buff, size_t size, size_t *pdone);
void           *mce_io_load_file                        (const char *path, size_t *psize);
void           *mce_io_load_file_until_eof              (const char *path, size_t *psize);
gboolean        mce_io_save_file                        (const char *path, const void *data, size_t size, mode_t mode);
gboolean        mce_io_save_to_existing_file            (const char *path, const void *data, size_t size);
gboolean        mce_io_save_file_atomic                 (const char *path, const void *data, size_t size, mode_t mode, gboolean keep_backup);
gboolean        mce_io_update_file_atomic               (const char *path, const void *data, size_t size, mode_t mode, gboolean keep_backup);

/* ========================================================================= *
 * SUSPEND_DETECTION
 * ========================================================================= */

/** Detect suspend/resume cycle from CLOCK_MONOTONIC vs CLOCK_BOOTTIME
 */
static void io_detect_resume(void)
{
	static int64_t prev = 0;

	int64_t boot = mce_lib_get_boot_tick();
	int64_t mono = mce_lib_get_mono_tick();
	int64_t diff = boot - mono;

	int64_t skip = diff - prev;

	// small jitter can be due to scheduling too
	if( skip < 100 )
		goto EXIT;

	prev = diff;

	// no logging from the 1st time skip
	if( prev == skip )
		goto EXIT;

	mce_log(LL_DEVEL, "time skip: assume %"PRId64".%03"PRId64"s suspend",
		skip / 1000, skip % 1000);

	// notify in case some timers need re-evaluating
	datapipe_exec_output_triggers(&resume_detected_event_pipe,
				      &prev,
				      USE_INDATA);

EXIT:
	return;
}

/* ========================================================================= *
 * GLIB_IO_HELPERS
 * ========================================================================= */

/**
 * Get glib io condition as human readable string
 *
 * @param cond Bitmap of glib io conditions
 *
 * @return Names of bits set, separated with " | "
 */
static const char *io_condition_repr(GIOCondition cond)
{
	static const struct
	{
		GIOCondition bit;
		const char  *name;
	} lut[] =
	{
		{ .bit = G_IO_IN,   .name = "IN"   },
		{ .bit = G_IO_OUT,  .name = "OUT"  },
		{ .bit = G_IO_PRI,  .name = "PRI"  },
		{ .bit = G_IO_ERR,  .name = "ERR"  },
		{ .bit = G_IO_HUP,  .name = "HUP"  },
		{ .bit = G_IO_NVAL, .name = "NVAL" },
		// sentinel
		{ .bit = 0,         .name = 0      }
	};

	static char buf[64];
	char *end = buf + sizeof buf - 1;
	char *pos = buf;

	auto void add(const char *s);

	auto void add(const char *s)
	{
		while( pos < end && *s ) *pos++ = *s++;
	}

	for( size_t i = 0; lut[i].bit; ++i ) {
		if( cond & lut[i].bit ) {
			cond ^= lut[i].bit;
			if( pos > buf ) add("|");
			add(lut[i].name);
		}
	}
	*pos = 0;
	if( cond ) {
		if( pos > buf ) add("|");
		snprintf(pos, end - pos, "0x%x", cond);
	}

	return buf;
}

/**
 * Get glib io status as human readable string
 *
 * @param io_status as returned from g_io_channel_read_chars()
 *
 * @return Name of the status enum, without the common prefix
 */
static const char *io_status_name(GIOStatus io_status)
{
	const char *status_name = "UNKNOWN";
	switch (io_status) {
	case G_IO_STATUS_NORMAL: status_name = "NORMAL"; break;
	case G_IO_STATUS_ERROR:  status_name = "ERROR";  break;
	case G_IO_STATUS_EOF:    status_name = "EOF";    break;
	case G_IO_STATUS_AGAIN:  status_name = "AGAIN";  break;
	default: break; // ... just to keep static analysis happy
	}
	return status_name;
}

/* ========================================================================= *
 * IO_MONITOR
 * ========================================================================= */

/** Create I/O monitor object
 *
 * Allocates I/O monitor object and does all initialization that
 * does not need monitoring type information.
 *
 * Specifically the io watch is not activated from within this
 * function, it needs to be done separately.
 *
 * @param path       File path
 * @param delete_cb  I/O monitor object delete notification callback
 *
 * @return I/O monitor object
 */
static mce_io_mon_t *mce_io_mon_create(const char *path, mce_io_mon_delete_cb delete_cb)
{
	mce_io_mon_t *self = 0;

	if( !path ) {
		mce_log(LL_ERR, "path == NULL!");
		goto EXIT;
	}

	if( !delete_cb ) {
		mce_log(LL_ERR, "delete_cb == NULL!");
		goto EXIT;
	}

	if( !(self = g_slice_new(mce_io_mon_t)) )
		goto EXIT;

	memset(self, 0, sizeof *self);

	/* Fill in sane default values */

	self->path          = g_strdup(path);
	self->type          = IOMON_UNSET;
	self->chunk_size    = 0;

	self->seekable      = FALSE;
	self->suspended     = TRUE;

	self->iochan        = 0;
	self->iowatch_id    = 0;

	self->nofity_cb     = 0;
	self->delete_cb     = delete_cb;

	self->error_policy  = MCE_IO_ERROR_POLICY_WARN;
	self->rewind_policy = FALSE;

	self->user_data     = 0;
	self->user_free_cb  = 0;

	mce_log(LL_DEBUG, "adding monitor for: %s", self->path);

EXIT:
	return self;
}

/** Delete I/O monitor object
 *
 * Calls delete notification callback to allow upper level
 * logic to perform cleanup.
 *
 * Then removes io watch, closes io channel and releases
 * all dynamic resources associated with the I/O monitor object.
 *
 * @param self I/O monitor object
 */
static void mce_io_mon_delete(mce_io_mon_t *self)
{
	if( !self )
		goto EXIT;

	mce_log(LL_NOTICE, "removing monitor for: %s", self->path);

	/* Call the about to delete callback */
	if( self->delete_cb ) {
		self->delete_cb(self);
	}

	/* Free attached user data */
	if( self->user_data && self->user_free_cb )
		self->user_free_cb(self->user_data);

	/* Unlink from monitor list */
	if( !g_slist_find(file_monitors, self) ) {
		mce_log(LL_WARN, "Trying to unregister non-registered"
			" file monitor");
	}
	else {
		file_monitors = g_slist_remove(file_monitors, self);
	}

	/* Remove I/O watch */
	mce_io_mon_suspend(self);

	/* Close the I/O channel */
	if( self->iochan ) {
		GError    *error    = NULL;
		GIOStatus  iostatus = g_io_channel_shutdown(self->iochan,
							    TRUE, &error);

		if( iostatus != G_IO_STATUS_NORMAL ) {
			loglevel_t loglevel = LL_ERR;

			/* If we get ENODEV, only log a debug message,
			 * since this happens for hotpluggable
			 * /dev/input files
			 */
			if( (error->code == G_IO_CHANNEL_ERROR_FAILED) &&
			    (errno == ENODEV) )
				loglevel = LL_DEBUG;

			mce_log(loglevel, "Cannot close `%s'; %s",
				self->path, error->message);
		}
		g_clear_error(&error);

		g_io_channel_unref(self->iochan);
		self->iochan = 0;
	}

	/* Forget file path */
	g_free(self->path), self->path = 0;

	/* Reset to something that is likely to generate segfaults
	 * if it ends up used after freeing ... */
	memset(self, 0xff, sizeof *self);

	g_slice_free(mce_io_mon_t, self);
EXIT:
	return;
}

/**
 * Check if the monitored io channel is truly seekable
 *
 * Glib seems to be making guesses based on file type and
 * gets it massively wrong for the files MCE needs to read.
 */

static void mce_io_mon_probe_seekable(mce_io_mon_t *self)
{
	gboolean glib = FALSE, kernel = FALSE;

	/* glib assumes ... */
	if (g_io_channel_get_flags(self->iochan) & G_IO_FLAG_IS_SEEKABLE) {
		glib = TRUE;
	}
	/* ... kernel knows */
	if (lseek64(g_io_channel_unix_get_fd(self->iochan), 0, SEEK_CUR) != -1) {
		kernel = TRUE;
	}
	/* report the difference */
	if (kernel != glib) {
		mce_log(LL_DEBUG, "%s: is %sseekable, while glib thinks it is %sseekable",
			self->path, kernel ? "" : "NOT ", glib ? "" : "NOT ");
	}

	self->seekable = kernel;
}

/** Process input for chunked io monitor
 *
 * For use from mce_io_mon_input_cb() only.
 *
 * @param source    The source of the activity
 * @param condition The I/O condition
 * @param data      The iomon structure
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean mce_io_mon_read_chunks(GIOChannel *source,
				       GIOCondition condition,
				       gpointer data)
{
	gboolean      status      = FALSE;

	mce_io_mon_t  *iomon      = data;
	gchar        *buffer      = NULL;
	gsize         bytes_want  = 4096;
	gsize         bytes_have  = 0;
	gsize         chunks_have = 0;
	gsize         chunks_done = 0;
	GError       *error       = NULL;
	GIOStatus     io_status   = G_IO_STATUS_NORMAL;

#ifdef ENABLE_WAKELOCKS
	/* Since the locks on kernel side are released once all
	 * events are read, we must obtain the userspace lock
	 * before reading the available data */
	wakelock_lock("mce_input_handler", -1);
#endif

	/* We get input from evdev nodes at resume, handle that 1st */
	io_detect_resume();

	// paranoia mode:  upper levels should take care of these
	if( !(condition & G_IO_IN) )
		goto EXIT;

	if( !iomon )
		goto EXIT;

	/* Seek to the beginning of the file before reading if needed */
	if( iomon->rewind_policy ) {
		g_io_channel_seek_position(source, 0, G_SEEK_SET, &error);

		if( error ) {
			mce_log(LL_ERR,	"%s: seek error: %s",
				iomon->path, error->message);
			g_clear_error(&error);
		}
	}

	/* Adjust read size to multiples of small sized chunks,
	 * or size of one larger chunk */
	if( iomon->chunk_size < bytes_want )
		bytes_want -= bytes_want % iomon->chunk_size;
	else
		bytes_want = iomon->chunk_size;

	/* Allocate read buffer */
	buffer = g_malloc(bytes_want);

	io_status = g_io_channel_read_chars(source, buffer,
					    bytes_want, &bytes_have, &error);

	/* If the read was interrupted, ignore */
	if( io_status == G_IO_STATUS_AGAIN ) {
		status = TRUE;
		goto EXIT;
	}

	if( error ) {
		mce_log(LL_ERR, "Error when reading from %s: %s",
			iomon->path, error->message);
		g_clear_error(&error);
		goto EXIT;
	}

	if( bytes_have % iomon->chunk_size ) {
		mce_log(LL_WARN, "Incomplete chunks read from: %s",
			iomon->path);
	}

	/* Process the data, and optionally ignore some of it */
	chunks_have = bytes_have / iomon->chunk_size;
	if( !chunks_have ) {
		mce_log(LL_ERR, "Empty read from %s", iomon->path);
	}
	else {
		gchar *chunk = buffer;
		for( ; chunks_done < chunks_have ; chunk += iomon->chunk_size ) {
			++chunks_done;

			if( !iomon->nofity_cb(iomon, chunk, iomon->chunk_size) ) {
				continue;
			}

			/* Ignore rest of the data already read */
			if( !iomon->seekable )
				break;

			/* Try to seek to end of the file */
			g_io_channel_seek_position(iomon->iochan, 0,
						   G_SEEK_END, &error);

			if( error ) {
				mce_log(LL_ERR, "Error when reading from %s: %s",
					iomon->path, error->message);
				g_clear_error(&error);
			}
			break;
		}
	}

	mce_log(LL_INFO, "%s: status=%s, data=%d/%d=%d+%d, skipped=%d",
		iomon->path, io_status_name(io_status),
		bytes_have, (int)iomon->chunk_size, chunks_have,
		bytes_have % (int)iomon->chunk_size, chunks_have - chunks_done);

	status = TRUE;

EXIT:
	g_clear_error(&error);
	g_free(buffer);

#ifdef ENABLE_WAKELOCKS
	/* Release the lock after we're done with processing it */
	wakelock_unlock("mce_input_handler");
#endif

	return status;
}

/** Process input for string io monitor
 *
 * For use from mce_io_mon_input_cb() only.
 *
 * @param source    The source of the activity
 * @param condition The I/O condition
 * @param data      The iomon structure
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean mce_io_mon_read_string(GIOChannel *source,
				       GIOCondition condition,
				       gpointer data)
{
	gboolean      status     = FALSE;

	mce_io_mon_t *iomon      = data;
	gchar        *str        = NULL;
	gsize         bytes_read = 0;
	GError       *error      = NULL;

	// paranoia mode:  upper levels should take care of these
	if( !(condition & G_IO_IN) )
		goto EXIT;

	if( !iomon )
		goto EXIT;

	/* Seek to the beginning of the file before reading if needed */
	if( iomon->rewind_policy ) {
		g_io_channel_seek_position(source, 0, G_SEEK_SET, &error);

		if( error ) {
			mce_log(LL_ERR,	"%s: seek error: %s",
				iomon->path, error->message);
			g_clear_error(&error);
		}
	}

	g_io_channel_read_line(source, &str, &bytes_read, NULL, &error);

	if( error ) {
		mce_log(LL_ERR, "Error when reading from %s: %s",
			iomon->path, error->message);
		goto EXIT;
	}

	if( !bytes_read || !str || !*str )
		mce_log(LL_ERR, "Empty read from %s",iomon->path);
	else
		iomon->nofity_cb(iomon, str, bytes_read);

	status = TRUE;

EXIT:
	g_free(str);
	g_clear_error(&error);

	return status;
}

/** Callback for I/O watch
 *
 * Handles error conditions first; then does input monitor
 * type specific input processing.
 *
 * The I/O monitor will be disabled and deleted on errors.
 * Additionally the whole process can be terminated if
 * error policy requires it.
 *
 * @param source    Unused
 * @param condition The GIOCondition for the error
 * @param data      The iomon structure
 *
 * @return TRUE to keep iomon active, FALSE to disable it;
 *         may also exit depending on error policy for iomon
 */
static gboolean mce_io_mon_input_cb(GIOChannel *source,
				    GIOCondition condition,
				    gpointer data)
{
	(void)source; // unused

	mce_io_mon_t *iomon      = data;
	gboolean      keep_going = TRUE;
	gboolean      terminate  = FALSE;
	loglevel_t    loglevel   = LL_DEBUG;

	// sanity checks
	if( !iomon ) {
		mce_log(LL_ERR, "iomon == NULL!");
		keep_going = FALSE;
		goto EXIT;
	}

	// error conditions
	if( condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) ) {
		mce_log(LL_ERR, "iomon '%s' got %s", iomon->path,
			io_condition_repr(condition));
		keep_going = FALSE;
		goto EXIT;
	}

	// input processing
	if( condition & G_IO_IN ) {
		switch (iomon->type) {
		case IOMON_STRING:
			if( !mce_io_mon_read_string(source, condition, data) ) {
				mce_log(LL_WARN, "mce_io_mon_read_string failed");
			}
			break;

		case IOMON_CHUNK:
			if( !mce_io_mon_read_chunks(source, condition, data) ) {
				mce_log(LL_WARN, "mce_io_mon_read_chunks failed");
			}
			break;

		default:
		case IOMON_UNSET:
			mce_log(LL_WARN, "unknown iomon type");
			keep_going = FALSE;
			break;
		}
	}

EXIT:
	// cancel io monitor
	if( !keep_going && iomon ) {
		/* Mark error watch as removed */
		iomon->iowatch_id = 0;

		/* Adjust actions based on error policy */
		switch (iomon->error_policy) {
		case MCE_IO_ERROR_POLICY_EXIT:
			terminate = TRUE;
			loglevel = LL_CRIT;
			break;

		case MCE_IO_ERROR_POLICY_WARN:
			loglevel = LL_WARN;
			break;

		default:
		case MCE_IO_ERROR_POLICY_IGNORE:
			loglevel = LL_DEBUG;
			break;
		}

		/* Write log */
		mce_log(loglevel, "disabling io monitor for: %s", iomon->path);

		/* Remove IO monitor */
		mce_io_mon_unregister(iomon);
	}

	// terminate process
	if( terminate ) {
		mce_log(LL_CRIT, "terminating due to error policy");
		mce_quit_mainloop();
	}

	return keep_going;
}

/* ========================================================================= *
 * I/O MONITOR API
 * ========================================================================= */

/**
 * Register an I/O monitor; reads and returns data
 *
 * @param fd File Descriptor; this takes priority over file; -1 if not used
 * @param file Path to the file
 * @param error_policy MCE_IO_ERROR_POLICY_EXIT to exit on error,
 *                     MCE_IO_ERROR_POLICY_WARN to warn about errors
 *                                              but ignore them,
 *                     MCE_IO_ERROR_POLICY_IGNORE to silently ignore errors
 * @param callback Function to call with result
 * @return An I/O monitor pointer on success, NULL on failure
 */
static mce_io_mon_t *mce_io_mon_register(gint fd,
					 const gchar *path,
					 error_policy_t error_policy,
					 gboolean rewind_policy,
					 mce_io_mon_notify_cb callback,
					 mce_io_mon_delete_cb delete_cb)
{
	bool          success = false;
	mce_io_mon_t *iomon   = 0;
	GError       *error   = NULL;

	/* Sanity checks */
	if( !path ) {
		mce_log(LL_ERR, "path == NULL!");
		goto EXIT;
	}

	if( !callback ) {
		mce_log(LL_ERR, "callback == NULL!");
		goto EXIT;
	}

	/* Silently ignore non-existing files */
	if( fd == -1 && access(path, F_OK) == -1 )
		goto EXIT;

	/* Allocate monitor object */
	if( !(iomon = mce_io_mon_create(path, delete_cb)) )
		goto EXIT;

	/* Add to monitor list */
	file_monitors = g_slist_prepend(file_monitors, iomon);

	/* Set custom props */
	iomon->nofity_cb    = callback;
	iomon->error_policy = error_policy;

	/* Set up io channel */
	if( fd != -1 )
		iomon->iochan = g_io_channel_unix_new(fd);
	else
		iomon->iochan = g_io_channel_new_file(path, "r", &error);

	if( !iomon->iochan ) {
		mce_log(LL_ERR, "Failed to open `%s'; %s", path,
			error ? error->message : "unknown error");
		goto EXIT;
	}

	/* Transfer fd ownership to io channel */
	g_io_channel_set_close_on_unref(iomon->iochan, TRUE), fd = -1;

	/* Glib seekability is broken, probe via syscall */
	mce_io_mon_probe_seekable(iomon);

	/* Set rewind policy */
	if( iomon->seekable ) {
		iomon->rewind_policy = rewind_policy;
	} else if( rewind_policy ) {
		mce_log(LL_ERR, "Attempting to set rewind policy to TRUE "
			"on non-seekable I/O channel `%s'", path);
	}

	success = true;
EXIT:
	if( fd != -1 )
		close(fd);

	if( !success )
		mce_io_mon_delete(iomon), iomon = 0;

	g_clear_error(&error);

	return iomon;
}

/** Unregister an I/O monitor
 *
 * @param io_monitor A pointer to the I/O monitor to unregister
 */
void mce_io_mon_unregister(mce_io_mon_t *iomon)
{
	mce_io_mon_delete(iomon);
}

/** Remove all touch device I/O monitors in a list
 *
 * @param list A list of I/O monitors
 */
void mce_io_mon_unregister_list(GSList *list)
{
	GSList *now, *zen;
	for( now = list; now; now = zen ) {
		zen = now->next;
		mce_io_mon_unregister(now->data);
	}
}

/** Unregister I/O monitors for the given path
 *
 * @param path Path to file for which all monitors should be unregistered
 */
void mce_io_mon_unregister_at_path(const char *path)
{
	GSList *now, *zen;

	if( !path )
		goto EXIT;

	for( now = file_monitors; now; now = zen ) {
		zen = now->next;

		mce_io_mon_t *self = now->data;

		if( !self->path || strcmp(self->path, path) )
			continue;

		mce_io_mon_unregister(self);
	}
EXIT:
	return;
}

/**
 * Suspend an I/O monitor
 *
 * @param io_monitor A pointer to the I/O monitor to suspend
 */
void mce_io_mon_suspend(mce_io_mon_t *iomon)
{
	if( !iomon ) {
		mce_log(LL_ERR, "iomon == NULL!");
		goto EXIT;
	}

	/* Remove I/O watches */
	if( iomon->iowatch_id ) {
		g_source_remove(iomon->iowatch_id),
			iomon->iowatch_id = 0;
	}

	iomon->suspended = TRUE;

EXIT:
	return;
}

/**
 * Resume an I/O monitor
 *
 * @param io_monitor A pointer to the I/O monitor to resume
 */
void mce_io_mon_resume(mce_io_mon_t *iomon)
{
	if( !iomon ) {
		mce_log(LL_ERR, "iomon == NULL!");
		goto EXIT;
	}

	if( !iomon->suspended )
		goto EXIT;

	/* Seek to the end of the file if the file is seekable,
	 * and rewind policy is not requested
	 */
	if( iomon->seekable && !iomon->rewind_policy ) {
		GError *error = NULL;
		g_io_channel_seek_position(iomon->iochan, 0,
					   G_SEEK_END, &error);
		if( error ) {
			mce_log(LL_ERR,	"%s: seek error: %s",
				iomon->path, error->message);
		}
		g_clear_error(&error);
	}

	/* Set up input monitor */
	if( iomon->iowatch_id )
		g_source_remove(iomon->iowatch_id);

	iomon->iowatch_id =
		g_io_add_watch(iomon->iochan,
			       G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
			       mce_io_mon_input_cb, iomon);

	/* Mark as not-suspended */
	iomon->suspended = FALSE;

EXIT:
	return;
}

/**
 * Register an I/O monitor; reads and returns a string
 *
 * @param fd File Descriptor; this takes priority over file; -1 if not used
 * @param file Path to the file
 * @param error_policy MCE_IO_ERROR_POLICY_EXIT to exit on error,
 *                     MCE_IO_ERROR_POLICY_WARN to warn about errors
 *                                              but ignore them,
 *                     MCE_IO_ERROR_POLICY_IGNORE to silently ignore errors
 * @param rewind_policy TRUE to seek to the beginning,
 *                      FALSE to stay at current position
 * @param callback Function to call with result
 * @return An I/O monitor cookie on success, NULL on failure
 */
mce_io_mon_t *mce_io_mon_register_string(const gint fd,
					 const gchar *const file,
					 error_policy_t error_policy,
					 gboolean rewind_policy,
					 mce_io_mon_notify_cb callback,
					 mce_io_mon_delete_cb delete_cb)
{
	mce_io_mon_t *iomon = NULL;

	iomon = mce_io_mon_register(fd, file,
				    error_policy, rewind_policy,
				    callback, delete_cb);

	if (iomon == NULL)
		goto EXIT;

	/* Set the I/O monitor type and call resume to add an I/O watch */
	iomon->type = IOMON_STRING;
	mce_io_mon_resume(iomon);

EXIT:
	return iomon;
}

/**
 * Register an I/O monitor; reads and returns a chunk of specified size
 *
 * @param fd File Descriptor; this takes priority over file; -1 if not used
 * @param file Path to the file
 * @param error_policy MCE_IO_ERROR_POLICY_EXIT to exit on error,
 *                     MCE_IO_ERROR_POLICY_WARN to warn about errors
 *                                              but ignore them,
 *                     MCE_IO_ERROR_POLICY_IGNORE to silently ignore errors
 * @param rewind_policy TRUE to seek to the beginning,
 *                      FALSE to stay at current position
 * @param callback Function to call with result
 * @param chunk_size The number of bytes to read in each chunk
 * @return An I/O monitor cookie on success, NULL on failure
 */
mce_io_mon_t *mce_io_mon_register_chunk(const gint fd,
					const gchar *const file,
					error_policy_t error_policy,
					gboolean rewind_policy,
					mce_io_mon_notify_cb callback,
					mce_io_mon_delete_cb delete_cb,
					gulong chunk_size)
{
	mce_io_mon_t *iomon = NULL;
	GError *error = NULL;

	iomon = mce_io_mon_register(fd, file,
				    error_policy, rewind_policy,
				    callback, delete_cb);

	if( !iomon )
		goto EXIT;

	/* We only read this file in binary form */
	g_io_channel_set_encoding(iomon->iochan, NULL, &error);
	g_clear_error(&error);

	/* No buffering since we're using this for reading data from
	 * device drivers and need to keep the i/o state in sync
	 * between kernel and user space for the automatic suspend
	 * prevention via wakelocks to work
	 */
	g_io_channel_set_buffered(iomon->iochan, FALSE);

	/* Don't block */
	g_io_channel_set_flags(iomon->iochan, G_IO_FLAG_NONBLOCK, &error);
	g_clear_error(&error);

	/* Set the I/O monitor type and call resume to add an I/O watch */
	iomon->type       = IOMON_CHUNK;
	iomon->chunk_size = chunk_size;
	mce_io_mon_resume(iomon);

EXIT:
	return iomon;
}

/**
 * Return the name of the monitored file
 *
 * @param io_monitor An opaque pointer to the I/O monitor structure
 * @return The name of the monitored file
 */
const gchar *mce_io_mon_get_path(const mce_io_mon_t *iomon)
{
	const gchar *path = 0;

	if( iomon )
		path = iomon->path;

	return path;
}

/**
 * Return the file descriptor of the monitored file;
 * if the file being monitored was opened from a path
 * rather than a file descriptor, -1 is returned
 *
 * @param io_monitor An opaque pointer to the I/O monitor structure
 * @return The file descriptor of the monitored file
 */
int mce_io_mon_get_fd(const mce_io_mon_t *iomon)
{
	int fd = -1;

	if( iomon && iomon->iochan )
		fd = g_io_channel_unix_get_fd(iomon->iochan);

	return fd;
}

/** Attach user data block to io monitor
 *
 * If non-null free_cb callback is given, the user_data block will
 * be released using it when io-monitor itself is deleted.
 *
 * Note: The delete notification callback is called before the
 *       user data is released, i.e. user data is still available
 *       at that point.
 *
 * @param io_monitor An opaque pointer to the I/O monitor structure
 * @param user_data  Data block to attach to io monitor, or NULL
 * @param free_cb    Free function to release data block or NULL
 */
void mce_io_mon_set_user_data(mce_io_mon_t *iomon,
			      void *user_data,
			      mce_io_mon_free_cb free_cb)
{
	if( !iomon )
		goto EXIT;

	/* Clear already existing user data */
	if( iomon->user_data && iomon->user_free_cb )
		iomon->user_free_cb(iomon->user_data);

	/* Set user data */
	iomon->user_data = user_data;
	iomon->user_free_cb = free_cb;
EXIT:
	return;
}

/** Get user data block attached to io monitor
 *
 * @param io_monitor An opaque pointer to the I/O monitor structure
 *
 * @return user data block, or NULL if not set
 */
void *mce_io_mon_get_user_data(const mce_io_mon_t *iomon)
{
	return iomon ? iomon->user_data : 0;
}

/* ========================================================================= *
 * MISC_UTILS
 * ========================================================================= */

/**
 * Helper function for closing files that checks for NULL,
 * prints proper error messages and NULLs the file pointer after close
 *
 * @param file The name of the file to close; only used by error messages
 * @param fp A pointer to the file pointer to close
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_close_file(const gchar *const file, FILE **fp)
{
	gboolean status = FALSE;

	if (fp == NULL) {
		mce_log(LL_CRIT,
			"fp == NULL!");
		goto EXIT;
	}

	if (*fp == NULL) {
		status = TRUE;
		goto EXIT;
	}

	if (fclose(*fp) == EOF) {
		mce_log(LL_ERR,
			"Failed to close `%s'; %s",
			file ? file : "<unset>",
			g_strerror(errno));
		status = FALSE;

		/* Ignore error */
		errno = 0;
		goto EXIT;
	}

	*fp = NULL;

	status = TRUE;

EXIT:
	return status;
}

/**
 * Read a chunk from a file
 *
 * @param file Path to the file, or NULL to use an already open fd instead
 * @param[out] data A newly allocated buffer with the first chunk from the file
 * @param[in,out] len [in] The length of the buffer to read
 *                    [out] The number of bytes read
 * @param flags Additional flags to pass to open();
 *              by default O_RDONLY is always passed -- this is mainly
 *              to allow passing O_NONBLOCK
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_read_chunk_from_file(const gchar *const file, void **data,
				  gssize *len, int flags)
{
	gboolean status = FALSE;
	gint again_count = 0;
	gssize result = -1;
	gint fd;

	if (file == NULL) {
		mce_log(LL_CRIT, "file == NULL!");
		goto EXIT;
	}

	if (len == NULL) {
		mce_log(LL_CRIT, "len == NULL!");
		goto EXIT;
	}

	if (*len <= 0) {
		mce_log(LL_CRIT, "*len <= 0!");
		goto EXIT;
	}

	/* If we cannot open the file, abort */
	if ((fd = open(file, O_RDONLY | flags)) == -1) {
		mce_log(LL_ERR,
			"Cannot open `%s' for reading; %s",
			file, g_strerror(errno));

		/* Ignore error */
		errno = 0;
		goto EXIT;
	}

	if ((*data = g_try_malloc(*len)) == NULL) {
		mce_log(LL_CRIT,
			"Failed to allocate memory (%zd bytes)!",
			*len);
		goto EXIT2;
	}

	while (again_count++ < 10) {
		/* Clear errors from earlier iterations */
		errno = 0;

		result = read(fd, *data, *len);

		if ((result == -1) &&
		    ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
			continue;
		} else {
			break;
		}
	}

	if (result == -1) {
		mce_log(LL_ERR,
			"Failed to read from `%s'; %s",
			file, g_strerror(errno));

		/* Ignore error */
		errno = 0;
		goto EXIT2;
	}

	status = TRUE;

EXIT2:
	if (close(fd) == -1) {
		mce_log(LL_ERR,
			"Failed to close `%s'; %s",
			file, g_strerror(errno));
		errno = 0;
	}

	/* Ignore error */
	errno = 0;

	*len = result;

EXIT:
	return status;
}

/**
 * Read a string from a file
 *
 * @param file Path to the file
 * @param[out] string A newly allocated string with the first line of the file
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_read_string_from_file(const gchar *const file, gchar **string)
{
	GError *error = NULL;
	gboolean status = FALSE;

	if (file == NULL) {
		mce_log(LL_CRIT, "file == NULL!");
		goto EXIT;
	}

	if (g_file_get_contents(file, string, NULL, &error) == FALSE) {
		mce_log(LL_ERR,
			"Cannot open `%s' for reading; %s",
			file, error->message);
		goto EXIT;
	}

	status = TRUE;

EXIT:
	/* Reset errno,
	 * to avoid false positives down the line
	 */
	errno = 0;
	g_clear_error(&error);

	return status;
}

/**
 * Read a number representation of a string from a file
 *
 * @param file Path to the file, or NULL to user an already open FILE * instead
 * @param[out] number A number representation of the first line of the file
 * @param fp A pointer to a FILE *; set the FILE * to NULL to use the file
 *           path instead
 * @param rewind_file TRUE to seek to the beginning of the file,
 *                    FALSE to read from the current position;
 *                    only affects already open files
 * @param close_on_exit TRUE to close the file on exit,
 *                      FALSE to leave the file open
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_read_number_string_from_file(const gchar *const file,
					  gulong *number, FILE **fp,
					  gboolean rewind_file,
					  gboolean close_on_exit)
{
	gboolean status = FALSE;
	gint again_count = 0;
	FILE *new_fp = NULL;
	gint retval;

	if ((file == NULL) && ((fp == NULL) || (*fp == NULL))) {
		mce_log(LL_CRIT,
			"(file == NULL) && ((fp == NULL) || (*fp == NULL))!");
		goto EXIT;
	}

	if ((fp == NULL) && (close_on_exit == FALSE)) {
		mce_log(LL_CRIT,
			"(fp == NULL) && (close_on_exit == FALSE)!");
		goto EXIT;
	}

	/* If we cannot open the file, abort */
	if ((fp == NULL) || (*fp == NULL)) {
		if ((new_fp = fopen(file, "r")) == NULL) {
			mce_log(LL_ERR,
				"Cannot open `%s' for reading; %s",
				file, g_strerror(errno));

			/* Ignore error */
			errno = 0;
			goto EXIT;
		}
	} else {
		new_fp = *fp;
	}

	/* Rewind file if we already have one */
	if ((fp != NULL) && (*fp != NULL) && (rewind_file == TRUE)) {
		if (fseek(*fp, 0L, SEEK_SET) == -1) {
			mce_log(LL_ERR,
				"Failed to rewind `%s'; %s",
				file, g_strerror(errno));

			/* Ignore error */
			errno = 0;
			goto EXIT2;
		}
	}

	if ((fp != NULL) && (*fp == NULL))
		*fp = new_fp;

	while (again_count++ < 10) {
		/* Clear errors from earlier iterations */
		clearerr(new_fp);
		errno = 0;

		retval = fscanf(new_fp, "%lu", number);

		if ((retval == EOF) &&
		    (ferror(new_fp) != 0) && (errno == EAGAIN)) {
			continue;
		} else {
			break;
		}
	}

	/* Was the read successful? */
	if ((retval == EOF) && (ferror(new_fp) != 0)) {
		mce_log(LL_ERR,
			"Failed to read from `%s'; %s",
			file, g_strerror(errno));
		clearerr(new_fp);

		/* Ignore error */
		errno = 0;
		goto EXIT2;
	}

	if (retval != 1) {
		mce_log(LL_ERR,
			"Could not match any values when reading from `%s'",
			file);
		goto EXIT2;
	}

	status = TRUE;

EXIT2:
	/* XXX: improve close policy? */
	if ((status == FALSE) || (close_on_exit == TRUE)) {
		mce_close_file(file, &new_fp);

		if (fp != NULL)
			*fp = NULL;
	}

	/* Ignore error */
	errno = 0;

EXIT:
	return status;
}

/**
 * Write a string to a file
 *
 * @param file Path to the file
 * @param string The string to write
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_write_string_to_file(const gchar *const file,
				  const gchar *const string)
{
	gboolean status = FALSE;
	FILE *fp = NULL;
	gint retval;

	if (file == NULL) {
		mce_log(LL_CRIT, "file == NULL!");
		goto EXIT;
	}

	if (string == NULL) {
		mce_log(LL_CRIT, "string == NULL!");
		goto EXIT;
	}

	if ((fp = fopen(file, "w")) == NULL) {
		mce_log(LL_ERR,
			"Cannot open `%s' for %s; %s",
			file,
			"writing",
			g_strerror(errno));

		/* Ignore error */
		errno = 0;
		goto EXIT;
	}

	retval = fprintf(fp, "%s", string);

	/* Was the write successful? */
	if (retval < 0) {
		mce_log(LL_ERR,
			"Failed to write to `%s'; %s",
			file, g_strerror(errno));

		/* Ignore error */
		errno = 0;
		goto EXIT2;
	}

	status = TRUE;

EXIT2:
	mce_close_file(file, &fp);

EXIT:
	return status;
}

/**
 * Cleanup function for output file control structures
 *
 * Closes file stream associated with output if it is open
 *
 * It is explicitly permitted to call this function:
 * 1) with NULL output parameter
 * 2) without open stream in ouput
 * 3) more than one times
 *
 * @param output control structure for writing to a file
 */

void mce_close_output(output_state_t *output)
{
	if( output && output->file ) {
		if( fclose(output->file) == EOF ) {
			mce_log(LL_WARN,"%s: can't close %s: %m", output->context, output->path);
		}
		output->file = 0;
	}
}

/**
 * Write a string representation of a number to a file
 *
 * Note: this variant uses in-place rewrites when truncating.
 * It should thus not be used in cases where atomicity is expected.
 * For atomic replace, use mce_write_number_string_to_file_atomic()
 *
 * @param output control structure for writing to a file
 * @param number The number to write
 *
 * @return TRUE on success, FALSE on failure
 */

gboolean mce_write_number_string_to_file(output_state_t *output, const gulong number)
{
	gboolean status = FALSE; // assume failure

	if( !output ) {
		mce_log(LL_CRIT, "NULL output passed, terminating");
		mce_abort();
	}

	if( !output->context ) {
		mce_log(LL_CRIT, "output->context missing, terminating");
		mce_abort();
	}

	if( !output->path ) {
		if( !output->invalid_config_reported ) {
			output->invalid_config_reported = TRUE;
			mce_log(LL_ERR, "%s: output->path not configured", output->context);
		}
		goto EXIT;
	}

	if( !output->file ) {
		output->file = fopen(output->path, output->truncate_file ? "w" : "a");
		if( !output->file ) {
			mce_log(LL_ERR,"%s: can't open %s: %m", output->context, output->path);
			goto EXIT;
		}
	}
	else if( output->truncate_file )
	{
		rewind(output->file);
		if( ftruncate(fileno(output->file), 0) == -1 ) {
			mce_log(LL_WARN,"%s: can't truncate %s: %m", output->context, output->path);
		}
	}

	// from now on assume success
	status = TRUE;

	if( fprintf(output->file, "%lu", number) < 0 ) {
		mce_log(LL_WARN,"%s: can't write %s: %m", output->context, output->path);
		status = FALSE;
	}

	if( fflush(output->file) == EOF ) {
		mce_log(LL_WARN,"%s: can't flush %s: %m", output->context, output->path);
		status = FALSE;
	}

EXIT:

	if( output->close_on_exit && output->file ) {
		if( fclose(output->file) == EOF ) {
			mce_log(LL_WARN,"%s: can't close %s: %m", output->context, output->path);
		}
		output->file = 0;
	}

	return status;
}

/**
 * Write a string representation of a number to a file
 * in an atomic manner
 *
 * @param file Path to the file to write to
 * @param number The number to write
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_write_number_string_to_file_atomic(const gchar *const file,
						const gulong number)
{
	gboolean status = FALSE;
	gchar *tmpname = NULL;
	FILE *fp = NULL;
	gint retval;
	int fd;

	if (file == NULL) {
		mce_log(LL_CRIT,
			"file == NULL");
		goto EXIT;
	}

	if ((tmpname = g_strconcat(file, TMP_SUFFIX, NULL)) == NULL) {
		mce_log(LL_ERR,
			"Failed to allocate memory for `%s%s'",
			file, TMP_SUFFIX);
		goto EXIT;
	}

	/* If we cannot open the file, abort */
	if ((fp = fopen(tmpname, "w")) == NULL) {
		mce_log(LL_ERR,
			"Cannot open `%s' for writing; %s",
			tmpname, g_strerror(errno));

		/* Ignore error */
		errno = 0;
		goto EXIT;
	}

	retval = fprintf(fp, "%lu", number);

	/* Was the write successful? */
	if (retval < 0) {
		mce_log(LL_ERR,
			"Failed to write to `%s'; %s",
			tmpname, g_strerror(errno));

		/* Ignore error */
		errno = 0;
		goto EXIT2;
	}

	if ((fd = fileno(fp)) == -1) {
		mce_log(LL_ERR,
			"Failed to convert *fp to fd; %s",
			g_strerror(errno));

		/* Ignore error */
		errno = 0;
		goto EXIT2;
	}

	/* Ensure that the data makes it to disk */
	if (fsync(fd) == -1) {
		mce_log(LL_ERR,
			"Failed to fsync `%s'; %s",
			tmpname, g_strerror(errno));

		/* Ignore error */
		errno = 0;
		goto EXIT2;
	}

	status = TRUE;

EXIT2:
	/* Close the temporary file */
	if (mce_close_file(tmpname, &fp) == FALSE) {
		status = FALSE;
		goto EXIT;
	}

	/* And if everything has been successful so far,
	 * rename the temporary file over the old file
	 */
	if (status == TRUE) {
		if (rename(tmpname, file) == -1) {
			mce_log(LL_ERR,
				"Failed to rename `%s' to `%s'; %s",
				tmpname, file, g_strerror(errno));

			status = FALSE;

			/* Ignore error */
			errno = 0;
			goto EXIT;
		}
	}

EXIT:
	g_free(tmpname);

	return status;
}

/**
 * Test whether there's a settings lock due to pending
 * backup/restore or device clear/factory reset operation
 *
 * @return TRUE if the settings lock file is in place,
 *         FALSE if the settings lock file is not in place
 */
gboolean mce_are_settings_locked(void)
{
	gboolean status = (g_access(MCE_SETTINGS_LOCK_FILE_PATH, F_OK) == 0);

	errno = 0;

	return status;
}

/**
 * Remove the settings lock file
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_unlock_settings(void)
{
	gboolean status = (g_unlink(MCE_SETTINGS_LOCK_FILE_PATH) == 0);

	errno = 0;

	return status;
}

/** Helper for dealing with partially successful reads
 *
 * @param fd file descriptor to read from
 * @param buff address where to read to
 * @param size number of bytes to read
 * @param pdone where to store actual read count, or NULL
 *
 * @return TRUE if all bytes could be read, otherwise FALSE
 */
static
gboolean mce_io_read_all(int fd, void *buff, size_t size, size_t *pdone)
{
	size_t  done = 0;
	char   *data = buff;

	while( done < size ) {
		ssize_t rc = TEMP_FAILURE_RETRY(read(fd, data+done, size));

		if( rc < 0 )
			break;

		if( rc == 0 ) {
			// clear errno if returning prematurely due to eof
			errno = 0;
			break;
		}

		done += (size_t)rc;
	}

	if( pdone )
		*pdone = done;

	return (done == size);
}

/** Helper for dealing with partially successful writes
 *
 * @param fd file descriptor to write to
 * @param buff address where to write from
 * @param size number of bytes to write
 * @param pdone where to store actual write count, or NULL
 *
 * @return TRUE if all bytes could be written, otherwise FALSE
 */
static
gboolean mce_io_write_all(int fd, const void *buff, size_t size, size_t *pdone)
{
	size_t      done = 0;
	const char *data = buff;

	while( done < size ) {
		ssize_t rc = TEMP_FAILURE_RETRY(write(fd, data+done, size));

		if( rc < 0 )
			break;

		done += (size_t)rc;
	}

	if( pdone )
		*pdone = done;

	return (done == size);
}

/** Load contents of a file
 *
 * @param path file to read from
 * @param psize where to store size of the loaded file, or NULL if not needed
 *
 * @return Zero terminated contents of the file, or NULL in case of errors
 */
void *mce_io_load_file(const char *path, size_t *psize)
{
	void  *res  = 0;
	char  *data = 0;
	size_t size = 0;
	int    fd = -1;

	struct stat st;

	if( (fd = TEMP_FAILURE_RETRY(open(path, O_RDONLY))) == -1 ) {
		if( errno != ENOENT )
			mce_log(LL_WARN, "open(%s): %m", path);
		goto EXIT;
	}

	if( fstat(fd, &st) == -1 ) {
		mce_log(LL_WARN, "stat(%s): %m", path);
		goto EXIT;
	}

	size = st.st_size;
	data = g_malloc(size + 1);

	if( !mce_io_read_all(fd, data, size, 0) ) {
		mce_log(LL_WARN, "read(%s): %m", path);
		goto EXIT;
	}

	data[size] = 0;

	res = data, data = 0;

EXIT:
	g_free(data);

	if( fd != -1 && TEMP_FAILURE_RETRY(close(fd)) == -1 )
		mce_log(LL_WARN, "close(%s): %m", path);

	if( psize )
		*psize = res ? size : 0;

	return res;
}

/** Load contents of a file, ignoring the reported file size
 *
 * @param path file to read from
 * @param psize where to store size of the loaded file, or NULL if not needed
 *
 * @return Zero terminated contents of the file, or NULL in case of errors
 */
void *mce_io_load_file_until_eof(const char *path, size_t *psize)
{
	void   *res  = 0;
	char   *data = 0;
	size_t  used = 0;
	size_t  size = 0;
	int     fd   = -1;
	ssize_t rc;

	if( (fd = TEMP_FAILURE_RETRY(open(path, O_RDONLY))) == -1 ) {
		if( errno != ENOENT )
			mce_log(LL_WARN, "open(%s): %m", path);
		goto EXIT;
	}

	size = 1024;
	data = g_malloc(size);

	for( ;; ) {
		rc = TEMP_FAILURE_RETRY(read(fd, data + used, size - used));;

		if( rc == 0 )
			break;

		if( rc == -1 ) {
			mce_log(LL_WARN, "read(%s): %m", path);
			goto EXIT;
		}

		used += (size_t)rc;
		if( size - used < 512 ) {
			size = size * 2;
			data = g_realloc(data, size);
		}
	}

	data = g_realloc(data, used + 1);
	data[used] = 0;
	res = data, data = 0;

EXIT:
	g_free(data);

	if( fd != -1 && TEMP_FAILURE_RETRY(close(fd)) == -1 )
		mce_log(LL_WARN, "close(%s): %m", path);

	if( psize )
		*psize = res ? used : 0;

	return res;
}

/** Write datablock to a file
 *
 * @param path file to write to
 * @param data start of the data to write
 * @param size length of the data to write
 * @param mode protection bits to apply
 *
 * @return TRUE on success, FALSE on errors
 */
gboolean mce_io_save_file(const char *path,
			  const void *data, size_t size,
			  mode_t mode)
{
	gboolean res = FALSE;
	int      fd  = -1;

	if( mode <= 0 )
		mode = 0664;

	fd = TEMP_FAILURE_RETRY(open(path, O_WRONLY|O_CREAT|O_TRUNC, 0600));
	if( fd == -1 ) {
		mce_log(LL_WARN, "open(%s): %m", path);
		goto EXIT;
	}

	if( !mce_io_write_all(fd, data, size, 0) ) {
		mce_log(LL_WARN, "write(%s): %m", path);
		goto EXIT;
	}

	if( fchmod(fd, mode) == -1 ) {
		mce_log(LL_WARN, "chmod(%s, %03o): %m", path, (int)mode);
		goto EXIT;
	}

	res = TRUE;

EXIT:
	if( fd != -1 && TEMP_FAILURE_RETRY(close(fd)) == -1 )
		mce_log(LL_WARN, "close(%s): %m", path);

	return res;
}

/** Write datablock to an existing file
 *
 * @param path file to write to
 * @param data start of the data to write
 * @param size length of the data to write
 *
 * @return TRUE on success, FALSE on errors
 */
gboolean mce_io_save_to_existing_file(const char *path,
				      const void *data, size_t size)
{
	gboolean res = FALSE;
	int      fd  = -1;

	fd = TEMP_FAILURE_RETRY(open(path, O_WRONLY|O_TRUNC));
	if( fd == -1 ) {
		mce_log(LL_WARN, "open(%s): %m", path);
		goto EXIT;
	}

	if( !mce_io_write_all(fd, data, size, 0) ) {
		mce_log(LL_WARN, "write(%s): %m", path);
		goto EXIT;
	}

	res = TRUE;

EXIT:
	if( fd != -1 && TEMP_FAILURE_RETRY(close(fd)) == -1 )
		mce_log(LL_WARN, "close(%s): %m", path);

	return res;
}

/** Atomically replace a file contents
 *
 * First writes to a temp file, then duplicates the original as
 * a backup and atomically replaces the original with freshly
 * written data.
 *
 * @param path file to write to
 * @param data start of the data to write
 * @param size length of the data to write
 * @param mode protection bits to apply
 * @param keep_backup whether to keep backup file on successful update
 *
 * @return TRUE on success, FALSE on errors
 */
gboolean mce_io_save_file_atomic(const char *path,
				 const void *data, size_t size,
				 mode_t mode, gboolean keep_backup)
{
	gboolean res = FALSE;

	gchar *temp = g_strdup_printf("%s.tmp", path);
	gchar *back = g_strdup_printf("%s.bak", path);

	if( !mce_io_save_file(temp, data, size, mode) )
		goto EXIT;

	if( unlink(back) == -1 && errno != ENOENT ) {
		mce_log(LL_WARN, "unlink(%s): %m", back);
		goto EXIT;
	}

	if( link(path, back) == -1 && errno != ENOENT ) {
		mce_log(LL_WARN, "link(%s, %s): %m", path, back);
		goto EXIT;
	}

	if( rename(temp, path) == -1 ) {
		mce_log(LL_WARN, "rename(%s, %s): %m", temp, path);
		goto EXIT;
	}

	if( !keep_backup && unlink(back) == -1 && errno != ENOENT ) {
		mce_log(LL_WARN, "unlink(%s): %m", back);
		goto EXIT;
	}

	res = TRUE;

EXIT:
	if( temp && unlink(temp) == -1 && errno != ENOENT )
		mce_log(LL_WARN, "unlink(%s): %m", temp);

	g_free(back);
	g_free(temp);

	return res;
}

/** Atomically replace a file if existing file content differs from wanted
 *
 * The purpose of this function is to avoid unnecessary writes when
 * writing to filesystem is slow or otherwise undesired (flash wear out).
 *
 * @param path file to write to
 * @param data start of the data to write
 * @param size length of the data to write
 * @param mode protection bits to apply
 * @param keep_backup whether to keep backup file on successful update
 *
 * @return TRUE on success, FALSE on errors
 */

gboolean mce_io_update_file_atomic(const char *path,
				   const void *data, size_t size,
				   mode_t mode, gboolean keep_backup)
{
	gboolean res = FALSE;

	size_t old_size = 0;
	void  *old_data = 0;

	/* Skip write if the content would not change */
	if( (old_data = mce_io_load_file(path, &old_size)) ) {
		if( old_size == size && !memcmp(old_data, data, size) )	{
			res = TRUE;
			goto EXIT;
		}
	}

	res = mce_io_save_file_atomic(path, data, size, mode, keep_backup);

EXIT:
	g_free(old_data);

	return res;
}
