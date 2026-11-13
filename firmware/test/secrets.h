// Test-only credentials (fake values) so the tests build without a real
// firmware/secrets.h. Never used on a device.
#pragma once
#define WIFI_SSID     "TestNet"
#define WIFI_PASSWORD "test-password"
#define MQTT_HOST     "broker.test"
#define MQTT_PORT     1883
#define MQTT_USER     ""
#define MQTT_PASSWORD ""
