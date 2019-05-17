/**
 * @file radiostates.h
 * Headers for the radio states module
 * <p>
 * Copyright © 2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2014-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
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
#ifndef _RADIOSTATES_H_
#define _RADIOSTATES_H_

/** Name of radio states configuration group */
#define MCE_CONF_RADIO_STATES_GROUP     "RadioStates"

/** Name of Master radio state configuration key */
#define MCE_CONF_MASTER_RADIO_STATE     "MasterRadioState"

/** Name of Master radio state configuration key */
#define MCE_CONF_CELLULAR_RADIO_STATE   "CellularRadioState"

/** Name of WLAN radio state configuration key */
#define MCE_CONF_WLAN_RADIO_STATE       "WLANRadioState"

/** Name of Bluetooth radio state configuration key */
#define MCE_CONF_BLUETOOTH_RADIO_STATE  "BluetoothRadioState"

/** Name of NFC radio state configuration key */
#define MCE_CONF_NFC_RADIO_STATE        "NFCRadioState"

/** Name of FM transmitter radio state configuration key */
#define MCE_CONF_FMTX_RADIO_STATE       "FMTXRadioState"

/** Default Master radio state */
#define DEFAULT_MASTER_RADIO_STATE      FALSE

/** Default Cellular radio state */
#define DEFAULT_CELLULAR_RADIO_STATE    FALSE

/** Default WLAN radio state */
#define DEFAULT_WLAN_RADIO_STATE        FALSE

/** Default Bluetooth radio state */
#define DEFAULT_BLUETOOTH_RADIO_STATE   FALSE

/** Default NFC radio state */
#define DEFAULT_NFC_RADIO_STATE         FALSE

/** Default FM transmitter radio state */
#define DEFAULT_FMTX_RADIO_STATE        FALSE

/** Path to online radio states file */
#define MCE_ONLINE_RADIO_STATES_PATH    G_STRINGIFY(MCE_VAR_DIR) "/radio_states.online"

/** Path to offline radio states file */
#define MCE_OFFLINE_RADIO_STATES_PATH   G_STRINGIFY(MCE_VAR_DIR) "/radio_states.offline"

#endif /* _RADIOSTATES_H_ */
