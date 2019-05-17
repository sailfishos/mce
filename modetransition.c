/**
 * @file modetransition.c
 * This file implements the submode handling component
 * of the Mode Control Entity
 * <p>
 * Copyright © 2004-2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2013-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Tapio Rantala <ext-tapio.rantala@nokia.com>
 * @author Santtu Lakkala <ext-santtu.1.lakkala@nokia.com>
 * @author Irina Bezruk <ext-irina.bezruk@nokia.com>
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

#include "modetransition.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-io.h"

#include <unistd.h>
#include <errno.h>

#include <glib/gstdio.h>

/**
 * Set the MCE submode flags
 *
 * @param submode All submodes to set OR:ed together
 * @return TRUE on success, FALSE on failure
 */
static gboolean mce_set_submode_int32(const submode_t submode)
{
	submode_t old_submode = datapipe_get_gint(submode_pipe);

	if (old_submode == submode)
		goto EXIT;

	mce_log(LL_NOTICE, "submode change: %s",
		submode_change_repr(old_submode, submode));

	datapipe_exec_full(&submode_pipe, GINT_TO_POINTER(submode));
EXIT:
	return TRUE;
}

/**
 * Add flags to the MCE submode
 *
 * @param submode submode(s) to add OR:ed together
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_add_submode_int32(const submode_t submode)
{
	submode_t old_submode = datapipe_get_gint(submode_pipe);

	return mce_set_submode_int32(old_submode | submode);
}

/**
 * Remove flags from the MCE submode
 *
 * @param submode submode(s) to remove OR:ed together
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_rem_submode_int32(const submode_t submode)
{
	submode_t old_submode = datapipe_get_gint(submode_pipe);

	return mce_set_submode_int32(old_submode & ~submode);
}

/**
 * Return all set MCE submode flags
 *
 * @return All set submode flags OR:ed together
 */
submode_t mce_get_submode_int32(void) G_GNUC_PURE;
submode_t mce_get_submode_int32(void)
{
	submode_t submode = datapipe_get_gint(submode_pipe);

	return submode;
}

/**
 * Handle system state change
 *
 * @param data The system state stored in a pointer
 */
static void system_state_trigger(gconstpointer data)
{
	static system_state_t old_system_state = MCE_SYSTEM_STATE_UNDEF;
	system_state_t system_state = GPOINTER_TO_INT(data);

	switch (system_state) {
	case MCE_SYSTEM_STATE_USER:
		break;

	case MCE_SYSTEM_STATE_SHUTDOWN:
	case MCE_SYSTEM_STATE_REBOOT:
		/* Actions to perform when shutting down/rebooting
		 */
		switch( old_system_state ) {
		case MCE_SYSTEM_STATE_USER:
		case MCE_SYSTEM_STATE_BOOT:
		case MCE_SYSTEM_STATE_UNDEF:
		case MCE_SYSTEM_STATE_ACTDEAD:
			datapipe_exec_full(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_DEVICE_ON);
			datapipe_exec_full(&led_pattern_activate_pipe, MCE_LED_PATTERN_POWER_OFF);
			break;

		default:
			break;
		}

		/* If we're shutting down/rebooting from actdead or
		 * user mode, ui side will do shutdown animation.
		 * Unblank the screen to make it visible. */
		switch( old_system_state ) {
		case MCE_SYSTEM_STATE_USER:
		case MCE_SYSTEM_STATE_ACTDEAD:
			mce_datapipe_request_display_state(MCE_DISPLAY_ON);
			break;

		default:
			break;
		}
		break;

	case MCE_SYSTEM_STATE_ACTDEAD:
		break;

	case MCE_SYSTEM_STATE_UNDEF:
		goto EXIT;

	default:
		break;
	}

	mce_log(LL_DEBUG,
		"dsmestate set to: %d (old: %d)",
		system_state, old_system_state);

	old_system_state = system_state;

EXIT:
	return;
}

/** Array of datapipe handlers */
static datapipe_handler_t mce_mode_datapipe_handlers[] =
{
	// output triggers
	{
		.datapipe  = &system_state_pipe,
		.output_cb = system_state_trigger,
	},
	// sentinel
	{
		.datapipe = 0,
	}
};

static datapipe_bindings_t mce_mode_datapipe_bindings =
{
	.module   = "mce_mode",
	.handlers = mce_mode_datapipe_handlers,
};

/** Append triggers/filters to datapipes
 */
static void mce_mode_datapipe_init(void)
{
	mce_datapipe_init_bindings(&mce_mode_datapipe_bindings);
}

/** Remove triggers/filters from datapipes
 */
static void mce_mode_datapipe_quit(void)
{
	mce_datapipe_quit_bindings(&mce_mode_datapipe_bindings);
}

/**
 * Init function for the modetransition component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_mode_init(void)
{
	gboolean status = FALSE;

	/* Append triggers/filters to datapipes */
	mce_mode_datapipe_init();

	/* If the bootup file exists, mce has crashed / restarted;
	 * since it exists in /var/run it will be removed when we reboot.
	 *
	 * If the file doesn't exist, create it to ensure that
	 * restarting mce doesn't get mce stuck in the transition submode
	 */
	if (g_access(MCE_BOOTUP_FILENAME, F_OK) == -1) {
		if (errno == ENOENT) {
			mce_log(LL_DEBUG, "Bootup mode enabled");
			mce_add_submode_int32(MCE_SUBMODE_TRANSITION);
			errno = 0;

			mce_write_string_to_file(MCE_BOOTUP_FILENAME,
						 ENABLED_STRING);

			if (g_access(MALF_FILENAME, F_OK) == 0) {
				mce_add_submode_int32(MCE_SUBMODE_MALF);
				mce_log(LL_DEBUG, "Malf mode enabled");
				if (g_access(MCE_MALF_FILENAME, F_OK) == -1) {
					if (errno != ENOENT) {
						mce_log(LL_CRIT,
							"access() failed: %s. Exiting.",
							g_strerror(errno));
						goto EXIT;
					}

					mce_write_string_to_file(MCE_MALF_FILENAME,
								 ENABLED_STRING);
				}
			}
		} else {
			mce_log(LL_CRIT,
				"access() failed: %s. Exiting.",
				g_strerror(errno));
			goto EXIT;
		}
	} else {
		if (g_access(MALF_FILENAME, F_OK) == 0) {
			if (g_access(MCE_MALF_FILENAME, F_OK) == 0) {
				mce_add_submode_int32(MCE_SUBMODE_MALF);
				mce_log(LL_DEBUG, "Malf mode enabled");
			}
		} else if ((errno == ENOENT) &&
			   (g_access(MCE_MALF_FILENAME, F_OK) == 0)) {
			g_remove(MCE_MALF_FILENAME);
		}
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * Exit function for the modetransition component
 *
 * @todo D-Bus unregistration
 */
void mce_mode_exit(void)
{
	/* Remove triggers/filters from datapipes */
	mce_mode_datapipe_quit();

	return;
}
