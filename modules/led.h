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
 * Name of LED single-colour pattern configuration group for
 * RX-34
 */
#define MCE_CONF_LED_PATTERN_RX34_GROUP		"LEDPatternMonoRX34"

/**
 * Name of LED NJoy-controlled RGB pattern configuration group for
 * RX-44
 */
#define MCE_CONF_LED_PATTERN_RX44_GROUP		"LEDPatternNJoyRX44"

/**
 * Name of LED NJoy-controlled RGB pattern configuration group for
 * RX-48
 */
#define MCE_CONF_LED_PATTERN_RX48_GROUP		"LEDPatternNJoyRX48"

/**
 * Name of LED Lysti-controlled RGB pattern configuration group for
 * RX-51
 */
#define MCE_CONF_LED_PATTERN_RX51_GROUP		"LEDPatternLystiRX51"

/**
 * Name of LED Lysti-controlled RGB pattern configuration group for
 * RM-680/RM-690
 */
#define MCE_CONF_LED_PATTERN_RM680_GROUP	"LEDPatternLystiRM680"

/**
 * Name of LED Lysti-controlled RGB pattern configuration group for
 * RM-696/RM-716
 */
#define MCE_CONF_LED_PATTERN_RM696_GROUP	"LEDPatternNJoyRM696"

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

/** Reno control channel */
#define TWL5031_BCC				0x4a

/** Reno LED controller */
#define LED_DRIVER_CTRL				0xaf

/** Reno command to disable LED */
#define LEDC_DISABLE				0x08

/** Default NJoy (LP5521)-controlled RGB LED current */
#define MAXIMUM_NJOY_RGB_LED_CURRENT		47	/* 4.7 mA */

/** Default NJoy (LP5521)-controlled monochrome LED current */
#define MAXIMUM_NJOY_MONOCHROME_LED_CURRENT	50	/* 5.0 mA */

/** Maximum Lysti (LP5523)-controlled RGB LED current */
#define MAXIMUM_LYSTI_RGB_LED_CURRENT		47	/* 4.7 mA */

/** Maximum Lysti (LP5523)-controlled monochrome LED current */
#define MAXIMUM_LYSTI_MONOCHROME_LED_CURRENT	100	/* 10.0 mA */

/** Maximum libhybris led brightness */
#define MAXIMUM_HYBRIS_LED_BRIGHTNESS		100	/* % */

/** Path to the mono LED /sys directory */
#define MCE_MONO_LED_SYS_PATH			"/sys/class/leds/keypad"

/** Monochrome LED on period file */
#define MCE_LED_ON_PERIOD_PATH			MCE_MONO_LED_SYS_PATH "/delay_on"

/** Monochrome LED off period file */
#define MCE_LED_OFF_PERIOD_PATH			MCE_MONO_LED_SYS_PATH "/delay_off"

/** Monochrome LED trigger file */
#define MCE_LED_TRIGGER_PATH			MCE_MONO_LED_SYS_PATH "/trigger"

/* Trigger type */

/** Timer based trigger */
#define MCE_LED_TRIGGER_TIMER			"timer"

/** No trigger */
#define MCE_LED_TRIGGER_NONE			"none"

/* LED modes */

/** LED disabled */
#define MCE_LED_DISABLED_MODE			"disabled"

/** LED direct control mode */
#define MCE_LED_DIRECT_MODE			"direct"

/** LED load mode */
#define MCE_LED_LOAD_MODE			"load"

/** LED run mode */
#define MCE_LED_RUN_MODE			"run"

/** Suffix used for LED current */
#define MCE_LED_CURRENT_SUFFIX			"/led_current"

/** Suffix used for LED brightness */
#define MCE_LED_BRIGHTNESS_SUFFIX		"/brightness"

/** Path to direct LED control /sys directory */
#define MCE_LED_DIRECT_SYS_PATH			"/sys/class/leds"

/** Directory prefix for keypad LED controller */
#define MCE_LED_KEYPAD_PREFIX			"/keypad"

/** Directory prefix for cover LED controller */
#define MCE_LED_COVER_PREFIX			"/cover"

/** Directory prefix for keyboard LED controller */
#define MCE_LED_KEYBOARD_PREFIX			"/keyboard"

/** Directory prefix for LP5521 LED controller */
#define MCE_LED_LP5521_PREFIX			"/lp5521"

/** Directory prefix for LP5523 LED controller */
#define MCE_LED_LP5523_PREFIX			"/lp5523"

/** Name of LED channel 0 */
#define MCE_LED_CHANNEL0			":channel0"

/** Name of LED channel 1 */
#define MCE_LED_CHANNEL1			":channel1"

/** Name of LED channel 2 */
#define MCE_LED_CHANNEL2			":channel2"

/** Name of LED channel 3 */
#define MCE_LED_CHANNEL3			":channel3"

/** Name of LED channel 4 */
#define MCE_LED_CHANNEL4			":channel4"

/** Name of LED channel 5 */
#define MCE_LED_CHANNEL5			":channel5"

/** Name of LED channel 6 */
#define MCE_LED_CHANNEL6			":channel6"

/** Name of LED channel 7 */
#define MCE_LED_CHANNEL7			":channel7"

/** Name of LED channel 8 */
#define MCE_LED_CHANNEL8			":channel8"

/** Name of Engine 1 */
#define MCE_LED_ENGINE1				"/engine1_"

/** Name of Engine 2 */
#define MCE_LED_ENGINE2				"/engine2_"

/** Name of Engine 3 */
#define MCE_LED_ENGINE3				"/engine3_"

/** LED device suffix */
#define MCE_LED_DEVICE				"/device"

/** LED mode suffix */
#define MCE_LED_MODE_SUFFIX			"mode"

/** LED load suffix */
#define MCE_LED_LOAD_SUFFIX			"load"

/** LED leds suffix */
#define MCE_LED_LEDS_SUFFIX			"leds"

/**
 * Lysti LED mask for LED 1 (channel 8);
 * RM-680/RM-690 monochrome LED
 * RX-51 keyboard backlight 6
 */
#define MCE_LYSTI_LED1_MASK			(1 << 0)

/**
 * Lysti LED mask for LED 2 (channel 7);
 * RM-680/RM-690 Monochrome LED behind model name
 * RX-51 keyboard backlight 5
 */
#define MCE_LYSTI_LED2_MASK			(1 << 1)

/**
 * Lysti LED mask for LED 3 (channel 6);
 * RM-680/RM-690 Monochrome LED behind model name
 * RX-51 red component of RGB LED
 */
#define MCE_LYSTI_LED3_MASK			(1 << 2)

/**
 * Lysti LED mask for LED 4 (channel 5);
 * RM-680/RM-690 monochrome keyboard backlight 6
 * RX-51 green component of RGB LED
 */
#define MCE_LYSTI_LED4_MASK			(1 << 3)

/**
 * Lysti LED mask for LED 5 (channel 4);
 * RM-680/RM-690 monochrome keyboard backlight 5
 * RX-51 blue component of RGB LED
 */
#define MCE_LYSTI_LED5_MASK			(1 << 4)

/**
 * Lysti LED mask for LED 6 (channel 3);
 * RM-680/RM-690 monochrome keyboard backlight 4
 * RX-51 monochrome keyboard backlight 4
 */
#define MCE_LYSTI_LED6_MASK			(1 << 5)

/**
 * Lysti LED mask for LED 7 (channel 2);
 * RM-680/RM-690 monochrome keyboard backlight 3
 * RX-51 monochrome keyboard backlight 3
 */
#define MCE_LYSTI_LED7_MASK			(1 << 6)

/**
 * Lysti LED mask for LED 8 (channel 1);
 * RM-680/RM-690 monochrome keyboard backlight 2
 * RX-51 monochrome keyboard backlight 2
 */
#define MCE_LYSTI_LED8_MASK			(1 << 7)

/**
 * Lysti LED mask for LED 9 (channel 0);
 * RM-680/RM-690 monochrome keyboard backlight 1
 * RX-51 monochrome keyboard backlight 1
 */
#define MCE_LYSTI_LED9_MASK			(1 << 8)

/**
 * Lysti LED mask for the
 * RM-680/RM-690 monochrome power button LED
 */
#define MCE_LYSTI_MONOCHROME_MASK_RM680		MCE_LYSTI_LED1_MASK

/** Lysti LED mask for the RX-51 red RGB LED component */
#define MCE_LYSTI_RED_MASK_RX51			MCE_LYSTI_LED3_MASK

/** Lysti LED mask for the RX-51 green RGB LED component */
#define MCE_LYSTI_GREEN_MASK_RX51		MCE_LYSTI_LED4_MASK

/** Lysti LED mask for the RX-51 blue RGB LED component */
#define MCE_LYSTI_BLUE_MASK_RX51		MCE_LYSTI_LED5_MASK

/** Lysti LED mask for the RX-51 keyboard backlight */
#define MCE_LYSTI_KB_BACKLIGHT_MASK_RX51	(MCE_LYSTI_LED9_MASK | \
						 MCE_LYSTI_LED8_MASK | \
						 MCE_LYSTI_LED7_MASK | \
						 MCE_LYSTI_LED6_MASK | \
						 MCE_LYSTI_LED2_MASK | \
						 MCE_LYSTI_LED1_MASK)

/** Lysti LED mask for the RM-680/RM-690 keyboard backlight */
#define MCE_LYSTI_KB_BACKLIGHT_MASK_RM680	(MCE_LYSTI_LED9_MASK | \
						 MCE_LYSTI_LED8_MASK | \
						 MCE_LYSTI_LED7_MASK | \
						 MCE_LYSTI_LED6_MASK | \
						 MCE_LYSTI_LED5_MASK | \
						 MCE_LYSTI_LED4_MASK)

/* Brightness levels used by the keypad LED */
#define BRIGHTNESS_LEVEL_0			"0"	/**< off */
#define BRIGHTNESS_LEVEL_1			"12"	/**< faintest */
#define BRIGHTNESS_LEVEL_2			"24"	/**< level 2 */
#define BRIGHTNESS_LEVEL_3			"36"	/**< level 3 */
#define BRIGHTNESS_LEVEL_4			"48"	/**< level 4 */
#define BRIGHTNESS_LEVEL_5			"60"	/**< level 5 */
#define BRIGHTNESS_LEVEL_6			"72"	/**< level 6 */
#define BRIGHTNESS_LEVEL_7			"84"	/**< level 7 */
#define BRIGHTNESS_LEVEL_8			"96"	/**< level 8 */
#define BRIGHTNESS_LEVEL_9			"108"	/**< level 9 */
#define BRIGHTNESS_LEVEL_10			"120"	/**< level 10 */
#define BRIGHTNESS_LEVEL_11			"132"	/**< level 11 */
#define BRIGHTNESS_LEVEL_12			"144"	/**< level 12 */
#define BRIGHTNESS_LEVEL_13			"156"	/**< level 13 */
#define BRIGHTNESS_LEVEL_14			"168"	/**< level 14 */
#define BRIGHTNESS_LEVEL_15			"180"	/**< brightest */

#endif /* LED_H_ */
