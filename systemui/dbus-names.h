/**
 * @file dbus-names.h
 * D-Bus Interface for communicating with SystemUI
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012-2019 Jolla Ltd.
 *
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Vesa Halttunen <vesa.halttunen@jollamobile.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * @author Andrew den Exter <andrew.den.exter@jolla.com>
 *
 * These headers are free software; you can redistribute them
 * and/or modify them under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation.
 *
 * These headers are distributed in the hope that they will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _SYSTEMUI_DBUS_NAMES_H
#define _SYSTEMUI_DBUS_NAMES_H

/** The System UI service */
#define SYSTEMUI_SERVICE       "org.nemomobile.lipstick"

/** The System UI request interface. */
#define SYSTEMUI_REQUEST_IF    "org.nemomobile.lipstick.screenlock"

/** The System UI request path. */
#define SYSTEMUI_REQUEST_PATH  "/screenlock"

/** The device lock service */
#define DEVICELOCK_SERVICE       "org.nemomobile.devicelock"

/** The device lock request interface. */
#define DEVICELOCK_REQUEST_IF    "org.nemomobile.lipstick.devicelock"

/** The device lock request path. */
#define DEVICELOCK_REQUEST_PATH  "/devicelock"

#endif /* _SYSTEMUI_DBUS_NAMES_H */
