// End-to-end bridge: runs the REAL firmware logic (sketch.ino) in real
// time against the fake hardware, and prints every MQTT action it takes.
// tools/e2e/run.js replays those actions on a real MQTT broker, so the
// dashboard can be checked against genuine firmware traffic.
//
// Script: dusk falls at 12 s, the access point disappears from 35 s to
// 75 s while the light keeps changing, then everything recovers.
#include "Arduino.h"
#include "WiFi.h"
#include "PubSubClient.h"
#include "Preferences.h"
#include "esp_task_wdt.h"
#include <chrono>
#include <map>
#include <thread>

unsigned long fake_millis = 0;
int fake_analog = 700;
int fake_button = HIGH;
int fake_pwm = -1;
uint32_t fake_cpu_mhz = 240;
int fake_config_time_calls = 0;
esp_reset_reason_t fake_reset_reason = ESP_RST_POWERON;
uint32_t fake_wdt_timeout_ms = 0;
bool fake_wdt_panic = false;
int fake_wdt_tasks = 0;
long fake_wdt_feeds = 0;
std::map<std::string, std::map<std::string, std::string>> fake_nvs;
FakeESP ESP;
FakeSerial Serial;
FakeWiFi WiFi;

#include SKETCH_PATH

static void hook(const char* ev, const std::string& a, const std::string& b, bool retained) {
  if (!std::strcmp(ev, "pub"))          std::printf("PUB\t%d\t%s\t%s\n", retained ? 1 : 0, a.c_str(), b.c_str());
  else if (!std::strcmp(ev, "connect")) std::printf("CONNECT\t%s\t%s\n", a.c_str(), b.c_str());
  else                                   std::printf("DROP\n");
  std::fflush(stdout);
}

int main(int argc, char** argv) {
  unsigned long runMs = argc > 1 ? std::atol(argv[1]) * 1000UL : 120000UL;
  fake_mqtt_hook = hook;
  auto t0 = std::chrono::steady_clock::now();
  setup();
  while (fake_millis < runMs) {
    fake_millis = (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    unsigned long s = fake_millis / 1000;
    fake_analog = s < 12 ? 700 : s < 45 ? 2600 : s < 55 ? 1800 : s < 65 ? 3200 : 2400;
    bool up = !(s >= 35 && s < 75);
    if (up != WiFi.apUp) {
      WiFi.apUp = up;
      if (!up) {                // the link, and the TCP socket on it, die right now
        WiFi.status();
        if (mqtt.session) { mqtt.session = false; hook("drop", "", "", false); }
      }
      std::fprintf(stderr, "[script] t=%lus access point %s\n", s, up ? "UP" : "DOWN");
    }
    loop();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::fprintf(stderr, "%s", Serial.out.c_str());
  return 0;
}
