/**
 * @file mce-hal.h
 * Headers for the Hardware Abstraction Layer for MCE
 * <p>
 * Copyright © 2008-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Tapio Rantala <ext-tapio.rantala@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author: Simo Piiroinen <simo.piiroinen@jollamobile.com>
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
#ifndef _MCE_HAL_H_
#define _MCE_HAL_H_

#include <glib.h>

#define PRODUCT_SU18_STR		"SU-18"	/**< 770 */
#define PRODUCT_RX34_STR		"RX-34"	/**< N800 */
#define PRODUCT_RX44_STR		"RX-44"	/**< N810 */
#define PRODUCT_RX48_STR		"RX-48"	/**< N810 WiMAX Edition */
#define PRODUCT_RX51_STR		"RX-51" /**< N900 */
#define PRODUCT_RX71_STR		"RX-71" /**< Development board */
#define PRODUCT_RM680_STR		"RM-680" /**< ??? */
#define PRODUCT_RM690_STR		"RM-690" /**< ??? */
#define PRODUCT_RM696_STR		"RM-696" /**< ??? */
#define PRODUCT_RM716_STR		"RM-716" /**< ??? */

/** Product ID type */
typedef enum {
	PRODUCT_UNSET = -1,			/**< Product not set */
	PRODUCT_UNKNOWN = 0,			/**< Product unknown */
	PRODUCT_SU18 = 1,			/**< SU-18 */
	PRODUCT_RX34 = 2,			/**< RX-34 */
	PRODUCT_RX44 = 3,			/**< RX-44 */
	PRODUCT_RX48 = 4,			/**< RX-48 */
	PRODUCT_RX51 = 5,			/**< RX-51 */
	PRODUCT_RX71 = 6,			/**< RX-71 */
	PRODUCT_RM680 = 9,			/**< RM-680 */
	PRODUCT_RM690 = 10,			/**< RM-690 */
	PRODUCT_RM696 = 11,			/**< RM-696 */
	PRODUCT_RM716 = 12			/**< RM-716 */
} product_id_t;

gboolean get_sysinfo_value(const gchar *const key, guint8 **array, gulong *len);
product_id_t get_product_id(void);

#endif /* _MCE_HAL_H_ */
