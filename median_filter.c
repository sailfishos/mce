/**
 * @file median_filter.c
 * median filter -- this implements a median filter
 * <p>
 * Copyright © 2007-2008 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Semi Malinen <semi.malinen@nokia.com>
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

#include "median_filter.h"

/**
 * Initialise median filter

 * @param filter The median filter to initialise
 * @param window_size The window size to use
 *
 * @return FALSE if window_size is too big or filter is NULL,
 *         TRUE on success
 */
gboolean median_filter_init(median_filter_struct *filter, gsize window_size)
{
	gboolean status = FALSE;
	guint i;

	if ((filter == NULL) || (window_size > MEDIAN_FILTER_MAX_WINDOW_SIZE))
		goto EXIT;

	filter->window_size = window_size;

	for (i = 0; i < filter->window_size; i++) {
		filter->window[i] = 0;
		filter->ordered_window[i] = 0;
	}

	filter->samples = 0;
	filter->oldest = 0;

	status = TRUE;

EXIT:
	return status;
}

/**
 * Insert a new sample into the median filter
 *
 * @param filter The median filter to insert the value into
 * @param value The value to insert
 * @param oldest The oldest value
 * @return The filtered value
 */
static gint insert_ordered(median_filter_struct *filter,
			   gint value, gint oldest)
{
	guint i;

	/* If the filter window hasn't been filled yet, insert the new value */
	if (filter->samples < filter->window_size) {
		/* Find insertion point */
		for (i = 0; i < filter->samples; i++) {
			if (filter->ordered_window[i] >= value) {
				/* Found the insertion point */
				for ( ; i < filter->samples; ++i) {
					gint tmp;

					/* Swap the value at insertion point
					 * with the new value
					 */
					tmp = filter->ordered_window[i];
					filter->ordered_window[i] = value;
					value = tmp;
				}

				break;
			}
		}

		/* Do the insertion */
		filter->ordered_window[i] = value;
		filter->samples++;

		goto EXIT;
	} else {
		/* The filter window is full;
		 * remove the oldest value and insert new
		 */
		if (value == oldest) {
			/* Do nothing */
			goto EXIT;
		}

		/* Find either the insertion point
		 * and/or the deletion point
		 */
		for (i = 0; i < filter->window_size; i++) {
			if (filter->ordered_window[i] >= value) {
				/* Found the insertion point
				 * (it might be the deletion point
				 * as well!)
				 */
				for ( ; i < filter->window_size; i++) {
					/* Swap value at insertion
					 * point and the new value
					 */
					int tmp = filter->ordered_window[i];

					filter->ordered_window[i] = value;
					value = tmp;

					if (value == oldest) {
						/* Found the deletion point */
						goto EXIT;
					}
				}

				goto EXIT;
			} else if (filter->ordered_window[i] == oldest) {
				/* Found the deletion point */
				for ( ; i < filter->window_size - 1; i++) {
					if (filter->ordered_window[i + 1] >= value) {
						/* Found the insertion point */
						break;
					}
					/* Shift the window,
					 * overwriting the value to delete
					 */
					filter->ordered_window[i] = filter->ordered_window[i + 1];
				}
				/* Insert */
				filter->ordered_window[i] = value;
				goto EXIT;
			}
		}
	}

EXIT:
	/* For odd number of samples return the middle one
	 * For even number of samples return the average
	 * of the two middle ones
	 */
	return (filter->ordered_window[(filter->samples - 1) / 2] +
		filter->ordered_window[filter->samples / 2]) / 2;
}

/**
 * Do a complete insertion of a sample into the median filter
 *
 * @param filter The median filter to insert the value into
 * @param value The value to insert
 * @return The filtered value
 */
gint median_filter_map(median_filter_struct *filter, gint value)
{
	gint filtered_value;

	/* Insert into the ordered array (deleting the oldest value) */
	filtered_value = insert_ordered(filter, value,
					filter->window[filter->oldest]);

	/* Insert into the ring buffer (overwriting the oldest value) */
	filter->window[filter->oldest] = value;
	filter->oldest = (filter->oldest + 1) % filter->window_size;

	return filtered_value;
}
