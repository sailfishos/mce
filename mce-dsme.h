/**
 * @file mce-dsme.h
 * Headers for the DSME<->MCE interface and logic
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
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

/** How long to delay removal of MCE_TRANSITION_SUBMODE after
 *  receiving system state change from dsme
 *
 * <0 -> immediately
 *  0 -> from idle callback
 * >0 -> from timer callback after TRANSITION_DELAY ms
 */
#define TRANSITION_DELAY		-1

/** Name of Powerkey configuration group */
#define MCE_CONF_SOFTPOWEROFF_GROUP	"SoftPowerOff"

/** Name of configuration key for charger connect policy in soft poweroff */
#define MCE_CONF_SOFTPOWEROFF_CHARGER_POLICY_CONNECT "ChargerPolicyConnect"

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

/** Soft poweroff charger connect policy */
enum {
	/** Stay in offline mode */
	SOFTOFF_CHARGER_CONNECT_WAKEUP = 0,
	/** Restore previous mode */
	SOFTOFF_CHARGER_CONNECT_IGNORE = 1,
	/** Default setting */
	DEFAULT_SOFTOFF_CHARGER_CONNECT = SOFTOFF_CHARGER_CONNECT_IGNORE,
};

void mce_dsme_request_powerup(void);
void mce_dsme_request_reboot(void);
void mce_dsme_request_soft_poweron(void);
void mce_dsme_request_soft_poweroff(void);
void mce_dsme_request_normal_shutdown(void);

/* When MCE is made modular, this will be handled differently */
gboolean mce_dsme_init(void);
void mce_dsme_exit(void);

#endif /* _MCE_DSME_H_ */
