/**
 * @file connectivity.h
 * Headers for the connectivity logic for the Mode Control Entity
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
#ifndef _CONNECTIVITY_H_
#define _CONNECTIVITY_H_

#include <glib.h>

gboolean get_connectivity_status(void);

gboolean mce_connectivity_init(void);
void mce_connectivity_exit(void);

#endif /* _CONNECTIVITY_H_ */
