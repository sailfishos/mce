/**
 * @file mce-dsme.h
 * Headers for the DSME<->MCE interface and logic
 * <p>
 * Copyright © 2004-2009 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _MCE_DSME_H_
#define _MCE_DSME_H_

#include <glib.h>

/** Default delay before the user can power up the device from acting dead */
#define TRANSITION_DELAY		1000		/**< 1 second */

/** Name of Powerkey configuration group */
#define MCE_CONF_SOFTPOWEROFF_GROUP	"SoftPowerOff"

/** Name of configuration key for connectivity policy with charger connected */
#define MCE_CONF_SOFTPOWEROFF_CONNECTIVITY_POLICY_CHARGER "ConnectivityPolicyCharger"

/** Name of configuration key for connectivity policy when running on battery */
#define MCE_CONF_SOFTPOWEROFF_CONNECTIVITY_POLICY_BATTERY "ConnectivityPolicyBattery"

/** Name of configuration key for connectivity policy when powering on */
#define MCE_CONF_SOFTPOWEROFF_CONNECTIVITY_POLICY_POWERON "ConnectivityPolicyPowerOn"

/** Name of configuration key for charger connect policy in soft poweroff */
#define MCE_CONF_SOFTPOWEROFF_CHARGER_POLICY_CONNECT "ChargerPolicyConnect"

/**
 * Name of configuration value for the "forced offline" policy
 * when entering soft poweroff
 */
#define SOFTOFF_CONNECTIVITY_FORCE_OFFLINE_STR		"forceoffline"
/**
 * Name of configuration value for the "soft offline" policy
 * when entering soft poweroff
 */
#define SOFTOFF_CONNECTIVITY_SOFT_OFFLINE_STR		"softoffline"
/**
 * Name of configuration value for the "retain connectivity" policy
 * when entering soft poweroff
 */
#define SOFTOFF_CONNECTIVITY_RETAIN_STR			"retain"
/**
 * Name of configuration value for the "stay offline" policy
 * when powering on from soft poweroff
 */
#define SOFTOFF_CONNECTIVITY_OFFLINE_STR		"offline"
/**
 * Name of configuration value for the "restore connectivity" policy
 * when powering on from soft poweroff
 */
#define SOFTOFF_CONNECTIVITY_RESTORE_STR		"restore"
/**
 * Name of configuration value for the "wake on charger" policy
 * when in soft poweroff
 */
#define SOFTOFF_CHARGER_CONNECT_WAKEUP_STR		"wakeup"
/**
 * Name of configuration value for the "ignore charger" policy
 * when in soft poweroff
 */
#define SOFTOFF_CHARGER_CONNECT_IGNORE_STR		"ignore"

/** Soft poweroff connectivity policies */
enum {
	/** Policy not set */
	SOFTOFF_CONNECTIVITY_INVALID = MCE_INVALID_TRANSLATION,
	/** Retain connectivity */
	SOFTOFF_CONNECTIVITY_RETAIN = 0,
	/** Default setting when charger connected */
	DEFAULT_SOFTOFF_CONNECTIVITY_CHARGER = SOFTOFF_CONNECTIVITY_RETAIN,
	/** Go to offline mode if no connections are open */
	SOFTOFF_CONNECTIVITY_SOFT_OFFLINE = 1,
	/** Go to offline mode */
	SOFTOFF_CONNECTIVITY_FORCE_OFFLINE = 2,
	/** Default setting when running on battery */
	DEFAULT_SOFTOFF_CONNECTIVITY_BATTERY = SOFTOFF_CONNECTIVITY_FORCE_OFFLINE,
};

/** Soft poweron connectivity policies */
enum {
	/** Stay in offline mode */
	SOFTOFF_CONNECTIVITY_OFFLINE = 0,
	/** Default setting */
	DEFAULT_SOFTOFF_CONNECTIVITY_POWERON = SOFTOFF_CONNECTIVITY_OFFLINE,
	/** Restore previous mode */
	SOFTOFF_CONNECTIVITY_RESTORE = 1,
};

/** Soft poweroff charger connect policy */
enum {
	/** Stay in offline mode */
	SOFTOFF_CHARGER_CONNECT_WAKEUP = 0,
	/** Restore previous mode */
	SOFTOFF_CHARGER_CONNECT_IGNORE = 1,
	/** Default setting */
	DEFAULT_SOFTOFF_CHARGER_CONNECT = SOFTOFF_CHARGER_CONNECT_IGNORE,
};

void request_powerup(void);
void request_reboot(void);
void request_soft_poweron(void);
void request_soft_poweroff(void);
void request_normal_shutdown(void);

/* When MCE is made modular, this will be handled differently */
gboolean mce_dsme_init(gboolean debug_mode);
void mce_dsme_exit(void);

#endif /* _MCE_DSME_H_ */
