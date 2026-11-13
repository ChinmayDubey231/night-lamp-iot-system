// Fake PubSubClient: tests flip `brokerUp` or make publish() fail mid-backlog.
#pragma once
#include "WiFi.h"
#include <string>
#include <vector>

struct PublishedMsg { std::string topic, payload; bool retained; unsigned long at; };

// Optional hook for the end-to-end bridge (e2e_bridge.cpp): mirrors what
// the firmware does onto a real MQTT broker. Null in unit tests.
inline void (*fake_mqtt_hook)(const char* event, const std::string& a, const std::string& b, bool retained) = nullptr;

class PubSubClient {
 public:
  explicit PubSubClient(WiFiClient&) {}
  bool brokerUp = true;
  int failPublishAfter = -1;
  bool session = false;
  unsigned sessionEpoch = 0;   // WiFi link the TCP socket belongs to
  std::vector<unsigned long> connectTimes;
  std::string clientId, user, willTopic, willMessage;
  bool willRetain = false;
  uint16_t bufferSize = 256;
  std::vector<PublishedMsg> published;

  void setServer(const char*, uint16_t) {}
  void setKeepAlive(uint16_t) {}
  void setSocketTimeout(uint16_t) {}
  bool setBufferSize(uint16_t n) { bufferSize = n; return true; }

  bool connect(const char* id, const char* u, const char*,
               const char* wTopic, uint8_t, bool wRetain, const char* wMsg) {
    connectTimes.push_back(fake_millis);
    clientId = id; user = u ? u : "";
    willTopic = wTopic; willMessage = wMsg; willRetain = wRetain;
    session = (WiFi.status() == WL_CONNECTED) && brokerUp;
    sessionEpoch = WiFi.linkEpoch;
    if (session && fake_mqtt_hook) fake_mqtt_hook("connect", clientId, willTopic + "\t" + willMessage, willRetain);
    return session;
  }
  bool connected() {
    if (session && (WiFi.status() != WL_CONNECTED || !brokerUp || WiFi.linkEpoch != sessionEpoch)) {
      session = false;
      if (fake_mqtt_hook) fake_mqtt_hook("drop", "", "", false);
    }
    return session;
  }
  bool publish(const char* topic, const char* payload, bool retained = false) {
    if (!connected()) return false;
    if (std::strlen(topic) + std::strlen(payload) + 7 > bufferSize) return false;  // like the real lib
    if (failPublishAfter == 0) {
      session = false; failPublishAfter = -1;
      if (fake_mqtt_hook) fake_mqtt_hook("drop", "", "", false);
      return false;
    }
    if (failPublishAfter > 0) failPublishAfter--;
    published.push_back({topic, payload, retained, fake_millis});
    if (fake_mqtt_hook) fake_mqtt_hook("pub", topic, payload, retained);
    return true;
  }
  bool loop() { return connected(); }
  int state() { return session ? 0 : -2; }
};
