/**
 * @file tklock.c
 * This file implements the touchscreen/keypad lock component
 * of the Mode Control Entity
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
#include <glib.h>
#include <glib/gstdio.h>		/* g_access() */

#include <errno.h>			/* errno */
#include <string.h>			/* strcmp() */
#include <unistd.h>			/* W_OK */
#include <linux/input.h>		/* struct input_event */

#include <mce/mode-names.h>
#include <systemui/dbus-names.h>
#include <systemui/tklock-dbus-names.h>

#include "mce.h"
#include "tklock.h"

#include "mce-io.h"			/* mce_write_string_to_file(),
					 * mce_write_number_string_to_file()
					 */
#include "mce-log.h"			/* mce_log(), LL_* */
#include "datapipe.h"			/* execute_datapipe(),
					 * datapipe_get_gint(),
					 * append_input_trigger_to_datapipe(),
					 * append_output_trigger_to_datapipe(),
					 * remove_input_trigger_from_datapipe(),
					 * remove_output_trigger_from_datapipe()
					 */
#include "mce-conf.h"			/* mce_conf_get_bool() */
#include "mce-dbus.h"			/* mce_dbus_handler_add(),
					 * mce_dbus_owner_monitor_add(),
					 * mce_dbus_owner_monitor_remove_all(),
					 * dbus_send(),
					 * dbus_send_message(),
					 * dbus_new_method_reply(),
					 * dbus_new_signal(),
					 * dbus_message_append_args(),
					 * dbus_message_get_args(),
					 * dbus_message_get_no_reply(),
					 * dbus_message_get_sender(),
					 * dbus_message_unref(),
					 * dbus_error_init(),
					 * dbus_error_free(),
					 * DBUS_MESSAGE_TYPE_METHOD_CALL,
					 * DBUS_TYPE_BOOLEAN,
					 * DBUS_TYPE_UINT32, DBUS_TYPE_INT32,
					 * DBUS_TYPE_STRING,
					 * DBUS_TYPE_INVALID
					 * DBusMessage, DBusError,
					 * dbus_bool_t,
					 * dbus_uint32_t, dbus_int32_t
					 */
#include "mce-gconf.h"			/* mce_gconf_notifier_add(),
					 * mce_gconf_get_bool(),
					 * gconf_entry_get_key(),
					 * gconf_entry_get_value(),
					 * gconf_value_get_bool(),
					 * GConfClient, GConfEntry, GConfValue
					 */

/**
 * TRUE if the touchscreen/keypad autolock is enabled,
 * FALSE if the touchscreen/keypad autolock is disabled
 */
static gboolean tk_autolock_enabled = DEFAULT_TK_AUTOLOCK;

/** GConf callback ID for the autolock entry */
static guint tk_autolock_enabled_cb_id = 0;

/** GConf callback ID for the double tap gesture */
static guint doubletap_gesture_policy_cb_id = 0;

/** Doubletap gesture proximity timeout ID */
static guint doubletap_proximity_timeout_cb_id = 0;

/** Doubletap gesture proximity timeout ID */
static guint pocket_mode_proximity_timeout_cb_id = 0;

/** Blanking timeout ID for the visual tklock */
static guint tklock_visual_blank_timeout_cb_id = 0;

/** Dimming timeout ID for the tklock */
static guint tklock_dim_timeout_cb_id = 0;

/** ID for touchscreen/keypad unlock source */
static guint tklock_unlock_timeout_cb_id = 0;

/** Powerkey repeat emulation ID */
static guint powerkey_repeat_emulation_cb_id = 0;

/** Powerkey repeats counter */
static guint powerkey_repeat_count = 0;

/** Blank immediately on tklock instead of dim/blank */
static gboolean blank_immediately = DEFAULT_BLANK_IMMEDIATELY;

/** Dim immediately on tklock instead of timeout */
static gboolean dim_immediately = DEFAULT_DIM_IMMEDIATELY;

/** Touchscreen/keypad dim timeout */
static gint dim_delay = DEFAULT_DIM_DELAY;

/** Disable touchscreen immediately on tklock instead of at blank */
static gint disable_ts_immediately = DEFAULT_TS_OFF_IMMEDIATELY;

/** Disable keypad immediately on tklock instead of at blank */
static gint disable_kp_immediately = DEFAULT_KP_OFF_IMMEDIATELY;

/** Inhibit autolock when slide is open */
static gboolean autolock_with_open_slide = DEFAULT_AUTOLOCK_SLIDE_OPEN;

/** Inhibit proximity lock when slide is open */
static gboolean proximity_lock_with_open_slide = DEFAULT_PROXIMITY_LOCK_SLIDE_OPEN;

/** Unconditionally enable lock when keyboard slide is closed */
static gboolean always_lock_on_slide_close = DEFAULT_LOCK_ON_SLIDE_CLOSE;

/** Unlock the TKLock when the lens cover is opened */
static gboolean lens_cover_unlock = DEFAULT_LENS_COVER_UNLOCK;

/** Proximity based locking when the phone is ringing */
static gboolean proximity_lock_when_ringing = DEFAULT_PROXIMITY_LOCK_WHEN_RINGING;

/** Doubletap gesture is enabled */
static gboolean doubletap_gesture_enabled = FALSE;

/** Doubletap gesture inhibited */
static gboolean doubletap_gesture_inhibited = FALSE;

/** Trigger unlock screen when volume keys are pressed */
static gboolean volkey_visual_trigger = DEFAULT_VOLKEY_VISUAL_TRIGGER;

/** SysFS path to touchscreen event disable */
static const gchar *mce_touchscreen_sysfs_disable_path = NULL;

/** SysFS path to touchscreen double-tap gesture control */
static const gchar *mce_touchscreen_gesture_control_path = NULL;

/** SysFS path to touchscreen recalibration control */
static const gchar *mce_touchscreen_calibration_control_path = NULL;

/** SysFS path to keypad event disable */
static const gchar *mce_keypad_sysfs_disable_path = NULL;

/** Touchscreen double tap gesture policy */
static gint doubletap_gesture_policy = DEFAULT_DOUBLETAP_GESTURE_POLICY;

/** Submode at the beginning of a call */
static submode_t saved_submode = MCE_INVALID_SUBMODE;

/** List of monitored SystemUI processes (should be one or zero) */
static GSList *systemui_monitor_list = NULL;

/** Double tap recalibration delays */
static const guint doubletap_recal_delays[] = {
	2, 4, 8, 16, 30
};

/** Double tap recalibration index */
static guint doubletap_recal_index = 0;

/** Double tap recalibration toumeout identifier */
static guint doubletap_recal_timeout_id = 0;

/** Do double tap recalibration on heartbeat */
static gboolean doubletap_recal_on_heartbeat = FALSE;

/** TKLock saved state type */
typedef enum {
	/** TKLock was not enabled */
	MCE_TKLOCK_UNLOCKED_STATE = 0,
	/** Visual TKLock was enabled */
	MCE_TKLOCK_VISUAL_STATE = 1,
	/** Full TKLock was enabled */
	MCE_TKLOCK_LOCKED_STATE = 2
} saved_tklock_state_t;

/** TKLock saved state */
static saved_tklock_state_t saved_tklock_state = MCE_TKLOCK_UNLOCKED_STATE;

/** TKLock UI state type */
typedef enum {
	/** TKLock UI state unknown */
	MCE_TKLOCK_UI_UNSET = -1,
	/** No TKLock UI active */
	MCE_TKLOCK_UI_NONE = 0,
	/** Normal TKLock UI active */
	MCE_TKLOCK_UI_NORMAL = 1,
	/** Event eater UI active */
	MCE_TKLOCK_UI_EVENT_EATER = 2,
	/** Slider UI active */
	MCE_TKLOCK_UI_SLIDER = 3,
	/** Low power mode UI active */
	MCE_TKLOCK_UI_LPM = 4
} tklock_ui_state_t;

/** TKLock UI state */
static tklock_ui_state_t tklock_ui_state = MCE_TKLOCK_UI_UNSET;

/** Touch screen state type */
typedef enum {
	/** Touch screen state unknown */
	MCE_TS_UNSET = -1,
	/** Touch screen disabled */
	MCE_TS_DISABLED,
	/** Touch screen enabled */
	MCE_TS_ENABLED 
} ts_state_t;

/** Touch screen state */
static ts_state_t ts_state = MCE_TS_UNSET;

/** Double tap state type */
typedef enum {
	/** Double tap state unknown */
	MCE_DT_UNSET = -1,
	/** Double tap disabled */
	MCE_DT_DISABLED,
	/** Double tap enabled */
	MCE_DT_ENABLED
} dt_state_t;

/** Double tap state */
static dt_state_t dt_state = MCE_DT_UNSET;

/* Valid triggers for autorelock */

/** No autorelock triggers */
#define AUTORELOCK_NO_TRIGGERS	0
/** Autorelock on keyboard slide closed */
#define AUTORELOCK_KBD_SLIDE	(1 << 0)
/** Autorelock on lens cover */
#define AUTORELOCK_LENS_COVER	(1 << 1)
/** Autorelock on proximity sensor */
#define AUTORELOCK_ON_PROXIMITY	(1 << 2)

/** Inhibit proximity relock type */
typedef enum {
	/** Inhibit proximity relock */
	MCE_INHIBIT_PROXIMITY_RELOCK = 0,
	/** Allow proximity relock */
	MCE_ALLOW_PROXIMITY_RELOCK = 1,
	/** Temporarily inhibit proximity relock */
	MCE_TEMP_INHIBIT_PROXIMITY_RELOCK = 2,
} inhibit_proximity_relock_t;

/** Inhibit autorelock using proximity sensor */
static inhibit_proximity_relock_t inhibit_proximity_relock = MCE_ALLOW_PROXIMITY_RELOCK;

/** Autorelock when call ends */
static gboolean autorelock_after_call_end = DEFAULT_AUTORELOCK_AFTER_CALL_END;

/** Autorelock triggers */
static gint autorelock_triggers = AUTORELOCK_NO_TRIGGERS;

static void set_doubletap_gesture(gboolean enable);
static void ts_enable(void);
static void ts_disable(void);
static void set_tklock_state(lock_state_t lock_state);
static void autorelock_touchscreen_trigger(gconstpointer const data);
static void cancel_tklock_dim_timeout(void);
static void cancel_tklock_unlock_timeout(void);

/**
 * Query the event eater status
 *
 * @return TRUE if the event eater is enabled,
 *         FALSE if the event eater is disabled
 */
static gboolean is_eveater_enabled(void) G_GNUC_PURE;
static gboolean is_eveater_enabled(void)
{
	return ((mce_get_submode_int32() & MCE_EVEATER_SUBMODE) != 0);
}

/**
 * Query the touchscreen/keypad lock status
 *
 * @return TRUE if the touchscreen/keypad lock is enabled,
 *         FALSE if the touchscreen/keypad lock is disabled
 */
static gboolean is_tklock_enabled(void) G_GNUC_PURE;
static gboolean is_tklock_enabled(void)
{
	return ((mce_get_submode_int32() & MCE_TKLOCK_SUBMODE) != 0);
}

/**
 * Query whether the device is in the MALF state
 *
 * @return TRUE if the malf state is enabled,
 *         FALSE if the malf state is disabled
 */
static gboolean is_malf_state_enabled(void) G_GNUC_PURE;
static gboolean is_malf_state_enabled(void)
{
	return ((mce_get_submode_int32() & MCE_MALF_SUBMODE) != 0);
}

/**
 * Query the visual touchscreen/keypad lock status
 *
 * @return TRUE if the visual touchscreen/keypad lock is enabled,
 *         FALSE if the visual touchscreen/keypad lock is disabled
 */
static gboolean is_visual_tklock_enabled(void) G_GNUC_PURE;
static gboolean is_visual_tklock_enabled(void)
{
	return ((mce_get_submode_int32() & MCE_VISUAL_TKLOCK_SUBMODE) != 0);
}

/**
 * Query the touchscreen/keypad lock status based on proximity
 *
 * @return TRUE if the touchscreen/keypad lock is activated by proximity,
 *         FALSE if the touchscreen/keypad lock is not activated by proximity
 */
static gboolean is_tklock_enabled_by_proximity(void) G_GNUC_PURE;
static gboolean is_tklock_enabled_by_proximity(void)
{
	return ((mce_get_submode_int32() & MCE_PROXIMITY_TKLOCK_SUBMODE) != 0);
}

/**
 * Query the autorelock status
 *
 * @return TRUE if the autorelock is enabled,
 *         FALSE if the autorelock is disabled
 */
static gboolean is_autorelock_enabled(void) G_GNUC_PURE;
static gboolean is_autorelock_enabled(void)
{
	return ((mce_get_submode_int32() & MCE_AUTORELOCK_SUBMODE) != 0);
}

/**
 * Query the pocket mode status
 *
 * @return TRUE if the pocket mode is enabled,
 *         FALSE if the pocket mode is disabled
 */
static gboolean is_pocket_mode_enabled(void) G_GNUC_PURE;
static gboolean is_pocket_mode_enabled(void)
{
	return ((mce_get_submode_int32() & MCE_POCKET_SUBMODE) != 0);
}

/**
 * Heartbeat trigger
 *
 * @param data Not used
 */
static void heartbeat_trigger(gconstpointer data)
{
	(void)data;

	if (doubletap_recal_on_heartbeat == TRUE) {
		mce_log(LL_DEBUG, "Recalibrating double tap");
		(void)mce_write_string_to_file(mce_touchscreen_calibration_control_path,
					       "1");
	}
}

/**
 * Callback for doubletap recalibration
 *
 * @param data Not used.
 * @return Always FALSE for remove event source
 */
static gboolean doubletap_recal_timeout_cb(gpointer data)
{
	(void)data;

	mce_log(LL_DEBUG, "Recalibrating double tap");
	(void)mce_write_string_to_file(mce_touchscreen_calibration_control_path,
				       "1");

	/* If at last delay, start recalibrating on DSME heartbeat */
	if (doubletap_recal_index == G_N_ELEMENTS(doubletap_recal_delays) - 1) {
		doubletap_recal_timeout_id = 0;
		doubletap_recal_on_heartbeat = TRUE;

		return FALSE;
	}

	/* Otherwise use next delay */
	doubletap_recal_index++;
	doubletap_recal_timeout_id =
	       	g_timeout_add_seconds(doubletap_recal_delays[doubletap_recal_index],
				      doubletap_recal_timeout_cb, NULL);

	return FALSE;
}

/**
 * Cancel doubletap recalibration timeouts
 */
static void cancel_doubletap_recal_timeout(void)
{
	if (doubletap_recal_timeout_id != 0)
		g_source_remove(doubletap_recal_timeout_id);
	doubletap_recal_timeout_id = 0;
	doubletap_recal_on_heartbeat = FALSE;
}

/**
 * Setup doubletap recalibration timeouts
 */
static void setup_doubletap_recal_timeout(void)
{
	if (mce_touchscreen_calibration_control_path == NULL)
		return;

	cancel_doubletap_recal_timeout();
	doubletap_recal_index = 0;
	doubletap_recal_on_heartbeat = FALSE;

	doubletap_recal_timeout_id =
		g_timeout_add_seconds(doubletap_recal_delays[doubletap_recal_index],
				      doubletap_recal_timeout_cb, NULL);

}

/**
 * Enable auto-relock
 */
static void enable_autorelock(void)
{
	cover_state_t kbd_slide_state = datapipe_get_gint(keyboard_slide_pipe);
	cover_state_t lens_cover_state = datapipe_get_gint(lens_cover_pipe);

	if (autorelock_triggers != AUTORELOCK_ON_PROXIMITY) {
		/* Reset autorelock triggers */
		autorelock_triggers = AUTORELOCK_NO_TRIGGERS;

		/* If the keyboard slide is closed, use it as a trigger */
		if (kbd_slide_state == COVER_CLOSED)
			autorelock_triggers |= AUTORELOCK_KBD_SLIDE;

		/* If the lens cover is closed, use it as a trigger */
		if (lens_cover_state == COVER_CLOSED)
			autorelock_triggers |= AUTORELOCK_LENS_COVER;
	}

	/* Only setup touchscreen monitoring once,
	 * and only if there are autorelock triggers
	 * and it's not the proximity sensor
	 */
	if ((is_autorelock_enabled() == FALSE) &&
	    (autorelock_triggers != AUTORELOCK_NO_TRIGGERS) &&
	    (autorelock_triggers != AUTORELOCK_ON_PROXIMITY)) {
		append_input_trigger_to_datapipe(&touchscreen_pipe,
						 autorelock_touchscreen_trigger);
	}

	mce_add_submode_int32(MCE_AUTORELOCK_SUBMODE);
}

/**
 * Disable auto-relock
 */
static void disable_autorelock(void)
{
	/* Touchscreen monitoring is only needed for the autorelock */
	remove_input_trigger_from_datapipe(&touchscreen_pipe,
					   autorelock_touchscreen_trigger);
	mce_rem_submode_int32(MCE_AUTORELOCK_SUBMODE);

	/* Reset autorelock triggers */
	autorelock_triggers = AUTORELOCK_NO_TRIGGERS;
}

/**
 * Disable auto-relock based on policy
 */
static void disable_autorelock_policy(void)
{
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);

	/* Don't disable autorelock if the alarm UI is visible */
	if ((alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32) ||
	    (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32))
		goto EXIT;

	/* If the tklock is enabled
	 * or proximity autorelock is active, don't disable
	 */
	if ((is_tklock_enabled() == TRUE) ||
	    (autorelock_triggers == AUTORELOCK_ON_PROXIMITY))
		goto EXIT;

	disable_autorelock();

EXIT:
	return;
}

/**
 * Cancel timeout for pocket mode
 */
static void cancel_pocket_mode_timeout(void)
{
	if (pocket_mode_proximity_timeout_cb_id != 0) {
		g_source_remove(pocket_mode_proximity_timeout_cb_id);
		pocket_mode_proximity_timeout_cb_id = 0;
	}
}

/**
 * Timeout callback for doubletap gesture proximity
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean doubletap_proximity_timeout_cb(gpointer data)
{
	call_state_t call_state = datapipe_get_gint(call_state_pipe);
	audio_route_t audio_route = datapipe_get_gint(audio_route_pipe);

	(void)data;

	if ((audio_route == AUDIO_ROUTE_HANDSET) &&
	    ((call_state == CALL_STATE_RINGING) ||
	     (call_state == CALL_STATE_ACTIVE))) {
		cancel_pocket_mode_timeout();
		mce_add_submode_int32(MCE_POCKET_SUBMODE);
		mce_add_submode_int32(MCE_PROXIMITY_TKLOCK_SUBMODE);
	}

	doubletap_proximity_timeout_cb_id = 0;

	/* First disable touchscreen interrupts, then disable gesture */
	ts_disable();
	set_doubletap_gesture(FALSE);
	doubletap_gesture_inhibited = TRUE;

	return FALSE;
}

/**
 * Timeout callback for pocket mode
 *
 * @param data Unused
 * @return Always returns FALSE to disable the timeout
 */
static gboolean pocket_mode_timeout_cb(gpointer data)
{
	(void)data;

	pocket_mode_proximity_timeout_cb_id = 0;

	mce_add_submode_int32(MCE_POCKET_SUBMODE);

	return FALSE;
}

/**
 * Setup a timeout for pocket mode
 */
static void setup_pocket_mode_timeout(void)
{
	if (pocket_mode_proximity_timeout_cb_id != 0)
		return;

	pocket_mode_proximity_timeout_cb_id =
		g_timeout_add_seconds(DEFAULT_POCKET_MODE_PROXIMITY_TIMEOUT,
				      pocket_mode_timeout_cb, NULL);
}

/**
 * Cancel timeout for doubletap gesture proximity
 */
static void cancel_doubletap_proximity_timeout(void)
{
	/* Remove the timer source for doubletap gesture proximity */
	if (doubletap_proximity_timeout_cb_id != 0) {
		g_source_remove(doubletap_proximity_timeout_cb_id);
		doubletap_proximity_timeout_cb_id = 0;
	}
}

/**
 * Setup a timeout for doubletap gesture proximity
 */
static void setup_doubletap_proximity_timeout(void)
{
	gint timeout = DEFAULT_DOUBLETAP_PROXIMITY_TIMEOUT;
	call_state_t call_state = datapipe_get_gint(call_state_pipe);
	audio_route_t audio_route = datapipe_get_gint(audio_route_pipe);
	cancel_doubletap_proximity_timeout();

	if (doubletap_gesture_enabled == FALSE)
		goto EXIT;

	/* Setup new timeout */
	if ((audio_route == AUDIO_ROUTE_HANDSET) &&
	    ((call_state == CALL_STATE_RINGING) ||
	     (call_state == CALL_STATE_ACTIVE)))
		timeout = 0;

	doubletap_proximity_timeout_cb_id =
		g_timeout_add_seconds(timeout,
				      doubletap_proximity_timeout_cb, NULL);

EXIT:
	return;
}

/**
 * Enable/disable double tap gesture control
 *
 * @param enable TRUE enable gesture recognition,
 *               FALSE disable gesture recognition
 */
static void set_doubletap_gesture(gboolean enable)
{
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	call_state_t call_state = datapipe_get_gint(call_state_pipe);
	cover_state_t proximity_sensor_state =
			datapipe_get_gint(proximity_sensor_pipe);

	if (mce_touchscreen_gesture_control_path == NULL)
		goto EXIT;

	/* If the double-tap gesture policy is 0,
	 * then we should just disable touchscreen interrupts instead.
	 * Likewise if there's a call or an alarm, and the proximity sensor
	 * is covered
	 */
	if ((enable == TRUE) &&
	    ((doubletap_gesture_policy == 0) ||
	     (doubletap_gesture_inhibited == TRUE) ||
	     ((proximity_sensor_state == COVER_CLOSED) &&
	      ((call_state != CALL_STATE_NONE) ||
	       (alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32) ||
	       (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32))))) {

		cancel_doubletap_proximity_timeout();

		doubletap_gesture_enabled = FALSE;
		ts_disable();

		goto EXIT;
	}

	doubletap_gesture_enabled = enable;

	/* Adjust the touchscreen idle frequency */
	if (enable == TRUE) {
		mce_rem_submode_int32(MCE_POCKET_SUBMODE);
		cancel_doubletap_proximity_timeout();
		cancel_pocket_mode_timeout();

		if (proximity_sensor_state == COVER_CLOSED) {
			setup_doubletap_proximity_timeout();
			setup_pocket_mode_timeout();
		}
	} else {
		cancel_doubletap_proximity_timeout();
	}

	if (enable && dt_state != MCE_DT_ENABLED) {
		(void)mce_write_string_to_file(mce_touchscreen_gesture_control_path, "4");
		setup_doubletap_recal_timeout();
		dt_state = MCE_DT_ENABLED;
	} else if (!enable && dt_state != MCE_DT_DISABLED) {
		(void)mce_write_string_to_file(mce_touchscreen_gesture_control_path, "0");
		cancel_doubletap_recal_timeout();
		/* Disabling the double tap gesture causes recalibration */
		if (ts_state == MCE_TS_ENABLED) {
			g_usleep(MCE_TOUCHSCREEN_CALIBRATION_DELAY);
		}
		dt_state = MCE_DT_DISABLED;
	}

	/* Finally, ensure that touchscreen interrupts are enabled
	 * if doubletap gestures are enabled
	 */
	if (enable == TRUE) {
		ts_enable();
	}
EXIT:
	return;
}

/**
 * Enable/disable touchscreen/keypad events
 *
 * @note Since nothing sensible can be done on error except reporting it,
 *       we don't return the status
 * @param file Path to enable/disable file
 * @param enable TRUE enable events, FALSE disable events
 */
static void generic_event_control(const gchar *const file,
				  const gboolean enable)
{
	if (file == NULL)
		goto EXIT;

	if (mce_write_number_string_to_file(file, !enable ? 1 : 0,
					    NULL, TRUE, TRUE) == FALSE) {
		mce_log(LL_ERR,
			"%s: Event status *not* modified",
			file);
		goto EXIT;
	}

	mce_log(LL_DEBUG,
		"%s: events %s\n",
		file, enable ? "enabled" : "disabled");

EXIT:
	return;
}

/**
 * Enable touchscreen interrupts (events will be generated by kernel)
 */
static void ts_enable(void)
{
	if (ts_state != MCE_TS_ENABLED) {
		generic_event_control(mce_touchscreen_sysfs_disable_path,
				      TRUE);
		g_usleep(MCE_TOUCHSCREEN_CALIBRATION_DELAY);
		ts_state = MCE_TS_ENABLED;
	}
}

/**
 * Disable touchscreen interrupts (no events will be generated by kernel)
 */
static void ts_disable(void)
{
	if (ts_state != MCE_TS_DISABLED) {
		generic_event_control(mce_touchscreen_sysfs_disable_path,
				      FALSE);
		ts_state = MCE_TS_DISABLED;
	}
}

/**
 * Enable keypress interrupts (events will be generated by kernel)
 */
static void kp_enable(void)
{
	generic_event_control(mce_keypad_sysfs_disable_path, TRUE);
}

/**
 * Disable keypress interrupts (no events will be generated by kernel)
 */
static void kp_disable(void)
{
	generic_event_control(mce_keypad_sysfs_disable_path, FALSE);
}

/**
 * Policy based enabling of touchscreen and keypad
 */
static void ts_kp_enable_policy(void)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	cover_state_t lid_cover_state = datapipe_get_gint(lid_cover_pipe);
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);

	/* If the cover is closed, don't bother */
	if (lid_cover_state == COVER_CLOSED)
		goto EXIT;

	if ((system_state == MCE_STATE_USER) ||
	    (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32) ||
	    (alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32)) {
		set_doubletap_gesture(FALSE);
		ts_enable();
		kp_enable();
	}

EXIT:
	return;
}

/**
 * Policy based disabling of touchscreen and keypad
 */
static void ts_kp_disable_policy(void)
{
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	submode_t submode = mce_get_submode_int32();
	call_state_t call_state = datapipe_get_gint(call_state_pipe);

	/* If we're in softoff submode, always disable */
	if ((submode & MCE_SOFTOFF_SUBMODE) != 0) {
		ts_disable();
		kp_disable();
		goto EXIT;
	}

	/* If the Alarm UI is visible, don't disable,
	 * unless the tklock UI is active
	 */
	if (((alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32) ||
	     (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32)) &&
	    (tklock_ui_state != MCE_TKLOCK_UI_NORMAL)) {
		mce_log(LL_DEBUG,
			"Alarm UI visible; refusing to disable touchscreen "
			"and keypad events");
		goto EXIT;
	}

	if ((system_state != MCE_STATE_USER) || 
	    (is_malf_state_enabled() == TRUE)){
		ts_disable();
		kp_disable();
	} else if (((display_state == MCE_DISPLAY_OFF) ||
	            (display_state == MCE_DISPLAY_LPM_OFF) ||
	            (display_state == MCE_DISPLAY_LPM_ON)) &&
		   (is_tklock_enabled() == TRUE)) {
		/* Display is off -- we only need to check for
		 * disable_{ts,kp}_immediately == 2
		 */
		if (disable_kp_immediately == 2) {
			if (disable_ts_immediately == 2) {
				set_doubletap_gesture(TRUE);
			} else {
				ts_disable();
			}
		} else {
			/*  Don't disable kp during call (volume must work) */
			if (call_state != CALL_STATE_NONE) {
				if (disable_ts_immediately == 2) {
					set_doubletap_gesture(TRUE);
				} else {
					ts_disable();
				}
			} else {
				if (disable_ts_immediately == 2) {
					set_doubletap_gesture(TRUE);
				} else {
					ts_disable();
				}

				kp_disable();
			}
		}
	} else if (is_tklock_enabled() == TRUE) {
		/*  Don't disable kp during call (volume keys must work) */
		if (call_state != CALL_STATE_NONE) {
			if (disable_ts_immediately == 2) {
				set_doubletap_gesture(TRUE);
			} else if (disable_ts_immediately == 1) {
				ts_disable();
			}
		} else if (disable_kp_immediately == 1) {
			if (disable_ts_immediately == 2) {
				set_doubletap_gesture(TRUE);
			} else if (disable_ts_immediately == 1) {
				ts_disable();
			}

			kp_disable();
		} else {
			if (disable_ts_immediately == 2) {
				set_doubletap_gesture(TRUE);
			} else if (disable_ts_immediately == 1) {
				ts_disable();
			}
		}
	}

EXIT:
	return;
}

/**
 * Synthesise activity, since activity is filtered when tklock is active;
 * also, the lock key doesn't normally generate activity
 */
static void synthesise_activity(void)
{
	(void)execute_datapipe(&device_inactive_pipe,
			       GINT_TO_POINTER(FALSE),
			       USE_INDATA, CACHE_INDATA);
}

/**
 * Synthesise inactivity, since we want immediate inactivity
 * when the tklock is activated
 */
static void synthesise_inactivity(void)
{
	(void)execute_datapipe(&device_inactive_pipe,
			       GINT_TO_POINTER(TRUE),
			       USE_INDATA, CACHE_INDATA);
}

/**
 * Send the touchscreen/keypad lock mode
 *
 * @param method_call A DBusMessage to reply to;
 *                    pass NULL to send a tklock mode signal instead
 * @return TRUE on success, FALSE on failure
 */
static gboolean send_tklock_mode(DBusMessage *const method_call)
{
	DBusMessage *msg = NULL;
	const gchar *modestring;
	gboolean status = FALSE;

	if (is_tklock_enabled() == TRUE)
		modestring = MCE_TK_LOCKED;
	else
		modestring = MCE_TK_UNLOCKED;

	/* If method_call is set, send a reply,
	 * otherwise, send a signal
	 */
	if (method_call != NULL) {
		msg = dbus_new_method_reply(method_call);
	} else {
		/* tklock_mode_ind */
		msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
				      MCE_TKLOCK_MODE_SIG);
	}

	/* Append the new mode */
	if (dbus_message_append_args(msg,
				     DBUS_TYPE_STRING, &modestring,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append %sargument to D-Bus message "
			"for %s.%s",
			method_call ? "reply " : "",
			method_call ? MCE_REQUEST_IF :
				      MCE_SIGNAL_IF,
			method_call ? MCE_TKLOCK_MODE_GET :
				      MCE_TKLOCK_MODE_SIG);
		dbus_message_unref(msg);
		goto EXIT;
	}

	/* Send the message */
	status = dbus_send_message(msg);

EXIT:
	return status;
}

/**
 * D-Bus callback used for monitoring SystemUI; if it disappears,
 * disable the tklock for reliability reasons *if* the tklock was
 * active
 */
static gboolean systemui_owner_monitor_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;
	const gchar *old_name;
	const gchar *new_name;
	const gchar *service;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	/* Extract result */
	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &service,
				  DBUS_TYPE_STRING, &old_name,
				  DBUS_TYPE_STRING, &new_name,
				  DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_ERR,
			"Failed to get argument from %s.%s; %s",
			"org.freedesktop.DBus", "NameOwnerChanged",
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	/* Stop monitoring the old (non-existing) SystemUI process */
	mce_dbus_owner_monitor_remove_all(&systemui_monitor_list);

	if (is_tklock_enabled() == TRUE)
		set_tklock_state(LOCK_OFF_DELAYED);

	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus reply handler for touchscreen/keypad lock UI
 *
 * @param pending_call The DBusPendingCall
 * @param data Unused
 */
static void tklock_reply_dbus_cb(DBusPendingCall *pending_call,
				 void *data)
{
	const gchar *sender;
	DBusMessage *reply;
	dbus_int32_t retval;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	(void)data;

	mce_log(LL_DEBUG, "Received TKLock UI reply");

	if ((reply = dbus_pending_call_steal_reply(pending_call)) == NULL) {
		mce_log(LL_ERR,
			"TKLock UI reply callback invoked, "
			"but no pending call available");
		goto EXIT;
	}

	/* Make sure we didn't get an error message */
	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		char *error_msg;

		/* If we got an error, it's a string */
		if (dbus_message_get_args(reply, &error,
					  DBUS_TYPE_STRING, &error_msg,
					  DBUS_TYPE_INVALID) == FALSE) {
			mce_log(LL_CRIT,
				"Failed to get error reply argument "
				"from %s.%s: %s",
				SYSTEMUI_REQUEST_IF, SYSTEMUI_TKLOCK_OPEN_REQ,
				error.message);
			dbus_error_free(&error);
		} else {
			mce_log(LL_ERR,
				"D-Bus call to %s.%s failed: %s",
				SYSTEMUI_REQUEST_IF, SYSTEMUI_TKLOCK_OPEN_REQ,
				error_msg);

			/* If the call failed, disable tklock */
			set_tklock_state(LOCK_OFF_DELAYED);
		}

		goto EXIT2;
	}

	/* Setup a D-Bus owner monitor for SystemUI */
	sender = dbus_message_get_sender(reply);

	if (mce_dbus_owner_monitor_add(sender,
				       systemui_owner_monitor_dbus_cb,
				       &systemui_monitor_list, 1) == -1) {
		mce_log(LL_INFO,
			"Failed to add name owner monitoring for `%s'",
			sender);
	}

	/* Extract reply */
	if (dbus_message_get_args(reply, &error,
				  DBUS_TYPE_INT32, &retval,
				  DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to get reply argument from %s.%s: %s",
			SYSTEMUI_REQUEST_IF, SYSTEMUI_TKLOCK_OPEN_REQ,
			error.message);
		dbus_error_free(&error);
		goto EXIT2;
	}

	mce_log(LL_DEBUG,
		"Return value: %d",
		retval);

EXIT2:
	dbus_message_unref(reply);

EXIT:
	dbus_pending_call_unref(pending_call);

	return;
}

/**
 * Show the touchscreen/keypad lock UI
 *
 * @param mode The mode to open in; valid modes:
 *             TKLOCK_ONEINPUT (open the tklock in event eater mode)
 *             TKLOCK_ENABLE_VISUAL (show the gesture unlock interface)
 *             TKLOCK_ENABLE_LPM_UI (show the low power mode UI)
 *             TKLOCK_PAUSE_UI (stop animations; display is off)
 * @return TRUE on success, FALSE on FAILURE
 */
static gboolean open_tklock_ui(dbus_uint32_t mode)
{
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
	const gchar *const cb_service = MCE_SERVICE;
	const gchar *const cb_path = MCE_REQUEST_PATH;
	const gchar *const cb_interface = MCE_REQUEST_IF;
	const gchar *const cb_method = MCE_TKLOCK_CB_REQ;
	dbus_bool_t flicker_key = has_flicker_key;
	tklock_ui_state_t new_tklock_ui_state;
	gboolean silent = TRUE;
	gboolean status = FALSE;

	switch (mode) {
	case TKLOCK_ONEINPUT:
		new_tklock_ui_state = MCE_TKLOCK_UI_EVENT_EATER;
		break;

	case TKLOCK_ENABLE_VISUAL:
		new_tklock_ui_state = MCE_TKLOCK_UI_SLIDER;
		break;

	case TKLOCK_ENABLE_LPM_UI:
		if ((display_state == MCE_DISPLAY_LPM_ON) ||
		    (display_state == MCE_DISPLAY_LPM_OFF)) {
			new_tklock_ui_state = MCE_TKLOCK_UI_LPM;
		} else {
			/* Fallback in case LPM is disabled or not supported */
			new_tklock_ui_state = MCE_TKLOCK_UI_SLIDER;
			mode = TKLOCK_ENABLE_VISUAL;
		}

		break;

	case TKLOCK_PAUSE_UI:
		/* To avoid special cases */
		if (display_state == MCE_DISPLAY_LPM_OFF) {
			new_tklock_ui_state = tklock_ui_state;
		} else {
			/* Fallback in case LPM is disabled or not supported */
			new_tklock_ui_state = MCE_TKLOCK_UI_SLIDER;
			mode = TKLOCK_ENABLE_VISUAL;
		}

		break;

	default:
		mce_log(LL_ERR, "Invalid TKLock UI mode requested");
		goto EXIT;
	}

	/* com.nokia.system_ui.request.tklock_open */
	status = dbus_send(SYSTEMUI_SERVICE, SYSTEMUI_REQUEST_PATH,
			   SYSTEMUI_REQUEST_IF, SYSTEMUI_TKLOCK_OPEN_REQ,
			   tklock_reply_dbus_cb,
			   DBUS_TYPE_STRING, &cb_service,
			   DBUS_TYPE_STRING, &cb_path,
			   DBUS_TYPE_STRING, &cb_interface,
			   DBUS_TYPE_STRING, &cb_method,
			   DBUS_TYPE_UINT32, &mode,
			   DBUS_TYPE_BOOLEAN, &silent,
			   DBUS_TYPE_BOOLEAN, &flicker_key,
			   DBUS_TYPE_INVALID);

	if (status == FALSE) {
		mce_log(LL_ERR,
			"Failed to open tklock UI (mode: %d)", mode);
		goto EXIT;
	}

	/* We managed to open the new UI; update accordingly */
	tklock_ui_state = new_tklock_ui_state;

EXIT:
	return status;
}

/**
 * Hide the touchscreen/keypad lock UI
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean close_tklock_ui(void)
{
	gboolean silent = TRUE;
	gboolean status = FALSE;

	/* com.nokia.system_ui.request.tklock_close */
	status = dbus_send(SYSTEMUI_SERVICE, SYSTEMUI_REQUEST_PATH,
			   SYSTEMUI_REQUEST_IF, SYSTEMUI_TKLOCK_CLOSE_REQ,
			   NULL,
			   DBUS_TYPE_BOOLEAN, &silent,
			   DBUS_TYPE_INVALID);

	/* Stop monitoring the SystemUI process; there's nothing
	 * sensible we can do if there's a failure, so remove the
	 * monitor even if closing the tklock UI failed
	 */
	mce_dbus_owner_monitor_remove_all(&systemui_monitor_list);

	/* If the tklock UI isn't on record to be open,
	 * we treat the close operation as a success even if it failed
	 */
	if (tklock_ui_state == MCE_TKLOCK_UI_NONE)
		goto EXIT;

	if (status == FALSE) {
		mce_log(LL_ERR,
			"Failed to close tklock UI");
		goto EXIT;
	}

	/* TKLock UI closed */
	tklock_ui_state = MCE_TKLOCK_UI_NONE;

EXIT:
	return status;
}

/**
 * Enable the touchscreen/keypad lock without UI
 *
 * @note Calling enable_tklock_raw() when the UI is already on-screen
 *       will NOT close the UI
 *
 * @return TRUE on success, FALSE on failure
 */
static void enable_tklock_raw(void)
{
	mce_add_submode_int32(MCE_TKLOCK_SUBMODE);
	mce_rem_submode_int32(MCE_EVEATER_SUBMODE);
	mce_rem_submode_int32(MCE_VISUAL_TKLOCK_SUBMODE);
	(void)send_tklock_mode(NULL);

	/* Enable automagic relock */
	enable_autorelock();
}

/**
 * Enable the touchscreen/keypad lock or low power mode ui
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean enable_tklock(void)
{
	gboolean status = FALSE;

	if ((is_malf_state_enabled() == FALSE) &&
	    (open_tklock_ui(TKLOCK_ENABLE_LPM_UI) == FALSE))
		goto EXIT;

	enable_tklock_raw();

	if (is_malf_state_enabled() == FALSE) {
		mce_add_submode_int32(MCE_VISUAL_TKLOCK_SUBMODE);
	}

	if (saved_tklock_state == MCE_TKLOCK_VISUAL_STATE)
		saved_tklock_state = MCE_TKLOCK_LOCKED_STATE;

	status = TRUE;

EXIT:
	return status;
}

/**
 * Cancel timeout for visual touchscreen/keypad lock blanking
 */
static void cancel_tklock_visual_blank_timeout(void)
{
	/* Remove the timer source for visual tklock blanking */
	if (tklock_visual_blank_timeout_cb_id != 0) {
		g_source_remove(tklock_visual_blank_timeout_cb_id);
		tklock_visual_blank_timeout_cb_id = 0;
	}
}

/**
 * Timeout callback for visual touchscreen/keypad lock blanking
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean tklock_visual_blank_timeout_cb(gpointer data)
{
	(void)data;

	cancel_tklock_visual_blank_timeout();

	if (saved_tklock_state == MCE_TKLOCK_VISUAL_STATE)
		saved_tklock_state = MCE_TKLOCK_LOCKED_STATE;

	if (is_tklock_enabled_by_proximity() == FALSE)
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_LPM_ON),
				       USE_INDATA, CACHE_INDATA);

	return FALSE;
}

/**
 * Setup the timeout for touchscreen/keypad lock blanking
 */
static void setup_tklock_visual_blank_timeout(void)
{
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	call_state_t call_state = datapipe_get_gint(call_state_pipe);

	cancel_tklock_dim_timeout();
	cancel_tklock_visual_blank_timeout();

	/* Do not setup the timeout if the
	 * call state or alarm state is ringing
	 */
	if ((call_state == CALL_STATE_RINGING) ||
	    (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32))
		goto EXIT;

	/* Setup blank timeout */
	tklock_visual_blank_timeout_cb_id =
		g_timeout_add_seconds(DEFAULT_VISUAL_BLANK_DELAY, tklock_visual_blank_timeout_cb, NULL);

EXIT:
	return;
}

/**
 * Timeout callback for touchscreen/keypad lock dim
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean tklock_dim_timeout_cb(gpointer data)
{
	(void)data;

	tklock_dim_timeout_cb_id = 0;

	if (blank_immediately == TRUE) {
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_LPM_ON),
				       USE_INDATA, CACHE_INDATA);
		if (saved_tklock_state == MCE_TKLOCK_VISUAL_STATE)
			saved_tklock_state = MCE_TKLOCK_LOCKED_STATE;
	} else {
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_DIM),
				       USE_INDATA, CACHE_INDATA);
	}

	return FALSE;
}

/**
 * Cancel timeout for tklock dimming
 */
static void cancel_tklock_dim_timeout(void)
{
	/* Remove the timer source for tklock dimming */
	if (tklock_dim_timeout_cb_id != 0) {
		g_source_remove(tklock_dim_timeout_cb_id);
		tklock_dim_timeout_cb_id = 0;
	}
}

/**
 * Setup a timeout for tklock dimming
 */
static void setup_tklock_dim_timeout(void)
{
	cancel_tklock_dim_timeout();

	/* Setup new timeout */
	tklock_dim_timeout_cb_id =
		g_timeout_add_seconds(dim_delay, tklock_dim_timeout_cb, NULL);
}

/**
 * Helper function to setup dim/blank timeouts according to policies
 *
 * @param force Force immediate dimming/blanking;
 *                    MCE_DISPLAY_OFF -- force immediate display off
 *                                       (or, if supported, low power)
 *                    MCE_DISPLAY_DIM -- force immediate display dim
 *                    MCE_DISPLAY_ON -- N/A
 *                    MCE_DISPLAY_UNDEF -- keep current display state
 */
static void setup_dim_blank_timeout_policy(display_state_t force)
{
	display_state_t display_state = datapipe_get_gint(display_state_pipe);

	cancel_tklock_visual_blank_timeout();
	cancel_tklock_unlock_timeout();
	cancel_tklock_dim_timeout();

	/* If the display is already blank, don't bother */
	if ((display_state == MCE_DISPLAY_OFF) ||
	    (display_state == MCE_DISPLAY_LPM_OFF) ||
	    (display_state == MCE_DISPLAY_LPM_ON))
		goto EXIT;

	/* If we're forcing blank,
	 * or if the display is already dimmed and we blank immediately,
	 * or if the we dim and blank immediately, then blank
	 *
	 * If we dim immediately, dim the screen (blank timeout takes care
	 * of the rest) else use the dim timeout
	 */
	if ((force == MCE_DISPLAY_OFF) ||
	    (((display_state == MCE_DISPLAY_DIM) ||
	      (dim_immediately == TRUE)) &&
	     (blank_immediately == TRUE))) {
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_LPM_ON),
				       USE_INDATA, CACHE_INDATA);
	} else if ((force == MCE_DISPLAY_DIM) || (dim_immediately == TRUE)) {
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_DIM),
				       USE_INDATA, CACHE_INDATA);
	} else {
		setup_tklock_dim_timeout();
	}

EXIT:
	return;
}

/**
 * Enable the touchscreen/keypad lock with policy
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean enable_tklock_policy(void)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	gboolean status = FALSE;

	/* If we're in any other state than USER, don't enable tklock */
	if (system_state != MCE_STATE_USER) {
		status = TRUE;
		goto EXIT;
	}

	/* Enable lock */
	if (enable_tklock() == FALSE)
		goto EXIT;

	setup_dim_blank_timeout_policy(MCE_DISPLAY_OFF);

	/* Disable touchscreen and keypad */
	ts_kp_disable_policy();

	status = TRUE;

EXIT:
	return status;
}

/**
 * Disable the touchscreen/keypad lock
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean disable_tklock(void)
{
	gboolean status = FALSE;

	/* Only disable the UI if the active UI is the tklock */
	if ((tklock_ui_state == MCE_TKLOCK_UI_NORMAL) || 
	    (tklock_ui_state == MCE_TKLOCK_UI_LPM) ||
	    (tklock_ui_state == MCE_TKLOCK_UI_SLIDER)) {
		if (close_tklock_ui() == FALSE)
			goto EXIT;
	}

	/* Disable timeouts, just to be sure */
	cancel_tklock_visual_blank_timeout();
	cancel_tklock_unlock_timeout();
	cancel_tklock_dim_timeout();

	mce_rem_submode_int32(MCE_VISUAL_TKLOCK_SUBMODE);
	mce_rem_submode_int32(MCE_TKLOCK_SUBMODE);
	(void)send_tklock_mode(NULL);
	set_doubletap_gesture(FALSE);
	ts_enable();
	kp_enable();
	status = TRUE;

EXIT:
	return status;
}

/**
 * Enable the touchscreen/keypad single event eater
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean enable_eveater(void)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	gboolean status = TRUE;

	/* If we're in acting dead and no alarm is visible,
	 * don't activate the event eater
	 */
	if (((system_state == MCE_STATE_ACTDEAD) &&
	     ((alarm_ui_state != MCE_ALARM_UI_VISIBLE_INT32) ||
	      (alarm_ui_state != MCE_ALARM_UI_RINGING_INT32))) ||
	    (is_malf_state_enabled() == TRUE))
		goto EXIT;

	/* If we're already showing a tklock UI, exit */
	if ((tklock_ui_state != MCE_TKLOCK_UI_NONE) &&
	    (tklock_ui_state != MCE_TKLOCK_UI_UNSET))
		goto EXIT;

	if ((status = open_tklock_ui(TKLOCK_ONEINPUT)) == TRUE)
		mce_add_submode_int32(MCE_EVEATER_SUBMODE);

EXIT:
	return status;
}

/**
 * Disable the touchscreen/keypad single event eater
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean disable_eveater(void)
{
	gboolean status = FALSE;

	/* If the event eater isn't enabled, ignore the request */
	if (is_eveater_enabled() == FALSE) {
		status = TRUE;
		goto EXIT;
	}

	/* Only disable the UI if the active UI is the event eater */
	if (tklock_ui_state == MCE_TKLOCK_UI_EVENT_EATER) {
		if (close_tklock_ui() == FALSE)
			goto EXIT;
	}

	mce_rem_submode_int32(MCE_EVEATER_SUBMODE);
	status = TRUE;

EXIT:
	return status;
}

/**
 * Timeout callback for tklock unlock
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean tklock_unlock_timeout_cb(gpointer data)
{
	(void)data;

	tklock_unlock_timeout_cb_id = 0;

	set_tklock_state(LOCK_OFF);

	return FALSE;
}

/**
 * Cancel timeout for delayed unlocking of touchscreen/keypad lock
 */
static void cancel_tklock_unlock_timeout(void)
{
	/* Remove the timer source for delayed tklock unlocking */
	if (tklock_unlock_timeout_cb_id != 0) {
		g_source_remove(tklock_unlock_timeout_cb_id);
		tklock_unlock_timeout_cb_id = 0;
	}
}

/**
 * Setup a timeout for delayed unlocking of touchscreen/keypad lock
 */
static void setup_tklock_unlock_timeout(void)
{
	cancel_tklock_unlock_timeout();

	/* Setup new timeout */
	tklock_unlock_timeout_cb_id =
		g_timeout_add(MCE_TKLOCK_UNLOCK_DELAY,
			      tklock_unlock_timeout_cb, NULL);
}

/**
 * Timeout callback for emulated powerkey repeat
 *
 * @param data Unused
 * @return TRUE until the repeat limit has been reached, after that
 *         FALSE to cancel the timeout
 */
static gboolean powerkey_repeat_emulation_cb(gpointer data)
{
	(void)data;

	if (powerkey_repeat_count < DEFAULT_POWERKEY_REPEAT_LIMIT) {
		powerkey_repeat_count++;
		synthesise_activity();
		return TRUE;
	}

	return FALSE;
}

/**
 * Cancel timeout for emulated powerkey repeat
 */
static void cancel_powerkey_repeat_emulation_timeout(void)
{
	/* Remove the timer source for powerkey pressed emulation */
	if (powerkey_repeat_emulation_cb_id != 0) {
		g_source_remove(powerkey_repeat_emulation_cb_id);
		powerkey_repeat_emulation_cb_id = 0;
	}
}

/**
 * Setup the timeout for powerkey repeat emulation
 */
static void setup_powerkey_repeat_emulation_timeout(void)
{
    cancel_powerkey_repeat_emulation_timeout();
    powerkey_repeat_count = 0;

    /* Setup powerkey repeat emulation timeout */
    powerkey_repeat_emulation_cb_id =
	g_timeout_add_seconds(DEFAULT_POWERKEY_REPEAT_DELAY, powerkey_repeat_emulation_cb, NULL);
}

/**
 * Enable the touchscreen/keypad autolock
 *
 * Will enable touchscreen/keypad lock if tk_autolock_enabled is TRUE,
 * and enable the touchscreen/keypad single event eater if FALSE
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean enable_autokeylock(void)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	cover_state_t slide_state = datapipe_get_gint(keyboard_slide_pipe);
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	call_state_t call_state = datapipe_get_gint(call_state_pipe);
	submode_t submode = datapipe_get_gint(submode_pipe);
	gboolean status = TRUE;

	/* Don't enable automatic tklock during bootup, except when in MALF
	 * state
	 */
	if (((submode & MCE_BOOTUP_SUBMODE) != 0) &&
	    (is_malf_state_enabled() == FALSE))
		goto EXIT;

	if ((system_state == MCE_STATE_USER) &&
	    ((slide_state != COVER_OPEN) ||
	     (autolock_with_open_slide == TRUE)) &&
	    (tk_autolock_enabled == TRUE) &&
	    (alarm_ui_state != MCE_ALARM_UI_VISIBLE_INT32) &&
	    (alarm_ui_state != MCE_ALARM_UI_RINGING_INT32) &&
	    ((call_state == CALL_STATE_INVALID) ||
	     (call_state == CALL_STATE_NONE))) {
		if ((status = enable_tklock()) == TRUE)
			ts_kp_disable_policy();
	} else {
		if (((alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32) ||
		     (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32)) &&
		    ((tklock_ui_state == MCE_TKLOCK_UI_NONE) ||
		     (tklock_ui_state == MCE_TKLOCK_UI_EVENT_EATER)))
			disable_autorelock();

		status = enable_eveater();
	}

EXIT:
	return status;
}

/**
 * State machine for lock change requests
 *
 * @param lock_state The requested touchscreen/keypad lock state
 */
static void set_tklock_state(lock_state_t lock_state)
{
	submode_t submode = mce_get_submode_int32();
	call_state_t call_state = datapipe_get_gint(call_state_pipe);

	/* Ignore requests to enable tklock during bootup */
	switch (lock_state) {
	case LOCK_TOGGLE:
		if (is_tklock_enabled() == TRUE)
			break;

	case LOCK_ON:
	case LOCK_ON_DIMMED:
	case LOCK_ON_PROXIMITY:
		if (((submode & MCE_BOOTUP_SUBMODE) != 0) &&
		    (is_malf_state_enabled() == FALSE) &&
		    ((lock_state != LOCK_ON_PROXIMITY) ||
		     ((call_state != CALL_STATE_RINGING) &&
		      (call_state != CALL_STATE_ACTIVE))))
			goto EXIT;
	default:
		break;
	}

	switch (lock_state) {
	case LOCK_OFF:
		saved_tklock_state = MCE_TKLOCK_UNLOCKED_STATE;
		if (is_tklock_enabled_by_proximity() || is_pocket_mode_enabled())
			goto EXIT;

		/* Allow proximity relock if call ringing or active */
		if (call_state == CALL_STATE_RINGING ||
		    call_state == CALL_STATE_ACTIVE)
			inhibit_proximity_relock = MCE_ALLOW_PROXIMITY_RELOCK;

		(void)disable_tklock();
		(void)disable_eveater();
		disable_autorelock();
		synthesise_activity();
		break;

	case LOCK_OFF_DELAYED:
		setup_tklock_unlock_timeout();
		break;

	case LOCK_OFF_PROXIMITY:
		(void)disable_tklock();
		(void)disable_eveater();
		synthesise_activity();
		break;

	case LOCK_ON:
		synthesise_inactivity();

		if (enable_tklock() == TRUE)
			setup_dim_blank_timeout_policy(MCE_DISPLAY_UNDEF);
		saved_tklock_state = MCE_TKLOCK_LOCKED_STATE;
		break;

	case LOCK_ON_DIMMED:
		synthesise_inactivity();

		if (enable_tklock() == TRUE)
			setup_dim_blank_timeout_policy(MCE_DISPLAY_DIM);
		saved_tklock_state = MCE_TKLOCK_LOCKED_STATE;
		break;

	case LOCK_ON_PROXIMITY:
		synthesise_inactivity();
		enable_tklock_raw();
		setup_dim_blank_timeout_policy(MCE_DISPLAY_UNDEF);

		if (saved_tklock_state == MCE_TKLOCK_VISUAL_STATE)
			setup_tklock_visual_blank_timeout();
		break;

	case LOCK_TOGGLE:
		/* Touchscreen/keypad lock */
		if ((is_tklock_enabled() == FALSE) ||
		    ((is_tklock_enabled() == TRUE) &&
		     (tklock_ui_state == MCE_TKLOCK_UI_NONE))) {
			synthesise_inactivity();

			/* XXX: Should this be a duplicate of LOCK_ON? */
			(void)enable_tklock_policy();
		} else {
			/* Exact duplicate of LOCK_OFF */
			(void)disable_tklock();
			(void)disable_eveater();
			disable_autorelock();
			synthesise_activity();
		}

		break;

	default:
		break;
	}

EXIT:
	return;
}

/**
 * Visual touchscreen/keypad lock logic
 *
 * @param powerkey TRUE if the visual tklock was triggered by the powerkey
 *                 FALSE if not
 */
static void trigger_visual_tklock(gboolean powerkey)
{
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
	submode_t submode = mce_get_submode_int32();

	if ((is_malf_state_enabled() == TRUE) || 
	    (is_tklock_enabled() == FALSE) ||
	    (is_autorelock_enabled() == FALSE) ||
	    (system_state != MCE_STATE_USER) ||
	    (alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32) ||
	    (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32) ||
	    ((submode & MCE_POCKET_SUBMODE) != 0 &&
	     (powerkey == FALSE))) {
		goto EXIT;
	}

	/* If woken from pocket mode, doubletap inhibit might stay on */
	doubletap_gesture_inhibited = FALSE;

	/* Only activate visual tklock if the display is off;
	 * else blank the screen again
	 */
	if ((display_state == MCE_DISPLAY_OFF) ||
	    (display_state == MCE_DISPLAY_LPM_OFF) ||
	    (display_state == MCE_DISPLAY_LPM_ON)) {
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_ON),
				       USE_INDATA, CACHE_INDATA);
	} else if (powerkey == TRUE) {
		/* XXX: we probably want to make this configurable */
		/* Blank screen */
		if (tklock_dim_timeout_cb_id == 0) {
			(void)execute_datapipe(&display_state_pipe,
					       GINT_TO_POINTER(MCE_DISPLAY_LPM_ON),
					       USE_INDATA, CACHE_INDATA);
			if (saved_tklock_state == MCE_TKLOCK_VISUAL_STATE)
				saved_tklock_state = MCE_TKLOCK_LOCKED_STATE;

			cancel_tklock_visual_blank_timeout();
		}
	} else {
		/* If visual tklock is enabled, reset the timeout */
		if (is_visual_tklock_enabled() == TRUE) {
			setup_tklock_visual_blank_timeout();
		}
	}

EXIT:
	return;
}

/**
 * D-Bus callback for the get tklock mode method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean tklock_mode_get_req_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	mce_log(LL_DEBUG, "Received tklock mode get request");

	/* Try to send a reply that contains the current tklock mode */
	if (send_tklock_mode(msg) == FALSE)
		goto EXIT;

	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus callback for the tklock mode change method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean tklock_mode_change_req_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	const gchar *mode = NULL;
	gboolean status = FALSE;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	mce_log(LL_DEBUG, "Received tklock mode change request");

	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &mode,
				  DBUS_TYPE_INVALID) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_CRIT,
			"Failed to get argument from %s.%s: %s",
			MCE_REQUEST_IF, MCE_TKLOCK_MODE_CHANGE_REQ,
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	/* Try to change to the requested tklock mode
	 * XXX: right now we silently ignore invalid modes;
	 * should we return an error?
	 */
	if (!strcmp(MCE_TK_LOCKED, mode)) {
		set_tklock_state(LOCK_ON);
	} else if (!strcmp(MCE_TK_LOCKED_DIM, mode)) {
		set_tklock_state(LOCK_ON_DIMMED);
	} else if (!strcmp(MCE_TK_UNLOCKED, mode)) {
		set_tklock_state(LOCK_OFF);

		/* Clear the tklock submode; external unlock
		 * requests overrides automagic relocking
		 */
		saved_submode = ~(~saved_submode | MCE_TKLOCK_SUBMODE);
	} else {
		mce_log(LL_ERR,
			"Received an invalid tklock mode; ignoring");
	}

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

EXIT:
	return status;
}

/**
 * D-Bus callback from SystemUI touchscreen/keypad lock
 *
 * @todo the calls to disable_tklock/open_tklock_ui need error handling
 *
 * @param msg D-Bus message with the lock status
 * @return TRUE on success, FALSE on failure
 */
static gboolean systemui_tklock_dbus_cb(DBusMessage *const msg)
{
	dbus_int32_t result = INT_MAX;
	gboolean status = FALSE;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	mce_log(LL_DEBUG, "Received tklock callback");

	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_INT32, &result,
				  DBUS_TYPE_INVALID) == FALSE) {
		// XXX: should we return an error instead?
		mce_log(LL_CRIT,
			"Failed to get argument from %s.%s: %s",
			MCE_REQUEST_IF, MCE_TKLOCK_CB_REQ,
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	mce_log(LL_DEBUG, "tklock callback value: %d", result);

	switch (result) {
	case TKLOCK_UNLOCK:
		/* Unlock the tklock */
		if ((tklock_ui_state == MCE_TKLOCK_UI_NORMAL) ||
		    (tklock_ui_state == MCE_TKLOCK_UI_SLIDER) ||
		    (tklock_ui_state == MCE_TKLOCK_UI_LPM)) {
			set_tklock_state(LOCK_OFF);
		} else {
			disable_eveater();
		}

		break;

	case TKLOCK_CLOSED:
	default:
		break;
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * GConf callback for touchscreen/keypad lock related settings
 *
 * @param gcc Unused
 * @param id Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data Unused
 */
static void tklock_gconf_cb(GConfClient *const gcc, const guint id,
			    GConfEntry *const entry, gpointer const data)
{
	const GConfValue *gcv = gconf_entry_get_value(entry);

	(void)gcc;
	(void)data;

	/* Key is unset */
	if (gcv == NULL) {
		mce_log(LL_DEBUG,
			"GConf Key `%s' has been unset",
			gconf_entry_get_key(entry));
		goto EXIT;
	}

	if (id == tk_autolock_enabled_cb_id) {
		tk_autolock_enabled = gconf_value_get_bool(gcv) ? 1 : 0;
	} else if (id == doubletap_gesture_policy_cb_id) {
		doubletap_gesture_policy = gconf_value_get_int(gcv);

		if ((doubletap_gesture_policy < 0) ||
		    (doubletap_gesture_policy > 2)) {
			mce_log(LL_WARN,
				"Double tap gesture has invalid policy: %d; "
				"using default",
				doubletap_gesture_policy);
			doubletap_gesture_policy =
				DEFAULT_DOUBLETAP_GESTURE_POLICY;
		}
	} else {
		mce_log(LL_WARN, "Spurious GConf value received; confused!");
	}

EXIT:
	return;
}

static void return_from_proximity(void)
{
	mce_rem_submode_int32(MCE_PROXIMITY_TKLOCK_SUBMODE);
	mce_rem_submode_int32(MCE_POCKET_SUBMODE);

	switch (saved_tklock_state) {
	case MCE_TKLOCK_UNLOCKED_STATE:
	default:
		/* Disable tklock */
		set_tklock_state(LOCK_OFF_PROXIMITY);

		break;

	case MCE_TKLOCK_LOCKED_STATE:
		mce_add_submode_int32(MCE_VISUAL_TKLOCK_SUBMODE);

		/* Enable tklock */
		set_tklock_state(LOCK_ON);

		/* Blank screen */
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_LPM_ON),
				       USE_INDATA, CACHE_INDATA);
		break;

	case MCE_TKLOCK_VISUAL_STATE:
		mce_add_submode_int32(MCE_VISUAL_TKLOCK_SUBMODE);

		/* Enable tklock */
		trigger_visual_tklock(FALSE);

		/* Unblank screen */
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_ON),
				       USE_INDATA, CACHE_INDATA);

		break;
	}
}

/**
 * Process the proximity state
 */
static void process_proximity_state(void)
{
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
	cover_state_t slide_state = datapipe_get_gint(keyboard_slide_pipe);
	cover_state_t proximity_sensor_state =
				datapipe_get_gint(proximity_sensor_pipe);
	/* audio_route_t audio_route = datapipe_get_gint(audio_route_pipe); */
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	call_state_t call_state = datapipe_get_gint(call_state_pipe);

	if ((display_state == MCE_DISPLAY_OFF) ||
	    (display_state == MCE_DISPLAY_LPM_OFF) ||
	    (display_state == MCE_DISPLAY_LPM_ON)) {
		if (proximity_sensor_state == COVER_OPEN) {
			doubletap_gesture_inhibited = FALSE;
			cancel_doubletap_proximity_timeout();
			cancel_pocket_mode_timeout();
			mce_rem_submode_int32(MCE_POCKET_SUBMODE);
			ts_kp_disable_policy();
		} else if (doubletap_gesture_inhibited == FALSE) {
			if (doubletap_gesture_policy != 0)
				setup_doubletap_proximity_timeout();
			if ((is_tklock_enabled_by_proximity() == FALSE) &&
			    (is_pocket_mode_enabled() == FALSE))
				setup_pocket_mode_timeout();
		}
	}


	if ((((alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32) ||
	      (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32)) &&
	     (call_state == CALL_STATE_NONE) &&
	     ((autorelock_triggers & AUTORELOCK_ON_PROXIMITY) == 0)))
			goto EXIT;

	/* If there's an incoming call or an alarm is visible,
	 * and the proximity sensor reports open, unblank the display
	 */
	if ((((call_state == CALL_STATE_RINGING) && 
	      (inhibit_proximity_relock != MCE_TEMP_INHIBIT_PROXIMITY_RELOCK))||
	     ((alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32) ||
	      (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32))) &&
	    (proximity_sensor_state == COVER_OPEN)) {
		ts_kp_enable_policy();

		if (is_eveater_enabled() == TRUE) {
			/* Disable event eater */
			if (close_tklock_ui() == FALSE)
				goto EXIT;
		}

		/* Disable timeouts, just to be sure */
		cancel_tklock_visual_blank_timeout();
		cancel_tklock_unlock_timeout();
		cancel_tklock_dim_timeout();

		/* Unblank screen */
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_ON),
				       USE_INDATA, CACHE_INDATA);

		if ((alarm_ui_state != MCE_ALARM_UI_VISIBLE_INT32) ||
		    (alarm_ui_state != MCE_ALARM_UI_RINGING_INT32)) {
			autorelock_triggers = AUTORELOCK_ON_PROXIMITY;

		} else {
			autorelock_triggers = ~(~autorelock_triggers |
				                AUTORELOCK_ON_PROXIMITY);
		}

		if (call_state == CALL_STATE_RINGING)
			inhibit_proximity_relock = MCE_TEMP_INHIBIT_PROXIMITY_RELOCK;
		mce_rem_submode_int32(MCE_PROXIMITY_TKLOCK_SUBMODE);
		goto EXIT;
	}

	/* If there's no incoming or active call, or the audio isn't
	 * routed to the handset or headset, or if the slide is open, exit
         *
         * XXX: Audio routing has been taken out from the condition, as mce
         * does not currently get the information anywhere.
         * Condition should be re-enabled once audio routing information
         * is available.
	 */

	if (((((call_state != CALL_STATE_RINGING) ||
	       (proximity_lock_when_ringing != TRUE)) &&
	      (call_state != CALL_STATE_ACTIVE)) /* ||
             ((audio_route != AUDIO_ROUTE_HANDSET) &&
	      ((audio_route != AUDIO_ROUTE_SPEAKER) ||
	       (call_state != CALL_STATE_RINGING)))*/) ||
	    ((proximity_lock_with_open_slide == FALSE) &&
	     (slide_state == COVER_OPEN))) {
		goto EXIT;
	}

	switch (proximity_sensor_state) {
	case COVER_OPEN:
		if (autorelock_triggers == AUTORELOCK_ON_PROXIMITY)
			return_from_proximity();
		break;

	case COVER_CLOSED:
		if ((inhibit_proximity_relock == MCE_ALLOW_PROXIMITY_RELOCK) &&
		    ((((is_tklock_enabled() == FALSE) &&
		       (is_autorelock_enabled() == FALSE)) ||
		      ((is_autorelock_enabled() == TRUE) &&
		       (autorelock_triggers == AUTORELOCK_ON_PROXIMITY))) || 
		     ((saved_tklock_state == MCE_TKLOCK_LOCKED_STATE) || 
		      (saved_tklock_state == MCE_TKLOCK_VISUAL_STATE)))) {

			mce_add_submode_int32(MCE_PROXIMITY_TKLOCK_SUBMODE);

			if ((alarm_ui_state != MCE_ALARM_UI_VISIBLE_INT32) &&
			    (alarm_ui_state != MCE_ALARM_UI_RINGING_INT32))
				autorelock_triggers = AUTORELOCK_ON_PROXIMITY;

			/* Enable proximity tklock */
			set_tklock_state(LOCK_ON_PROXIMITY);
		}

		break;

	default:
		break;
	}

EXIT:
	return;
}

/**
 * Datapipe trigger for device inactivity
 *
 * @param data The inactivity stored in a pointer;
 *             TRUE if the device is inactive,
 *             FALSE if the device is active
 */
static void device_inactive_trigger(gconstpointer const data)
{
	gboolean device_inactive = GPOINTER_TO_INT(data);

	if (device_inactive == FALSE) {
		if ((is_tklock_enabled() == TRUE) &&
		    (tklock_visual_blank_timeout_cb_id != 0)) {
			setup_tklock_visual_blank_timeout();
		}
	}
}

/**
 * Datapipe trigger for the keyboard slide
 *
 * @param data COVER_OPEN if the keyboard slide is open,
 *             COVER_CLOSED if the keyboard slide is closed
 */
static void keyboard_slide_trigger(gconstpointer const data)
{
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	cover_state_t kbd_slide_state = GPOINTER_TO_INT(data);

	if ((system_state != MCE_STATE_USER))
		goto EXIT;

	switch (kbd_slide_state) {
	case COVER_OPEN:
		if (is_tklock_enabled() == TRUE) {
			/* Only the trigger that caused the unlock
			 * should trigger autorelock
			 */
			if ((autorelock_triggers & AUTORELOCK_KBD_SLIDE) != 0)
				autorelock_triggers = AUTORELOCK_KBD_SLIDE;

			/* Disable tklock */
			(void)disable_tklock();
			synthesise_activity();
		}

		break;

	case COVER_CLOSED:
		if (((tk_autolock_enabled == TRUE) &&
		     (display_state == MCE_DISPLAY_OFF)) ||
		    ((is_autorelock_enabled() == TRUE) &&
		     ((autorelock_triggers & AUTORELOCK_KBD_SLIDE) != 0)) ||
		    (always_lock_on_slide_close == TRUE)) {
			synthesise_inactivity();

			/* This will also reset the autorelock policy */
			(void)enable_tklock_policy();
		}

		break;

	default:
		break;
	}

	process_proximity_state();

EXIT:
	return;
}

/**
 * Datapipe trigger for the [lock] flicker key
 *
 * @param data 1 if the key was pressed, 0 if the key was released
 */
static void lockkey_trigger(gconstpointer const data)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	call_state_t call_state = datapipe_get_gint(call_state_pipe);

	/* Only react on the [lock] flicker key in USER state */
	if ((GPOINTER_TO_INT(data) == 1) && (system_state == MCE_STATE_USER)) {
		/* Using the flicker key during a call
		 * disables proximity based locking/unlocking
		 */
		if (call_state == CALL_STATE_ACTIVE) {
			autorelock_triggers = ~(~autorelock_triggers |
			                        AUTORELOCK_ON_PROXIMITY);
			inhibit_proximity_relock = MCE_INHIBIT_PROXIMITY_RELOCK;
		}

		set_tklock_state(LOCK_TOGGLE);
	}
}

/**
 * Datapipe trigger for keypresses
 *
 * @param data Keypress state
 */
static void keypress_trigger(gconstpointer const data)
{
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
	static gboolean skip_release = FALSE;
	struct input_event const *const *evp;
	struct input_event const *ev;

	/* Don't dereference until we know it's safe */
	if (data == NULL)
		goto EXIT;

	evp = data;
	ev = *evp;

	disable_autorelock_policy();

	/* Don't dereference until we know it's safe */
	if (ev == NULL)
		goto EXIT;

	if (ev->code == KEY_POWER) {
		if ((skip_release == TRUE) && (ev->value == 0)) {
			cancel_powerkey_repeat_emulation_timeout();
			skip_release = FALSE;
			goto EXIT;
		}

		if ((display_state == MCE_DISPLAY_OFF) ||
		    (display_state == MCE_DISPLAY_LPM_OFF) ||
		    (display_state == MCE_DISPLAY_LPM_ON)) {
			if (ev->value == 1) {
				trigger_visual_tklock(TRUE);
				setup_powerkey_repeat_emulation_timeout();
				skip_release = TRUE;
			}
		} else if (ev->value == 0) {
			trigger_visual_tklock(TRUE);
			cancel_powerkey_repeat_emulation_timeout();
		} else if (ev->value == 1) {
			setup_powerkey_repeat_emulation_timeout();
		}
	} else {
		/* If the keypress is any of:
		 * KEY_CAMERA, KEY_VOLUMEDOWN, KEY_VOLUMEUP
		 * trigger the visual unlock UI on keypress
		 */
		if (((ev->code == KEY_CAMERA) ||
		     ((volkey_visual_trigger == TRUE) &&
		      ((ev->code == KEY_VOLUMEDOWN) ||
		       (ev->code == KEY_VOLUMEUP)))) &&
		     (ev->value == 1)) {
			trigger_visual_tklock(FALSE);
		}
	}

EXIT:
	return;
}

/**
 * Datapipe trigger for camera button
 *
 * @param data Unused
 */
static void camera_button_trigger(gconstpointer const data)
{
	(void)data;

	disable_autorelock_policy();
	trigger_visual_tklock(FALSE);
}

/**
 * Datapipe trigger for touchscreen events; used by autorelock only
 *
 * @param data A pointer to the input_event struct
 */
static void autorelock_touchscreen_trigger(gconstpointer const data)
{
	struct input_event const *const *evp;
	struct input_event const *ev;

	/* Don't dereference until we know it's safe */
	if (data == NULL)
		goto EXIT;

	evp = data;
	ev = *evp;

	if (ev == NULL)
		goto EXIT;

	if (is_tklock_enabled() == FALSE) {
		disable_autorelock_policy();
	}

EXIT:
	return;
}

/**
 * Datapipe trigger for touchscreen events; normal case
 *
 * @param data A pointer to the input_event struct
 */
static void touchscreen_trigger(gconstpointer const data)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	call_state_t call_state = datapipe_get_gint(call_state_pipe);
	alarm_ui_state_t alarm_ui_state =
		datapipe_get_gint(alarm_ui_state_pipe);
	struct input_event const *const *evp;
	struct input_event const *ev;

	/* If we're not in USER state, and there's no call or alarm active,
	 * don't unlock on double tap
	 */
	if ((system_state != MCE_STATE_USER) &&
	    (alarm_ui_state != MCE_ALARM_UI_VISIBLE_INT32) &&
	    (alarm_ui_state != MCE_ALARM_UI_RINGING_INT32) &&
	    ((call_state == CALL_STATE_NONE) ||
	     (call_state == CALL_STATE_INVALID)))
		goto EXIT;

	/* Don't dereference until we know it's safe */
	if (data == NULL)
		goto EXIT;

	evp = data;
	ev = *evp;

	if (ev == NULL)
		goto EXIT;

	if (is_tklock_enabled() == TRUE) {
		/* Double tap */
		if ((ev->type == EV_MSC) &&
		    (ev->code == MSC_GESTURE) &&
		    (ev->value == 0x4)) {
			if (doubletap_gesture_policy == 1) {
				trigger_visual_tklock(FALSE);
			} else if (doubletap_gesture_policy == 2) {
				set_tklock_state(LOCK_OFF_DELAYED);
			} else {
				mce_log(LL_ERR,
					"Got a double tap gesture "
					"even though we haven't enabled "
					"gestures -- this shouldn't happen");
			}
		}
	}

EXIT:
	return;
}

/**
 * Handle system state change
 *
 * @param data The system state stored in a pointer
 */
static void system_state_trigger(gconstpointer data)
{
	system_state_t system_state = GPOINTER_TO_INT(data);

	switch (system_state) {
	case MCE_STATE_SHUTDOWN:
	case MCE_STATE_REBOOT:
	case MCE_STATE_ACTDEAD:
		ts_kp_disable_policy();
		break;

	case MCE_STATE_USER:
	default:
		ts_kp_enable_policy();
		break;
	}
}

/**
 * Handle display state change
 *
 * @param data The display state stored in a pointer
 */
static void display_state_trigger(gconstpointer data)
{
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	static display_state_t old_display_state = MCE_DISPLAY_UNDEF;
	display_state_t display_state = GPOINTER_TO_INT(data);

	if (old_display_state == display_state)
		goto EXIT;

	switch (display_state) {
	case MCE_DISPLAY_OFF:
	case MCE_DISPLAY_LPM_OFF:
		if (is_tklock_enabled_by_proximity() == TRUE) {
			ts_kp_disable_policy();
		} else if ((alarm_ui_state != MCE_ALARM_UI_RINGING_INT32) &&
		           (is_tklock_enabled() == TRUE)) {
			if (is_malf_state_enabled() == FALSE) { 
				(void)open_tklock_ui(TKLOCK_ENABLE_VISUAL);
			}
			ts_kp_disable_policy();
		} else {
			(void)enable_autokeylock();
		}

		break;

	case MCE_DISPLAY_LPM_ON:
		if ((alarm_ui_state != MCE_ALARM_UI_RINGING_INT32) &&
		    (is_tklock_enabled() == TRUE)) {
			if (enable_tklock() == TRUE)
				ts_kp_disable_policy();
		} else {
			(void)enable_autokeylock();
		}

		break;


	case MCE_DISPLAY_DIM:
		if (is_tklock_enabled_by_proximity() == FALSE) {
			enable_eveater();
		}

		/* If the display transitions from OFF, UNDEF or LOW_POWER
		 * to DIM or ON, do policy based enable
		 */
		if ((old_display_state == MCE_DISPLAY_UNDEF) ||
		    (old_display_state == MCE_DISPLAY_OFF) ||
		    (old_display_state == MCE_DISPLAY_LPM_OFF) ||
		    (old_display_state == MCE_DISPLAY_LPM_ON)) {
			ts_kp_enable_policy();
		}

		cancel_pocket_mode_timeout();
		mce_rem_submode_int32(MCE_POCKET_SUBMODE);

		break;

	case MCE_DISPLAY_ON:
	default:
		/* If the display transitions from OFF, UNDEF or LOW_POWER
		 * to DIM or ON, do policy based enable
		 */
		if ((old_display_state == MCE_DISPLAY_UNDEF) ||
		    (old_display_state == MCE_DISPLAY_OFF) ||
		    (old_display_state == MCE_DISPLAY_LPM_OFF) ||
		    (old_display_state == MCE_DISPLAY_LPM_ON)) {
			ts_kp_enable_policy();

			/* If visual tklock is enabled, reset the timeout,
			 * and open the visual tklock
			 */
			if (is_visual_tklock_enabled() == TRUE) {
				open_tklock_ui(TKLOCK_ENABLE_VISUAL);
				saved_tklock_state = MCE_TKLOCK_VISUAL_STATE;
				setup_tklock_visual_blank_timeout();
			}
		}

		cancel_pocket_mode_timeout();
		mce_rem_submode_int32(MCE_POCKET_SUBMODE);

		(void)disable_eveater();
		break;
	}

	old_display_state = display_state;

EXIT:
	return;
}

/**
 * Handle alarm UI state change
 *
 * @param data The alarm state stored in a pointer
 */
static void alarm_ui_state_trigger(gconstpointer data)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	cover_state_t proximity_sensor_state =
				datapipe_get_gint(proximity_sensor_pipe);
	alarm_ui_state_t alarm_ui_state = GPOINTER_TO_INT(data);
	call_state_t call_state = datapipe_get_gint(call_state_pipe);
	audio_route_t audio_route = datapipe_get_gint(audio_route_pipe);

	switch (alarm_ui_state) {
	case MCE_ALARM_UI_VISIBLE_INT32:
		mce_rem_submode_int32(MCE_PROXIMITY_TKLOCK_SUBMODE);

		if (is_tklock_enabled() == TRUE) {
			/* Event eater is used when tklock is disabled,
			 * so make sure to disable it if we enable the tklock
			 */
			disable_eveater();

			if (open_tklock_ui(TKLOCK_ENABLE_LPM_UI) == FALSE) {
				(void)disable_tklock();
				goto EXIT;
			}

			enable_autorelock();
			setup_dim_blank_timeout_policy(MCE_DISPLAY_OFF);
		} else if (is_eveater_enabled() == TRUE) {
			ts_kp_enable_policy();

			if (open_tklock_ui(TKLOCK_ONEINPUT) == FALSE) {
				disable_eveater();
				goto EXIT;
			}

			setup_dim_blank_timeout_policy(MCE_DISPLAY_UNDEF);
		}

		break;

	case MCE_ALARM_UI_RINGING_INT32:
		/* If the proximity state is "open",
		 * disable event eater UI and proximity sensor
		 */
		if (proximity_sensor_state == COVER_OPEN) {
			ts_kp_enable_policy();

			autorelock_triggers = ~(~autorelock_triggers |
				                AUTORELOCK_ON_PROXIMITY);
			mce_rem_submode_int32(MCE_PROXIMITY_TKLOCK_SUBMODE);

			/* Disable timeouts, just to be sure */
			cancel_tklock_visual_blank_timeout();
			cancel_tklock_unlock_timeout();
			cancel_tklock_dim_timeout();

			/* Unblank screen */
			(void)execute_datapipe(&display_state_pipe,
					       GINT_TO_POINTER(MCE_DISPLAY_ON),
					       USE_INDATA, CACHE_INDATA);
		} else {
			inhibit_proximity_relock = MCE_ALLOW_PROXIMITY_RELOCK;
			autorelock_triggers = (autorelock_triggers |
					       AUTORELOCK_ON_PROXIMITY);
			if (is_tklock_enabled() == TRUE) {
				mce_add_submode_int32(MCE_PROXIMITY_TKLOCK_SUBMODE);
			} else {
				mce_rem_submode_int32(MCE_PROXIMITY_TKLOCK_SUBMODE);
			}
		}

		break;

	case MCE_ALARM_UI_OFF_INT32:
		if ((is_tklock_enabled_by_proximity() == TRUE) &&
		    (call_state != CALL_STATE_INVALID) &&
		    (call_state != CALL_STATE_NONE) &&
		    (audio_route == AUDIO_ROUTE_HANDSET))
			break;

		mce_rem_submode_int32(MCE_PROXIMITY_TKLOCK_SUBMODE);

		/* In acting dead the event eater is only
		 * used when showing the alarm UI
		 */
		if (system_state != MCE_STATE_USER) {
			disable_eveater();
		} else if ((call_state != CALL_STATE_INVALID) &&
			   (call_state != CALL_STATE_NONE) &&
			   (is_tklock_enabled() == TRUE)) {
			disable_eveater();
			set_tklock_state(LOCK_OFF);
		} else if (is_visual_tklock_enabled() == TRUE) {
			setup_tklock_visual_blank_timeout();
		} else if (is_tklock_enabled() == TRUE) {
			ts_kp_disable_policy();

			/* Event eater is used when tklock is disabled,
			 * so make sure to disable it if we enable the tklock
			 */
			disable_eveater();

			if (open_tklock_ui(TKLOCK_ENABLE_LPM_UI) == FALSE) {
				(void)disable_tklock();
				goto EXIT;
			} else {
				mce_add_submode_int32(MCE_VISUAL_TKLOCK_SUBMODE);
			}

			enable_autorelock();
			setup_dim_blank_timeout_policy(MCE_DISPLAY_OFF);
		} else if (is_eveater_enabled() == TRUE) {
			if (open_tklock_ui(TKLOCK_ONEINPUT) == FALSE) {
				disable_eveater();
				goto EXIT;
			}

			setup_dim_blank_timeout_policy(MCE_DISPLAY_UNDEF);
		}

		break;

	default:
		break;
	}

EXIT:
	return;
}

/**
 * Handle lid cover sensor state change
 *
 * @param data The lid cover state stored in a pointer
 */
static void lid_cover_trigger(gconstpointer data)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	cover_state_t lid_cover_state = GPOINTER_TO_INT(data);

	switch (lid_cover_state) {
	case COVER_OPEN:
		if (system_state == MCE_STATE_USER) {
			setup_tklock_unlock_timeout();
			(void)execute_datapipe(&display_state_pipe,
					       GINT_TO_POINTER(MCE_DISPLAY_ON),
					       USE_INDATA, CACHE_INDATA);
		}

		break;

	case COVER_CLOSED:
		if (system_state == MCE_STATE_USER) {
			synthesise_inactivity();

			if (enable_tklock_policy() == TRUE) {
				/* Blank screen */
				(void)execute_datapipe(&display_state_pipe,
						       GINT_TO_POINTER(MCE_DISPLAY_LPM_OFF),
						       USE_INDATA, CACHE_INDATA);
			}
		}

		break;

	default:
		break;
	}
}

/**
 * Handle proximity sensor state change
 *
 * @param data Unused
 */
static void proximity_sensor_trigger(gconstpointer data)
{
	(void)data;

	process_proximity_state();
}

/**
 * Handle lens cover state change
 *
 * @param data The lens cover state stored in a pointer
 */
static void lens_cover_trigger(gconstpointer data)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	cover_state_t lens_cover_state = GPOINTER_TO_INT(data);

	if ((system_state != MCE_STATE_USER))
		goto EXIT;

	if (lens_cover_unlock == FALSE)
		goto EXIT;

	switch (lens_cover_state) {
	case COVER_OPEN:
		if (is_tklock_enabled() == TRUE) {
			/* Only the trigger that caused the unlock
			 * should trigger autorelock
			 */
			if ((autorelock_triggers & AUTORELOCK_LENS_COVER) != 0)
				autorelock_triggers = AUTORELOCK_LENS_COVER;

			/* Disable tklock */
			(void)disable_tklock();
			synthesise_activity();
		}

		break;

	case COVER_CLOSED:
		if ((is_autorelock_enabled() == TRUE) &&
		    ((autorelock_triggers & AUTORELOCK_LENS_COVER) != 0)) {
			synthesise_inactivity();

			/* This will also reset the autorelock policy */
			(void)enable_tklock_policy();
		}

		break;

	default:
		break;
	}

EXIT:
	return;
}

/**
 * Handle touchscreen/keypad lock state
 *
 * @param data The touchscreen/keypad lock state stored in a pointer
 */
static void tk_lock_trigger(gconstpointer data)
{
	lock_state_t tk_lock_state = GPOINTER_TO_INT(data);

	set_tklock_state(tk_lock_state);
}

/**
 * Handle submode change
 *
 * @param data The submode stored in a pointer
 */
static void submode_trigger(gconstpointer data)
{
	static submode_t old_submode = MCE_NORMAL_SUBMODE;
	submode_t submode = GPOINTER_TO_INT(data);

	/* If we transition from !softoff to softoff,
	 * disable touchscreen and keypad events,
	 * otherwise enable them
	 */
	if ((submode & MCE_SOFTOFF_SUBMODE) != 0) {
		if ((old_submode & MCE_SOFTOFF_SUBMODE) == 0) {
			ts_disable();
			kp_disable();
		}
	} else {
		if ((old_submode & MCE_SOFTOFF_SUBMODE) != 0) {
			set_doubletap_gesture(FALSE);
			kp_enable();
			ts_enable();
		}
	}

	old_submode = submode;
}

/**
 * Handle call state change
 *
 * @param data The call state stored in a pointer
 */
static void call_state_trigger(gconstpointer data)
{
	static call_state_t old_call_state = CALL_STATE_INVALID;
	call_state_t call_state = GPOINTER_TO_INT(data);
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
	/* Saving the state for not to interfere with old call paths */
	gboolean proximity_locked = is_tklock_enabled_by_proximity();

	switch (call_state) {
	case CALL_STATE_RINGING:
		inhibit_proximity_relock = MCE_ALLOW_PROXIMITY_RELOCK;

		/* Incoming call, update the submode,
		 * unless there's already a call ongoing
		 */
		if (old_call_state != CALL_STATE_ACTIVE) {
			saved_submode = mce_get_submode_int32();
		}

		break;

	case CALL_STATE_ACTIVE:
		if (is_visual_tklock_enabled() == TRUE) {
			setup_tklock_visual_blank_timeout();
		}

		if (old_call_state != CALL_STATE_ACTIVE)
			inhibit_proximity_relock = MCE_ALLOW_PROXIMITY_RELOCK;

		/* If we're answering a call, don't alter anything */
		if (old_call_state == CALL_STATE_RINGING)
			break;

		/* Call initiated on our end, update the submode,
		 * unless we're just upgrading a normal call to
		 * an emergency call
		 */
		if (old_call_state != CALL_STATE_ACTIVE)
			saved_submode = mce_get_submode_int32();

		break;

	case CALL_STATE_NONE:
	default:
		/* Submode not set, update submode */
		if (saved_submode == MCE_INVALID_SUBMODE)
			saved_submode = mce_get_submode_int32();

		if (autorelock_triggers == AUTORELOCK_ON_PROXIMITY)
			autorelock_triggers = AUTORELOCK_NO_TRIGGERS;

		if (proximity_locked == TRUE) {
			if ((saved_tklock_state == MCE_TKLOCK_LOCKED_STATE) ||
			    ((autorelock_after_call_end == TRUE) &&
			     ((saved_submode & MCE_TKLOCK_SUBMODE) != 0)))
				saved_tklock_state = MCE_TKLOCK_VISUAL_STATE;

			return_from_proximity();
		} else if (is_visual_tklock_enabled() == TRUE) {
			if (display_state == MCE_DISPLAY_ON)
				setup_tklock_visual_blank_timeout();
		} else if ((autorelock_after_call_end == TRUE) &&
			   ((saved_submode & MCE_TKLOCK_SUBMODE) != 0)) {
			synthesise_inactivity();

			/* Enable the tklock again */
			enable_tklock_policy();
		} else if (is_tklock_enabled() == FALSE) {
			/* Disable autorelock */
			disable_autorelock();

			/* Unblank screen */
			(void)execute_datapipe(&display_state_pipe,
					       GINT_TO_POINTER(MCE_DISPLAY_ON),
					       USE_INDATA, CACHE_INDATA);
		}

		break;
	}

	process_proximity_state();
	old_call_state = call_state;
}

/**
 * Handle audio routing changes
 *
 * @param data The audio route stored in a pointer
 */
static void audio_route_trigger(gconstpointer data)
{
	audio_route_t audio_route = GPOINTER_TO_INT(data);

	switch (audio_route) {
	case AUDIO_ROUTE_HANDSET:
	case AUDIO_ROUTE_HEADSET:
		if (inhibit_proximity_relock ==
		    MCE_TEMP_INHIBIT_PROXIMITY_RELOCK)
			inhibit_proximity_relock = MCE_ALLOW_PROXIMITY_RELOCK;
		break;

	case AUDIO_ROUTE_SPEAKER:
	case AUDIO_ROUTE_UNDEF:
	default:
		if (inhibit_proximity_relock == MCE_ALLOW_PROXIMITY_RELOCK)
			inhibit_proximity_relock =
				MCE_TEMP_INHIBIT_PROXIMITY_RELOCK;
		break;
	}

	/* process_proximity_state() would be better place for this */
	if (is_tklock_enabled_by_proximity() == TRUE) {
		mce_rem_submode_int32(MCE_PROXIMITY_TKLOCK_SUBMODE);
		/* disable_tklock() resets mode, we are not in this branch if we're
		 * in LPM/pocket mode or normally tklocked */
		disable_tklock();
		(void)execute_datapipe(&display_state_pipe, GINT_TO_POINTER(MCE_DISPLAY_ON), USE_INDATA, CACHE_INDATA);
	}

	process_proximity_state();
}

/**
 * Handle USB cable connection change
 *
 * @param data The usb cable state stored in a pointer
 */
static void usb_cable_trigger(gconstpointer data)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	usb_cable_state_t usb_cable_state = GPOINTER_TO_INT(data);

	if ((system_state != MCE_STATE_USER) ||
		is_tklock_enabled_by_proximity() || is_pocket_mode_enabled())
		goto EXIT;

	switch (usb_cable_state) {
	case USB_CABLE_CONNECTED:
	case USB_CABLE_DISCONNECTED:
		trigger_visual_tklock(FALSE);
		break;

	default:
		break;
	}

EXIT:
	return;
}

/**
 * Handle jack sense change
 *
 * @param data The jack sense state stored in a pointer
 */
static void jack_sense_trigger(gconstpointer data)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	cover_state_t jack_sense_state = GPOINTER_TO_INT(data);

	if ((system_state != MCE_STATE_USER) ||
		is_tklock_enabled_by_proximity() || is_pocket_mode_enabled())
		goto EXIT;

	switch (jack_sense_state) {
	case COVER_OPEN:
	case COVER_CLOSED:
		trigger_visual_tklock(FALSE);
		break;

	default:
		break;
	}

EXIT:
	return;
}

/**
 * Init function for the touchscreen/keypad lock component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_tklock_init(void)
{
	gboolean status = FALSE;

	/* Init event control files */
	if (g_access(MCE_RX51_KEYBOARD_SYSFS_DISABLE_PATH, W_OK) == 0) {
		mce_keypad_sysfs_disable_path =
			MCE_RX51_KEYBOARD_SYSFS_DISABLE_PATH;
	} else if (g_access(MCE_RX44_KEYBOARD_SYSFS_DISABLE_PATH, W_OK) == 0) {
		mce_keypad_sysfs_disable_path =
			MCE_RX44_KEYBOARD_SYSFS_DISABLE_PATH;
	} else if (g_access(MCE_KEYPAD_SYSFS_DISABLE_PATH, W_OK) == 0) {
		mce_keypad_sysfs_disable_path =
			MCE_KEYPAD_SYSFS_DISABLE_PATH;
	} else {
		mce_log(LL_INFO,
			"No touchscreen event control interface available");
	}

	if (g_access(MCE_RM680_TOUCHSCREEN_SYSFS_DISABLE_PATH, W_OK) == 0) {
		mce_touchscreen_sysfs_disable_path =
			MCE_RM680_TOUCHSCREEN_SYSFS_DISABLE_PATH;
	} else if (g_access(MCE_RX44_TOUCHSCREEN_SYSFS_DISABLE_PATH, W_OK) == 0) {
		mce_touchscreen_sysfs_disable_path =
			MCE_RX44_TOUCHSCREEN_SYSFS_DISABLE_PATH;
	} else {
		mce_log(LL_INFO,
			"No keypress event control interface available");
	}

	if (g_access(MCE_RM680_DOUBLETAP_SYSFS_PATH, W_OK) == 0) {
		mce_touchscreen_gesture_control_path =
			MCE_RM680_DOUBLETAP_SYSFS_PATH;
	} else {
		mce_log(LL_INFO,
			"No touchscreen gesture control interface available");
	}

	if (g_access(MCE_RM680_TOUCHSCREEN_CALIBRATION_PATH, W_OK) == 0) {
		mce_touchscreen_calibration_control_path =
			MCE_RM680_TOUCHSCREEN_CALIBRATION_PATH;
	} else {
		mce_log(LL_INFO,
			"No touchscreen calibration control interface available");
	}

	errno = 0;

	/* Close the touchscreen/keypad lock and event eater UI,
	 * to make sure MCE doesn't end up in a confused state
	 * if restarted
	 */
	// FIXME: error handling?
	(void)disable_tklock();
	(void)disable_eveater();
	disable_autorelock();

	/* Append triggers/filters to datapipes */
	append_input_trigger_to_datapipe(&device_inactive_pipe,
					 device_inactive_trigger);
	append_input_trigger_to_datapipe(&touchscreen_pipe,
					 touchscreen_trigger);
	append_input_trigger_to_datapipe(&keyboard_slide_pipe,
					 keyboard_slide_trigger);
	append_input_trigger_to_datapipe(&lockkey_pipe,
					 lockkey_trigger);
	append_input_trigger_to_datapipe(&keypress_pipe,
					 keypress_trigger);
	append_input_trigger_to_datapipe(&camera_button_pipe,
					 camera_button_trigger);
	append_output_trigger_to_datapipe(&system_state_pipe,
					  system_state_trigger);
	append_output_trigger_to_datapipe(&display_state_pipe,
					  display_state_trigger);
	append_output_trigger_to_datapipe(&alarm_ui_state_pipe,
					  alarm_ui_state_trigger);
	append_output_trigger_to_datapipe(&lid_cover_pipe,
					  lid_cover_trigger);
	append_output_trigger_to_datapipe(&proximity_sensor_pipe,
					  proximity_sensor_trigger);
	append_output_trigger_to_datapipe(&lens_cover_pipe,
					  lens_cover_trigger);
	append_output_trigger_to_datapipe(&tk_lock_pipe,
					  tk_lock_trigger);
	append_output_trigger_to_datapipe(&submode_pipe,
					  submode_trigger);
	append_output_trigger_to_datapipe(&call_state_pipe,
					  call_state_trigger);
	append_output_trigger_to_datapipe(&audio_route_pipe,
					  audio_route_trigger);
	append_output_trigger_to_datapipe(&jack_sense_pipe,
					  jack_sense_trigger);
	append_output_trigger_to_datapipe(&usb_cable_pipe,
					  usb_cable_trigger);
	append_output_trigger_to_datapipe(&heartbeat_pipe,
					  heartbeat_trigger);

	/* Touchscreen/keypad autolock */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_bool(MCE_GCONF_TK_AUTOLOCK_ENABLED_PATH,
				 &tk_autolock_enabled);

	/* Touchscreen/keypad autolock enabled/disabled */
	if (mce_gconf_notifier_add(MCE_GCONF_LOCK_PATH,
				   MCE_GCONF_TK_AUTOLOCK_ENABLED_PATH,
				   tklock_gconf_cb,
				   &tk_autolock_enabled_cb_id) == FALSE)
		goto EXIT;

	/* Touchscreen/keypad double-tap gesture policy */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_int(MCE_GCONF_TK_DOUBLE_TAP_GESTURE_PATH,
				 &doubletap_gesture_policy);

	if ((doubletap_gesture_policy < 0) ||
	    (doubletap_gesture_policy > 2)) {
		mce_log(LL_WARN,
			"Double tap gesture has invalid policy: %d; "
			"using default",
			doubletap_gesture_policy);
		doubletap_gesture_policy =
			DEFAULT_DOUBLETAP_GESTURE_POLICY;
	}

	/* Touchscreen/keypad autolock enabled/disabled */
	if (mce_gconf_notifier_add(MCE_GCONF_LOCK_PATH,
				   MCE_GCONF_TK_DOUBLE_TAP_GESTURE_PATH,
				   tklock_gconf_cb,
				   &doubletap_gesture_policy_cb_id) == FALSE)
		goto EXIT;

	/* get_tklock_mode */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_TKLOCK_MODE_GET,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 tklock_mode_get_req_dbus_cb) == NULL)
		goto EXIT;

	/* req_tklock_mode_change */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_TKLOCK_MODE_CHANGE_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 tklock_mode_change_req_dbus_cb) == NULL)
		goto EXIT;

	/* tklock_callback */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_TKLOCK_CB_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 systemui_tklock_dbus_cb) == NULL)
		goto EXIT;

	/* Get configuration options */
	blank_immediately =
		mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
				  MCE_CONF_BLANK_IMMEDIATELY,
				  DEFAULT_BLANK_IMMEDIATELY,
				  NULL);

	dim_immediately =
		mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
				 MCE_CONF_DIM_IMMEDIATELY,
				 DEFAULT_DIM_IMMEDIATELY,
				 NULL);

	dim_delay = mce_conf_get_int(MCE_CONF_TKLOCK_GROUP,
				     MCE_CONF_DIM_DELAY,
				     DEFAULT_DIM_DELAY,
				     NULL);

	disable_ts_immediately =
		mce_conf_get_int(MCE_CONF_TKLOCK_GROUP,
				 MCE_CONF_TS_OFF_IMMEDIATELY,
				 DEFAULT_TS_OFF_IMMEDIATELY,
				 NULL);

	/* Fallback in case double tap event is not supported */
	if ((mce_touchscreen_gesture_control_path == NULL) &&
	    (disable_ts_immediately == 2)) {
		disable_ts_immediately = 1;
	}

	disable_kp_immediately =
		mce_conf_get_int(MCE_CONF_TKLOCK_GROUP,
				 MCE_CONF_KP_OFF_IMMEDIATELY,
				 DEFAULT_KP_OFF_IMMEDIATELY,
				 NULL);

	autolock_with_open_slide =
		mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
				  MCE_CONF_AUTOLOCK_SLIDE_OPEN,
				  DEFAULT_AUTOLOCK_SLIDE_OPEN,
				  NULL);

	proximity_lock_with_open_slide =
		mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
				  MCE_CONF_PROXIMITY_LOCK_SLIDE_OPEN,
				  DEFAULT_PROXIMITY_LOCK_SLIDE_OPEN,
				  NULL);

	always_lock_on_slide_close =
		mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
				  MCE_CONF_LOCK_ON_SLIDE_CLOSE,
				  DEFAULT_LOCK_ON_SLIDE_CLOSE,
				  NULL);

	lens_cover_unlock =
		mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
				  MCE_CONF_LENS_COVER_UNLOCK,
				  DEFAULT_LENS_COVER_UNLOCK,
				  NULL);

	volkey_visual_trigger =
		mce_conf_get_bool(MCE_CONF_TKLOCK_GROUP,
				  MCE_CONF_VOLKEY_VISUAL_TRIGGER,
				  DEFAULT_VOLKEY_VISUAL_TRIGGER,
				  NULL);

	status = TRUE;

EXIT:
	return status;
}

/**
 * Exit function for the touchscreen/keypad lock component
 *
 * @todo D-Bus unregistration
 */
void mce_tklock_exit(void)
{
	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&heartbeat_pipe,
					    heartbeat_trigger);
	remove_output_trigger_from_datapipe(&usb_cable_pipe,
					    usb_cable_trigger);
	remove_output_trigger_from_datapipe(&jack_sense_pipe,
					    jack_sense_trigger);
	remove_output_trigger_from_datapipe(&audio_route_pipe,
					    audio_route_trigger);
	remove_output_trigger_from_datapipe(&call_state_pipe,
					    call_state_trigger);
	remove_output_trigger_from_datapipe(&submode_pipe,
					    submode_trigger);
	remove_output_trigger_from_datapipe(&tk_lock_pipe,
					    tk_lock_trigger);
	remove_output_trigger_from_datapipe(&lens_cover_pipe,
					    lens_cover_trigger);
	remove_output_trigger_from_datapipe(&proximity_sensor_pipe,
					    proximity_sensor_trigger);
	remove_output_trigger_from_datapipe(&lid_cover_pipe,
					    lid_cover_trigger);
	remove_output_trigger_from_datapipe(&alarm_ui_state_pipe,
					    alarm_ui_state_trigger);
	remove_output_trigger_from_datapipe(&display_state_pipe,
					    display_state_trigger);
	remove_output_trigger_from_datapipe(&system_state_pipe,
					    system_state_trigger);
	remove_input_trigger_from_datapipe(&camera_button_pipe,
					   camera_button_trigger);
	remove_input_trigger_from_datapipe(&keypress_pipe,
					   keypress_trigger);
	remove_input_trigger_from_datapipe(&lockkey_pipe,
					   lockkey_trigger);
	remove_input_trigger_from_datapipe(&keyboard_slide_pipe,
					   keyboard_slide_trigger);
	remove_input_trigger_from_datapipe(&touchscreen_pipe,
					   touchscreen_trigger);
	remove_input_trigger_from_datapipe(&device_inactive_pipe,
					   device_inactive_trigger);

	/* This trigger is conditional; attempt to remove it anyway */
	remove_input_trigger_from_datapipe(&touchscreen_pipe,
					   autorelock_touchscreen_trigger);

	/* Remove all timeout sources */
	cancel_powerkey_repeat_emulation_timeout();
	cancel_doubletap_proximity_timeout();
	cancel_pocket_mode_timeout();
	cancel_tklock_visual_blank_timeout();
	cancel_tklock_unlock_timeout();
	cancel_tklock_dim_timeout();
	cancel_doubletap_recal_timeout();

	return;
}
