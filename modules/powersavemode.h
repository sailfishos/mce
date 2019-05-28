/**
 * @file powersavemode.h
 * Headers for the power saving mode module
 * <p>
 * Copyright © 2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2014-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
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
#ifndef POWERSAVEMODE_H_
# define POWERSAVEMODE_H_

/* ========================================================================= *
 * Settings
 * ========================================================================= */

/** Prefix for energy management setting keys */
# define MCE_SETTING_EM_PATH                    "/system/osso/dsm/energymanagement"

/** Whether power save mode activation is allowed
 *
 * Power Save Mode = when battery percentage falls below some threshold,
 * potentially power hungry features are disabled.
 *
 * Note: This is legacy feature for Nokia devices. While the triggering
 *       mechanism and ui side notifications are fully working, the
 *       usefulness of it is rather limited since cellular, networking
 *       etc middleware does not (yet) react to psm state changes.
 */
# define MCE_SETTING_EM_ENABLE_PSM               MCE_SETTING_EM_PATH "/enable_power_saving"
# define MCE_DEFAULT_EM_ENABLE_PSM               false

/** Whether power save mode should be active always when not charging */
# define MCE_SETTING_EM_FORCED_PSM               MCE_SETTING_EM_PATH "/force_power_saving"
# define MCE_DEFAULT_EM_FORCED_PSM               false

/** Threshold when to activate PSM [battery %] */
# define MCE_SETTING_EM_PSM_THRESHOLD            MCE_SETTING_EM_PATH "/psm_threshold"
# define MCE_DEFAULT_EM_PSM_THRESHOLD            20

/** List of 5 possible PSM threshold [battery %]
 *
 * Hint for settings UI. Not used by MCE itself.
 */
# define MCE_SETTING_EM_POSSIBLE_PSM_THRESHOLDS  MCE_SETTING_EM_PATH "/possible_psm_thresholds"
# define MCE_DEFAULT_EM_POSSIBLE_PSM_THRESHOLDS  10,20,30,40,50

#endif /* POWERSAVEMODE_H_ */
