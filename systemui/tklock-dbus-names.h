/**
 * @file tklock-dbus-names.h
 * Defines used when communicating with SystemUI TKLock
 * <p>
 * Copyright © 2005-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2014-2019 Jolla Ltd.
 *
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
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
#ifndef _SYSTEMUI_TKLOCK_DBUS_NAMES_H
#define _SYSTEMUI_TKLOCK_DBUS_NAMES_H

/* TODO: These constants are related to semi-functional legacy interface
 *       and all of it should be removed both from mce and lipstick...
 */

#define SYSTEMUI_TKLOCK_OPEN_REQ       "tklock_open"
#define SYSTEMUI_TKLOCK_CLOSE_REQ      "tklock_close"

/** Enum of modes used when calling SystemUI */
typedef enum {
	/** Disable TKLock; deprecated -- use close call instead */
	TKLOCK_NONE,
	/** Enable non-slider TKLock; deprecated */
	TKLOCK_ENABLE,
	/** Show TKLock help; deprecated */
	TKLOCK_HELP,
	/** Show TKLock press select text; deprecated */
	TKLOCK_SELECT,
	/** Enable event eater */
	TKLOCK_ONEINPUT,
	/** Enable slider TKLock */
	TKLOCK_ENABLE_VISUAL,
	/** Enable low power mode UI */
	TKLOCK_ENABLE_LPM_UI,
	/** Display turned off; pause UI updates */
	TKLOCK_PAUSE_UI
} tklock_mode;

#endif /* _SYSTEMUI_TKLOCK_DBUS_NAMES_H */
