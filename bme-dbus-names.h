/**
 * @file bme-dbus-names.h
 */
#ifndef _BME_DBUS_NAMES_H_
#define _BME_DBUS_NAMES_H_

#define DBUS_BROADCAST			"org.freedesktop.DBus.Broadcast"

#define BME_SERVICE			"com.nokia.bme"

#define BME_REQUEST_IF			"com.nokia.bme.request"
#define BME_REQUEST_PATH		"/com/nokia/bme/request"

#define BME_SIGNAL_IF			"com.nokia.bme.signal"
#define BME_SIGNAL_PATH			"/com/nokia/bme/signal"

#define BME_ERROR_FATAL			"com.nokia.bme.error.fatal"
#define BME_ERROR_INVALID_ARGS		"org.freedesktop.DBus.Error.InvalidArgs"

#define BME_BATTERY_STATE_UPDATE	"battery_state_changed"
#define BME_BATTERY_FULL		"battery_full"
#define BME_BATTERY_OK			"battery_ok"
#define BME_BATTERY_LOW			"battery_low"
#define BME_BATTERY_EMPTY		"battery_empty"
#define BME_BATTERY_TIMELEFT		"battery_timeleft"

#define BME_CHARGER_CONNECTED		"charger_connected"
#define BME_CHARGER_DISCONNECTED	"charger_disconnected"
#define BME_CHARGER_CHARGING_ON		"charger_charging_on"
#define BME_CHARGER_CHARGING_OFF	"charger_charging_off"
#define BME_CHARGER_CHARGING_FAILED	"charger_charging_failed"

#define BME_STATUS_INFO_REQ		"status_info_req"
#define BME_TIMELEFT_INFO_REQ		"timeleft_info_req"

#endif /* _BME_DBUS_NAMES_H_ */
