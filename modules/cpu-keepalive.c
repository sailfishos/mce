/* ------------------------------------------------------------------------- *
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: LGPLv2
 * ------------------------------------------------------------------------- */

/**
 * @file cpu-keepalive.c
 * cpu-keepalive module -- this implements late suspend blocking for MCE
 * <p>
 * Copyright © 2013 Jolla Ltd.
 * <p>
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

#include "../mce-log.h"
#include "../mce-dbus.h"

#ifdef ENABLE_WAKELOCKS
# include "../libwakelock.h"
#endif

#include <string.h>

#include <mce/dbus-names.h>

#include <gmodule.h>

/** Fallback context to use when clients make query keepalive period */
#define CONTEXT_INITIAL "initial"

/** Fallback context to use when clients start/stop keepalive period */
#define CONTEXT_DEFAULT "undefined"

typedef struct client_t client_t;

G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
G_MODULE_EXPORT void         g_module_unload    (GModule *module);

/** The name of this module */
static const char module_name[] = "cpu-keepalive";

#ifdef ENABLE_WAKELOCKS
/** RTC wakeup wakelock - acquired by dsme and released by mce */
static const char rtc_wakelock[] = "mce_rtc_wakeup";

/* CPU keepalive wakelock - held by mce while there are active clients */
static const char cpu_wakelock[] = "mce_cpu_keepalive";
#endif /* ENABLE_WAKELOCKS */

static DBusConnection *systembus = 0;

/** Clients we are tracking over D-Bus */
static GHashTable *clients = 0;

/** Timestamp of wakeup from dsme */
static time_t wakeup_started  = 0;

/** Timeout for "clients should have issued keep alive requests" */
static time_t wakeup_timeout  = 0;

/** Timer for releasing cpu-keepalive wakelock */
static guint timer_id = 0;

/** Suggested delay between MCE_CPU_KEEPALIVE_START_REQ method calls [s] */
#ifdef ENABLE_WAKELOCKS
# define MCE_CPU_KEEPALIVE_SUGGESTED_PERIOD 60         // 1 minute
#else
# define MCE_CPU_KEEPALIVE_SUGGESTED_PERIOD (60*60*24) // 1 day
#endif

/** Maximum delay between MCE_CPU_KEEPALIVE_START_REQ method calls [s] */
# define MCE_CPU_KEEPALIVE_MAXIMUM_PERIOD \
   (MCE_CPU_KEEPALIVE_SUGGESTED_PERIOD + 15)

/** Maximum delay between rtc wakeup and the 1st keep alive request */
#define MCE_RTC_WAKEUP_1ST_TIMEOUT_SECONDS   2

/** Extend rtc wakeup timeout if at least one keep alive request is received */
#define MCE_RTC_WAKEUP_2ND_TIMEOUT_SECONDS   4

/* ========================================================================= *
 *
 * GENERIC UTILITIES
 *
 * ========================================================================= */

/** Get monotonic timestamp not affected by system time / timezone changes
 *
 * @return seconds since some reference point in time
 */
static
time_t
cpu_keepalive_get_time(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_BOOTTIME, &ts);
  return ts.tv_sec;
}

/* ========================================================================= *
 *
 * D-BUS UTILITIES
 *
 * ========================================================================= */

/** Helper for sending boolean replies to dbus method calls
 *
 * Reply will not be sent if no_reply attribute is set
 * in the method call message.
 *
 * @param msg method call message to reply
 * @param value TRUE/FALSE to send
 *
 * @return TRUE on success, or FALSE if reply could not be sent
 */
static
gboolean
cpu_keepalive_reply_bool(DBusMessage *const msg, gboolean value)
{
  gboolean success = TRUE;

  if( !dbus_message_get_no_reply(msg) )
  {
    dbus_bool_t  data  = value;
    DBusMessage *reply = dbus_new_method_reply(msg);
    dbus_message_append_args(reply,
                             DBUS_TYPE_BOOLEAN, &data,
                             DBUS_TYPE_INVALID);

    /* dbus_send_message() unrefs the message */
    success  = dbus_send_message(reply), reply = 0;

    if( !success )
    {
      mce_log(LL_WARN, "failed to send reply to %s",
              dbus_message_get_member(msg));
    }
  }

  return success;
}

/** Helper for sending boolean replies to dbus method calls
 *
 * Reply will not be sent if no_reply attribute is set
 * in the method call message.
 *
 * @param msg method call message to reply
 * @param value integer number to send
 *
 * @return TRUE on success, or FALSE if reply could not be sent
 */
static
gboolean
cpu_keepalive_reply_int(DBusMessage *const msg, gint value)
{
  gboolean success = TRUE;

  if( !dbus_message_get_no_reply(msg) )
  {
    dbus_int32_t data  = value;
    DBusMessage *reply = dbus_new_method_reply(msg);
    dbus_message_append_args(reply,
                             DBUS_TYPE_INT32, &data,
                             DBUS_TYPE_INVALID);

    /* dbus_send_message() unrefs the message */
    success  = dbus_send_message(reply), reply = 0;

    if( !success )
    {
      mce_log(LL_WARN, "failed to send reply to %s",
              dbus_message_get_member(msg));
    }
  }

  return success;
}

/** Create a GetNameOwner method call message
 *
 * @param name the dbus name to query
 *
 * @return DBusMessage pointer
 */
static
DBusMessage *
cpu_keepalive_create_GetNameOwner_req(const char *name)
{
  DBusMessage *req = 0;

  req = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
                                     DBUS_PATH_DBUS,
                                     DBUS_INTERFACE_DBUS,
                                     "GetNameOwner");
  dbus_message_append_args(req,
                           DBUS_TYPE_STRING, &name,
                           DBUS_TYPE_INVALID);

  return req;
}

/** Parse a reply message to GetNameOwner method call
 *
 * @param rsp method call reply message
 *
 * @return dbus name of the name owner, or NULL in case of errors
 */
static
gchar *
cpu_keepalive_parse_GetNameOwner_rsp(DBusMessage *rsp)
{
  char     *res = 0;
  DBusError err = DBUS_ERROR_INIT;
  char     *dta = NULL;

  if( dbus_set_error_from_message(&err, rsp) ||
      !dbus_message_get_args(rsp, &err,
                             DBUS_TYPE_STRING, &dta,
                             DBUS_TYPE_INVALID) )
  {
      if( strcmp(err.name, DBUS_ERROR_NAME_HAS_NO_OWNER) ) {
          mce_log(LL_WARN, "%s: %s", err.name, err.message);
      }
      goto EXIT;
  }

  res = g_strdup(dta);

EXIT:
  dbus_error_free(&err);
  return res;
}

/* ========================================================================= *
 *
 * INFORMATION ABOUT ACTIVE CLIENTS
 *
 * ========================================================================= */

/** Format string for constructing name owner lost match rules */
static const char client_match_fmt[] =
"type='signal'"
",sender='"DBUS_SERVICE_DBUS"'"
",interface='"DBUS_INTERFACE_DBUS"'"
",member='NameOwnerChanged'"
",path='"DBUS_PATH_DBUS"'"
",arg0='%s'"
",arg2=''"
;

/** Book keeping information for clients we are tracking */
struct client_t
{
  /** The (private/sender) name of the dbus client */
  gchar  *dbus_name;

  /** NameOwnerChanged signal match used for tracking death of client */
  char   *match_rule;

  /** Upper bound for reneval of cpu keepalive for this client */
  time_t  timeout;

  /** One client can have several keepalive objects */
  GHashTable *contexts; // [string] -> (gpointer)time_t
};

/** Iterator function for use from client_scan_timeout()
 */
static
void
client_scan_timeout_cb(gpointer key, gpointer val, gpointer aptr)
{
  (void)key;
  client_t *self = aptr;
  time_t    when = GPOINTER_TO_INT(val);

  mce_log(LOG_DEBUG, "keepalive client=%s context=%s: T%+ld",
          self->dbus_name, (const char *)key,
          (long)(cpu_keepalive_get_time() - when));

  if( self->timeout < when )
  {
    self->timeout = when;
  }
}

/** Update client timeout to be maximum of context timeouts
 */
static
void
client_scan_timeout(client_t *self)
{
  time_t now  = cpu_keepalive_get_time();

  self->timeout = now;

  g_hash_table_foreach(self->contexts,
                       client_scan_timeout_cb,
                       self);

  mce_log(LOG_DEBUG, "keepalive client=%s: T%+ld",
          self->dbus_name, (long)(now - self->timeout));
}

/** Clear client cpu-keepalive timeout
 *
 * @param self pointer to client_t structure
 */
static
void
client_clear_timeout(client_t *self, const char *context)
{
  if( g_hash_table_remove(self->contexts, context) )
  {
    mce_log(LOG_DEBUG, "keepalive client=%s context=%s: cleared",
            self->dbus_name, context);
    client_scan_timeout(self);
  }
}

/** Update client cpu-keepalive timeout
 *
 * @param self pointer to client_t structure
 * @param when end of client cpu-keepalive period
 */
static
void
client_update_timeout(client_t *self, const char *context, time_t when)
{
  mce_log(LOG_DEBUG, "keepalive client=%s context=%s: T%+ld",
          self->dbus_name, context,
          (long)(cpu_keepalive_get_time() - when));

  g_hash_table_replace(self->contexts, g_strdup(context),
                       GINT_TO_POINTER(when));
  client_scan_timeout(self);
}

/** Create bookkeeping information for a dbus client
 *
 * Note: Will also add signal matching rule so that we get notified
 *       when the client loses dbus connection
 *
 * @param dbus_name name of the dbus client to track
 *
 * @return pointer to client_t structure
 */
static
client_t *
client_create(const char *dbus_name)
{
  client_t *self = g_malloc0(sizeof *self);

  self->dbus_name  = g_strdup(dbus_name);
  self->match_rule = g_strdup_printf(client_match_fmt, self->dbus_name);
  self->timeout    = 0;

  self->contexts   = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, NULL);

  mce_log(LL_NOTICE, "added cpu-keepalive client %s", self->dbus_name);

  /* NULL error -> match will be added asynchronously */
  dbus_bus_add_match(systembus, self->match_rule, 0);

  return self;
}

/** Destroy bookkeeping information about a dbus client
 *
 * Note: Will also remove the signal matching rule used for detecting
 *       when the client loses dbus connection
 *
 * @param self pointer to client_t structure
 */

static
void
client_delete(client_t *self)
{
  if( self != 0 )
  {
    mce_log(LL_NOTICE, "removed cpu-keepalive client %s", self->dbus_name);

    /* NULL error -> match will be removed asynchronously */
    dbus_bus_remove_match(systembus, self->match_rule, 0);

    g_hash_table_unref(self->contexts);
    g_free(self->dbus_name);
    g_free(self->match_rule);
    g_free(self);
  }
}

/** Typeless helper function for use as destroy callback
 *
 * @param self pointer to client_t structure
 */
static
void
client_delete_cb(void *self)
{
  client_delete(self);
}

/* ========================================================================= *
 *
 * CPU-KEEPALIVE TIMER MANAGEMENT
 *
 * ========================================================================= */

/** Handle triggering of cpu-keepalive timer
 *
 * Releases cpu keepalive wakelock and thus allows the system to
 * enter late suspend according to other policies.
 *
 * @param data (not used)
 *
 * @return FALSE (to stop then timer from repeating)
 */
static
gboolean
cpu_keepalive_timer_cb(gpointer data)
{
  (void)data;

  if( timer_id != 0 )
  {
    mce_log(LL_DEVEL, "cpu-keepalive ended");
    timer_id = 0;

#ifdef ENABLE_WAKELOCKS
    wakelock_unlock(cpu_wakelock);
#endif
  }

  return FALSE;
}

/** Cancel end of cpu-keepalive timer
 */
static
void
cpu_keepalive_cancel_timer(void)
{
  if( timer_id != 0 )
  {
    g_source_remove(timer_id), timer_id = 0;
  }
}

/** Reset cpu-keepalive timer
 *
 * @param when monotonic time of the end of cpu-keepalive period
 */
static
void
cpu_keepalive_set_timer(time_t when)
{
  cpu_keepalive_cancel_timer();

  time_t now = cpu_keepalive_get_time();

  if( when < now ) when = now;

  mce_log(LL_NOTICE, "cpu-keepalive ends at T%+d", (int)(now - when));

  if( now < when )
  {
    timer_id = g_timeout_add_seconds(when - now,
                                     cpu_keepalive_timer_cb, 0);
  }
  else
  {
    timer_id = g_idle_add(cpu_keepalive_timer_cb, 0);
  }
}

/** Re-evaluate the end of cpu-keepalive period
 *
 * Calculates maximum of wakeup period and per client renew periods
 * and uses it to reprogram the end of cpu-keepalive period
 */
static
void
cpu_keepalive_rethink(void)
{
  time_t maxtime = wakeup_timeout;

  GHashTableIter iter;
  gpointer key, val;

#ifdef ENABLE_WAKELOCKS
  wakelock_lock(cpu_wakelock, -1);
#endif

  g_hash_table_iter_init(&iter, clients);
  while( g_hash_table_iter_next (&iter, &key, &val) )
  {
    client_t *client = val;
    if( maxtime < client->timeout )
    {
      maxtime = client->timeout;
    }
  }
  cpu_keepalive_set_timer(maxtime);
}

/* ========================================================================= *
 *
 * CLIENT MANAGEMENT
 *
 * ========================================================================= */

/** Remove bookkeeping data for a client and re-evaluate cpu keepalive status
 *
 * @param dbus_name dbus name of the client
 */
static
void
cpu_keepalive_remove_client(const gchar *dbus_name)
{
  if( g_hash_table_remove(clients, dbus_name) )
  {
    cpu_keepalive_rethink();
  }
}

/** Obtain bookkeeping data for a client
 *
 * @param dbus_name dbus name of the client
 *
 * @return client data, or NULL if dbus_name is not tracked
 */
static
client_t *
cpu_keepalive_get_client(const gchar *dbus_name)
{
  client_t *client = g_hash_table_lookup(clients, dbus_name);
  return client;
}

/** Call back for handling asynchronous client verification via GetNameOwner
 *
 * @param pending   control structure for asynchronous d-bus methdod call
 * @param user_data dbus_name of the client as void poiter
 */
static
void
cpu_keepalive_verify_name_cb(DBusPendingCall *pending, void *user_data)
{
  const gchar *name   = user_data;
  gchar       *owner  = 0;
  DBusMessage *rsp    = 0;
  client_t    *client = 0;

  if( !(rsp = dbus_pending_call_steal_reply(pending)) )
  {
    goto EXIT;
  }

  if( !(client = cpu_keepalive_get_client(name)) )
  {
    mce_log(LL_WARN, "untracked client %s", name);
  }

  if( !(owner = cpu_keepalive_parse_GetNameOwner_rsp(rsp)) || !*owner )
  {
    mce_log(LL_WARN, "dead client %s", name);
    cpu_keepalive_remove_client(name), client = 0;
  }
  else
  {
    mce_log(LL_WARN, "live client %s, owner %s", name, owner);
  }

EXIT:
  g_free(owner);

  if( rsp ) dbus_message_unref(rsp);
}

/** Verify that a client exists via an asynchronous GetNameOwner method call
 *
 * @param name the dbus name who's owner we wish to know
 *
 * @return TRUE if the method call was initiated, or FALSE in case of errors
 */
static
gboolean
cpu_keepalive_verify_name(const char *name)
{
  gboolean         res = FALSE;
  DBusMessage     *req = 0;
  DBusPendingCall *pc  = 0;
  gchar           *key = 0;

  if( !(req = cpu_keepalive_create_GetNameOwner_req(name)) )
  {
    goto EXIT;
  }

  if( !dbus_connection_send_with_reply(systembus, req, &pc, -1) )
  {
    goto EXIT;
  }

  if( !pc )
  {
    goto EXIT;
  }

  key = g_strdup(name);

  if( !dbus_pending_call_set_notify(pc, cpu_keepalive_verify_name_cb,
                                    key, g_free) )
  {
    goto EXIT;
  }

  // key string is owned by pending call
  key = 0;

  // success
  res = TRUE;

EXIT:

  g_free(key);

  if( pc  ) dbus_pending_call_unref(pc);
  if( req ) dbus_message_unref(req);

  return res;
}

/** Find existing / create new client data by dbus name
 *
 * @param dbus_name dbus name of the client
 *
 * @return client data
 */
static
client_t *
cpu_keepalive_add_client(const gchar *dbus_name)
{
  client_t *client = g_hash_table_lookup(clients, dbus_name);

  if( !client )
  {
    /* The client_create() adds NameOwnerChanged signal match
     * so that we know when/if the client exits, crashes or
     * otherwise loses dbus connection. */

    client = client_create(dbus_name);
    g_hash_table_insert(clients, g_strdup(dbus_name), client);

    /* Then make an explicit GetNameOwner request to verify that
     * the client is still running when we have the signal
     * listening in the place. */
    cpu_keepalive_verify_name(dbus_name);
  }

  return client;
}

/** Register client and start minor keep-alive period
 *
 * If needed will add the dbus_name to list of tracked clients.
 *
 * @param dbus_name name of the tracked client
 */
static
void
cpu_keepalive_register(const gchar *dbus_name, const char *context)
{
  client_t *client = cpu_keepalive_add_client(dbus_name);

  time_t when = cpu_keepalive_get_time() + 2;

  client_update_timeout(client, context, when);

  cpu_keepalive_rethink();
}

/** Adjust the cpu-keepalive timeout for dbus client
 *
 * If needed will add the dbus_name to list of tracked clients.
 *
 * @param dbus_name name of the tracked client
 */
static
void
cpu_keepalive_start(const gchar *dbus_name, const gchar *context)
{
  client_t *client = cpu_keepalive_add_client(dbus_name);

  time_t when = cpu_keepalive_get_time() + MCE_CPU_KEEPALIVE_MAXIMUM_PERIOD;

  client_clear_timeout(client, CONTEXT_INITIAL);
  client_update_timeout(client, context, when);

  /* We got at least one keep alive request, extend the minimum
   * alive time a bit to give other clients time to get scheduled */
  wakeup_timeout = wakeup_started + MCE_RTC_WAKEUP_2ND_TIMEOUT_SECONDS;

  cpu_keepalive_rethink();
}

/** Remove the cpu-keepalive timeout for dbus client
 *
 * @param dbus_name name of the tracked client
 */
static
void
cpu_keepalive_stop(const gchar *dbus_name, const char *context)
{
  client_t *client = cpu_keepalive_get_client(dbus_name);

  if( client )
  {
    client_clear_timeout(client, CONTEXT_INITIAL);
    client_clear_timeout(client, context);
    cpu_keepalive_rethink();
  }
  else
  {
    mce_log(LL_WARN, "untracked client %s", dbus_name);
  }
}

/** Transfer resume-due-to-rtc-input wakelock from dsme to mce
 *
 * @param dbus_name name of the client issuing the request
 */
static
void
cpu_keepalive_wakeup(const gchar *dbus_name)
{
  // FIXME: we should check that the dbus_name == DSME
  (void)dbus_name;

  /* Time of wakeup received */
  wakeup_started = cpu_keepalive_get_time();

  /* Timeout for the 1st keepalive message to come through */
  wakeup_timeout = wakeup_started + MCE_RTC_WAKEUP_1ST_TIMEOUT_SECONDS;

  cpu_keepalive_rethink();

  mce_log(LL_NOTICE, "rtc wakeup finished");
#ifdef ENABLE_WAKELOCKS
  wakelock_unlock(rtc_wakelock);
#endif
}

/* ========================================================================= *
 *
 * D-BUS METHOD CALL HANDLERS
 *
 * ========================================================================= */

/** D-Bus callback for the MCE_CPU_KEEPALIVE_PERIOD_REQ method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static
gboolean
cpu_keepalive_period_cb(DBusMessage *const msg)
{
  gboolean success = FALSE;

  DBusError   err     = DBUS_ERROR_INIT;
  const char *sender  = 0;
  const char *context = 0;

  if( !(sender = dbus_message_get_sender(msg)) )
  {
    goto EXIT;
  }

  mce_log(LL_DEVEL, "got keepalive period query from %s",
          mce_dbus_get_name_owner_ident(sender));

  if( !dbus_message_get_args(msg, &err,
                             DBUS_TYPE_STRING, &context,
                             DBUS_TYPE_INVALID) )
  {
    // initial dbus interface did not include context string
    if( strcmp(err.name, DBUS_ERROR_INVALID_ARGS) )
    {
      mce_log(LL_WARN, "%s: %s", err.name, err.message);
      goto EXIT;
    }

    context = CONTEXT_INITIAL;
    mce_log(LL_DEBUG, "sender did not supply context string; using '%s'", context);
  }

  mce_log(LL_DEBUG, "sender=%s, context=%s", sender, context);

  cpu_keepalive_register(sender, context);

  success = cpu_keepalive_reply_int(msg, MCE_CPU_KEEPALIVE_SUGGESTED_PERIOD);

EXIT:
  dbus_error_free(&err);
  return success;
}

/** D-Bus callback for the MCE_CPU_KEEPALIVE_START_REQ method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static
gboolean
cpu_keepalive_start_cb(DBusMessage *const msg)
{
  gboolean success = FALSE;

  DBusError  err      = DBUS_ERROR_INIT;
  const char *sender  = 0;
  const char *context = 0;

  if( !(sender = dbus_message_get_sender(msg)) )
  {
    goto EXIT;
  }

  mce_log(LL_DEVEL, "got keepalive start from %s",
          mce_dbus_get_name_owner_ident(sender));

  if( !dbus_message_get_args(msg, &err,
                             DBUS_TYPE_STRING, &context,
                             DBUS_TYPE_INVALID) )
  {
    // initial dbus interface did not include context string
    if( strcmp(err.name, DBUS_ERROR_INVALID_ARGS) )
    {
      mce_log(LL_WARN, "%s: %s", err.name, err.message);
      goto EXIT;
    }

    context = CONTEXT_DEFAULT;
    mce_log(LL_DEBUG, "sender did not supply context string; using '%s'", context);
  }

  mce_log(LL_DEBUG, "sender=%s, context=%s", sender, context);

  cpu_keepalive_start(sender, context);

  success = TRUE;

EXIT:
  cpu_keepalive_reply_bool(msg, success);

  dbus_error_free(&err);
  return success;
}

/** D-Bus callback for the MCE_CPU_KEEPALIVE_STOP_REQ method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static
gboolean
cpu_keepalive_stop_cb(DBusMessage *const msg)
{
  gboolean success = FALSE;

  DBusError  err      = DBUS_ERROR_INIT;
  const char *sender  =  0;
  const char *context = 0;

  if( !(sender = dbus_message_get_sender(msg)) )
  {
    goto EXIT;
  }

  mce_log(LL_DEVEL, "got keepalive stop from %s",
          mce_dbus_get_name_owner_ident(sender));

  if( !dbus_message_get_args(msg, &err,
                             DBUS_TYPE_STRING, &context,
                             DBUS_TYPE_INVALID) )
  {
    // initial dbus interface did not include context string
    if( strcmp(err.name, DBUS_ERROR_INVALID_ARGS) )
    {
      mce_log(LL_WARN, "%s: %s", err.name, err.message);
      goto EXIT;
    }

    context = CONTEXT_DEFAULT;
    mce_log(LL_DEBUG, "sender did not supply context string; using '%s'", context);
  }

  mce_log(LL_DEBUG, "sender=%s, context=%s", sender, context);

  cpu_keepalive_stop(sender, context);

  success = TRUE;

EXIT:
  cpu_keepalive_reply_bool(msg, success);

  dbus_error_free(&err);
  return success;
}

/** D-Bus callback for the MCE_CPU_KEEPALIVE_WAKEUP_REQ method call
 *
 * @param msg The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static
gboolean
cpu_keepalive_wakeup_cb(DBusMessage *const msg)
{
  gboolean success = FALSE;

  const char *sender =  0;

  if( !(sender = dbus_message_get_sender(msg)) )
  {
    goto EXIT;
  }

  mce_log(LL_DEVEL, "got keepalive wakeup from %s",
          mce_dbus_get_name_owner_ident(sender));

  cpu_keepalive_wakeup(sender);

  success = TRUE;

EXIT:
  cpu_keepalive_reply_bool(msg, success);

  return success;
}

/* ========================================================================= *
 *
 * D-BUS SIGNAL HANDLERS
 *
 * ========================================================================= */

/** D-Bus message filter for handling NameOwnerChanged signals
 *
 * @param con       dbus connection
 * @param msg       message to be acted upon
 * @param user_data (not used)
 *
 * @return DBUS_HANDLER_RESULT_NOT_YET_HANDLED (other filters see the msg too)
 */
static
DBusHandlerResult
cpu_keepalive_dbus_filter_cb(DBusConnection *con, DBusMessage *msg,
                             void *user_data)
{
  (void)user_data;

  DBusHandlerResult res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  const char *sender = 0;
  const char *object = 0;

  const char *name = 0;
  const char *prev = 0;
  const char *curr = 0;

  DBusError err = DBUS_ERROR_INIT;

  if( con != systembus )
  {
    goto EXIT;
  }

  if( !dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged") )
  {
    goto EXIT;
  }

  sender = dbus_message_get_sender(msg);
  if( !sender || strcmp(sender, DBUS_SERVICE_DBUS) )
  {
    goto EXIT;
  }

  object = dbus_message_get_path(msg);
  if( !object || strcmp(object, DBUS_PATH_DBUS) )
  {
    goto EXIT;
  }

  if( !dbus_message_get_args(msg, &err,
                             DBUS_TYPE_STRING, &name,
                             DBUS_TYPE_STRING, &prev,
                             DBUS_TYPE_STRING, &curr,
                             DBUS_TYPE_INVALID) )
  {
    mce_log(LL_WARN, "%s: %s", err.name, err.message);
    goto EXIT;
  }

  if( !*curr )
  {
    mce_log(LL_INFO, "name lost owner: %s", name);
    cpu_keepalive_remove_client(name);
  }

EXIT:
  dbus_error_free(&err);
  return res;
}

/* ========================================================================= *
 *
 * MODULE INIT/QUIT
 *
 * ========================================================================= */

typedef gboolean (*handler_t)(DBusMessage *const msg);

/** Book keeping for dbus method call handler status */
static struct
{
  const char      *member;
  const handler_t  handler;
  gconstpointer    cookie;
} methods[] =
{
  { MCE_CPU_KEEPALIVE_PERIOD_REQ, cpu_keepalive_period_cb, 0 },
  { MCE_CPU_KEEPALIVE_START_REQ,  cpu_keepalive_start_cb,  0 },
  { MCE_CPU_KEEPALIVE_STOP_REQ,   cpu_keepalive_stop_cb,   0 },
  { MCE_CPU_KEEPALIVE_WAKEUP_REQ, cpu_keepalive_wakeup_cb, 0 },
  { 0, 0, 0 }
};

/** Install signal and method call message handlers
 *
 * @return TRUE on success, or FALSE on failure
 */
static gboolean cpu_keepalive_attach_to_dbus(void)
{
  gboolean success = TRUE;

  /* Register signal handling filter */
  dbus_connection_add_filter(systembus, cpu_keepalive_dbus_filter_cb, 0, 0);

  /* Register dbus method call handlers */
  for( size_t i = 0; methods[i].member; ++i )
  {
    mce_log(LL_INFO, "registering handler for: %s", methods[i].member);

    methods[i].cookie = mce_dbus_handler_add(MCE_REQUEST_IF,
                                             methods[i].member,
                                             NULL,
                                             DBUS_MESSAGE_TYPE_METHOD_CALL,
                                             methods[i].handler);
    if( !methods[i].cookie )
    {
      mce_log(LL_WARN, "failed to add dbus handler for: %s",
              methods[i].member);
      success = FALSE;
    }
  }

  return success;
}

/** Remove signal and method call message handlers
 */
static void cpu_keepalive_detach_from_dbus(void)
{
  /* Remove signal handling filter */
  dbus_connection_remove_filter(systembus, cpu_keepalive_dbus_filter_cb, 0);

  /* Remove dbus method call handlers that we have registered */
  for( size_t i = 0; methods[i].member; ++i )
  {
    if( methods[i].cookie )
    {
      mce_log(LL_INFO, "removing handler for: %s", methods[i].member);
      mce_dbus_handler_remove(methods[i].cookie);
      methods[i].cookie = 0;
    }
  }
}

/** Init function for the cpu-keepalive module
 *
 * @param module (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *g_module_check_init(GModule *module)
{
  (void)module;

  const gchar *status = NULL;

  if( !(systembus = dbus_connection_get()) )
  {
    status = "mce has no dbus connection";
    goto EXIT;
  }

  if( !cpu_keepalive_attach_to_dbus() )
  {
    status = "attaching to dbus connection failed";
    goto EXIT;
  }

  clients = g_hash_table_new_full(g_str_hash, g_str_equal,
                                  g_free, client_delete_cb);

EXIT:

  mce_log(LL_NOTICE, "loaded %s, status: %s", module_name, status ?: "ok");

  return status;
}

/** Exit function for the cpu-keepalive module
 *
 * @param module (not used)
 */
void g_module_unload(GModule *module)
{
  (void)module;

  /* If we have active clients, removal expects a valid dbus
   * connection -> purge clients first */
  if( clients )
  {
    g_hash_table_unref(clients), clients = 0;
  }

  if( systembus )
  {
    cpu_keepalive_detach_from_dbus();
    dbus_connection_unref(systembus), systembus = 0;
  }

  mce_log(LL_NOTICE, "unloaded %s", module_name);

  return;
}
