/**
 * @file proximity.h
 * Headers for the proximity sensor module
 * <p>
 * Copyright © 2010-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2013-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Tuomo Tanskanen <ext-tuomo.1.tanskanen@nokia.com>
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
#ifndef PROXIMITY_H_
# define PROXIMITY_H_

/** Prefix for proximity sensor setting keys */
# define MCE_SETTING_PROXIMITY_PATH             "/system/osso/dsm/proximity"

/** Whether MCE is allowed to use proximity sensor */
# define MCE_SETTING_PROXIMITY_PS_ENABLED       MCE_SETTING_PROXIMITY_PATH "/ps_enabled"
# define MCE_DEFAULT_PROXIMITY_PS_ENABLED       true

/** Whether proximity sensor should be used on-demand*/
# define MCE_SETTING_PROXIMITY_ON_DEMAND        MCE_SETTING_PROXIMITY_PATH "/on_demand"
# define MCE_DEFAULT_PROXIMITY_ON_DEMAND        false

/** Whether proximity sensor should be treated as cover closed sensor */
# define MCE_SETTING_PROXIMITY_PS_ACTS_AS_LID   MCE_SETTING_PROXIMITY_PATH "/ps_acts_as_lid"
# define MCE_DEFAULT_PROXIMITY_PS_ACTS_AS_LID   false

#endif /* PROXIMITY_H_ */
