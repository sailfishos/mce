/**
 * @file cpu-keepalive.c
 * cpu-keepalive module -- this implements late suspend blocking for MCE
 * <p>
 * Copyright (C) 2013-2019 Jolla Ltd.
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
#include "../mce-lib.h"
#include "../mce-dbus.h"

#ifdef ENABLE_WAKELOCKS
# include "../libwakelock.h"
#endif

#include <string.h>
#include <inttypes.h>

#include <mce/dbus-names.h>

#include <gmodule.h>

/* ========================================================================= *
 * CONSTANTS
 * ========================================================================= */

/** The name of this module */
static const char module_name[] = "cpu-keepalive";

#ifdef ENABLE_WAKELOCKS
/** RTC wakeup wakelock - acquired by dsme and released by mce */
static const char rtc_wakelock[] = "mce_rtc_wakeup";

/* CPU keepalive wakelock - held by mce while there are active clients */
static const char cpu_wakelock[] = "mce_cpu_keepalive";
#endif

/** Fallback session_id to use when clients make query keepalive period */
#define SESSION_ID_INITIAL "initial"

/** Fallback session_id to use when clients start/stop keepalive period */
#define SESSION_ID_DEFAULT "undefined"

/** Suggested delay between MCE_CPU_KEEPALIVE_START_REQ method calls [s] */
#ifdef ENABLE_WAKELOCKS
# define MCE_CPU_KEEPALIVE_SUGGESTED_PERIOD_S 60         // 1 minute
#else
# define MCE_CPU_KEEPALIVE_SUGGESTED_PERIOD_S (24*60*60) // 1 day
#endif

/** Maximum delay between MCE_CPU_KEEPALIVE_START_REQ method calls [s] */
# define MCE_CPU_KEEPALIVE_MAXIMUM_PERIOD_S \
   (MCE_CPU_KEEPALIVE_SUGGESTED_PERIOD_S + 15)

/** Auto blocking after MCE_CPU_KEEPALIVE_PERIOD_REQ method calls [s] */
# define MCE_CPU_KEEPALIVE_QUERY_PERIOD_S 2

/** Maximum delay between rtc wakeup and the 1st keep alive request
 *
 * FIXME: The rtc wakeup timeouts need to be tuned once timed and
 *        alarm-ui are modified to use iphb wakeups + cpu-keepalive.
 *
 *        For now we need to delay going back to suspend just in case the wakeup
 *        is needed for showing an alarm and there are hiccups with starting
 *        alarm-ui.
 */
#define MCE_RTC_WAKEUP_1ST_TIMEOUT_S    5

/** Extend rtc wakeup timeout if at least one keep alive request is received */
#define MCE_RTC_WAKEUP_2ND_TIMEOUT_S    5

/** Warning limit for: individual session lasts too long */
#define KEEPALIVE_SESSION_WARN_LIMIT_MS (3 * 60 * 1000) // 3 minutes

/** Warning limit for: keepalive state is kept active too long */
#define KEEPALIVE_STATE_WARN_LIMIT_MS   (5 * 60 * 1000) // 5 minutes

/* ========================================================================= *
 * TYPEDEFS
 * ========================================================================= */

/** Millisecond resolution time value used for cpu keepalive tracking */
typedef int64_t tick_t;

typedef struct cka_session_t cka_session_t;

typedef struct cka_client_t cka_client_t;

/* ========================================================================= *
 * FUNCTION_PROTOTYPES
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * GENERIC_UTILITIES
 * ------------------------------------------------------------------------- */

static tick_t cka_tick_get_current (void);
static tick_t cka_tick_get_timeout (tick_t base_ms, int add_seconds);

/* ------------------------------------------------------------------------- *
 * DBUS_UTILITIES
 * ------------------------------------------------------------------------- */

static gboolean     cka_dbusutil_reply_bool             (DBusMessage *const msg, gboolean value);
static gboolean     cka_dbusutil_reply_int              (DBusMessage *const msg, gint value);
static DBusMessage *cka_dbusutil_create_GetNameOwner_req(const char *name);
static gchar       *cka_dbusutil_parse_GetNameOwner_rsp (DBusMessage *rsp);

/* ------------------------------------------------------------------------- *
 * SESSION_TRACKING
 * ------------------------------------------------------------------------- */

/** Book keeping information for client sessions we are tracking */
struct cka_session_t
{
  /** Link to containing client object */
  cka_client_t *ses_client;

  /** Session identifier provided by client via D-Bus API */
  char         *ses_session;

  /** Internal unique identifier */
  unsigned      ses_unique;

  /** When the session was started */
  tick_t        ses_started;

  /** When the session should end */
  tick_t        ses_timeout;

  /** Number of times timeout has been renewed */
  unsigned      ses_renewed;

  /** Has "too long session" already been reported */
  bool          ses_flagged;

  /** Has the session been finished */
  bool          ses_finished;
};

static cka_session_t *cka_session_create   (cka_client_t *client, const char *session);
static void           cka_session_renew    (cka_session_t *self, tick_t timeout);
static void           cka_session_finish   (cka_session_t *self, tick_t now);
static void           cka_session_delete   (cka_session_t *self);
static void           cka_session_delete_cb(void *self);

/* ------------------------------------------------------------------------- *
 * CLIENT_TRACKING
 * ------------------------------------------------------------------------- */

/** Book keeping information for clients we are tracking */
struct cka_client_t
{
  /** The (private/sender) name of the dbus client */
  gchar      *cli_dbus_name;

  /** NameOwnerChanged signal match used for tracking death of client */
  char       *cli_match_rule;

  /** Upper bound for reneval of cpu keepalive for this client */
  tick_t      cli_timeout;

  /** One client can have several keepalive objects */
  GHashTable *cli_sessions; // [string] -> cka_session_t *
};

/** Format string for constructing name owner lost match rules */
static const char cka_client_match_fmt[] =
"type='signal'"
",sender='"DBUS_SERVICE_DBUS"'"
",interface='"DBUS_INTERFACE_DBUS"'"
",member='NameOwnerChanged'"
",path='"DBUS_PATH_DBUS"'"
",arg0='%s'"
",arg2=''"
;

static cka_session_t *cka_client_get_session   (cka_client_t *self, const char *session_id);
static cka_session_t *cka_client_add_session   (cka_client_t *self, const char *session_id);
static void           cka_client_scan_timeout  (cka_client_t *self);
static void           cka_client_remove_timeout(cka_client_t *self, const char *session_id);
static void           cka_client_update_timeout(cka_client_t *self, const char *session_id, tick_t when);
static cka_client_t  *cka_client_create        (const char *dbus_name);
static const char    *cka_client_identify      (cka_client_t *self);

static void           cka_client_delete        (cka_client_t *self);
static void           cka_client_delete_cb     (void *self);

/* ------------------------------------------------------------------------- *
 * KEEPALIVE_STATE
 * ------------------------------------------------------------------------- */

/** Timer for releasing cpu-keepalive wakelock */
static guint    cka_state_timer_id = 0;

static void     cka_state_set       (bool active);
static gboolean cka_state_timer_cb  (gpointer data);
static void     cka_state_reset     (void);
static void     cka_state_rethink   (void);

/* ------------------------------------------------------------------------- *
 * CLIENT_MANAGEMENT
 * ------------------------------------------------------------------------- */

/** Clients we are tracking over D-Bus */
static GHashTable   *cka_clients_lut = 0;

/** Timestamp of wakeup from dsme */
static tick_t        cka_clients_wakeup_started  = 0;

/** Timeout for "clients should have issued keep alive requests" */
static tick_t        cka_clients_wakeup_timeout  = 0;

static void          cka_clients_verify_name_cb (DBusPendingCall *pending, void *user_data);
static gboolean      cka_clients_verify_name    (const char *name);

static void          cka_clients_remove_client  (const gchar *dbus_name);
static cka_client_t *cka_clients_get_client     (const gchar *dbus_name);
static cka_client_t *cka_clients_add_client     (const gchar *dbus_name);

static void          cka_clients_add_session    (const gchar *dbus_name, const char *session_id);
static void          cka_clients_start_session  (const gchar *dbus_name, const gchar *session_id);
static void          cka_clients_stop_session   (const gchar *dbus_name, const char *session_id);

static void          cka_clients_handle_wakeup  (const gchar *dbus_name);

static void          cka_clients_init           (void);
static void          cka_clients_quit           (void);

/* ------------------------------------------------------------------------- *
 * DBUS_HANDLERS
 * ------------------------------------------------------------------------- */

/** D-Bus system bus connection */
static DBusConnection    *cka_dbus_systembus = 0;

static gboolean           cka_dbus_handle_period_cb  (DBusMessage *const msg);
static gboolean           cka_dbus_handle_start_cb   (DBusMessage *const msg);
static gboolean           cka_dbus_handle_stop_cb    (DBusMessage *const msg);
static gboolean           cka_dbus_handle_wakeup_cb  (DBusMessage *const msg);

static DBusHandlerResult  cka_dbus_filter_message_cb (DBusConnection *con, DBusMessage *msg, void *user_data);

static gboolean           cka_dbus_init              (void);
static void               cka_dbus_quit              (void);

/* ------------------------------------------------------------------------- *
 * MODULE_INIT_QUIT
 * ------------------------------------------------------------------------- */

G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
G_MODULE_EXPORT void         g_module_unload    (GModule *module);

/* ========================================================================= *
 *
 * GENERIC_UTILITIES
 *
 * ========================================================================= */

/** Get monotonic timestamp not affected by system time / timezone changes
 *
 * @return milliseconds since some reference point in time
 */
static
tick_t
cka_tick_get_current(void)
{
  return mce_lib_get_boot_tick();
}

/** Helper for calculating timeout values from ms base + seconds offset
 *
 * @param base_ms      Base time in milliseconds, or -1 to use current time
 * @param add_Seconds  Offset time in seconds
 *
 * @return timeout in millisecond resolution
 */
static
tick_t
cka_tick_get_timeout(tick_t base_ms, int add_seconds)
{
  if( base_ms < 0 ) base_ms = cka_tick_get_current();

  return base_ms + add_seconds * 1000;
}

/* ========================================================================= *
 *
 * DBUS_UTILITIES
 *
 * ========================================================================= */

/** Helper for sending boolean replies to dbus method calls
 *
 * Reply will not be sent if no_reply attribute is set
 * in the method call message.
 *
 * @param msg    method call message to reply
 * @param value  TRUE/FALSE to send
 *
 * @return TRUE on success, or FALSE if reply could not be sent
 */
static
gboolean
cka_dbusutil_reply_bool(DBusMessage *const msg, gboolean value)
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
 * @param msg    method call message to reply
 * @param value  integer number to send
 *
 * @return TRUE on success, or FALSE if reply could not be sent
 */
static
gboolean
cka_dbusutil_reply_int(DBusMessage *const msg, gint value)
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
 * @param name  the dbus name to query
 *
 * @return DBusMessage pointer
 */
static
DBusMessage *
cka_dbusutil_create_GetNameOwner_req(const char *name)
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
 * @param rsp  method call reply message
 *
 * @return dbus name of the name owner, or NULL in case of errors
 */
static
gchar *
cka_dbusutil_parse_GetNameOwner_rsp(DBusMessage *rsp)
{
  char     *res = 0;
  DBusError err = DBUS_ERROR_INIT;
  char     *dta = NULL;

  if( dbus_set_error_from_message(&err, rsp) ||
      !dbus_message_get_args(rsp, &err,
                             DBUS_TYPE_STRING, &dta,
                             DBUS_TYPE_INVALID) )
  {
    if( strcmp(err.name, DBUS_ERROR_NAME_HAS_NO_OWNER) )
    {
      mce_log(LL_WARN, "%s: %s", err.name, err.message);
    }
    goto EXIT;
  }

  res = g_strdup(dta);

EXIT:
  dbus_error_free(&err);
  return res;
}

/* ------------------------------------------------------------------------- *
 * SESSION_TRACKING
 * ------------------------------------------------------------------------- */

/** Create bookkeeping information for a keepalive session
 *
 * @param client      client object
 * @param session_id  assumed unique id provided by the client
 *
 * @return pointer to cka_session_t structure
 */
static
cka_session_t *
cka_session_create(cka_client_t *client, const char *session_id)
{
  static unsigned id = 0;

  cka_session_t *self = g_malloc0(sizeof *self);

  self->ses_client   = client;
  self->ses_session  = g_strdup(session_id);
  self->ses_unique     = ++id;
  self->ses_timeout  = 0;
  self->ses_started  = cka_tick_get_current();
  self->ses_renewed  = 0;
  self->ses_flagged  = false;
  self->ses_finished = false;

  mce_log(LL_DEVEL, "session created; id=%u/%s %s",
          self->ses_unique, self->ses_session,
          cka_client_identify(self->ses_client));

  return self;
}

/** Renew timeout for a keepalive session
 *
 * @param self     session object
 * @param timeout  end of session timeout to use
 */
static
void
cka_session_renew(cka_session_t *self, tick_t timeout)
{
  self->ses_timeout  = timeout;
  self->ses_renewed += 1;

  tick_t now = cka_tick_get_current();
  tick_t dur = now - self->ses_started;

  if( !self->ses_flagged && dur > KEEPALIVE_SESSION_WARN_LIMIT_MS )
  {
    self->ses_flagged = true;
    mce_log(LL_CRIT, "long session active after %"PRId64" ms; "
            "id=%u/%s %s",
            dur, self->ses_unique, self->ses_session,
            cka_client_identify(self->ses_client));
  }
  else
  {
    mce_log(LL_DEBUG, "session T%+"PRId64"; id=%u/%s %s",
            now - self->ses_timeout,
            self->ses_unique, self->ses_session,
            cka_client_identify(self->ses_client));
  }
}

/** Finish a keepalive session
 *
 * @param self  session object
 * @param now   current time
 */
static
void
cka_session_finish(cka_session_t *self, tick_t now)
{
  tick_t dur = now - self->ses_started;

  if( dur > KEEPALIVE_SESSION_WARN_LIMIT_MS )
  {
    mce_log(LL_CRIT, "long session lasted %"PRId64" ms; id=%u/%s %s",
            dur, self->ses_unique, self->ses_session,
            cka_client_identify(self->ses_client));
  }
  else
  {
    mce_log(LL_DEVEL, "session lasted %"PRId64" ms; id=%u/%s %s",
            dur, self->ses_unique, self->ses_session,
            cka_client_identify(self->ses_client));
  }

  self->ses_finished = true;
}

/** Delete bookkeeping information for a keepalive session
 *
 * @param self  session object, or NULL
 */
static
void
cka_session_delete(cka_session_t *self)
{
  if( !self )
  {
    goto EXIT;
  }

  mce_log(LL_DEBUG, "session deleted; id=%u/%s %s",
          self->ses_unique, self->ses_session,
          cka_client_identify(self->ses_client));

  g_free(self->ses_session);
  g_free(self);

EXIT:
  return;
}

/** Typeless helper function for use as destroy callback
 *
 * @param self  session object, or NULL
 */
static
void
cka_session_delete_cb(void *self)
{
  cka_session_delete(self);
}

/* ========================================================================= *
 *
 * CLIENT_TRACKING
 *
 * ========================================================================= */

/** Lookup client session object
 *
 * @param self        pointer to cka_client_t structure
 * @param session_id  client provided session id
 *
 * @return session object, or NULL
 */
static
cka_session_t *
cka_client_get_session(cka_client_t *self, const char *session_id)
{
  cka_session_t *session = g_hash_table_lookup(self->cli_sessions, session_id);

  return session;
}

/** Lookup existing / create new client session object
 *
 * @param self        pointer to cka_client_t structure
 * @param session_id  client provided session id
 *
 * @return session object
 */
static
cka_session_t *
cka_client_add_session(cka_client_t *self, const char *session_id)
{
  cka_session_t *session = g_hash_table_lookup(self->cli_sessions, session_id);

  if( !session )
  {
    session = cka_session_create(self, session_id);
    g_hash_table_replace(self->cli_sessions, g_strdup(session_id), session);
  }

  return session;
}

/** Update client timeout to be maximum of context timeouts
 *
 * @param self pointer to cka_client_t structure
 */
static
void
cka_client_scan_timeout(cka_client_t *self)
{
  tick_t now  = cka_tick_get_current();

  self->cli_timeout = 0;

  /* Expire sessions / update client timeout */
  GHashTableIter iter;
  gpointer       val;

  g_hash_table_iter_init(&iter, self->cli_sessions);
  while( g_hash_table_iter_next(&iter, 0, &val) )
  {
    cka_session_t *session = val;

    if( session->ses_timeout <= now )
    {
      /* Expire session */
      cka_session_finish(session, now);
      g_hash_table_iter_remove(&iter);
    }
    else if( self->cli_timeout < session->ses_timeout )
    {
      /* Update client timeout */
      self->cli_timeout = session->ses_timeout;
    }
  }

  if( self->cli_timeout > now )
  {
    mce_log(LL_DEBUG, "client T%+"PRId64"; %s",
            now - self->cli_timeout,
            cka_client_identify(self));
  }
}

/** Clear client cpu-keepalive timeout
 *
 * @param self        pointer to cka_client_t structure
 * @param session_id  client provided session id
 */
static
void
cka_client_remove_timeout(cka_client_t *self, const char *session_id)
{
  cka_session_t *session = cka_client_get_session(self, session_id);

  if( session )
  {
    tick_t now = cka_tick_get_current();
    cka_session_finish(session, now);
    g_hash_table_remove(self->cli_sessions, session_id);
  }
}

/** Update client cpu-keepalive timeout
 *
 * @param self        pointer to cka_client_t structure
 * @param session_id  client provided session id
 * @param when        end of client cpu-keepalive period
 */
static
void
cka_client_update_timeout(cka_client_t *self, const char *session_id,
                          tick_t when)
{
  cka_session_t *session = cka_client_add_session(self, session_id);

  if( session )
  {
    cka_session_renew(session, when);
  }
}

/** Get client identification information
 *
 * @param self  pointer to cka_client_t structure
 *
 * @return human readable string identifying the client process
 */
static
const char *
cka_client_identify(cka_client_t *self)
{
  return mce_dbus_get_name_owner_ident(self->cli_dbus_name);
}

/** Create bookkeeping information for a dbus client
 *
 * Note: Will also add signal matching rule so that we get notified
 *       when the client loses dbus connection
 *
 * @param dbus_name  name of the dbus client to track
 *
 * @return pointer to cka_client_t structure
 */
static
cka_client_t *
cka_client_create(const char *dbus_name)
{
  cka_client_t *self = g_malloc0(sizeof *self);

  self->cli_dbus_name  = g_strdup(dbus_name);
  self->cli_match_rule = g_strdup_printf(cka_client_match_fmt,
                                         self->cli_dbus_name);
  self->cli_timeout    = 0;

  self->cli_sessions   = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, cka_session_delete_cb);

  mce_log(LL_DEBUG, "client created; %s", cka_client_identify(self));

  /* NULL error -> match will be added asynchronously */
  dbus_bus_add_match(cka_dbus_systembus, self->cli_match_rule, 0);

  return self;
}

/** Destroy bookkeeping information about a dbus client
 *
 * Note: Will also remove the signal matching rule used for detecting
 *       when the client loses dbus connection
 *
 * @param self  pointer to cka_client_t structure
 */

static
void
cka_client_delete(cka_client_t *self)
{
  if( self != 0 )
  {
    mce_log(LL_DEBUG, "client deleted; %s", cka_client_identify(self));

    tick_t now = cka_tick_get_current();

    /* Finish all sessions */
    GHashTableIter iter;
    gpointer       val;

    g_hash_table_iter_init(&iter, self->cli_sessions);
    while( g_hash_table_iter_next(&iter, 0, &val) )
    {
      cka_session_t *session = val;

      cka_session_finish(session, now);
    }

    /* NULL error -> match will be removed asynchronously */
    dbus_bus_remove_match(cka_dbus_systembus, self->cli_match_rule, 0);

    /* Cleanup */
    g_hash_table_unref(self->cli_sessions);
    g_free(self->cli_dbus_name);
    g_free(self->cli_match_rule);
    g_free(self);
  }
}

/** Typeless helper function for use as destroy callback
 *
 * @param self  pointer to cka_client_t structure
 */
static
void
cka_client_delete_cb(void *self)
{
  cka_client_delete(self);
}

/* ========================================================================= *
 *
 * KEEPALIVE_STATE
 *
 * ========================================================================= */

/** Set keepalive state
 *
 * @param active  true to start keepalive, false to end keepalive
 */
static void cka_state_set(bool active)
{
  static bool keepalive_is_active = false;
  static bool flagged = false;
  static tick_t started = 0;

  if( keepalive_is_active != active )
  {
    tick_t now = cka_tick_get_current();

    if( (keepalive_is_active = active) )
    {
#ifdef ENABLE_WAKELOCKS
      wakelock_lock(cpu_wakelock, -1);
#endif
      started = now;
      mce_log(LL_DEVEL, "keepalive started");
    }
    else
    {
      tick_t dur = now - started;

      if( dur > KEEPALIVE_STATE_WARN_LIMIT_MS )
      {
        mce_log(LL_CRIT, "long keepalive stopped after %"PRId64" ms", dur);
      }
      else
      {
        mce_log(LL_DEVEL, "keepalive stopped after %"PRId64" ms", dur);
      }

      flagged = false;

#ifdef ENABLE_WAKELOCKS
      wakelock_unlock(cpu_wakelock);
#endif
    }
  }
  else if( keepalive_is_active && !flagged ) {
    tick_t now = cka_tick_get_current();
    tick_t dur = now - started;
    if( dur > KEEPALIVE_STATE_WARN_LIMIT_MS )
    {
      flagged = true;
      mce_log(LL_CRIT, "long keepalive active after %"PRId64" ms", dur);
    }
  }
}

/** Handle triggering of cpu-keepalive timer
 *
 * Releases cpu keepalive wakelock and thus allows the system to
 * enter late suspend according to other policies.
 *
 * @param data  (not used)
 *
 * @return FALSE (to stop then timer from repeating)
 */
static
gboolean
cka_state_timer_cb(gpointer data)
{
  (void)data;

  if( cka_state_timer_id != 0 )
  {
    mce_log(LL_DEBUG, "cpu-keepalive timeout triggered");
    cka_state_timer_id = 0;

    /* Do full rethink to expire client sessions */
    cka_state_rethink();
  }

  return FALSE;
}

/** Cancel end of cpu-keepalive timer
 */
static
void
cka_state_reset(void)
{
  if( cka_state_timer_id != 0 )
  {
    mce_log(LL_DEBUG, "cpu-keepalive timeout canceled");
    g_source_remove(cka_state_timer_id), cka_state_timer_id = 0;
  }

  cka_state_set(false);
}

/** Re-evaluate the end of cpu-keepalive period
 *
 * Calculates maximum of wakeup period and per client renew periods
 * and uses it to reprogram the end of cpu-keepalive period
 */
static
void
cka_state_rethink(void)
{
  tick_t now = cka_tick_get_current();

  /* Find furthest away client renew timeout */
  tick_t maxtime = cka_clients_wakeup_timeout;

  GHashTableIter iter;
  gpointer       val;

  g_hash_table_iter_init(&iter, cka_clients_lut);
  while( g_hash_table_iter_next(&iter, 0, &val) )
  {
    cka_client_t *client = val;

    cka_client_scan_timeout(client);

    if( maxtime < client->cli_timeout )
    {
      maxtime = client->cli_timeout;
    }
  }

  /* Remove existing timer */
  if( cka_state_timer_id != 0 )
  {
    g_source_remove(cka_state_timer_id), cka_state_timer_id = 0;
  }

  /* If needed, program timer */
  static tick_t oldtime = 0;

  if( now < maxtime )
  {
    if( maxtime != oldtime )
    {
      mce_log(LL_DEBUG, "cpu-keepalive timeout at T%+"PRId64"",
              now - maxtime);
    }
    cka_state_timer_id = g_timeout_add(maxtime - now,
                             cka_state_timer_cb, 0);
  }

  oldtime = maxtime;

  cka_state_set(cka_state_timer_id != 0);
}

/* ========================================================================= *
 *
 * CLIENT_MANAGEMENT
 *
 * ========================================================================= */

/** Remove bookkeeping data for a client and re-evaluate cpu keepalive status
 *
 * @param dbus_name  dbus name of the client
 */
static
void
cka_clients_remove_client(const gchar *dbus_name)
{
  if( g_hash_table_remove(cka_clients_lut, dbus_name) )
  {
    cka_state_rethink();
  }
}

/** Obtain bookkeeping data for a client
 *
 * @param dbus_name  dbus name of the client
 *
 * @return client data, or NULL if dbus_name is not tracked
 */
static
cka_client_t *
cka_clients_get_client(const gchar *dbus_name)
{
  cka_client_t *client = g_hash_table_lookup(cka_clients_lut, dbus_name);
  return client;
}

/** Call back for handling asynchronous client verification via GetNameOwner
 *
 * @param pending    control structure for asynchronous d-bus methdod call
 * @param user_data  dbus_name of the client as void poiter
 */
static
void
cka_clients_verify_name_cb(DBusPendingCall *pending, void *user_data)
{
  const gchar *name   = user_data;
  gchar       *owner  = 0;
  DBusMessage *rsp    = 0;
  cka_client_t    *client = 0;

  if( !(rsp = dbus_pending_call_steal_reply(pending)) )
  {
    goto EXIT;
  }

  if( !(client = cka_clients_get_client(name)) )
  {
    mce_log(LL_WARN, "untracked client %s", name);
  }

  if( !(owner = cka_dbusutil_parse_GetNameOwner_rsp(rsp)) || !*owner )
  {
    mce_log(LL_WARN, "dead client %s", name);
    cka_clients_remove_client(name), client = 0;
  }
  else
  {
    mce_log(LL_DEBUG, "live client %s, owner %s", name, owner);
  }

EXIT:
  g_free(owner);

  if( rsp ) dbus_message_unref(rsp);
}

/** Verify that a client exists via an asynchronous GetNameOwner method call
 *
 * @param name  the dbus name who's owner we wish to know
 *
 * @return TRUE if the method call was initiated, or FALSE in case of errors
 */
static
gboolean
cka_clients_verify_name(const char *name)
{
  gboolean         res = FALSE;
  DBusMessage     *req = 0;
  DBusPendingCall *pc  = 0;
  gchar           *key = 0;

  if( !(req = cka_dbusutil_create_GetNameOwner_req(name)) )
  {
    goto EXIT;
  }

  if( !dbus_connection_send_with_reply(cka_dbus_systembus, req, &pc, -1) )
  {
    goto EXIT;
  }

  if( !pc )
  {
    goto EXIT;
  }

  mce_dbus_pending_call_blocks_suspend(pc);

  key = g_strdup(name);

  if( !dbus_pending_call_set_notify(pc, cka_clients_verify_name_cb,
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
 * @param dbus_name  dbus name of the client
 *
 * @return client data
 */
static
cka_client_t *
cka_clients_add_client(const gchar *dbus_name)
{
  cka_client_t *client = g_hash_table_lookup(cka_clients_lut, dbus_name);

  if( !client )
  {
    /* Make a dummy peer identification request, so we have it already
     * cached in case we actually need it later on */
    mce_dbus_get_name_owner_ident(dbus_name);

    /* The cka_client_create() adds NameOwnerChanged signal match
     * so that we know when/if the client exits, crashes or
     * otherwise loses dbus connection. */

    client = cka_client_create(dbus_name);
    g_hash_table_insert(cka_clients_lut, g_strdup(dbus_name), client);

    /* Then make an explicit GetNameOwner request to verify that
     * the client is still running when we have the signal
     * listening in the place. */
    cka_clients_verify_name(dbus_name);
  }

  return client;
}

/** Register client and start minor keep-alive period
 *
 * If needed will add the dbus_name to list of tracked clients.
 *
 * @param dbus_name  name of the tracked client
 */
static
void
cka_clients_add_session(const gchar *dbus_name, const char *session_id)
{
  cka_client_t *client = cka_clients_add_client(dbus_name);

  tick_t when = cka_tick_get_timeout(-1, MCE_CPU_KEEPALIVE_QUERY_PERIOD_S);

  cka_client_update_timeout(client, session_id, when);

  cka_state_rethink();
}

/** Adjust the cpu-keepalive timeout for dbus client
 *
 * If needed will add the dbus_name to list of tracked clients.
 *
 * @param dbus_name  name of the tracked client
 */
static
void
cka_clients_start_session(const gchar *dbus_name, const gchar *session_id)
{
  cka_client_t *client = cka_clients_add_client(dbus_name);

  tick_t when = cka_tick_get_timeout(-1, MCE_CPU_KEEPALIVE_MAXIMUM_PERIOD_S);

  cka_client_remove_timeout(client, SESSION_ID_INITIAL);
  cka_client_update_timeout(client, session_id, when);

  /* We got at least one keep alive request, extend the minimum
   * alive time a bit to give other clients time to get scheduled */
  cka_clients_wakeup_timeout =
    cka_tick_get_timeout(cka_clients_wakeup_started,
                         MCE_RTC_WAKEUP_2ND_TIMEOUT_S);

  cka_state_rethink();
}

/** Remove the cpu-keepalive timeout for dbus client
 *
 * @param dbus_name  name of the tracked client
 */
static
void
cka_clients_stop_session(const gchar *dbus_name, const char *session_id)
{
  cka_client_t *client = cka_clients_get_client(dbus_name);

  if( client )
  {
    cka_client_remove_timeout(client, SESSION_ID_INITIAL);
    cka_client_remove_timeout(client, session_id);
    cka_state_rethink();
  }
  else
  {
    mce_log(LL_WARN, "untracked client %s", dbus_name);
  }
}

/** Transfer resume-due-to-rtc-input wakelock from dsme to mce
 *
 * @param dbus_name  name of the client issuing the request
 */
static
void
cka_clients_handle_wakeup(const gchar *dbus_name)
{
  // FIXME: we should check that the dbus_name == DSME
  (void)dbus_name;

  /* Time of wakeup received */
  cka_clients_wakeup_started = cka_tick_get_current();

  /* Timeout for the 1st keepalive message to come through */
  cka_clients_wakeup_timeout =
    cka_tick_get_timeout(cka_clients_wakeup_started,
                         MCE_RTC_WAKEUP_1ST_TIMEOUT_S);

  cka_state_rethink();

  mce_log(LL_NOTICE, "rtc wakeup finished");
#ifdef ENABLE_WAKELOCKS
  wakelock_unlock(rtc_wakelock);
#endif
}

/** Initialize client tracking
 */
static void cka_clients_init(void)
{
  if( !cka_clients_lut )
  {
    cka_clients_lut = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, cka_client_delete_cb);
  }
}

/** Cleanup client tracking
 */
static void cka_clients_quit(void)
{
  if( cka_clients_lut )
  {
    g_hash_table_unref(cka_clients_lut), cka_clients_lut = 0;
  }
}

/* ========================================================================= *
 *
 * DBUS_HANDLERS
 *
 * ========================================================================= */

/** D-Bus callback for the MCE_CPU_KEEPALIVE_PERIOD_REQ method call
 *
 * @param msg  The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static
gboolean
cka_dbus_handle_period_cb(DBusMessage *const msg)
{
  gboolean success = FALSE;

  DBusError   err     = DBUS_ERROR_INIT;
  const char *sender  = 0;
  const char *session_id = 0;

  if( !(sender = dbus_message_get_sender(msg)) )
  {
    goto EXIT;
  }

  mce_log(LL_NOTICE, "got keepalive period query from %s",
          mce_dbus_get_name_owner_ident(sender));

  if( !dbus_message_get_args(msg, &err,
                             DBUS_TYPE_STRING, &session_id,
                             DBUS_TYPE_INVALID) )
  {
    // initial dbus interface did not include session_id string
    if( strcmp(err.name, DBUS_ERROR_INVALID_ARGS) )
    {
      mce_log(LL_WARN, "%s: %s", err.name, err.message);
      goto EXIT;
    }

    session_id = SESSION_ID_INITIAL;
    mce_log(LL_DEBUG, "sender did not supply session_id string; using '%s'",
            session_id);
  }

  cka_clients_add_session(sender, session_id);

  success = cka_dbusutil_reply_int(msg, MCE_CPU_KEEPALIVE_SUGGESTED_PERIOD_S);

EXIT:
  dbus_error_free(&err);
  return success;
}

/** D-Bus callback for the MCE_CPU_KEEPALIVE_START_REQ method call
 *
 * @param msg  The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static
gboolean
cka_dbus_handle_start_cb(DBusMessage *const msg)
{
  gboolean success = FALSE;

  DBusError  err         = DBUS_ERROR_INIT;
  const char *sender     = 0;
  const char *session_id = 0;

  if( !(sender = dbus_message_get_sender(msg)) )
  {
    goto EXIT;
  }

  mce_log(LL_NOTICE, "got keepalive start from %s",
          mce_dbus_get_name_owner_ident(sender));

  if( !dbus_message_get_args(msg, &err,
                             DBUS_TYPE_STRING, &session_id,
                             DBUS_TYPE_INVALID) )
  {
    // initial dbus interface did not include session_id string
    if( strcmp(err.name, DBUS_ERROR_INVALID_ARGS) )
    {
      mce_log(LL_WARN, "%s: %s", err.name, err.message);
      goto EXIT;
    }

    session_id = SESSION_ID_DEFAULT;
    mce_log(LL_DEBUG, "sender did not supply session_id string; using '%s'",
            session_id);
  }

  cka_clients_start_session(sender, session_id);

  success = TRUE;

EXIT:
  cka_dbusutil_reply_bool(msg, success);

  dbus_error_free(&err);
  return success;
}

/** D-Bus callback for the MCE_CPU_KEEPALIVE_STOP_REQ method call
 *
 * @param msg  The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static
gboolean
cka_dbus_handle_stop_cb(DBusMessage *const msg)
{
  gboolean success = FALSE;

  DBusError  err      = DBUS_ERROR_INIT;
  const char *sender  =  0;
  const char *session_id = 0;

  if( !(sender = dbus_message_get_sender(msg)) )
  {
    goto EXIT;
  }

  mce_log(LL_NOTICE, "got keepalive stop from %s",
          mce_dbus_get_name_owner_ident(sender));

  if( !dbus_message_get_args(msg, &err,
                             DBUS_TYPE_STRING, &session_id,
                             DBUS_TYPE_INVALID) )
  {
    // initial dbus interface did not include session_id string
    if( strcmp(err.name, DBUS_ERROR_INVALID_ARGS) )
    {
      mce_log(LL_WARN, "%s: %s", err.name, err.message);
      goto EXIT;
    }

    session_id = SESSION_ID_DEFAULT;
    mce_log(LL_DEBUG, "sender did not supply session_id string; using '%s'",
            session_id);
  }

  cka_clients_stop_session(sender, session_id);

  success = TRUE;

EXIT:
  cka_dbusutil_reply_bool(msg, success);

  dbus_error_free(&err);
  return success;
}

/** D-Bus callback for the MCE_CPU_KEEPALIVE_WAKEUP_REQ method call
 *
 * @param msg  The D-Bus message
 *
 * @return TRUE on success, FALSE on failure
 */
static
gboolean
cka_dbus_handle_wakeup_cb(DBusMessage *const msg)
{
  gboolean success = FALSE;

  const char *sender =  0;

  if( !(sender = dbus_message_get_sender(msg)) )
  {
    goto EXIT;
  }

  mce_log(LL_NOTICE, "got keepalive wakeup from %s",
          mce_dbus_get_name_owner_ident(sender));

  cka_clients_handle_wakeup(sender);

  success = TRUE;

EXIT:
  cka_dbusutil_reply_bool(msg, success);

  return success;
}

/** D-Bus message filter for handling NameOwnerChanged signals
 *
 * @param con        dbus connection
 * @param msg        message to be acted upon
 * @param user_data  (not used)
 *
 * @return DBUS_HANDLER_RESULT_NOT_YET_HANDLED (other filters see the msg too)
 */
static
DBusHandlerResult
cka_dbus_filter_message_cb(DBusConnection *con, DBusMessage *msg,
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

  if( con != cka_dbus_systembus )
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
    mce_log(LL_DEBUG, "name lost owner: %s", name);
    cka_clients_remove_client(name);
  }

EXIT:
  dbus_error_free(&err);
  return res;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t cka_dbus_handlers[] =
{
  /* method calls */
  {
    .interface = MCE_REQUEST_IF,
    .name      = MCE_CPU_KEEPALIVE_PERIOD_REQ,
    .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
    .callback  = cka_dbus_handle_period_cb,
    .args      =
      "    <arg direction=\"in\" name=\"session_id\" type=\"s\"/>\n"
      "    <arg direction=\"out\" name=\"period\" type=\"i\"/>\n"
  },
  {
    .interface = MCE_REQUEST_IF,
    .name      = MCE_CPU_KEEPALIVE_START_REQ,
    .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
    .callback  = cka_dbus_handle_start_cb,
    .args      =
      "    <arg direction=\"in\" name=\"session_id\" type=\"s\"/>\n"
      "    <arg direction=\"out\" name=\"success\" type=\"b\"/>\n"
  },
  {
    .interface = MCE_REQUEST_IF,
    .name      = MCE_CPU_KEEPALIVE_STOP_REQ,
    .type      = DBUS_MESSAGE_TYPE_METHOD_CALL,
    .callback  = cka_dbus_handle_stop_cb,
    .args      =
      "    <arg direction=\"in\" name=\"session_id\" type=\"s\"/>\n"
      "    <arg direction=\"out\" name=\"success\" type=\"b\"/>\n"
  },
  {
    .interface  = MCE_REQUEST_IF,
    .name       = MCE_CPU_KEEPALIVE_WAKEUP_REQ,
    .type       = DBUS_MESSAGE_TYPE_METHOD_CALL,
    .callback   = cka_dbus_handle_wakeup_cb,
    .privileged = true,
    .args       =
      "    <arg direction=\"out\" name=\"success\" type=\"b\"/>\n"
  },
  /* sentinel */
  {
    .interface = 0
  }
};

/** Install signal and method call message handlers
 *
 * @return TRUE on success, or FALSE on failure
 */
static gboolean cka_dbus_init(void)
{
  gboolean success = FALSE;

  if( !(cka_dbus_systembus = dbus_connection_get()) )
  {
    goto EXIT;
  }

  /* Register signal handling filter */
  dbus_connection_add_filter(cka_dbus_systembus,
                             cka_dbus_filter_message_cb, 0, 0);

  /* Register dbus method call handlers */
  mce_dbus_handler_register_array(cka_dbus_handlers);

  success = TRUE;

EXIT:
  return success;
}

/** Remove signal and method call message handlers
 */
static void cka_dbus_quit(void)
{
  if( !cka_dbus_systembus )
  {
    goto EXIT;
  }

  /* Remove signal handling filter */
  dbus_connection_remove_filter(cka_dbus_systembus,
                                cka_dbus_filter_message_cb, 0);

  /* Remove dbus method call handlers that we have registered */
  mce_dbus_handler_unregister_array(cka_dbus_handlers);

  dbus_connection_unref(cka_dbus_systembus), cka_dbus_systembus = 0;

EXIT:
  return;
}

/* ========================================================================= *
 *
 * MODULE_INIT_QUIT
 *
 * ========================================================================= */

/** Init function for the cpu-keepalive module
 *
 * @param module  (not used)
 *
 * @return NULL on success, a string with an error message on failure
 */
const gchar *g_module_check_init(GModule *module)
{
  (void)module;

  const gchar *status = NULL;

  if( !cka_dbus_init() )
  {
    status = "initializing dbus connection failed";
    goto EXIT;
  }

  cka_clients_init();

EXIT:

  mce_log(LL_DEBUG, "loaded %s, status: %s", module_name, status ?: "ok");

  return status;
}

/** Exit function for the cpu-keepalive module
 *
 * @param module  (not used)
 */
void g_module_unload(GModule *module)
{
  (void)module;

  /* If we have active clients, removal expects a valid dbus
   * connection -> purge clients first */
  cka_clients_quit();

  cka_dbus_quit();

  /* Make sure the wakelock is released */
  cka_state_reset();

  mce_log(LL_DEBUG, "unloaded %s", module_name);

  return;
}
