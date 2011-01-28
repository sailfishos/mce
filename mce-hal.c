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

#include "mce-hal.h"

#include "mce-lib.h"			/* strmemcmp() */
#include "mce-log.h"			/* mce_log(), LL_* */
#include "mce-dbus.h"			/* dbus_send(),
					 * dbus_message_iter_init(),
					 * dbus_message_iter_get_arg_type(),
					 * dbus_message_iter_get_basic(),
					 * dbus_message_iter_next(),
					 * dbus_error_init(),
					 * dbus_error_free(),
					 * DBUS_TYPE_BYTE,
					 * DBUS_TYPE_ARRAY,
					 * DBUS_TYPE_STRING,
					 * DBUS_TYPE_INVALID,
					 * DBusMessage, DBusError,
					 * dbus_bool_t,
					 */

#ifndef SYSINFOD_SERVICE
/** SYSINFOD D-Bus service */
#define SYSINFOD_SERVICE                "com.nokia.SystemInfo"
#endif /* SYSINFOD_SERVICE */

#ifndef SYSINFOD_INTERFACE
/** SYSINFOD D-Bus interface */
#define SYSINFOD_INTERFACE              "com.nokia.SystemInfo"
#endif /* SYSINFOD_INTERFACE */

#ifndef SYSINFOD_PATH
/** SYSINFOD D-Bus object path */
#define SYSINFOD_PATH                   "/com/nokia/SystemInfo"
#endif /* SYSINFOD_PATH */

#ifndef SYSINFOD_GET_CONFIG_VALUE
/** Query value of a sysinfo key */
#define SYSINFOD_GET_CONFIG_VALUE       "GetConfigValue"
#endif /* SYSINFOD_GET_CONFIG_VALUE */

/** The sysinfo key to request */
#define PRODUCT_SYSINFO_KEY		"/component/product"

/**
 * The product ID of the device
 */
static product_id_t product_id = PRODUCT_UNSET;

/**
 * Retrieve a sysinfo value via D-Bus
 *
 * @param key The sysinfo key to retrieve
 * @param[out] array A newly allocated byte array with the result;
 *                   this array should to be freed when no longer used
 * @param[out] len The length of the newly allocated string
 * @return TRUE on success, FALSE on failure
 */
gboolean get_sysinfo_value(const gchar *const key, guint8 **array, gulong *len)
{
	DBusMessage *reply;
	guint8 *tmp = NULL;
	gboolean status = FALSE;

	if ((reply = dbus_send_with_block(SYSINFOD_SERVICE, SYSINFOD_PATH,
					  SYSINFOD_INTERFACE,
					  SYSINFOD_GET_CONFIG_VALUE,
					  -1,
					  DBUS_TYPE_STRING, &key,
					  DBUS_TYPE_INVALID)) != NULL) {
		dbus_message_get_args(reply, NULL,
				      DBUS_TYPE_ARRAY,
				      DBUS_TYPE_BYTE,
				      &tmp, len,
				      DBUS_TYPE_INVALID);

		if (*len > 0) {
			if ((*array = malloc(*len)) != NULL) {
				*array = memcpy(*array, tmp, *len);
			} else {
				*len = 0;
			}
		}

		dbus_message_unref(reply);
		status = TRUE;
	}

	return status;
}

/**
 * Get product ID
 *
 * @return The product ID
 */
product_id_t get_product_id(void)
{
	guint8 *tmp = NULL;
	gulong len;

	if (product_id != PRODUCT_UNSET)
		goto EXIT;

	if (get_sysinfo_value(PRODUCT_SYSINFO_KEY, &tmp, &len) == FALSE) {
		mce_log(LL_ERR,
			"Failed to get the product ID");
		product_id = PRODUCT_UNKNOWN;
		goto EXIT;
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

EXIT:
	return product_id;
}
