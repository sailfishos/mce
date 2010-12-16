/**
 * @file dbus-names.h
 * DBus Interface to the System UI
 * <p>
 * This file is part of osso-systemui-dbus-dev
 * <p>
 * Copyright (C) 2004-2006 Nokia Corporation.
 * <p>
 * Contact person: David Weinehall <david.weinehall@nokia.com>
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
 * License along with this software; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#ifndef _SYSTEMUI_DBUS_NAMES_H
#define _SYSTEMUI_DBUS_NAMES_H

/** The System UI service */
#define SYSTEMUI_SERVICE       "com.nokia.system_ui"
/** The System UI request interface. */
#define SYSTEMUI_REQUEST_IF    "com.nokia.system_ui.request"
/** The System UI signal interface. */
#define SYSTEMUI_SIGNAL_IF     "com.nokia.system_ui.signal"
/** The System UI request path. */
#define SYSTEMUI_REQUEST_PATH  "/com/nokia/system_ui/request"
/** The System UI signal path. */
#define SYSTEMUI_SIGNAL_PATH   "/com/nokia/system_ui/signal"

/**
 * Requests system UI to shut down.
 */
#define SYSTEMUI_QUIT_REQ      "quit"

/**
 * Notify everyone that the System UI has started.
 */
#define SYSTEMUI_STARTED_SIG   "system_ui_started"

#endif
