/**
   @file tklock-dbus-names.h
   <p>
   Copyright (c) 2005-09 Nokia Corporation

   Contact: David Weinehall <david.weinehall@nokia.com>
*/

#ifndef _SYSTEMUI_TKLOCK_DBUS_NAMES_H
#define _SYSTEMUI_TKLOCK_DBUS_NAMES_H

#define SYSTEMUI_TKLOCK_OPEN_REQ       "tklock_open"
#define SYSTEMUI_TKLOCK_CLOSE_REQ      "tklock_close"

#define TKLOCK_SIGNAL_IF		"com.nokia.tklock.signal"
#define TKLOCK_SIGNAL_PATH		"/com/nokia/tklock/signal"
#define TKLOCK_MM_KEY_PRESS_SIG         "mm_key_press"

typedef enum
{
        TKLOCK_NONE,
	TKLOCK_ENABLE,
	TKLOCK_HELP,
	TKLOCK_SELECT,
	TKLOCK_ONEINPUT,
        /* slider screen unlocking ui mode,
	 * alternative to unconditional unlocking with flicker
	 */
	TKLOCK_ENABLE_VISUAL 
} tklock_mode;

typedef enum
{
	TKLOCK_UNLOCK = 1,
	TKLOCK_RETRY,
	TKLOCK_TIMEOUT,
	TKLOCK_CLOSED

} tklock_status;

#endif
