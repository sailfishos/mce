/**
 * @file datapipe.h
 * Headers for the simple filter framework
 * <p>
 * Copyright © 2007 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _DATAPIPE_H_
#define _DATAPIPE_H_

#include <glib.h>

/**
 * Datapipe structure
 *
 * Only access this struct through the functions
 */
typedef struct {
	GSList *filters;		/**< The filters */
	GSList *input_triggers;		/**< Triggers called on indata */
	GSList *output_triggers;	/**< Triggers called on outdata */
	GSList *refcount_triggers;	/**< Triggers called on
					 *   reference count changes
					 */
	gpointer cached_data;		/**< Latest cached data */
	gsize datasize;			/**< Size of data; NULL == automagic */
	gboolean free_cache;		/**< Free the cache? */
	gboolean read_only;		/**< Datapipe is read only */
} datapipe_struct;

/**
 * Read only policy type
 */
typedef enum {
	READ_WRITE = FALSE,		/**< The pipe is read/write */
	READ_ONLY = TRUE		/**< The pipe is read only */
} read_only_policy_t;

/**
 * Policy used for the cache when freeing a datapipe
 */
typedef enum {
	DONT_FREE_CACHE = FALSE,	/**< Don't free the cache */
	FREE_CACHE = TRUE		/**< Free the cache */
} cache_free_policy_t;

/**
 * Policy for the data source
 */
typedef enum {
	USE_INDATA = FALSE,		/**< Use the indata as data source */
	USE_CACHE = TRUE		/**< Use the cache as data source */
} data_source_t;

/**
 * Policy used for caching indata
 */
typedef enum {
	DONT_CACHE_INDATA = FALSE,	/**< Do not cache the indata */
	CACHE_INDATA = TRUE		/**< Cache the indata */
} caching_policy_t;

/* Data retrieval */

/** Retrieve a gboolean from a datapipe */
#define datapipe_get_gbool(_datapipe)	(GPOINTER_TO_INT((_datapipe).cached_data))
/** Retrieve a gint from a datapipe */
#define datapipe_get_gint(_datapipe)	(GPOINTER_TO_INT((_datapipe).cached_data))
/** Retrieve a guint from a datapipe */
#define datapipe_get_guint(_datapipe)	(GPOINTER_TO_UINT((_datapipe).cached_data))
/** Retrieve a gsize from a datapipe */
#define datapipe_get_gsize(_datapipe)	(GPOINTER_TO_SIZE((_datapipe).cached_data))
/** Retrieve a gpointer from a datapipe */
#define datapipe_get_gpointer(_datapipe)	((_datapipe).cached_data)

/* Reference count */

/** Retrieve the filter reference count from a datapipe */
#define datapipe_get_filter_refcount(_datapipe)	(g_slist_length((_datapipe).filters))
/** Retrieve the input trigger reference count from a datapipe */
#define datapipe_get_input_trigger_refcount(_datapipe)	(g_slist_length((_datapipe).input_triggers))
/** Retrieve the output trigger reference count from a datapipe */
#define datapipe_get_output_trigger_refcount(_datapipe)	(g_slist_length((_datapipe).output_triggers))

/* Datapipe execution */
void execute_datapipe_input_triggers(datapipe_struct *const datapipe,
				     gpointer const indata,
				     const data_source_t use_cache,
				     const caching_policy_t cache_indata);
gconstpointer execute_datapipe_filters(datapipe_struct *const datapipe,
				       gpointer indata,
				       const data_source_t use_cache);
void execute_datapipe_output_triggers(const datapipe_struct *const datapipe,
				      gconstpointer indata,
				      const data_source_t use_cache);
gconstpointer execute_datapipe(datapipe_struct *const datapipe,
			       gpointer indata,
			       const data_source_t use_cache,
			       const caching_policy_t cache_indata);

/* Filters */
void append_filter_to_datapipe(datapipe_struct *const datapipe,
			       gpointer (*filter)(gpointer data));
void remove_filter_from_datapipe(datapipe_struct *const datapipe,
				 gpointer (*filter)(gpointer data));

/* Input triggers */
void append_input_trigger_to_datapipe(datapipe_struct *const datapipe,
				      void (*trigger)(gconstpointer data));
void remove_input_trigger_from_datapipe(datapipe_struct *const datapipe,
					void (*trigger)(gconstpointer data));

/* Output triggers */
void append_output_trigger_to_datapipe(datapipe_struct *const datapipe,
				       void (*trigger)(gconstpointer data));
void remove_output_trigger_from_datapipe(datapipe_struct *const datapipe,
					 void (*trigger)(gconstpointer data));

/* Reference count triggers */
void append_refcount_trigger_to_datapipe(datapipe_struct *const datapipe,
					 void (*trigger)(void));
void remove_refcount_trigger_from_datapipe(datapipe_struct *const datapipe,
					   void (*trigger)(void));

void setup_datapipe(datapipe_struct *const datapipe,
		    const read_only_policy_t read_only,
		    const cache_free_policy_t free_cache,
		    const gsize datasize, gpointer initial_data);
void free_datapipe(datapipe_struct *const datapipe);

#endif /* _DATAPIPE_H_ */
