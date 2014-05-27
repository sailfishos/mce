/**
 * @file mce-dbus.c
 * D-Bus handling code for the Mode Control Entity
 * <p>
 * Copyright © 2004-2009 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Ismo Laitinen <ismo.laitinen@nokia.com>
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

#include "mce-dbus.h"

#include "mce.h"
#include "mce-log.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#include <dbus/dbus-glib-lowlevel.h>

#include <mce/dbus-names.h>

/** Placeholder for any basic dbus data type */
typedef union
{
	dbus_int16_t i16;
	dbus_int32_t i32;
	dbus_int64_t i64;

	dbus_uint16_t u16;
	dbus_uint32_t u32;
	dbus_uint64_t u64;

	dbus_bool_t   b;
	unsigned char o;
	const char   *s;
	double        d;

} dbus_any_t;

/** Emit one iterm from dbus message iterator to file
 *
 * @param file output file
 * @param iter dbus message parse position
 *
 * @return TRUE if more items can be parsed, FALSE otherwise
 */
static dbus_bool_t
mce_dbus_message_repr_any(FILE *file, DBusMessageIter *iter)
{
	dbus_any_t val = { .u64 = 0 };
	DBusMessageIter sub;

	switch( dbus_message_iter_get_arg_type(iter) ) {
	case DBUS_TYPE_INVALID:
		return FALSE;
	default:
	case DBUS_TYPE_UNIX_FD:
		fprintf(file, " ???");
		return FALSE;
	case DBUS_TYPE_BYTE:
		dbus_message_iter_get_basic(iter, &val.o);
		fprintf(file, " byte:%d", val.o);
		break;
	case DBUS_TYPE_BOOLEAN:
		dbus_message_iter_get_basic(iter, &val.b);
		fprintf(file, " bool:%d", val.b);
		break;
	case DBUS_TYPE_INT16:
		dbus_message_iter_get_basic(iter, &val.i16);
		fprintf(file, " i16:%d", val.i16);
		break;
	case DBUS_TYPE_INT32:
		dbus_message_iter_get_basic(iter, &val.i32);
		fprintf(file, " i32:%d", val.i32);
		break;
	case DBUS_TYPE_INT64:
		dbus_message_iter_get_basic(iter, &val.i64);
		fprintf(file, " i64:%lld", (long long)val.i64);
		break;
	case DBUS_TYPE_UINT16:
		dbus_message_iter_get_basic(iter, &val.u16);
		fprintf(file, " u16:%u", val.u16);
		break;
	case DBUS_TYPE_UINT32:
		dbus_message_iter_get_basic(iter, &val.u32);
		fprintf(file, " u32:%u", val.u32);
		break;
	case DBUS_TYPE_UINT64:
		dbus_message_iter_get_basic(iter, &val.u64);
		fprintf(file, " u64:%llu", (unsigned long long)val.u64);
		break;
	case DBUS_TYPE_DOUBLE:
		dbus_message_iter_get_basic(iter, &val.d);
		fprintf(file, " dbl:%g", val.d);
		break;
	case DBUS_TYPE_STRING:
		dbus_message_iter_get_basic(iter, &val.s);
		fprintf(file, " str:\"%s\"", val.s);
		break;
	case DBUS_TYPE_OBJECT_PATH:
		dbus_message_iter_get_basic(iter, &val.s);
		fprintf(file, " obj:\"%s\"", val.s);
		break;
	case DBUS_TYPE_SIGNATURE:
		dbus_message_iter_get_basic(iter, &val.s);
		fprintf(file, " sgn:\"%s\"", val.s);
		break;
	case DBUS_TYPE_ARRAY:
		dbus_message_iter_recurse(iter, &sub);
		fprintf(file, " [");
		while( mce_dbus_message_repr_any(file, &sub) ) {}
		fprintf(file, " ]");
		break;
	case DBUS_TYPE_VARIANT:
		dbus_message_iter_recurse(iter, &sub);
		fprintf(file, " var");
		mce_dbus_message_repr_any(file, &sub);
		break;
	case DBUS_TYPE_STRUCT:
		dbus_message_iter_recurse(iter, &sub);
		fprintf(file, " {");
		while( mce_dbus_message_repr_any(file, &sub) ) {}
		fprintf(file, " }");
		break;
	case DBUS_TYPE_DICT_ENTRY:
		dbus_message_iter_recurse(iter, &sub);
		fprintf(file, " key");
		mce_dbus_message_repr_any(file, &sub);
		fprintf(file, " val");
		mce_dbus_message_repr_any(file, &sub);
		break;
	}

	return dbus_message_iter_next(iter);
}

/** Convert dbus message read iterator to string
 *
 * Caller must release returned string with free().
 *
 * @param iter dbus message iterator
 *
 * @returns representation of the iterator, or NULL
 */
char *
mce_dbus_message_iter_repr(DBusMessageIter *iter)
{
	size_t  size = 0;
	char   *data = 0;
	FILE   *file = open_memstream(&data, &size);

	if( !iter )
		goto EXIT;

	while( mce_dbus_message_repr_any(file, iter) ) {}
EXIT:
	fclose(file);
	return data;
}

/** Convert dbus message to string
 *
 * Caller must release returned string with free().
 *
 * @param msg dbus message
 *
 * @returns representation of the dbus message, or NULL
 */
char *
mce_dbus_message_repr(DBusMessage *const msg)
{
	size_t  size = 0;
	char   *data = 0;
	FILE   *file = open_memstream(&data, &size);

	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);
	int type = dbus_message_get_type(msg);
	const char *tname = dbus_message_type_to_string(type);
	const char *sender = dbus_message_get_sender(msg);

	fprintf(file, "%s", tname);

	if( sender ) fprintf(file, " from %s", sender);
	if( iface )  fprintf(file, " %s", iface);
	if( member ) fprintf(file, " %s", member);

	DBusMessageIter iter;
	dbus_message_iter_init(msg, &iter);
	if( dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INVALID )
		goto EXIT;

	fprintf(file, ":");

	while( mce_dbus_message_repr_any(file, &iter) ) {}
EXIT:
	fclose(file);
	return data;
}

/** Pointer to the DBusConnection */
static DBusConnection *dbus_connection = NULL;

/** List of all D-Bus handlers */
static GSList *dbus_handlers = NULL;

/** D-Bus handler callback function */
typedef gboolean (*handler_callback_t)(DBusMessage *const msg);

/** D-Bus handler structure */
typedef struct
{
	handler_callback_t  callback;   /**< Handler callback */
	gchar              *interface;  /**< The interface to listen on */
	gchar              *rules;      /**< Additional matching rules */
	gchar              *name;       /**< Method call or signal name */
	int                 type;       /**< DBUS_MESSAGE_TYPE */
} handler_struct_t;

/** Set handler type for D-Bus handler structure */
static inline void handler_struct_set_type(handler_struct_t *self, int val)
{
	self->type = val;
}

/** Set interface name for D-Bus handler structure */
static inline void handler_struct_set_interface(handler_struct_t *self, const char *val)
{
	g_free(self->interface), self->interface = val ? g_strdup(val) : 0;
}

/** Set member name for D-Bus handler structure */
static inline void handler_struct_set_name(handler_struct_t *self, const char *val)
{
	g_free(self->name), self->name = val ? g_strdup(val) : 0;
}

/** Set custom rules for D-Bus handler structure */
static inline void handler_struct_set_rules(handler_struct_t *self, const char *val)
{
	g_free(self->rules), self->rules = val ? g_strdup(val) : 0;
}

/** Set callback function for D-Bus handler structure */
static inline void handler_struct_set_callback(handler_struct_t *self,
					       handler_callback_t val)
{
	self->callback = val;
}

/** Release D-Bus handler structure */
static void handler_struct_delete(handler_struct_t *self)
{
	if( !self )
		goto EXIT;

	g_free(self->name);
	g_free(self->rules);
	g_free(self->interface);
	g_free(self);

EXIT:
	return;
}

/** Allocate D-Bus handler structure */
static handler_struct_t *handler_struct_create(void)
{
	handler_struct_t *self = g_malloc0(sizeof *self);

	self->interface = 0;
	self->rules     = 0;
	self->name      = 0;
	self->type      = DBUS_MESSAGE_TYPE_INVALID;
	return self;
}

/** Return reference to dbus connection cached at mce-dbus module
 *
 * For use in situations where the abstraction provided by mce-dbus
 * makes things too complicated.
 *
 * Caller must release non-null return values with dbus_connection_unref().
 *
 * @return DBusConnection, or NULL if mce has no dbus connection
 */
DBusConnection *dbus_connection_get(void)
{
	if( !dbus_connection ) {
		mce_log(LL_WARN, "no dbus connection");
		return NULL;
	}
	return dbus_connection_ref(dbus_connection);
}

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
		// FIXME: this is not how one should exit from mainloop
		mce_quit_mainloop();
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
		// FIXME: this is not how one should exit from mainloop
		mce_quit_mainloop();
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
		// FIXME: this is not how one should exit from mainloop
		mce_quit_mainloop();
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
		// FIXME: this is not how one should exit from mainloop
		mce_quit_mainloop();
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
 * @param user_data Data to pass to callback
 * @param user_free Delete callback for user_data
 * @return TRUE on success, FALSE on failure
 */
static gboolean
dbus_send_message_with_reply_handler(DBusMessage *const msg,
				     DBusPendingCallNotifyFunction callback,
				     void *user_data,
				     DBusFreeFunction user_free)
{
	gboolean         status = FALSE;
	DBusPendingCall *pc     = 0;

	if( !msg )
		goto EXIT;

	if( !dbus_connection_send_with_reply(dbus_connection, msg, &pc, -1) ) {
		mce_log(LL_CRIT, "Out of memory when sending D-Bus message");
		goto EXIT;
	}

	if( !pc ) {
		mce_log(LL_ERR, "D-Bus connection disconnected");
		goto EXIT;
	}

	// FIXME: do we really need the flush?
	dbus_connection_flush(dbus_connection);

	if( !dbus_pending_call_set_notify(pc, callback,
					  user_data, user_free) ) {
		mce_log(LL_CRIT, "Out of memory when sending D-Bus message");
		goto EXIT;
	}

	/* FIXME: After succesful set_notify the notification holds a ref
	 *        to the pending call and we could and should always unref
	 *        within this function. BUT, since the currently existing
	 *        callbacks do call unref, we can't do that before fixing
	 *        each and every one of them...
	 *
	 *        Instead we do not unref on success, i.e. after getting here
	 */
	pc = 0;

	/* Ownership of user_data passed on */
	user_free = 0, user_data = 0;

	status = TRUE;

EXIT:
	/* Release user_data if the ownership was not passed on */
	if( user_free )
		user_free(user_data);

	if( pc )
		dbus_pending_call_unref(pc);

	if( msg )
		dbus_message_unref(msg);

	return status;
}

static gboolean dbus_send_va(const char *service,
			     const char *path,
			     const char *interface,
			     const char *name,
			     DBusPendingCallNotifyFunction callback,
			     void *user_data, DBusFreeFunction user_free,
			     int first_arg_type, va_list va)
{
	gboolean     res = FALSE;
	DBusMessage *msg = 0;

	/* Method call or signal? */
	if( service ) {
		msg = dbus_new_method_call(service, path, interface, name);

		if( !callback )
			dbus_message_set_no_reply(msg, TRUE);
	}
	else if( callback ) {
		mce_log(LL_ERR, "Programmer snafu! "
			"dbus_send() called with a DBusPending "
			"callback for a signal.  Whoopsie!");
		goto EXIT;
	}
	else {
		msg = dbus_new_signal(path, interface, name);
	}

	/* Append the arguments, if any */
	if( first_arg_type != DBUS_TYPE_INVALID  &&
	    !dbus_message_append_args_valist(msg, first_arg_type, va)) {
		mce_log(LL_CRIT, "Failed to append arguments to D-Bus message "
			"for %s.%s", interface, name);
		goto EXIT;
	}

	/* Send the signal / call the method */
	if( !callback ) {
		res = dbus_send_message(msg);
		msg = 0;
	}
	else {
		res = dbus_send_message_with_reply_handler(msg, callback,
							   user_data,
							   user_free);
		msg = 0;

		/* Ownership of user_data passed on */
		user_data = 0, user_free = 0;
	}

EXIT:
	/* Release user_data if the ownership was not passed on */
	if( user_free )
		user_free(user_data);

	if( msg ) dbus_message_unref(msg);

	return res;

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
 * @param user_data Data to pass to callback
 * @param user_free Data release callback for user_data
 * @param first_arg_type The DBUS_TYPE of the first argument in the list
 * @param ... The arguments to append to the D-Bus message;
 *            terminate with DBUS_TYPE_INVALID
 *            Note: the arguments MUST be passed by reference
 * @return TRUE on success, FALSE on failure
 */
}

gboolean dbus_send_ex(const char *service,
		      const char *path,
		      const char *interface,
		      const char *name,
		      DBusPendingCallNotifyFunction callback,
		      void *user_data, DBusFreeFunction user_free,
		      int first_arg_type, ...)
{
	va_list va;
	va_start(va, first_arg_type);
	gboolean res = dbus_send_va(service, path, interface, name,
				    callback, user_data, user_free,
				    first_arg_type, va);
	va_end(va);
	return res;
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
	va_list va;
	va_start(va, first_arg_type);
	gboolean res = dbus_send_va(service, path, interface, name,
				    callback, 0, 0, first_arg_type, va);
	va_end(va);
	return res;
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
	reply = dbus_connection_send_with_reply_and_block(dbus_connection, msg,
							  timeout, &error);

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

/** Helper for appending gconf string list to dbus message
 *
 * @param conf GConfValue of string list type
 * @param pcount number of items in the returned array is stored here
 * @return array of string pointers that can be easily added to DBusMessage
 */
static const char **string_array_from_gconf_value(GConfValue *conf, int *pcount)
{
	const char **array = 0;
	int    count = 0;

	GSList *list, *item;

	if( conf->type != GCONF_VALUE_LIST )
		goto EXIT;

	if( gconf_value_get_list_type(conf) != GCONF_VALUE_STRING )
		goto EXIT;

	list = gconf_value_get_list(conf);

	for( item = list; item; item = item->next )
		++count;

	array = g_malloc_n(count, sizeof *array);
	count = 0;

	for( item = list; item; item = item->next )
		array[count++] = gconf_value_get_string(item->data);

EXIT:
	return *pcount = count, array;
}

/** Helper for appending gconf int list to dbus message
 *
 * @param conf GConfValue of int list type
 * @param pcount number of items in the returned array is stored here
 * @return array of integers that can be easily added to DBusMessage
 */
static dbus_int32_t *int_array_from_gconf_value(GConfValue *conf, int *pcount)
{
	dbus_int32_t *array = 0;
	int           count = 0;

	GSList *list, *item;

	if( conf->type != GCONF_VALUE_LIST )
		goto EXIT;

	if( gconf_value_get_list_type(conf) != GCONF_VALUE_INT )
		goto EXIT;

	list = gconf_value_get_list(conf);

	for( item = list; item; item = item->next )
		++count;

	array = g_malloc_n(count, sizeof *array);
	count = 0;

	for( item = list; item; item = item->next )
		array[count++] = gconf_value_get_int(item->data);

EXIT:
	return *pcount = count, array;
}

/** Helper for appending gconf bool list to dbus message
 *
 * @param conf GConfValue of bool list type
 * @param pcount number of items in the returned array is stored here
 * @return array of booleans that can be easily added to DBusMessage
 */
static dbus_bool_t *bool_array_from_gconf_value(GConfValue *conf, int *pcount)
{
	dbus_bool_t *array = 0;
	int          count = 0;

	GSList *list, *item;

	if( conf->type != GCONF_VALUE_LIST )
		goto EXIT;

	if( gconf_value_get_list_type(conf) != GCONF_VALUE_BOOL )
		goto EXIT;

	list = gconf_value_get_list(conf);

	for( item = list; item; item = item->next )
		++count;

	array = g_malloc_n(count, sizeof *array);
	count = 0;

	for( item = list; item; item = item->next )
		array[count++] = gconf_value_get_bool(item->data);

EXIT:
	return *pcount = count, array;
}

/** Helper for appending gconf float list to dbus message
 *
 * @param conf GConfValue of float list type
 * @param pcount number of items in the returned array is stored here
 * @return array of doubles that can be easily added to DBusMessage
 */
static double *float_array_from_gconf_value(GConfValue *conf, int *pcount)
{
	double *array = 0;
	int     count = 0;

	GSList *list, *item;

	if( conf->type != GCONF_VALUE_LIST )
		goto EXIT;

	if( gconf_value_get_list_type(conf) != GCONF_VALUE_FLOAT )
		goto EXIT;

	list = gconf_value_get_list(conf);

	for( item = list; item; item = item->next )
		++count;

	array = g_malloc_n(count, sizeof *array);
	count = 0;

	for( item = list; item; item = item->next )
		array[count++] = gconf_value_get_float(item->data);

EXIT:
	return *pcount = count, array;
}

/** Helper for deducing what kind of array signature we need for a list value
 *
 * @param type Non-complex gconf value type
 *
 * @return D-Bus signature needed for adding given type to a container
 */
static const char *type_signature(GConfValueType type)
{
	switch( type ) {
	case GCONF_VALUE_STRING: return DBUS_TYPE_STRING_AS_STRING;
	case GCONF_VALUE_INT:    return DBUS_TYPE_INT32_AS_STRING;
	case GCONF_VALUE_FLOAT:  return DBUS_TYPE_DOUBLE_AS_STRING;
	case GCONF_VALUE_BOOL:   return DBUS_TYPE_BOOLEAN_AS_STRING;
	default: break;
	}
	return 0;
}

/** Helper for deducing what kind of variant signature we need for a value
 *
 * @param conf GConf value
 *
 * @return D-Bus signature needed for adding given value to a container
 */
static const char *value_signature(GConfValue *conf)
{
	if( conf->type != GCONF_VALUE_LIST ) {
		return type_signature(conf->type);
	}

	switch( gconf_value_get_list_type(conf) ) {
	case GCONF_VALUE_STRING:
		return DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_STRING_AS_STRING;
	case GCONF_VALUE_INT:
		return DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_INT32_AS_STRING;
	case GCONF_VALUE_FLOAT:
		return DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_DOUBLE_AS_STRING;
	case GCONF_VALUE_BOOL:
		return DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BOOLEAN_AS_STRING;
	default: break;
	}

	return 0;
}

/** Helper for appending GConfValue to dbus message
 *
 * @param reply DBusMessage under construction
 * @param conf GConfValue to be added to the reply
 *
 * @return TRUE if the value was succesfully appended, or FALSE on failure
 */
static gboolean append_gconf_value_to_dbus_message(DBusMessage *reply, GConfValue *conf)
{
	const char *sig = 0;

	DBusMessageIter body, variant, array;

	if( !(sig = value_signature(conf)) ) {
		goto bailout_message;
	}

	dbus_message_iter_init_append(reply, &body);

	if( !dbus_message_iter_open_container(&body, DBUS_TYPE_VARIANT,
					      sig, &variant) ) {
		goto bailout_message;
	}

	switch( conf->type ) {
	case GCONF_VALUE_STRING:
		{
			const char *arg = gconf_value_get_string(conf) ?: "";
			dbus_message_iter_append_basic(&variant,
						       DBUS_TYPE_STRING,
						       &arg);
		}
		break;

	case GCONF_VALUE_INT:
		{
			dbus_int32_t arg = gconf_value_get_int(conf);
			dbus_message_iter_append_basic(&variant,
						       DBUS_TYPE_INT32,
						       &arg);
		}
		break;

	case GCONF_VALUE_FLOAT:
		{
			double arg = gconf_value_get_float(conf);
			dbus_message_iter_append_basic(&variant,
						       DBUS_TYPE_DOUBLE,
						       &arg);
		}
		break;

	case GCONF_VALUE_BOOL:
		{
			dbus_bool_t arg = gconf_value_get_bool(conf);
			dbus_message_iter_append_basic(&variant,
						       DBUS_TYPE_BOOLEAN,
						       &arg);
		}
		break;

	case GCONF_VALUE_LIST:
		if( !(sig = type_signature(gconf_value_get_list_type(conf))) ) {
			goto bailout_variant;
		}

		if( !dbus_message_iter_open_container(&variant,
						      DBUS_TYPE_ARRAY,
						      sig, &array) ) {
			goto bailout_variant;
		}

		switch( gconf_value_get_list_type(conf) ) {
		case GCONF_VALUE_STRING:
			{
				int          cnt = 0;
				const char **arg = string_array_from_gconf_value(conf, &cnt);
				for( int i = 0; i < cnt; ++i ) {
					const char *str = arg[i];
					dbus_message_iter_append_basic(&array,
								       DBUS_TYPE_STRING,
								       &str);
				}
				g_free(arg);
			}
			break;
		case GCONF_VALUE_INT:
			{
				int           cnt = 0;
				dbus_int32_t *arg = int_array_from_gconf_value(conf, &cnt);
				dbus_message_iter_append_fixed_array(&array,
								     DBUS_TYPE_INT32,
								     &arg, cnt);
				g_free(arg);
			}
			break;
		case GCONF_VALUE_FLOAT:
			{
				int     cnt = 0;
				double *arg = float_array_from_gconf_value(conf, &cnt);
				dbus_message_iter_append_fixed_array(&array,
								     DBUS_TYPE_DOUBLE,
								     &arg, cnt);
				g_free(arg);
			}
			break;
		case GCONF_VALUE_BOOL:
			{
				int          cnt = 0;
				dbus_bool_t *arg = bool_array_from_gconf_value(conf, &cnt);
				dbus_message_iter_append_fixed_array(&array,
								     DBUS_TYPE_BOOLEAN,
								     &arg, cnt);
				g_free(arg);
			}
			break;

		default:
			goto bailout_array;
		}

		if( !dbus_message_iter_close_container(&variant, &array) ) {
			goto bailout_variant;
		}
		break;

	default:
		goto bailout_variant;
	}

	if( !dbus_message_iter_close_container(&body, &variant) ) {
		goto bailout_message;
	}
	return TRUE;

bailout_array:
	dbus_message_iter_abandon_container(&variant, &array);

bailout_variant:
	dbus_message_iter_abandon_container(&body, &variant);

bailout_message:
	return FALSE;
}

/* FIXME: Once the constants are in mce-dev these can be removed */
#ifndef MCE_CONFIG_GET
# define MCE_CONFIG_GET         "get_config"
# define MCE_CONFIG_SET         "set_config"
# define MCE_CONFIG_CHANGE_SIG  "config_change_ind"
#endif

/**
 * D-Bus callback for the config get method call
 *
 * @param msg The D-Bus message to reply to
 *
 * @return TRUE if reply message was successfully sent, FALSE on failure
 */
static gboolean config_get_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;
	DBusMessage *reply = NULL;
	const char *key = NULL;
	GError *err = NULL;
	GConfValue *conf = 0;

	DBusMessageIter body;

	mce_log(LL_DEBUG, "Received configuration query request");

	dbus_message_iter_init(msg, &body);

	/* HACK: The key used to be object path, not string.
	 *       Allow clients to use either one. */
	switch( dbus_message_iter_get_arg_type(&body) ) {
	case DBUS_TYPE_OBJECT_PATH:
	case DBUS_TYPE_STRING:
		dbus_message_iter_get_basic(&body, &key);
		dbus_message_iter_next(&body);
		break;

	default:
		reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
					       "expected string/object path");
		goto EXIT;
	}

	if( !(conf = gconf_client_get(gconf_client_get_default(), key, &err)) ) {
		reply = dbus_message_new_error(msg,
					       "com.nokia.mce.GConf.Error",
					       err->message ?: "unknown");
		goto EXIT;
	}

	if( !(reply = dbus_new_method_reply(msg)) )
		goto EXIT;

	if( !append_gconf_value_to_dbus_message(reply, conf) ) {
		dbus_message_unref(reply);
		reply = dbus_message_new_error(msg,
					       "com.nokia.mce.GConf.Error",
					       "constructing reply failed");
	}

EXIT:
	/* Send a reply if we have one */
	if( reply ) {
		if( dbus_message_get_no_reply(msg) ) {
			dbus_message_unref(reply), reply = 0;
			status = TRUE;
		}
		else {
			/* dbus_send_message unrefs the reply message */
			status = dbus_send_message(reply), reply = 0;
		}
	}

	if( conf )
		gconf_value_free(conf);

	g_clear_error(&err);

	return status;
}

/** Send configuration changed notification signal
 *
 * @param entry changed setting
 */
void mce_dbus_send_config_notification(GConfEntry *entry)
{
	const char  *key = 0;
	GConfValue  *val = 0;
	DBusMessage *sig = 0;

	if( !entry )
		goto EXIT;

	if( !(key = gconf_entry_get_key(entry)) )
		goto EXIT;

	if( !(val = gconf_entry_get_value(entry)) )
		goto EXIT;

	mce_log(LL_DEBUG, "%s: changed", key);

	sig = dbus_message_new_signal(MCE_SIGNAL_PATH,
				      MCE_SIGNAL_IF,
				      MCE_CONFIG_CHANGE_SIG);

	if( !sig ) goto EXIT;

	dbus_message_append_args(sig,
				 DBUS_TYPE_STRING, &key,
				 DBUS_TYPE_INVALID);

	append_gconf_value_to_dbus_message(sig, val);

	dbus_send_message(sig), sig = 0;

EXIT:

	if( sig ) dbus_message_unref(sig);

	return;
}

/** Release GSList of GConfValue objects
 *
 * @param list GSList where item->data members are pointers to GConfValue
 */
static void value_list_free(GSList *list)
{
	g_slist_free_full(list, (GDestroyNotify)gconf_value_free);
}

/** Convert D-Bus string array into GSList of GConfValue objects
 *
 * @param iter D-Bus message iterator at DBUS_TYPE_ARRAY
 * @return GSList where item->data members are pointers to GConfValue
 */
static GSList *value_list_from_string_array(DBusMessageIter *iter)
{
	GSList *res = 0;

	DBusMessageIter subiter;

	dbus_message_iter_recurse(iter, &subiter);

	int i = 0;
	while ( dbus_message_iter_get_arg_type(&subiter) == DBUS_TYPE_STRING ) {
		const char *tmp = 0;
		dbus_message_iter_get_basic(&subiter, &tmp);
		dbus_message_iter_next(&subiter);

		mce_log(LL_INFO, "arr[%d] = string:%s", i++, tmp);

		GConfValue *value = gconf_value_new(GCONF_VALUE_STRING);
		gconf_value_set_string(value, tmp);
		res = g_slist_prepend(res, value);
	}

	res = g_slist_reverse(res);

	return res;
}

/** Convert D-Bus int32 array into GSList of GConfValue objects
 *
 * @param iter D-Bus message iterator at DBUS_TYPE_ARRAY
 * @return GSList where item->data members are pointers to GConfValue
 */
static GSList *value_list_from_int_array(DBusMessageIter *iter)
{
	GSList *res = 0;

	DBusMessageIter subiter;

	dbus_message_iter_recurse(iter, &subiter);

	int i = 0;
	while ( dbus_message_iter_get_arg_type(&subiter) == DBUS_TYPE_INT32 ) {
		dbus_int32_t tmp = 0;
		dbus_message_iter_get_basic(&subiter, &tmp);
		dbus_message_iter_next(&subiter);

		mce_log(LL_INFO, "arr[%d] = int:%d", i++, tmp);

		GConfValue *value = gconf_value_new(GCONF_VALUE_INT);
		gconf_value_set_int(value, tmp);
		res = g_slist_prepend(res, value);
	}

	res = g_slist_reverse(res);

	return res;
}

/** Convert D-Bus bool array into GSList of GConfValue objects
 *
 * @param iter D-Bus message iterator at DBUS_TYPE_ARRAY
 * @return GSList where item->data members are pointers to GConfValue
 */
static GSList *value_list_from_bool_array(DBusMessageIter *iter)
{
	GSList *res = 0;

	DBusMessageIter subiter;

	dbus_message_iter_recurse(iter, &subiter);

	int i = 0;
	while ( dbus_message_iter_get_arg_type(&subiter) == DBUS_TYPE_BOOLEAN ) {
		dbus_bool_t tmp = 0;
		dbus_message_iter_get_basic(&subiter, &tmp);
		dbus_message_iter_next(&subiter);

		mce_log(LL_INFO, "arr[%d] = bool:%s", i++, tmp ? "true" : "false");

		GConfValue *value = gconf_value_new(GCONF_VALUE_BOOL);
		gconf_value_set_bool(value, tmp);
		res = g_slist_prepend(res, value);
	}

	res = g_slist_reverse(res);

	return res;
}

/** Convert D-Bus double array into GSList of GConfValue objects
 *
 * @param iter D-Bus message iterator at DBUS_TYPE_ARRAY
 * @return GSList where item->data members are pointers to GConfValue
 */
static GSList *value_list_from_float_array(DBusMessageIter *iter)
{
	GSList *res = 0;

	DBusMessageIter subiter;

	dbus_message_iter_recurse(iter, &subiter);

	int i = 0;
	while ( dbus_message_iter_get_arg_type(&subiter) == DBUS_TYPE_DOUBLE ) {
		double tmp = 0;
		dbus_message_iter_get_basic(&subiter, &tmp);
		dbus_message_iter_next(&subiter);

		mce_log(LL_INFO, "arr[%d] = float:%g", i++, tmp);

		GConfValue *value = gconf_value_new(GCONF_VALUE_FLOAT);
		gconf_value_set_float(value, tmp);
		res = g_slist_prepend(res, value);
	}

	res = g_slist_reverse(res);

	return res;
}

/**
 * D-Bus callback for the config set method call
 *
 * @param msg The D-Bus message to reply to
 *
 * @return TRUE if reply message was successfully sent, FALSE on failure
 */
static gboolean config_set_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;
	DBusMessage *reply = NULL;
	const char *key = NULL;
	GError *err = NULL;
	GConfClient *client = 0;
	GSList *list = 0;

	DBusError error = DBUS_ERROR_INIT;
	DBusMessageIter body, iter;

	mce_log(LL_DEBUG, "Received configuration change request");

	if( !(client = gconf_client_get_default()) )
		goto EXIT;

	dbus_message_iter_init(msg, &body);

	/* HACK: The key used to be object path, not string.
	 *       Allow clients to use either one. */
	switch( dbus_message_iter_get_arg_type(&body) ) {
	case DBUS_TYPE_OBJECT_PATH:
	case DBUS_TYPE_STRING:
		dbus_message_iter_get_basic(&body, &key);
		dbus_message_iter_next(&body);
		break;

	default:
		reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
					       "expected string/object path");
		goto EXIT;
	}

	if( dbus_message_iter_get_arg_type(&body) == DBUS_TYPE_VARIANT ) {
		dbus_message_iter_recurse(&body, &iter);
		dbus_message_iter_next(&body);
	}
	else if( dbus_message_iter_get_arg_type(&body) == DBUS_TYPE_ARRAY ) {
		/* HACK: dbus-send does not know how to handle nested
		 * containers,  so it can't be used to send variant
		 * arrays 'variant:array:int32:1,2,3', so we allow array
		 * requrest without variant too ... */
		iter = body;
	}
	else {
		reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
					       "expected variant");
		goto EXIT;
	}

	switch( dbus_message_iter_get_arg_type(&iter) ) {
	case DBUS_TYPE_BOOLEAN:
		{
			dbus_bool_t arg = 0;
			dbus_message_iter_get_basic(&iter, &arg);
			gconf_client_set_bool(client, key, arg, &err);
		}
		break;
	case DBUS_TYPE_INT32:
		{
			dbus_int32_t arg = 0;
			dbus_message_iter_get_basic(&iter, &arg);
			gconf_client_set_int(client, key, arg, &err);
		}
		break;
	case DBUS_TYPE_DOUBLE:
		{
			double arg = 0;
			dbus_message_iter_get_basic(&iter, &arg);
			gconf_client_set_float(client, key, arg, &err);
		}
		break;
	case DBUS_TYPE_STRING:
		{
			const char *arg = 0;
			dbus_message_iter_get_basic(&iter, &arg);
			gconf_client_set_string(client, key, arg, &err);
		}
		break;

	case DBUS_TYPE_ARRAY:
		switch( dbus_message_iter_get_element_type(&iter) ) {
		case DBUS_TYPE_BOOLEAN:
			list = value_list_from_bool_array(&iter);
			gconf_client_set_list(client, key, GCONF_VALUE_BOOL, list, &err);
			break;
		case DBUS_TYPE_INT32:
			list = value_list_from_int_array(&iter);
			gconf_client_set_list(client, key, GCONF_VALUE_INT, list, &err);
			break;
		case DBUS_TYPE_DOUBLE:
			list = value_list_from_float_array(&iter);
			gconf_client_set_list(client, key, GCONF_VALUE_FLOAT, list, &err);
			break;
		case DBUS_TYPE_STRING:
			list = value_list_from_string_array(&iter);
			gconf_client_set_list(client, key, GCONF_VALUE_STRING, list, &err);
			break;
		default:
			reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
						       "unexpected value array type");
			goto EXIT;

		}
		break;

	default:
		reply = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
					       "unexpected value type");
		goto EXIT;
	}

	if( err )
	{
		/* some of the above gconf_client_set_xxx() calls failed */
		reply = dbus_message_new_error(msg,
					       "com.nokia.mce.GConf.Error",
					       err->message ?: "unknown");
		goto EXIT;
	}

	/* we changed something */
	gconf_client_suggest_sync(client, &err);
	if( err ) {
		mce_log(LL_ERR, "gconf_client_suggest_sync: %s", err->message);
	}

	if( !(reply = dbus_new_method_reply(msg)) )
		goto EXIT;

	/* it is either error reply or true, and we got here... */
	{
		dbus_bool_t arg = TRUE;
		dbus_message_append_args(reply,
					 DBUS_TYPE_BOOLEAN, &arg,
					 DBUS_TYPE_INVALID);
	}

EXIT:
	value_list_free(list);

	/* Send a reply if we have one */
	if( reply ) {
		if( dbus_message_get_no_reply(msg) ) {
			dbus_message_unref(reply), reply = 0;
			status = TRUE;
		}
		else {
			/* dbus_send_message unrefs the reply message */
			status = dbus_send_message(reply), reply = 0;
		}
	}

	g_clear_error(&err);
	dbus_error_free(&error);

	return status;
}

/**
 * D-Bus rule checker
 *
 * @param msg The D-Bus message being checked
 * @param rules The rule string to check against
 * @return TRUE if message matches the rules,
	   FALSE if not
 */
static gboolean check_rules(DBusMessage *const msg,
			    const char *rules)
{
	if (rules == NULL)
		return TRUE;
	rules += strspn(rules, " ");;

	while (*rules != '\0') {
		const char *eq;
		const char *value;
		const char *value_end;
		const char *val = NULL;
		gboolean quot = FALSE;

		if ((eq = strchr(rules, '=')) == NULL)
			return FALSE;
		eq += strspn(eq, " ");

		if (eq[1] == '\'') {
			value = eq + 2;
			value_end = strchr(value, '\'');
			quot = TRUE;
		} else {
			value = eq + 1;
			value_end = strchrnul(value, ',');
		}

		if (value_end == NULL)
			return FALSE;

		if (strncmp(rules, "arg", 3) == 0) {
			int fld = atoi(rules + 3);

			DBusMessageIter iter;

			if (dbus_message_iter_init(msg, &iter) == FALSE)
				return FALSE;

			for (; fld; fld--) {
				if (dbus_message_iter_has_next(&iter) == FALSE)
					return FALSE;
				dbus_message_iter_next(&iter);
			}

			if (dbus_message_iter_get_arg_type(&iter) !=
			    DBUS_TYPE_STRING)
				return FALSE;
			dbus_message_iter_get_basic(&iter, &val);

		} else if (strncmp(rules, "path", 4) == 0) {
			val = dbus_message_get_path(msg);
		}

		if (((value_end != NULL) &&
		     ((strncmp(value, val, value_end - value) != 0) ||
		      (val[value_end - value] != '\0'))) ||
		    ((value_end == NULL) &&
		     (strcmp(value, val) != 0)))
			return FALSE;

		if (value_end == NULL)
			break;

		rules = value_end + (quot == TRUE ? 1 : 0);
		rules += strspn(rules, " ");;

		if (*rules == ',')
			rules++;
		rules += strspn(rules, " ");;
	}

	return TRUE;
}

/** Build a dbus signal match string
 *
 * For use from mce_dbus_handler_add() and mce_dbus_handler_remove()
 */
static gchar *mce_dbus_build_signal_match(const gchar *interface,
					  const gchar *name,
					  const gchar *rules)
{
	gchar *match = 0;

	gchar *match_member = 0;
	gchar *match_iface  = 0;
	gchar *match_extra  = 0;

	if( name )
		match_member = g_strdup_printf(",member='%s'", name);

	if( interface )
		match_iface = g_strdup_printf(",interface='%s'", interface);

	if( rules )
		match_extra = g_strdup_printf(",%s", rules);

	match = g_strdup_printf("type='signal'%s%s%s",
				match_iface  ?: "",
				match_member ?: "",
				match_extra  ?: "");
	g_free(match_extra);
	g_free(match_iface);
	g_free(match_member);

	return match;
}

/** Remove links with NULL data from GSList */
static void mce_dbus_squeeze_slist(GSList **list)
{
	GSList *now, *zen;

	if( !list || !*list )
		goto EXIT;

	/* Move null links to trash, keep the rest in the original order
	 *
	 * Note: This is now one pass O(N) complexity. Using the singly
	 *       linked list api from glib would not allow that. */

	GSList *trash = 0;

	for( now = *list; now; now = zen ) {
		zen = now->next;
		if( now->data )
			*list = now, list = &now->next;
		else
			now->next = trash, trash = now;
	}
	*list = 0;

	/* Release the empty slices */
	g_slist_free(trash);

EXIT:

	return;
}

/** Check if message attribute value matches handler attribute value
 *
 * @return true on match, false otherwise
 */
static bool mce_dbus_match(const char *msg_val, const char *hnd_val)
{
	/* Special case 1: If message attribute has null value,
	 *                 no handler value can be a match */
	if( !msg_val )
		return false;

	/* Special case 2: If handler attribyte has null value,
	 *                 it mathes any non-null message value. */
	if( !hnd_val )
		return true;

	/* Normally we just test for string equality */
	return !strcmp(msg_val, hnd_val);
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
	(void)connection;
	(void)user_data;

	guint status = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	int   type   = dbus_message_get_type(msg);

	const char *interface = dbus_message_get_interface(msg);
	const char *member    = dbus_message_get_member(msg);

	for( GSList *now = dbus_handlers; now; now = now->next ) {

		handler_struct_t *handler = now->data;

		/* Skip half removed handlers */
		if( !handler )
			continue;

		/* Skip not applicable handlers */
		if( handler->type != type )
			continue;

		switch( handler->type ) {
		case DBUS_MESSAGE_TYPE_METHOD_CALL:
			if( !mce_dbus_match(interface, handler->interface) )
				break;

			if( !mce_dbus_match(member, handler->name) )
				break;

			handler->callback(msg);
			status = DBUS_HANDLER_RESULT_HANDLED;
			goto EXIT;

		case DBUS_MESSAGE_TYPE_ERROR:
			if( !mce_dbus_match(member, handler->name) )
				break;

			handler->callback(msg);
			break;

		case DBUS_MESSAGE_TYPE_SIGNAL:
			if( !mce_dbus_match(interface, handler->interface) )
				break;

			if( !mce_dbus_match(member, handler->name) )
				break;

			if( !check_rules(msg, handler->rules) )
				break;

			handler->callback(msg);
			break;

		default:
			mce_log(LL_ERR, "There's a bug somewhere in MCE; something "
				"has registered an invalid D-Bus handler");
			break;
		}
	}

	/* Purge half removed handlers */
	mce_dbus_squeeze_slist(&dbus_handlers);

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
	handler_struct_t *handler = 0;
	gchar            *match   = 0;

	if( !interface ) {
		mce_log(LL_CRIT, "D-Bus handler must specify interface");
		goto EXIT;
	}

	switch( type ) {
	case DBUS_MESSAGE_TYPE_SIGNAL:
		match = mce_dbus_build_signal_match(interface, name, rules);
		if( !match ) {
			mce_log(LL_CRIT, "Failed to allocate memory for match");
			goto EXIT;
		}
		break;

	case DBUS_MESSAGE_TYPE_METHOD_CALL:
		if( !name ) {
			mce_log(LL_CRIT, "D-Bus method call handler must specify name");
			goto EXIT;
		}
		break;

	default:
		mce_log(LL_CRIT, "There's definitely a programming error somewhere; "
			"MCE is trying to register an invalid message type");
		goto EXIT;
	}

	handler = handler_struct_create();
	handler_struct_set_type(handler, type);
	handler_struct_set_interface(handler, interface);
	handler_struct_set_name(handler, name);
	handler_struct_set_rules(handler, rules);
	handler_struct_set_callback(handler, callback);

	/* Only register D-Bus matches for signals */
	if( match )
		dbus_bus_add_match(dbus_connection, match, 0);

	dbus_handlers = g_slist_prepend(dbus_handlers, handler);

EXIT:
	g_free(match);

	return handler;
}

/**
 * Unregister a D-Bus signal or method handler
 *
 * @param cookie A D-Bus handler cookie for
 *               the handler that should be removed
 */
void mce_dbus_handler_remove(gconstpointer cookie)
{
	handler_struct_t *handler = (handler_struct_t *)cookie;
	gchar            *match   = 0;
	GSList           *item    = 0;

	if( !handler )
		goto EXIT;

	if( !(item = g_slist_find(dbus_handlers, handler)) ) {
		mce_log(LL_CRIT, "removing unregistered dbus handler");
	}
	else {
		/* Detach from containing list. The list itself is
		 * not modified so that possible ongoing iteration
		 * is not adversely affected. List cleanup happens
		 * at msg_handler() and mce_dbus_exit().
		 */
		item->data = 0;
	}

	if( handler->type == DBUS_MESSAGE_TYPE_SIGNAL ) {
		match = mce_dbus_build_signal_match(handler->interface,
						    handler->name,
						    handler->rules);

		if( !match ) {
			mce_log(LL_CRIT, "Failed to allocate memory for match");
		}
		else if( dbus_connection_get_is_connected(dbus_connection) ) {
			dbus_bus_remove_match(dbus_connection, match, 0);
		}
	}
	else if( handler->type != DBUS_MESSAGE_TYPE_METHOD_CALL ) {
		mce_log(LL_ERR, "There's definitely a programming error somewhere; "
			"MCE is trying to unregister an invalid message type");
		/* Don't abort here, since we want to unregister it anyway */
	}

	handler_struct_delete(handler);

EXIT:
	g_free(match);
}

/**
 * Unregister a D-Bus signal or method handler;
 * to be used with g_slist_foreach()
 *
 * @param handler A pointer to the handler struct that should be removed
 * @param user_data Unused
 */
static void mce_dbus_handler_remove_cb(gpointer handler, gpointer user_data)
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
	handler_struct_t *hs = (handler_struct_t *)owner_id;

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

	/* Signal content: arg0=name, arg1=old owner, arg2=new owner.
	 * In This case  we want to track loss of name owner. */
	if ((rule = g_strdup_printf("arg0='%s',arg2=''", service)) == NULL)
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
 * Generate and handle fake owner gone message
 *
 * @param data Name of owner that is gone
 * @return Always FALSE
 */
static gboolean fake_owner_gone(gpointer data)
{
	DBusMessage *msg;
	const char *empty = "";

	msg = dbus_message_new_signal("/org/freedesktop/DBus",
				      "org.freedesktop.DBus",
				      "NameOwnerChanged");
	dbus_message_append_args(msg, DBUS_TYPE_STRING, &data,
				 DBUS_TYPE_STRING, &data,
				 DBUS_TYPE_STRING, &empty,
				 DBUS_TYPE_INVALID);

	msg_handler(NULL, msg, NULL);

	dbus_message_unref(msg), msg = 0;

	return FALSE;
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

	/* Signal content: arg0=name, arg1=old owner, arg2=new owner.
	 * In This case  we want to track loss of name owner. */
	if ((rule = g_strdup_printf("arg0='%s',arg2=''", service)) == NULL)
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

	// FIXME: does synchronous roundtrip to dbus-daemon and back
	if (dbus_bus_name_has_owner(dbus_connection, service, NULL) == FALSE)
		g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
				fake_owner_gone, g_strdup(service),
				g_free);

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
	if( monitor_list && *monitor_list ) {
		g_slist_foreach(*monitor_list, mce_dbus_handler_remove_cb, 0);
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

/** Get dbus data type to human readable form
 *
 * @param type type id (DBUS_TYPE_BOOLEAN etc)
 *
 * @return name of the type, without the DBUS_TYPE_ prefix
 */
const char *
mce_dbus_type_repr(int type)
{
	const char *res = "UNKNOWN";
	switch( type ) {
	case DBUS_TYPE_INVALID:     res = "INVALID";     break;
	case DBUS_TYPE_BYTE:        res = "BYTE";        break;
	case DBUS_TYPE_BOOLEAN:     res = "BOOLEAN";     break;
	case DBUS_TYPE_INT16:       res = "INT16";       break;
	case DBUS_TYPE_UINT16:      res = "UINT16";      break;
	case DBUS_TYPE_INT32:       res = "INT32";       break;
	case DBUS_TYPE_UINT32:      res = "UINT32";      break;
	case DBUS_TYPE_INT64:       res = "INT64";       break;
	case DBUS_TYPE_UINT64:      res = "UINT64";      break;
	case DBUS_TYPE_DOUBLE:      res = "DOUBLE";      break;
	case DBUS_TYPE_STRING:      res = "STRING";      break;
	case DBUS_TYPE_OBJECT_PATH: res = "OBJECT_PATH"; break;
	case DBUS_TYPE_SIGNATURE:   res = "SIGNATURE";   break;
	case DBUS_TYPE_UNIX_FD:     res = "UNIX_FD";     break;
	case DBUS_TYPE_ARRAY:       res = "ARRAY";       break;
	case DBUS_TYPE_VARIANT:     res = "VARIANT";     break;
	case DBUS_TYPE_STRUCT:      res = "STRUCT";      break;
	case DBUS_TYPE_DICT_ENTRY:  res = "DICT_ENTRY";  break;
	default: break;
	}
	return res;
}

/** End of iterator reached predicate
 *
 * @param iter dbus message iterator
 *
 * @return true if no more iterms can be read, false otherwise
 */
bool
mce_dbus_iter_at_end(DBusMessageIter *iter)
{
	return dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_INVALID;
}

/** Iterator points to expected data type predicate
 *
 * If not, error diagnostic is emitted
 *
 * @param iter dbus message iterator
 * @param want dbus type id
 *
 * @return true if iterator points to expected type, false otherwise
 */
static bool
mce_dbus_iter_req_type(DBusMessageIter *iter, int want)
{
	int have = dbus_message_iter_get_arg_type(iter);

	if( have == want ) {
		return true;
	}

	mce_log(LL_ERR, "expected: %s, got: %s",
		mce_dbus_type_repr(want),
		mce_dbus_type_repr(have));

	return false;
}

/** Iterator points to expected data type predicate
 *
 * If not, error diagnostic is emitted
 *
 * @param iter dbus message iterator
 * @param want dbus type id
 *
 * @return true if iterator points to expected type, false otherwise
 */
static bool
mce_dbus_iter_get_basic(DBusMessageIter *iter, void *pval, int type)
{
	if( !dbus_type_is_basic(type) ) {
		mce_log(LL_ERR, "%s: is not basic dbus type",
			mce_dbus_type_repr(type));
		return false;
	}

	if( !mce_dbus_iter_req_type(iter, type) )
		return false;

	dbus_message_iter_get_basic(iter, pval);
	dbus_message_iter_next(iter);
	return true;
}

/** Get object path string from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param pval where to store the result
 *
 * @return true if *pval was modified, false otherwise
 */
bool
mce_dbus_iter_get_object(DBusMessageIter *iter, const char **pval)
{
	return mce_dbus_iter_get_basic(iter, pval, DBUS_TYPE_OBJECT_PATH);
}

/** Get string from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param pval where to store the result
 *
 * @return true if *pval was modified, false otherwise
 */
bool
mce_dbus_iter_get_string(DBusMessageIter *iter, const char **pval)
{
	return mce_dbus_iter_get_basic(iter, pval, DBUS_TYPE_STRING);
}

/** Get bool from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param pval where to store the result
 *
 * @return true if *pval was modified, false otherwise
 */
bool
mce_dbus_iter_get_bool(DBusMessageIter *iter, bool *pval)
{
	dbus_bool_t val = 0;
	bool        res = mce_dbus_iter_get_basic(iter, &val,
						  DBUS_TYPE_BOOLEAN);
	if( res )
		*pval = (val != 0);
	return res;
}

/** Get int32 from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param pval where to store the result
 *
 * @return true if *pval was modified, false otherwise
 */
bool
mce_dbus_iter_get_int32(DBusMessageIter *iter, dbus_int32_t *pval)
{
	return mce_dbus_iter_get_basic(iter, pval, DBUS_TYPE_INT32);
}

/** Get uint32 from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param pval where to store the result
 *
 * @return true if *pval was modified, false otherwise
 */
bool
mce_dbus_iter_get_uint32(DBusMessageIter *iter, dbus_uint32_t *pval)
{
	return mce_dbus_iter_get_basic(iter, pval, DBUS_TYPE_UINT32);
}

/** Get sub iterator from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param sub dbus message iterator
 * @param type expected container type id
 *
 * @return true if sub was modified, false otherwise
 */
static bool
mce_dbus_iter_get_container(DBusMessageIter *iter, DBusMessageIter *sub,
			    int type)
{
	if( !dbus_type_is_container(type) ) {
		mce_log(LL_ERR, "%s: is not container dbus type",
			mce_dbus_type_repr(type));
		return false;
	}

	if( !mce_dbus_iter_req_type(iter, type) )
		return false;

	dbus_message_iter_recurse(iter, sub);
	dbus_message_iter_next(iter);
	return true;
}

/** Get array sub iterator from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param sub dbus message iterator
 *
 * @return true if sub was modified, false otherwise
 */
bool
mce_dbus_iter_get_array(DBusMessageIter *iter, DBusMessageIter *sub)
{
	return mce_dbus_iter_get_container(iter, sub, DBUS_TYPE_ARRAY);
}

/** Get struct sub iterator from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param sub dbus message iterator
 *
 * @return true if sub was modified, false otherwise
 */
bool
mce_dbus_iter_get_struct(DBusMessageIter *iter, DBusMessageIter *sub)
{
	return mce_dbus_iter_get_container(iter, sub, DBUS_TYPE_STRUCT);
}

/** Get dict entry sub iterator from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param sub dbus message iterator
 *
 * @return true if sub was modified, false otherwise
 */
bool
mce_dbus_iter_get_entry(DBusMessageIter *iter, DBusMessageIter *sub)
{
	return mce_dbus_iter_get_container(iter, sub, DBUS_TYPE_DICT_ENTRY);
}

/** Get variant sub iterator from dbus message iterator
 *
 * @param iter dbus message iterator
 * @param sub dbus message iterator
 *
 * @return true if sub was modified, false otherwise
 */
bool
mce_dbus_iter_get_variant(DBusMessageIter *iter, DBusMessageIter *sub)
{
	return mce_dbus_iter_get_container(iter, sub, DBUS_TYPE_VARIANT);
}

/** Register D-Bus message handler
 *
 * @param self handler data
 */
void
mce_dbus_handler_register(mce_dbus_handler_t *self)
{
	if( !self->cookie ) {
		self->cookie = mce_dbus_handler_add(self->interface,
						    self->name,
						    self->rules,
						    self->type,
						    self->callback);
		if( !self->cookie )
			mce_log(LL_ERR, "%s.%s: failed to add handler",
				self->interface, self->name);
	}
}

/** Unregister D-Bus message handler
 *
 * @param self handler data
 */
void
mce_dbus_handler_unregister(mce_dbus_handler_t *self)
{
	if( self->cookie )
		mce_dbus_handler_remove(self->cookie), self->cookie = 0;
}

/** Register an array of D-Bus message handlers
 *
 * @param array handler data array, terminated with .interface=NULL
 */
void
mce_dbus_handler_register_array(mce_dbus_handler_t *array)
{
	for( ; array && array->interface; ++array )
		mce_dbus_handler_register(array);
}

/** Unregister an array of D-Bus message handlers
 *
 * @param array handler data array, terminated with .interface=NULL
 */
void
mce_dbus_handler_unregister_array(mce_dbus_handler_t *array)
{
	for( ; array && array->interface; ++array )
		mce_dbus_handler_unregister(array);
}

/* ========================================================================= *
 * DBUS NAME INFO
 * ========================================================================= */

/** Cached D-Bus name owner identification data */
typedef struct
{
	char                ni_repr[64];
	char               *ni_name;
	int                 ni_pid;
	char               *ni_exe;
	mce_dbus_handler_t  ni_hnd;

} mce_dbus_ident_t;

static void              mce_dbus_rem_ident(const char *name);
static mce_dbus_ident_t *mce_dbus_get_ident(const char *name);
static mce_dbus_ident_t *mce_dbus_add_ident(const char *name);

/** Get human readable representation of D-Bus name owner identification data
 */
static const char *
mce_dbus_ident_get_repr(const mce_dbus_ident_t *self)
{
	return self->ni_repr;
}

/** Update human readable representation of D-Bus name owner identification
 */
static void
mce_dbus_ident_update_repr(mce_dbus_ident_t *self)
{
	char pid[16];

	if( self->ni_pid < 0 )
		snprintf(pid, sizeof pid, "???");
	else
		snprintf(pid, sizeof pid, "%d", self->ni_pid);

	snprintf(self->ni_repr, sizeof self->ni_repr, "name=%s pid=%s cmd=%s",
		 self->ni_name, pid, self->ni_exe ?: "???");
}

/** Update executable name in D-Bus name owner identification data
 */
static void mce_dbus_ident_update_exe(mce_dbus_ident_t *self)
{
	int file = -1;
	char path[256];
	unsigned char text[64];

	if( self->ni_exe )
		goto EXIT;

	snprintf(path, sizeof path, "/proc/%d/cmdline", self->ni_pid);
	if( (file = open(path, O_RDONLY)) == -1 ) {
		mce_log(LL_ERR, "%s: open: %m", path);
		goto EXIT;
	}

	int n = read(file, text, sizeof text - 1);
	if( n == -1 ) {
		mce_log(LL_ERR, "%s: read: %m", path);
		goto EXIT;
	}
	text[n] = 0;

	for( int i = 0; i < n; ++i ) {
		if( text[i] < 32 )
			text[i] = ' ';
	}

	self->ni_exe = strdup((char *)text);

EXIT:
	if( file != -1 ) close(file);

	return;
}

/** Handle reply to asynchronous pid of D-Bus name owner query
 */
static void mce_dbus_ident_query_pid_cb(DBusPendingCall *pc, void *aptr)
{
	const char   *name = 0;
	DBusMessage  *rsp  = 0;
	DBusError     err  = DBUS_ERROR_INIT;
	dbus_uint32_t dta  = 0;

	mce_dbus_ident_t *self = 0;

	if( !(name = aptr) )
		goto EXIT;

	if( !pc )
		goto EXIT;

	if( !(rsp = dbus_pending_call_steal_reply(pc)) )
		goto EXIT;

	if( dbus_set_error_from_message(&err, rsp) ||
	    !dbus_message_get_args(rsp, &err,
				   DBUS_TYPE_UINT32, &dta,
				   DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "%s: %s", err.name, err.message);
		mce_dbus_rem_ident(name);
		goto EXIT;
	}

	if( !(self = mce_dbus_get_ident(name)) )
		goto EXIT;

	if( self->ni_pid != 0 )
		goto EXIT;

	self->ni_pid = (int)dta;
	mce_dbus_ident_update_exe(self);
	mce_dbus_ident_update_repr(self);
	mce_log(LL_DEVEL, "%s", mce_dbus_ident_get_repr(self));

EXIT:
	if( rsp ) dbus_message_unref(rsp);
	if( pc )  dbus_pending_call_unref(pc);
	dbus_error_free(&err);

	return;
}

/** Start asynchronous pid of D-Bus name owner query
 */
static void mce_dbus_ident_query_pid(mce_dbus_ident_t *self)
{
	// already done, or in progress
	if( self->ni_pid >= 0 )
		goto EXIT;

	// mark as: in progress
	self->ni_pid = 0;

	const char *name = self->ni_name;

	// start async query
	dbus_send_ex(DBUS_SERVICE_DBUS,
		     DBUS_PATH_DBUS,
		     DBUS_INTERFACE_DBUS,
		     "GetConnectionUnixProcessID",
		     // ----------------
		     mce_dbus_ident_query_pid_cb,
		     strdup(name),
		     free,
		     // ----------------
		     DBUS_TYPE_STRING, &name,
		     DBUS_TYPE_INVALID);

EXIT:
	return;
}

/** Handle NameOwnerChanged signal for cached name owner tracking
 */
static gboolean
mce_dbus_ident_lost_cb(DBusMessage *sig)
{
	const char *name = 0;
	const char *prev = 0;
	const char *curr = 0;
	DBusError   err  = DBUS_ERROR_INIT;

	if( dbus_set_error_from_message(&err, sig) ) {
		mce_log(LL_ERR, "%s: %s", err.name, err.message);
		goto EXIT;
	}

	if( !dbus_message_get_args(sig, &err,
				   DBUS_TYPE_STRING, &name,
				   DBUS_TYPE_STRING, &prev,
				   DBUS_TYPE_STRING, &curr,
				   DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "%s: %s", err.name, err.message);
		goto EXIT;
	}

	if( curr && *curr )
		goto EXIT;

	mce_dbus_rem_ident(name);

EXIT:
	dbus_error_free(&err);
	return TRUE;
}

/** Allocate cached D-Bus name owner identification data
 */
static mce_dbus_ident_t *
mce_dbus_ident_create(const char *name)
{
	mce_dbus_ident_t *self = calloc(1, sizeof *self);

	self->ni_name = strdup(name);
	self->ni_pid  = -1;
	self->ni_exe  = 0;

	mce_log(LL_DEBUG, "start tracking %s", self->ni_name);

	self->ni_hnd.interface = DBUS_INTERFACE_DBUS;
	self->ni_hnd.name      = "NameOwnerChanged";
	self->ni_hnd.rules     = g_strdup_printf("arg0='%s',arg2=''", name);
	self->ni_hnd.type      = DBUS_MESSAGE_TYPE_SIGNAL;
	self->ni_hnd.callback  = mce_dbus_ident_lost_cb;

	mce_dbus_handler_register(&self->ni_hnd);
	mce_dbus_ident_update_repr(self);
	mce_dbus_ident_query_pid(self);

	return self;
}

/** Release Cached D-Bus name owner identification data
 */
static void
mce_dbus_ident_delete(mce_dbus_ident_t *self)
{
	if( self ) {
		mce_log(LL_DEBUG, "stop tracking %s", self->ni_name);
		mce_dbus_handler_unregister(&self->ni_hnd);
		g_free((void *)self->ni_hnd.rules);
		free(self->ni_name);
		free(self->ni_exe);
		free(self);
	}
}

/** Type agnostic callback for releasing D-Bus name owner identification data
 */
static void
mce_dbus_ident_delete_cb(gpointer self)
{
	mce_dbus_ident_delete(self);
}

/** Lookup table for cached D-Bus name owner identification data */
static GHashTable *info_lut = 0;

/** Remove D-Bus name owner identification data from cache
 */
static void mce_dbus_rem_ident(const char *name)
{
	if( !info_lut )
		goto EXIT;

	g_hash_table_remove(info_lut, name);

EXIT:
	return;
}

/** Locate D-Bus name owner identification data from cache
 */
static mce_dbus_ident_t *
mce_dbus_get_ident(const char *name)
{
	mce_dbus_ident_t *res = 0;

	if( !info_lut )
		goto EXIT;

	res = g_hash_table_lookup(info_lut, name);

EXIT:
	return res;
}

/** Add D-Bus name owner identification data to cache
 */
static mce_dbus_ident_t *
mce_dbus_add_ident(const char *name)
{
	mce_dbus_ident_t *res = 0;

	// have existing value?
	if( (res = mce_dbus_get_ident(name)) )
		goto EXIT;

	// have lookup table?
	if( !info_lut ) {
		info_lut = g_hash_table_new_full(g_str_hash, g_str_equal, free,
						 mce_dbus_ident_delete_cb);
	}

	// insert new entry
	res = mce_dbus_ident_create(name);
	g_hash_table_replace(info_lut, strdup(name), res);

EXIT:

	return res;
}

/** Get identification string of D-Bus name owner
 */
const char *mce_dbus_get_name_owner_ident(const char *name)
{
	const char *res = 0;
	const mce_dbus_ident_t *info = 0;

	if( !name )
		goto EXIT;

	if( !(info = mce_dbus_add_ident(name)) )
		goto EXIT;

	res = mce_dbus_ident_get_repr(info);

EXIT:
	return res ?: "unknown";
}

/** Get identification string of sender of D-Bus message
 */
const char *mce_dbus_get_message_sender_ident(DBusMessage *msg)
{
	const char *name = 0;
	if( msg )
		name = dbus_message_get_sender(msg);
	return mce_dbus_get_name_owner_ident(name);
}

/* ========================================================================= *
 * ASYNC PID QUERY
 * ========================================================================= */

typedef struct mce_dbus_pid_query_t mce_dbus_pid_query_t;

struct mce_dbus_pid_query_t
{
	gchar                 *name;
	mce_dbus_pid_notify_t  notify;
};

static mce_dbus_pid_query_t *
mce_dbus_pid_query_create(const char *name, mce_dbus_pid_notify_t cb)
{
	mce_dbus_pid_query_t *self = calloc(1, sizeof *self);
	self->name = strdup(name);
	self->notify = cb;
	return self;
}

static void
mce_dbus_pid_query_delete(mce_dbus_pid_query_t *self)
{
	if( self )
	{
		free(self->name);
		free(self);
	}
}

static void
mce_dbus_pid_query_delete_cb(gpointer self)
{
	mce_dbus_pid_query_delete(self);
}

static void
mce_dbus_pid_query_notify(const mce_dbus_pid_query_t *self, int pid)
{
	if( self && self->notify )
		self->notify(self->name, pid);
}

/** Handle reply to asynchronous pid of D-Bus name owner query
 *
 * @param pc   pending call object
 * @param aptr pid query state object as void pointer
 */
static void
mce_dbus_get_pid_async_cb(DBusPendingCall *pc, void *aptr)
{
	mce_dbus_pid_query_t  *self = aptr;
	DBusMessage  *rsp  = 0;
	DBusError     err  = DBUS_ERROR_INIT;
	dbus_uint32_t dta  = 0;
	int           pid  = -1;

	if( !self )
		goto EXIT;

	if( !pc )
		goto EXIT;

	if( !(rsp = dbus_pending_call_steal_reply(pc)) )
		goto EXIT;

	if( dbus_set_error_from_message(&err, rsp) ||
	    !dbus_message_get_args(rsp, &err,
				   DBUS_TYPE_UINT32, &dta,
				   DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "%s: %s", err.name, err.message);
	}
	else {
		pid = (int)dta;
	}

	mce_log(LL_DEVEL, "name: %s, owner.pid: %d", self->name, pid);
	mce_dbus_pid_query_notify(self, pid);

EXIT:

	if( rsp ) dbus_message_unref(rsp);
	if( pc )  dbus_pending_call_unref(pc);
	dbus_error_free(&err);

	return;
}

/** Start asynchronous pid of D-Bus name owner query
 *
 * @param name D-Bus name, whose owner pid we want to know
 * @param cb   Function to call when async pid query is finished
 */
void
mce_dbus_get_pid_async(const char *name, mce_dbus_pid_notify_t cb)
{
	if( !name || !*name )
		goto EXIT;

	dbus_send_ex(DBUS_SERVICE_DBUS,
		     DBUS_PATH_DBUS,
		     DBUS_INTERFACE_DBUS,
		     "GetConnectionUnixProcessID",
		     // ----------------
		     mce_dbus_get_pid_async_cb,
		     mce_dbus_pid_query_create(name, cb),
		     mce_dbus_pid_query_delete_cb,
		     // ----------------
		     DBUS_TYPE_STRING, &name,
		     DBUS_TYPE_INVALID);

EXIT:
	return;
}

/* ========================================================================= *
 * LOAD/UNLOAD
 * ========================================================================= */

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

	/* get_config */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_CONFIG_GET,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 config_get_dbus_cb) == NULL)
		goto EXIT;

	/* set_config */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_CONFIG_SET,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 config_set_dbus_cb) == NULL)
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
	/* Remove info look up table */
	if( info_lut )
		g_hash_table_unref(info_lut), info_lut = 0;

	/* Unregister remaining D-Bus handlers */
	if( dbus_handlers ) {
		g_slist_foreach(dbus_handlers, mce_dbus_handler_remove_cb, 0);
		g_slist_free(dbus_handlers);
		dbus_handlers = 0;
	}

	/* If there is an established D-Bus connection, unreference it */
	if (dbus_connection != NULL) {
		mce_log(LL_DEBUG, "Unreferencing D-Bus connection");
		dbus_connection_unref(dbus_connection);
		dbus_connection = NULL;
	}

	return;
}
