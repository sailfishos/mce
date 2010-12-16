/**
 * @file callstate.h
 * Headers for the callstate module
 * <p>
 * Copyright © 2009 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _CALLSTATE_H_
#define _CALLSTATE_H_

/* If this is set, the call state can only be modified by MCE and the owner
 * of the current call state, unless the old call state is "none" or the
 * new call type is emergency
 */
#define STRICT_CALL_STATE_OWNER_POLICY

#endif /* _CALLSTATE_H_ */
