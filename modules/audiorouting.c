/**
 * @file audiorouting.c
 * Audio routing module -- this listens to the audio routing
 * <p>
 * Copyright © 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
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
#include <gmodule.h>

#include <string.h>			/* strcmp() */

#include "mce.h"

#include "mce-log.h"			/* mce_log(), LL_* */
#include "mce-dbus.h"			/* Direct:
					 * ---
					 * DBusMessageIter
					 *
					 * Indirect:
					 * ---
					 */

/** Module name */
#define MODULE_NAME		"audiorouting"

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
	/** Name of the module */
	.name = MODULE_NAME,
	/** Module provides */
	.provides = provides,
	/** Module priority */
	.priority = 100
};

/** D-Bus interface for the policy framework */
#define POLICY_DBUS_INTERFACE		"com.nokia.policy"
/** D-Bus signal for actions from the policy framework */
#define POLICY_AUDIO_ACTIONS		"audio_actions"

/** Macro to access offsets in the action data struct */
#define STRUCT_OFFSET(s, m)		((char *)&(((s *)0)->m) - (char *)0)

/** audio route arguments */
struct argrt {
	gchar *type;			/**< Audio route type */
	gchar *device;			/**< Device */
	gchar *mode;			/**< Mode */
	gchar *hwid;			/**< Hardware ID */
};

/** argument descriptor for actions */
struct argdsc {
	const gchar *name;		/**< Name */
	gint offs;			/**< Offset */
	gint type;			/**< Type */
};

/** Full audio route */
typedef enum {
	FULL_AUDIO_ROUTE_UNDEF = -1,		/**< Audio routing not set */
	/** Audio disabled */
	FULL_AUDIO_ROUTE_NULL = 0,
	/** Audio routed through internal stereo speakers */
	FULL_AUDIO_ROUTE_IHF = 1,
	/** Audio routed through FM transmitter */
	FULL_AUDIO_ROUTE_FMTX = 2,
	/** Audio routed through internal stereo speakers and FM transmitter */
	FULL_AUDIO_ROUTE_IHF_AND_FMTX = 3,
	/** Audio routed through earpiece */
	FULL_AUDIO_ROUTE_EARPIECE = 4,
	/** Audio routed through earpiece and TV-out */
	FULL_AUDIO_ROUTE_EARPIECE_AND_TVOUT = 5,
	/** Audio routed through TV-out */
	FULL_AUDIO_ROUTE_TVOUT = 6,
	/** Audio routed through internal stereo speakers and TV-out */
	FULL_AUDIO_ROUTE_IHF_AND_TVOUT = 7,
	/** Audio routed through headphone */
	FULL_AUDIO_ROUTE_HEADPHONE = 8,
	/** Audio routed through headset */
	FULL_AUDIO_ROUTE_HEADSET = 9,
	/** Audio routed through BT HSP */
	FULL_AUDIO_ROUTE_BTHSP = 10,
	/** Audio routed through BT A2DP */
	FULL_AUDIO_ROUTE_BTA2DP = 11,
	/** Audio routed through internal stereo speakers and headset */
	FULL_AUDIO_ROUTE_IHF_AND_HEADSET = 12,
	/** Audio routed through internal stereo speakers and BT HSP */
	FULL_AUDIO_ROUTE_IHF_AND_BTHSP = 13,
	/** Audio routed through TV-out and BT HSP */
	FULL_AUDIO_ROUTE_TVOUT_AND_BTHSP = 14,
	/** Audio routed through TV-out and BT A2DP */
	FULL_AUDIO_ROUTE_TVOUT_AND_BTA2DP = 15,
	/** Audio routed through some other device */
	FULL_AUDIO_ROUTE_OTHER = 255
} full_audio_route_t;

/**
 * Parser used to parse the action information
 *
 * @param actit Iterator for the action data
 * @param descs A struct with the argument descriptors
 * @param args The arguments to parse
 * @param len The size of args
 * @return TRUE on success, FALSE on failure
 */
static gboolean action_parser(DBusMessageIter *actit, struct argdsc *descs,
			      void *args, gint len)
{
	gboolean status = FALSE;
	DBusMessageIter cmdit;
	DBusMessageIter argit;
	DBusMessageIter valit;
	struct argdsc *desc;
	gchar *argname;
	void *argval;

	dbus_message_iter_recurse(actit, &cmdit);

	memset(args, 0, len);

	do {
		if (dbus_message_iter_get_arg_type(&cmdit) != DBUS_TYPE_STRUCT)
			goto EXIT;

		dbus_message_iter_recurse(&cmdit, &argit);

		if (dbus_message_iter_get_arg_type(&argit) != DBUS_TYPE_STRING)
			goto EXIT;

		dbus_message_iter_get_basic(&argit, (void *)&argname);

		if (!dbus_message_iter_next(&argit))
			goto EXIT;

		if (dbus_message_iter_get_arg_type(&argit) != DBUS_TYPE_VARIANT)
			goto EXIT;

		dbus_message_iter_recurse(&argit, &valit);

		for (desc = descs; desc->name != NULL; desc++) {
			if (!strcmp(argname, desc->name)) {
				if ((desc->offs +
				     (gint)sizeof (gchar *)) > len) {
					mce_log(LL_ERR,
						"%s desc offset %d is "
						"out of range %d",
						"action_parser()",
						desc->offs, len);
					goto EXIT;
				} else {
					if (dbus_message_iter_get_arg_type(&valit) != desc->type)
						goto EXIT;

					argval = (char *)args + desc->offs;

					dbus_message_iter_get_basic(&valit, argval);
				}

				break;
			}
		}

	} while (dbus_message_iter_next(&cmdit));

	status = TRUE;

EXIT:
	return status;
}

/**
 * Parser for the audio route
 *
 * @param data Iterator for the data
 * @return TRUE on success, FALSE on failure
 */
static gboolean audio_route_parser(DBusMessageIter *data)
{
	static struct argdsc descs[] = {
		{
			"type",
			STRUCT_OFFSET(struct argrt, type),
			DBUS_TYPE_STRING
		}, {
			"device",
			STRUCT_OFFSET(struct argrt, device),
			DBUS_TYPE_STRING
		}, {
			"mode",
			STRUCT_OFFSET(struct argrt, mode),
			DBUS_TYPE_STRING
		}, {
			"hwid",
			STRUCT_OFFSET(struct argrt, hwid),
			DBUS_TYPE_STRING
		}, { NULL, 0, DBUS_TYPE_INVALID }
	};

	full_audio_route_t full_audio_route = FULL_AUDIO_ROUTE_UNDEF;
	static audio_route_t old_audio_route = AUDIO_ROUTE_UNDEF;
	audio_route_t audio_route = AUDIO_ROUTE_UNDEF;
	gboolean status = FALSE;
	struct argrt args;

	do {
		/* If we fail to parse, abort */
		if (!action_parser(data, descs, &args, sizeof (args)))
			goto EXIT;

		/* If we don't get the type or device, abort */
		if ((args.type == NULL) || (args.device == NULL))
			goto EXIT;

		/* If this isn't the sink, we're not interested */
		if (strcmp(args.type, "sink"))
			continue;

		if (!strcmp(args.device, "null")) {
			full_audio_route = FULL_AUDIO_ROUTE_NULL;
		} else if (!strcmp(args.device, "ihf") ||
			   !strcmp(args.device, "ihfforcall")) {
			full_audio_route = FULL_AUDIO_ROUTE_IHF;
		} else if (!strcmp(args.device, "fmtx")) {
			full_audio_route = FULL_AUDIO_ROUTE_FMTX;
		} else if (!strcmp(args.device, "ihfandfmtx")) {
			full_audio_route = FULL_AUDIO_ROUTE_IHF_AND_FMTX;
		} else if (!strcmp(args.device, "earpiece")) {
			full_audio_route = FULL_AUDIO_ROUTE_EARPIECE;
		} else if (!strcmp(args.device, "earpieceandtvout")) {
			full_audio_route = FULL_AUDIO_ROUTE_EARPIECE_AND_TVOUT;
		} else if (!strcmp(args.device, "tvout")) {
			full_audio_route = FULL_AUDIO_ROUTE_TVOUT;
		} else if (!strcmp(args.device, "ihfandtvout")) {
			full_audio_route = FULL_AUDIO_ROUTE_IHF_AND_TVOUT;
		} else if (!strcmp(args.device, "headphone")) {
			full_audio_route = FULL_AUDIO_ROUTE_HEADPHONE;
		} else if (!strcmp(args.device, "headset")) {
			full_audio_route = FULL_AUDIO_ROUTE_HEADSET;
		} else if (!strcmp(args.device, "headsetforcall")) {
			full_audio_route = FULL_AUDIO_ROUTE_HEADSET;
		} else if (!strcmp(args.device, "bthsp")) {
			full_audio_route = FULL_AUDIO_ROUTE_BTHSP;
		} else if (!strcmp(args.device, "bta2dp")) {
			full_audio_route = FULL_AUDIO_ROUTE_BTA2DP;
		} else if (!strcmp(args.device, "ihfandheadset")) {
			full_audio_route = FULL_AUDIO_ROUTE_IHF_AND_HEADSET;
		} else if (!strcmp(args.device, "ihfandbthsp")) {
			full_audio_route = FULL_AUDIO_ROUTE_IHF_AND_BTHSP;
		} else if (!strcmp(args.device, "tvoutandbthsp")) {
			full_audio_route = FULL_AUDIO_ROUTE_TVOUT_AND_BTHSP;
		} else if (!strcmp(args.device, "tvoutandbta2dp")) {
			full_audio_route = FULL_AUDIO_ROUTE_TVOUT_AND_BTA2DP;
		} else {
			full_audio_route = FULL_AUDIO_ROUTE_OTHER;
			mce_log(LL_WARN, "unknown audio sink device = '%s'",
				args.device);
		}
	} while (dbus_message_iter_next(data));

EXIT:
	/* Convert the full audio route to a simplified version
	 * suitable for mass consumption
	 */
	switch (full_audio_route) {
	/* Handset */
	case FULL_AUDIO_ROUTE_EARPIECE:
	case FULL_AUDIO_ROUTE_EARPIECE_AND_TVOUT:
		audio_route = AUDIO_ROUTE_HANDSET;
		break;

	/* Internal stereo speakers */
	case FULL_AUDIO_ROUTE_IHF:
	case FULL_AUDIO_ROUTE_IHF_AND_FMTX:
	case FULL_AUDIO_ROUTE_IHF_AND_TVOUT:
		audio_route = AUDIO_ROUTE_SPEAKER;
		break;

	/* Headset */
	case FULL_AUDIO_ROUTE_HEADSET:
	case FULL_AUDIO_ROUTE_IHF_AND_HEADSET:
	case FULL_AUDIO_ROUTE_BTHSP:
	case FULL_AUDIO_ROUTE_BTA2DP:
	case FULL_AUDIO_ROUTE_TVOUT_AND_BTHSP:
	case FULL_AUDIO_ROUTE_TVOUT_AND_BTA2DP:
		audio_route = AUDIO_ROUTE_HEADSET;
		break;

	/* If the full audio route didn't change, don't change audio route;
	 * also ignore NULL routes
	 */
	case FULL_AUDIO_ROUTE_NULL:
	case FULL_AUDIO_ROUTE_UNDEF:
		audio_route = old_audio_route;
		break;

	/* For cases that aren't relevant to us anyway, map to undef */
	default:
		audio_route = AUDIO_ROUTE_UNDEF;
		break;
	}

	if (audio_route != old_audio_route) {
		execute_datapipe(&audio_route_pipe,
				 GINT_TO_POINTER(audio_route),
				 USE_INDATA, CACHE_INDATA);
		old_audio_route = audio_route;
	}

	if (full_audio_route != FULL_AUDIO_ROUTE_UNDEF) {
		status = TRUE;
	}

	return status;
}

/**
 * D-Bus callback for the actions signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean actions_dbus_cb(DBusMessage *msg)
{
	gboolean status = FALSE;
	DBusMessageIter msgit;
	DBusMessageIter arrit;
	DBusMessageIter entit;
	DBusMessageIter actit;
	dbus_uint32_t txid;
	gchar *actname;

	mce_log(LL_DEBUG, "Received policy actions");

	dbus_message_iter_init(msg, &msgit);

	if (dbus_message_iter_get_arg_type(&msgit) != DBUS_TYPE_UINT32)
		goto EXIT;

	dbus_message_iter_get_basic(&msgit, (void *)&txid);

	mce_log(LL_DEBUG, "got actions; txid: %d", txid);

	if (!dbus_message_iter_next(&msgit) ||
	    dbus_message_iter_get_arg_type(&msgit) != DBUS_TYPE_ARRAY)
		goto EXIT;

	dbus_message_iter_recurse(&msgit, &arrit);

	do {
		if (dbus_message_iter_get_arg_type(&arrit) != DBUS_TYPE_DICT_ENTRY)
			continue;

		dbus_message_iter_recurse(&arrit, &entit);

		do {
			if (dbus_message_iter_get_arg_type(&entit) != DBUS_TYPE_STRING)
				continue;

			dbus_message_iter_get_basic(&entit, (void *)&actname);

			if (!dbus_message_iter_next(&entit) ||
			    dbus_message_iter_get_arg_type(&entit) != DBUS_TYPE_ARRAY)
				continue;

			dbus_message_iter_recurse(&entit, &actit);

			if (dbus_message_iter_get_arg_type(&actit) != DBUS_TYPE_ARRAY)
				continue;

			if (!strcmp("com.nokia.policy.audio_route", actname))
				if (audio_route_parser(&actit) == TRUE)
					break;
		} while (dbus_message_iter_next(&entit));
	} while (dbus_message_iter_next(&arrit));

	status = TRUE;

EXIT:
	return status;
}

/**
 * Init function for the audio routing module
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
	(void)module;

	/* actions */
	if (mce_dbus_handler_add(POLICY_DBUS_INTERFACE,
				 POLICY_AUDIO_ACTIONS,
				 NULL,
				 DBUS_MESSAGE_TYPE_SIGNAL,
				 actions_dbus_cb) == NULL)
		goto EXIT;

EXIT:
	return NULL;
}

/**
 * Exit function for the audio routing module
 *
 * @todo D-Bus unregistration
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	return;
}
