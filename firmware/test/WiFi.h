// Fake ESP32 WiFi. Tests flip `apUp` to make the access point vanish.
#pragma once
#include "Arduino.h"
#include <vector>

#define WL_IDLE_STATUS 0
#define WL_CONNECTED 3
#define WIFI_STA 1

struct FakeWiFi {
  bool apUp = true;
  unsigned long joinTimeMs = 3000;
  int st = WL_IDLE_STATUS;
  bool joining = false;
  unsigned long joinStartedAt = 0;
  std::vector<unsigned long> beginTimes;
  std::string lastSsid, lastPass;
  bool radioOn = false, noFactoryMac = false, sleepOn = false, autoReconnect = true;
  // Bumped every time the link goes down. A TCP socket opened on an older
  // link is dead, exactly like on real hardware.
  unsigned linkEpoch = 0;

  int status() {
    if (!apUp && st == WL_CONNECTED) linkEpoch++;
    if (!apUp) st = WL_IDLE_STATUS;
    else if (joining && fake_millis - joinStartedAt >= joinTimeMs) { st = WL_CONNECTED; joining = false; }
    return st;
  }
  void begin(const char* ssid, const char* pass) {
    beginTimes.push_back(fake_millis);
    lastSsid = ssid; lastPass = pass;
    joining = true; joinStartedAt = fake_millis; st = WL_IDLE_STATUS;
  }
  void disconnect() { if (st == WL_CONNECTED) linkEpoch++; st = WL_IDLE_STATUS; joining = false; }
  void mode(int) { radioOn = true; }
  void setAutoReconnect(bool v) { autoReconnect = v; }
  void setSleep(bool v) { sleepOn = v; }
  int RSSI() { return -61; }
  void macAddress(uint8_t* m) {
    const uint8_t mac[6] = {0x24, 0x0a, 0xc4, 0x12, 0x34, 0x56};
    for (int i = 0; i < 6; i++) m[i] = (radioOn && !noFactoryMac) ? mac[i] : 0;
  }
  const char* localIP() { return "10.10.0.2"; }
};
extern FakeWiFi WiFi;

class WiFiClient {};
