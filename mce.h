/**
 * @file mce.h
 * Generic headers for Mode Control Entity
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
#ifndef _MCE_H_
#define _MCE_H_

#include "datapipe.h"

/** Dummy _() */
#define _(str_)		str_

/** Indicate enabled (sub)mode */
#define DISABLED_STRING			"yes"

/** Indicate disabled (sub)mode */
#define ENABLED_STRING			"no"

/* Names of LED patterns */

/** LED pattern used when powering on the device */
#define MCE_LED_PATTERN_POWER_ON		"PatternPowerOn"

/** LED pattern used when powering off the device */
#define MCE_LED_PATTERN_POWER_OFF		"PatternPowerOff"

/** LED pattern used when camera is active */
#define MCE_LED_PATTERN_CAMERA			"PatternWebcamActive"

/** LED pattern used to indicate that the device is on when idle */
#define MCE_LED_PATTERN_DEVICE_ON		"PatternDeviceOn"

/** LED pattern used when the device is in soft poweroff mode */
#define MCE_LED_PATTERN_DEVICE_SOFT_OFF		"PatternDeviceSoftOff"

/** LED pattern used when charging the battery */
#define MCE_LED_PATTERN_BATTERY_CHARGING	"PatternBatteryCharging"

/** LED pattern used when the battery is full */
#define MCE_LED_PATTERN_BATTERY_FULL		"PatternBatteryFull"

/** LED pattern used when the battery is low */
#define MCE_LED_PATTERN_BATTERY_LOW		"PatternBatteryLow"

/** LED pattern used for communication events */
#define MCE_LED_PATTERN_COMMUNICATION_EVENT	"PatternCommunication"

/** LED pattern used for communication events when battery is full */
#define MCE_LED_PATTERN_COMMUNICATION_EVENT_BATTERY_FULL	"PatternCommunicationAndBatteryFull"

/** Persistent lock file for backups */
#define MCE_SETTINGS_LOCK_FILE_PATH		G_STRINGIFY(MCE_RUN_DIR) "/restored"

/** Path for system MALF state indicator file */
#define MALF_FILENAME				"/var/malf"

/** Path for MCE MALF state indicator file */
#define MCE_MALF_FILENAME			G_STRINGIFY(MCE_RUN_DIR) "/malf"

/** Module information */
typedef struct {
	/** Name of the module */
	const gchar *const name;

	/** Module dependencies */
	const gchar *const *const depends;

	/** Module recommends */
	const gchar *const *const recommends;

	/** Module provides */
	const gchar *const *const provides;

	/** Module provides */
	const gchar *const *const enhances;

	/** Module conflicts */
	const gchar *const *const conflicts;

	/** Module replaces */
	const gchar *const *const replaces;

	/** Module priority:
	 * lower value == higher priority
	 * This value is only used when modules conflict
	 */
	const gint priority;
} module_info_struct;

/** Used for invalid translations and values */
#define MCE_INVALID_TRANSLATION		-1

/** Alarm UI states; integer representations */
typedef enum {
	/** Alarm UI state not valid */
	MCE_ALARM_UI_INVALID_INT32 = MCE_INVALID_TRANSLATION,
	/** Alarm UI not visible */
	MCE_ALARM_UI_OFF_INT32 = 0,
	/** Alarm UI visible and ringing */
	MCE_ALARM_UI_RINGING_INT32 = 1,
	/** Alarm UI visible but not ringing */
	MCE_ALARM_UI_VISIBLE_INT32 = 2,
} alarm_ui_state_t;

/** System sub-modes; several of these can be active at once */
typedef gint submode_t;

/** Submode invalid */
#define MCE_INVALID_SUBMODE		(1 << 31)

/** No submodes enabled */
#define MCE_NORMAL_SUBMODE		0

/** Touchscreen/Keypad lock enabled */
#define MCE_TKLOCK_SUBMODE		(1 << 0)

/** Event eater enabled */
#define MCE_EVEATER_SUBMODE		(1 << 1)

/** Device emulates soft poweroff */
#define MCE_SOFTOFF_SUBMODE		(1 << 2)

/** Bootup in progress */
#define MCE_BOOTUP_SUBMODE		(1 << 3)

/** State transition in progress */
#define MCE_TRANSITION_SUBMODE		(1 << 4)

/** Touchscreen/Keypad autorelock active */
#define MCE_AUTORELOCK_SUBMODE		(1 << 5)

/** Visual Touchscreen/Keypad active */
#define MCE_VISUAL_TKLOCK_SUBMODE	(1 << 6)

/** Proximity is used to protect from accidental events */
#define MCE_POCKET_SUBMODE		(1 << 7)

/** Touchscreen/Keypad lock is enabled based on proximity state */
#define MCE_PROXIMITY_TKLOCK_SUBMODE	(1 << 8)

/** Device is in MALF state */
#define MCE_MALF_SUBMODE		(1 << 9)

/** System state */
typedef enum {
	MCE_STATE_UNDEF = -1,		/**< System state not set */
	MCE_STATE_SHUTDOWN = 0,		/**< System is in shutdown state */
	MCE_STATE_USER = 2,		/**< System is in user state */
	MCE_STATE_ACTDEAD = 5,		/**< System is in acting dead state */
	MCE_STATE_REBOOT = 6,		/**< System is in reboot state */
	MCE_STATE_BOOT = 9		/**< System is in bootup state */
} system_state_t;

/** Call state */
typedef enum {
	/** Invalid call state */
	CALL_STATE_INVALID = MCE_INVALID_TRANSLATION,
	/** No call on-going */
	CALL_STATE_NONE = 0,
	/** There's an incoming call ringing */
	CALL_STATE_RINGING = 1,
	/** There's an active call */
	CALL_STATE_ACTIVE = 2,
	/** The device is in service state */
	CALL_STATE_SERVICE = 3
} call_state_t;

/** Call type */
typedef enum {
	/** Invalid call type */
	INVALID_CALL = MCE_INVALID_TRANSLATION,
	/** The call is a normal call */
	NORMAL_CALL = 0,
	/** The call is an emergency call */
	EMERGENCY_CALL = 1
} call_type_t;

/** Display state */
typedef enum {
	MCE_DISPLAY_UNDEF = -1,		/**< Display state not set */
	MCE_DISPLAY_OFF	= 0,		/**< Display is off */
	MCE_DISPLAY_LPM_OFF = 1,	/**< Display is off in low power mode */
	MCE_DISPLAY_LPM_ON = 2,		/**< Display is on in low power mode */
	MCE_DISPLAY_DIM = 3,		/**< Display is dimmed */
	MCE_DISPLAY_ON = 4,		/**< Display is on */
	MCE_DISPLAY_POWER_UP,		/**< Display is resuming */
	MCE_DISPLAY_POWER_DOWN,		/**< Display is suspending */
} display_state_t;

/** Cover state */
typedef enum {
	COVER_UNDEF = -1,		/**< Cover state not set */
	COVER_CLOSED = 0,		/**< Cover is closed */
	COVER_OPEN = 1			/**< Cover is open */
} cover_state_t;

/** Lock state */
typedef enum {
	/** Lock state not set */
	LOCK_UNDEF = -1,
	/** Lock is disabled */
	LOCK_OFF = 0,
	/** Delayed unlock; write only */
	LOCK_OFF_DELAYED = 1,
	/** Lock is disabled, but autorelock isn't disabled; write only */
	LOCK_OFF_PROXIMITY = 2,
	/** Lock is enabled */
	LOCK_ON = 3,
	/** Dimmed lock; write only */
	LOCK_ON_DIMMED = 4,
	/** Enable proximity lock (no UI); write only */
	LOCK_ON_PROXIMITY = 5,
	/** Toggle lock state; write only */
	LOCK_TOGGLE = 6,
	/** Delayed lock; write only */
	LOCK_ON_DELAYED = 7
} lock_state_t;

/** Battery status */
typedef enum {
	BATTERY_STATUS_UNDEF = -1,	/**< Battery status not known */
	BATTERY_STATUS_FULL = 0,	/**< Battery full */
	BATTERY_STATUS_OK = 1,		/**< Battery ok */
	BATTERY_STATUS_LOW = 2,		/**< Battery low */
	BATTERY_STATUS_EMPTY = 3,	/**< Battery empty */
} battery_status_t;

/** Camera button state */
typedef enum {
	CAMERA_BUTTON_UNDEF = -1,	/**< Camera button state not set */
	CAMERA_BUTTON_UNPRESSED = 0,	/**< Camera button not pressed */
	CAMERA_BUTTON_LAUNCH = 1,	/**< Camera button fully pressed */
} camera_button_state_t;

/** Audio route */
typedef enum {
	/** Audio route not defined */
	AUDIO_ROUTE_UNDEF = -1,
	/** Audio routed to handset */
	AUDIO_ROUTE_HANDSET = 0,
	/** Audio routed to speaker */
	AUDIO_ROUTE_SPEAKER = 1,
	/** Audio routed to headset */
	AUDIO_ROUTE_HEADSET = 2,
} audio_route_t;

/** USB cable state */
typedef enum {
	USB_CABLE_UNDEF = -1,		/**< Usb cable state not set */
	USB_CABLE_DISCONNECTED = 0,	/**< Cable is not connected */
	USB_CABLE_CONNECTED = 1		/**< Cable is connected */
} usb_cable_state_t;

/** Thermal status */
typedef enum {
	/** Thermal state not set */
	THERMAL_STATE_UNDEF = -1,
	/** Thermal state ok */
	THERMAL_STATE_OK = 0,
	/** Thermal sensors indicate overheating */
	THERMAL_STATE_OVERHEATED = 1,
} thermal_state_t;

/** Exceptional UI status */
typedef enum {
	UIEXC_NONE   = 0,
	UIEXC_LINGER = 1<<0,
	UIEXC_CALL   = 1<<1,
	UIEXC_ALARM  = 1<<2,
	UIEXC_NOTIF  = 1<<3,
} uiexctype_t;

/* XXX: use HAL */

/** Does the device have a flicker key? */
extern gboolean has_flicker_key;

/**
 * Default inactivity timeout, in seconds;
 * dim timeout: 30 seconds
 * blank timeout: 3 seconds
 *
 * Used in case the display module doesn't load for some reason
 */
#define DEFAULT_INACTIVITY_TIMEOUT	33

submode_t mce_get_submode_int32(void);
gboolean mce_add_submode_int32(const submode_t submode);
gboolean mce_rem_submode_int32(const submode_t submode);

void mce_abort(void) __attribute__((noreturn));
void mce_quit_mainloop(void);

#define display_state_get() ({\
	gint res = GPOINTER_TO_INT(display_state_pipe.cached_data);\
	mce_log(LL_DEBUG, "display_state=%d", res);\
	res;\
})

#define proximity_state_get() ({\
	gint res = GPOINTER_TO_INT(proximity_sensor_pipe.cached_data);\
	mce_log(LL_DEBUG, "proximity_state=%d", res);\
	res;\
})

/** Translate integer value from one range to another
 *
 * Linear conversion of a value in [src_lo, src_hi] range
 * to [dst_lo, dst_hi] range.
 *
 * Uses rounding, so that 55 [0,100] -> 6 [0, 10].
 *
 * @param src_lo lower bound for source range
 * @param src_hi upper bound for source range
 * @param dst_lo lower bound for destination range
 * @param dst_hi upper bound for destination range
 * @param val    value in source range to be translated
 *
 * @return input value mapped to destination range
 */
static inline int
mce_xlat_int(int src_lo, int src_hi, int dst_lo, int dst_hi, int val)
{
	/* Deal with empty ranges first; assume that the
	 * low bound is sanest choise available */
	if( src_lo >= src_hi || dst_lo >= dst_hi )
		return dst_lo;

	int src_range = src_hi - src_lo;
	int dst_range = dst_hi - dst_lo;

	val -= src_lo;
	val = (val * dst_range + src_range / 2) / src_range;
	val += dst_lo;

	if( val > dst_hi ) val = dst_hi; else
	if( val < dst_lo ) val = dst_lo;

	return val;
}

#endif /* _MCE_H_ */
