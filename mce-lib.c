/**
 * @file mce-lib.c
 * This file provides various helper functions
 * for the Mode Control Entity
 * <p>
 * Copyright © 2004-2010 Nokia Corporation and/or its subsidiary(-ies).
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

#include <stdio.h>			/* sscanf() */
#include <string.h>			/* strcmp() */

#include "mce.h"                        /* MCE_INVALID_TRANSLATION */
#include "mce-lib.h"                    /* mce_translation_t */

#include "mce-log.h"			/* mce_log(), LL_* */

/**
 * Set a bit
 *
 * @param bit The bit to set
 * @param bitfield A pointer to an array with the bitfield
 */
void set_bit(guint bit, gulong **bitfield)
{
	if ((bitfield == NULL) || (*bitfield == NULL))
		goto EXIT;

	(*bitfield)[bit / bitsize_of(**bitfield)] |= 1UL << (bit % bitsize_of(**bitfield));

EXIT:
	return;
}

/**
 * Clear a bit
 *
 * @param bit The bit to clear
 * @param bitfield A pointer to an array with the bitfield
 */
void clear_bit(guint bit, gulong **bitfield)
{
	if ((bitfield == NULL) || (*bitfield == NULL))
		goto EXIT;

	(*bitfield)[bit / bitsize_of(**bitfield)] &= ~(1UL << (bit % bitsize_of(**bitfield)));

EXIT:
	return;
}

/**
 * Test whether a bit is set
 *
 * @param bit The bit to test for
 * @param bitfield An array with the bitfield
 * @return TRUE if the bit is set,
 *         FALSE if the bit is unset
 */
gboolean test_bit(guint bit, const gulong *bitfield)
{
	return ((1UL << (bit % bitsize_of(*bitfield))) &
		(((gulong *)bitfield)[bit / bitsize_of(*bitfield)])) != 0;
}

/**
 * Convert a string to a bitfield
 *
 * @param string The string with comma-separated numbers
 *               to turn into a bitfield
 * @param[in,out] bitfield A bitfield to return the string in
 * @param bitfieldsize The size of the bitfield
 * @return TRUE on success,
 *         FALSE if the string could not be parsed numerically
 *               or if a number was out of range for the bitfield
 */
gboolean string_to_bitfield(const gchar *string,
			    gulong **bitfield, gsize bitfieldsize)
{
	gchar *tmp = (gchar *)string;
	gboolean status = FALSE;
	int offset = 0;
	guint num;

	if ((string == NULL) || (bitfield == NULL) || (*bitfield == NULL))
		goto EXIT;

	while ((sscanf(tmp, "%u%n", &num, &offset) != 0) && (offset != 0)) {
		/* Make sure we can represent this number */
		if (num > (bitfieldsize * bitsize_of(**bitfield)))
			goto EXIT;

		set_bit(num, bitfield);
		tmp += (offset + 1);
		offset = 0;
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * Convert a bitfield to a string
 *
 * @param bitfield The bitfield to convert to a comma-separated string
 *                 with the numbers of the set bits
 * @param bitfieldsize The size of the bitfield
 * @return A string with the newly allocated string on success,
 *         NULL on failure
 */
char *bitfield_to_string(const gulong *bitfield, gsize bitfieldsize)
{
	gchar *tmp = NULL;
	guint i, j;

	/* Always pass 0; this way a NULL string represents failure,
	 * and a string with no bits set will represent an empty mask;
	 * we also simplify the g_strdup_printf() case quite a bit
	 */
	if ((tmp = strdup("0")) == NULL) {
		mce_log(LL_CRIT,
			"Failed to allocate memory "
			"for tmp");
		goto EXIT;
	}

	for (i = 0; i < bitfieldsize; i++) {
		for (j = 0; bitfield[i] && j < bitsize_of(*bitfield); j++) {
			if (bitfield[i] & (1UL << j)) {
				gchar *tmp2;

				tmp2 = g_strdup_printf("%s,%u",
						       tmp, (i * bitsize_of(*bitfield)) + j);

				g_free(tmp);

				if (tmp2 == NULL) {
					mce_log(LL_CRIT,
						"Failed to allocate memory "
						"for tmp2");
					goto EXIT;
				}

				tmp = tmp2;
			}
		}
	}

EXIT:
	return tmp;
}

/**
 * Convert a value to a binary string (9-bit, since it's for Lysti)
 * FIXME: convert to handle arbitrary length instead and make reentrant
 * @note This function is non-reentrant; it returns a fixed sized,
 *       statically allocated, string that should not be freed
 *
 * @param bin The value to convert to a binary string
 * @return A static string with a representation the value
 */
const gchar *bin_to_string(guint bin)
{
	static gchar bin_string[] = "000000000";
	gint i;

	for (i = 0; i < 9; i++) {
		bin_string[8 - i] = (bin & (1 << i)) ? '1' : '0';
	}

	return bin_string;
}

/**
 * Translate an integer to its string representation;
 * if no valid mapping exists, return the provided default string
 * (if one has been provided)
 *
 * @param translation A mce_translation_t mapping
 * @param number The number to map to a string
 * @param default_string The default string to return if no match is found
 * @return A string translation of the integer
 */
const gchar *mce_translate_int_to_string_with_default(const mce_translation_t translation[], gint number, const gchar *default_string)
{
	const gchar *string;
	gint i = 0;

	/* This might seem awkward, but it's made to allow sparse
	 * number spaces
	 */
	do {
		string = translation[i].string;
	} while (translation[i].number != MCE_INVALID_TRANSLATION &&
		 translation[i++].number != number);

	/* XXX: will this really behave correctly if there's only
	 * one (MCE_INVALID_TRANSLATION) element in the structure?
	 */
	if ((translation[i].number == MCE_INVALID_TRANSLATION) &&
	    (translation[i - 1].number != number) &&
	    (default_string != NULL))
		string = default_string;

	return string;
}

/**
 * Translate an integer to its string representation
 *
 * @param translation A mce_translation_t mapping
 * @param number The number to map to a string
 * @return A string translation of the integer
 */
const gchar *mce_translate_int_to_string(const mce_translation_t translation[],
					 gint number)
{
	return mce_translate_int_to_string_with_default(translation, number, NULL);
}

/**
 * Translate a string to its integer representation
 * if no valid mapping exists, return the provided default integer
 * (if one has been provided)
 *
 * @param translation A mce_translation_t mapping
 * @param string The string to map to an number
 * @param default_integer The number to return if no match is found
 * @return An integer translation value of the string
 */
gint mce_translate_string_to_int_with_default(const mce_translation_t translation[], const gchar *const string, gint default_integer)
{
	gint number = MCE_INVALID_TRANSLATION;
	gint i = 0;

	while (translation[i].number != MCE_INVALID_TRANSLATION) {
		/* If the string matches, set number and stop searching */
		if (strcmp(translation[i].string, string) == 0) {
			number = translation[i].number;
			break;
		}

		i++;
	}

	if (translation[i].number == MCE_INVALID_TRANSLATION)
		number = default_integer;

	return number;
}
/**
 * Translate a string to its integer representation
 *
 * @param translation A mce_translation_t mapping
 * @param string The string to map to an number
 * @return An integer translation value of the string
 */
gint mce_translate_string_to_int(const mce_translation_t translation[],
				 const gchar *const string)
{
	return mce_translate_string_to_int_with_default(translation, string, MCE_INVALID_TRANSLATION);
}

/**
 * Locate a delimited substring
 *
 * @param haystack The string to search in
 * @param needle The string to search for
 * @param delimiter The delimiter
 * @return A pointer to the position of the substring on match,
 *         NULL if no match found
 */
gchar *strstr_delim(const gchar *const haystack, const char *needle,
		    const char *const delimiter)
{
	char *match = NULL;
	const char *tmp2;
	size_t dlen;

	if ((haystack == NULL) || (needle == NULL))
		return NULL;

	/* If there's no delimiter, we'll behave as strstr */
	dlen = (delimiter == NULL) ? 0 : strlen(delimiter);

	tmp2 = haystack;

	while (tmp2 != NULL) {
		const char *tmp;
		ptrdiff_t len;

		/* Find the first occurence of the delimiter */
		if (dlen != 0)
			tmp = strstr(tmp2, delimiter);
		else
			tmp = NULL;

		/* If there's a delimiter, match up to it,
		 * if not, match the entire remaining string
		 */
		if (tmp != NULL)
			len = tmp - tmp2;
		else
			len = strlen(tmp2);

		/* If we find a match, we're done */
		if ((match = g_strstr_len(tmp2, len, needle)) != NULL)
			goto EXIT;

		/* If there's no more delimiters, we're done */
		if (len == 0)
			goto EXIT;

		/* Skip past the current token + the delimiter */
		tmp2 += (len + dlen);
	};

EXIT:
	return match;
}

/**
 * Compare a string with memory, with length checks
 *
 * @param mem The memory to compare with
 * @param str The string to compare the memory to
 * @param len The length of the memory area
 * @return TRUE if the string matches the memory area,
 *         FALSE if the memory area does not match, or if the lengths differ
 */
gboolean strmemcmp(guint8 *mem, const gchar *str, gulong len)
{
	gboolean result = FALSE;

	if (strlen(str) != len)
		goto EXIT;

	if (memcmp(mem, str, len) != 0)
		goto EXIT;

	result = TRUE;

EXIT:
	return result;
}
