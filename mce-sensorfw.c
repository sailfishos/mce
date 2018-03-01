/**
 * @file mce-sensorfw.c
 *
 * Mode Control Entity - Interprocess communication with sensord
 *
 * <p>
 *
 * Copyright (C) 2013-2014 Jolla Ltd.
 *
 * <p>
 *
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

#include "mce-sensorfw.h"

#include "mce.h"
#include "mce-log.h"
#include "mce-dbus.h"
#include "libwakelock.h"

#include <linux/input.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>

/* ========================================================================= *
 *
 * Internal Layering - Top-Down
 *
 * ========================================================================= *
 *
 * SENSORFW_MODULE:
 * - implements internal mce interface defined in mce-sensorfw.h
 *
 * SENSORFW_SERVICE:
 * - state machine that tracks availablity of sensord on system bus
 * - availalibility changes trigger activity in SENSORFW_PLUGIN
 *
 * SENSORFW_PLUGIN:
 * - state machine that takes care of sensord plugin loading ipc
 * - one instance / sensor
 * - finishing plugin loading triggers activity in SENSORFW_SESSION
 *
 * SENSORFW_SESSION:
 * - state machine that takes care of data session start ipc
 * - one instance / sensor
 * - acquiring a session id triggers activity in SENSORFW_CONNECTION
 *
 * SENSORFW_CONNECTION:
 * - state machine that takes care of data connection with sensord
 * - one instance / sensor
 * - establishing a connection triggers activity in SENSORFW_OVERRIDE
 *   and SENSORFW_REPORTING
 *
 * SENSORFW_OVERRIDE:
 * - state machine that takes care of standby override ipc
 * - one instance / sensor
 *
 *
 * SENSORFW_REPORTING:
 * - state machine that takes care of sensor start/stop ipc
 * - one instance / sensor
 * - value changes reported via sensor specific SENSORFW_BACKEND
 *
 * SENSORFW_BACKEND:
 * - contains all sensor specific constants and logic
 *
 * SENSORFW_NOTIFY:
 * - manages cached sensor state
 * - provides default values used when sensord is not available
 * - feeds sensor data to upper level logic when state changes
 *   and/or listening callbacks are registered
 *
 * Error handling:
 * - The layer where error occurred makes transition to ERROR state
 *   and all layers below that are reset to IDLE state and sensor
 *   states are reverted to sensord-is-not-available defaults.
 * - The failed operation is reattempted after brief delay until it
 *   succeeds
 * - Sensord restart might still be needed for full recovery.
 *
 * State transitions:
 * - state transition diagram can be generated from mce-sensorfw.dot
 *   via installing graphviz package and running from command line
 *     dot -Tpng mce-sensorfw.dot -o mce-sensorfw.png
 *
 * ========================================================================= *
 *
 * Rough Data/Control Flow Diagram - Error Handling and EVDEV Input Excluded
 *
 * ========================================================================= *
 *
 *             .--------------------.  .--------------------.
 *             | MCE INIT/EXIT      |  | MCE SENSOR LOGIC   |
 *             `--------------------'  `--------------------'
 *                 |                                     |
 *                 | init/quit            enable/disable |
 *                 |                                     |
 *                 v                                     v
 * .--------------------------------------------------------.
 * | SENSORFW_MODULE                                        |
 * `--------------------------------------------------------'
 *   |             |                                  |
 *   | start       | probe/reset       enable/disable |
 *   |             |                                  |
 *   |             v                                  v
 *   |  .---------------------------------------------------.
 *   |  | SENSORFW_SERVICE                                  |
 *   |  `---------------------------------------------------'
 *   |    |        |                              |      |
 *   |    | start  | load/reset         set/unset |      |
 *   |    |        |                              |      | start/stop
 *   |    |        v                              |      |
 *   |    |    .------------------------------.   |      |
 *   |    |    | SENSORFW_PLUGIN              |   |      |
 *   |    |    `------------------------------'   |      |
 *   |    |        |                              |      |
 *   |    |        | start/reset                  |      |
 *   |    |        |                              |      |
 *   |    |        v                              |      |
 *   |    |    .------------------------------.   |      |
 *   |    |    | SENSORFW_SESSION             |   |      |
 *   |    |    `------------------------------'   |      |
 *   |    |        |                              |      |
 *   |    |        | connect/reset                |      |
 *   |    |        |                              |      |
 *   |    |        v                              |      |
 *   |    |    .------------------------------.   |      |
 *   |    |    | SENSORFW_CONNECTION          |   |      |
 *   |    |    `------------------------------'   |      |
 *   |    |      |                   |            |      |
 *   |    |      |     rethink/reset |            |      |
 *   |    |      |                   |            |      |
 *   |    |      | rethink/reset     |            |      |
 *   |    |      |                   |            |      |
 *   |    |      |                   v            v      |
 *   |    |      | .--------------------------------.    |
 *   |    |      | | SENSORFW_OVERRIDE              |    |
 *   |    |      | `--------------------------------'    |
 *   |    |      v                                       v
 *   |    |    .--------------------------------------------.
 *   |    |    | SENSORFW_REPORTING                         |
 *   |    |    `--------------------------------------------'
 *   |    |                   |
 *   |    |                   | initial/change/reset
 *   |    |                   |
 *   |    |                   v
 *   |    |    .--------------------------------------------.
 *   |    |    | SENSORFW_BACKEND                           |
 *   |    |    `--------------------------------------------'
 *   v    v                |
 * .--------------------.  | sensor state
 * | SENSORFW_EXCEPTION |  |
 * `--------------------'  |
 *               |         |
 *        active |         |
 *               v         v
 *             .--------------------------------------------.
 *             | SENSORFW_NOTIFY                            |
 *             `--------------------------------------------'
 *                            |
 *                            | sensor state
 *                            |
 *                            v
 *             .--------------------------------------------.
 *             | MCE SENSOR LOGIC                           |
 *             `--------------------------------------------'
 *
 * ========================================================================= */

/* ========================================================================= *
 * D-BUS CONSTANTS FOR D-BUS DAEMON
 * ========================================================================= */

/** org.freedesktop.DBus.NameOwnerChanged D-Bus signal */
#define DBUS_SIGNAL_NAME_OWNER_CHANGED         "NameOwnerChanged"

#define DBUS_METHOD_GET_NAME_OWNER             "GetNameOwner"

/* ========================================================================= *
 * D-BUS CONSTANTS FOR SENSORD SERVICE
 * ========================================================================= */

/** How long to wait before retrying failed sensorfw dbus requests */
#define SENSORFW_RETRY_DELAY_MS		       10000

/** D-Bus name of the sensord service */
#define SENSORFW_SERVICE                       "com.nokia.SensorService"

// ----------------------------------------------------------------

/** D-Bus object path for sensord sensor manager */
#define SENSORFW_MANAGER_OBJECT                "/SensorManager"

/** D-Bus interface used by sensor manager */
#define SENSORFW_MANAGER_INTEFCACE             "local.SensorManager"

/** D-Bus method for loading sensor plugin */
#define SENSORFW_MANAGER_METHOD_LOAD_PLUGIN    "loadPlugin"

/** D-Bus method for starting sensor session */
#define SENSORFW_MANAGER_METHOD_START_SESSION  "requestSensor"

// ----------------------------------------------------------------

/* Note: Every sensor has unique object path & interface */

/** D-Bus interface used by proximity sensor plugin */
#define SENSORFW_SENSOR_INTERFACE_PS           "local.ProximitySensor"

/** D-Bus interface used by ambient light sensor plugin */
#define SENSORFW_SENSOR_INTERFACE_ALS          "local.ALSSensor"

/** D-Bus interface used by orientation sensor plugin */
#define SENSORFW_SENSOR_INTERFACE_ORIENT       "local.OrientationSensor"

/* Common methods supported by all sensor interfaces */

/** D-Bus method for enabling sensor */
#define SENSORFW_SENSOR_METHOD_START           "start"

/** D-Bus method for disabling sensor */
#define SENSORFW_SENSOR_METHOD_STOP            "stop"

/** D-Bus method for changing sensor standby override */
#define SENSORFW_SENSOR_METHOD_SET_OVERRIDE    "setStandbyOverride"

/* Note: The sensor specific methods for reading current state
 *       differ only by method name, so common logic can stll be
 *       used for making the queries and processing the replies. */

/** D-Bus method for reading intial proximity state */
#define SENSORFW_SENSOR_METHOD_READ_PS         "proximity"

/** D-Bus method for reading intial ambient light state */
#define SENSORFW_SENSOR_METHOD_READ_ALS        "lux"

/** D-Bus method for reading intial orientation state */
#define SENSORFW_SENSOR_METHOD_READ_ORIENT     "orientation"

// ----------------------------------------------------------------

/** Connect path to sensord data unix domain socket  */
#define SENSORFW_DATA_SOCKET                   "/var/run/sensord.sock"

// ----------------------------------------------------------------

/** Name of proximity sensor */
#define SENSORFW_SENSOR_NAME_PS                "proximitysensor"

/** Name of ambient light sensor */
#define SENSORFW_SENSOR_NAME_ALS               "alssensor"

/** Name of orientation sensor */
#define SENSORFW_SENSOR_NAME_ORIENT            "orientationsensor"

/* ========================================================================= *
 * FORWARD_DECLARATIONS
 * ========================================================================= */

typedef struct sfw_service_t       sfw_service_t;

typedef struct sfw_plugin_t        sfw_plugin_t;
typedef struct sfw_session_t       sfw_session_t;
typedef struct sfw_connection_t    sfw_connection_t;
typedef struct sfw_override_t      sfw_override_t;
typedef struct sfw_reporting_t     sfw_reporting_t;
typedef struct sfw_backend_t       sfw_backend_t;

typedef struct sfw_sample_als_t    sfw_sample_als_t;
typedef struct sfw_sample_ps_t     sfw_sample_ps_t;
typedef struct sfw_sample_orient_t sfw_sample_orient_t;

/* ========================================================================= *
 * SENSORD_DATA_TYPES
 * ========================================================================= */

/** ALS data block as sensord sends them */
struct sfw_sample_als_t
{
    /** microseconds, monotonic */
    uint64_t als_timestamp;

    /** amount of light [lux] */
    uint32_t als_value;
};

/** PS data block as sensord sends them */
struct sfw_sample_ps_t
{
    /** microseconds, monotonic */
    uint64_t ps_timestamp;

    /** distance of blocking object [cm] */
    uint32_t ps_value;

    /** sensor covered [bool]
     *
     * This should be the size of a C++ bool on the same platform.
     * Unfortunately there's no way to find out in a C program
     */
    uint8_t  ps_withinProximity;
};

/** Orientation data block as sensord sends them */
struct sfw_sample_orient_t
{
    /* microseconds, monotonic */
    uint64_t orient_timestamp;

    /* orientation [enum orientation_state_t] */
    int32_t  orient_state;
};

/* ========================================================================= *
 * SENSORFW_BACKEND
 * ========================================================================= */

/** Callback function type: initial value reporting */
typedef void (*sfw_value_fn)(sfw_plugin_t *plugin, unsigned value);

/** Callback function type: value change reporting */
typedef void (*sfw_sample_fn)(sfw_plugin_t *plugin, const void *sample);

/** Callback function type: value reset reporting */
typedef void (*sfw_reset_fn)(sfw_plugin_t *plugin);

/** Sensor specific data and callbacks */
struct sfw_backend_t
{
    /** Name of the sensor as expected by sensord */
    const char   *be_sensor_name;

    /** D-Bus object path for the sensor, or NULL to construct default one */
    const char   *be_sensor_object;

    /** D-Bus interface for the sensor */
    const char   *be_sensor_interface;

    /** Size of sensor data blobs sensord will be sending */
    size_t        be_sample_size;

    /** Callback for handling sensor data blob */
    sfw_sample_fn be_sample_cb;

    /** D-Bus method name for querying the initial sensor value */
    const char   *be_value_method;

    /** Callback for handling received initial sensor value */
    sfw_value_fn  be_value_cb;

    /** Callback for resetting sensor value back to default */
    sfw_reset_fn  be_reset_cb;

    /** Callback for restoring sensor value back to last-known */
    sfw_reset_fn  be_restore_cb;

    /** Callback for setting sensor value when stopped */
    sfw_reset_fn  be_forget_cb;
};

/* ========================================================================= *
 * SENSORFW_HELPERS
 * ========================================================================= */

static bool              sfw_socket_set_blocking        (int fd, bool blocking);
static int               sfw_socket_open                (void);
static guint             sfw_socket_add_notify          (int fd, bool close_on_unref, GIOCondition cnd, GIOFunc io_cb, gpointer aptr);

/* ========================================================================= *
 * SENSORFW_REPORTING
 * ========================================================================= */

typedef enum
{
    /** Initial state used before availability of sensord is known */
    REPORTING_INITIAL,

    /** Sensord is not available */
    REPORTING_IDLE,

    /** Choose between start() and stop() */
    REPORTING_RETHINK,

    /** Waiting a reply to start() */
    REPORTING_ENABLING,

    /** Sensor started succesfully */
    REPORTING_ENABLED,

    /** Waiting a reply to stop() */
    REPORTING_DISABLING,

    /** Sensor stopped succesfully */
    REPORTING_DISABLED,

    /** Something went wrong */
    REPORTING_ERROR,

    REPORTING_NUMSTATES
} sfw_reporting_state_t;

/** State machine for handling sensor start/stop */
struct sfw_reporting_t
{
    /** Pointer to containing plugin object */
    sfw_plugin_t          *rep_plugin;

    /** Current reporting state */
    sfw_reporting_state_t  rep_state;

    /** Pending start/stop dbus method call */
    DBusPendingCall       *rep_change_pc;

    /** Pending initial value dbus method call */
    DBusPendingCall       *rep_value_pc;

    /** Flag for: MCE wants sensor to be enabled */
    bool                  rep_target;

    /** Timer for: Retry after ipc error */
    guint                 rep_retry_id;
};

static const char       *sfw_reporting_state_name         (sfw_reporting_state_t state);

static sfw_reporting_t  *sfw_reporting_create             (sfw_plugin_t *plugin);
static void              sfw_reporting_delete             (sfw_reporting_t *self);

static void              sfw_reporting_cancel_change      (sfw_reporting_t *self);
static void              sfw_reporting_cancel_value       (sfw_reporting_t *self);
static void              sfw_reporting_cancel_retry       (sfw_reporting_t *self);

static void              sfw_reporting_trans              (sfw_reporting_t *self, sfw_reporting_state_t state);

static void              sfw_reporting_change_cb          (DBusPendingCall *pc, void *aptr);
static void              sfw_reporting_value_cb           (DBusPendingCall *pc, void *aptr);
static gboolean          sfw_reporting_retry_cb           (gpointer aptr);

static void              sfw_reporting_do_rethink         (sfw_reporting_t *self);
static void              sfw_reporting_do_start           (sfw_reporting_t *self);
static void              sfw_reporting_do_reset           (sfw_reporting_t *self);

static void              sfw_reporting_set_target         (sfw_reporting_t *self, bool enable);

/* ========================================================================= *
 * SENSORFW_OVERRIDE
 * ========================================================================= */

typedef enum  {
    /** Initial state used before availability of sensord is known */
    OVERRIDE_INITIAL,

    /** Sensord is not available */
    OVERRIDE_IDLE,

    /** Choose between standby override set/unset */
    OVERRIDE_RETHINK,

    /** Waiting for a reply to set standby override */
    OVERRIDE_ENABLING,

    /** Standby override succesfully set */
    OVERRIDE_ENABLED,

    /** Waiting for a reply to unset standby override */
    OVERRIDE_DISABLING,

    /** Standby override succesfully unset */
    OVERRIDE_DISABLED,

    /** Something went wrong */
    OVERRIDE_ERROR,

    OVERRIDE_NUMSTATES
} sfw_override_state_t;

/** State machine for handling sensor standby override */
struct sfw_override_t
{
    /** Pointer to containing plugin object */
    sfw_plugin_t         *ovr_plugin;

    /** Current override state */
    sfw_override_state_t  ovr_state;

    /** Pending standby override set/unset method call */
    DBusPendingCall      *ovr_start_pc;

    /** Flag for: MCE wants standby override to be set */
    bool                  ovr_target;

    /** Timer for: Retry after ipc error */
    guint                 ovr_retry_id;
};

static const char       *sfw_override_state_name         (sfw_override_state_t state);

static sfw_override_t    *sfw_override_create            (sfw_plugin_t *plugin);
static void              sfw_override_delete             (sfw_override_t *self);

static void              sfw_override_cancel_start       (sfw_override_t *self);
static void              sfw_override_cancel_retry       (sfw_override_t *self);

static void              sfw_override_trans              (sfw_override_t *self, sfw_override_state_t state);

static void              sfw_override_start_cb           (DBusPendingCall *pc, void *aptr);
static gboolean          sfw_override_retry_cb           (gpointer aptr);

static void              sfw_override_do_rethink         (sfw_override_t *self);
static void              sfw_override_do_start           (sfw_override_t *self);
static void              sfw_override_do_reset           (sfw_override_t *self);

static void              sfw_override_set_target         (sfw_override_t *self, bool enable);

/* ========================================================================= *
 * SENSORFW_CONNECTION
 * ========================================================================= */

typedef enum
{
    /** Initial state used before availability of sensord is known */
    CONNECTION_INITIAL,

    /** Sensord is not available */
    CONNECTION_IDLE,

    /** Waiting for data socket to come writable */
    CONNECTION_CONNECTING,

    /** Waiting for handshake reply */
    CONNECTION_REGISTERING,

    /** Waiting for sensor data */
    CONNECTION_CONNECTED,

    /** Something went wrong */
    CONNECTION_ERROR,

    CONNECTION_NUMSTATES
} sfw_connection_state_t;

/** State machine for handling sensor data connection */
struct sfw_connection_t
{
    /** Pointer to containing plugin object */
    sfw_plugin_t           *con_plugin;

    /** Current connection state */
    sfw_connection_state_t  con_state;

    /** File descriptor for data socket */
    int                     con_fd;

    /** I/O watch id id for: ready to write */
    guint                   con_rx_id;

    /** I/O watch id id for: data available */
    guint                   con_tx_id;

    /** Timer for: Retry after ipc error */
    guint                   con_retry_id;
};

static const char       *sfw_connection_state_name      (sfw_connection_state_t state);

static sfw_connection_t *sfw_connection_create          (sfw_plugin_t *plugin);
static void              sfw_connection_delete          (sfw_connection_t *self);

static bool              sfw_connection_handle_samples  (sfw_connection_t *self, char *data, size_t size);

static int               sfw_connection_get_session_id  (const sfw_connection_t *self);

static bool              sfw_connection_tx_req          (sfw_connection_t *self);
static bool              sfw_connection_rx_ack          (sfw_connection_t *self);
static bool              sfw_connection_rx_dta          (sfw_connection_t *self);

static gboolean          sfw_connection_tx_cb           (GIOChannel *chn, GIOCondition cnd, gpointer aptr);
static gboolean          sfw_connection_rx_cb           (GIOChannel *chn, GIOCondition cnd, gpointer aptr);

static void              sfw_connection_remove_tx_notify(sfw_connection_t *self);
static void              sfw_connection_remove_rx_notify(sfw_connection_t *self);
static void              sfw_connection_close_socket    (sfw_connection_t *self);
static bool              sfw_connection_open_socket     (sfw_connection_t *self);

static gboolean          sfw_connection_retry_cb        (gpointer aptr);
static void              sfw_connection_cancel_retry    (sfw_connection_t *self);

static void              sfw_connection_trans           (sfw_connection_t *self, sfw_connection_state_t state);

static void              sfw_connection_do_start        (sfw_connection_t *self);
static void              sfw_connection_do_reset        (sfw_connection_t *self);

/* ========================================================================= *
 * SENSORFW_SESSION
 * ========================================================================= */

typedef enum
{
    /** Initial state used before availability of sensord is known */
    SESSION_INITIAL,

    /** Sensord is not available */
    SESSION_IDLE,

    /** Waiting for session id reply */
    SESSION_REQUESTING,

    /** Have a session id */
    SESSION_ACTIVE,

    /** Something went wrong */
    SESSION_ERROR,

    /** Sensor is not supported */
    SESSION_INVALID,

    SESSION_NUMSTATES
} sfw_session_state_t;

/** State machine for handling sensor data session */
struct sfw_session_t
{
    /** Pointer to containing plugin object */
    sfw_plugin_t        *ses_plugin;

    /** Current session state */
    sfw_session_state_t  ses_state;

    /** Pending session start dbus method call */
    DBusPendingCall     *ses_start_pc;

    /** Session ID received from sensord */
    dbus_int32_t         ses_id;

    /** Timer for: Retry after ipc error */
    guint                ses_retry_id;
};

/** Sensor not supported session id from sensorfwd */
#define SESSION_ID_INVALID (-1)

/** Placeholder value for session id not parsed yet */
#define SESSION_ID_UNKNOWN (-2)

static const char       *sfw_session_state_name         (sfw_session_state_t state);

static sfw_session_t    *sfw_session_create             (sfw_plugin_t *plugin);
static void              sfw_session_delete             (sfw_session_t *self);

static int               sfw_session_get_id             (const sfw_session_t *self);

static void              sfw_session_cancel_start       (sfw_session_t *self);
static void              sfw_session_cancel_retry       (sfw_session_t *self);

static void              sfw_session_trans              (sfw_session_t *self, sfw_session_state_t state);

static void              sfw_session_start_cb           (DBusPendingCall *pc, void *aptr);
static gboolean          sfw_session_retry_cb           (gpointer aptr);

static void              sfw_session_do_start           (sfw_session_t *self);
static void              sfw_session_do_reset           (sfw_session_t *self);

/* ========================================================================= *
 * SENSORFW_PLUGIN
 * ========================================================================= */

typedef enum
{
    /** Initial state used before availability of sensord is known */
    PLUGIN_INITIAL,

    /** Sensord is not available */
    PLUGIN_IDLE,

    /** Waiting for a reply to plugin load method call */
    PLUGIN_LOADING,

    /** Sensor plugin is loaded  */
    PLUGIN_LOADED,

    /** Something went wrong */
    PLUGIN_ERROR,

    PLUGIN_NUMSTATES
} sfw_plugin_state_t;

/** State machine for handling sensor plugin loading */
struct sfw_plugin_t
{
    /** Sensor specific backend data & callbacks */
    const sfw_backend_t  *plg_backend;

    /** Default sensor specific D-BUs object path
     *
     * Constructed if backend does not define one explicitly */
    char                 *plg_sensor_object;

    /** Current plugin state */
    sfw_plugin_state_t    plg_state;

    /** Pending plugin load dbus method call */
    DBusPendingCall      *plg_load_pc;

    /** Session state machine object */
    sfw_session_t        *plg_session;

    /** Connection state machine object */
    sfw_connection_t     *plg_connection;

    /** Standby override state machine object */
    sfw_override_t       *plg_override;

    /** Sensor reporting state machine object */
    sfw_reporting_t      *plg_reporting;

    /** Timer for: Retry after ipc error */
    guint                 plg_retry_id;
};

static const char       *sfw_plugin_state_name          (sfw_plugin_state_t state);

static sfw_plugin_t     *sfw_plugin_create              (const sfw_backend_t *backend);
static void              sfw_plugin_delete              (sfw_plugin_t *self);

static const char       *sfw_plugin_get_sensor_name     (const sfw_plugin_t *self);
static const char       *sfw_plugin_get_sensor_object   (const sfw_plugin_t *self);
static const char       *sfw_plugin_get_sensor_interface(const sfw_plugin_t *self);
static int               sfw_plugin_get_session_id      (const sfw_plugin_t *self);
static const char       *sfw_plugin_get_value_method    (const sfw_plugin_t *self);
static size_t            sfw_plugin_get_sample_size     (const sfw_plugin_t *self);
static void              sfw_plugin_handle_sample       (sfw_plugin_t *self, const void *sample);
static void              sfw_plugin_handle_value        (sfw_plugin_t *self, unsigned value);
static void              sfw_plugin_reset_value         (sfw_plugin_t *self);
static void              sfw_plugin_restore_value       (sfw_plugin_t *self);
static void              sfw_plugin_forget_value        (sfw_plugin_t *self);

static void              sfw_plugin_cancel_load         (sfw_plugin_t *self);
static void              sfw_plugin_cancel_retry        (sfw_plugin_t *self);

static void              sfw_plugin_trans               (sfw_plugin_t *self, sfw_plugin_state_t state);

static void              sfw_plugin_load_cb             (DBusPendingCall *pc, void *aptr);
static gboolean          sfw_plugin_retry_cb            (gpointer aptr);

static void              sfw_plugin_do_load             (sfw_plugin_t *self);
static void              sfw_plugin_do_reset            (sfw_plugin_t *self);

static void              sfw_plugin_do_session_start    (sfw_plugin_t *self);
static void              sfw_plugin_do_session_reset    (sfw_plugin_t *self);

static void              sfw_plugin_do_connection_start (sfw_plugin_t *self);
static void              sfw_plugin_do_connection_reset (sfw_plugin_t *self);

static void              sfw_plugin_do_override_start   (sfw_plugin_t *self);
static void              sfw_plugin_do_override_reset   (sfw_plugin_t *self);

static void              sfw_plugin_do_reporting_start  (sfw_plugin_t *self);
static void              sfw_plugin_do_reporting_reset  (sfw_plugin_t *self);

/* ========================================================================= *
 * SENSORFW_SERVICE
 * ========================================================================= */

typedef enum
{
    /** Sensord availability is not known */
    SERVICE_UNKNOWN,

    /** Waiting for a reply to name owner query */
    SERVICE_QUERYING,

    /** Sensord service is available */
    SERVICE_RUNNING,

    /** Sensord service is not available */
    SERVICE_STOPPED,

    SERVICE_NUMSTATES
} sfw_service_state_t;

/** State machine for handling sensord service availability tracking */
struct sfw_service_t
{
    /** Sensord service tracking state */
    sfw_service_state_t  srv_state;

    /** Pending name owner dbus method call */
    DBusPendingCall     *srv_query_pc;

    /** State machine object for proximity sensor */
    sfw_plugin_t        *srv_ps;

    /** State machine object for ambient light sensor */
    sfw_plugin_t        *srv_als;

    /** State machine object for orientation sensor */
    sfw_plugin_t        *srv_orient;

};

static const char       *sfw_service_state_name         (sfw_service_state_t state);

static sfw_service_t    *sfw_service_create             (void);
static void              sfw_service_delete             (sfw_service_t *self);

static void              sfw_service_cancel_query       (sfw_service_t *self);
static void              sfw_service_query_cb           (DBusPendingCall *pc, void *aptr);

static void              sfw_service_trans              (sfw_service_t *self, sfw_service_state_t state);

static void              sfw_service_do_start           (sfw_service_t *self);
static void              sfw_service_do_stop            (sfw_service_t *self);
static void              sfw_service_do_query           (sfw_service_t *self);

static void              sfw_service_set_ps             (sfw_service_t *self, bool enable);
static void              sfw_service_set_als            (sfw_service_t *self, bool enable);
static void              sfw_service_set_orient         (sfw_service_t *self, bool enable);

/* ========================================================================= *
 * SENSORFW_NOTIFY
 * ========================================================================= */

/** Proximity state to use when sensor can't be enabled
 *
 * This must be "uncovered" so that absense of or failures within
 * sensord do not make the device unusable due to proximity values
 * blocking display power up and touch input.
 */
#define SWF_NOTIFY_DEFAULT_PS false

/** Proximity state to use when waiting for sensord startup
 *
 * This must be "covered" so that we do not prematurely unblock
 * touch input / allow display power up during bootup and during
 * sensord restarts.
 */
#define SWF_NOTIFY_EXCEPTION_PS true

/** Ambient light level [lux] to use when sensor can't be enabled */
#define SWF_NOTIFY_DEFAULT_ALS 400

/** Orientation state to use when sensor can't be enabled */
#define SWF_NOTIFY_DEFAULT_ORIENT MCE_ORIENTATION_UNDEFINED

/** Dummy sensor value to use when re-sending cached state data */
#define SWF_NOTIFY_DUMMY 0

typedef enum
{
    /** Cached sensor state should be reset to default value */
    NOTIFY_RESET,

    /** Cached sensor state should be restored from last-known value */
    NOTIFY_RESTORE,

    /** Cached sensor state should be sent again */
    NOTIFY_REPEAT,

    /** Sensor state was received from evdev source */
    NOTIFY_EVDEV,

    /** Sensor state was received from sensord */
    NOTIFY_SENSORD,

    NOTIFY_NUMTYPES
} sfw_notify_t;

static gboolean          sfw_name_owner_changed_cb      (DBusMessage *const msg);

static void              sfw_notify_ps                  (sfw_notify_t type, bool covered);
static void              sfw_notify_als                 (sfw_notify_t type, unsigned lux);
static void              sfw_notify_orient              (sfw_notify_t type, int state);

/* ========================================================================= *
 * SENSORFW_EXCEPTION
 * ========================================================================= */

/** Durations for enforcing use of exceptional sensor values */
typedef enum {
    /** Expected time until sensord availability is known */
     SFW_EXCEPTION_LENGTH_MCE_STARTING_UP = 30 * 1000,

    /** Expected time until data from sensord is available */
     SFW_EXCEPTION_LENGTH_SENSORD_RUNNING =  2 * 1000,

    /** Expected time until sensord gets (re)started */
     SFW_EXCEPTION_LENGTH_SENSORD_STOPPED =  5 * 1000,
} sfw_exception_delay_t;

static gboolean          sfw_exception_timer_cb  (gpointer aptr);
static void              sfw_exception_cancel    (void);
static void              sfw_exception_start     (sfw_exception_delay_t delay_ms);
static bool              sfw_exception_is_active (void);

/* ========================================================================= *
 * SENSORFW_MODULE
 * ========================================================================= */

// (exported API defined in "mce-sensorfw.h")

/* ========================================================================= *
 * SENSORFW_BACKEND
 * ========================================================================= */

/** Callback for handling ambient light events from sensord */
static void
sfw_backend_als_sample_cb(sfw_plugin_t *plugin, const void *sample)
{
    (void)plugin;

    const sfw_sample_als_t *self = sample;

    mce_log(LL_DEBUG, "ALS: time=%"PRIu64" light=%"PRIu32"",
            self->als_timestamp, self->als_value);

    sfw_notify_als(NOTIFY_SENSORD, self->als_value);
}

/** Callback for handling proximity events from sensord */
static void
sfw_backend_ps_sample_cb(sfw_plugin_t *plugin, const void *sample)
{
    (void)plugin;

    const sfw_sample_ps_t *self = sample;

    mce_log(LL_DEBUG, "PS: time=%"PRIu64" distance=%"PRIu32" proximity=%s",
            self->ps_timestamp, self->ps_value,
            self->ps_withinProximity ? "true" : "false");

    sfw_notify_ps(NOTIFY_SENSORD, self->ps_withinProximity);
}

/** Callback for handling orientation events from sensord */
static void
sfw_backend_orient_sample_cb(sfw_plugin_t *plugin, const void *sample)
{
    (void)plugin;

    const sfw_sample_orient_t *self = sample;

    mce_log(LL_DEBUG, "ORIENT: time=%"PRIu64" state=%"PRId32"",
            self->orient_timestamp, self->orient_state);

    sfw_notify_orient(NOTIFY_SENSORD, self->orient_state);
}

// ----------------------------------------------------------------

/** Callback for handling reply to ambient light query from sensord */
static void
sfw_backend_als_value_cb(sfw_plugin_t *plugin, unsigned value)
{
    (void)plugin;

    mce_log(LL_DEBUG, "ALS: initial light=%u", value);

    sfw_notify_als(NOTIFY_SENSORD, value);
}

/** Callback for handling reply to proximity query from sensord */
static void
sfw_backend_ps_value_cb(sfw_plugin_t *plugin, unsigned value)
{
    (void)plugin;

    bool covered = (value < 1);

    mce_log(LL_DEBUG, "PS: initial distance=%u proximity=%s",
            value, covered ? "true" : "false");

    sfw_notify_ps(NOTIFY_SENSORD, covered);
}

/** Callback for handling reply to orientation query from sensord */
static void
sfw_backend_orient_value_cb(sfw_plugin_t *plugin, unsigned value)
{
    (void)plugin;

    mce_log(LL_DEBUG, "ORIENT: initial state=%u", value);

    sfw_notify_orient(NOTIFY_SENSORD, value);
}

// ----------------------------------------------------------------

/** Callback for resetting ambient light state to default */
static void
sfw_backend_als_reset_cb(sfw_plugin_t *plugin)
{
    (void)plugin;

    mce_log(LL_DEBUG, "ALS: light=reset-to-default");

    sfw_notify_als(NOTIFY_RESET, SWF_NOTIFY_DUMMY);
}

/** Callback for resetting proximity state to default */
static void
sfw_backend_ps_reset_cb(sfw_plugin_t *plugin)
{
    (void)plugin;

    mce_log(LL_DEBUG, "PS: covered=reset-to-default");

    sfw_notify_ps(NOTIFY_RESET, SWF_NOTIFY_DUMMY);
}

/** Callback for resetting orientation state to default */
static void
sfw_backend_orient_reset_cb(sfw_plugin_t *plugin)
{
    (void)plugin;

    mce_log(LL_DEBUG, "ORIENT: state=reset-to-default");

    sfw_notify_orient(NOTIFY_RESET, SWF_NOTIFY_DUMMY);
}

/** Callback for restoring ambient light state to last-known */
static void
sfw_backend_als_restore_cb(sfw_plugin_t *plugin)
{
    (void)plugin;

    mce_log(LL_DEBUG, "ALS: light=restore-to-last-known");

    sfw_notify_als(NOTIFY_RESTORE, SWF_NOTIFY_DUMMY);
}

/** Callback for restoring proximity state to last-known */
static void
sfw_backend_ps_restore_cb(sfw_plugin_t *plugin)
{
    (void)plugin;

    mce_log(LL_DEBUG, "PS: covered=restore-to-last-known");

    sfw_notify_ps(NOTIFY_RESTORE, SWF_NOTIFY_DUMMY);
}

/** Callback for restoring orientation state to last-known */
static void
sfw_backend_orient_restore_cb(sfw_plugin_t *plugin)
{
    (void)plugin;

    mce_log(LL_DEBUG, "ORIENT: state=restore-to-last-known");

    sfw_notify_orient(NOTIFY_RESTORE, SWF_NOTIFY_DUMMY);
}

// ----------------------------------------------------------------

/** Data and callbacks for proximity sensor */
static const sfw_backend_t sfw_backend_ps =
{
    .be_sensor_name      = SENSORFW_SENSOR_NAME_PS,
    .be_sensor_object    = 0,
    .be_sensor_interface = SENSORFW_SENSOR_INTERFACE_PS,

    .be_sample_size      = sizeof(sfw_sample_ps_t),
    .be_sample_cb        = sfw_backend_ps_sample_cb,

    .be_value_method     = SENSORFW_SENSOR_METHOD_READ_PS,
    .be_value_cb         = sfw_backend_ps_value_cb,

    .be_reset_cb         = sfw_backend_ps_reset_cb,
    .be_restore_cb       = sfw_backend_ps_restore_cb,
    .be_forget_cb        = 0,
};

/** Data and callbacks for ambient light sensor */
static const sfw_backend_t sfw_backend_als =
{
    .be_sensor_name      = SENSORFW_SENSOR_NAME_ALS,
    .be_sensor_object    = 0,
    .be_sensor_interface = SENSORFW_SENSOR_INTERFACE_ALS,

    .be_sample_size      = sizeof(sfw_sample_als_t),
    .be_sample_cb        = sfw_backend_als_sample_cb,

    .be_value_method     = SENSORFW_SENSOR_METHOD_READ_ALS,
    .be_value_cb         = sfw_backend_als_value_cb,

    .be_reset_cb         = sfw_backend_als_reset_cb,
    .be_restore_cb       = sfw_backend_als_restore_cb,
    .be_forget_cb        = 0,
};

/** Data and callbacks for orientation sensor */
static const sfw_backend_t sfw_backend_orient =
{
    .be_sensor_name      = SENSORFW_SENSOR_NAME_ORIENT,
    .be_sensor_object    = 0,
    .be_sensor_interface = SENSORFW_SENSOR_INTERFACE_ORIENT,

    .be_sample_size      = sizeof(sfw_sample_orient_t),
    .be_sample_cb        = sfw_backend_orient_sample_cb,

    .be_value_method     = SENSORFW_SENSOR_METHOD_READ_ORIENT,
    .be_value_cb         = sfw_backend_orient_value_cb,

    .be_reset_cb         = sfw_backend_orient_reset_cb,
    .be_restore_cb       = sfw_backend_orient_restore_cb,
    .be_forget_cb        = sfw_backend_orient_reset_cb,
};

/* ========================================================================= *
 * SENSORFW_HELPERS
 * ========================================================================= */

/* Set file descriptor to blocking/unblocking state
 */
static bool
sfw_socket_set_blocking(int fd, bool blocking)
{
    bool ok    = false;
    int  flags = 0;

    if( (flags = fcntl(fd, F_GETFL, 0)) == -1 ) {
        mce_log(LL_ERR, "could not get socket flags");
        goto EXIT;
    }

    if( blocking )
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    if( fcntl(fd, F_SETFL, flags) == -1 ){
        mce_log(LL_ERR, "could not set socket flags");
        goto EXIT;
    }

    ok = true;

EXIT:
    return ok;
}

/** Get a sensord data socket connection file descriptor
 */
static int
sfw_socket_open(void)
{
    bool ok = false;
    int  fd = -1;

    struct sockaddr_un sa;
    socklen_t sa_len;

    /* create socket */
    if( (fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1 ) {
        mce_log(LL_ERR, "could not open local domain socket");
        goto EXIT;
    }

    /* make it non-blocking -> connect() will return immediately */
    if( !sfw_socket_set_blocking(fd, false) )
        goto EXIT;

    /* connect to daemon */
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", SENSORFW_DATA_SOCKET);
    sa_len = strchr(sa.sun_path, 0) + 1 - (char *)&sa;

    if( connect(fd, (struct sockaddr *)&sa, sa_len) == -1 ) {
        mce_log(LL_ERR, "could not connect to %s: %m",
                SENSORFW_DATA_SOCKET);
        goto EXIT;
    }

    ok = true;

EXIT:
    /* all or nothing */
    if( !ok && fd != -1 )
        close(fd), fd = -1;

    return fd;
}

/** Add a glib I/O notification for a file descriptor
 */
static guint
sfw_socket_add_notify(int fd, bool close_on_unref,
                      GIOCondition cnd, GIOFunc io_cb, gpointer aptr)
{
    guint         wid = 0;
    GIOChannel   *chn = 0;

    if( !(chn = g_io_channel_unix_new(fd)) ) {
        goto EXIT;
    }

    g_io_channel_set_close_on_unref(chn, close_on_unref);

    cnd |= G_IO_ERR | G_IO_HUP | G_IO_NVAL;

    if( !(wid = g_io_add_watch(chn, cnd, io_cb, aptr)) )
        goto EXIT;

EXIT:
    if( chn != 0 ) g_io_channel_unref(chn);

    return wid;

}

/* ========================================================================= *
 * SENSORFW_REPORTING
 * ========================================================================= */

/** Translate reporting state to human readable form
 */
static const char *
sfw_reporting_state_name(sfw_reporting_state_t state)
{
    static const char *const lut[REPORTING_NUMSTATES] =
    {
        [REPORTING_INITIAL]     = "INITIAL",
        [REPORTING_IDLE]        = "IDLE",
        [REPORTING_RETHINK]     = "RETHINK",
        [REPORTING_ENABLING]    = "ENABLING",
        [REPORTING_ENABLED]     = "ENABLED",
        [REPORTING_DISABLING]   = "DISABLING",
        [REPORTING_DISABLED]    = "DISABLED",
        [REPORTING_ERROR]       = "ERROR",
    };

    return (state < REPORTING_NUMSTATES) ? lut[state] : 0;
}

/** Create sensor start/stop state machine object
 */
static sfw_reporting_t *
sfw_reporting_create(sfw_plugin_t *plugin)
{
    sfw_reporting_t *self = calloc(1, sizeof *self);

    self->rep_plugin    = plugin;
    self->rep_state     = REPORTING_INITIAL;
    self->rep_change_pc = 0;
    self->rep_value_pc  = 0;
    self->rep_target    = false;
    self->rep_retry_id  = 0;

    return self;
}

/** Delete sensor start/stop state machine object
 */
static void
sfw_reporting_delete(sfw_reporting_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self ) {
        sfw_reporting_trans(self, REPORTING_INITIAL);
        self->rep_plugin = 0;
        free(self);
    }
}

/** Set sensor start/stop target state
 */
static void
sfw_reporting_set_target(sfw_reporting_t *self, bool enable)
{
    if( self->rep_target != enable ) {
        self->rep_target = enable;
        sfw_reporting_do_rethink(self);
    }
}

/** Cancel pending sensor start/stop method call
 */
static void
sfw_reporting_cancel_change(sfw_reporting_t *self)
{
    if( self->rep_change_pc ) {
        dbus_pending_call_cancel(self->rep_change_pc);
        dbus_pending_call_unref(self->rep_change_pc);
        self->rep_change_pc = 0;
    }
}

/** Cancel pending sensor initial value method call
 */
static void
sfw_reporting_cancel_value(sfw_reporting_t *self)
{
    if( self->rep_value_pc ) {
        dbus_pending_call_cancel(self->rep_value_pc);
        dbus_pending_call_unref(self->rep_value_pc);
        self->rep_value_pc = 0;
    }
}

/** Cancel pending ipc retry timer
 */
static void
sfw_reporting_cancel_retry(sfw_reporting_t *self)
{
    if( self->rep_retry_id ) {
        g_source_remove(self->rep_retry_id);
        self->rep_retry_id = 0;
    }
}

/** Make a state transition
 */
static void
sfw_reporting_trans(sfw_reporting_t *self, sfw_reporting_state_t state)
{
    dbus_int32_t sid  = sfw_plugin_get_session_id(self->rep_plugin);

    if( self->rep_state == state )
        goto EXIT;

    sfw_reporting_cancel_change(self);
    sfw_reporting_cancel_value(self);
    sfw_reporting_cancel_retry(self);

    mce_log(LL_DEBUG, "reporting(%s): %s -> %s",
            sfw_plugin_get_sensor_name(self->rep_plugin),
            sfw_reporting_state_name(self->rep_state),
            sfw_reporting_state_name(state));

    self->rep_state = state;

    switch( self->rep_state ) {
    case REPORTING_RETHINK:
        if( self->rep_target )
            sfw_reporting_trans(self, REPORTING_ENABLING);
        else
            sfw_reporting_trans(self, REPORTING_DISABLING);
        break;

    case REPORTING_ENABLING:
        // resend last known state
        sfw_plugin_restore_value(self->rep_plugin);

        dbus_send_ex(SENSORFW_SERVICE,
                     sfw_plugin_get_sensor_object(self->rep_plugin),
                     sfw_plugin_get_sensor_interface(self->rep_plugin),
                     SENSORFW_SENSOR_METHOD_START,
                     sfw_reporting_change_cb,
                     self, 0, &self->rep_change_pc,
                     DBUS_TYPE_INT32, &sid,
                     DBUS_TYPE_INVALID);
        break;

    case REPORTING_DISABLING:
        // optional: switch to sensor stopped value
        sfw_plugin_forget_value(self->rep_plugin);

        dbus_send_ex(SENSORFW_SERVICE,
                     sfw_plugin_get_sensor_object(self->rep_plugin),
                     sfw_plugin_get_sensor_interface(self->rep_plugin),
                     SENSORFW_SENSOR_METHOD_STOP,
                     sfw_reporting_change_cb,
                     self, 0, &self->rep_change_pc,
                     DBUS_TYPE_INT32, &sid,
                     DBUS_TYPE_INVALID);
        break;

    case REPORTING_ENABLED:
        dbus_send_ex(SENSORFW_SERVICE,
                     sfw_plugin_get_sensor_object(self->rep_plugin),
                     sfw_plugin_get_sensor_interface(self->rep_plugin),
                     sfw_plugin_get_value_method(self->rep_plugin),
                     sfw_reporting_value_cb,
                     self, 0, &self->rep_value_pc,
                     DBUS_TYPE_INVALID);

        break;

    case REPORTING_DISABLED:
        // NOP
        break;

    case REPORTING_ERROR:
        self->rep_retry_id = g_timeout_add(SENSORFW_RETRY_DELAY_MS,
                                           sfw_reporting_retry_cb,
                                           self);
        break;

    default:
    case REPORTING_IDLE:
    case REPORTING_INITIAL:
        // reset sensor value to default state
        sfw_plugin_reset_value(self->rep_plugin);
        break;

    }

EXIT:

    return;
}

/** Handle reply to initial sensor value query
 */
static void
sfw_reporting_value_cb(DBusPendingCall *pc, void *aptr)
{
    sfw_reporting_t *self = aptr;

    DBusMessage  *rsp  = 0;
    DBusError     err  = DBUS_ERROR_INIT;
    dbus_uint32_t tck  = 0;
    dbus_uint32_t val  = 0;
    bool          ack  = false;

    if( !pc || !self || pc != self->rep_value_pc )
        goto EXIT;

    dbus_pending_call_unref(self->rep_value_pc),
        self->rep_value_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
        mce_log(LL_ERR, "reporting(%s): no reply",
                sfw_plugin_get_sensor_name(self->rep_plugin));
        goto EXIT;
    }

    if( dbus_set_error_from_message(&err, rsp) ) {
        mce_log(LL_ERR, "reporting(%s): error reply: %s: %s",
                sfw_plugin_get_sensor_name(self->rep_plugin),
                err.name, err.message);
        goto EXIT;
    }

    DBusMessageIter body, data;

    if( !dbus_message_iter_init(rsp, &body) )
        goto PARSE_FAILED;

    if( dbus_message_iter_get_arg_type(&body) != DBUS_TYPE_STRUCT )
        goto PARSE_FAILED;

    dbus_message_iter_recurse(&body, &data);
    dbus_message_iter_next(&body);

    if( dbus_message_iter_get_arg_type(&data) !=  DBUS_TYPE_UINT64 )
        goto PARSE_FAILED;

    dbus_message_iter_get_basic(&data, &tck);
    dbus_message_iter_next(&data);

    if( dbus_message_iter_get_arg_type(&data) !=  DBUS_TYPE_UINT32 )
        goto PARSE_FAILED;

    dbus_message_iter_get_basic(&data, &val);
    dbus_message_iter_next(&data);

    sfw_plugin_handle_value(self->rep_plugin, val);
    ack = true;
    goto EXIT;

PARSE_FAILED:
    mce_log(LL_ERR, "reporting(%s): parse error",
            sfw_plugin_get_sensor_name(self->rep_plugin));

EXIT:
    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);

    if( !ack && self->rep_state == REPORTING_ENABLED )
        sfw_reporting_trans(self, REPORTING_ERROR);

    return;
}

/** Handle reply to start/stop sensor request
 */
static void
sfw_reporting_change_cb(DBusPendingCall *pc, void *aptr)
{
    sfw_reporting_t *self = aptr;

    DBusMessage *rsp = 0;
    DBusError    err = DBUS_ERROR_INIT;
    dbus_bool_t  ack = FALSE;

    if( !pc || !self || pc != self->rep_change_pc )
        goto EXIT;

    dbus_pending_call_unref(self->rep_change_pc),
        self->rep_change_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
        mce_log(LL_ERR, "reporting(%s): no reply",
                sfw_plugin_get_sensor_name(self->rep_plugin));
        goto EXIT;
    }

    if( dbus_set_error_from_message(&err, rsp) ) {
        mce_log(LL_ERR, "reporting(%s): error reply: %s: %s",
                sfw_plugin_get_sensor_name(self->rep_plugin),
                err.name, err.message);
        goto EXIT;
    }

    /* there is no payload in the reply, so if we do not get
     * an error reply -> assume everything is ok */
    ack = true;

EXIT:
    mce_log(LL_DEBUG, "reporting(%s): ack=%d",
            sfw_plugin_get_sensor_name(self->rep_plugin), ack);

    if( self->rep_state == REPORTING_ENABLING ||
        self->rep_state == REPORTING_DISABLING ) {

        if( !ack )
            sfw_reporting_trans(self, REPORTING_ERROR);
        else if( self->rep_state == REPORTING_ENABLING )
            sfw_reporting_trans(self, REPORTING_ENABLED);
        else
            sfw_reporting_trans(self, REPORTING_DISABLED);
    }

    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);

    return;
}

/** Handle triggering of start/stop retry timer
 */
static gboolean
sfw_reporting_retry_cb(gpointer aptr)
{
    sfw_reporting_t *self = aptr;

    if( !self->rep_retry_id )
        goto EXIT;

    self->rep_retry_id = 0;

    mce_log(LL_WARN, "reporting(%s): retry",
            sfw_plugin_get_sensor_name(self->rep_plugin));

    if( self->rep_state == REPORTING_ERROR )
        sfw_reporting_trans(self, REPORTING_RETHINK);

EXIT:

    return FALSE;
}

/** Choose between sensor start() and stop()
 */
static void
sfw_reporting_do_rethink(sfw_reporting_t *self)
{
    switch( self->rep_state ) {
    case REPORTING_IDLE:
    case REPORTING_INITIAL:
        // nop
        break;

    default:
        sfw_reporting_trans(self, REPORTING_RETHINK);
        break;
    }
}

/** Initiate sensor start() request
 */
static void
sfw_reporting_do_start(sfw_reporting_t *self)
{
    switch( self->rep_state ) {
    case REPORTING_IDLE:
    case REPORTING_INITIAL:
        sfw_reporting_trans(self, REPORTING_RETHINK);
        break;

    default:
        // nop
        break;
    }
}

/** Initiate sensor stop() request
 */
static void
sfw_reporting_do_reset(sfw_reporting_t *self)
{
    sfw_reporting_trans(self, REPORTING_IDLE);
}

/* ========================================================================= *
 * SENSORFW_OVERRIDE
 * ========================================================================= */

/** Translate override state to human readable form
 */
static const char *
sfw_override_state_name(sfw_override_state_t state)
{
    static const char *const lut[OVERRIDE_NUMSTATES] =
    {
        [OVERRIDE_INITIAL]      = "INITIAL",
        [OVERRIDE_IDLE]         = "IDLE",
        [OVERRIDE_RETHINK]      = "RETHINK",
        [OVERRIDE_ENABLING]     = "ENABLING",
        [OVERRIDE_ENABLED]      = "ENABLED",
        [OVERRIDE_DISABLING]    = "DISABLING",
        [OVERRIDE_DISABLED]     = "DISABLED",
        [OVERRIDE_ERROR]        = "ERROR",
    };

    return (state < OVERRIDE_NUMSTATES) ? lut[state] : 0;
}

/** Create sensor standby set/unset state machine object
 */
static sfw_override_t *
sfw_override_create(sfw_plugin_t *plugin)
{
    sfw_override_t *self = calloc(1, sizeof *self);

    self->ovr_plugin   = plugin;
    self->ovr_state    = OVERRIDE_INITIAL;
    self->ovr_start_pc = 0;
    self->ovr_target   = false;
    self->ovr_retry_id = 0;

    return self;
}

/** Delete sensor standby set/unset state machine object
 */
static void
sfw_override_delete(sfw_override_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self ) {
        sfw_override_trans(self, OVERRIDE_INITIAL);
        self->ovr_plugin = 0;
        free(self);
    }
}

/** Set sensor standby override set/unset target state
 */
static void
sfw_override_set_target(sfw_override_t *self, bool enable)
{
    if( self->ovr_target != enable ) {
        self->ovr_target = enable;
        sfw_override_do_rethink(self);
    }
}

/** Cancel pending sensor standby override set/unset method call
 */
static void
sfw_override_cancel_start(sfw_override_t *self)
{
    if( self->ovr_start_pc ) {
        dbus_pending_call_cancel(self->ovr_start_pc);
        dbus_pending_call_unref(self->ovr_start_pc);
        self->ovr_start_pc = 0;
    }
}

/** Cancel pending ipc retry timer
 */
static void
sfw_override_cancel_retry(sfw_override_t *self)
{
    if( self->ovr_retry_id ) {
        g_source_remove(self->ovr_retry_id);
        self->ovr_retry_id = 0;
    }
}

/** Make a state transition
 */
static void
sfw_override_trans(sfw_override_t *self, sfw_override_state_t state)
{
    dbus_int32_t sid  = sfw_plugin_get_session_id(self->ovr_plugin);
    dbus_bool_t  val  = self->ovr_target;

    if( self->ovr_state == state )
        goto EXIT;

    sfw_override_cancel_start(self);
    sfw_override_cancel_retry(self);

    mce_log(LL_DEBUG, "override(%s): %s -> %s",
            sfw_plugin_get_sensor_name(self->ovr_plugin),
            sfw_override_state_name(self->ovr_state),
            sfw_override_state_name(state));

    self->ovr_state = state;

    switch( self->ovr_state ) {
    case OVERRIDE_RETHINK:
        if( self->ovr_target )
            sfw_override_trans(self, OVERRIDE_ENABLING);
        else
            sfw_override_trans(self, OVERRIDE_DISABLING);
        break;

    case OVERRIDE_ENABLING:
    case OVERRIDE_DISABLING:
        dbus_send_ex(SENSORFW_SERVICE,
                     sfw_plugin_get_sensor_object(self->ovr_plugin),
                     sfw_plugin_get_sensor_interface(self->ovr_plugin),
                     SENSORFW_SENSOR_METHOD_SET_OVERRIDE,
                     sfw_override_start_cb,
                     self, 0, &self->ovr_start_pc,
                     DBUS_TYPE_INT32, &sid,
                     DBUS_TYPE_BOOLEAN, &val,
                     DBUS_TYPE_INVALID);
        break;

    case OVERRIDE_ENABLED:
    case OVERRIDE_DISABLED:
        // NOP
        break;

    case OVERRIDE_ERROR:
        self->ovr_retry_id = g_timeout_add(SENSORFW_RETRY_DELAY_MS,
                                           sfw_override_retry_cb,
                                           self);
        break;

    default:
    case OVERRIDE_IDLE:
    case OVERRIDE_INITIAL:
        // NOP
        break;

    }

EXIT:

    return;
}

/** Handle reply to sensor standby override set/unset method call
 */
static void
sfw_override_start_cb(DBusPendingCall *pc, void *aptr)
{
    sfw_override_t *self = aptr;

    DBusMessage *rsp = 0;
    DBusError    err = DBUS_ERROR_INIT;
    dbus_bool_t  ack = FALSE;

    if( !pc || !self || pc != self->ovr_start_pc )
        goto EXIT;

    dbus_pending_call_unref(self->ovr_start_pc),
        self->ovr_start_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
        mce_log(LL_ERR, "override(%s): no reply",
                sfw_plugin_get_sensor_name(self->ovr_plugin));
        goto EXIT;
    }

    if( dbus_set_error_from_message(&err, rsp) ) {
        mce_log(LL_ERR, "override(%s): error reply: %s: %s",
                sfw_plugin_get_sensor_name(self->ovr_plugin),
                err.name, err.message);
        goto EXIT;
    }

    if( !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_BOOLEAN, &ack,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "override(%s): parse error: %s: %s",
                sfw_plugin_get_sensor_name(self->ovr_plugin),
                err.name, err.message);
        goto EXIT;
    }

EXIT:
    mce_log(LL_DEBUG, "override(%s): ack=%d",
            sfw_plugin_get_sensor_name(self->ovr_plugin), ack);

    if( self->ovr_state == OVERRIDE_ENABLING ||
        self->ovr_state == OVERRIDE_DISABLING ) {

        if( !ack )
            sfw_override_trans(self, OVERRIDE_ERROR);
        else if( self->ovr_state == OVERRIDE_ENABLING )
            sfw_override_trans(self, OVERRIDE_ENABLED);
        else
            sfw_override_trans(self, OVERRIDE_DISABLED);
    }

    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);

    return;
}

/** Handle triggering of start/stop retry timer
 */
static gboolean
sfw_override_retry_cb(gpointer aptr)
{
    sfw_override_t *self = aptr;

    if( !self->ovr_retry_id )
        goto EXIT;

    self->ovr_retry_id = 0;

    mce_log(LL_WARN, "override(%s): retry",
            sfw_plugin_get_sensor_name(self->ovr_plugin));

    if( self->ovr_state == OVERRIDE_ERROR )
        sfw_override_trans(self, OVERRIDE_RETHINK);

EXIT:

    return FALSE;
}

/** Choose between sensor standby override set/unset
 */
static void
sfw_override_do_rethink(sfw_override_t *self)
{
    switch( self->ovr_state ) {
    case OVERRIDE_IDLE:
    case OVERRIDE_INITIAL:
        // nop
        break;

    default:
        sfw_override_trans(self, OVERRIDE_RETHINK);
        break;
    }
}

/** Initiate sensor standby override set request
 */
static void
sfw_override_do_start(sfw_override_t *self)
{
    switch( self->ovr_state ) {
    case OVERRIDE_IDLE:
    case OVERRIDE_INITIAL:
        sfw_override_trans(self, OVERRIDE_RETHINK);
        break;

    default:
        // nop
        break;
    }
}

/** Initiate sensor standby override unset request
 */
static void
sfw_override_do_reset(sfw_override_t *self)
{
    sfw_override_trans(self, OVERRIDE_IDLE);
}

/* ========================================================================= *
 * SENSORFW_CONNECTION
 * ========================================================================= */

/** Translate connection state to human readable form
 */
static const char *
sfw_connection_state_name(sfw_connection_state_t state)
{
    static const char *const lut[CONNECTION_NUMSTATES] =
    {
        [CONNECTION_INITIAL]       = "INITIAL",
        [CONNECTION_IDLE]          = "IDLE",
        [CONNECTION_CONNECTING]    = "CONNECTING",
        [CONNECTION_REGISTERING]   = "REGISTERING",
        [CONNECTION_CONNECTED]     = "CONNECTED",
        [CONNECTION_ERROR]         = "ERROR",
    };

    return (state < CONNECTION_NUMSTATES) ? lut[state] : 0;
}

/** Get cached session id granted by sensord
 */
static int
sfw_connection_get_session_id(const sfw_connection_t *self)
{
    return sfw_plugin_get_session_id(self->con_plugin);
}

/** Handle array of sensor events sent by sensord
 */
static bool
sfw_connection_handle_samples(sfw_connection_t *self,
                              char *data, size_t size)
{
    bool     res    = false;
    uint32_t count  = 0;
    uint32_t block  = sfw_plugin_get_sample_size(self->con_plugin);
    void    *sample = 0;

    while( size > 0 ) {
        if( size < sizeof count ) {
            mce_log(LL_ERR, "connection(%s): received invalid packet",
                    sfw_plugin_get_sensor_name(self->con_plugin));
            goto EXIT;
        }

        memcpy(&count, data, sizeof count);
        data += sizeof count;
        size -= sizeof count;

        if( size < count * block ) {
            mce_log(LL_ERR, "connection(%s): received invalid sample",
                    sfw_plugin_get_sensor_name(self->con_plugin));
            goto EXIT;
        }

        if( count > 0 ) {
            sample = data + block * (count - 1);
            data += block * count;
            size -= block * count;
        }
    }

    if( !sample ) {
            mce_log(LL_ERR, "connection(%s): no sample was received",
                    sfw_plugin_get_sensor_name(self->con_plugin));
        goto EXIT;
    }

    res = true;
    sfw_plugin_handle_sample(self->con_plugin, sample);

EXIT:
    return res;
}

/** Do a handshake with sensord after opening a data connection
 */
static bool
sfw_connection_tx_req(sfw_connection_t *self)
{
    bool    res = false;
    int32_t sid = sfw_connection_get_session_id(self);

    errno = 0;
    if( write(self->con_fd, &sid, sizeof sid) != sizeof sid ) {
        mce_log(LL_ERR, "connection(%s): handshake write failed: %m",
                sfw_plugin_get_sensor_name(self->con_plugin));
        goto EXIT;
    }

    res = true;

EXIT:
    return res;
}

/** Handle reply to handshake from sensord
 */
static bool
sfw_connection_rx_ack(sfw_connection_t *self)
{
    bool res = false;
    char ack = 0;

    errno = 0;
    if( read(self->con_fd, &ack, sizeof ack) != sizeof ack ) {
        mce_log(LL_ERR, "connection(%s): handshake read failed: %m",
                sfw_plugin_get_sensor_name(self->con_plugin));
        goto EXIT;
    }

    if( ack != '\n' ) {
        mce_log(LL_ERR, "connection(%s): invalid handshake received",
                sfw_plugin_get_sensor_name(self->con_plugin));
        goto EXIT;
    }

    res = true;

EXIT:
    return res;
}

/** Handle sensor events received over data connection
 */
static bool
sfw_connection_rx_dta(sfw_connection_t *self)
{
    bool    res = false;

    char    dta[1024];

    if( self->con_fd == -1 )
        goto EXIT;

    errno = 0;
    int rc = read(self->con_fd, dta, sizeof dta);

    if( rc == 0 ) {
        mce_log(LL_ERR, "connection(%s): received EOF",
                sfw_plugin_get_sensor_name(self->con_plugin));
        goto EXIT;
    }

    if( rc == -1 ) {
        if( errno != EAGAIN && errno != EINTR )
            mce_log(LL_ERR, "connection(%s): received ERR; %m",
                    sfw_plugin_get_sensor_name(self->con_plugin));
        else
            res = true;
        goto EXIT;
    }

    mce_log(LL_DEBUG, "connection(%s): received %d bytes",
            sfw_plugin_get_sensor_name(self->con_plugin), rc);

    /* Note: Writing smallish chunks to unix domain socket
     *       either fully succeeds or fails/sender blocks.
     *
     *       With this in mind it is assumed that a succesful
     *       read will contain fully valid sample data. However,
     *       depending on how sensord actually does the sending,
     *       there is a slight hazard point here...
     *
     *       It might be that separate state needs to be
     *       introduced for handling header (sample count)
     *       and payload (sensor samples) separately.
     */
    res = sfw_connection_handle_samples(self, dta, rc);

EXIT:
    return res;
}

/** Handle sensor data connection is writable conditions
 *
 * Used for initial session handshake only.
 */
static gboolean
sfw_connection_tx_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
{
    (void)chn;
    (void)cnd;

    sfw_connection_t *self = aptr;

    gboolean keep_going = FALSE;
    bool     success    = false;

    if( self->con_state != CONNECTION_CONNECTING )
        goto EXIT;

    success = sfw_connection_tx_req(self);

EXIT:
    if( !keep_going ) {
        mce_log(LL_DEBUG, "connection(%s): disabling tx notify",
                sfw_plugin_get_sensor_name(self->con_plugin));
        self->con_tx_id = 0;
    }

    if( success )
        sfw_connection_trans(self, CONNECTION_REGISTERING);
    else
        sfw_connection_trans(self, CONNECTION_ERROR);

    return keep_going;
}

/** Handle data available from data connection
 *
 * Used 1st for handling handshake reply and then for
 * receiving sensor event packets.
 */
static gboolean
sfw_connection_rx_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
{
    (void)chn;
    (void)cnd;

    sfw_connection_t *self = aptr;

    sfw_connection_state_t next_state = self->con_state;

    switch( self->con_state )
    {
    case CONNECTION_REGISTERING:
        if( sfw_connection_rx_ack(self) )
            next_state = CONNECTION_CONNECTED;
        else
            next_state = CONNECTION_ERROR;
        break;

    case CONNECTION_CONNECTED:
        if( !sfw_connection_rx_dta(self) )
            next_state = CONNECTION_ERROR;
        break;

    default:
        next_state = CONNECTION_ERROR;
        break;
    }

    bool keep_going = (next_state != CONNECTION_ERROR);

    if( !keep_going ) {
        mce_log(LL_CRIT, "connection(%s): disabling rx notify",
                sfw_plugin_get_sensor_name(self->con_plugin));
        self->con_rx_id = 0;
    }

    sfw_connection_trans(self, next_state);

    return keep_going;
}

/** Remove data available glib I/O watch
 */
static void
sfw_connection_remove_rx_notify(sfw_connection_t *self)
{
    if( self->con_rx_id ) {
        mce_log(LL_DEBUG, "connection(%s): removing rx notify",
                sfw_plugin_get_sensor_name(self->con_plugin));
        g_source_remove(self->con_rx_id),
            self->con_rx_id = 0;
    }
}

/** Remove ready to send glib I/O watch
 */
static void
sfw_connection_remove_tx_notify(sfw_connection_t *self)
{
    if( self->con_tx_id ) {
        mce_log(LL_DEBUG, "connection(%s): removing tx notify",
                sfw_plugin_get_sensor_name(self->con_plugin));
        g_source_remove(self->con_tx_id),
            self->con_tx_id = 0;
    }
}

/** Close sensord data connection
 */
static void
sfw_connection_close_socket(sfw_connection_t *self)
{
    sfw_connection_remove_tx_notify(self);
    sfw_connection_remove_rx_notify(self);

    if( self->con_fd != -1 ) {
        mce_log(LL_DEBUG, "connection(%s): closing socket",
                sfw_plugin_get_sensor_name(self->con_plugin));
        close(self->con_fd),
            self->con_fd = -1;
    }
}

/** Open sensord data connection
 */
static bool
sfw_connection_open_socket(sfw_connection_t *self)
{
    bool res = false;

    sfw_connection_close_socket(self);

    if( (self->con_fd = sfw_socket_open()) == -1 )
        goto EXIT;

    mce_log(LL_DEBUG, "connection(%s): opened socket",
            sfw_plugin_get_sensor_name(self->con_plugin));

    self->con_tx_id = sfw_socket_add_notify(self->con_fd, false, G_IO_OUT,
                                            sfw_connection_tx_cb, self);

    if( !self->con_tx_id )
        goto EXIT;

    mce_log(LL_DEBUG, "connection(%s): added tx notify",
            sfw_plugin_get_sensor_name(self->con_plugin));

    self->con_rx_id = sfw_socket_add_notify(self->con_fd, false, G_IO_IN,
                                            sfw_connection_rx_cb, self);

    if( !self->con_rx_id )
        goto EXIT;

    mce_log(LL_DEBUG, "connection(%s): added rx notify",
            sfw_plugin_get_sensor_name(self->con_plugin));

    res = true;

EXIT:

    /* all or nothing */
    if( !res )
        sfw_connection_close_socket(self);

    return res;
}

/** Handle triggering of ipc retry timer
 */
static gboolean
sfw_connection_retry_cb(gpointer aptr)
{
    sfw_connection_t *self = aptr;

    if( !self->con_retry_id )
        goto EXIT;

    self->con_retry_id = 0;

    mce_log(LL_WARN, "connection(%s): retry",
            sfw_plugin_get_sensor_name(self->con_plugin));

    if( self->con_state == CONNECTION_ERROR )
        sfw_connection_trans(self, CONNECTION_CONNECTING);

EXIT:

    return FALSE;
}

/** Cancel pending ipc retry timer
 */
static void
sfw_connection_cancel_retry(sfw_connection_t *self)
{
    if( self->con_retry_id ) {
        g_source_remove(self->con_retry_id);
        self->con_retry_id = 0;
    }
}

/** Make a state transition
 */
static void
sfw_connection_trans(sfw_connection_t *self, sfw_connection_state_t state)
{
    if( self->con_state == state )
        goto EXIT;

    mce_log(LL_DEBUG, "connection(%s): %s -> %s",
            sfw_plugin_get_sensor_name(self->con_plugin),
            sfw_connection_state_name(self->con_state),
            sfw_connection_state_name(state));

    self->con_state = state;

    sfw_connection_cancel_retry(self);

    switch( self->con_state ) {
    case CONNECTION_CONNECTING:
        if( !sfw_connection_open_socket(self) )
            sfw_connection_trans(self, CONNECTION_ERROR);
        break;

    case CONNECTION_REGISTERING:
        sfw_connection_remove_tx_notify(self);
        break;

    case CONNECTION_CONNECTED:
        sfw_plugin_do_override_start(self->con_plugin);
        sfw_plugin_do_reporting_start(self->con_plugin);
        break;

    default:
    case CONNECTION_IDLE:
    case CONNECTION_ERROR:
    case CONNECTION_INITIAL:
        sfw_plugin_do_reporting_reset(self->con_plugin);
        sfw_plugin_do_override_reset(self->con_plugin);
        sfw_connection_close_socket(self);

        if( self->con_state == CONNECTION_ERROR )
            self->con_retry_id = g_timeout_add(SENSORFW_RETRY_DELAY_MS,
                                               sfw_connection_retry_cb,
                                               self);
        break;
    }

EXIT:
    return;
}

/** Create sensord data connection state machine object
 */
static sfw_connection_t *
sfw_connection_create(sfw_plugin_t *plugin)
{
    sfw_connection_t *self = calloc(1, sizeof *self);

    self->con_plugin   = plugin;
    self->con_state    = CONNECTION_INITIAL;
    self->con_fd       = -1;
    self->con_rx_id    = 0;
    self->con_tx_id    = 0;
    self->con_retry_id = 0;

    return self;
}

/** Delete sensord data connection state machine object
 */
static void
sfw_connection_delete(sfw_connection_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self ) {
        sfw_connection_trans(self, CONNECTION_INITIAL);
        self->con_plugin = 0;
        free(self);
    }
}

/** Initiate data connection
 */
static void
sfw_connection_do_start(sfw_connection_t *self)
{
    switch( self->con_state ) {
    case CONNECTION_IDLE:
    case CONNECTION_INITIAL:
        sfw_connection_trans(self, CONNECTION_CONNECTING);
        break;

    default:
        // nop
        break;
    }
}

/** Close data connection
 */
static void
sfw_connection_do_reset(sfw_connection_t *self)
{
    sfw_connection_trans(self, CONNECTION_IDLE);
}

/* ========================================================================= *
 * SENSORFW_SESSION
 * ========================================================================= */

/** Translate session state to human readable form
 */
static const char *
sfw_session_state_name(sfw_session_state_t state)
{
    static const char *const lut[SESSION_NUMSTATES] =
    {
        [SESSION_INITIAL]    = "INITIAL",
        [SESSION_IDLE]       = "IDLE",
        [SESSION_REQUESTING] = "REQUESTING",
        [SESSION_ACTIVE]     = "ACTIVE",
        [SESSION_ERROR]      = "ERROR",
        [SESSION_INVALID]    = "INVALID",
    };

    return (state < SESSION_NUMSTATES) ? lut[state] : 0;
}

/** Create sensor data session state machine object
 */
static sfw_session_t *
sfw_session_create(sfw_plugin_t *plugin)
{
    sfw_session_t *self = calloc(1, sizeof *self);

    self->ses_plugin   = plugin;
    self->ses_state    = SESSION_INITIAL;
    self->ses_id       = SESSION_ID_INVALID;
    self->ses_start_pc = 0;
    self->ses_retry_id = 0;

    return self;
}

/** Delete sensor data session state machine object
 */
static void
sfw_session_delete(sfw_session_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self ) {
        sfw_session_trans(self, SESSION_INITIAL);
        self->ses_plugin = 0;
        free(self);
    }
}

/** Get session id granted by sensord
 */
static int
sfw_session_get_id(const sfw_session_t *self)
{
    // Explicitly allow NULL self pointer to be used
    return self ? self->ses_id : SESSION_ID_INVALID;
}

/** Cancel pending session start method call
 */
static void
sfw_session_cancel_start(sfw_session_t *self)
{
    if( self->ses_start_pc ) {
        dbus_pending_call_cancel(self->ses_start_pc);
        dbus_pending_call_unref(self->ses_start_pc);
        self->ses_start_pc = 0;
    }
}

/** Cancel pending ipc retry timer
 */
static void
sfw_session_cancel_retry(sfw_session_t *self)
{
    if( self->ses_retry_id ) {
        g_source_remove(self->ses_retry_id);
        self->ses_retry_id = 0;
    }
}

/** Make a state transition
 */
static void
sfw_session_trans(sfw_session_t *self, sfw_session_state_t state)
{
    if( self->ses_state == state )
        goto EXIT;

    sfw_session_cancel_start(self);
    sfw_session_cancel_retry(self);

    mce_log(LL_DEBUG, "session(%s): %s -> %s",
            sfw_plugin_get_sensor_name(self->ses_plugin),
            sfw_session_state_name(self->ses_state),
            sfw_session_state_name(state));

    self->ses_state = state;

    switch( self->ses_state ) {
    case SESSION_REQUESTING:
        {
            dbus_int64_t  pid  = getpid();
            const char   *name = sfw_plugin_get_sensor_name(self->ses_plugin);

            dbus_send_ex(SENSORFW_SERVICE,
                         SENSORFW_MANAGER_OBJECT,
                         SENSORFW_MANAGER_INTEFCACE,
                         SENSORFW_MANAGER_METHOD_START_SESSION,
                         sfw_session_start_cb,
                         self, 0, &self->ses_start_pc,
                         DBUS_TYPE_STRING, &name,
                         DBUS_TYPE_INT64,  &pid,
                         DBUS_TYPE_INVALID);
        }
        break;

    case SESSION_ACTIVE:
        sfw_plugin_do_connection_start(self->ses_plugin);
        break;

    default:
    case SESSION_IDLE:
    case SESSION_ERROR:
    case SESSION_INVALID:
    case SESSION_INITIAL:
        sfw_plugin_do_connection_reset(self->ses_plugin);

        if( self->ses_state == SESSION_ERROR )
            self->ses_retry_id = g_timeout_add(SENSORFW_RETRY_DELAY_MS,
                                               sfw_session_retry_cb,
                                               self);
        break;
    }

EXIT:

    return;
}

/** Handle reply to data session start request
 */
static void
sfw_session_start_cb(DBusPendingCall *pc, void *aptr)
{
    sfw_session_t *self = aptr;

    DBusMessage *rsp = 0;
    DBusError    err = DBUS_ERROR_INIT;
    dbus_int32_t ses = SESSION_ID_UNKNOWN;

    if( !pc || !self || pc != self->ses_start_pc )
        goto EXIT;

    dbus_pending_call_unref(self->ses_start_pc),
        self->ses_start_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
        mce_log(LL_ERR, "session(%s): no reply",
                sfw_plugin_get_sensor_name(self->ses_plugin));
        goto EXIT;
    }

    if( dbus_set_error_from_message(&err, rsp) ) {
        mce_log(LL_ERR, "session(%s): error reply: %s: %s",
                sfw_plugin_get_sensor_name(self->ses_plugin),
                err.name, err.message);
        goto EXIT;
    }

    if( !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_INT32, &ses,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "session(%s): parse error: %s: %s",
                sfw_plugin_get_sensor_name(self->ses_plugin),
                err.name, err.message);
        goto EXIT;
    }

EXIT:
    mce_log(LL_DEBUG, "session(%s): sid=%d",
            sfw_plugin_get_sensor_name(self->ses_plugin),
            (int)ses);

    switch( ses ) {
    case SESSION_ID_INVALID:
        self->ses_id = SESSION_ID_INVALID;
        sfw_session_trans(self, SESSION_INVALID);
        break;

    case SESSION_ID_UNKNOWN:
        self->ses_id = SESSION_ID_INVALID;
        sfw_session_trans(self, SESSION_ERROR);
        break;

    default:
        self->ses_id = ses;
        sfw_session_trans(self, SESSION_ACTIVE);
    }

    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);

    return;
}

/** Handle triggering of start/stop retry timer
 */
static gboolean
sfw_session_retry_cb(gpointer aptr)
{
    sfw_session_t *self = aptr;

    if( !self->ses_retry_id )
        goto EXIT;

    self->ses_retry_id = 0;

    mce_log(LL_WARN, "session(%s): retry",
            sfw_plugin_get_sensor_name(self->ses_plugin));

    if( self->ses_state == SESSION_ERROR )
        sfw_session_trans(self, SESSION_REQUESTING);

EXIT:

    return FALSE;
}

/** Initiate data session
 */
static void
sfw_session_do_start(sfw_session_t *self)
{
    switch( self->ses_state ) {
    case SESSION_IDLE:
    case SESSION_INITIAL:
        sfw_session_trans(self, SESSION_REQUESTING);
        break;
    default:
        // nop
        break;
    }
}

/** Close data session
 */
static void
sfw_session_do_reset(sfw_session_t *self)
{
    sfw_session_trans(self, SESSION_IDLE);
}

/* ========================================================================= *
 * SENSORFW_PLUGIN
 * ========================================================================= */

/** Translate plugin state to human readable form
 */
static const char *
sfw_plugin_state_name(sfw_plugin_state_t state)
{
    static const char *const lut[PLUGIN_NUMSTATES] =
    {
        [PLUGIN_INITIAL]  = "INITIAL",
        [PLUGIN_IDLE]     = "IDLE",
        [PLUGIN_LOADING]  = "LOADING",
        [PLUGIN_LOADED]   = "LOADED",
        [PLUGIN_ERROR]    = "ERROR",
    };

    return (state < PLUGIN_NUMSTATES) ? lut[state] : 0;
}

/** Get sensord compatible name of the sensor
 */
static const char *
sfw_plugin_get_sensor_name(const sfw_plugin_t *self)
{
    // Explicitly allow NULL self pointer to be used
    return self ? self->plg_backend->be_sensor_name : 0;
}

/** Get sensord compatible D-Bus object path for the sensor
 */
static const char *
sfw_plugin_get_sensor_object(const sfw_plugin_t *self)
{
    // Explicitly allow NULL self pointer to be used
    if( !self )
        return 0;

    return self->plg_sensor_object ?: self->plg_backend->be_sensor_object;
}

/** Get sensord compatible D-Bus interface for the sensor
 */
static const char *
sfw_plugin_get_sensor_interface(const sfw_plugin_t *self)
{
    // Explicitly allow NULL self pointer to be used
    return self ? self->plg_backend->be_sensor_interface : 0;
}

/** Get cached session id granted by sensord
 */
static int
sfw_plugin_get_session_id(const sfw_plugin_t *self)
{
    // Explicitly allow NULL self pointer to be used
    return self ? sfw_session_get_id(self->plg_session) : SESSION_ID_INVALID;
}

/** Handle sensor specific change event received from data connection
 */
static void
sfw_plugin_handle_sample(sfw_plugin_t *self, const void *sample)
{
    if( self->plg_backend->be_sample_cb )
        self->plg_backend->be_sample_cb(self, sample);
}

/** Handle sensor specific initial value received via dbus query
 */
static void
sfw_plugin_handle_value(sfw_plugin_t *self, unsigned value)
{
    mce_log(LL_DEBUG, "plugin(%s): initial=%u",
            sfw_plugin_get_sensor_name(self), value);

    if( self->plg_backend->be_value_cb )
        self->plg_backend->be_value_cb(self, value);
}

/** Reset sensor state to assumed default state
 *
 * Used when sensord drops from D-Bus or sensor start() or stop()
 * ceases to function.
 */
static void
sfw_plugin_reset_value(sfw_plugin_t *self)
{
    mce_log(LL_DEBUG, "plugin(%s): reset",
            sfw_plugin_get_sensor_name(self));

    if( self->plg_backend->be_reset_cb )
        self->plg_backend->be_reset_cb(self);
}

/** Restore sensor state to last seen state
 *
 * Used when sensord comes to D-Bus.
 */
static void
sfw_plugin_restore_value(sfw_plugin_t *self)
{
    mce_log(LL_DEBUG, "plugin(%s): restore",
            sfw_plugin_get_sensor_name(self));

    if( self->plg_backend->be_restore_cb )
        self->plg_backend->be_restore_cb(self);
}

/** Set sensor state when stopped
 *
 * Used when sensor tracking is stopped
 */
static void
sfw_plugin_forget_value(sfw_plugin_t *self)
{
    mce_log(LL_DEBUG, "plugin(%s): stopped",
            sfw_plugin_get_sensor_name(self));

    if( self->plg_backend->be_forget_cb )
        self->plg_backend->be_forget_cb(self);
}

/** Get size of sensor specific change event
 */
static size_t
sfw_plugin_get_sample_size(const sfw_plugin_t *self)
{
    return self->plg_backend->be_sample_size;
}

/** Get name of sensor method for querying initial value
 */
static const char *
sfw_plugin_get_value_method(const sfw_plugin_t *self)
{
    return self->plg_backend->be_value_method;
}

/** Cancel pending plugin load() dbus method call
 */
static void
sfw_plugin_cancel_load(sfw_plugin_t *self)
{
    if( self->plg_load_pc ) {
        dbus_pending_call_cancel(self->plg_load_pc);
        dbus_pending_call_unref(self->plg_load_pc);
        self->plg_load_pc = 0;
    }
}

/** Cancel pending ipc retry timer
 */
static void
sfw_plugin_cancel_retry(sfw_plugin_t *self)
{
    if( self->plg_retry_id ) {
        g_source_remove(self->plg_retry_id);
        self->plg_retry_id = 0;
    }
}

/** Make a state transition
 */
static void
sfw_plugin_trans(sfw_plugin_t *self, sfw_plugin_state_t state)
{
    if( self->plg_state == state )
        goto EXIT;

    mce_log(LL_DEBUG, "plugin(%s): %s -> %s",
            sfw_plugin_get_sensor_name(self),
            sfw_plugin_state_name(self->plg_state),
            sfw_plugin_state_name(state));

    self->plg_state = state;

    sfw_plugin_cancel_load(self);
    sfw_plugin_cancel_retry(self);

    switch( self->plg_state ) {
    case PLUGIN_IDLE:
    case PLUGIN_ERROR:
    case PLUGIN_INITIAL:
        sfw_plugin_do_session_reset(self);

        if( self->plg_state == PLUGIN_ERROR )
            self->plg_retry_id = g_timeout_add(SENSORFW_RETRY_DELAY_MS,
                                               sfw_plugin_retry_cb,
                                               self);
        break;

    case PLUGIN_LOADING:
        {
            const char *sensor_name = sfw_plugin_get_sensor_name(self);

            dbus_send_ex(SENSORFW_SERVICE,
                         SENSORFW_MANAGER_OBJECT,
                         SENSORFW_MANAGER_INTEFCACE,
                         SENSORFW_MANAGER_METHOD_LOAD_PLUGIN,
                         sfw_plugin_load_cb,
                         self, 0, &self->plg_load_pc,
                         DBUS_TYPE_STRING, &sensor_name,
                         DBUS_TYPE_INVALID);
        }
        break;

    case PLUGIN_LOADED:
        sfw_plugin_do_session_start(self);
        break;

    default:
        break;
    }

EXIT:
    return;
}

/** Create sensor plugin load/unload state machine object
 */
static sfw_plugin_t *
sfw_plugin_create(const sfw_backend_t *backend)
{
    sfw_plugin_t *self = calloc(1, sizeof *self);

    self->plg_state         = PLUGIN_INITIAL;
    self->plg_backend       = backend;
    self->plg_sensor_object = 0;
    self->plg_load_pc       = 0;
    self->plg_retry_id      = 0;

    /* If backend does not define object path, construct it
     * from manager object path + sensor name */
    if( !self->plg_backend->be_sensor_object ) {
        self->plg_sensor_object =
            g_strdup_printf("%s/%s",
                            SENSORFW_MANAGER_OBJECT,
                            self->plg_backend->be_sensor_name);
    }

    mce_log(LL_DEBUG, "Initializing sensor plugin");
    mce_log(LL_DEBUG, "sensor:    %s", sfw_plugin_get_sensor_name(self));
    mce_log(LL_DEBUG, "object:    %s", sfw_plugin_get_sensor_object(self));
    mce_log(LL_DEBUG, "interface: %s", sfw_plugin_get_sensor_interface(self));

    self->plg_session    = sfw_session_create(self);
    self->plg_connection = sfw_connection_create(self);
    self->plg_override   = sfw_override_create(self);
    self->plg_reporting  = sfw_reporting_create(self);

    return self;
}

/** Delete sensor plugin load/unload state machine object
 */
static void
sfw_plugin_delete(sfw_plugin_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self ) {
        sfw_plugin_trans(self, PLUGIN_IDLE);

        sfw_reporting_delete(self->plg_reporting),
            self->plg_reporting = 0;

        sfw_override_delete(self->plg_override),
            self->plg_override = 0;

        sfw_connection_delete(self->plg_connection),
            self->plg_connection = 0;

        sfw_session_delete(self->plg_session),
            self->plg_session = 0;

        g_free(self->plg_sensor_object),
            self->plg_sensor_object = 0;

        free(self);
    }
}

/** Handle reply to plugin load() dbus method call
 */
static void
sfw_plugin_load_cb(DBusPendingCall *pc, void *aptr)
{
    sfw_plugin_t *self = aptr;
    DBusMessage  *rsp  = 0;
    DBusError     err  = DBUS_ERROR_INIT;
    dbus_bool_t   ack  = FALSE;

    if( !pc || !self || pc != self->plg_load_pc )
        goto EXIT;

    dbus_pending_call_unref(self->plg_load_pc),
        self->plg_load_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) ) {
        mce_log(LL_ERR, "plugin(%s): no reply",
                sfw_plugin_get_sensor_name(self));
        goto EXIT;
    }

    if( dbus_set_error_from_message(&err, rsp) ) {
        mce_log(LL_ERR, "plugin(%s): error reply: %s: %s",
                sfw_plugin_get_sensor_name(self), err.name, err.message);
        goto EXIT;
    }

    if( !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_BOOLEAN, &ack,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "plugin(%s): parse error: %s: %s",
                sfw_plugin_get_sensor_name(self), err.name, err.message);
        goto EXIT;
    }

EXIT:

    if( self->plg_state == PLUGIN_LOADING) {
        if( ack )
            sfw_plugin_trans(self, PLUGIN_LOADED);
        else
            sfw_plugin_trans(self, PLUGIN_ERROR);
    }

    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);

    return;
}

/** Handle triggering of ipc retry timer
 */
static gboolean
sfw_plugin_retry_cb(gpointer aptr)
{
    sfw_plugin_t *self = aptr;

    if( !self->plg_retry_id )
        goto EXIT;

    self->plg_retry_id = 0;

    mce_log(LL_WARN, "plugin(%s): retry",
            sfw_plugin_get_sensor_name(self));

    if( self->plg_state == PLUGIN_ERROR )
        sfw_plugin_trans(self, PLUGIN_LOADING);

EXIT:

    return FALSE;
}

/** Initiate plugin loading
 */
static void
sfw_plugin_do_load(sfw_plugin_t *self)
{
    switch( self->plg_state ) {
    case PLUGIN_IDLE:
    case PLUGIN_INITIAL:
        sfw_plugin_trans(self, PLUGIN_LOADING);
        break;
    default:
        // nop
        break;
    }
}

/** Unload plugin
 */
static void
sfw_plugin_do_reset(sfw_plugin_t *self)
{
    sfw_plugin_trans(self, PLUGIN_IDLE);
}

/** Initiate data session
 */
static void
sfw_plugin_do_session_start(sfw_plugin_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self && self->plg_session )
            sfw_session_do_start(self->plg_session);
}

/** Close data session
 */
static void
sfw_plugin_do_session_reset(sfw_plugin_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self && self->plg_session )
            sfw_session_do_reset(self->plg_session);
}

/** Initiate data connection
 */
static void
sfw_plugin_do_connection_start(sfw_plugin_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self && self->plg_connection )
            sfw_connection_do_start(self->plg_connection);
}

/** Close data connection
 */
static void
sfw_plugin_do_connection_reset(sfw_plugin_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self && self->plg_connection )
            sfw_connection_do_reset(self->plg_connection);
}

/** Initiate standby override handling
 */
static void
sfw_plugin_do_override_start(sfw_plugin_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self && self->plg_override )
            sfw_override_do_start(self->plg_override);
}

/** Cease standby override handling
 */
static void
sfw_plugin_do_override_reset(sfw_plugin_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self && self->plg_override )
            sfw_override_do_reset(self->plg_override);
}

/** Initiate sensor start/stop handling
 */
static void
sfw_plugin_do_reporting_start(sfw_plugin_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self && self->plg_reporting )
            sfw_reporting_do_start(self->plg_reporting);
}

/** Cease sensor start/stop handling
 */
static void
sfw_plugin_do_reporting_reset(sfw_plugin_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self && self->plg_reporting )
            sfw_reporting_do_reset(self->plg_reporting);
}

/* ========================================================================= *
 * SENSORFW_SERVICE
 * ========================================================================= */

/** Translate service state to human readable form
 */
static const char *
sfw_service_state_name(sfw_service_state_t state)
{
    static const char *const lut[SERVICE_NUMSTATES] =
    {
        [SERVICE_UNKNOWN]   = "INITIAL",
        [SERVICE_QUERYING]  = "QUERYING",
        [SERVICE_RUNNING]   = "RUNNING",
        [SERVICE_STOPPED]   = "STOPPED",
    };

    return (state < SERVICE_NUMSTATES) ? lut[state] : 0;
}

/** Create sensord availability tracking state machine object
 */
static sfw_service_t *
sfw_service_create(void)
{
    sfw_service_t *self = calloc(1, sizeof *self);

    self->srv_state    = SERVICE_UNKNOWN;
    self->srv_query_pc = 0;
    self->srv_ps       = sfw_plugin_create(&sfw_backend_ps);
    self->srv_als      = sfw_plugin_create(&sfw_backend_als);
    self->srv_orient   = sfw_plugin_create(&sfw_backend_orient);

    return self;
}

/** Delete sensord availability tracking state machine object
 */
static void
sfw_service_delete(sfw_service_t *self)
{
    // using NULL self pointer explicitly allowed

    if( self ) {
        sfw_service_trans(self, SERVICE_UNKNOWN);

        sfw_plugin_delete(self->srv_ps),
            self->srv_ps = 0;

        sfw_plugin_delete(self->srv_als),
            self->srv_als = 0;

        sfw_plugin_delete(self->srv_orient),
            self->srv_orient = 0;

        free(self);
    }
}

/** Cancel pending sensord name owner dbus method call
 */
static void
sfw_service_cancel_query(sfw_service_t *self)
{
    if( self->srv_query_pc ) {
        dbus_pending_call_cancel(self->srv_query_pc);
        dbus_pending_call_unref(self->srv_query_pc);
        self->srv_query_pc = 0;
    }
}

/** Handle reply to sensord name owner dbus method call
 */
static void
sfw_service_query_cb(DBusPendingCall *pc, void *aptr)
{
    sfw_service_t *self  = aptr;
    DBusMessage   *rsp   = 0;
    const char    *owner = 0;
    DBusError      err   = DBUS_ERROR_INIT;

    if( !pc || !self )
        goto EXIT;

    if( pc != self->srv_query_pc )
        goto EXIT;

    dbus_pending_call_unref(self->srv_query_pc),
        self->srv_query_pc = 0;

    if( !(rsp = dbus_pending_call_steal_reply(pc)) )
        goto EXIT;

    if( dbus_set_error_from_message(&err, rsp) ||
        !dbus_message_get_args(rsp, &err,
                               DBUS_TYPE_STRING, &owner,
                               DBUS_TYPE_INVALID) )
    {
        if( strcmp(err.name, DBUS_ERROR_NAME_HAS_NO_OWNER) )
            mce_log(LL_WARN, "%s: %s", err.name, err.message);
        owner = 0;
    }

EXIT:

    if( self->srv_state == SERVICE_QUERYING ) {
        if( owner && *owner )
            sfw_service_trans(self, SERVICE_RUNNING);
        else
            sfw_service_trans(self, SERVICE_STOPPED);
    }

    if( rsp ) dbus_message_unref(rsp);
    dbus_error_free(&err);
}

/** Make a state transition
 */
static void
sfw_service_trans(sfw_service_t *self, sfw_service_state_t state)
{
    static const char *service_name = SENSORFW_SERVICE;

    if( self->srv_state == state )
        goto EXIT;

    mce_log(LL_DEBUG, "service: %s -> %s",
            sfw_service_state_name(self->srv_state),
            sfw_service_state_name(state));

    self->srv_state = state;

    sfw_service_cancel_query(self);

    switch( self->srv_state ) {
    case SERVICE_QUERYING:
        dbus_send_ex(DBUS_SERVICE_DBUS,
                     DBUS_PATH_DBUS,
                     DBUS_INTERFACE_DBUS,
                     DBUS_METHOD_GET_NAME_OWNER,
                     sfw_service_query_cb,
                     self, 0,
                     &self->srv_query_pc,
                     DBUS_TYPE_STRING, &service_name,
                     DBUS_TYPE_INVALID);
        break;

    case SERVICE_RUNNING:
        sfw_exception_start(SFW_EXCEPTION_LENGTH_SENSORD_RUNNING);
        sfw_plugin_do_load(self->srv_ps);
        sfw_plugin_do_load(self->srv_als);
        sfw_plugin_do_load(self->srv_orient);
        break;

    case SERVICE_UNKNOWN:
    case SERVICE_STOPPED:
        sfw_exception_start(SFW_EXCEPTION_LENGTH_SENSORD_STOPPED);
        sfw_plugin_do_reset(self->srv_ps);
        sfw_plugin_do_reset(self->srv_als);
        sfw_plugin_do_reset(self->srv_orient);
        break;

    default:
        break;
    }

    // TODO: broadcast sensord status over datapipe?

EXIT:
    return;
}

/** Set sensord tracking to available state
 */
static void
sfw_service_do_start(sfw_service_t *self)
{
    sfw_service_trans(self, SERVICE_RUNNING);
}

/** Set sensord tracking to not available state
 */
static void
sfw_service_do_stop(sfw_service_t *self)
{
    sfw_service_trans(self, SERVICE_STOPPED);
}

/** Initiate sensord name owner tracking
 */
static void
sfw_service_do_query(sfw_service_t *self)
{
    if( self->srv_state == SERVICE_UNKNOWN )
        sfw_service_trans(self, SERVICE_QUERYING);
}

/** Perform actions needed when proximity sensor needed state changes
 */
static void
sfw_service_set_ps(sfw_service_t *self, bool enable)
{
    // using NULL self pointer explicitly allowed

    mce_log(LL_DEBUG, "%s", enable ? "enable" : "disable");
    if( self && self->srv_ps ) {
        if( self->srv_ps->plg_override )
            sfw_override_set_target(self->srv_ps->plg_override, enable);
        if( self->srv_ps->plg_reporting )
            sfw_reporting_set_target(self->srv_ps->plg_reporting, enable);
    }
}

/** Perform actions needed when ambient light sensor needed state changes
 */
static void
sfw_service_set_als(sfw_service_t *self, bool enable)
{
    // using NULL self pointer explicitly allowed

    mce_log(LL_DEBUG, "%s", enable ? "enable" : "disable");
    if( self && self->srv_als ) {
        if( self->srv_als->plg_override )
            sfw_override_set_target(self->srv_als->plg_override, enable);
        if( self->srv_als->plg_reporting )
            sfw_reporting_set_target(self->srv_als->plg_reporting, enable);
    }
}

/** Perform actions needed when orientation sensor needed state changes
 */
static void
sfw_service_set_orient(sfw_service_t *self, bool enable)
{
    // using NULL self pointer explicitly allowed

    mce_log(LL_DEBUG, "%s", enable ? "enable" : "disable");
    if( self && self->srv_orient ) {
        if( self->srv_orient->plg_override )
            sfw_override_set_target(self->srv_orient->plg_override, enable);
        if( self->srv_orient->plg_reporting )
            sfw_reporting_set_target(self->srv_orient->plg_reporting, enable);
    }
}

/* ========================================================================= *
 * SENSORFW_NOTIFY
 * ========================================================================= */

/** io watch id for PS evdev file descriptor
 *
 * If this is non-zero, proximity data received from sensord is ignored.
 */
static guint ps_evdev_id = 0;

/** io watch id for ALS evdev file descriptor
 *
 * If this is non-zero, ambient light data received from sensord is ignored.
 */
static guint als_evdev_id = 0;

/** Proximity change callback used for notifying upper level logic */
static void (*sfw_notify_ps_cb)(bool covered) = 0;

/** Ambient light change callback used for notifying upper level logic */
static void (*sfw_notify_als_cb)(int lux) = 0;

/** Orientation change callback used for notifying upper level logic */
static void (*sfw_notify_orient_cb)(int state) = 0;

/** Translate notification type to human readable form
 */
static const char *
sfw_notify_name(sfw_notify_t type)
{
    static const char *const lut[NOTIFY_NUMTYPES] =
    {
        [NOTIFY_RESET]   = "RESET",
        [NOTIFY_RESTORE] = "RESTORE",
        [NOTIFY_REPEAT]  = "REPEAT",
        [NOTIFY_EVDEV]   = "EVDEV",
        [NOTIFY_SENSORD] = "SENSORD",
    };

    return (type < NOTIFY_NUMTYPES) ? lut[type] : 0;
}

/** Notify proximity state via callback
 */
static void
sfw_notify_ps(sfw_notify_t type, bool input_value)
{
    static bool cached_value    = SWF_NOTIFY_DEFAULT_PS;
    const  bool default_value   = SWF_NOTIFY_DEFAULT_PS;
    static bool tracking_active = false;

    /* Always update cached state data */
    switch( type ) {
    default:
    case NOTIFY_REPEAT:
        break;

    case NOTIFY_RESET:
        tracking_active = false;
        break;

    case NOTIFY_RESTORE:
        tracking_active = true;
        break;

    case NOTIFY_EVDEV:
        cached_value = input_value;
        break;

    case NOTIFY_SENSORD:
        if( ps_evdev_id )
            mce_log(LL_DEBUG, "ignoring PS=%d from sensord", input_value);
        else
            cached_value = input_value;
        break;
    }

    /* The rest can be skipped if callback function is unset */
    if( !sfw_notify_ps_cb )
        goto EXIT;

    /* Default value is used unless we are in fully working state */
    bool output_value = tracking_active ? cached_value : default_value ;

    /* Use separate value while expecting sensord startup */
    if( sfw_exception_is_active() && !output_value ) {
        mce_log(LL_DEBUG, "waiting for sensord; using proximity=covered");
        output_value = SWF_NOTIFY_EXCEPTION_PS;
    }

    mce_log(LL_DEBUG, "%s: input %s -> notify %s",
            sfw_notify_name(type),
            input_value ? "covered" : "uncovered",
            output_value ? "covered" : "uncovered");

    sfw_notify_ps_cb(output_value);

EXIT:
    return;
}

/** Notify ambient light level via callback
 */
static void
sfw_notify_als(sfw_notify_t type, unsigned input_value)
{
    static unsigned cached_value    = SWF_NOTIFY_DEFAULT_ALS;
    const  unsigned default_value   = SWF_NOTIFY_DEFAULT_ALS;
    static bool     tracking_active = false;

    /* Always update cached state data */
    switch( type ) {
    default:
    case NOTIFY_REPEAT:
        break;

    case NOTIFY_RESET:
        tracking_active = false;
        break;

    case NOTIFY_RESTORE:
        tracking_active = true;
        break;

    case NOTIFY_EVDEV:
        cached_value = input_value;
        break;

    case NOTIFY_SENSORD:
        if( als_evdev_id )
            mce_log(LL_DEBUG, "ignoring ALS=%u from sensord", input_value);
        else
            cached_value = input_value;
        break;
    }

    /* The rest can be skipped if callback function is unset */
    if( !sfw_notify_als_cb )
        goto EXIT;

    /* Default value is used unless we are in fully working state */
    unsigned output_value = tracking_active ? cached_value : default_value ;

    mce_log(LL_DEBUG, "%s: input %u -> notify %u",
            sfw_notify_name(type),
            input_value, output_value);

    sfw_notify_als_cb(output_value);

EXIT:
    return;
}

/** Notify orientation state via callback
 */
static void
sfw_notify_orient(sfw_notify_t type, int input_value)
{
    static unsigned cached_value    = SWF_NOTIFY_DEFAULT_ORIENT;
    const  unsigned default_value   = SWF_NOTIFY_DEFAULT_ORIENT;
    static bool     tracking_active = false;

    /* Always update cached state data */
    switch( type ) {
    default:
    case NOTIFY_REPEAT:
        break;

    case NOTIFY_RESET:
        tracking_active = false;
        cached_value = default_value;
        break;

    case NOTIFY_RESTORE:
        tracking_active = true;
        break;

    case NOTIFY_EVDEV:
    case NOTIFY_SENSORD:
        cached_value = input_value;
        break;
    }

    /* The rest can be skipped if callback function is unset */
    if( !sfw_notify_orient_cb )
        goto EXIT;

    /* Default value is used unless we are in fully working state */
    unsigned output_value = tracking_active ? cached_value : default_value ;

    mce_log(LL_DEBUG, "%s: input %s -> notify %s",
            sfw_notify_name(type),
            orientation_state_repr(input_value),
            orientation_state_repr(output_value));

    sfw_notify_orient_cb(output_value);

EXIT:
    return;
}

/* ========================================================================= *
 * SENSORFW_EXCEPTION
 * ========================================================================= */

/** Flag for: mce is starting up, use exceptional reporting */
static bool sfw_exception_inititial = true;

/** Timer id for: end of exceptional reporting state */
static guint sfw_exception_timer_id = 0;

/** Timer callback for ending exception state
 *
 * Switch back to reporting actual or built-in default sensor values
 *
 * @return TRUE (to stop timer from repeating)
 */
static gboolean sfw_exception_timer_cb(gpointer aptr)
{
    (void)aptr;

    if( !sfw_exception_timer_id )
        goto EXIT;

    sfw_exception_timer_id = 0;

    mce_log(LL_DEBUG, "exceptional reporting ended");

    sfw_notify_ps(NOTIFY_REPEAT, SWF_NOTIFY_DUMMY);

EXIT:
    return FALSE;
}

/** Predicate for: exceptional sensor values should be reported
 *
 * Currently this should affect only proximity sensor which
 * 1) needs to be assumed "covered" while we do not know whether
 *    sensord is going to be available or not (device bootup and
 *    mce/sensord restarts)
 * 2) must be considered "uncovered" if it becomes clear that
 *    sensord will not be available or fails to function (act dead
 *    mode, unfinished porting to new hw platform, etc)
 *
 * @return true if exceptional values should be used, false otherwise
 */
static bool sfw_exception_is_active(void)
{
    return sfw_exception_timer_id || sfw_exception_inititial;
}

/** Start exceptional value reporting period
 *
 * @delay_ms length of exceptional reporting period
 */
static void sfw_exception_start(sfw_exception_delay_t delay_ms)
{
    mce_log(LL_DEBUG, "exceptional reporting for %d ms", delay_ms);

    if( sfw_exception_timer_id )
        g_source_remove(sfw_exception_timer_id);

    sfw_exception_timer_id = g_timeout_add(delay_ms,
                                 sfw_exception_timer_cb,
                                 0);
    sfw_exception_inititial = false;

    sfw_notify_ps(NOTIFY_REPEAT, SWF_NOTIFY_DUMMY);
}

/** Cancel exceptional value reporting period
 */
static void sfw_exception_cancel(void)
{
    if( sfw_exception_timer_id ) {
        g_source_remove(sfw_exception_timer_id),
            sfw_exception_timer_id = 0;
        mce_log(LL_DEBUG, "exceptional reporting canceled");
    }
}

/* ========================================================================= *
 * SENSORFW_MODULE
 * ========================================================================= */

/** Sensord availability tracking state machine object */
static sfw_service_t *sfw_service = 0;

// ----------------------------------------------------------------

/** Prepare sensors for suspending
 */
void
mce_sensorfw_suspend(void)
{
    // DUMMY
}

/** Rethink sensors after resuming
 */
void
mce_sensorfw_resume(void)
{
    // DUMMY
}

// ----------------------------------------------------------------

/** Set ALS notification callback
 *
 * @param cb function to call when ALS events are received
 */
void
mce_sensorfw_als_set_notify(void (*cb)(int lux))
{
    if( (sfw_notify_als_cb = cb) )
        sfw_notify_als(NOTIFY_REPEAT, 0);
}

/** Try to enable ALS input
 */
void
mce_sensorfw_als_enable(void)
{
    sfw_service_set_als(sfw_service, true);
}

/** Try to disable ALS input
 */
void
mce_sensorfw_als_disable(void)
{
    sfw_service_set_als(sfw_service, false);
}

// ----------------------------------------------------------------

/** Set PS notification callback
 *
 * @param cb function to call when PS events are received
 */
void
mce_sensorfw_ps_set_notify(void (*cb)(bool covered))
{
    if( (sfw_notify_ps_cb = cb) )
        sfw_notify_ps(NOTIFY_REPEAT, 0);
}

/** Try to enable PS input
 */
void
mce_sensorfw_ps_enable(void)
{
    sfw_service_set_ps(sfw_service, true);
}

/** Try to disable PS input
 */
void
mce_sensorfw_ps_disable(void)
{
    sfw_service_set_ps(sfw_service, false);
}

// ----------------------------------------------------------------

/** Set Orientation notification callback
 *
 * @param cb function to call when Orientation events are received
 */
void
mce_sensorfw_orient_set_notify(void (*cb)(int state))
{
    if( (sfw_notify_orient_cb = cb) )
        sfw_notify_orient(NOTIFY_REPEAT, 0);
}

/** Try to enable Orientation input
 */
void
mce_sensorfw_orient_enable(void)
{
    sfw_service_set_orient(sfw_service, true);
}

/** Try to disable Orientation input
 */
void
mce_sensorfw_orient_disable(void)
{
    sfw_service_set_orient(sfw_service, false);
}

// ----------------------------------------------------------------

/** Callback function for processing evdev events
 *
 * @param chn  io channel
 * @param cnd  conditions to handle
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

    /* wakelock must be taken before reading the data */
    wakelock_lock("mce_input_handler", -1);

    if( cnd & (G_IO_ERR | G_IO_HUP | G_IO_NVAL) ) {
        goto EXIT;
    }

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
        sfw_notify_als(NOTIFY_EVDEV, als);

    if( ps != -1 )
        sfw_notify_ps(NOTIFY_EVDEV, ps < 1);

EXIT:
    if( !keep && *id ) {
        *id = 0;
        mce_log(LL_CRIT, "stopping io watch");
    }

    /* wakelock must be released when we are done with the data */
    wakelock_unlock("mce_input_handler");

    return keep;
}

/** Stop I/O watch for als evdev device node
 */
static void
mce_sensorfw_als_detach(void)
{
    if( als_evdev_id ) {
        g_source_remove(als_evdev_id),
            als_evdev_id = 0;
    }
}

/** Use evdev file descriptor as ALS data source
 *
 * Called from evdev probing if ALS device node is detected.
 *
 * Caller expects the file descriptor to be owned by
 * sensor module after the call and it must thus be closed
 * if input monitoring is not succesfully initiated.
 *
 * @param fd file descriptor
 */
void
mce_sensorfw_als_attach(int fd)
{
    if( fd == -1 )
        goto EXIT;

    mce_sensorfw_als_detach();

    struct input_absinfo info;
    memset(&info, 0, sizeof info);

    if( ioctl(fd, EVIOCGABS(ABS_MISC), &info) == -1 ) {
        mce_log(LL_ERR, "EVIOCGABS(%s): %m", "ABS_MISC");
        goto EXIT;
    }

    /* Note: als_evdev_id must be set before calling als_notify() */
    als_evdev_id = sfw_socket_add_notify(fd, true, G_IO_IN,
                                         mce_sensorfw_evdev_cb,
                                         &als_evdev_id);

    if( !als_evdev_id )
        goto EXIT;

    /* The I/O watch owns the file descriptor now */
    fd = -1;

    mce_log(LL_INFO, "ALS: %d (initial)", info.value);
    sfw_notify_als(NOTIFY_EVDEV, info.value);

EXIT:
    if( fd != -1 )
        close(fd);

    return;
}

/** Stop I/O watch for ps evdev device node
 */
static void
mce_sensorfw_ps_detach(void)
{
    if( ps_evdev_id ) {
        g_source_remove(ps_evdev_id),
            ps_evdev_id = 0;
    }
}

/** Use evdev file descriptor as PS data source
 *
 * Called from evdev probing if PS device node is detected.
 *
 * Caller expects the file descriptor to be owned by
 * sensor module after the call and it must thus be closed
 * if input monitoring is not succesfully initiated.
 *
 * @param fd file descriptor
 */
void
mce_sensorfw_ps_attach(int fd)
{
    if( fd == -1 )
        goto EXIT;

    mce_sensorfw_ps_detach();

    struct input_absinfo info;
    memset(&info, 0, sizeof info);

    if( ioctl(fd, EVIOCGABS(ABS_DISTANCE), &info) == -1 ) {
        mce_log(LL_ERR, "EVIOCGABS(%s): %m", "ABS_DISTANCE");
        goto EXIT;
    }

    /* Note: ps_evdev_id must be set before calling ps_notify() */
    ps_evdev_id = sfw_socket_add_notify(fd, true, G_IO_IN,
                                        mce_sensorfw_evdev_cb,
                                        &ps_evdev_id);
    if( !ps_evdev_id )
        goto EXIT;

    /* The I/O watch owns the file descriptor now */
    fd = -1;

    mce_log(LL_NOTICE, "PS: %d (initial)", info.value);
    sfw_notify_ps(NOTIFY_EVDEV, info.value < 1);

EXIT:
    if( fd != -1 )
        close(fd);
}

// ----------------------------------------------------------------

/** Handle name owner changed signals for SENSORFW_SERVICE
 */
static gboolean
sfw_name_owner_changed_cb(DBusMessage *const msg)
{
    DBusError   err  = DBUS_ERROR_INIT;
    const char *name = 0;
    const char *prev = 0;
    const char *curr = 0;

    if( !msg )
        goto EXIT;

    if( !dbus_message_get_args(msg, &err,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_STRING, &prev,
                               DBUS_TYPE_STRING, &curr,
                               DBUS_TYPE_INVALID) ) {
        mce_log(LL_ERR, "Failed to parse name owner signal: %s: %s",
                err.name, err.message);
        goto EXIT;
    }

    if( !name || strcmp(name, SENSORFW_SERVICE) )
        goto EXIT;

    if( !sfw_service )
        goto EXIT;

    if( curr && *curr )
        sfw_service_do_start(sfw_service);
    else
        sfw_service_do_stop(sfw_service);

EXIT:
    dbus_error_free(&err);

    return TRUE;
}

/** Array of dbus message handlers */
static mce_dbus_handler_t sfw_dbus_handlers[] =
{
    /* signals */
    {
        .interface = DBUS_INTERFACE_DBUS,
        .name      = DBUS_SIGNAL_NAME_OWNER_CHANGED,
        .rules     = "arg0='"SENSORFW_SERVICE"'",
        .type      = DBUS_MESSAGE_TYPE_SIGNAL,
        .callback  = sfw_name_owner_changed_cb,
    },

    /* sentinel */
    {
        .interface = 0
    }
};

// ----------------------------------------------------------------

/** Initialize mce sensorfw module
 */
bool
mce_sensorfw_init(void)
{
    sfw_exception_start(SFW_EXCEPTION_LENGTH_MCE_STARTING_UP);

    /* Register D-Bus handlers */
    mce_dbus_handler_register_array(sfw_dbus_handlers);

    /* Start tracking sensord availablity */
    sfw_service = sfw_service_create();
    sfw_service_do_query(sfw_service);

    return true;
}

/** Cleanup mce sensorfw module
 */
void
mce_sensorfw_quit(void)
{
    /* Remove evdev I/O watches */
    mce_sensorfw_ps_detach();
    mce_sensorfw_als_detach();

    /* Remove D-Bus handlers */
    mce_dbus_handler_unregister_array(sfw_dbus_handlers);

    /* Stop tracking sensord availablity */
    sfw_service_delete(sfw_service), sfw_service = 0;

    sfw_exception_cancel();
}
