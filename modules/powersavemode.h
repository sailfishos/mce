/**
 * @file powersavemode.h
 * Headers for the power saving mode module
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
#ifndef POWERSAVEMODE_H_
# define POWERSAVEMODE_H_

/* ========================================================================= *
 * Settings
 * ========================================================================= */

/** Path to the GConf settings for energy management */
# define MCE_SETTING_EM_PATH                    "/system/osso/dsm/energymanagement"

/** Path to the power saving mode GConf setting */
# define MCE_SETTING_EM_ENABLE_PSM               MCE_SETTING_EM_PATH "/enable_power_saving"
# define MCE_DEFAULT_EM_ENABLE_PSM               false

/** Path to the forced power saving mode GConf setting */
# define MCE_SETTING_EM_FORCED_PSM               MCE_SETTING_EM_PATH "/force_power_saving"
# define MCE_DEFAULT_EM_FORCED_PSM               false

/** Path to the power save mode threshold GConf setting */
# define MCE_SETTING_EM_PSM_THRESHOLD            MCE_SETTING_EM_PATH "/psm_threshold"
# define MCE_DEFAULT_EM_PSM_THRESHOLD            20

/** Setting for: Possible PSM thresholds
 *
 * Hint for settings UI. Not used by MCE itself.
 */
# define MCE_SETTING_EM_POSSIBLE_PSM_THRESHOLDS  MCE_SETTING_EM_PATH "/possible_psm_thresholds"
# define MCE_DEFAULT_EM_POSSIBLE_PSM_THRESHOLDS  10,20,30,40,50

#endif /* POWERSAVEMODE_H_ */
