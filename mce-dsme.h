/**
 * @file mce-dsme.h
 * Headers for the DSME<->MCE interface and logic
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2013-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Tapio Rantala <ext-tapio.rantala@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
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
#ifndef _MCE_DSME_H_
#define _MCE_DSME_H_

#include <glib.h>

/** How long to delay removal of MCE_SUBMODE_TRANSITION after
 *  receiving system state change from dsme
 *
 * <0 -> immediately
 *  0 -> from idle callback
 * >0 -> from timer callback after TRANSITION_DELAY ms
 */
#define TRANSITION_DELAY                -1

void mce_dsme_request_powerup(void);
void mce_dsme_request_reboot(void);
void mce_dsme_request_normal_shutdown(void);

/* When MCE is made modular, this will be handled differently */
gboolean mce_dsme_init(void);
void mce_dsme_exit(void);

#endif /* _MCE_DSME_H_ */
