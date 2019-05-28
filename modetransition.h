/**
 * @file modetransition.h
 * Headers for the mode transition component of the Mode Control Entity
 * <p>
 * Copyright © 2004-2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2018-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
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
#ifndef _MODETRANSITION_H_
#define _MODETRANSITION_H_

#include <glib.h>

/** Path to the boot detection file */
#define MCE_BOOTUP_FILENAME             G_STRINGIFY(MCE_RUN_DIR) "/boot"

#define SPLASH_DELAY                    500             /**< 0.5 seconds */
#define ACTDEAD_DELAY                   1500            /**< 1.5 seconds */
#define POWERUP_DELAY                   3500            /**< 3.5 seconds */

/* When MCE is made modular, this will be handled differently */
gboolean mce_mode_init(void);
void mce_mode_exit(void);

#endif /* _MODETRANSITION_H_ */
