/**
 * @file mce-io.h
 * Headers for the generic I/O functionality for the Mode Control Entity
 * <p>
 * Copyright © 2007, 2009-2011 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _MCE_IO_H_
#define _MCE_IO_H_

#include <sys/stat.h>

#include <stdio.h>
#include <stdbool.h>

#include <glib.h>

/** Error policies for mce-io */
typedef enum {
	/** Exit on error */
	MCE_IO_ERROR_POLICY_EXIT,
	/** Warn about errors */
	MCE_IO_ERROR_POLICY_WARN,
	/** Silently ignore errors */
	MCE_IO_ERROR_POLICY_IGNORE
} error_policy_t;

/** Control structure for updating output files */
typedef struct {
	/* static configuration */

	/** descriptive context information used for identifying the
	 *  purpose of the output file even if no valid output path
	 *  is available */
	const gchar *context;

	/** TRUE to truncate the file before writing,
	 *  FALSE to append to the end of the file */
	gboolean truncate_file;

	/** TRUE to close the file on exit
	 *  [from mce_write_number_string_to_file() function],
	 *  FALSE to leave the file open */
	gboolean close_on_exit;

	/* runtime configuration */

	/** Path to the file, or NULL (in which case one misconfiguration
	 *  error message will be logged if write helpers are called) */
	const char *path;

	/* dynamic state */

	/** Cached output stream, use mce_close_output() to close */
	FILE *file;

	/** TRUE if missing path configuration error has already been
	 *  written for this file */
	gboolean invalid_config_reported;
} output_state_t;

typedef struct mce_io_mon_t mce_io_mon_t;

/** Callback function type for I/O monitor input notifications */
typedef gboolean (*mce_io_mon_notify_cb)(mce_io_mon_t *iomon, gpointer data, gsize bytes_read);

/** Callback function type for I/O monitor delete notifications */
typedef void (*mce_io_mon_delete_cb)(mce_io_mon_t *iomon);

/** Callback function type for releasing I/O monitor user data block */
typedef void (*mce_io_mon_free_cb)(void *user_data);

/* iomon functions */

mce_io_mon_t *mce_io_mon_register_string(const gint fd,
					 const gchar *const file,
					 error_policy_t error_policy,
					 gboolean rewind_policy,
					 mce_io_mon_notify_cb callback,
					 mce_io_mon_delete_cb delete_cb);

mce_io_mon_t *mce_io_mon_register_chunk(const gint fd,
					const gchar *const file,
					error_policy_t error_policy,
					gboolean rewind_policy,
					mce_io_mon_notify_cb callback,
					mce_io_mon_delete_cb delete_cb,
					gulong chunk_size);

void mce_io_mon_unregister(mce_io_mon_t *iomon);

void mce_io_mon_unregister_list(GSList *list);

void mce_io_mon_unregister_at_path(const char *path);

void mce_io_mon_suspend(mce_io_mon_t *iomon);

void mce_io_mon_resume(mce_io_mon_t *iomon);

const gchar *mce_io_mon_get_path(const mce_io_mon_t *iomon);

int mce_io_mon_get_fd(const mce_io_mon_t *iomon);

void mce_io_mon_set_user_data(mce_io_mon_t *iomon,
			      void *user_data,
			      mce_io_mon_free_cb free_cb);

void *mce_io_mon_get_user_data(const mce_io_mon_t *iomon);

/* output_state_t funtions */

void mce_close_output(output_state_t *output);

gboolean mce_write_number_string_to_file(output_state_t *output, const gulong number);

/* misc utils */

gboolean mce_close_file(const gchar *const file, FILE **fp);

gboolean mce_read_chunk_from_file(const gchar *const file, void **data,
				  gssize *len, int flags);

gboolean mce_read_string_from_file(const gchar *const file, gchar **string);

gboolean mce_read_number_string_from_file(const gchar *const file,
					  gulong *number, FILE **fp,
					  gboolean rewind,
					  gboolean close_on_exit);

gboolean mce_write_string_to_file(const gchar *const file,
				  const gchar *const string);

gboolean mce_write_number_string_to_file_atomic(const gchar *const file,
						const gulong number);

gboolean mce_are_settings_locked(void);

gboolean mce_unlock_settings(void);

void *mce_io_load_file(const char *path, size_t *psize);

void *mce_io_load_file_until_eof(const char *path, size_t *psize);

gboolean mce_io_save_file(const char *path,
			  const void *data, size_t size,
			  mode_t mode);

gboolean mce_io_save_to_existing_file(const char *path,
				      const void *data, size_t size);

gboolean mce_io_save_file_atomic(const char *path,
				 const void *data, size_t size,
				 mode_t mode, gboolean keep_backup);

gboolean mce_io_update_file_atomic(const char *path,
				   const void *data, size_t size,
				   mode_t mode, gboolean keep_backup);

/* Resume from suspend detection */
void mce_io_init_resume_timer(void);
void mce_io_quit_resume_timer(void);

guint mce_io_add_watch(int fd, bool close_on_unref, GIOCondition cnd, GIOFunc io_cb, gpointer aptr);
const char *mce_io_condition_repr (GIOCondition cond);
const char *mce_io_status_name    (GIOStatus io_status);

#endif /* _MCE_IO_H_ */
