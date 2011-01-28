/**
 * @file modetransition.c
 * This file implements the mode transition component
 * (normal/flight mode/off)
 * of the Mode Control Entity
 * <p>
 * Copyright © 2004-2010 Nokia Corporation and/or its subsidiary(-ies).
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

#include <errno.h>			/* errno, ENOENT */
#include <stdlib.h>			/* exit(), EXIT_FAILURE */
#include <unistd.h>			/* access(), F_OK */

#include <mce/mode-names.h>

#include "mce.h"			/* MCE_LED_PATTERN_POWER_ON,
					 * MCE_LED_PATTERN_POWER_OFF,
					 * MCE_LED_PATTERN_DEVICE_ON,
					 * MCE_STATE_UNDEF,
					 * submode_t,
					 * system_state_t,
					 * MCE_TRANSITION_SUBMODE,
					 * mainloop,
					 * display_state_pipe,
					 * led_pattern_activate_pipe,
					 * led_pattern_deactivate_pipe,
					 * submode_pipe,
					 * system_state_pipe
					 */
#include "modetransition.h"

#include "mce-io.h"			/* mce_read_string_from_file(),
					 * mce_write_string_to_file()
					 */
#include "mce-log.h"			/* mce_log(), LL_* */
#include "datapipe.h"			/* execute_datapipe(),
					 * execute_datapipe_output_triggers(),
					 * datapipe_get_gint(),
					 * append_output_trigger_to_datapipe(),
					 * remove_output_trigger_from_datapipe()
					 */

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

	execute_datapipe(&submode_pipe, GINT_TO_POINTER(submode),
			 USE_INDATA, CACHE_INDATA);
	mce_log(LL_DEBUG, "Submode changed to %d", submode);

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
	static system_state_t old_system_state = MCE_STATE_UNDEF;
	system_state_t system_state = GPOINTER_TO_INT(data);

	switch (system_state) {
	case MCE_STATE_USER:
		if (old_system_state == MCE_STATE_ACTDEAD) {
			execute_datapipe_output_triggers(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_POWER_ON, USE_INDATA);
		}

		break;

	case MCE_STATE_SHUTDOWN:
	case MCE_STATE_REBOOT:
		/* Actions to perform when shutting down/rebooting
		 * from anything else than acting dead
		 */
		if ((old_system_state == MCE_STATE_USER) ||
		    (old_system_state == MCE_STATE_BOOT) ||
		    (old_system_state == MCE_STATE_UNDEF)) {
			execute_datapipe_output_triggers(&led_pattern_deactivate_pipe, MCE_LED_PATTERN_DEVICE_ON, USE_INDATA);
			execute_datapipe_output_triggers(&led_pattern_activate_pipe, MCE_LED_PATTERN_POWER_OFF, USE_INDATA);
		}

		/* If we're shutting down/rebooting from acting dead,
		 * blank the screen
		 */
		if (old_system_state == MCE_STATE_ACTDEAD) {
			execute_datapipe(&display_state_pipe,
					 GINT_TO_POINTER(MCE_DISPLAY_OFF),
					 USE_INDATA, CACHE_INDATA);
		}

		break;

	case MCE_STATE_ACTDEAD:
		break;

	case MCE_STATE_UNDEF:
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

/**
 * Init function for the modetransition component
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_mode_init(void)
{
	gboolean status = FALSE;

	/* Append triggers/filters to datapipes */
	append_output_trigger_to_datapipe(&system_state_pipe,
					  system_state_trigger);

	/* If the bootup file exists, mce has crashed / restarted;
	 * since it exists in /var/run it will be removed when we reboot.
	 *
	 * If the file doesn't exist, create it to ensure that
	 * restarting mce doesn't get mce stuck in the transition submode
	 */
	if (access(MCE_BOOTUP_FILENAME, F_OK) == -1) {
		if (errno == ENOENT) {
			mce_log(LL_DEBUG, "Bootup mode enabled");
			mce_add_submode_int32(MCE_TRANSITION_SUBMODE);
			errno = 0;

			(void)mce_write_string_to_file(MCE_BOOTUP_FILENAME,
						       ENABLED_STRING);
		} else {
			mce_log(LL_CRIT,
				"access() failed: %s. Exiting.",
				g_strerror(errno));
			goto EXIT;
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
	remove_output_trigger_from_datapipe(&system_state_pipe,
					    system_state_trigger);

	return;
}
