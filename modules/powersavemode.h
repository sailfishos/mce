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
#ifndef _POWERSAVEMODE_H_
#define _POWERSAVEMODE_H_

/** Default Power Saving Mode; boolean */
#define DEFAULT_POWER_SAVING_MODE		FALSE

/** Default PSM threshold as battery percentage; integer */
#define DEFAULT_PSM_THRESHOLD			10

#ifndef MCE_GCONF_EM_PATH
/** Path to the GConf settings for energy management */
# define MCE_GCONF_EM_PATH			"/system/osso/dsm/energymanagement"
#endif

/** Path to the power saving mode GConf setting */
#define MCE_GCONF_PSM_PATH			MCE_GCONF_EM_PATH "/enable_power_saving"

/** Path to the forced power saving mode GConf setting */
#define MCE_GCONF_FORCED_PSM_PATH		MCE_GCONF_EM_PATH "/force_power_saving"

/** Path to the power save mode threshold GConf setting */
#define MCE_GCONF_PSM_THRESHOLD_PATH		MCE_GCONF_EM_PATH "/psm_threshold"

/** Setting for: Possible PSM thresholds
 *
 * Hint for settings UI. Not used by MCE itself.
 */
#define MCE_GCONF_PSM_POSSIBLE_THRESHOLDS_PATH	MCE_GCONF_EM_PATH "/possible_psm_thresholds"

#endif /* _POWERSAVEMODE_H_ */
