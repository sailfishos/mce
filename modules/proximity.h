/**
 * @file proximity.h
 * Headers for the proximity sensor module
 * <p>
 * Copyright © 2010-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Tuomo Tanskanen <ext-tuomo.1.tanskanen@nokia.com>
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
#ifndef _PROXIMITY_H_
#define _PROXIMITY_H_

/** Path to the GConf settings for the proximity */
#define MCE_GCONF_PROXIMITY_PATH		"/system/osso/dsm/proximity"

/** Proximity sensor enabled GConf setting */
#define MCE_GCONF_PROXIMITY_PS_ENABLED_PATH     MCE_GCONF_PROXIMITY_PATH "/ps_enabled"
#define DEFAULT_PROXIMITY_PS_ENABLED            true

/** Proximity sensor acts as lid sensor setting */
#define MCE_GCONF_PROXIMITY_PS_ACTS_AS_LID      MCE_GCONF_PROXIMITY_PATH "/ps_acts_as_lid"
#define DEFAULT_PROXIMITY_PS_ACTS_AS_LID        false

#endif /* _PROXIMITY_H_ */
