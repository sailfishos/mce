/**
 * @file led.h
 * Headers for the LED module
 * <p>
 * Copyright © 2006-2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2013-2019 Jolla Ltd.
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
#ifndef LED_H_
#define LED_H_

/* ========================================================================= *
 * Configuration
 * ========================================================================= */

/** Name of LED configuration group */
#define MCE_CONF_LED_GROUP			"LED"

/** Name of configuration key for the list of required LED patterns */
#define MCE_CONF_LED_PATTERNS_REQUIRED		"LEDPatternsRequired"

/** Name of configuration key for the list of disabled LED patterns */
#define MCE_CONF_LED_PATTERNS_DISABLED		"LEDPatternsDisabled"

/** Name of configuration key for the list of LED Pattern combination-rules */
#define MCE_CONF_LED_COMBINATION_RULES		"CombinationRules"

/**
 * Name of LED RGB pattern configuration group for libhybris
 */
#define MCE_CONF_LED_PATTERN_HYBRIS_GROUP	"LEDPatternHybris"

/* ========================================================================= *
 * Settings
 * ========================================================================= */

/** Prefix for led setting keys */
#define MCE_SETTING_LED_PATH			"/system/osso/dsm/leds"

/** Whether sw based led breathing is enabled */
#define MCE_SETTING_LED_SW_BREATH_ENABLED	MCE_SETTING_LED_PATH"/sw_breath_enabled"
#define MCE_DEFAULT_LED_SW_BREATH_ENABLED	true

/** Mimimum battery level to allow led breathing without charger */
#define MCE_SETTING_LED_SW_BREATH_BATTERY_LIMIT	MCE_SETTING_LED_PATH"/sw_breath_battery_limit"
#define MCE_DEFAULT_LED_SW_BREATH_BATTERY_LIMIT	101

/** Default value for LED pattern enabled settings
 *
 * Note: Keynames are dynamically constructed from MCE_SETTING_LED_PATH
 *       prefix and led pattern name.
 */
#define MCE_DEFAULT_LED_PATTERN_ENABLED		true

/* ========================================================================= *
 * HW Constants
 * ========================================================================= */

/** Maximum libhybris led brightness */
#define MAXIMUM_HYBRIS_LED_BRIGHTNESS		100	/* % */

#endif /* LED_H_ */
