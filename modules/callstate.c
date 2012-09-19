/**
 * @file callstate.c
 * Call state module -- this handles the call state for MCE
 * <p>
 * Copyright © 2008-2009 Nokia Corporation and/or its subsidiary(-ies).
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

#include <string.h>			/* strncmp(), strlen() */

#include "mce.h"
#include "callstate.h"

#include "ofono-dbus-names.h"

#include <mce/mode-names.h>		/* MCE_CALL_STATE_NONE,
					 * MCE_CALL_STATE_ACTIVE,
					 * MCE_CALL_STATE_SERVICE,
					 * MCE_NORMAL_CALL,
					 * MCE_EMERGENCY_CALL
					 */

#include "mce.h"			/* FIXME
					 * CALL_STATE_INVALID,
					 * CALL_STATE_NONE,
					 * CALL_STATE_RINGING,
					 * CALL_STATE_ACTIVE,
					 * CALL_STATE_SERVICE,
					 * INVALID_CALL,
					 * NORMAL_CALL,
					 * EMERGENCY_CALL,
					 * call_state_t,
					 * call_type_t
					 */

#include "mce-lib.h"			/* mce_translate_int_to_string(),
					 * mce_translate_string_to_int(),
					 * mce_translation_t
					 */
#include "mce-log.h"			/* mce_log(), LL_* */
#include "mce-dbus.h"			/* Direct:
					 * ---
					 * mce_dbus_handler_add(),
					 * dbus_send_message(),
					 * dbus_new_signal(),
					 * dbus_new_method_reply(),
					 * dbus_message_get_args(),
					 * dbus_message_get_no_reply(),
					 * dbus_message_append_args(),
					 * dbus_message_unref(),
					 * dbus_error_init(),
					 * dbus_error_free(),
					 * DBUS_MESSAGE_TYPE_METHOD_CALL,
					 * DBUS_TYPE_STRING,
					 * DBUS_TYPE_INVALID,
					 * DBusMessage, DBusError,
					 * dbus_bool_t
					 *
					 * Indirect:
					 * ---
					 * MCE_SIGNAL_IF,
					 * MCE_SIGNAL_PATH,
					 * MCE_REQUEST_IF,
					 * MCE_CALL_STATE_GET,
					 * MCE_CALL_STATE_CHANGE_REQ,
					 * MCE_CALL_STATE_SIG
					 */
#include "datapipe.h"			/* FIXME execute_datapipe() */

/** Module name */
#define MODULE_NAME		"callstate"

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
	/** Name of the module */
	.name = MODULE_NAME,
	/** Module provides */
	.provides = provides,
	/** Module priority */
	.priority = 250
};

/** Mapping of call state integer <-> call state string */
static const mce_translation_t call_state_translation[] = {
	{
		.number = CALL_STATE_NONE,
		.string = MCE_CALL_STATE_NONE
	}, {
		.number = CALL_STATE_RINGING,
		.string = MCE_CALL_STATE_RINGING,
	}, {
		.number = CALL_STATE_ACTIVE,
		.string = MCE_CALL_STATE_ACTIVE,
	}, {
		.number = CALL_STATE_SERVICE,
		.string = MCE_CALL_STATE_SERVICE
	}, { /* MCE_INVALID_TRANSLATION marks the end of this array */
		.number = MCE_INVALID_TRANSLATION,
		.string = MCE_CALL_STATE_NONE
	}
};

/** Mapping of call type integer <-> call type string */
static const mce_translation_t call_type_translation[] = {
	{
		.number = NORMAL_CALL,
		.string = MCE_NORMAL_CALL
	}, {
		.number = EMERGENCY_CALL,
		.string = MCE_EMERGENCY_CALL
	}, { /* MCE_INVALID_TRANSLATION marks the end of this array */
		.number = MCE_INVALID_TRANSLATION,
		.string = MCE_NORMAL_CALL
	}
};

/** List of monitored call state requesters; holds zero or one entries */
static GSList *call_state_monitor_list = NULL;

/** Keep track of whether call state is monitored */
static gboolean call_state_is_monitored = FALSE;

/**
 * Send the call state and type
 *
 * @param method_call A DBusMessage to reply to;
 *                    pass NULL to send a signal instead
 * @param call_state A string representation of an alternate state
 *                   to send instead of the real call state
 * @param call_type A string representation of an alternate type
 *                  to send instead of the real call type
 * @return TRUE on success, FALSE on failure
 */
static gboolean send_call_state(DBusMessage *const method_call,
				const gchar *const call_state,
				const gchar *const call_type)
{
	DBusMessage *msg = NULL;
	gboolean status = FALSE;
	const gchar *sstate;
	const gchar *stype;

	/* Allow spoofing */
	if (call_state != NULL)
		sstate = call_state;
	else
		sstate = mce_translate_int_to_string(call_state_translation,
						     datapipe_get_gint(call_state_pipe));

	if (call_type != NULL)
		stype = call_type;
	else
		stype = mce_translate_int_to_string(call_type_translation,
						    datapipe_get_gint(call_type_pipe));

	/* If method_call is set, send a reply,
	 * otherwise, send a signal
	 */
	if (method_call != NULL) {
		msg = dbus_new_method_reply(method_call);
	} else {
		/* sig_call_state_ind */
		msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
				      MCE_CALL_STATE_SIG);
	}

	/* Append the call state and call type */
	if (dbus_message_append_args(msg,
				     DBUS_TYPE_STRING, &sstate,
				     DBUS_TYPE_STRING, &stype,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append %sarguments to D-Bus message "
			"for %s.%s",
			method_call ? "reply " : "",
			method_call ? MCE_REQUEST_IF :
				      MCE_SIGNAL_IF,
			method_call ? MCE_CALL_STATE_GET :
				      MCE_CALL_STATE_SIG);
		dbus_message_unref(msg);
		goto EXIT;
	}

	/* Send the message */
	status = dbus_send_message(msg);

EXIT:
	return status;
}

/**
 * D-Bus callback used for monitoring the process that requested
 * the call state; if that process exits, immediately
 * restore the call state to "none" and call type to "normal"
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean call_state_owner_monitor_dbus_cb(DBusMessage *const msg)
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

	/* Remove the name monitor for the call state requester */
	if (mce_dbus_owner_monitor_remove(old_name,
					  &call_state_monitor_list) == 0) {
		/* Signal the new call state/type */
		send_call_state(NULL, MCE_CALL_STATE_NONE, MCE_NORMAL_CALL);

		(void)execute_datapipe(&call_state_pipe,
				       GINT_TO_POINTER(CALL_STATE_NONE),
				       USE_INDATA, CACHE_INDATA);

		(void)execute_datapipe(&call_type_pipe,
				       GINT_TO_POINTER(NORMAL_CALL),
				       USE_INDATA, CACHE_INDATA);

		call_state_is_monitored = FALSE;
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus callback for the call state change request method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean change_call_state_dbus_cb(DBusMessage *const msg)
{
	call_state_t old_call_state = datapipe_get_gint(call_state_pipe);
	call_state_t old_call_type = datapipe_get_gint(call_type_pipe);
	const gchar *sender = dbus_message_get_sender(msg);
	call_state_t call_state = CALL_STATE_NONE;
	dbus_bool_t monitored_owner_ok = FALSE;
	call_type_t call_type = NORMAL_CALL;
	dbus_bool_t state_changed = FALSE;
	DBusMessage *reply = NULL;
	const gchar *state = NULL;
	const gchar *type = NULL;
	gboolean status = FALSE;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	mce_log(LL_DEBUG,
		"Received set call state request");

	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &state,
				  DBUS_TYPE_STRING, &type,
				  DBUS_TYPE_INVALID) == FALSE) {
		// XXX: return an error!
		mce_log(LL_CRIT,
			"Failed to get argument from %s.%s: %s",
			MCE_REQUEST_IF, MCE_CALL_STATE_CHANGE_REQ,
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	/* Convert call state to enum */
	call_state = mce_translate_string_to_int(call_state_translation,
						 state);

	if (call_state == MCE_INVALID_TRANSLATION) {
		mce_log(LL_DEBUG,
			"Invalid call state received; request ignored");
		goto EXIT;
	}

	/* Convert call type to enum */
	call_type = mce_translate_string_to_int(call_type_translation,
						type);

	if (call_type == MCE_INVALID_TRANSLATION) {
		mce_log(LL_DEBUG,
			"Invalid call type received; request ignored");
		goto EXIT;
	}

	/* If call state isn't monitored or if the request comes from
	 * the owner of the current state, then some additional changes
	 * are ok
	 */
	if ((call_state_is_monitored == FALSE) ||
	    (call_state_monitor_list == NULL) ||
	    ((mce_dbus_is_owner_monitored(sender,
					  call_state_monitor_list) == TRUE))) {
		monitored_owner_ok = TRUE;
	}

	/* Only transitions to/from "none" are allowed,
	 * and from "ringing" to "active",
	 * to avoid race conditions; except when new tuple
	 * is active:emergency
	 */
	if ((call_state != CALL_STATE_NONE) &&
	    (old_call_state != CALL_STATE_NONE) &&
	    ((call_state != CALL_STATE_ACTIVE) ||
	     (old_call_state != CALL_STATE_RINGING)) &&
	    ((call_state != CALL_STATE_RINGING) ||
	     (old_call_state != CALL_STATE_ACTIVE) ||
	     (monitored_owner_ok == FALSE)) &&
	    ((call_state != CALL_STATE_ACTIVE) ||
	     (call_type != EMERGENCY_CALL))) {
		mce_log(LL_INFO,
		        "Call state change vetoed.  Requested: %i:%i "
			"(current: %i:%i)",
			call_state, call_type,
			old_call_state, old_call_type);
		goto EXIT;
	}

#ifdef STRICT_CALL_STATE_OWNER_POLICY
	/* We always allow changes to the call state
	 * if the new type is emergency, or if the old call state is none,
	 * but otherwise we only allow them if the requester of
	 * the change is the owner of the current call state
	 */
	if ((old_call_state != CALL_STATE_NONE) &&
	    (call_type != EMERGENCY_CALL) &&
	    (monitored_owner_ok == FALSE)) {
			mce_log(LL_ERR,
				"Call state change vetoed.  "
				"`%s' request the new call state (%i:%i), "
				"but does not own current call state (%i:%i)",
				sender,
				call_state, call_type,
				old_call_state, old_call_type);
			goto EXIT;
	}
#endif /* STRICT_CALL_STATE_OWNER_POLICY */

	if (call_state != CALL_STATE_NONE) {
		if (mce_dbus_owner_monitor_add(sender,
					       call_state_owner_monitor_dbus_cb,
					       &call_state_monitor_list, 1) == -1) {
			/* This is dangerous, but calls are our priority */
			mce_log(LL_ERR,
				"Failed to add a D-Bus service owner monitor "
				"for the call state; "
				"call state will not be monitored!");
			call_state_is_monitored = FALSE;
		} else {
			call_state_is_monitored = TRUE;
		}
	} else {
		(void)mce_dbus_owner_monitor_remove(sender,
						    &call_state_monitor_list);
		call_state_is_monitored = FALSE;
	}

	state_changed = TRUE;

EXIT:
	/* Setup the reply */
	reply = dbus_new_method_reply(msg);

	/* Append the result */
	if (dbus_message_append_args(reply,
				     DBUS_TYPE_BOOLEAN, &state_changed,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append reply arguments to D-Bus "
			"message for %s.%s",
			MCE_REQUEST_IF, MCE_CALL_STATE_CHANGE_REQ);
		dbus_message_unref(reply);

		/* If we cannot send the reply,
		 * we have to abort the state change
		 */
		state_changed = FALSE;
	} else {
		/* Send the message */
		status = dbus_send_message(reply);
	}

	/* If the state changed, signal the new state;
	 * first externally, then internally
	 *
	 * The reason we do it externally first is to
	 * make sure that the camera application doesn't
	 * grab audio, otherwise the ring tone might go missing
	 */
	if (state_changed == TRUE) {
		/* Signal the new call state/type */
		send_call_state(NULL, state, type);

		(void)execute_datapipe(&call_state_pipe,
				       GINT_TO_POINTER(call_state),
				       USE_INDATA, CACHE_INDATA);

		(void)execute_datapipe(&call_type_pipe,
				       GINT_TO_POINTER(call_type),
				       USE_INDATA, CACHE_INDATA);
	}

	return status;
}

/**
 * D-Bus callback for the get call state method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean get_call_state_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	mce_log(LL_DEBUG, "Received call state get request");

	/* Try to send a reply that contains the current call state and type */
	if (send_call_state(msg, NULL, NULL) == FALSE)
		goto EXIT;

	status = TRUE;

EXIT:
	return status;
}

/**
 * Parses an ofono property message from given iterator.
 *
 * Actions related to the property are done.
 *
 * @param it Iterator to ofono property.
 * @return TRUE on success, FALSE on failure.
 */
static gboolean ofono_handle_call_property(DBusMessageIter *it)
{
	gboolean status = FALSE;
	gboolean call_state_changed = FALSE;
	DBusMessageIter varit;
	gchar *propname;
	gchar *prop_value_str;
	gboolean prop_value_bool;
	call_state_t old_call_state = datapipe_get_gint(call_state_pipe);
	call_type_t old_call_type = datapipe_get_gint(call_type_pipe);
	call_state_t call_state = old_call_state;
	call_type_t call_type = old_call_type;

	if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_STRING) {
		mce_log(LL_WARN, "Parsing failure with ofono signal");
		goto EXIT;
	}
	dbus_message_iter_get_basic(it, (void *)&propname);
	mce_log(LL_DEBUG, "Handling call property '%s' from ofono", propname);

	dbus_message_iter_next(it);

	if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_VARIANT) {
		mce_log(LL_WARN, "Parsing failure with ofono signal");
		goto EXIT;
	}
	dbus_message_iter_recurse(it, &varit);

	if (!strcmp(propname, "State")) {
		if (dbus_message_iter_get_arg_type(&varit) != DBUS_TYPE_STRING) {
			mce_log(LL_WARN, "Parsing failure with ofono signal");
			goto EXIT;
		}
		dbus_message_iter_get_basic(&varit, (void *)&prop_value_str);

		if (!strcmp(prop_value_str, "incoming") ||
		    !strcmp(prop_value_str, "dialing")) {
			call_state = CALL_STATE_RINGING;
		} else if (!strcmp(prop_value_str, "disconnected")) {
			call_state = CALL_STATE_NONE;
		} else {
			call_state = CALL_STATE_ACTIVE;
		}

		if (old_call_state != call_state)
			call_state_changed = TRUE;

	} else if (!strcmp(propname, "Emergency")) {
		if (dbus_message_iter_get_arg_type(&varit) != DBUS_TYPE_BOOLEAN) {
			mce_log(LL_WARN, "Parsing failure with ofono signal");
			goto EXIT;
		}
		dbus_message_iter_get_basic(&varit, (void *)&prop_value_bool);

		if (prop_value_bool) {
			call_type = EMERGENCY_CALL;
		} else {
			call_type = NORMAL_CALL;
		}

		if (old_call_type != call_type)
			call_state_changed = TRUE;

	} else {
		mce_log(LL_DEBUG,
		        "No handling for property '%s' from ofono", propname);
	}

	if (call_state_changed) {
		(void)execute_datapipe(&call_state_pipe,
					   GINT_TO_POINTER(call_state),
					   USE_INDATA, CACHE_INDATA);

		(void)execute_datapipe(&call_type_pipe,
					   GINT_TO_POINTER(call_type),
					   USE_INDATA, CACHE_INDATA);
	}

	status = TRUE;
EXIT:
	return status;
}

/**
 * D-Bus callback for ofono call added signal.
 *
 * @param msg The D-Bus message.
 * @return TRUE on success, FALSE on failure.
 */
 static gboolean ofono_call_added_dbus_cb(DBusMessage *const msg)
 {
	gboolean status = FALSE;
	DBusMessageIter msgit;
	DBusMessageIter arrit;
	DBusMessageIter entit;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	mce_log(LL_DEBUG,
		"Received call added signal from ofono");

	dbus_message_iter_init(msg, &msgit);

	/* Message correctness checking */
	if (dbus_message_iter_get_arg_type(&msgit) != DBUS_TYPE_OBJECT_PATH) {
		mce_log(LL_WARN, "Parsing failure with ofono signal");
		goto EXIT;
	}

	if (!dbus_message_iter_next(&msgit) ||
	    dbus_message_iter_get_arg_type(&msgit) != DBUS_TYPE_ARRAY) {
		mce_log(LL_WARN, "Parsing failure with ofono signal");
		goto EXIT;
	}

	dbus_message_iter_recurse(&msgit, &arrit);

	if (dbus_message_iter_get_arg_type(&arrit) != DBUS_TYPE_DICT_ENTRY) {
		mce_log(LL_WARN, "Parsing failure with ofono signal");
		goto EXIT;
	}

	dbus_message_iter_recurse(&arrit, &entit);

	do {
		if (!ofono_handle_call_property(&entit)) {
			mce_log(LL_WARN,
			        "Failed to parse call property change from ofono");
		}
		goto EXIT;
	} while (dbus_message_iter_next(&entit));

	status = TRUE;
EXIT:
	return status;
 }

/**
 * D-Bus callback for ofono call property changed signal.
 *
 * @param msg The D-Bus message.
 * @return TRUE on success, FALSE on failure.
 */
 static gboolean ofono_call_props_changed_dbus_cb(DBusMessage *const msg)
 {
	gboolean status = FALSE;
	DBusMessageIter msgit;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	mce_log(LL_DEBUG,
		"Received call property changed signal from ofono");

	dbus_message_iter_init(msg, &msgit);

	if (!ofono_handle_call_property(&msgit)) {
		mce_log(LL_WARN,
		        "Failed to parse call property change from ofono");
		goto EXIT;
	}

	status = TRUE;
EXIT:
	return status;
}

/**
 * Init function for the call state module
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

	/* req_call_state_change */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_CALL_STATE_CHANGE_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 change_call_state_dbus_cb) == NULL)
		goto EXIT;

	/* get_call_state */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_CALL_STATE_GET,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 get_call_state_dbus_cb) == NULL)
		goto EXIT;

	//~ /* Listen to call added signal from ofono */
	if (mce_dbus_handler_add(OFONO_VOICEMGR_SIGNAL_IF,
				 OFONO_CALL_ADDED_SIG,
				 NULL,
				 DBUS_MESSAGE_TYPE_SIGNAL,
				 ofono_call_added_dbus_cb) == NULL)
		goto EXIT;

	/* Listen to call property change signal from ofono */
	if (mce_dbus_handler_add(OFONO_VOICE_SIGNAL_IF,
				 OFONO_VOICE_PROP_CHANGED_SIG,
				 NULL,
				 DBUS_MESSAGE_TYPE_SIGNAL,
				 ofono_call_props_changed_dbus_cb) == NULL)
		goto EXIT;

EXIT:
	return NULL;
}

/**
 * Exit function for the call state module
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
