/**
 * @file mce-lib.h
 * Headers for various helper functions
 * for the Mode Control Entity
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _MCE_LIB_H_
#define _MCE_LIB_H_

#include <stdint.h>

#include <glib.h>

/** Find the number of bits of a type */
#define bitsize_of(__x)			(guint)(sizeof (__x) * 8)

/** translation structure */
typedef struct {
	const gint number;		/**< Number representation */
	const gchar *const string;	/**< String representation */
} mce_translation_t;

void set_bit(guint bit, gulong **bitfield);
void clear_bit(guint bit, gulong **bitfield);
gboolean test_bit(guint bit, const gulong *bitfield);

gboolean string_to_bitfield(const gchar *string,
			    gulong **bitfield, gsize bitfieldsize);
char *bitfield_to_string(const gulong *bitfield, gsize bitfieldsize);

const gchar *bin_to_string(guint bin);

const gchar *mce_translate_int_to_string_with_default(const mce_translation_t translation[], gint number, const gchar *default_string);
const gchar *mce_translate_int_to_string(const mce_translation_t translation[],
					 gint number);

gint mce_translate_string_to_int_with_default(const mce_translation_t translation[], const gchar *const string, gint default_number);
gint mce_translate_string_to_int(const mce_translation_t translation[],
				 const gchar *const string);

gchar *strstr_delim(const gchar *const haystack, const char *needle,
		    const char *const delimiter);
gboolean strmemcmp(guint8 *mem, const gchar *str, gulong len);

int64_t mce_lib_get_boot_tick(void);
int64_t mce_lib_get_mono_tick(void);
int64_t mce_lib_get_real_tick(void);

guint mce_wakelocked_timeout_add_full(gint priority, guint interval,
				      GSourceFunc function,
				      gpointer data, GDestroyNotify notify);
guint mce_wakelocked_timeout_add(guint interval, GSourceFunc function,
				 gpointer data);
guint mce_wakelocked_idle_add(GSourceFunc function, gpointer data);

#endif /* _MCE_LIB_H_ */
