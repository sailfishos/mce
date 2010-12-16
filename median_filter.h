/**
 * @file median_filter.h
 * Headers for the median filter
 * <p>
 * Copyright © 2007-2009 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _MEDIAN_FILTER_H_
#define _MEDIAN_FILTER_H_

#include <glib.h>

/** Maximum window size of the median filter */
#define MEDIAN_FILTER_MAX_WINDOW_SIZE	11

/** Median filter */
typedef struct {
	gsize window_size;				/**< Window size */
	/** Current number of samples in the window */
	gsize samples;
	/** Index of the oldest sample in the window */
	gsize oldest;
	gint window[MEDIAN_FILTER_MAX_WINDOW_SIZE];	/**< Ring buffer */
	gint ordered_window[MEDIAN_FILTER_MAX_WINDOW_SIZE];	/**< Ordered buffer */
} median_filter_struct;

gboolean median_filter_init(median_filter_struct *filter, gsize window_size);
gint median_filter_map(median_filter_struct *filter, gint value);

#endif /* _MEDIAN_FILTER_H_ */
