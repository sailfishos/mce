/**
 * @file mce-dbus.c
 * D-Bus handling code for the Mode Control Entity
 * <p>
 * Copyright © 2004-2009 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Ismo Laitinen <ismo.laitinen@nokia.com>
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

#include <stdarg.h>			/* va_start(), va_end() */
#include <stdlib.h>			/* exit(), EXIT_FAILURE */
#include <string.h>			/* strcmp() */
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>	/* dbus_connection_setup_with_g_main */

#include "mce.h"
#include "mce-dbus.h"

#include "mce-log.h"			/* mce_log(), LL_* */

/** List of all D-Bus handlers */
static GSList *dbus_handlers = NULL;

/** D-Bus handler structure */
typedef struct {
	gboolean (*callback)(DBusMessage *const msg);	/**< Handler callback */
	gchar *interface;		/**< The interface to listen on */
	gchar *rules;			/**< Additional matching rules */
	gchar *name;			/**< Method call or signal name */
	guint type;			/**< DBUS_MESSAGE_TYPE */
} handler_struct;

/** Pointer to the DBusConnection */
static DBusConnection *dbus_connection = NULL;

/**
 * Create a new D-Bus signal, with proper error checking
 * will exit the mainloop if an error occurs
 *
 * @param path The signal path
 * @param interface The signal interface
 * @param name The name of the signal to send
 * @return A new DBusMessage
 */
DBusMessage *dbus_new_signal(const gchar *const path,
			     const gchar *const interface,
			     const gchar *const name)
{
	DBusMessage *msg;

	if ((msg = dbus_message_new_signal(path, interface, name)) == NULL) {
		mce_log(LL_CRIT, "No memory for new signal!");
		g_main_loop_quit(mainloop);
		exit(EXIT_FAILURE);
	}

	return msg;
}

#if 0
/**
 * Create a new D-Bus error message, with proper error checking
 * will exit the mainloop if an error occurs
 *
 * @param message The DBusMessage that caused the error message to be sent
 * @param error The message to send
 * @return A new DBusMessage
 */
static DBusMessage *dbus_new_error(DBusMessage *const message,
				   const gchar *const error)
{
	DBusMessage *error_msg;

	if ((error_msg = dbus_message_new_error(message, error,
						NULL)) == NULL) {
		mce_log(LL_CRIT, "No memory for new D-Bus error message!");
		g_main_loop_quit(mainloop);
		exit(EXIT_FAILURE);
	}

	return error_msg;
}
#endif

/**
 * Create a new D-Bus method call, with proper error checking
 * will exit the mainloop if an error occurs
 *
 * @param service The method call service
 * @param path The method call path
 * @param interface The method call interface
 * @param name The name of the method to call
 * @return A new DBusMessage
 */
DBusMessage *dbus_new_method_call(const gchar *const service,
				  const gchar *const path,
				  const gchar *const interface,
				  const gchar *const name)
{
	DBusMessage *msg;

	if ((msg = dbus_message_new_method_call(service, path,
						interface, name)) == NULL) {
		mce_log(LL_CRIT,
			"Cannot allocate memory for D-Bus method call!");
		g_main_loop_quit(mainloop);
		exit(EXIT_FAILURE);
	}

	return msg;
}

/**
 * Create a new D-Bus method call reply, with proper error checking
 * will exit the mainloop if an error occurs
 *
 * @param message The DBusMessage to reply to
 * @return A new DBusMessage
 */
DBusMessage *dbus_new_method_reply(DBusMessage *const message)
{
	DBusMessage *msg;

	if ((msg = dbus_message_new_method_return(message)) == NULL) {
		mce_log(LL_CRIT, "No memory for new reply!");
		g_main_loop_quit(mainloop);
		exit(EXIT_FAILURE);
	}

	return msg;
}

/**
 * Send a D-Bus message
 * Side-effects: frees msg
 *
 * @param msg The D-Bus message to send
 * @return TRUE on success, FALSE on out of memory
 */
gboolean dbus_send_message(DBusMessage *const msg)
{
	gboolean status = FALSE;

	if (dbus_connection_send(dbus_connection, msg, NULL) == FALSE) {
		mce_log(LL_CRIT,
			"Out of memory when sending D-Bus message");
		goto EXIT;
	}

	dbus_connection_flush(dbus_connection);
	status = TRUE;

EXIT:
	dbus_message_unref(msg);

	return status;
}

/**
 * Send a D-Bus message and setup a reply callback
 * Side-effects: frees msg
 *
 * @param msg The D-Bus message to send
 * @param callback The reply callback
 * @return TRUE on success, FALSE on failure
 */
gboolean dbus_send_message_with_reply_handler(DBusMessage *const msg,
					      DBusPendingCallNotifyFunction callback)
{
	DBusPendingCall *pending_call;
	gboolean status = FALSE;

	if (dbus_connection_send_with_reply(dbus_connection, msg,
					    &pending_call, -1) == FALSE) {
		mce_log(LL_CRIT,
			"Out of memory when sending D-Bus message");
		goto EXIT;
	} else if (pending_call == NULL) {
		mce_log(LL_ERR,
			"D-Bus connection disconnected");
		goto EXIT;
	}

	dbus_connection_flush(dbus_connection);

	if (dbus_pending_call_set_notify(pending_call, callback, NULL, NULL) == FALSE) {
		mce_log(LL_CRIT,
			"Out of memory when sending D-Bus message");
		goto EXIT;
	}

	status = TRUE;

EXIT:
	dbus_message_unref(msg);

	return status;
}

/**
 * Generic function to send D-Bus messages and signals
 * to send a signal, call dbus_send with service == NULL
 *
 * @todo Make it possible to send D-Bus replies as well
 *
 * @param service D-Bus service; for signals, set to NULL
 * @param path D-Bus path
 * @param interface D-Bus interface
 * @param name The D-Bus method or signal name to send to
 * @param callback A reply callback, or NULL to set no reply;
 *                 for signals, this is unused, but please use NULL
 *                 for consistency
 * @param first_arg_type The DBUS_TYPE of the first argument in the list
 * @param ... The arguments to append to the D-Bus message;
 *            terminate with DBUS_TYPE_INVALID
 *            Note: the arguments MUST be passed by reference
 * @return TRUE on success, FALSE on failure
 */
gboolean dbus_send(const gchar *const service, const gchar *const path,
		   const gchar *const interface, const gchar *const name,
		   DBusPendingCallNotifyFunction callback,
		   int first_arg_type, ...)
{
	DBusMessage *msg;
	gboolean status = FALSE;
	va_list var_args;

	if (service != NULL) {
		msg = dbus_new_method_call(service, path, interface, name);

		if (callback == NULL)
			dbus_message_set_no_reply(msg, TRUE);
	} else {
		if (callback != NULL) {
			mce_log(LL_ERR,
				"Programmer snafu! "
				"dbus_send() called with a DBusPending "
				"callback for a signal.  Whoopsie!");
			callback = NULL;
		}

		msg = dbus_new_signal(path, interface, name);
	}

	/* Append the arguments, if any */
	va_start(var_args, first_arg_type);

	if (first_arg_type != DBUS_TYPE_INVALID) {
		if (dbus_message_append_args_valist(msg,
						    first_arg_type,
						    var_args) == FALSE) {
			mce_log(LL_CRIT,
				"Failed to append arguments to D-Bus message "
				"for %s.%s",
				interface, name);
			dbus_message_unref(msg);
			goto EXIT;
		}
	}

	/* Send the signal / call the method */
	if (callback == NULL) {
		status = dbus_send_message(msg);
	} else {
		status = dbus_send_message_with_reply_handler(msg, callback);
	}

EXIT:
	va_end(var_args);

	return status;
}

/**
 * Generic function to send D-Bus messages, blocking version
 *
 * @param service D-Bus service
 * @param path D-Bus path
 * @param interface D-Bus interface
 * @param name The D-Bus method to send to
 * @param timeout The reply timeout in milliseconds to use
 * @param first_arg_type The DBUS_TYPE of the first argument in the list
 * @param ... The arguments to append to the D-Bus message;
 *            terminate with DBUS_TYPE_INVALID
 *            Note: the arguments MUST be passed by reference
 * @return A new DBusMessage with the reply on success, NULL on failure
 */
DBusMessage *dbus_send_with_block(const gchar *const service,
				  const gchar *const path,
				  const gchar *const interface,
				  const gchar *const name,
				  gint timeout, int first_arg_type, ...)
{
	DBusMessage *reply = NULL;
	DBusMessage *msg = NULL;
	va_list var_args;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	msg = dbus_new_method_call(service, path, interface, name);

	/* Append the arguments, if any */
	va_start(var_args, first_arg_type);

	if (first_arg_type != DBUS_TYPE_INVALID) {
		if (dbus_message_append_args_valist(msg,
						    first_arg_type,
						    var_args) == FALSE) {
			mce_log(LL_CRIT,
				"Failed to append arguments to D-Bus message "
				"for %s.%s",
				interface, name);
			dbus_message_unref(msg);
			goto EXIT;
		}
	}

	/* Call the method */
	reply = dbus_connection_send_with_reply_and_block(dbus_connection, msg,								  timeout, &error);

	dbus_message_unref(msg);

	if (dbus_error_is_set(&error) == TRUE) {
		mce_log(LL_ERR,
			"Error sending with reply to %s.%s: %s",
			interface, name, error.message);
		dbus_error_free(&error);
		reply = NULL;
	}

EXIT:
	va_end(var_args);

	return reply;
}

/**
 * Translate a D-Bus bus name into a pid
 *
 * @param bus_name A string with the bus name
 * @return The pid of the process, or -1 if no process could be identified
 */
pid_t dbus_get_pid_from_bus_name(const gchar *const bus_name)
{
	dbus_uint32_t pid = -1;
	DBusMessage *reply;

	if ((reply = dbus_send_with_block("org.freedesktop.DBus",
				          "/org/freedesktop/DBus/Bus",
				          "org.freedesktop.DBus",
				          "GetConnectionUnixProcessID", -1,
				          DBUS_TYPE_STRING, &bus_name,
				          DBUS_TYPE_INVALID)) != NULL) {
		dbus_message_get_args(reply, NULL,
				      DBUS_TYPE_UINT32, &pid,
				      DBUS_TYPE_INVALID);
		dbus_message_unref(reply);
	}

	return (pid_t)pid;
}


/**
 * D-Bus callback for the version get method call
 *
 * @param msg The D-Bus message to reply to
 * @return TRUE on success, FALSE on failure
 */
static gboolean version_get_dbus_cb(DBusMessage *const msg)
{
	static const gchar *const versionstring = G_STRINGIFY(PRG_VERSION);
	DBusMessage *reply = NULL;
	gboolean status = FALSE;

	mce_log(LL_DEBUG, "Received version information request");

	/* Create a reply */
	reply = dbus_new_method_reply(msg);

	/* Append the version information */
	if (dbus_message_append_args(reply,
				     DBUS_TYPE_STRING, &versionstring,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append reply argument to D-Bus message "
			"for %s.%s",
			MCE_REQUEST_IF, MCE_VERSION_GET);
		dbus_message_unref(reply);
		goto EXIT;
	}

	/* Send the message */
	status = dbus_send_message(reply);

EXIT:
	return status;
}

/**
 * D-Bus message handler
 *
 * @param connection Unused
 * @param msg The D-Bus message received
 * @param user_data Unused
 * @return DBUS_HANDLER_RESULT_HANDLED for handled messages
 *         DBUS_HANDLER_RESULT_NOT_HANDLED for unhandled messages
 */
static DBusHandlerResult msg_handler(DBusConnection *const connection,
				     DBusMessage *const msg,
				     gpointer const user_data)
{
	guint status = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	GSList *list;

	(void)connection;
	(void)user_data;

	for (list = dbus_handlers; list != NULL; list = g_slist_next(list)) {
		handler_struct *handler = list->data;

		switch (handler->type) {
		case DBUS_MESSAGE_TYPE_METHOD_CALL:
			if (dbus_message_is_method_call(msg,
							handler->interface,
							handler->name) == TRUE) {
				handler->callback(msg);
				status = DBUS_HANDLER_RESULT_HANDLED;
				goto EXIT;
			}

			break;

		case DBUS_MESSAGE_TYPE_ERROR:
			if (dbus_message_is_error(msg,
						  handler->name) == TRUE) {
				handler->callback(msg);
				status = DBUS_HANDLER_RESULT_HANDLED;
				goto EXIT;
			}

			break;

		case DBUS_MESSAGE_TYPE_SIGNAL:
			if (dbus_message_is_signal(msg,
						   handler->interface,
						   handler->name) == TRUE) {
				handler->callback(msg);
				status = DBUS_HANDLER_RESULT_HANDLED;
			}

			break;

		default:
			mce_log(LL_ERR,
				"There's a bug somewhere in MCE; something "
				"has registered an invalid D-Bus handler");
			break;
		}
	}

EXIT:
	return status;
}

/**
 * Register a D-Bus signal or method handler
 *
 * @param interface The interface to listen on
 * @param name The signal/method call to listen for
 * @param rules Additional matching rules
 * @param type DBUS_MESSAGE_TYPE
 * @param callback The callback function
 * @return A D-Bus handler cookie on success, NULL on failure
 */
gconstpointer mce_dbus_handler_add(const gchar *const interface,
				    const gchar *const name,
				    const gchar *const rules,
				    const guint type,
				    gboolean (*callback)(DBusMessage *const msg))
{
	handler_struct *h = NULL;
	gchar *match = NULL;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	if (type == DBUS_MESSAGE_TYPE_SIGNAL) {
		if ((match = g_strdup_printf("type='signal'"
					     "%s%s%s"
					     ", member='%s'"
					     "%s%s",
					     interface ? ", interface='" : "",
					     interface ? interface : "",
					     interface ? "'" : "",
					     name,
					     rules ? ", " : "",
					     rules ? rules : "")) == NULL) {
			mce_log(LL_CRIT,
				"Failed to allocate memory for match");
			goto EXIT;
		}
	} else if (type != DBUS_MESSAGE_TYPE_METHOD_CALL) {
		mce_log(LL_CRIT,
			"There's definitely a programming error somewhere; "
			"MCE is trying to register an invalid message type");
		goto EXIT;
	}

	if ((h = g_try_malloc(sizeof (*h))) == NULL) {
		mce_log(LL_CRIT, "Failed to allocate memory for h");
		goto EXIT;
	}

	h->interface = NULL;

	if (interface && (h->interface = g_strdup(interface)) == NULL) {
		mce_log(LL_CRIT, "Failed to allocate memory for h->interface");
		g_free(h);
		h = NULL;
		goto EXIT;
	}

	h->rules = NULL;

	if (rules && (h->rules = g_strdup(rules)) == NULL) {
		mce_log(LL_CRIT, "Failed to allocate memory for h->rules");
		g_free(h->interface);
		g_free(h);
		h = NULL;
		goto EXIT;
	}

	if ((h->name = g_strdup(name)) == NULL) {
		mce_log(LL_CRIT, "Failed to allocate memory for h->name");
		g_free(h->interface);
		g_free(h->rules);
		g_free(h);
		h = NULL;
		goto EXIT;
	}

	h->type = type;
	h->callback = callback;

	/* Only register D-Bus matches for signals */
	if (match != NULL) {
		dbus_bus_add_match(dbus_connection, match, &error);

		if (dbus_error_is_set(&error) == TRUE) {
			mce_log(LL_CRIT,
				"Failed to add D-Bus match '%s' for '%s'; %s",
				match, h->interface, error.message);
			dbus_error_free(&error);
			g_free(h->interface);
			g_free(h->rules);
			g_free(h);
			h = NULL;
			goto EXIT;
		}
	}

	dbus_handlers = g_slist_prepend(dbus_handlers, h);

EXIT:
	g_free(match);

	return h;
}

/**
 * Unregister a D-Bus signal or method handler
 *
 * @param cookie A D-Bus handler cookie for
 *               the handler that should be removed
 */
void mce_dbus_handler_remove(gconstpointer cookie)
{
	handler_struct *h = (handler_struct *)cookie;
	gchar *match = NULL;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	if (h->type == DBUS_MESSAGE_TYPE_SIGNAL) {
		match = g_strdup_printf("type='signal'"
					"%s%s%s"
					", member='%s'"
					"%s%s",
					h->interface ? ", interface='" : "",
					h->interface ? h->interface : "",
					h->interface ? "'" : "",
					h->name,
					h->rules ? ", " : "",
					h->rules ? h->rules : "");

		if (match != NULL) {
			dbus_bus_remove_match(dbus_connection, match, &error);

			if (dbus_error_is_set(&error) == TRUE) {
				mce_log(LL_CRIT,
					"Failed to remove D-Bus match "
					"'%s' for '%s': %s",
					match, h->interface, error.message);
				dbus_error_free(&error);
			}
		} else {
			mce_log(LL_CRIT,
				"Failed to allocate memory for match");
		}
	} else if (h->type != DBUS_MESSAGE_TYPE_METHOD_CALL) {
		mce_log(LL_ERR,
			"There's definitely a programming error somewhere; "
			"MCE is trying to unregister an invalid message type");
		/* Don't abort here, since we want to unregister it anyway */
	}

	dbus_handlers = g_slist_remove(dbus_handlers, h);

	g_free(h->interface);
	g_free(h->rules);
	g_free(h->name);
	g_free(h);
	g_free(match);
}

/**
 * Unregister a D-Bus signal or method handler;
 * to be used with g_slist_foreach()
 *
 * @param handler A pointer to the handler struct that should be removed
 * @param user_data Unused
 */
static void mce_dbus_handler_remove_foreach(gpointer handler,
					    gpointer user_data)
{
	(void)user_data;

	mce_dbus_handler_remove(handler);
}

/**
 * Custom compare function used to find owner monitor entries
 *
 * @param owner_id An owner monitor cookie
 * @param name The name to search for
 * @return Less than, equal to, or greater than zero depending
 *         whether the name of the rules with the id owner_id
 *         is less than, equal to, or greater than name
 */
static gint monitor_compare(gconstpointer owner_id, gconstpointer name)
{
	handler_struct *hs = (handler_struct *)owner_id;

	return strcmp(hs->rules, name);
}

/**
 * Locate the specified D-Bus service in the monitor list
 *
 * @param service The service to check for
 * @param monitor_list The monitor list check
 * @return A pointer to the entry if the entry is in the list,
 *         NULL if the entry is not in the list
 */
static GSList *find_monitored_service(const gchar *service,
				      GSList *monitor_list)
{
	gchar *rule = NULL;
	GSList *tmp = NULL;

	if (service == NULL)
		goto EXIT;

	if ((rule = g_strdup_printf("arg1='%s'", service)) == NULL)
		goto EXIT;

	tmp = g_slist_find_custom(monitor_list, rule, monitor_compare);

	g_free(rule);

EXIT:
	return tmp;
}

/**
 * Check whether the D-Bus service in question is in the monitor list or not
 *
 * @param service The service to check for
 * @param monitor_list The monitor list check
 * @return TRUE if the entry is in the list,
 *         FALSE if the entry is not in the list
 */
gboolean mce_dbus_is_owner_monitored(const gchar *service,
				     GSList *monitor_list)
{
	return (find_monitored_service(service, monitor_list) != NULL);
}

/**
 * Add a service to a D-Bus owner monitor list
 *
 * @param service The service to monitor
 * @param callback A D-Bus monitor callback
 * @param monitor_list The list of monitored services
 * @param max_num The maximum number of monitored services;
 *                keep this number low, for performance
 *                and memory usage reasons
 * @return -1 if the amount of monitored services would be exceeded;
 *            if either of service or monitor_list is NULL,
 *            or if adding a D-Bus monitor fails
 *          0 if the service is already monitored
 *         >0 represents the number of monitored services after adding
 *            this service
 */
gssize mce_dbus_owner_monitor_add(const gchar *service,
				  gboolean (*callback)(DBusMessage *const msg),
				  GSList **monitor_list,
				  gssize max_num)
{
	gconstpointer cookie;
	gchar *rule = NULL;
	gssize retval = -1;
	gssize num;

	/* If service or monitor_list is NULL, fail */
	if (service == NULL) {
		mce_log(LL_CRIT,
			"A programming error occured; "
			"mce_dbus_owner_monitor_add() called with "
			"service == NULL");
		goto EXIT;
	} else if (monitor_list == NULL) {
		mce_log(LL_CRIT,
			"A programming error occured; "
			"mce_dbus_owner_monitor_add() called with "
			"monitor_list == NULL");
		goto EXIT;
	}

	/* If the service is already in the list, we're done */
	if (find_monitored_service(service, *monitor_list) != NULL) {
		retval = 0;
		goto EXIT;
	}

	/* If the service isn't in the list, and the list already
	 * contains max_num elements, bail out
	 */
	if ((num = g_slist_length(*monitor_list)) == max_num)
		goto EXIT;

	if ((rule = g_strdup_printf("arg1='%s'", service)) == NULL)
		goto EXIT;

	/* Add ownership monitoring for the service */
	cookie = mce_dbus_handler_add("org.freedesktop.DBus",
				      "NameOwnerChanged",
				      rule,
				      DBUS_MESSAGE_TYPE_SIGNAL,
				      callback);

	if (cookie == NULL)
		goto EXIT;

	*monitor_list = g_slist_prepend(*monitor_list, (gpointer)cookie);
	retval = num + 1;

EXIT:
	g_free(rule);

	return retval;
}

/**
 * Remove a service from a D-Bus owner monitor list
 *
 * @param service The service to remove from the monitor list
 * @param monitor_list The monitor list to remove the service from
 * @return The new number of monitored connections;
 *         -1 if the service was not monitored,
 *            if removing monitoring failed,
 *            or if either of service or monitor_list is NULL
 */
gssize mce_dbus_owner_monitor_remove(const gchar *service,
				     GSList **monitor_list)
{
	gssize retval = -1;
	GSList *tmp;

	/* If service or monitor_list is NULL, fail */
	if ((service == NULL) || (monitor_list == NULL))
		goto EXIT;

	/* If the service is not in the list, fail */
	if ((tmp = find_monitored_service(service, *monitor_list)) == NULL)
		goto EXIT;

	/* Remove ownership monitoring for the service */
	mce_dbus_handler_remove(tmp->data);
	*monitor_list = g_slist_remove(*monitor_list, tmp->data);
	retval = g_slist_length(*monitor_list);

EXIT:
	return retval;
}

/**
 * Remove all monitored service from a D-Bus owner monitor list
 *
 * @param monitor_list The monitor list to remove the service from
 */
void mce_dbus_owner_monitor_remove_all(GSList **monitor_list)
{
	if ((monitor_list != NULL) && (*monitor_list != NULL)) {
		g_slist_foreach(*monitor_list,
				(GFunc)mce_dbus_handler_remove_foreach, NULL);
		g_slist_free(*monitor_list);
		*monitor_list = NULL;
	}
}

/**
 * Acquire D-Bus services
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbus_acquire_services(void)
{
	gboolean status = FALSE;
	int ret;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	ret = dbus_bus_request_name(dbus_connection, MCE_SERVICE, 0, &error);

	if (ret == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		mce_log(LL_DEBUG, "Service %s acquired", MCE_SERVICE);
	} else {
		mce_log(LL_CRIT, "Cannot acquire service: %s", error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * Initialise the message handler used by MCE
 *
 * @return TRUE on success, FALSE on failure
 */
static gboolean dbus_init_message_handler(void)
{
	gboolean status = FALSE;

	if (dbus_connection_add_filter(dbus_connection, msg_handler,
				       NULL, NULL) == FALSE) {
		mce_log(LL_CRIT, "Failed to add D-Bus filter");
		goto EXIT;
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * Init function for the mce-dbus component
 * Pre-requisites: glib mainloop registered
 *
 * @param systembus TRUE to use system bus, FALSE to use session bus
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_dbus_init(const gboolean systembus)
{
	DBusBusType bus_type = DBUS_BUS_SYSTEM;
	gboolean status = FALSE;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	if (systembus == FALSE)
		bus_type = DBUS_BUS_SESSION;

	mce_log(LL_DEBUG, "Establishing D-Bus connection");

	/* Establish D-Bus connection */
	if ((dbus_connection = dbus_bus_get(bus_type,
					    &error)) == NULL) {
		mce_log(LL_CRIT, "Failed to open connection to message bus; %s",
			  error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	mce_log(LL_DEBUG, "Connecting D-Bus to the mainloop");

	/* Connect D-Bus to the mainloop */
	dbus_connection_setup_with_g_main(dbus_connection, NULL);

	mce_log(LL_DEBUG, "Acquiring D-Bus service");

	/* Acquire D-Bus service */
	if (dbus_acquire_services() == FALSE)
		goto EXIT;

	/* Initialise message handlers */
	if (dbus_init_message_handler() == FALSE)
		goto EXIT;

	/* Register callbacks that are handled inside mce-dbus.c */

	/* get_version */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_VERSION_GET,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 version_get_dbus_cb) == NULL)
		goto EXIT;

	status = TRUE;

EXIT:
	return status;
}

/**
 * Exit function for the mce-dbus component
 */
void mce_dbus_exit(void)
{
	/* Unregister D-Bus handlers */
	if (dbus_handlers != NULL) {
		g_slist_foreach(dbus_handlers,
				(GFunc)mce_dbus_handler_remove_foreach, NULL);
		g_slist_free(dbus_handlers);
		dbus_handlers = NULL;
	}

	/* If there is an established D-Bus connection, unreference it */
	if (dbus_connection != NULL) {
		mce_log(LL_DEBUG, "Unreferencing D-Bus connection");
		dbus_connection_unref(dbus_connection);
		dbus_connection = NULL;
	}

	return;
}
