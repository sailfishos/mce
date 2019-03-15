/**
 * @file mce-common.h
 * Header for common state logic for Mode Control Entity
 * <p>
 * Copyright (C) 2017-2019 Jolla Ltd.
 * <p>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
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

#ifndef  MCE_COMMON_H_
# define MCE_COMMON_H_

# include <stdbool.h>
# include <glib.h>

bool mce_common_init(void);
void mce_common_quit(void);

void common_on_proximity_schedule(const char *srce, GDestroyNotify func, gpointer aptr);
void common_on_proximity_cancel  (const char *srce, GDestroyNotify func, gpointer aptr);

#endif /* MCE_COMMON_H_ */
