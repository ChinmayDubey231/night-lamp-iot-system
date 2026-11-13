// Host-side tests for firmware/sketch.ino. Build with run_tests.sh, which
// compiles a copy of the sketch so these tests always use test/secrets.h.
#include "Arduino.h"
#include "WiFi.h"
#include "PubSubClient.h"
#include "Preferences.h"
#include "esp_task_wdt.h"
#include <algorithm>
#include <map>

unsigned long fake_millis = 0;
int fake_analog = 680;          // bright room (12-bit)
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

static int failures = 0;
#define CHECK(cond, ...)                                       \
  do {                                                         \
    std::printf((cond) ? "  ok   " : "  FAIL ");               \
    std::printf(__VA_ARGS__);                                  \
    std::printf("\n");                                         \
    if (!(cond)) failures++;                                   \
  } while (0)

static long loops = 0;
static void run_for(unsigned long ms) {
  unsigned long end = fake_millis + ms;
  while (fake_millis < end) { loop(); loops++; fake_millis += 10; }
}

static std::vector<PublishedMsg> on(const char* topic) {
  std::vector<PublishedMsg> v;
  for (auto& m : mqtt.published) if (m.topic == topic) v.push_back(m);
  return v;
}
static long num(const std::string& json, const char* key) {
  std::string k = std::string("\"") + key + "\":";
  size_t p = json.find(k);
  return p == std::string::npos ? -1 : std::atol(json.c_str() + p + k.size());
}
static std::string str(const std::string& json, const char* key) {
  std::string k = std::string("\"") + key + "\":\"";
  size_t p = json.find(k);
  if (p == std::string::npos) return "";
  p += k.size();
  return json.substr(p, json.find('"', p) - p);
}
static bool contiguous(size_t from = 0) {
  auto r = on(topicReading);
  for (size_t i = from + 1; i < r.size(); i++)
    if (num(r[i].payload, "seq") != num(r[i - 1].payload, "seq") + 1) return false;
  return true;
}

int main() {
  std::printf("Night Lamp IoT firmware (firmware/sketch.ino)\n");
  setup();

  // ---- Power ------------------------------------------------------------
  CHECK(fake_cpu_mhz == 80, "power: CPU clocked down to %u MHz", fake_cpu_mhz);
  CHECK(WiFi.sleepOn, "power: WiFi modem sleep enabled");

  // ---- Reliability: watchdog, reset reason, boot count -------------------
  CHECK(fake_wdt_timeout_ms == 30000 && fake_wdt_panic, "watchdog: 30 s timeout, reboots on hang");
  CHECK(fake_wdt_tasks == 1, "watchdog: loop() task is registered");
  CHECK(!std::strcmp(resetReason, "poweron") && bootCount == 1,
        "reset reason \"%s\" and boot #%u recorded", resetReason, bootCount);
  CHECK(fake_nvs["sys"]["boots"] == "1", "boot count persisted in NVS");

  // ---- Secrets: development fallback -----------------------------------
  CHECK(!std::strcmp(credSource, "secrets.h") && !std::strcmp(wifiSsid, "TestNet"),
        "no NVS credentials yet: falls back to secrets.h (ssid=%s)", wifiSsid);

  // ---- Start-up and transport -------------------------------------------
  run_for(20000);
  CHECK(WiFi.lastSsid == "TestNet" && WiFi.lastPass == "test-password", "joins WiFi with the configured credentials");
  CHECK(!WiFi.autoReconnect, "built-in auto-reconnect off (firmware owns reconnects)");
  CHECK(mqtt.connected() && mqtt.clientId == deviceId, "MQTT connected as %s", deviceId);
  CHECK(std::string(topicReading) == std::string(TOPIC_BASE) + "/" + deviceId + "/reading",
        "per-device topics: %s", topicReading);
  CHECK(mqtt.willTopic == topicStatus && mqtt.willMessage == "offline" && mqtt.willRetain,
        "last will: retained \"offline\" on the status topic");
  auto st = on(topicStatus);
  CHECK(!st.empty() && st.back().payload == "online" && st.back().retained, "retained \"online\" after connecting");

  auto h = on(topicHealth);
  CHECK(!h.empty() && h.front().retained, "health published (retained) right after connecting");
  if (!h.empty()) {
    auto& p = h.back().payload;
    CHECK(str(p, "fw") == FW_VERSION && num(p, "rssi") == -61 && num(p, "heap") > 0 &&
          num(p, "cpu") == 80 && str(p, "reset") == "poweron" && num(p, "boots") == 1 &&
          str(p, "creds") == "secrets.h",
          "health has fw, rssi, heap, cpu, reset reason, boots, cred source");
    CHECK(p.find("password") == std::string::npos && p.find("test-password") == std::string::npos,
          "health never contains the WiFi password");
  }
  CHECK(fake_config_time_calls >= 1, "NTP started once WiFi is up (health timestamps)");

  // ---- Report-by-exception --------------------------------------------------
  auto r0 = on(topicReading);
  CHECK(r0.size() == 1 && str(r0[0].payload, "why") == "boot", "first reading is the boot reading");
  size_t before = on(topicReading).size();
  run_for(5UL * 60 * 1000);                       // 5 min of steady light
  size_t steady = on(topicReading).size() - before;
  CHECK(steady == 5, "steady light for 5 min: %zu heartbeat readings (one per 60 s, not 150)", steady);
  CHECK(str(on(topicReading).back().payload, "why") == "heartbeat", "they are marked \"heartbeat\"");

  before = on(topicReading).size();
  fake_analog = 680 + 60;                          // small flicker, inside the deadband
  run_for(10000);
  CHECK(on(topicReading).size() == before, "change smaller than the deadband: not sent");

  run_for(730);                                    // change lands between 2 s checks
  fake_analog = 2400;                              // lights dimmed
  unsigned long changedAt = fake_millis;
  run_for(5000);
  auto rc = on(topicReading);
  CHECK(rc.size() > before && str(rc[before].payload, "why") == "change",
        "real change: reading marked \"change\"");
  CHECK(rc.size() > before && rc[before].at - changedAt <= 2100,
        "...sent within %lu ms of the change", rc.size() > before ? rc[before].at - changedAt : 0);
  CHECK(lampOn && fake_pwm > 0, "lamp turned on in the dark (pwm=%d)", fake_pwm);

  // ---- Health cadence -------------------------------------------------------
  size_t h0 = on(topicHealth).size();
  run_for(180000);
  size_t hn = on(topicHealth).size() - h0;
  CHECK(hn == 3, "health every 60 s (%zu in 3 min)", hn);

  // ---- Watchdog is fed every pass --------------------------------------------
  CHECK(fake_wdt_feeds == loops, "watchdog fed on every loop() pass (%ld/%ld)", fake_wdt_feeds, loops);

  // ---- Outage: buffer, reconnect, deliver in order ----------------------------
  size_t pubBefore = on(topicReading).size();
  WiFi.apUp = false;
  for (int i = 0; i < 18; i++) {                   // 3 min offline, light changing
    fake_analog = (i % 2) ? 1500 : 3000;
    run_for(10000);
  }
  CHECK(on(topicReading).size() == pubBefore && bufCount >= 18,
        "offline 3 min: nothing sent, %u readings buffered", bufCount);
  CHECK(fake_pwm > 0, "offline: lamp still follows the light (pwm=%d)", fake_pwm);
  WiFi.apUp = true;
  mqtt.failPublishAfter = 4;                       // and the link dies mid-backlog
  run_for(120000);
  CHECK(bufCount == 0 && contiguous(), "back online: backlog delivered once, in order, even after a mid-flush drop");
  long maxAge = 0;
  for (auto& m : on(topicReading)) maxAge = std::max(maxAge, num(m.payload, "age"));
  CHECK(maxAge >= 170000, "buffered readings carry their age (oldest waited %ld s)", maxAge / 1000);
  CHECK(num(on(topicHealth).back().payload, "wifi_reconnects") >= 2, "health counts reconnects");
  CHECK(mqttReconnects >= 3, "MQTT session re-established after the WiFi outage (%u connects)", mqttReconnects);

  // ---- Secrets: provisioning over Serial into NVS ------------------------------
  Serial.out.clear();
  Serial.type("ssid HomeNet 5G\npass s3cret-pass\nmqttuser lamp01\nmqttpass mq-secret\n");
  run_for(100);
  CHECK(fake_nvs["net"]["ssid"] == "HomeNet 5G" && fake_nvs["net"]["pass"] == "s3cret-pass" &&
        fake_nvs["net"]["mqttuser"] == "lamp01", "serial `ssid/pass/mqttuser/mqttpass` stored in NVS (spaces OK)");
  loadCredentials();                               // what the next boot does
  CHECK(!std::strcmp(credSource, "nvs") && !std::strcmp(wifiSsid, "HomeNet 5G") && !std::strcmp(mqttUser, "lamp01"),
        "after reboot: NVS credentials win over secrets.h");
  Serial.out.clear();
  Serial.type("show\n");
  run_for(100);
  CHECK(Serial.out.find("HomeNet 5G") != std::string::npos &&
        Serial.out.find("s3cret-pass") == std::string::npos && Serial.out.find("mq-secret") == std::string::npos,
        "`show` prints the SSID but masks passwords");
  mqtt.session = false;                            // force a reconnect with the new MQTT user
  WiFi.disconnect();
  run_for(30000);
  CHECK(WiFi.lastSsid == "HomeNet 5G" && mqtt.user == "lamp01", "reconnects with provisioned WiFi and MQTT credentials");
  CHECK(str(on(topicHealth).back().payload, "creds") == "nvs", "health reports creds=nvs (never the values)");
  Serial.type("forget\nreboot\n");
  run_for(100);
  loadCredentials();
  CHECK(fake_nvs["net"].empty() && !std::strcmp(credSource, "secrets.h"), "`forget` erases them, back to secrets.h");
  CHECK(ESP.restarts == 1, "`reboot` restarts the board");

  // ---- Reset reason after a watchdog reboot ------------------------------------
  fake_reset_reason = ESP_RST_TASK_WDT;
  CHECK(!std::strcmp(describeReset(esp_reset_reason()), "task_watchdog"), "a watchdog reboot is reported as \"task_watchdog\"");
  fake_reset_reason = ESP_RST_BROWNOUT;
  CHECK(!std::strcmp(describeReset(esp_reset_reason()), "brownout"), "a power dip is reported as \"brownout\"");

  // ---- Device id in the simulator (no factory MAC) ------------------------------
  WiFi.noFactoryMac = true;
  makeDeviceId();
  CHECK(std::strcmp(deviceId, "lamp-000000") != 0, "no factory MAC: random device id (%s)", deviceId);

  std::printf(failures ? "  %d test(s) FAILED\n" : "  all passed\n", failures);
  return failures ? 1 : 0;
}
