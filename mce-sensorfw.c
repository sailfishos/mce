#include "mce-sensorfw.h"
#include "mce.h"
#include "mce-log.h"
#include "mce-dbus.h"
#include "libwakelock.h"

#include <linux/input.h>

#include <sys/ioctl.h>

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

/** We need to differentiate multiple real and synthetic input sources */
typedef enum {
	/** Synthetic input, for example when sensord is not running */
	DS_FAKED,
	/** Data read directly from kernel */
	DS_EVDEV,
	/** Data received from sensord */
	DS_SENSORD,
	/** Dummy data, use the last known good value instead */
	DS_RESTORE,
} input_source_t;

/** Names of input sources; for debuggging purposes */
static const char * const input_source_name[] =
{
	[DS_FAKED]   = "SYNTH",
	[DS_EVDEV]   = "EVDEV",
	[DS_SENSORD] = "SENSORD",
	[DS_RESTORE] = "RESTORE",
};

/* ========================================================================= *
 * STATE DATA
 * ========================================================================= */

/** D-Bus System Bus connection */
static DBusConnection *systembus = 0;

/** Flag for sensord is on system bus */
static bool       sensord_running = false;

/** Flag for system is suspended */
static bool       system_suspended = false;

static void als_notify(unsigned lux, input_source_t srce);
static void ps_notify(bool covered, input_source_t srce);

/* ========================================================================= *
 * EVDEV HOOKS
 * ========================================================================= */

/** Callback function for processing evdev events
 *
 * @param chn  io channel
 * @param cnd  (not used)
 * @param aptr pointer to io watch
 *
 * @return TRUE to keep io watch active, FALSE to stop it
 */
static gboolean
mce_sensorfw_evdev_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
{
	gboolean  keep = FALSE;
	int      *id   = aptr;
	int       fd   = g_io_channel_unix_get_fd(chn);
	int       als  = -1;
	int       ps   = -1;
	int       rc;

	struct input_event eve[256];

	if( cnd & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) ) {
		goto EXIT;
	}

	/* wakelock must be taken before reading the data */
	wakelock_lock("mce_input_handler", -1);

	rc = read(fd, eve, sizeof eve);

	if( rc == -1 ) {
		if( errno == EINTR || errno == EAGAIN )
			keep = TRUE;
		else
			mce_log(LL_ERR, "read events: %m");
		goto EXIT;
	}

	if( rc == 0 ) {
		mce_log(LL_ERR, "read events: EOF");
		goto EXIT;
	}

	keep = TRUE;

	size_t n = rc / sizeof *eve;
	for( size_t i = 0; i < n; ++i ) {
		if( eve[i].type != EV_ABS )
			continue;

		switch( eve[i].code ) {
		case ABS_MISC:
			als = eve[i].value;
			break;
		case ABS_DISTANCE:
			ps = eve[i].value;
			break;
		default:
			break;
		}
	}

	if( als != -1 )
		als_notify(als, DS_EVDEV);
	if( ps != -1 )
		ps_notify(ps < 1, DS_EVDEV);

EXIT:
	if( !keep && *id ) {
		*id = 0;
		mce_log(LL_CRIT, "stopping io watch");
	}

	/* wakelock must be released when we are done with the data */
	wakelock_unlock("mce_input_handler");

	return keep;
}

/** Helper function for registering io watch
 *
 * @param fd   file descriptor to watch
 * @param cb   io watch callback function
 * @param aptr context to pass to the callback
 *
 * @return source id, or 0 in case of errors
 */
static guint
mce_sensorfw_start_iomon(int fd, GIOFunc cb, void *aptr)
{
	GIOChannel *chn = 0;
	guint       id  = 0;

	if( !(chn = g_io_channel_unix_new(fd)) ) {
		goto EXIT;
	}

	if( !(id = g_io_add_watch(chn,
				  G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
				  cb, aptr)) )
		goto EXIT;

	g_io_channel_set_close_on_unref(chn, TRUE), fd = -1;

EXIT:
	if( chn != 0 ) g_io_channel_unref(chn);
	if( fd != -1 ) close(fd);

	return id;
}

/* ========================================================================= *
 * COMMON
 * ========================================================================= */

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

	if( !(wid = g_io_add_watch(chn,
				   G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
				   datafunc, 0)) ) {
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
		mce_log(LL_ERR, "loadPlugin(%s): no reply", id);
		goto EXIT;
	}

	if( dbus_set_error_from_message(&err, msg) ) {
		mce_log(LL_ERR, "loadPlugin(%s): error reply: %s: %s",
			id, err.name, err.message);
		goto EXIT;
	}

	if( !dbus_message_get_args(msg, &err,
				   DBUS_TYPE_BOOLEAN, &ack,
				   DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "loadPlugin(%s): parse reply: %s: %s",
			id, err.name, err.message);
		goto EXIT;
	}

	if( !ack ) {
		mce_log(LL_WARN, "loadPlugin(%s): request denied", id);
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
	char        *path = 0;
	dbus_int32_t sid  = sessionid;

	mce_log(LL_INFO, "start(%s, %d)", id, sessionid);

	if( asprintf(&path, "%s/%s", SENSORFW_PATH, id) < 0 ) {
		path = 0;
		goto EXIT;
	}

	res = dbus_send(SENSORFW_SERVICE,
			path,
			iface,
			"start",
			NULL,
			DBUS_TYPE_INT32, &sid,
			DBUS_TYPE_INVALID);

EXIT:
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
	char        *path = 0;
	dbus_int32_t sid  = sessionid;

	mce_log(LL_INFO, "stop(%s, %d)",id, sessionid);

	if( asprintf(&path, "%s/%s", SENSORFW_PATH, id) < 0 ) {
		path = 0;
		goto EXIT;
	}

	res = dbus_send(SENSORFW_SERVICE,
			path,
			iface,
			"stop",
			NULL,
			DBUS_TYPE_INT32, &sid,
			DBUS_TYPE_INVALID);

EXIT:
	free(path);

	return res;
}

/** Callback for handling replies to setStandbyOverride requests
 *
 * This is used only for logging possible error replies we
 * might get from trying to set the standby override property.
 *
 * @param pc   pending call object
 * @param aptr (not used)
 */
static void
mce_sensorfw_set_standby_override_cb(DBusPendingCall *pc, void *aptr)
{
	(void)aptr;

	static const char method[] = "setStandbyOverride";

	DBusMessage  *rsp = 0;
	DBusError     err = DBUS_ERROR_INIT;
	dbus_bool_t   val = FALSE;

	mce_log(LL_INFO, "Received %s() reply", method);

	if( !(rsp = dbus_pending_call_steal_reply(pc)) )
		goto EXIT;

	if( dbus_set_error_from_message(&err, rsp) ) {
		mce_log(LL_ERR, "%s(): %s: %s", method, err.name, err.message);
		goto EXIT;
	}
	if( !dbus_message_get_args(rsp, &err,
				   DBUS_TYPE_BOOLEAN, &val,
				   DBUS_TYPE_INVALID) ) {
		mce_log(LL_ERR, "%s(): %s: %s", method, err.name, err.message);
		goto EXIT;
	}

	mce_log(LL_INFO, "%s() -> %s", method, val ? "success" : "failure");

EXIT:
	if( rsp ) dbus_message_unref(rsp);
	dbus_pending_call_unref(pc);
	dbus_error_free(&err);
}

/** Issue sensor standby override request to sensord
 *
 * @param id        sensor name
 * @param iface     D-Bus interface for the sensor
 * @param sessionid sensord session id from mce_sensorfw_request_sensor()
 * @param value     true to enable standby override, false to disable
 *
 * @return true on success, or false in case of errors
 */
static bool
mce_sensorfw_set_standby_override(const char *id, const char *iface,
				  int sessionid, bool value)
{
	bool         res  = false;
	char        *path = 0;
	dbus_int32_t sid  = sessionid;
	dbus_bool_t  val  = value;

	mce_log(LL_INFO, "setStandbyOverride(%s, %d, %d)",
		id, sessionid, val);

	if( asprintf(&path, "%s/%s", SENSORFW_PATH, id) < 0 ) {
		path = 0;
		goto EXIT;
	}

	res = dbus_send(SENSORFW_SERVICE,
			path,
			iface,
			"setStandbyOverride",
			mce_sensorfw_set_standby_override_cb,
			DBUS_TYPE_INT32, &sid,
			DBUS_TYPE_BOOLEAN, &val,
			DBUS_TYPE_INVALID);

EXIT:
	free(path);

	return res;
}

/* ========================================================================= *
 * ALS
 * ========================================================================= */

/** io watch id for ALS evdev file descriptor*/
static guint als_evdev_id = 0;

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
static void     (*als_notify_cb)(unsigned lux) = 0;

/** Ambient light value to report when sensord is not available */
#define ALS_VALUE_WHEN_SENSORD_IS_DOWN 400

/** Wrapper for als_notify_cb hook
 *
 * @param lux  ambient light value
 * @param srce where does to value originate from
 */
static void
als_notify(unsigned lux, input_source_t srce)
{
	static unsigned als_lux_last = ALS_VALUE_WHEN_SENSORD_IS_DOWN;

	if( srce == DS_RESTORE )
		lux = als_lux_last;

	/* If we have evdev source, prefer that over sensord input */
	if( als_evdev_id ) {
		if( srce == DS_EVDEV )
			als_lux_last = lux;

		if( srce == DS_SENSORD ) {
			if( lux != als_lux_last )
				mce_log(LL_DEBUG, "sensord=%u vs evdev=%u",
					lux, als_lux_last);
			goto EXIT;
		}

	}
	else {
		if( srce == DS_SENSORD )
			als_lux_last = lux;

		if( srce == DS_EVDEV )
			goto EXIT;
	}

	mce_log(LL_DEBUG, "ALS: %u lux (%s)", lux,input_source_name[srce]);

	if( als_notify_cb )
		als_notify_cb(lux);
	else if( srce != DS_FAKED )
		mce_log(LL_INFO, "ALS data without notify cb");
EXIT:
	return;
}

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

	(void)aptr; // unused

	gboolean keep_going = FALSE;
	uint32_t count = 0;

	int fd;
	int rc;
	als_data_t data;

	if( cnd & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) ) {
		goto EXIT;
	}

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

	mce_log(LL_DEBUG, "Got %u ALS values", (unsigned)count);

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
	als_notify(data.value, DS_SENSORD);

	keep_going = TRUE;

EXIT:
	if( !keep_going ) {
		mce_log(LL_CRIT, "disabling io watch");
		als_wid = 0;
	}
	return keep_going;
}

/** Handle reply to initial ALS value request
 *
 * @param pc  pending call object
 * @param aptr (not used)
 */
static void mce_sensorfw_als_read_cb(DBusPendingCall *pc, void *aptr)
{
	(void)aptr;

	mce_log(LL_INFO, "Received intial ALS lux reply");

	bool          res = false;
	DBusMessage  *rsp = 0;
	DBusError     err = DBUS_ERROR_INIT;
	dbus_uint64_t tck = 0;
	dbus_uint32_t lux = 0;

	DBusMessageIter body, var, rec;

	if( !(rsp = dbus_pending_call_steal_reply(pc)) )
		goto EXIT;

	if( dbus_set_error_from_message(&err, rsp) ) {
		mce_log(LL_ERR, "als lux error reply: %s: %s",
			err.name, err.message);
		goto EXIT;
	}

	if( !dbus_message_iter_init(rsp, &body) )
		goto EXIT;

	if( dbus_message_iter_get_arg_type(&body) != DBUS_TYPE_VARIANT )
		goto EXIT;

	dbus_message_iter_recurse(&body, &var);
	dbus_message_iter_next(&body);
	if( dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRUCT )
		goto EXIT;

	dbus_message_iter_recurse(&var, &rec);
	dbus_message_iter_next(&var);
	if( dbus_message_iter_get_arg_type(&rec) !=  DBUS_TYPE_UINT64 )
		goto EXIT;

	dbus_message_iter_get_basic(&rec, &tck);
	dbus_message_iter_next(&rec);
	if( dbus_message_iter_get_arg_type(&rec) !=  DBUS_TYPE_UINT32 )
		goto EXIT;

	dbus_message_iter_get_basic(&rec, &lux);
	dbus_message_iter_next(&rec);

	mce_log(LL_INFO, "initial ALS value = %u", lux);
	als_notify(lux, DS_SENSORD);

	res = true;

EXIT:
	if( !res ) mce_log(LL_WARN, "did not get initial lux value");
	if( rsp ) dbus_message_unref(rsp);
	dbus_pending_call_unref(pc);
	dbus_error_free(&err);
}

/** Issue get ALS value IPC to sensord
 *
 * @param id        sensor name
 * @param iface     D-Bus interface for the sensor
 * @param sessionid sensord session id from mce_sensorfw_request_sensor()
 */
static void
mce_sensorfw_als_read(const char *id, const char *iface, int sessionid)
{
	(void)sessionid;

	char *path = 0;
	const char *prop = "lux";

	mce_log(LL_INFO, "read(%s, %d)", id, sessionid);

	if( asprintf(&path, "%s/%s", SENSORFW_PATH, id) < 0 ) {
		path = 0;
		goto EXIT;
	}

	dbus_send(SENSORFW_SERVICE,
		  path,
		  "org.freedesktop.DBus.Properties",
		  "Get",
		  mce_sensorfw_als_read_cb,
		  DBUS_TYPE_STRING, &iface,
		  DBUS_TYPE_STRING, &prop,
		  DBUS_TYPE_INVALID);
EXIT:
	free(path);
}

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
		als_notify(ALS_VALUE_WHEN_SENSORD_IS_DOWN, DS_FAKED);
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

/** Enable ALS via sensord
 */
static void mce_sensorfw_als_start_sensor(void)
{
	if( als_have )
		goto EXIT;

	if( !mce_sensorfw_als_start_session() )
		goto EXIT;

	if( !mce_sensorfw_start_sensor(als_name, als_iface, als_sid) )
		goto EXIT;

	/* ALS is used in lpm display states; from sensord point of view
	 * this means display is off and thus we need to set the standby
	 * override flag */

	/* No error checking here; failures will be logged when
	 * we get reply message from sensord */
	mce_sensorfw_set_standby_override(als_name, als_iface, als_sid, true);

	als_have = true;

	/* There is no quarantee that we get sensor input
	 * anytime soon, so we make an explicit get current
	 * value request after starting sensor */
	mce_sensorfw_als_read(als_name, als_iface, als_sid);

EXIT:
	return;
}

/** Disable ALS via sensord
 */
static void mce_sensorfw_als_stop_sensor(void)
{
	if( !als_have )
		goto EXIT;

	if( mce_sensorfw_als_have_session() )
		mce_sensorfw_stop_sensor(als_name, als_iface, als_sid);

	als_have = false;

EXIT:
	return;
}

/** Rethink ALS enabled state
 */
static void
mce_sensorfw_als_rethink(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);
	if( !sensord_running ) {
		mce_log(LL_NOTICE, "skipping als enable/disable;"
			" sensord not available");
		goto EXIT;
	}

	if( als_want == als_have )
		goto EXIT;

	if( system_suspended ) {
		mce_log(LL_NOTICE, "skipping als enable/disable;"
			" should be suspended");
		goto EXIT;
	}

	if( als_want ) {
		als_notify(0, DS_RESTORE);
		mce_sensorfw_als_start_sensor();
	}
	else
		mce_sensorfw_als_stop_sensor();
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

	if( (als_notify_cb = cb) ) {
		if( !sensord_running )
			als_notify(ALS_VALUE_WHEN_SENSORD_IS_DOWN, DS_FAKED);
		else
			als_notify(0, DS_RESTORE);
	}
}

/* ========================================================================= *
 * PS
 * ========================================================================= */

/** io watch id for PS evdev file descriptor*/
static guint ps_evdev_id = 0;

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
static void     (*ps_notify_cb)(bool covered) = 0;

/** Proximity state to report when sensord is not available */
#define PS_STATE_WHEN_SENSORD_IS_DOWN false
/** Wrapper for ps_notify_cb hook
 *
 * @param covered proximity sensor state
 * @param srce    where does to value originate from
 */
static void
ps_notify(bool covered, input_source_t srce)
{
	static bool ps_covered_last = PS_STATE_WHEN_SENSORD_IS_DOWN;

	if( srce == DS_RESTORE )
		covered = ps_covered_last;

	/* If we have evdev source, prefer that over sensord input */
	if( ps_evdev_id ) {
		if( srce == DS_EVDEV )
			ps_covered_last = covered;

		if( srce == DS_SENSORD ) {
			if( covered != ps_covered_last )
				mce_log(LL_WARN, "sensord=%u vs evdev=%u",
					covered, ps_covered_last);
			goto EXIT;
		}

	}
	else {
		if( srce == DS_SENSORD )
			ps_covered_last = covered;

		if( srce == DS_EVDEV )
			goto EXIT;
	}

	mce_log(LL_DEVEL, "PS: %scovered (%s)",
		covered ? "" : "not-", input_source_name[srce]);

	if( ps_notify_cb )
		ps_notify_cb(covered);
	else if( srce != DS_FAKED )
		mce_log(LL_INFO, "PS data without notify cb");
EXIT:
	return;
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

	(void)aptr; // unused

	gboolean keep_going = FALSE;
	uint32_t count = 0;

	int fd;
	int rc;
	ps_data_t data;

	if( cnd & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) ) {
		goto EXIT;
	}

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

	ps_notify(data.withinProximity != 0, DS_SENSORD);

	keep_going = TRUE;

EXIT:
	if( !keep_going ) {
		mce_log(LL_CRIT, "disabling io watch");
		ps_wid = 0;
	}
	return keep_going;
}

/** Handle reply to initial PS value request
 *
 * @param pc  pending call object
 * @param aptr (not used)
 */
static void mce_sensorfw_ps_read_cb(DBusPendingCall *pc, void *aptr)
{
	(void)aptr;

	mce_log(LL_INFO, "Received intial PS distance reply");

	bool          res = false;
	DBusMessage  *rsp = 0;
	DBusError     err = DBUS_ERROR_INIT;
	dbus_uint64_t tck = 0;
	dbus_uint32_t dst = 0;

	DBusMessageIter body, var, rec;

	if( !(rsp = dbus_pending_call_steal_reply(pc)) )
		goto EXIT;

	if( dbus_set_error_from_message(&err, rsp) ) {
		mce_log(LL_ERR, "proximity error reply: %s: %s",
			err.name, err.message);
		goto EXIT;
	}

	if( !dbus_message_iter_init(rsp, &body) )
		goto EXIT;

	if( dbus_message_iter_get_arg_type(&body) != DBUS_TYPE_VARIANT )
		goto EXIT;

	dbus_message_iter_recurse(&body, &var);
	dbus_message_iter_next(&body);
	if( dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRUCT )
		goto EXIT;

	dbus_message_iter_recurse(&var, &rec);
	dbus_message_iter_next(&var);
	if( dbus_message_iter_get_arg_type(&rec) !=  DBUS_TYPE_UINT64 )
		goto EXIT;

	dbus_message_iter_get_basic(&rec, &tck);
	dbus_message_iter_next(&rec);
	if( dbus_message_iter_get_arg_type(&rec) !=  DBUS_TYPE_UINT32 )
		goto EXIT;

	dbus_message_iter_get_basic(&rec, &dst);
	dbus_message_iter_next(&rec);

	mce_log(LL_NOTICE, "initial PS value = %u", dst);

	ps_notify(dst < 1, DS_SENSORD);

	res = true;

EXIT:
	if( !res ) mce_log(LL_WARN, "did not get initial proximity value");
	if( rsp ) dbus_message_unref(rsp);
	dbus_pending_call_unref(pc);
	dbus_error_free(&err);
}

/** Issue get PS value IPC to sensord
 *
 * @param id        sensor name
 * @param iface     D-Bus interface for the sensor
 * @param sessionid sensord session id from mce_sensorfw_request_sensor()
 */
static void
mce_sensorfw_ps_read(const char *id, const char *iface, int sessionid)
{
	(void)sessionid;

	char *path = 0;
	const char *prop = "proximity";

	mce_log(LL_INFO, "read(%s, %d)", id, sessionid);

	if( asprintf(&path, "%s/%s", SENSORFW_PATH, id) < 0 ) {
		path = 0;
		goto EXIT;
	}

	dbus_send(SENSORFW_SERVICE,
		  path,
		  "org.freedesktop.DBus.Properties",
		  "Get",
		  mce_sensorfw_ps_read_cb,
		  DBUS_TYPE_STRING, &iface,
		  DBUS_TYPE_STRING, &prop,
		  DBUS_TYPE_INVALID);
EXIT:
	free(path);
}

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

		ps_notify(PS_STATE_WHEN_SENSORD_IS_DOWN, DS_FAKED);
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

/** Enable PS via sensord
 */
static void mce_sensorfw_ps_start_sensor(void)
{
	if( ps_have )
		goto EXIT;

	if( !mce_sensorfw_ps_start_session() )
		goto EXIT;

	if( !mce_sensorfw_start_sensor(ps_name, ps_iface, ps_sid) )
		goto EXIT;

	/* No error checking here; failures will be logged when
	 * we get reply message from sensord */
	mce_sensorfw_set_standby_override(ps_name, ps_iface, ps_sid, true);

	ps_have = true;

	/* There is no quarantee that we get sensor input
	 * anytime soon, so we make an explicit get current
	 * value request after starting sensor */
	mce_sensorfw_ps_read(ps_name, ps_iface, ps_sid);
EXIT:
	return;
}

/** Disable PS via sensord
 */
static void mce_sensorfw_ps_stop_sensor(void)
{
	if( !ps_have )
		goto EXIT;

	if( mce_sensorfw_ps_have_session() ) {
		mce_sensorfw_set_standby_override(ps_name, ps_iface, ps_sid,
						  false);
		mce_sensorfw_stop_sensor(ps_name, ps_iface, ps_sid);
	}

	ps_have = false;
EXIT:
	return;
}

/** Rethink PS enabled state
 */
static void
mce_sensorfw_ps_rethink(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);
	if( !sensord_running ) {
		mce_log(LL_NOTICE, "skipping ps enable/disable;"
			" sensord not available");
		goto EXIT;
	}

	if( ps_want == ps_have )
		goto EXIT;

	if( system_suspended ) {
		mce_log(LL_NOTICE, "skipping ps enable/disable;"
			" should be suspended");
		goto EXIT;
	}

	if( ps_want ) {
		ps_notify(0, DS_RESTORE);
		mce_sensorfw_ps_start_sensor();
	}
	else
		mce_sensorfw_ps_stop_sensor();

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
	if( (ps_notify_cb = cb) ) {
		if( !sensord_running )
			ps_notify(PS_STATE_WHEN_SENSORD_IS_DOWN, DS_FAKED);
		else
			ps_notify(0, DS_RESTORE);
	}
}

/* ========================================================================= *
 * Orientation
 * ========================================================================= */

/** Orientation data block as sensord sends them */
typedef struct orient_data {
	uint64_t timestamp; /* microseconds, monotonic */
	int32_t  state;
} orient_data_t;

static const char * const orient_state_lut[] =
{
	[MCE_ORIENTATION_UNDEFINED]   = "Undefined",
	[MCE_ORIENTATION_LEFT_UP]     = "LeftUp",
	[MCE_ORIENTATION_RIGHT_UP]    = "RightUp",
	[MCE_ORIENTATION_BOTTOM_UP]   = "BottomUp",
	[MCE_ORIENTATION_BOTTOM_DOWN] = "BottomDown",
	[MCE_ORIENTATION_FACE_DOWN]   = "FaceDown",
	[MCE_ORIENTATION_FACE_UP]     = "FaceUp",
};
static const char *orient_state_name(int state)
{
	const char *res = 0;

	size_t count = sizeof orient_state_lut / sizeof *orient_state_lut;

	if( (size_t) state < count )
		res = orient_state_lut[state];

	return res ?: "Unknown";
}

/** Sensord name for Orientation */
static const char orient_name[]  = "orientationsensor";

/** Sensord D-Bus interface for Orientation */
static const char orient_iface[] = "local.OrientationSensor";

/** Sensord session id for Orientation */
static int        orient_sid     = -1;

/** Input watch for Orientation data */
static guint      orient_wid     = 0;

/** Flag for MCE wants to enable Orientation */
static bool       orient_want    = false;

/** Flag for Orientation enabled at sensord */
static bool       orient_have    = false;

/** Callback for sending Orientation data to where it is needed */
static void     (*orient_notify_cb)(int state) = 0;

/** Orientation state to report when sensord is not available */
#define ORIENT_STATE_WHEN_SENSORD_IS_DOWN MCE_ORIENTATION_UNDEFINED

/** Wrapper for orient_notify_cb hook
 */
static void
orient_notify(int state, bool synthetic)
{
	mce_log(LL_DEBUG, "orientation: %d / %s%s", state,
		orient_state_name(state),
		synthetic ? " (fake event)" : "");

	if( orient_notify_cb )
		orient_notify_cb(state);
	else if( !synthetic )
		mce_log(LL_WARN, "orientation enabled without notify cb");
}

/** Handle Orientation input from sensord
 *
 * @param chn  io channel
 * @param cnd  (not used)
 * @param aptr (not used)
 *
 * @return TRUE to keep io watch, or FALSE to remove it
 */
static gboolean
orient_input_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);

	(void)aptr; // unused

	gboolean keep_going = FALSE;
	uint32_t count = 0;

	int fd;
	int rc;
	orient_data_t data;

	if( cnd & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) ) {
		goto EXIT;
	}

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

	mce_log(LL_DEBUG, "Got %u orientation values", (unsigned)count);

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

	mce_log(LL_DEBUG, "last orientation value = %u", data.state);
	orient_notify(data.state, false);

	keep_going = TRUE;

EXIT:
	if( !keep_going ) {
		mce_log(LL_CRIT, "disabling io watch");
		orient_wid = 0;
	}
	return keep_going;
}

/** Handle reply to initial Orientation value request
 *
 * @param pc  pending call object
 * @param aptr (not used)
 */
static void mce_sensorfw_orient_read_cb(DBusPendingCall *pc, void *aptr)
{
	(void)aptr;

	mce_log(LL_INFO, "Received intial Orientation distance reply");

	bool          res = false;
	DBusMessage  *rsp = 0;
	DBusError     err = DBUS_ERROR_INIT;
	dbus_uint64_t tck = 0;
	dbus_uint32_t val = 0;

	DBusMessageIter body, var, rec;

	if( !(rsp = dbus_pending_call_steal_reply(pc)) )
		goto EXIT;

	if( dbus_set_error_from_message(&err, rsp) ) {
		mce_log(LL_ERR, "orientation error reply: %s: %s",
			err.name, err.message);
		goto EXIT;
	}

	if( !dbus_message_iter_init(rsp, &body) )
		goto EXIT;

	if( dbus_message_iter_get_arg_type(&body) != DBUS_TYPE_VARIANT )
		goto EXIT;

	dbus_message_iter_recurse(&body, &var);
	dbus_message_iter_next(&body);
	if( dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRUCT )
		goto EXIT;

	dbus_message_iter_recurse(&var, &rec);
	dbus_message_iter_next(&var);
	if( dbus_message_iter_get_arg_type(&rec) !=  DBUS_TYPE_UINT64 )
		goto EXIT;

	dbus_message_iter_get_basic(&rec, &tck);
	dbus_message_iter_next(&rec);
	if( dbus_message_iter_get_arg_type(&rec) !=  DBUS_TYPE_UINT32 )
		goto EXIT;

	dbus_message_iter_get_basic(&rec, &val);
	dbus_message_iter_next(&rec);

	mce_log(LL_INFO, "initial orientation value = %u", val);

	orient_notify(val, false);

	res = true;

EXIT:
	if( !res ) mce_log(LL_WARN, "did not get initial orientation value");
	if( rsp ) dbus_message_unref(rsp);
	dbus_pending_call_unref(pc);
	dbus_error_free(&err);
}

/** Issue get Orientation value IPC to sensord
 *
 * @param id        sensor name
 * @param iface     D-Bus interface for the sensor
 * @param sessionid sensord session id from mce_sensorfw_request_sensor()
 */
static void
mce_sensorfw_orient_read(const char *id, const char *iface, int sessionid)
{
	(void)sessionid;

	char *path = 0;
	const char *prop = "orientation";

	mce_log(LL_INFO, "read(%s, %d)", id, sessionid);

	if( asprintf(&path, "%s/%s", SENSORFW_PATH, id) < 0 ) {
		path = 0;
		goto EXIT;
	}

	dbus_send(SENSORFW_SERVICE,
		  path,
		  "org.freedesktop.DBus.Properties",
		  "Get",
		  mce_sensorfw_orient_read_cb,
		  DBUS_TYPE_STRING, &iface,
		  DBUS_TYPE_STRING, &prop,
		  DBUS_TYPE_INVALID);
EXIT:
	free(path);
}

/** Close Orientation session with sensord
 */
static void mce_sensorfw_orient_stop_session(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);

	if( orient_sid != -1 ) {
		if( sensord_running )
			mce_sensorfw_release_sensor(orient_name, orient_sid);
		orient_sid = -1;
	}

	if( orient_wid ) {
		g_source_remove(orient_wid), orient_wid = 0;
		orient_notify(ORIENT_STATE_WHEN_SENSORD_IS_DOWN, true);
	}
	orient_have = false;
}

/** Have Orientation session with sensord predicate
 *
 * @return true if session exists, or false if not
 */
static bool mce_sensorfw_orient_have_session(void)
{
	return orient_wid != 0;
}

/** Open Orientation session with sensord
 *
 * @return true on success, or false in case of errors
 */
static bool mce_sensorfw_orient_start_session(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);

	if( orient_wid ) {
		goto EXIT;
	}

	if( !mce_sensorfw_load_sensor(orient_name) ) {
		goto EXIT;
	}

	orient_sid = mce_sensorfw_request_sensor(orient_name);
	if( orient_sid < 0 )
		goto EXIT;

	orient_wid = mce_sensorfw_add_io_watch(orient_sid, orient_input_cb);
	if( !orient_wid )
		goto EXIT;

EXIT:
	if( !orient_wid ) {
		// all or nothing
		mce_sensorfw_orient_stop_session();
	}
	return orient_wid != 0;
}

/** Enable Orientation via sensord
 */
static void mce_sensorfw_orient_start_sensor(void)
{
	if( orient_have )
		goto EXIT;

	if( !mce_sensorfw_orient_start_session() )
		goto EXIT;

	if( !mce_sensorfw_start_sensor(orient_name, orient_iface, orient_sid) )
		goto EXIT;

	/* No error checking here; failures will be logged when
	 * we get reply message from sensord */
	mce_sensorfw_set_standby_override(orient_name, orient_iface, orient_sid, true);

	orient_have = true;

	/* There is no quarantee that we get sensor input
	 * anytime soon, so we make an explicit get current
	 * value request after starting sensor */
	mce_sensorfw_orient_read(orient_name, orient_iface, orient_sid);
EXIT:
	return;
}

/** Disable Orientation via sensord
 */
static void mce_sensorfw_orient_stop_sensor(void)
{
	if( !orient_have )
		goto EXIT;

	if( mce_sensorfw_orient_have_session() ) {
		mce_sensorfw_set_standby_override(orient_name, orient_iface, orient_sid,
						  false);
		mce_sensorfw_stop_sensor(orient_name, orient_iface, orient_sid);
	}

	orient_have = false;
	orient_notify(ORIENT_STATE_WHEN_SENSORD_IS_DOWN, true);
EXIT:
	return;
}

/** Rethink Orientation enabled state
 */
static void
mce_sensorfw_orient_rethink(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);
	if( !sensord_running ) {
		mce_log(LL_NOTICE, "skipping ps enable/disable;"
			" sensord not available");
		goto EXIT;
	}

	if( orient_want == orient_have )
		goto EXIT;

	if( system_suspended ) {
		mce_log(LL_NOTICE, "skipping ps enable/disable;"
			" should be suspended");
		goto EXIT;
	}

	if( orient_want )
		mce_sensorfw_orient_start_sensor();
	else
		mce_sensorfw_orient_stop_sensor();

EXIT:
	return;
}

/** Try to enable Orientation input
 */
void
mce_sensorfw_orient_enable(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);
	orient_want = true;
	mce_sensorfw_orient_rethink();
}

/** Try to disable Orientation input
 */
void
mce_sensorfw_orient_disable(void)
{
	mce_log(LL_DEBUG, "@%s()", __FUNCTION__);
	orient_want = false;
	mce_sensorfw_orient_rethink();
}

/** Set Orientation notification callback
 *
 * @param cb function to call when Orientation events are received
 */
void mce_sensorfw_orient_set_notify(void (*cb)(int covered))
{
	mce_log(LL_DEBUG, "@%s(%p)", __FUNCTION__, cb);
	orient_notify_cb = cb;
	if( !sensord_running )
		orient_notify(ORIENT_STATE_WHEN_SENSORD_IS_DOWN, true);
}

/* ========================================================================= *
 * SENSORD
 * ========================================================================= */

/* FIXME: Re-enabling proximity sensor while it is covered
 *        produces bogus data -> can't disable PS yet. This
 *        hack allows testing without mce recompilation
 */
static bool stop_ps_on_suspend(void)
{
	return access("/var/lib/mce/stop-ps", F_OK) == 0;
}

static void xsensord_rethink(void)
{
	static bool was_suspended = false;

	if( !sensord_running ) {
		mce_sensorfw_orient_stop_session();
		mce_sensorfw_als_stop_session();
		mce_sensorfw_ps_stop_session();
	}
	else if( system_suspended ) {
		mce_sensorfw_orient_stop_sensor();
		mce_sensorfw_als_stop_sensor();
		if( stop_ps_on_suspend() )
			mce_sensorfw_ps_stop_sensor();
	}
	else {
		mce_sensorfw_als_rethink();
		mce_sensorfw_ps_rethink();
		mce_sensorfw_orient_rethink();
	}
	if( system_suspended && !was_suspended ) {
		/* test callback pointer here too to avoid warning */
		if( stop_ps_on_suspend() && ps_notify_cb  ) {
			mce_log(LL_DEBUG, "faking proximity closed");
			ps_notify(true, DS_FAKED);
		}
	}

	was_suspended = system_suspended;
}

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
		xsensord_rethink();
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

	if( !pc )
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

#define TRACK_ALS_DATAPIPE 0
#define TRACK_PS_DATAPIPE  1

#if TRACK_ALS_DATAPIPE
/** Debug: how ALS shows up in mce state machines */
static void ambient_light_sensor_trigger(gconstpointer data)
{
	int lux = GPOINTER_TO_INT(data);
	mce_log(LL_NOTICE, "AMBIENT_LIGHT=%d", lux);
}
#endif

#if TRACK_PS_DATAPIPE
/** Debug: how PS shows up in mce state machines */
static void proximity_sensor_trigger(gconstpointer data)
{
	cover_state_t proximity_sensor_state = GPOINTER_TO_INT(data);

	const char *tag = "UNKNOWN";
	switch( proximity_sensor_state ) {
	case COVER_CLOSED: tag = "COVERED";     break;
	case COVER_OPEN:   tag = "NOT-COVERED"; break;
	default: break;
	}
	mce_log(LL_NOTICE, "PROXIMITY=%s", tag);
}
#endif

/** Initialize mce sensorfw module
 */
bool
mce_sensorfw_init(void)
{
	mce_log(LL_INFO, "@%s()", __FUNCTION__);

	bool res = false;

#if TRACK_ALS_DATAPIPE
	append_output_trigger_to_datapipe(&ambient_light_sensor_pipe,
					  ambient_light_sensor_trigger);
#endif

#if TRACK_PS_DATAPIPE
	append_output_trigger_to_datapipe(&proximity_sensor_pipe,
					  proximity_sensor_trigger);
#endif

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

	/* release evdev inputs */
	if( ps_evdev_id )
		g_source_remove(ps_evdev_id), ps_evdev_id = 0;

	if( als_evdev_id )
		g_source_remove(als_evdev_id), als_evdev_id = 0;

	/* remove datapipe triggers */
#if TRACK_ALS_DATAPIPE
	remove_output_trigger_from_datapipe(&ambient_light_sensor_pipe,
					    ambient_light_sensor_trigger);
#endif

#if TRACK_PS_DATAPIPE
	remove_output_trigger_from_datapipe(&proximity_sensor_pipe,
					    proximity_sensor_trigger);
#endif

	/* clean up sensord connection */
	mce_sensorfw_ps_stop_session();
	mce_sensorfw_als_stop_session();
	mce_sensorfw_orient_stop_session();

	if( systembus ) {
		dbus_connection_remove_filter(systembus,
					      xsensord_dbus_filter_cb, 0);

		dbus_bus_remove_match(systembus, xsensord_name_owner_rule, 0);

		dbus_connection_unref(systembus), systembus = 0;
	}
}

/** Prepare sensors for suspending
 */
void
mce_sensorfw_suspend(void)
{
	if( !system_suspended && stop_ps_on_suspend() ) {
		system_suspended = true;
		mce_log(LL_INFO, "@%s()", __FUNCTION__);
		xsensord_rethink();

		/* FIXME: This neither blocks nor is immediate,
		 *        so need to add asynchronous notification
		 *        when the dbus roundtrip to sensord has
		 *        been done.
		 */
	}
}

/** Rethink sensors after resuming
 */
void
mce_sensorfw_resume(void)
{
	if( system_suspended ) {
		system_suspended = false;
		mce_log(LL_INFO, "@%s()", __FUNCTION__);
		xsensord_rethink();
	}
}

/** Use evdev file descriptor as ALS data source
 *
 * Called from evdev probing if ALS device node is detected
 *
 * @param fd file descriptor
 */
void mce_sensorfw_als_attach(int fd)
{
	struct input_absinfo info;
	memset(&info, 0, sizeof info);

	/* Note: als_evdev_id must be set before calling als_notify() */
	als_evdev_id = mce_sensorfw_start_iomon(fd, mce_sensorfw_evdev_cb,
						&als_evdev_id);

	if( ioctl(fd, EVIOCGABS(ABS_MISC), &info) == -1 )
		mce_log(LL_ERR, "EVIOCGABS(%s): %m", "ABS_MISC");
	else {
		mce_log(LL_INFO, "ALS: %d (initial)", info.value);
		als_notify(info.value, DS_EVDEV);
	}

}

/** Use evdev file descriptor as PS data source
 *
 * Called from evdev probing if PS device node is detected
 *
 * @param fd file descriptor
 */
void mce_sensorfw_ps_attach(int fd)
{
	struct input_absinfo info;
	memset(&info, 0, sizeof info);

	/* Note: ps_evdev_id must be set before calling ps_notify() */
	ps_evdev_id = mce_sensorfw_start_iomon(fd, mce_sensorfw_evdev_cb,
					       &ps_evdev_id);

	if( ioctl(fd, EVIOCGABS(ABS_DISTANCE), &info) == -1 )
		mce_log(LL_ERR, "EVIOCGABS(%s): %m", "ABS_DISTANCE");
	else {
		mce_log(LL_NOTICE, "PS: %d (initial)", info.value);
		ps_notify(info.value < 1, DS_EVDEV);
	}

}
