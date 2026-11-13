// Copy this file to secrets.h and fill it in. secrets.h is gitignored, so
// real credentials never reach the repository.
//
// These values are only the development fallback. Credentials stored on the
// device with the serial `ssid` / `pass` / `mqttuser` / `mqttpass` commands
// take priority, so a production build can ship with this file left empty.
#pragma once

// Wokwi's simulated access point (open network, no password)
#define WIFI_SSID     "Wokwi-GUEST"
#define WIFI_PASSWORD ""

// Public demo broker: no credentials. For a private broker, set a
// per-device username and password here (or provision them over Serial).
#define MQTT_HOST     "broker.hivemq.com"
#define MQTT_PORT     1883
#define MQTT_USER     ""
#define MQTT_PASSWORD ""
