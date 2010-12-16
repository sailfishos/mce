/**
 * @file datapipe.c
 * This file implements the sinmple datapipe framework;
 * this can be used to filter data and to setup data triggers
 * <p>
 * Copyright © 2007-2008 Nokia Corporation and/or its subsidiary(-ies).
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

#include "datapipe.h"

#include "mce-log.h"			/* mce_log(), LL_* */

/**
 * Execute the input triggers of a datapipe
 *
 * @param datapipe The datapipe to execute
 * @param indata The input data to run through the datapipe
 * @param use_cache USE_CACHE to use data from cache,
 *                  USE_INDATA to use indata
 * @param cache_indata CACHE_INDATA to cache the indata,
 *                     DONT_CACHE_INDATA to keep the old data
 */
void execute_datapipe_input_triggers(datapipe_struct *const datapipe,
				     gpointer const indata,
				     const data_source_t use_cache,
				     const caching_policy_t cache_indata)
{
	void (*trigger)(gconstpointer const input);
	gpointer data;
	gint i;

	if (datapipe == NULL) {
		/* Potential memory leak! */
		mce_log(LL_ERR,
			"execute_datapipe_input_triggers() called "
			"without a valid datapipe");
		goto EXIT;
	}

	data = (use_cache == USE_CACHE) ? datapipe->cached_data : indata;

	if (cache_indata == CACHE_INDATA) {
		if (use_cache == USE_INDATA) {
			if (datapipe->free_cache == FREE_CACHE)
				g_free(datapipe->cached_data);

			datapipe->cached_data = data;
		}
	}

	for (i = 0; (trigger = g_slist_nth_data(datapipe->input_triggers,
						i)) != NULL; i++) {
		trigger(data);
	}

EXIT:
	return;
}

/**
 * Execute the filters of a datapipe
 *
 * @param datapipe The datapipe to execute
 * @param indata The input data to run through the datapipe
 * @param use_cache USE_CACHE to use data from cache,
 *                  USE_INDATA to use indata
 * @return The processed data
 */
gconstpointer execute_datapipe_filters(datapipe_struct *const datapipe,
				       gpointer indata,
				       const data_source_t use_cache)
{
	gpointer (*filter)(gpointer input);
	gpointer data;
	gconstpointer retval = NULL;
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"execute_datapipe_filters() called "
			"without a valid datapipe");
		goto EXIT;
	}

	data = (use_cache == USE_CACHE) ? datapipe->cached_data : indata;

	for (i = 0; (filter = g_slist_nth_data(datapipe->filters,
					       i)) != NULL; i++) {
		gpointer tmp = filter(data);

		/* If the data needs to be freed, and this isn't the indata,
		 * or if we're not using the cache, then free the data
		 */
		if ((datapipe->free_cache == FREE_CACHE) &&
		    ((i > 0) || (use_cache == USE_INDATA)))
			g_free(data);

		data = tmp;
	}

	retval = data;

EXIT:
	return retval;
}

/**
 * Execute the output triggers of a datapipe
 *
 * @param datapipe The datapipe to execute
 * @param indata The input data to run through the datapipe
 * @param use_cache USE_CACHE to use data from cache,
 *                  USE_INDATA to use indata
 */
void execute_datapipe_output_triggers(const datapipe_struct *const datapipe,
				      gconstpointer indata,
				      const data_source_t use_cache)
{
	void (*trigger)(gconstpointer input);
	gconstpointer data;
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"execute_datapipe_output_triggers() called "
			"without a valid datapipe");
		goto EXIT;
	}

	data = (use_cache == USE_CACHE) ? datapipe->cached_data : indata;

	for (i = 0; (trigger = g_slist_nth_data(datapipe->output_triggers,
						i)) != NULL; i++) {
		trigger(data);
	}

EXIT:
	return;
}

/**
 * Execute the datapipe
 *
 * @param datapipe The datapipe to execute
 * @param indata The input data to run through the datapipe
 * @param use_cache USE_CACHE to use data from cache,
 *                  USE_INDATA to use indata
 * @param cache_indata CACHE_INDATA to cache the indata,
 *                     DONT_CACHE_INDATA to keep the old data
 * @return The processed data
 */
gconstpointer execute_datapipe(datapipe_struct *const datapipe,
			       gpointer indata,
			       const data_source_t use_cache,
			       const caching_policy_t cache_indata)
{
	gconstpointer data = NULL;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"execute_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	execute_datapipe_input_triggers(datapipe, indata, use_cache,
					cache_indata);

	if (datapipe->read_only == READ_ONLY) {
		data = indata;
	} else {
		data = execute_datapipe_filters(datapipe, indata, use_cache);
	}

	execute_datapipe_output_triggers(datapipe, data, USE_INDATA);

EXIT:
	return data;
}

/**
 * Append a filter to an existing datapipe
 *
 * @param datapipe The datapipe to manipulate
 * @param filter The filter to add to the datapipe
 */
void append_filter_to_datapipe(datapipe_struct *const datapipe,
			       gpointer (*filter)(gpointer data))
{
	void (*refcount_trigger)(void);
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"append_filter_to_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (filter == NULL) {
		mce_log(LL_ERR,
			"append_filter_to_datapipe() called "
			"without a valid filter");
		goto EXIT;
	}

	if (datapipe->read_only == READ_ONLY) {
		mce_log(LL_ERR,
			"append_filter_to_datapipe() called "
			"on read only datapipe");
		goto EXIT;
	}

	datapipe->filters = g_slist_append(datapipe->filters, filter);

	for (i = 0; (refcount_trigger = g_slist_nth_data(datapipe->refcount_triggers, i)) != NULL; i++) {
		refcount_trigger();
	}

EXIT:
	return;
}

/**
 * Remove a filter from an existing datapipe
 * Non-existing filters are ignored
 *
 * @param datapipe The datapipe to manipulate
 * @param filter The filter to remove from the datapipe
 */
void remove_filter_from_datapipe(datapipe_struct *const datapipe,
				 gpointer (*filter)(gpointer data))
{
	void (*refcount_trigger)(void);
	guint oldlen;
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"remove_filter_from_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (filter == NULL) {
		mce_log(LL_ERR,
			"remove_filter_from_datapipe() called "
			"without a valid filter");
		goto EXIT;
	}

	if (datapipe->read_only == READ_ONLY) {
		mce_log(LL_ERR,
			"remove_filter_from_datapipe() called "
			"on read only datapipe");
		goto EXIT;
	}

	oldlen = g_slist_length(datapipe->filters);

	datapipe->filters = g_slist_remove(datapipe->filters, filter);

	/* Did we remove any entry? */
	if (oldlen == g_slist_length(datapipe->filters)) {
		mce_log(LL_DEBUG,
			"Trying to remove non-existing filter");
		goto EXIT;
	}

	for (i = 0; (refcount_trigger = g_slist_nth_data(datapipe->refcount_triggers, i)) != NULL; i++) {
		refcount_trigger();
	}

EXIT:
	return;
}

/**
 * Append an input trigger to an existing datapipe
 *
 * @param datapipe The datapipe to manipulate
 * @param trigger The trigger to add to the datapipe
 */
void append_input_trigger_to_datapipe(datapipe_struct *const datapipe,
				      void (*trigger)(gconstpointer data))
{
	void (*refcount_trigger)(void);
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"append_input_trigger_to_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"append_input_trigger_to_datapipe() called "
			"without a valid trigger");
		goto EXIT;
	}

	datapipe->input_triggers = g_slist_append(datapipe->input_triggers,
						  trigger);

	for (i = 0; (refcount_trigger = g_slist_nth_data(datapipe->refcount_triggers, i)) != NULL; i++) {
		refcount_trigger();
	}

EXIT:
	return;
}

/**
 * Remove an input trigger from an existing datapipe
 * Non-existing triggers are ignored
 *
 * @param datapipe The datapipe to manipulate
 * @param trigger The trigger to remove from the datapipe
 */
void remove_input_trigger_from_datapipe(datapipe_struct *const datapipe,
					void (*trigger)(gconstpointer data))
{
	void (*refcount_trigger)(void);
	guint oldlen;
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"remove_input_trigger_from_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"remove_input_trigger_from_datapipe() called "
			"without a valid trigger");
		goto EXIT;
	}

	oldlen = g_slist_length(datapipe->input_triggers);

	datapipe->input_triggers = g_slist_remove(datapipe->input_triggers,
						  trigger);

	/* Did we remove any entry? */
	if (oldlen == g_slist_length(datapipe->input_triggers)) {
		mce_log(LL_DEBUG,
			"Trying to remove non-existing input trigger");
		goto EXIT;
	}

	for (i = 0; (refcount_trigger = g_slist_nth_data(datapipe->refcount_triggers, i)) != NULL; i++) {
		refcount_trigger();
	}

EXIT:
	return;
}

/**
 * Append an output trigger to an existing datapipe
 *
 * @param datapipe The datapipe to manipulate
 * @param trigger The trigger to add to the datapipe
 */
void append_output_trigger_to_datapipe(datapipe_struct *const datapipe,
				       void (*trigger)(gconstpointer data))
{
	void (*refcount_trigger)(void);
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"append_output_trigger_to_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"append_output_trigger_to_datapipe() called "
			"without a valid trigger");
		goto EXIT;
	}

	datapipe->output_triggers = g_slist_append(datapipe->output_triggers,
						   trigger);

	for (i = 0; (refcount_trigger = g_slist_nth_data(datapipe->refcount_triggers, i)) != NULL; i++) {
		refcount_trigger();
	}

EXIT:
	return;
}

/**
 * Remove an output trigger from an existing datapipe
 * Non-existing triggers are ignored
 *
 * @param datapipe The datapipe to manipulate
 * @param trigger The trigger to remove from the datapipe
 */
void remove_output_trigger_from_datapipe(datapipe_struct *const datapipe,
					 void (*trigger)(gconstpointer data))
{
	void (*refcount_trigger)(void);
	guint oldlen;
	gint i;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"remove_output_trigger_from_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"remove_output_trigger_from_datapipe() called "
			"without a valid trigger");
		goto EXIT;
	}

	oldlen = g_slist_length(datapipe->output_triggers);

	datapipe->output_triggers = g_slist_remove(datapipe->output_triggers,
						   trigger);

	/* Did we remove any entry? */
	if (oldlen == g_slist_length(datapipe->output_triggers)) {
		mce_log(LL_DEBUG,
			"Trying to remove non-existing output trigger");
		goto EXIT;
	}

	for (i = 0; (refcount_trigger = g_slist_nth_data(datapipe->refcount_triggers, i)) != NULL; i++) {
		refcount_trigger();
	}

EXIT:
	return;
}

/**
 * Append a reference count trigger to an existing datapipe
 *
 * @param datapipe The datapipe to manipulate
 * @param trigger The trigger to add to the datapipe
 */
void append_refcount_trigger_to_datapipe(datapipe_struct *const datapipe,
					 void (*trigger)(void))
{
	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"append_refcount_trigger_to_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"append_refcount_trigger_to_datapipe() called "
			"without a valid trigger");
		goto EXIT;
	}

	datapipe->refcount_triggers = g_slist_append(datapipe->refcount_triggers, trigger);

EXIT:
	return;
}

/**
 * Remove a reference count trigger from an existing datapipe
 * Non-existing triggers are ignored
 *
 * @param datapipe The datapipe to manipulate
 * @param trigger The trigger to remove from the datapipe
 */
void remove_refcount_trigger_from_datapipe(datapipe_struct *const datapipe,
					   void (*trigger)(void))
{
	guint oldlen;

	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"remove_refcount_trigger_from_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	if (trigger == NULL) {
		mce_log(LL_ERR,
			"remove_refcount_trigger_from_datapipe() called "
			"without a valid trigger");
		goto EXIT;
	}

	oldlen = g_slist_length(datapipe->refcount_triggers);

	datapipe->refcount_triggers = g_slist_remove(datapipe->refcount_triggers, trigger);

	/* Did we remove any entry? */
	if (oldlen == g_slist_length(datapipe->refcount_triggers)) {
		mce_log(LL_DEBUG,
			"Trying to remove non-existing refcount trigger");
		goto EXIT;
	}

EXIT:
	return;
}

/**
 * Initialise a datapipe
 *
 * @param datapipe The datapipe to manipulate
 * @param read_only READ_ONLY if the datapipe is read only,
 *                  READ_WRITE if it's read/write
 * @param free_cache FREE_CACHE if the cached data needs to be freed,
 *                   DONT_FREE_CACHE if the cache data should not be freed
 * @param datasize Pass size of memory to copy,
 *		   or 0 if only passing pointers or data as pointers
 * @param initial_data Initial cache content
 */
void setup_datapipe(datapipe_struct *const datapipe,
		    const read_only_policy_t read_only,
		    const cache_free_policy_t free_cache,
		    const gsize datasize, gpointer initial_data)
{
	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"setup_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	datapipe->filters = NULL;
	datapipe->input_triggers = NULL;
	datapipe->output_triggers = NULL;
	datapipe->refcount_triggers = NULL;
	datapipe->datasize = datasize;
	datapipe->read_only = read_only;
	datapipe->free_cache = free_cache;
	datapipe->cached_data = initial_data;

EXIT:
	return;
}

/**
 * Deinitialize a datapipe
 *
 * @param datapipe The datapipe to manipulate
 */
void free_datapipe(datapipe_struct *const datapipe)
{
	if (datapipe == NULL) {
		mce_log(LL_ERR,
			"free_datapipe() called "
			"without a valid datapipe");
		goto EXIT;
	}

	/* Warn about still registered filters/triggers */
	if (datapipe->filters != NULL) {
		mce_log(LL_INFO,
			"free_datapipe() called on a datapipe that "
			"still has registered filter(s)");
	}

	if (datapipe->input_triggers != NULL) {
		mce_log(LL_INFO,
			"free_datapipe() called on a datapipe that "
			"still has registered input_trigger(s)");
	}

	if (datapipe->output_triggers != NULL) {
		mce_log(LL_INFO,
			"free_datapipe() called on a datapipe that "
			"still has registered output_trigger(s)");
	}

	if (datapipe->refcount_triggers != NULL) {
		mce_log(LL_INFO,
			"free_datapipe() called on a datapipe that "
			"still has registered refcount_trigger(s)");
	}

	if (datapipe->free_cache == FREE_CACHE) {
		g_free(datapipe->cached_data);
	}

EXIT:
	return;
}
