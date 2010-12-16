/**
 * @file mce-hal.c
 * Hardware Abstraction Layer for MCE
 * <p>
 * Copyright © 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
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

#include <string.h>			/* strstr() */
#include <stdlib.h>			/* free() */
#include <sysinfo.h>			/* sysinfo_init(),
					 * sysinfo_get_value(),
					 * sysinfo_finish(),
					 * struct system_config
					 */

#include "mce-hal.h"

#include "mce-log.h"			/* mce_log(), LL_* */

#if 0
/** Lock key type */
typdef enum {
	/** No lockkey */
	LOCKKEY_NONE,
	/** Flicker key */
	LOCKKEY_FLICKER,
	/** Slider key */
	LOCKKEY_SLIDER
} lockkey_t;

/** Hardware information */
typedef struct {
	/** Does the device have a lock key?  If so, what type? */
	lockkey_t lockkey;
	/** Does the device have a hardware keyboard? */
	gboolean keyboard;
} product_info_t;
#endif

/**
 * The product ID of the device
 */
static product_id_t product_id = PRODUCT_UNSET;

/**
 * Compare a string with memory, with length checks
 *
 * @param mem The memory to compare with
 * @param str The string to compare the memory to
 * @param len The length of the memory area
 * @return TRUE if the string matches the memory area,
 *         FALSE if the memory area does not match, or if the lengths differ
 */
static gboolean strmemcmp(guint8 *mem, const gchar *str, gulong len)
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

/**
 * Get product ID
 *
 * @return The product ID
 */
product_id_t get_product_id(void)
{
	static struct system_config *sc = 0;
	guint8 *tmp = NULL;
	gulong len = 0;

	if (product_id != PRODUCT_UNSET)
		goto EXIT;

	if (sysinfo_init(&sc) != 0) {
		mce_log(LL_ERR,
			"sysinfo_init() failed");
		product_id = PRODUCT_UNKNOWN;
		goto EXIT;
	}

	if (sysinfo_get_value(sc, PRODUCT_SYSINFO_KEY, &tmp, &len) != 0) {
		mce_log(LL_ERR,
			"sysinfo_get_value() failed");
		product_id = PRODUCT_UNKNOWN;
		goto EXIT2;
	}

	if (strmemcmp(tmp, PRODUCT_SU18_STR, len) == TRUE) {
		product_id = PRODUCT_SU18;
	} else if (strmemcmp(tmp, PRODUCT_RX34_STR, len) == TRUE) {
		product_id = PRODUCT_RX34;
	} else if (strmemcmp(tmp, PRODUCT_RX44_STR, len) == TRUE) {
		product_id = PRODUCT_RX44;
	} else if (strmemcmp(tmp, PRODUCT_RX48_STR, len) == TRUE) {
		product_id = PRODUCT_RX48;
	} else if (strmemcmp(tmp, PRODUCT_RX51_STR, len) == TRUE) {
		product_id = PRODUCT_RX51;
	} else if (strmemcmp(tmp, PRODUCT_RX71_STR, len) == TRUE) {
		product_id = PRODUCT_RX71;
	} else if (strmemcmp(tmp, PRODUCT_RM680_STR, len) == TRUE) {
		product_id = PRODUCT_RM680;
	} else if (strmemcmp(tmp, PRODUCT_RM690_STR, len) == TRUE) {
		product_id = PRODUCT_RM690;
	} else if (strmemcmp(tmp, PRODUCT_RM696_STR, len) == TRUE) {
		product_id = PRODUCT_RM696;
	} else if (strmemcmp(tmp, PRODUCT_RM716_STR, len) == TRUE) {
		product_id = PRODUCT_RM716;
	} else {
		product_id = PRODUCT_UNKNOWN;
	}

	free(tmp);

EXIT2:
	sysinfo_finish(sc);

EXIT:
	return product_id;
}
