/**
 * @file radiostates.h
 * Headers for the radio states module
 * <p>
 * Copyright © 2010 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _RADIOSTATES_H_
#define _RADIOSTATES_H_

#include <glib.h>

/** Path to online radio states file */
#define MCE_ONLINE_RADIO_STATES_PATH		G_STRINGIFY(MCE_VAR_DIR) "/radio_states.online"
/** Path to offline radio states file */
#define MCE_OFFLINE_RADIO_STATES_PATH		G_STRINGIFY(MCE_VAR_DIR) "/radio_states.offline"

#endif /* _RADIOSTATES_H_ */
