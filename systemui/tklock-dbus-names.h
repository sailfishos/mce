/**
 * @file tklock-dbus-names.h
 * Defines used when communicating with SystemUI TKLock
 * <p>
 * Copyright © 2005-2011 Nokia Corporation and/or its subsidiary(-ies).
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

#define SYSTEMUI_TKLOCK_OPEN_REQ       "tklock_open"
#define SYSTEMUI_TKLOCK_CLOSE_REQ      "tklock_close"

#define TKLOCK_SIGNAL_IF		"com.nokia.tklock.signal"
#define TKLOCK_SIGNAL_PATH		"/com/nokia/tklock/signal"
#define TKLOCK_MM_KEY_PRESS_SIG         "mm_key_press"

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

/** Enum of statuses received from SystemUI */
typedef enum {
	/** TKLock was unlocked by the user */
	TKLOCK_UNLOCK = 1,
	/** TKLock unlock attempt failed; deprecated */
	TKLOCK_RETRY,
	/** Attempt to open TKLock timed out; deprecated */
	TKLOCK_TIMEOUT,
	/** TKLock closed on request from mce */
	TKLOCK_CLOSED
} tklock_status;

#endif /* _SYSTEMUI_TKLOCK_DBUS_NAMES_H */
