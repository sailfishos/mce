/**
 * @file mce-io.c
 * Generic I/O functionality for the Mode Control Entity
 * <p>
 * Copyright © 2006-2011 Nokia Corporation and/or its subsidiary(-ies).
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
#include <glib/gstdio.h>		/* g_access(), g_unlink() */

#include <errno.h>			/* errno, EINVAL, ERANGE */
#include <fcntl.h>			/* open(), O_RDONLY */
#include <stdio.h>			/* fopen(), fscanf(), fseek(),
                                         * fclose(), fprintf(), fileno(),
					 * fflush()
					 */
#include <stdlib.h>			/* exit(), strtoul(), EXIT_FAILURE */
#include <string.h>			/* strlen() */
#include <unistd.h>			/* close(), read(), ftruncate() */

#include "mce.h"
#include "mce-io.h"

#include "mce-log.h"			/* mce_log(), LL_* */

#ifdef ENABLE_WAKELOCKS
# include "libwakelock.h"		/* API for wakelocks */
#endif

/** List of all file monitors */
static GSList *file_monitors = NULL;

/** I/O monitor type */
typedef enum {
	IOMON_UNSET = -1,			/**< I/O monitor type unset */
	IOMON_STRING = 0,			/**< String I/O monitor */
	IOMON_CHUNK = 1				/**< Chunk I/O monitor */
} iomon_type;

/** I/O monitor structure */
typedef struct {
	gchar *file;				/**< Monitored file */
	GIOChannel *iochan;			/**< I/O channel */
	iomon_cb callback;			/**< Callback */
	iomon_err_cb err_callback;	/**< error callback */
	gulong chunk_size;			/**< Read-chunk size */
	guint data_source_id;			/**< GSource ID for data */
	guint error_source_id;			/**< GSource ID for errors */
	gint fd;				/**< File Descriptor */
	iomon_type type;			/**< Monitor type */
	error_policy_t error_policy;		/**< Error policy */
	GIOCondition monitored_io_conditions;	/**< Conditions to monitor */
	GIOCondition latest_io_condition;	/**< Latest I/O condition */
	gboolean rewind;			/**< Rewind policy */
	gboolean suspended;			/**< Is the I/O monitor
						 *   suspended? */
	gboolean seekable;			/**< is the I/O channel seekable */
} iomon_struct;

/** Suffix used for temporary files */
#define TMP_SUFFIX				".tmp"

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
		(void)mce_close_file(file, &new_fp);

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
	(void)mce_close_file(file, &fp);

EXIT:
	return status;
}

/**
 * Write a string representation of a number to a file
 *
 * Note: this variant uses in-place rewrites when truncating.
 * It should thus not be used in cases where atomicity is expected.
 * For atomic replace, use mce_write_number_string_to_file_atomic()
 *
 * @param file Path to the file, or NULL to user an already open FILE * instead
 * @param number The number to write
 * @param fp A pointer to a FILE *; set the FILE * to NULL to use the file
 *           path instead
 * @param truncate_file TRUE to truncate the file before writing,
 *                      FALSE to append to the end of the file
 * @param close_on_exit TRUE to close the file on exit,
 *                      FALSE to leave the file open
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_write_number_string_to_file(const gchar *const file,
					 const gulong number, FILE **fp,
					 gboolean truncate_file,
					 gboolean close_on_exit)
{
	gboolean status = FALSE;
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
		if ((new_fp = fopen(file, truncate_file ? "w" : "a")) == NULL) {
			mce_log(LL_ERR,
				"Cannot open `%s' for %s; %s",
				file,
				truncate_file ? "writing" : "appending",
				g_strerror(errno));

			/* Ignore error */
			errno = 0;
			goto EXIT;
		}
	} else {
		new_fp = *fp;
	}

	/* Truncate file if we already have one */
	if ((fp != NULL) && (*fp != NULL) && (truncate_file == TRUE)) {
		int fd = fileno(*fp);

		if (fd == -1) {
			mce_log(LL_ERR,
				"Failed to convert *fp to fd; %s",
				g_strerror(errno));

			/* Ignore error */
			errno = 0;
			goto EXIT2;
		}

		if (ftruncate(fd, 0L) == -1) {
			mce_log(LL_ERR,
				"Failed to truncate `%s'; %s",
				file, g_strerror(errno));

			/* Ignore error */
			errno = 0;
			goto EXIT2;
		}
	}

	if ((fp != NULL) && (*fp == NULL))
		*fp = new_fp;

	retval = fprintf(new_fp, "%lu", number);

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
	/* XXX: improve close policy? */
	if ((status == FALSE) || (close_on_exit == TRUE)) {
		(void)mce_close_file(file, &new_fp);

		if (fp != NULL)
			*fp = NULL;
	} else {
		fflush(*fp);
	}

	/* Ignore error */
	errno = 0;

EXIT:
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
 * Callback for successful string I/O
 *
 * @param source The source of the activity
 * @param condition The I/O condition
 * @param data The iomon structure
 * @return Depending on error policy this function either exits
 *         or returns TRUE
 */
static gboolean io_string_cb(GIOChannel *source,
			     GIOCondition condition,
			     gpointer data)
{
	iomon_struct *iomon = data;
	gchar *str = NULL;
	gsize bytes_read;
	GError *error = NULL;
	gboolean status = TRUE;

	/* Silence warnings */
	(void)condition;

	if (iomon == NULL) {
		mce_log(LL_CRIT, "iomon == NULL!");
		status = FALSE;
		goto EXIT;
	}

	iomon->latest_io_condition = 0;

	/* Seek to the beginning of the file before reading if needed */
	if (iomon->rewind == TRUE) {
		g_io_channel_seek_position(source, 0, G_SEEK_SET, &error);

		if( error ) {
			mce_log(LL_ERR,	"%s: seek error: %s",
				iomon->file, error->message);
		}
		/* Reset errno,
		 * to avoid false positives down the line
		 */
		errno = 0;
		g_clear_error(&error);
	}

	g_io_channel_read_line(source, &str, &bytes_read, NULL, &error);

	/* Errors and empty reads are nasty */
	if (error != NULL) {
		mce_log(LL_ERR,
			"Error when reading from %s: %s",
			iomon->file, error->message);
		status = FALSE;
	} else if ((bytes_read == 0) || (str == NULL) || (strlen(str) == 0)) {
		mce_log(LL_ERR,
			"Empty read from %s",
			iomon->file);
	} else {
		(void)iomon->callback(str, bytes_read);
	}

	g_free(str);

	/* Reset errno,
	 * to avoid false positives down the line
	 */
	errno = 0;
	g_clear_error(&error);

EXIT:
	if ((status == FALSE) &&
	    (iomon != NULL) &&
	    (iomon->error_policy == MCE_IO_ERROR_POLICY_EXIT)) {
		g_main_loop_quit(mainloop);
		exit(EXIT_FAILURE);
	}

	return TRUE;
}
/**
 * Get glib io status as human readable string
 *
 * @param io_status as returned from g_io_channel_read_chars()
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

/**
 * Callback for successful chunk I/O
 *
 * @param source The source of the activity
 * @param condition The I/O condition
 * @param data The iomon structure
 * @return Depending on error policy this function either exits
 *         or returns TRUE
 */
static gboolean io_chunk_cb(GIOChannel *source,
			    GIOCondition condition,
			    gpointer data)
{
	iomon_struct *iomon = data;
	gchar *buffer = NULL;
	gsize bytes_want = 4096;
	gsize bytes_read = 0;
	gsize chunks_read = 0;
	gsize chunks_done = 0;
	GIOStatus io_status;
	GError *error = NULL;
	gboolean status = TRUE;

	/* Silence warnings */
	(void)condition;

	if (iomon == NULL) {
		mce_log(LL_CRIT, "iomon == NULL!");
		status = FALSE;
		goto EXIT;
	}

	iomon->latest_io_condition = 0;

	/* Seek to the beginning of the file before reading if needed */
	if (iomon->rewind == TRUE) {
		g_io_channel_seek_position(source, 0, G_SEEK_SET, &error);
		if( error ) {
			mce_log(LL_ERR,	"%s: seek error: %s",
				iomon->file, error->message);
		}

		/* Reset errno,
		 * to avoid false positives down the line
		 */
		errno = 0;
		g_clear_error(&error);
	}

	if( iomon->chunk_size < bytes_want ) {
		bytes_want -= bytes_want % iomon->chunk_size;
	} else {
		bytes_want -= iomon->chunk_size;
	}

	buffer = g_malloc(bytes_want);

#ifdef ENABLE_WAKELOCKS
	/* Since the locks on kernel side are released once all
	 * events are read, we must obtain the userspace lock
	 * before reading the available data */
	wakelock_lock("mce_input_handler", -1);
#endif

	io_status = g_io_channel_read_chars(source, buffer,
					    bytes_want, &bytes_read, &error);


	/* If the read was interrupted, ignore */
	if (io_status == G_IO_STATUS_AGAIN) {
		g_clear_error(&error);
	}

	if( bytes_read % iomon->chunk_size ) {
		mce_log(LL_WARN, "Incomplete chunks read from: %s", iomon->file);
	}

	/* Process the data, and optionally ignore some of it */
	if( (chunks_read = bytes_read / iomon->chunk_size) ) {
		gchar *chunk = buffer;
		for( ; chunks_done < chunks_read ; chunk += iomon->chunk_size ) {
			++chunks_done;
			if (iomon->callback(chunk, iomon->chunk_size) != TRUE) {
				continue;
			}
			/* if possible, seek to the end of file */
			if (iomon->seekable) {
				g_io_channel_seek_position(iomon->iochan, 0,
							   G_SEEK_END, &error);
			}
			/* in any case ignore rest of the data already read */
			break;
		}
	}

	mce_log(LL_INFO, "%s: status=%s, data=%d/%d=%d+%d, skipped=%d",
		iomon->file, io_status_name(io_status),
		bytes_read, (int)iomon->chunk_size, chunks_read,
		bytes_read % (int)iomon->chunk_size, chunks_read - chunks_done);

#ifdef ENABLE_WAKELOCKS
	/* Release the lock after we're done with processing it */
	wakelock_unlock("mce_input_handler");
#endif


	g_free(buffer);

	/* Were there any errors? */
	if (error != NULL) {
		mce_log(LL_ERR,
			"Error when reading from %s: %s",
			iomon->file, error->message);

		if ((error->code == G_IO_CHANNEL_ERROR_FAILED) &&
		    (errno == ENODEV) &&
		    (iomon->seekable)) {
			errno = 0;
			g_clear_error(&error);
			g_io_channel_seek_position(iomon->iochan, 0,
						   G_SEEK_END, &error);
			if( error ) {
				mce_log(LL_ERR,	"%s: seek error: %s",
					iomon->file, error->message);
			}
		} else {
			status = FALSE;
		}

		/* Reset errno,
		 * to avoid false positives down the line
		 */
		errno = 0;
		g_clear_error(&error);
	} else if ((bytes_read == 0) &&
		   (io_status != G_IO_STATUS_EOF) &&
		   (io_status != G_IO_STATUS_AGAIN)) {
		mce_log(LL_ERR,
			"Empty read from %s",
			iomon->file);
	}

EXIT:
	if ((status == FALSE) &&
	    (iomon != NULL) &&
	    (iomon->error_policy == MCE_IO_ERROR_POLICY_EXIT)) {
		g_main_loop_quit(mainloop);
		exit(EXIT_FAILURE);
	}

	return TRUE;
}

/**
 * Callback for I/O errors
 *
 * @param source Unused
 * @param condition The GIOCondition for the error
 * @param data The iomon structure
 * @return Depending on error policy this function either exits
 *         or returns TRUE
 */
static gboolean io_error_cb(GIOChannel *source,
			    GIOCondition condition,
			    gpointer data)
{
	iomon_struct *iomon = data;
	gboolean exit_on_error = FALSE;
	loglevel_t loglevel;

	/* Silence warnings */
	(void)source;

	if (iomon == NULL) {
		mce_log(LL_CRIT, "iomon == NULL!");
		goto EXIT;
	}

	switch (iomon->error_policy) {
	case MCE_IO_ERROR_POLICY_EXIT:
		exit_on_error = TRUE;
		loglevel = LL_CRIT;
		break;

	case MCE_IO_ERROR_POLICY_WARN:
		loglevel = LL_WARN;
		break;

	case MCE_IO_ERROR_POLICY_IGNORE:
	default:
		/* No log message when ignoring errors */
		loglevel = LL_NONE;
		break;
	}

	/* We just got an I/O condition we've already reported
	 * since the last successful read; don't log
	 */
	if ((exit_on_error == FALSE) &&
	    ((iomon->latest_io_condition & condition) == condition)) {
		loglevel = LL_NONE;
	} else {
		iomon->latest_io_condition |= condition;
	}

	if (loglevel != LL_NONE) {
		mce_log(loglevel,
			"Error accessing %s (condition: %d). %s",
			iomon->file, condition,
			(exit_on_error == TRUE) ? "Exiting" : "Ignoring");
	}

EXIT:
	if ((iomon != NULL) && (exit_on_error == TRUE)) {
		g_main_loop_quit(mainloop);
		exit(EXIT_FAILURE);
	}

	/* Call error callback if set */
	if (iomon->err_callback) {
		iomon->err_callback(iomon, condition);
	}

	return TRUE;
}

/**
 * Suspend an I/O monitor
 *
 * @param io_monitor A pointer to the I/O monitor to suspend
 */
void mce_suspend_io_monitor(gconstpointer io_monitor)
{
	iomon_struct *iomon = (iomon_struct *)io_monitor;

	if (iomon == NULL) {
		mce_log(LL_CRIT, "iomon == NULL!");
		goto EXIT;
	}

	if (iomon->suspended == TRUE)
		goto EXIT;

	/* Remove I/O watches */
	g_source_remove(iomon->data_source_id);
	g_source_remove(iomon->error_source_id);

	iomon->suspended = TRUE;

EXIT:
	return;
}

/**
 * Resume an I/O monitor
 *
 * @param io_monitor A pointer to the I/O monitor to resume
 */
void mce_resume_io_monitor(gconstpointer io_monitor)
{
	iomon_struct *iomon = (iomon_struct *)io_monitor;
	GIOFunc callback = NULL;

	if (iomon == NULL) {
		mce_log(LL_CRIT, "iomon == NULL!");
		goto EXIT;
	}

	if (iomon->suspended == FALSE)
		goto EXIT;

	switch (iomon->type) {
	case IOMON_STRING:
		callback = io_string_cb;
		break;

	case IOMON_CHUNK:
		callback = io_chunk_cb;
		break;

	case IOMON_UNSET:
	default:
		break;
	}

	if (callback != NULL) {
		GError *error = NULL;

		/* Seek to the end of the file if the file is seekable,
		 * unless we use the rewind policy
		 */
		if (iomon->seekable && !iomon->rewind) {
			g_io_channel_seek_position(iomon->iochan, 0,
						   G_SEEK_END, &error);
			if( error ) {
				mce_log(LL_ERR,	"%s: seek error: %s",
					iomon->file, error->message);
			}
			/* Reset errno,
			 * to avoid false positives down the line
			 */
			errno = 0;
			g_clear_error(&error);
		}

		iomon->error_source_id = g_io_add_watch(iomon->iochan,
							G_IO_HUP | G_IO_NVAL,
							io_error_cb, iomon);
		iomon->data_source_id = g_io_add_watch(iomon->iochan,
						       iomon->monitored_io_conditions,
						       callback, iomon);
		iomon->suspended = FALSE;
	} else {
		mce_log(LL_ERR,
			"Failed to resume `%s'; invalid callback",
			iomon->file);
	}

EXIT:
	return;
}
/**
 * Check if the monitored io channel is truly seekable
 *
 * Glib seems to be making guesses based on file type and
 * gets it massively wrong for the files MCE needs to read.
 */

static void mce_determine_io_monitor_seekable(iomon_struct *iomon)
{
	gboolean glib = FALSE, kernel = FALSE;

	/* glib assumes ... */
	if (g_io_channel_get_flags(iomon->iochan) & G_IO_FLAG_IS_SEEKABLE) {
		glib = TRUE;
	}
	/* ... kernel knows */
	if (lseek64(g_io_channel_unix_get_fd(iomon->iochan), 0, SEEK_CUR) != -1) {
		kernel = TRUE;
	}
	/* report the difference */
	if (kernel != glib) {
		mce_log(LL_WARN, "%s: is %sseekable, while glib thinks it is %sseekable",
			iomon->file, kernel ? "" : "NOT ", glib ? "" : "NOT ");
	}

	iomon->seekable = kernel;
}


/**
 * Register an I/O monitor; reads and returns data
 *
 * @param fd File Descriptor; this takes priority over file; -1 if not used
 * @param file Path to the file
 * @param error_policy MCE_IO_ERROR_POLICY_EXIT to exit on error,
 *                     MCE_IO_ERROR_POLICY_WARN to warn about errors
 *                                              but ignore them,
 *                     MCE_IO_ERROR_POLICY_IGNORE to silently ignore errors
 * @param monitored_conditions The GIOConditions to monitor
 * @param callback Function to call with result
 * @return An I/O monitor pointer on success, NULL on failure
 */
static iomon_struct *mce_register_io_monitor(const gint fd,
					     const gchar *const file,
					     error_policy_t error_policy,
					     GIOCondition monitored_conditions,
					     iomon_cb callback)
{
	iomon_struct *iomon = NULL;
	GIOChannel *iochan = NULL;
	GError *error = NULL;

	if (file == NULL) {
		mce_log(LL_CRIT, "file == NULL!");
		goto EXIT;
	}

	if (callback == NULL) {
		mce_log(LL_CRIT, "callback == NULL!");
		goto EXIT;
	}

	if ((iomon = g_slice_new(iomon_struct)) == NULL) {
		mce_log(LL_CRIT,
			"Failed to allocate memory for "
			"iomon_struct (%zd bytes)",
			sizeof (*iomon));
		goto EXIT;
	}

	if (fd != -1) {
		if ((iochan = g_io_channel_unix_new(fd)) == NULL) {
			/* XXX: this is probably not good either;
			 * we should only ignore non-existing files
			 */
			if (error_policy != MCE_IO_ERROR_POLICY_IGNORE)
				mce_log(LL_ERR, "Failed to open `%s'", file);

			g_slice_free(iomon_struct, iomon);
			iomon = NULL;
			goto EXIT;
		}
	} else {
		if ((iochan = g_io_channel_new_file(file, "r",
						   &error)) == NULL) {
			/* XXX: this is probably not good either;
			 * we should only ignore non-existing files
			 */
			if (error_policy != MCE_IO_ERROR_POLICY_IGNORE)
				mce_log(LL_ERR,
					"Failed to open `%s'; %s",
					file, error->message);

			g_slice_free(iomon_struct, iomon);
			iomon = NULL;
			goto EXIT;
		}
	}

	iomon->fd = fd;
	iomon->file = g_strdup(file);
	iomon->iochan = iochan;
	iomon->callback = callback;
	iomon->error_policy = error_policy;
	iomon->monitored_io_conditions = monitored_conditions;
	iomon->latest_io_condition = 0;
	iomon->rewind = FALSE;
	iomon->chunk_size = 0;
	iomon->err_callback = 0;

	mce_determine_io_monitor_seekable(iomon);

	file_monitors = g_slist_prepend(file_monitors, iomon);

	iomon->suspended = TRUE;

EXIT:
	/* Reset errno,
	 * to avoid false positives down the line
	 */
	errno = 0;
	g_clear_error(&error);

	return iomon;
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
 * @param monitored_conditions The GIOConditions to monitor
 * @param rewind_policy TRUE to seek to the beginning,
 *                      FALSE to stay at current position
 * @param callback Function to call with result
 * @return An I/O monitor cookie on success, NULL on failure
 */
gconstpointer mce_register_io_monitor_string(const gint fd,
					     const gchar *const file,
					     error_policy_t error_policy,
					     GIOCondition monitored_conditions,
					     gboolean rewind_policy,
					     iomon_cb callback)
{
	iomon_struct *iomon = NULL;

	iomon = mce_register_io_monitor(fd, file, error_policy, monitored_conditions, callback);

	if (iomon == NULL)
		goto EXIT;

	/* Verify that the rewind policy is sane */
	if (iomon->seekable) {
		/* Set the rewind policy */
		iomon->rewind = rewind_policy;
	} else if (rewind_policy == TRUE) {
		mce_log(LL_ERR,
			"Attempting to set rewind policy to TRUE "
			"on non-seekable I/O channel `%s'",
			file);
		iomon->rewind = FALSE;
	}

	/* Set the I/O monitor type and call resume to add an I/O watch */
	iomon->type = IOMON_STRING;
	mce_resume_io_monitor(iomon);

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
 * @param monitored_conditions The GIOConditions to monitor
 * @param rewind_policy TRUE to seek to the beginning,
 *                      FALSE to stay at current position
 * @param callback Function to call with result
 * @param chunk_size The number of bytes to read in each chunk
 * @return An I/O monitor cookie on success, NULL on failure
 */
gconstpointer mce_register_io_monitor_chunk(const gint fd,
					    const gchar *const file,
					    error_policy_t error_policy,
					    GIOCondition monitored_conditions,
					    gboolean rewind_policy,
					    iomon_cb callback,
					    gulong chunk_size)
{
	iomon_struct *iomon = NULL;
	GError *error = NULL;

	iomon = mce_register_io_monitor(fd, file, error_policy, monitored_conditions, callback);

	if (iomon == NULL)
		goto EXIT;

	/* Set the read chunk size */
	iomon->chunk_size = chunk_size;

	/* Verify that the rewind policy is sane */
	if (iomon->seekable) {
		/* Set the rewind policy */
		iomon->rewind = rewind_policy;
	} else if (rewind_policy == TRUE) {
		mce_log(LL_ERR,
			"Attempting to set rewind policy to TRUE "
			"on non-seekable I/O channel `%s'",
			file);
		iomon->rewind = FALSE;
	}

	/* We only read this file in binary form */
	(void)g_io_channel_set_encoding(iomon->iochan, NULL, &error);

	/* No buffering since we're using this for reading data from
	 * device drivers and need to keep the i/o state in sync
	 * between kernel and user space for the automatic suspend
	 * prevention via wakelocks to work
	 */
	g_io_channel_set_buffered(iomon->iochan, FALSE);

	/* Reset errno,
	 * to avoid false positives down the line
	 */
	errno = 0;
	g_clear_error(&error);

	/* Don't block */
	(void)g_io_channel_set_flags(iomon->iochan, G_IO_FLAG_NONBLOCK, &error);

	/* Reset errno,
	 * to avoid false positives down the line
	 */
	errno = 0;
	g_clear_error(&error);

	/* Set the I/O monitor type and call resume to add an I/O watch */
	iomon->type = IOMON_CHUNK;
	mce_resume_io_monitor(iomon);

EXIT:
	return iomon;
}

/**
 * Unregister an I/O monitor
 * Note: This does NOT shutdown I/O channels created from file descriptors
 *
 * @param io_monitor A pointer to the I/O monitor to unregister
 */
void mce_unregister_io_monitor(gconstpointer io_monitor)
{
	iomon_struct *iomon = (iomon_struct *)io_monitor;
	guint oldlen;

	if (iomon == NULL) {
		mce_log(LL_DEBUG, "iomon == NULL!");
		goto EXIT;
	}

	oldlen = g_slist_length(file_monitors);

	if (file_monitors != NULL)
		file_monitors = g_slist_remove(file_monitors, iomon);

	/* Did we remove any entry? */
	if (oldlen == g_slist_length(file_monitors)) {
		mce_log(LL_WARN,
			"Trying to unregister non-existing file monitor");
	}

	/* Remove I/O watches */
	mce_suspend_io_monitor(iomon);

	/* We can close this I/O channel, since it's not an external fd */
	if (iomon->fd == -1) {
		GIOStatus iostatus;
		GError *error = NULL;

		iostatus = g_io_channel_shutdown(iomon->iochan, TRUE, &error);

		if (iostatus != G_IO_STATUS_NORMAL) {
			loglevel_t loglevel = LL_ERR;

			/* If we get ENODEV, only log a debug message,
			 * since this happens for hotpluggable
			 * /dev/input files
			 */
			if ((error->code == G_IO_CHANNEL_ERROR_FAILED) &&
			    (errno == ENODEV))
				loglevel = LL_DEBUG;

			mce_log(loglevel,
				"Cannot close `%s'; %s",
				iomon->file, error->message);
		}

		/* Reset errno,
		 * to avoid false positives down the line
		 */
		errno = 0;
		g_clear_error(&error);
	}

	g_io_channel_unref(iomon->iochan);
	g_free(iomon->file);
	g_slice_free(iomon_struct, iomon);

EXIT:
	return;
}

/**
 * Set error handling callback for I/O monitor. Error handling callback
 * is called from io_error_cb.
 *
 * @param io_monitor A pointer to the I/O monitor
 * @param err_cb A pointer to the error callback. Can be 0 to unset the cb.
 */
void mce_set_io_monitor_err_cb(gconstpointer io_monitor, iomon_err_cb err_cb)
{
	iomon_struct *iomon = (iomon_struct *)io_monitor;

	if (iomon) {
		iomon->err_callback = err_cb;
	}
}

/**
 * Return the name of the monitored file
 *
 * @param io_monitor An opaque pointer to the I/O monitor structure
 * @return The name of the monitored file
 */
const gchar *mce_get_io_monitor_name(gconstpointer io_monitor)
{
	iomon_struct *iomon = (iomon_struct *)io_monitor;

	return iomon->file;
}

/**
 * Return the file descriptor of the monitored file;
 * if the file being monitored was opened from a path
 * rather than a file descriptor, -1 is returned
 *
 * @param io_monitor An opaque pointer to the I/O monitor structure
 * @return The file descriptor of the monitored file
 */
int mce_get_io_monitor_fd(gconstpointer io_monitor)
{
	iomon_struct *iomon = (iomon_struct *)io_monitor;

	return iomon->fd;
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
