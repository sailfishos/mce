/**
 * @file mce.h
 * Generic headers for Mode Control Entity
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012-2017 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Irina Bezruk <ext-irina.bezruk@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Vesa Halttunen <vesa.halttunen@jollamobile.com>
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
#ifndef _MCE_H_
#define _MCE_H_

#include "datapipe.h"

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

/** LED pattern used when charging the battery */
#define MCE_LED_PATTERN_BATTERY_CHARGING	"PatternBatteryCharging"

/** LED pattern used when the battery is full */
#define MCE_LED_PATTERN_BATTERY_FULL		"PatternBatteryFull"

/** Binary LED pattern used by CSD that should always use sw breathing */
#define MCE_LED_PATTERN_CSD_BINARY_BLINK	"PatternCsdLedBlink"

/** Rgb LED pattern used by CSD that should always use sw breathing */
#define MCE_LED_PATTERN_CSD_WHITE_BLINK		"PatternCsdWhiteBlink"

/** LED pattern used when the battery is low */
#define MCE_LED_PATTERN_BATTERY_LOW		"PatternBatteryLow"

/** LED pattern used when XXX */
#define MCE_LED_PATTERN_BATTERY_CHARGING_FLAT	"PatternBatteryChargingFlat"

/** LED pattern used by messaging mw */
#define MCE_LED_PATTERN_COMMON_NOTIFICATION	"PatternCommonNotification"

/** LED pattern used by messaging mw */
#define MCE_LED_PATTERN_COMMUNICATION_CALL	"PatternCommunicationCall"

/** LED pattern used by messaging mw */
#define MCE_LED_PATTERN_COMMUNICATION_EMAIL	"PatternCommunicationEmail"

/** LED pattern used by messaging mw */
#define MCE_LED_PATTERN_COMMUNICATION_IM	"PatternCommunicationIM"

/** LED pattern used by messaging mw */
#define MCE_LED_PATTERN_COMMUNICATION_SMS	"PatternCommunicationSMS"

/** LED pattern used by CSD application */
#define MCE_LED_PATTERN_CSD_WHITE		"PatternCsdWhite"

/** LED pattern used when blanking fails due to dbus ipc */
#define MCE_LED_PATTERN_DISPLAY_BLANK_FAILED	"PatternDisplayBlankFailed"

/** LED pattern used when unblanking fails due to dbus ipc */
#define MCE_LED_PATTERN_DISPLAY_UNBLANK_FAILED	"PatternDisplayUnblankFailed"

/** LED pattern used when frame buffer suspend fails */
#define MCE_LED_PATTERN_DISPLAY_SUSPEND_FAILED	"PatternDisplaySuspendFailed"

/** LED pattern used when frame buffer resume fails */
#define MCE_LED_PATTERN_DISPLAY_RESUME_FAILED	"PatternDisplayResumeFailed"

/** LED pattern used when mce kills unresponsive lipstick */
#define MCE_LED_PATTERN_KILLING_LIPSTICK	"PatternKillingLipstick"

/** LED pattern used when display is on, but mce holds touch input */
#define MCE_LED_PATTERN_TOUCH_INPUT_BLOCKED	"PatternTouchInputBlocked"

/** LED pattern used when XXX */
#define MCE_LED_PATTERN_DISPLAY_DIMMED		"PatternDisplayDimmed"

/** LED pattern used for communication events */
#define MCE_LED_PATTERN_COMMUNICATION_EVENT	"PatternCommunication"

/** LED pattern used for communication events when battery is full */
#define MCE_LED_PATTERN_COMMUNICATION_EVENT_BATTERY_FULL	"PatternCommunicationAndBatteryFull"

/** LED pattern used when fingerprint scanner is active */
#define MCE_LED_PATTERN_SCANNING_FINGERPRINT	"PatternScanningFingerprint"

/** LED pattern used when fingerprint acquisition events are seen */
#define MCE_LED_PATTERN_FINGERPRINT_ACQUIRED	"PatternFingerprintAcquired"

/** LED pattern used when proximity sensor is covered */
#define MCE_LED_PATTERN_PROXIMITY_COVERED	"PatternProximityCovered"

/** LED pattern used during proximity sensor uncover hysteresis */
#define MCE_LED_PATTERN_PROXIMITY_UNCOVERING	"PatternProximityUncovering"

/** LED pattern used when proximity sensor is uncovered */
#define MCE_LED_PATTERN_PROXIMITY_UNCOVERED	"PatternProximityUncovered"

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

const char *alarm_state_repr(alarm_ui_state_t state);

/** System sub-modes; several of these can be active at once */
typedef enum {
	/** Submode invalid */
	MCE_SUBMODE_INVALID		= (1 << 31),
	/** No submodes enabled */
	MCE_SUBMODE_NORMAL		= 0,
	/** Touchscreen/Keypad lock enabled */
	MCE_SUBMODE_TKLOCK		= (1 << 0),
	/** Event eater enabled */
	MCE_SUBMODE_EVEATER		= (1 << 1),
	/** Bootup in progress */
	MCE_SUBMODE_BOOTUP		= (1 << 3),
	/** State transition in progress */
	MCE_SUBMODE_TRANSITION		= (1 << 4),
	/** Touchscreen/Keypad autorelock active */
	MCE_SUBMODE_AUTORELOCK		= (1 << 5),
	/** Visual Touchscreen/Keypad active */
	MCE_SUBMODE_VISUAL_TKLOCK	= (1 << 6),
	/** Proximity is used to protect from accidental events */
	MCE_SUBMODE_POCKET		= (1 << 7),
	/** Touchscreen/Keypad lock is enabled based on proximity state */
	MCE_SUBMODE_PROXIMITY_TKLOCK	= (1 << 8),
	/** Device is in MALF state */
	MCE_SUBMODE_MALF		= (1 << 9),
} submode_t;

const char *submode_change_repr(submode_t prev, submode_t curr);
const char *submode_repr(submode_t submode);

/** System state */
typedef enum {
	MCE_SYSTEM_STATE_UNDEF    = -1, /**< System state not set */
	MCE_SYSTEM_STATE_SHUTDOWN =  0, /**< System is in shutdown state */
	MCE_SYSTEM_STATE_USER     =  2, /**< System is in user state */
	MCE_SYSTEM_STATE_ACTDEAD  =  5, /**< System is in acting dead state */
	MCE_SYSTEM_STATE_REBOOT   =  6, /**< System is in reboot state */
	MCE_SYSTEM_STATE_BOOT     =  9, /**< System is in bootup state */
} system_state_t;

const char *system_state_repr(system_state_t state);

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
	CALL_STATE_SERVICE = 3,
	/** Ringing call that is ignored by call ui and mce */
	CALL_STATE_IGNORED = 4,
} call_state_t;

const char *call_state_repr(call_state_t state);
const char *call_state_to_dbus(call_state_t state);
call_state_t call_state_from_dbus(const char *name);

/** Call type */
typedef enum {
	/** Invalid call type */
	CALL_TYPE_INVALID   = MCE_INVALID_TRANSLATION,
	/** The call is a normal call */
	CALL_TYPE_NORMAL    = 0,
	/** The call is an emergency call */
	CALL_TYPE_EMERGENCY = 1
} call_type_t;

const char *call_type_repr(call_type_t type);
call_type_t call_type_parse(const char *name);

/** Display state */
typedef enum {
	MCE_DISPLAY_UNDEF,       /**< Display state not set */
	MCE_DISPLAY_OFF,         /**< Display is off */
	MCE_DISPLAY_LPM_OFF,     /**< Display is off in low power mode */
	MCE_DISPLAY_LPM_ON,      /**< Display is on in low power mode */
	MCE_DISPLAY_DIM,         /**< Display is dimmed */
	MCE_DISPLAY_ON,          /**< Display is on */
	MCE_DISPLAY_POWER_UP,    /**< Display is resuming */
	MCE_DISPLAY_POWER_DOWN,  /**< Display is suspending */

	MCE_DISPLAY_NUMSTATES    /**< Number of display states */

} display_state_t;

const char *display_state_repr(display_state_t state);

/** Cover state */
typedef enum {
	COVER_UNDEF = -1,		/**< Cover state not set */
	COVER_CLOSED = 0,		/**< Cover is closed */
	COVER_OPEN = 1			/**< Cover is open */
} cover_state_t;

const char *cover_state_repr(cover_state_t state);
const char *proximity_state_repr(cover_state_t state);

/** Lock state */
typedef enum {
	/** Lock state not set */
	TKLOCK_REQUEST_UNDEF         = -1,
	/** Lock is disabled */
	TKLOCK_REQUEST_OFF           =  0,
	/** Delayed unlock; write only */
	TKLOCK_REQUEST_OFF_DELAYED   =  1,
	/** Lock is disabled, but autorelock isn't disabled; write only */
	TKLOCK_REQUEST_OFF_PROXIMITY =  2,
	/** Lock is enabled */
	TKLOCK_REQUEST_ON            =  3,
	/** Dimmed lock; write only */
	TKLOCK_REQUEST_ON_DIMMED     =  4,
	/** Enable proximity lock (no UI); write only */
	TKLOCK_REQUEST_ON_PROXIMITY  =  5,
	/** Toggle lock state; write only */
	TKLOCK_REQUEST_TOGGLE        =  6,
	/** Delayed lock; write only */
	TKLOCK_REQUEST_ON_DELAYED    =  7,
} tklock_request_t;

const char *tklock_request_repr(tklock_request_t state);

const char *tklock_status_repr(int status);

/** Assumed initial battery level */
#define BATTERY_LEVEL_INITIAL 100

/** Battery status */
typedef enum {
	BATTERY_STATUS_UNDEF = -1,	/**< Battery status not known */
	BATTERY_STATUS_FULL = 0,	/**< Battery full */
	BATTERY_STATUS_OK = 1,		/**< Battery ok */
	BATTERY_STATUS_LOW = 2,		/**< Battery low */
	BATTERY_STATUS_EMPTY = 3,	/**< Battery empty */
} battery_status_t;

const char *battery_status_repr(battery_status_t state);
const char *battery_status_to_dbus(battery_status_t state);

/** Charging status */
typedef enum {
	CHARGER_STATE_UNDEF = -1,	/**< Not known yet */
	CHARGER_STATE_OFF   =  0,	/**< Not charging */
	CHARGER_STATE_ON    =  1,	/**< Charging */
} charger_state_t;

const char *charger_state_repr(charger_state_t state);
const char *charger_state_to_dbus(charger_state_t state);

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
	USB_CABLE_UNDEF        = -1,	/**< Usb cable state not set */
	USB_CABLE_DISCONNECTED =  0,	/**< Cable is not connected */
	USB_CABLE_CONNECTED    =  1,	/**< Cable is connected */
	USB_CABLE_ASK_USER     =  2,	/**< Ask mode from user */
} usb_cable_state_t;

const char *usb_cable_state_repr(usb_cable_state_t state);
const char *usb_cable_state_to_dbus(usb_cable_state_t state);

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
	UIEXCEPTION_TYPE_NONE   = 0,
	UIEXCEPTION_TYPE_LINGER = 1<<0,
	UIEXCEPTION_TYPE_CALL   = 1<<1,
	UIEXCEPTION_TYPE_ALARM  = 1<<2,
	UIEXCEPTION_TYPE_NOTIF  = 1<<3,
	UIEXCEPTION_TYPE_NOANIM = 1<<4,
} uiexception_type_t;

const char *uiexception_type_repr(uiexception_type_t type);
const char *uiexception_type_to_dbus(uiexception_type_t type);

/** D-Bus service availability */
typedef enum {
	SERVICE_STATE_UNDEF   = -1,
	SERVICE_STATE_STOPPED =  0,
	SERVICE_STATE_RUNNING =  1,
} service_state_t;

const char *service_state_repr(service_state_t state);

/** These must match with what sensorfw uses */
typedef enum
{
    MCE_ORIENTATION_UNDEFINED   = 0,  /**< Orientation is unknown. */
    MCE_ORIENTATION_LEFT_UP     = 1,  /**< Device left side is up */
    MCE_ORIENTATION_RIGHT_UP    = 2,  /**< Device right side is up */
    MCE_ORIENTATION_BOTTOM_UP   = 3,  /**< Device bottom is up */
    MCE_ORIENTATION_BOTTOM_DOWN = 4,  /**< Device bottom is down */
    MCE_ORIENTATION_FACE_DOWN   = 5,  /**< Device face is down */
    MCE_ORIENTATION_FACE_UP     = 6,  /**< Device face is up */
} orientation_state_t;

const char *orientation_state_repr(orientation_state_t state);

/** Key pressed/realease state */
typedef enum {
    KEY_STATE_UNDEF    = -1,
    KEY_STATE_RELEASED =  0,
    KEY_STATE_PRESSED  =  1,
} key_state_t;

const char *key_state_repr(key_state_t state);

/** Generic "extended boolean" type */
typedef enum
{
  TRISTATE_UNKNOWN = -1,
  TRISTATE_FALSE   =  0,
  TRISTATE_TRUE    =  1,
} tristate_t;

const char *tristate_repr(tristate_t state);

/** Fingerprint daemon state */
typedef enum fpstate_t
{
    FPSTATE_UNSET,
    FPSTATE_ENUMERATING,
    FPSTATE_IDLE,
    FPSTATE_ENROLLING,
    FPSTATE_IDENTIFYING,
    FPSTATE_REMOVING,
    FPSTATE_VERIFYING,
    FPSTATE_ABORTING,
    FPSTATE_TERMINATING,
} fpstate_t;

fpstate_t   fpstate_parse(const char *name);
const char *fpstate_repr (fpstate_t state);

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
#define DEFAULT_INACTIVITY_DELAY	33

submode_t mce_get_submode_int32(void);
gboolean mce_add_submode_int32(const submode_t submode);
gboolean mce_rem_submode_int32(const submode_t submode);

bool mce_in_valgrind_mode(void);
bool mce_in_sensortest_mode(void);
void mce_abort(void) __attribute__((noreturn));
void mce_quit_mainloop(void);
void mce_signal_handlers_remove(void);

#define display_state_get() ({\
	gint res = GPOINTER_TO_INT(display_state_curr_pipe.dp_cached_data);\
	mce_log(LL_DEBUG, "display_state_curr=%s",\
		display_state_repr(res));\
	res;\
})

/** Clip integer value to given range
 *
 * @param range_lo minimum value
 * @param range_hi maximum value
 * @param val      value to clip
 *
 * @return val clipped to the range
 */
static inline int
mce_clip_int(int range_lo, int range_hi, int val)
{
	return val < range_lo ? range_lo : val > range_hi ? range_hi : val;
}

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

	return mce_clip_int(dst_lo, dst_hi, val);
}

#endif /* _MCE_H_ */
