#include "mce-sensorfw.h"
#include "mce-log.h"
#include "mce-dbus.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>

/** org.freedesktop.DBus.NameOwnerChanged D-Bus signal */
#define DBUS_NAME_OWNER_CHANGED_SIG "NameOwnerChanged"

#define SENSORFW_SERVICE "com.nokia.SensorService"
#define SENSORFW_PATH    "/SensorManager"
#define SENSOR_SOCKET    "/var/run/sensord.sock"

#if 0 // DEBUG: make all logging from this module "critical"
# undef mce_log
# define mce_log(LEV, FMT, ARGS...) \
	mce_log_file(LL_CRIT, __FILE__, __FUNCTION__, FMT , ## ARGS)
#endif

/** ALS data block as sensord sends them */
typedef struct als_data {
	uint64_t timestamp; /* microseconds, monotonic */
	uint32_t value;
} als_data_t;

/** PS data block as sensord sends them */
typedef struct ps_data {
	uint64_t timestamp; /* microseconds, monotonic */
	uint32_t value;
	// This should be the size of a C++ bool on the same platform.
	// Unfortunately there's no way to find out in a C program
	uint8_t  withinProximity;
} ps_data_t;

/* ========================================================================= *
 * STATE DATA
 * ========================================================================= */

/** D-Bus System Bus connection */
static DBusConnection *systembus = 0;

/** Flag for sensord is on system bus */
static bool       sensord_running = false;

/** Sensord name for ALS */
static const char als_name[]  = "alssensor";

/** Sensord D-Bus interface for ALS */
static const char als_iface[] = "local.ALSSensor";

/** Sensord session id for ALS */
static int        als_sid     = -1;

/** Input watch for ALS data */
static guint      als_wid     = 0;

/** Flag for MCE wants to enable ALS */
static bool       als_want    = false;

/** Flag for ALS enabled at sensord */
static bool       als_have    = false;

/** Callback for sending ALS data to where it is needed */
static void     (*als_notify)(unsigned lux) = 0;

/** Sensord name for PS */
static const char ps_name[]  = "proximitysensor";

/** Sensord D-Bus interface for PS */
static const char ps_iface[] = "local.ProximitySensor";

/** Sensord session id for PS */
static int        ps_sid     = -1;

/** Input watch for PS data */
static guint      ps_wid     = 0;

/** Flag for MCE wants to enable PS */
static bool       ps_want    = false;

/** Flag for PS enabled at sensord */
static bool       ps_have    = false;

/** Callback for sending PS data to where it is needed */
static void     (*ps_notify)(bool covered) = 0;

/* ========================================================================= *
 * COMMON
 * ========================================================================= */

/** Handle ALS input from sensord
 *
 * @param chn  io channel
 * @param cnd  (not used)
 * @param aptr (not used)
 *
 * @return TRUE to keep io watch, or FALSE to remove it
 */
static gboolean
als_input_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);

	(void)cnd; (void)aptr; // unused

	gboolean keep_going = FALSE;
	uint32_t count = 0;

	int fd;
	int rc;
	als_data_t data;

	memset(&data, 0, sizeof data);

	if( (fd = g_io_channel_unix_get_fd(chn)) < 0 ) {
		mce_log(LL_ERR, "io channel has no fd");
		goto EXIT;
	}

	// FIXME: there should be only one read() ...

	rc = read(fd, &count, sizeof count);
	if( rc == -1 ) {
		if( errno == EINTR || errno == EAGAIN )
			keep_going = TRUE;
		else
			mce_log(LL_ERR, "read sample count: %m");
		goto EXIT;
	}
	if( rc == 0 ) {
		mce_log(LL_ERR, "read sample count: EOF");
		goto EXIT;
	}
	if( rc != (int)sizeof count) {
		mce_log(LL_ERR, "read sample count: got %d of %d bytes",
			rc, (int)sizeof count);
		goto EXIT;
	}

	mce_log(LL_DEBUG, "got %u ALS values", (unsigned)count);

	if( count < 1 ) {
		keep_going = TRUE;
		goto EXIT;
	}

	for( unsigned i = 0; i < count; ++i ) {
		errno = 0, rc = read(fd, &data, sizeof data);
		if( rc != sizeof data ) {
			mce_log(LL_ERR, "failed to read sample: %m");
			goto EXIT;
		}
	}

	mce_log(LL_DEBUG, "last ALS value = %u", data.value);
	if( als_notify )
		als_notify(data.value);
	else
		mce_log(LL_WARN, "ALS enabled without notify cb");

	keep_going = TRUE;

EXIT:
	if( !keep_going ) {
		mce_log(LL_CRIT, "disabling io watch");
		als_wid = 0;
	}
	return keep_going;
}

/** Handle PS input from sensord
 *
 * @param chn  io channel
 * @param cnd  (not used)
 * @param aptr (not used)
 *
 * @return TRUE to keep io watch, or FALSE to remove it
 */
static gboolean
ps_input_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);

	(void)cnd; (void)aptr; // unused

	gboolean keep_going = FALSE;
	uint32_t count = 0;

	int fd;
	int rc;
	ps_data_t data;

	memset(&data, 0, sizeof data);

	if( (fd = g_io_channel_unix_get_fd(chn)) < 0 ) {
		mce_log(LL_ERR, "io channel has no fd");
		goto EXIT;
	}

	// FIXME: there should be only one read() ...

	rc = read(fd, &count, sizeof count);
	if( rc == -1 ) {
		if( errno == EINTR || errno == EAGAIN )
			keep_going = TRUE;
		else
			mce_log(LL_ERR, "read sample count: %m");
		goto EXIT;
	}
	if( rc == 0 ) {
		mce_log(LL_ERR, "read sample count: EOF");
		goto EXIT;
	}
	if( rc != (int)sizeof count) {
		mce_log(LL_ERR, "read sample count: got %d of %d bytes",
			rc, (int)sizeof count);
		goto EXIT;
	}

	mce_log(LL_DEBUG, "Got %u PS values", (unsigned)count);

	if( count < 1 ) {
		keep_going = TRUE;
		goto EXIT;
	}

	for( unsigned i = 0; i < count; ++i ) {
		errno = 0, rc = read(fd, &data, sizeof data);
		if( rc != sizeof data ) {
			mce_log(LL_ERR, "failed to read sample: %m");
			goto EXIT;
		}
	}

	mce_log(LL_DEBUG, "last PS value = %u / %d", data.value,
		data.withinProximity);

	if( ps_notify )
		ps_notify(data.withinProximity != 0);
	else
		mce_log(LL_WARN, "PS enabled without notify cb");

	keep_going = TRUE;

EXIT:
	if( !keep_going ) {
		mce_log(LL_CRIT, "disabling io watch");
		ps_wid = 0;
	}
	return keep_going;
}

/** Add input watch for sensord session
 *
 * @param sessionid sensord session id from mce_sensorfw_request_sensor()
 * @param datafunc  glib io watch callback
 *
 * @param glib io watch source id, or 0 in case of failure
 */
static guint
mce_sensorfw_add_io_watch(int sessionid, GIOFunc datafunc)
{
	guint       wid = 0;
	int         fd  = -1;
	int32_t     sid = sessionid;
	char        ack = 0;
	GIOChannel *chn = 0;

	struct sockaddr_un sa;
	socklen_t sa_len;

	mce_log(LL_INFO, "adding watch for session %d", sessionid);

	if( (fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1 ) {
		mce_log(LL_ERR, "could not open local domain socket");
		goto EXIT;
	}

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", SENSOR_SOCKET);
	sa_len = strchr(sa.sun_path, 0) + 1 - (char *)&sa;

	if( connect(fd, (struct sockaddr *) &sa, sa_len) == -1 ) {
		mce_log(LL_ERR, "could not connect to %s: %m",
			SENSOR_SOCKET);
		goto EXIT;
	}

	errno = 0;
	if( write(fd, &sid, sizeof sid) != sizeof sid ) {
		mce_log(LL_ERR, "could not initialize reader for"
			" session %d: %m",
			sessionid);
		goto EXIT;
	}

	errno = 0;
	if( read(fd, &ack, 1) != 1 || ack != '\n' ) {
		mce_log(LL_ERR, "could not get handshake for"
			" session %d: %m",
			sessionid);
		goto EXIT;
	}

	if( !(chn = g_io_channel_unix_new(fd)) ) {
		goto EXIT;
	}

	if( !(wid = g_io_add_watch(chn, G_IO_IN, datafunc, 0)) ) {
		goto EXIT;
	}

	g_io_channel_set_close_on_unref(chn, TRUE), fd = -1;

	mce_log(LL_DEBUG, "io watch for %d = %u", sessionid, wid);

EXIT:
	if( chn != 0 ) g_io_channel_unref(chn);
	if( fd != -1 ) close(fd);
	return wid;
}

/** Issue load sensor IPC to sensord
 *
 * @param id sensor name
 *
 * @return true on success, or false in case of errors
 */
static bool
mce_sensorfw_load_sensor(const char *id)
{
	bool         res = false;
	DBusError    err = DBUS_ERROR_INIT;
	DBusMessage *msg = 0;
	dbus_bool_t  ack = FALSE;

	mce_log(LL_INFO, "loadPlugin(%s)", id);

	// FIXME: should not block ...

	msg = dbus_send_with_block(SENSORFW_SERVICE,
				   SENSORFW_PATH,
				   "local.SensorManager",
				   "loadPlugin",
				   DBUS_TIMEOUT_USE_DEFAULT,
				   DBUS_TYPE_STRING, &id,
				   DBUS_TYPE_INVALID);
	if( !msg ) {
		mce_log(LOG_ERR, "loadPlugin(%s): no reply", id);
		goto EXIT;
	}

	if( dbus_set_error_from_message(&err, msg) ) {
		mce_log(LOG_ERR, "loadPlugin(%s): error reply: %s: %s",
			id, err.name, err.message);
		goto EXIT;
	}

	if( !dbus_message_get_args(msg, &err,
				   DBUS_TYPE_BOOLEAN, &ack,
				   DBUS_TYPE_INVALID) ) {
		mce_log(LOG_ERR, "loadPlugin(%s): parse reply: %s: %s",
			id, err.name, err.message);
		goto EXIT;
	}

	if( !ack ) {
		mce_log(LOG_WARNING, "loadPlugin(%s): request denied", id);
		goto EXIT;
	}

	res = true;
EXIT:
	if( msg ) dbus_message_unref(msg);
	dbus_error_free(&err);
	return res;
}

/** Issue request sensor IPC to sensord
 *
 * @param id sensor name
 *
 * @return session id, or -1 in case of errors
 */
static int
mce_sensorfw_request_sensor(const char *id)
{
	int          res = -1;
	DBusMessage *msg = 0;
	DBusError    err = DBUS_ERROR_INIT;
	dbus_int64_t pid = getpid();
	dbus_int32_t sid = -1;

	mce_log(LL_INFO, "requestSensor(%s)", id);

	// FIXME: should not block ...

	msg = dbus_send_with_block(SENSORFW_SERVICE,
				   SENSORFW_PATH,
				   "local.SensorManager",
				   "requestSensor",
				   DBUS_TIMEOUT_USE_DEFAULT,
				   DBUS_TYPE_STRING, &id,
				   DBUS_TYPE_INT64, &pid,
				   DBUS_TYPE_INVALID);
	if( !msg ) {
		mce_log(LL_ERR, "requestSensor(%s): no reply", id);
		goto EXIT;
	}

	if( dbus_set_error_from_message(&err, msg) ) {
		mce_log(LL_ERR, "requestSensor(%s): error reply: %s: %s",
			id, err.name, err.message);
		goto EXIT;
	}

	// NOTE: sessionid is an 'int' so we should use DBUS_TYPE_INT64
	// on a 64-bit platform.
	if( !dbus_message_get_args(msg, &err,
				   DBUS_TYPE_INT32, &sid,
				   DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "requestSensor(%s): parse reply: %s: %s",
			id, err.name, err.message);
		goto EXIT;
	}

	if( sid == -1 ) {
		mce_log(LL_ERR, "requestSensor(%s): failed", id);
		goto EXIT;
	}

	res = (int)sid;
	mce_log(LL_INFO, "requestSensor(%s): session=%d", id, res);

EXIT:
	if( msg ) dbus_message_unref(msg);
	return res;
}

/** Issue release sensor IPC to sensord
 *
 * @param id        sensor name
 * @param sessionid sensord session id from mce_sensorfw_request_sensor()
 *
 * @return true on success, or false in case of errors
 */
static bool
mce_sensorfw_release_sensor(const char *id, int sessionid)
{
	bool         res = false;
	DBusMessage *msg = 0;
	DBusError    err = DBUS_ERROR_INIT;
	dbus_int64_t pid = getpid();
	dbus_int32_t sid = sessionid;
	dbus_bool_t  ack = FALSE;

	mce_log(LL_INFO, "releaseSensor(%s, %d)", id, sessionid);

	// FIXME: should not block ...

	msg = dbus_send_with_block(SENSORFW_SERVICE,
				   SENSORFW_PATH,
				   "local.SensorManager",
				   "releaseSensor",
				   DBUS_TIMEOUT_USE_DEFAULT,
				   DBUS_TYPE_STRING, &id,
				   DBUS_TYPE_INT32, &sid,
				   DBUS_TYPE_INT64, &pid,
				   DBUS_TYPE_INVALID);
	if( !msg ) {
		mce_log(LL_ERR, "releaseSensor(%s, %d): no reply", id,
			sessionid);
		goto EXIT;
	}

	if( dbus_set_error_from_message(&err, msg) ) {
		mce_log(LL_ERR, "releaseSensor(%s, %d): error reply: %s: %s",
			id, sessionid, err.name, err.message);
		goto EXIT;
	}

	if( !dbus_message_get_args(msg, &err,
				   DBUS_TYPE_BOOLEAN, &ack,
				   DBUS_TYPE_INVALID) ) {

		mce_log(LL_ERR, "releaseSensor(%s, %d): parse reply: %s: %s",
			id, sessionid, err.name, err.message);
		goto EXIT;
	}

	if( !ack ) {
		mce_log(LL_WARN, "releaseSensor(%s, %d): failed", id,
			sessionid);
		goto EXIT;
	}

	res = true;
	mce_log(LL_DEBUG, "releaseSensor(%s, %d): success", id, sessionid);

EXIT:
	if( msg ) dbus_message_unref(msg);
	return res;
}

/** Issue start sensor IPC to sensord
 *
 * @param id        sensor name
 * @param iface     D-Bus interface for the sensor
 * @param sessionid sensord session id from mce_sensorfw_request_sensor()
 *
 * @return true on success, or false in case of errors
 */
static bool
mce_sensorfw_start_sensor(const char *id, const char *iface, int sessionid)
{
	bool         res  = false;
	DBusMessage *msg  = 0;
	char        *path = 0;
	dbus_int32_t sid  = sessionid;
	DBusError    err  = DBUS_ERROR_INIT;

	mce_log(LL_INFO, "start(%s, %d)", id, sessionid);

	if( asprintf(&path, "%s/%s", SENSORFW_PATH, id) < 0 ) {
		path = 0;
		goto EXIT;
	}

	// FIXME: should not block ...

	msg = dbus_send_with_block(SENSORFW_SERVICE,
				   path,
				   iface,
				   "start",
				   DBUS_TIMEOUT_USE_DEFAULT,
				   DBUS_TYPE_INT32, &sid,
				   DBUS_TYPE_INVALID);
	if( !msg ) {
		mce_log(LL_ERR, "start(%s, %d): no reply", id, sessionid);
		goto EXIT;
	}

	if( dbus_set_error_from_message(&err, msg) ) {
		mce_log(LL_ERR, "start(%s, %d): error reply: %s: %s", id,
			sessionid, err.name, err.message);
		goto EXIT;
	}

	res = true;

EXIT:
	dbus_error_free(&err);

	if( msg ) dbus_message_unref(msg);

	free(path);

	return res;
}

/** Issue stop sensor IPC to sensord
 *
 * @param id        sensor name
 * @param iface     D-Bus interface for the sensor
 * @param sessionid sensord session id from mce_sensorfw_request_sensor()
 *
 * @return true on success, or false in case of errors
 */
static bool
mce_sensorfw_stop_sensor(const char *id, const char *iface, int sessionid)
{
	bool         res  = false;
	DBusMessage *msg  = 0;
	char        *path = 0;
	dbus_int32_t sid  = sessionid;
	DBusError    err  = DBUS_ERROR_INIT;

	mce_log(LL_INFO, "stop(%s, %d)",id, sessionid);

	if( asprintf(&path, "%s/%s", SENSORFW_PATH, id) < 0 ) {
		path = 0;
		goto EXIT;
	}

	// FIXME: should not block ...

	msg = dbus_send_with_block(SENSORFW_SERVICE,
				   path,
				   iface,
				   "stop",
				   DBUS_TIMEOUT_USE_DEFAULT,
				   DBUS_TYPE_INT32, &sid,
				   DBUS_TYPE_INVALID);
	if( !msg ) {
		mce_log(LL_ERR, "stop(%s, %d): no reply", id, sessionid);
		goto EXIT;
	}

	if( dbus_set_error_from_message(&err, msg) ) {
		mce_log(LL_ERR, "stop(%s, %d): error reply: %s: %s", id,
			sessionid, err.name, err.message);
		goto EXIT;
	}

	res = true;

EXIT:
	dbus_error_free(&err);

	if( msg ) dbus_message_unref(msg);

	free(path);

	return res;
}

/* ========================================================================= *
 * ALS
 * ========================================================================= */

/** Close ALS session with sensord
 */
static void mce_sensorfw_als_stop_session(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);

	if( als_sid != -1 ) {
		if( sensord_running )
			mce_sensorfw_release_sensor(als_name, als_sid);
		als_sid = -1;
	}

	if( als_wid ) {
		g_source_remove(als_wid), als_wid = 0;
	}

	als_have = false;
}

/** Have ALS session with sensord predicate
 *
 * @return true if session exists, or false if not
 */
static bool mce_sensorfw_als_have_session(void)
{
	return als_wid != 0;
}

/** Open ALS session with sensord
 *
 * @return true on success, or false in case of errors
 */
static bool mce_sensorfw_als_start_session(void)
{
	if( als_wid )
		goto EXIT;

	if( !mce_sensorfw_load_sensor(als_name) )
		goto EXIT;

	als_sid = mce_sensorfw_request_sensor(als_name);
	if( als_sid < 0 )
		goto EXIT;

	als_wid = mce_sensorfw_add_io_watch(als_sid, als_input_cb);
	if( als_wid == 0 )
		goto EXIT;

EXIT:
	if( !als_wid ) {
		// all or nothing
		mce_sensorfw_als_stop_session();
	}
	return als_wid != 0;
}

/** Rethink ALS enabled state
 */
static void
mce_sensorfw_als_rethink(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);
	if( !sensord_running ) {
		mce_log(LL_WARN, "skipping als enable/disable;"
			" sensord not available");
		goto EXIT;
	}

	if( als_want == als_have )
		goto EXIT;

	if( als_want ) {
		if( !mce_sensorfw_als_start_session() )
			goto EXIT;
		if( !mce_sensorfw_start_sensor(als_name, als_iface, als_sid) )
			goto EXIT;
	}
	else {
		if( !mce_sensorfw_als_have_session() )
			goto EXIT;

		if( !mce_sensorfw_stop_sensor(als_name, als_iface, als_sid) )
			goto EXIT;
	}

	als_have = als_want;
EXIT:
	return;
}

/** Try to enable ALS input
 */
void
mce_sensorfw_als_enable(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);
	als_want = true;
	mce_sensorfw_als_rethink();
}

/** Try to disable ALS input
 */
void
mce_sensorfw_als_disable(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);
	als_want = false;
	mce_sensorfw_als_rethink();
}

/** Set ALS notification callback
 *
 * @param cb function to call when ALS events are received
 */
void mce_sensorfw_als_set_notify(void (*cb)(unsigned lux))
{
	mce_log(LL_DEBUG, "@%s(%p)", __FUNCTION__, cb);
	als_notify = cb;
}

/* ========================================================================= *
 * PS
 * ========================================================================= */

/** Close PS session with sensord
 */
static void mce_sensorfw_ps_stop_session(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);

	if( ps_sid != -1 ) {
		if( sensord_running )
			mce_sensorfw_release_sensor(ps_name, ps_sid);
		ps_sid = -1;
	}

	if( ps_wid ) {
		g_source_remove(ps_wid), ps_wid = 0;
	}
	ps_have = false;
}

/** Have PS session with sensord predicate
 *
 * @return true if session exists, or false if not
 */
static bool mce_sensorfw_ps_have_session(void)
{
	return ps_wid != 0;
}

/** Open PS session with sensord
 *
 * @return true on success, or false in case of errors
 */
static bool mce_sensorfw_ps_start_session(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);

	if( ps_wid ) {
		goto EXIT;
	}

	if( !mce_sensorfw_load_sensor(ps_name) ) {
		goto EXIT;
	}

	ps_sid = mce_sensorfw_request_sensor(ps_name);
	if( ps_sid < 0 )
		goto EXIT;

	ps_wid = mce_sensorfw_add_io_watch(ps_sid, ps_input_cb);
	if( !ps_wid )
		goto EXIT;

EXIT:
	if( !ps_wid ) {
		// all or nothing
		mce_sensorfw_ps_stop_session();
	}
	return ps_wid != 0;
}

/** Rethink PS enabled state
 */
static void
mce_sensorfw_ps_rethink(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);
	if( !sensord_running ) {
		mce_log(LL_WARN, "sensord not on dbus;"
			" skipping ps enable/disable for now");
		goto EXIT;
	}

	if( ps_want == ps_have )
		goto EXIT;

	if( ps_want ) {
		if( !mce_sensorfw_ps_start_session() )
			goto EXIT;
		if( !mce_sensorfw_start_sensor(ps_name, ps_iface, ps_sid) )
			goto EXIT;
	}
	else {
		if( !mce_sensorfw_ps_have_session() )
			goto EXIT;

		if( !mce_sensorfw_stop_sensor(ps_name, ps_iface, ps_sid) )
			goto EXIT;
	}

	ps_have = ps_want;
EXIT:
	return;
}

/** Try to enable PS input
 */
void
mce_sensorfw_ps_enable(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);
	ps_want = true;
	mce_sensorfw_ps_rethink();
}

/** Try to disable PS input
 */
void
mce_sensorfw_ps_disable(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);
	ps_want = false;
	mce_sensorfw_ps_rethink();
}

/** Set PS notification callback
 *
 * @param cb function to call when PS events are received
 */
void mce_sensorfw_ps_set_notify(void (*cb)(bool covered))
{
	mce_log(LL_DEBUG, "@%s(%p)", __FUNCTION__, cb);
	ps_notify = cb;
}

/* ========================================================================= *
 * SENSORD
 * ========================================================================= */

/** React to sensord presense on D-Bus system bus
 *
 * If sensord has stopped (=lost dbus name), existing sensor
 * sessions are cleaned up.
 *
 * If sensord has started (=has dbus name), sensor sessions
 * are re-established as needed.
 *
 * @param running true if sensord is on dbus, false if not
 */
static void
xsensord_set_runstate(bool running)
{
	if( sensord_running != running ) {
		sensord_running = running;
		mce_log(LL_NOTICE, "sensord is %savailable on dbus",
			sensord_running ? "" : "NOT ");
		if( !sensord_running ) {
			mce_sensorfw_als_stop_session();
			mce_sensorfw_ps_stop_session();
		}
		else {
			mce_sensorfw_als_rethink();
			mce_sensorfw_ps_rethink();
		}
	}
}

/** Handle reply to asynchronous sensord service name ownership query
 *
 * @param pc        State data for asynchronous D-Bus method call
 * @param user_data (not used)
 */
static void xsensord_get_name_owner_cb(DBusPendingCall *pc, void *user_data)
{
	(void)user_data;

	DBusMessage *rsp   = 0;
	const char  *owner = 0;
	DBusError    err   = DBUS_ERROR_INIT;

	if( !(rsp = dbus_pending_call_steal_reply(pc)) )
		goto EXIT;

	if( dbus_set_error_from_message(&err, rsp) ||
	    !dbus_message_get_args(rsp, &err,
				   DBUS_TYPE_STRING, &owner,
				   DBUS_TYPE_INVALID) )
	{
		if( strcmp(err.name, DBUS_ERROR_NAME_HAS_NO_OWNER) ) {
			mce_log(LL_WARN, "%s: %s", err.name, err.message);
		}
		goto EXIT;
	}

	xsensord_set_runstate(owner && *owner);

EXIT:
	if( rsp ) dbus_message_unref(rsp);
	dbus_error_free(&err);
}

/** Initiate asynchronous sensord service name ownership query
 *
 * @return TRUE if the method call was initiated, or FALSE in case of errors
 */
static gboolean xsensord_get_name_owner(void)
{
	gboolean         res  = FALSE;
	DBusMessage     *req  = 0;
	DBusPendingCall *pc   = 0;
	const char      *name = SENSORFW_SERVICE;

	if( !(req = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
						 DBUS_PATH_DBUS,
						 DBUS_INTERFACE_DBUS,
						 "GetNameOwner")) )
		goto EXIT;

	if( !dbus_message_append_args(req,
				      DBUS_TYPE_STRING, &name,
				      DBUS_TYPE_INVALID) )
		goto EXIT;

	if( !dbus_connection_send_with_reply(systembus, req, &pc, -1) )
		goto EXIT;

	if( !dbus_pending_call_set_notify(pc, xsensord_get_name_owner_cb, 0, 0) )
		goto EXIT;

	res = TRUE;

EXIT:
	if( pc )  dbus_pending_call_unref(pc);
	if( req ) dbus_message_unref(req);

	return res;
}

/** Handle sensord name owner changed signals
 *
 * @param msg DBUS_NAME_OWNER_CHANGED_SIG from dbus daemon
 */
static void
xsensord_name_owner_changed(DBusMessage *msg)
{
	const char *name = 0;
	const char *prev = 0;
	const char *curr = 0;
	DBusError   err  = DBUS_ERROR_INIT;

	if( !dbus_message_get_args(msg, &err,
				   DBUS_TYPE_STRING, &name,
				   DBUS_TYPE_STRING, &prev,
				   DBUS_TYPE_STRING, &curr,
				   DBUS_TYPE_INVALID) )
	{
		mce_log(LL_WARN, "%s: %s", err.name, err.message);
		goto EXIT;
	}

	if( strcmp(name, SENSORFW_SERVICE) )
		goto EXIT;

	xsensord_set_runstate(curr && *curr);

EXIT:
	dbus_error_free(&err);
	return;
}

/** D-Bus message filter for handling sensord related signals
 *
 * @param con       (not used)
 * @param msg       message to be acted upon
 * @param user_data (not used)
 *
 * @return DBUS_HANDLER_RESULT_NOT_YET_HANDLED (other filters see the msg too)
 */
static DBusHandlerResult
xsensord_dbus_filter_cb(DBusConnection *con,
			    DBusMessage *msg,
			    void *user_data)
{
	(void)con; (void)user_data;

	DBusHandlerResult res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if( dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS,
				   DBUS_NAME_OWNER_CHANGED_SIG) ) {
		xsensord_name_owner_changed(msg);
	}

	return res;
}

/** Rule for matching sensord service name owner changes */
static const char xsensord_name_owner_rule[] =
"type='signal'"
",sender='"DBUS_SERVICE_DBUS"'"
",interface='"DBUS_INTERFACE_DBUS"'"
",member='"DBUS_NAME_OWNER_CHANGED_SIG"'"
",path='"DBUS_PATH_DBUS"'"
",arg0='"SENSORFW_SERVICE"'"
;

/* ========================================================================= *
 * MODULE
 * ========================================================================= */

/** Initialize mce sensorfw module
 */
bool
mce_sensorfw_init(void)
{
	mce_log(LL_INFO, "@%s()", __FUNCTION__);

	bool res = false;

	if( !(systembus = dbus_connection_get()) )
		goto EXIT;

	/* start tracking sensord name ownership changes on system bus */
	dbus_connection_add_filter(systembus, xsensord_dbus_filter_cb,
				   0, 0);
	dbus_bus_add_match(systembus, xsensord_name_owner_rule, 0);

	/* initiate async query to find out current state of sensord */
	xsensord_get_name_owner();

	res = true;
EXIT:
	return res;
}

/** Cleanup mce sensorfw module
 */
void
mce_sensorfw_quit(void)
{
	mce_log(LL_INFO, "@%s()", __FUNCTION__);

	mce_sensorfw_ps_stop_session();
	mce_sensorfw_als_stop_session();

	if( systembus ) {
		dbus_connection_remove_filter(systembus,
					      xsensord_dbus_filter_cb, 0);

		dbus_bus_remove_match(systembus, xsensord_name_owner_rule, 0);

		dbus_connection_unref(systembus), systembus = 0;
	}
}
