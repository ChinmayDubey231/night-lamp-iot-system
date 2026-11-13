/*
 * Night Lamp IoT System — firmware (Task 04 capstone)
 *
 * ESP32 night lamp -> MQTT -> browser dashboard.
 *
 *  Local     LDR sampled every 100 ms, lamp LED brightness follows darkness.
 *            Works with or without a network.
 *  Reading   Checked every 2 s; queued only if the light changed noticeably
 *            or 60 s passed (report-by-exception saves radio time = power).
 *  Transport WiFi + MQTT with independent reconnect/backoff, offline ring
 *            buffer drained oldest-first, last-will "offline" status.
 *  Health    Retained snapshot every 60 s: uptime, RSSI, heap, reset
 *            reason, boot count, buffer stats, credential source.
 *  Reliable  Task watchdog reboots the board if loop() ever hangs; the
 *            reset reason is reported after the reboot.
 *  Power     CPU at 80 MHz, WiFi modem sleep, fewer transmissions.
 *  Secrets   Nothing secret in this file. Credentials come from NVS
 *            (set over Serial, see `help`) or, for development, from
 *            secrets.h, which is gitignored.
 *
 * Topics:  <base>/<device>/reading   JSON, one per queued reading
 *          <base>/<device>/health    JSON, retained
 *          <base>/<device>/status    "online" / "offline" (retained, LWT)
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <time.h>
#if __has_include(<esp_random.h>)
#include <esp_random.h>   // esp_random() moved here in ESP-IDF 5 (Arduino-ESP32 3.x)
#endif

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "secrets.h not found: copy firmware/secrets.example.h to firmware/secrets.h (see README)"
#endif

#define FW_VERSION "1.0.0"

// ---- Non-secret configuration ----------------------------------------------
const char* TOPIC_BASE = "chinmaydubey231/night-lamp-iot";

const uint8_t SENSOR_PIN = 34;   // ADC1 (ADC2 is unusable while WiFi is on)
const uint8_t LAMP_PIN   = 18;
const uint8_t BUTTON_PIN = 4;    // "drop network" demo button, to GND

const unsigned long SAMPLE_MS      = 100;
const unsigned long EVAL_MS        = 2000;
const unsigned long HEARTBEAT_MS   = 60000;  // a reading at least this often
const unsigned long HEALTH_MS      = 60000;
const int           CHANGE_DEADBAND = 100;   // ~2.5% of the 12-bit range
const unsigned long SIM_OUTAGE_MS  = 20000;
const unsigned long WIFI_CONNECT_WAIT_MS = 10000;
const unsigned long WIFI_BACKOFF_MIN_MS  = 2000;
const unsigned long MQTT_BACKOFF_MIN_MS  = 1000;
const unsigned long BACKOFF_MAX_MS       = 30000;
const uint8_t       MAX_PUBLISH_PER_PASS = 20;
const uint32_t      WDT_TIMEOUT_S = 30;      // > worst-case blocking mqtt.connect()
const uint32_t      CPU_MHZ = 80;            // lowest clock that still runs WiFi

const int DARK_ON = 1200, DARK_OFF = 1000, FULL_DARK = 3400;
const uint8_t AVG_SAMPLES = 8;

// ---- Offline buffer ---------------------------------------------------------
enum Why : uint8_t { WHY_BOOT, WHY_CHANGE, WHY_HEARTBEAT };
const char* WHY_NAMES[] = {"boot", "change", "heartbeat"};

struct Reading {
  uint32_t seq;
  uint32_t takenAt;
  uint16_t level;
  uint8_t  lamp;
  uint8_t  why;
};

const uint16_t BUFFER_SIZE = 300;
Reading  buffer[BUFFER_SIZE];
uint16_t bufHead = 0, bufCount = 0;
uint32_t droppedReadings = 0, nextSeq = 1;

void bufferPush(const Reading& r) {
  if (bufCount == BUFFER_SIZE) {         // full: drop oldest, keep newest
    bufHead = (bufHead + 1) % BUFFER_SIZE;
    bufCount--;
    droppedReadings++;
  }
  buffer[(bufHead + bufCount) % BUFFER_SIZE] = r;
  bufCount++;
}
const Reading& bufferPeek() { return buffer[bufHead]; }
void bufferPop() { bufHead = (bufHead + 1) % BUFFER_SIZE; bufCount--; }

// ---- Credentials (NVS first, secrets.h as development fallback) -------------
Preferences prefs;
char wifiSsid[33], wifiPass[65], mqttUser[33], mqttPass[65];
const char* credSource = "secrets.h";

void loadCredentials() {
  prefs.begin("net", true);
  bool fromNvs = prefs.getString("ssid", wifiSsid, sizeof(wifiSsid)) > 0;
  if (fromNvs) {
    prefs.getString("pass", wifiPass, sizeof(wifiPass));
    prefs.getString("mqttuser", mqttUser, sizeof(mqttUser));
    prefs.getString("mqttpass", mqttPass, sizeof(mqttPass));
    credSource = "nvs";
  } else {
    strlcpy(wifiSsid, WIFI_SSID, sizeof(wifiSsid));
    strlcpy(wifiPass, WIFI_PASSWORD, sizeof(wifiPass));
    strlcpy(mqttUser, MQTT_USER, sizeof(mqttUser));
    strlcpy(mqttPass, MQTT_PASSWORD, sizeof(mqttPass));
    credSource = "secrets.h";
  }
  prefs.end();
}

// ---- State ------------------------------------------------------------------
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
char deviceId[24];
char topicReading[96], topicHealth[96], topicStatus[96];

int samples[AVG_SAMPLES];
uint8_t sampleIndex = 0;
long sampleSum = 0;
int level = 0;
bool lampOn = false;
uint8_t brightness = 0;

unsigned long lastSampleAt = 0, lastEvalAt = 0, lastHealthAt = 0;
unsigned long lastQueuedAt = 0;
int  lastQueuedLevel = -10000;
bool lastQueuedLampOn = false;
bool queuedBoot = false;

bool wifiWasConnected = false, wifiAttemptActive = false;
unsigned long wifiAttemptAt = 0, nextWifiAttemptAt = 0;
uint8_t wifiFailures = 0;
uint32_t wifiReconnects = 0;

unsigned long nextMqttAttemptAt = 0;
uint8_t mqttFailures = 0;
uint32_t mqttReconnects = 0, publishedCount = 0;
bool healthDue = false;

unsigned long simOutageUntil = 0;
bool simOutage = false;
int lastButton = HIGH;

uint32_t bootCount = 0;
const char* resetReason = "unknown";

char lineBuf[100];
uint8_t lineLen = 0;

// ---- Helpers ------------------------------------------------------------------
bool due(unsigned long& last, unsigned long interval, unsigned long now) {
  if (now - last < interval) return false;
  last += interval;
  if (now - last >= interval) last = now;   // skip ahead, no catch-up burst
  return true;
}

unsigned long backoffMs(unsigned long minMs, uint8_t failures) {
  unsigned long d = minMs;
  for (uint8_t i = 1; i < failures && d < BACKOFF_MAX_MS; i++) d *= 2;
  if (d > BACKOFF_MAX_MS) d = BACKOFF_MAX_MS;
  return d + random(0, d / 4 + 1);           // jitter: avoid retry stampedes
}

const char* describeReset(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_watchdog";
    case ESP_RST_TASK_WDT:  return "task_watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_EXT:       return "external";
    default:                return "unknown";
  }
}

void makeDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  uint32_t suffix = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  if (suffix == 0) suffix = (esp_random() & 0xFFFFFF) | 1;   // simulator: no factory MAC
  snprintf(deviceId, sizeof(deviceId), "lamp-%06lx", (unsigned long)suffix);
  snprintf(topicReading, sizeof(topicReading), "%s/%s/reading", TOPIC_BASE, deviceId);
  snprintf(topicHealth,  sizeof(topicHealth),  "%s/%s/health",  TOPIC_BASE, deviceId);
  snprintf(topicStatus,  sizeof(topicStatus),  "%s/%s/status",  TOPIC_BASE, deviceId);
}

void setupWatchdog() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;                  // reboot, then report the reason
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);                    // watch the loop() task
}

// ---- Local: sensor and lamp ---------------------------------------------------
void sampleAndDrive() {
  int raw = analogRead(SENSOR_PIN);
  sampleSum -= samples[sampleIndex];
  samples[sampleIndex] = raw;
  sampleSum += raw;
  sampleIndex = (sampleIndex + 1) % AVG_SAMPLES;
  level = (int)(sampleSum / AVG_SAMPLES);

  if (!lampOn && level >= DARK_ON)  lampOn = true;
  if (lampOn  && level <  DARK_OFF) lampOn = false;
  brightness = lampOn
      ? (uint8_t)map(constrain(level, DARK_OFF, FULL_DARK), DARK_OFF, FULL_DARK, 20, 255)
      : 0;
  analogWrite(LAMP_PIN, brightness);
}

// Report-by-exception: only queue a reading when something changed.
void evaluateReading(unsigned long now) {
  Why why;
  if (!queuedBoot)                                         why = WHY_BOOT;
  else if (abs(level - lastQueuedLevel) >= CHANGE_DEADBAND
           || lampOn != lastQueuedLampOn)                  why = WHY_CHANGE;
  else if (now - lastQueuedAt >= HEARTBEAT_MS)             why = WHY_HEARTBEAT;
  else return;

  Reading r;
  r.seq = nextSeq++;
  r.takenAt = now;
  r.level = (uint16_t)level;
  r.lamp = brightness;
  r.why = why;
  bufferPush(r);

  queuedBoot = true;
  lastQueuedAt = now;
  lastQueuedLevel = level;
  lastQueuedLampOn = lampOn;
}

// ---- Serial provisioning --------------------------------------------------------
void printHelp() {
  Serial.println(F("Commands (stored in NVS, applied after `reboot`):"));
  Serial.println(F("  ssid <name>       WiFi network name"));
  Serial.println(F("  pass <password>   WiFi password"));
  Serial.println(F("  mqttuser <user>   MQTT username (blank = none)"));
  Serial.println(F("  mqttpass <pass>   MQTT password"));
  Serial.println(F("  show              current settings (passwords masked)"));
  Serial.println(F("  forget            erase stored credentials"));
  Serial.println(F("  reboot            restart to apply"));
}

void handleCommand(char* line) {
  char* arg = strchr(line, ' ');
  if (arg) { *arg = '\0'; arg++; } else { arg = (char*)""; }

  const char* key = nullptr;
  if      (!strcmp(line, "ssid"))     key = "ssid";
  else if (!strcmp(line, "pass"))     key = "pass";
  else if (!strcmp(line, "mqttuser")) key = "mqttuser";
  else if (!strcmp(line, "mqttpass")) key = "mqttpass";

  if (key) {
    prefs.begin("net", false);
    prefs.putString(key, arg);
    prefs.end();
    Serial.print(F("[cfg] saved "));
    Serial.print(key);
    Serial.println(F(", type `reboot` to apply"));
  } else if (!strcmp(line, "show")) {
    Serial.print(F("[cfg] source="));  Serial.print(credSource);
    Serial.print(F(" ssid="));         Serial.print(wifiSsid);
    Serial.print(F(" pass="));         Serial.print(wifiPass[0] ? F("****") : F("(none)"));
    Serial.print(F(" mqttuser="));     Serial.print(mqttUser[0] ? mqttUser : "(none)");
    Serial.print(F(" mqttpass="));     Serial.println(mqttPass[0] ? F("****") : F("(none)"));
  } else if (!strcmp(line, "forget")) {
    prefs.begin("net", false);
    prefs.clear();
    prefs.end();
    Serial.println(F("[cfg] credentials erased, secrets.h used after reboot"));
  } else if (!strcmp(line, "reboot")) {
    Serial.println(F("[cfg] rebooting"));
    ESP.restart();
  } else if (line[0]) {
    printHelp();
  }
}

void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      handleCommand(lineBuf);
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

// ---- Demo: simulated access-point outage ------------------------------------
void checkButton(unsigned long now) {
  int b = digitalRead(BUTTON_PIN);
  if (b == LOW && lastButton == HIGH && !simOutage) {
    simOutage = true;
    simOutageUntil = now + SIM_OUTAGE_MS;
    WiFi.disconnect();     // no clean MQTT DISCONNECT, so the last will fires
    Serial.println(F("[sim] access point DOWN for 20 s"));
  }
  lastButton = b;
  if (simOutage && (long)(now - simOutageUntil) >= 0) {
    simOutage = false;
    Serial.println(F("[sim] access point back UP"));
  }
}

// ---- WiFi ---------------------------------------------------------------------
void manageWifi(unsigned long now) {
  bool connected = (WiFi.status() == WL_CONNECTED) && !simOutage;

  if (connected) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      wifiAttemptActive = false;
      wifiFailures = 0;
      wifiReconnects++;
      Serial.print(F("[wifi] connected, IP "));
      Serial.print(WiFi.localIP());
      Serial.print(F(", RSSI "));
      Serial.println(WiFi.RSSI());
      configTime(0, 0, "pool.ntp.org");      // wall clock for health "ts"
      nextMqttAttemptAt = now;
    }
    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println(F("[wifi] connection LOST, buffering readings"));
    nextWifiAttemptAt = now;
  }
  if (simOutage) return;

  if (wifiAttemptActive) {
    if (now - wifiAttemptAt < WIFI_CONNECT_WAIT_MS) return;
    wifiAttemptActive = false;
    if (wifiFailures < 255) wifiFailures++;
    unsigned long wait = backoffMs(WIFI_BACKOFF_MIN_MS, wifiFailures);
    nextWifiAttemptAt = now + wait;
    Serial.print(F("[wifi] attempt failed, retry in "));
    Serial.print(wait);
    Serial.println(F(" ms"));
    return;
  }

  if ((long)(now - nextWifiAttemptAt) >= 0) {
    Serial.print(F("[wifi] connecting to "));
    Serial.println(wifiSsid);
    WiFi.disconnect();
    WiFi.begin(wifiSsid, wifiPass);
    wifiAttemptActive = true;
    wifiAttemptAt = now;
  }
}

// ---- MQTT ---------------------------------------------------------------------
void manageMqtt(unsigned long now) {
  if (!wifiWasConnected) return;
  if (mqtt.connected()) { mqtt.loop(); return; }
  if ((long)(now - nextMqttAttemptAt) < 0) return;

  Serial.print(F("[mqtt] connecting to "));
  Serial.print(MQTT_HOST);
  Serial.println(F("..."));
  bool ok = mqtt.connect(deviceId,
                         mqttUser[0] ? mqttUser : nullptr,
                         mqttUser[0] ? mqttPass : nullptr,
                         topicStatus, 1, true, "offline");
  if (ok) {
    mqttFailures = 0;
    mqttReconnects++;
    mqtt.publish(topicStatus, "online", true);
    healthDue = true;                        // fresh snapshot right away
    Serial.print(F("[mqtt] connected as "));
    Serial.print(deviceId);
    Serial.print(F(", "));
    Serial.print(bufCount);
    Serial.println(F(" reading(s) waiting"));
  } else {
    if (mqttFailures < 255) mqttFailures++;
    unsigned long wait = backoffMs(MQTT_BACKOFF_MIN_MS, mqttFailures);
    nextMqttAttemptAt = now + wait;
    Serial.print(F("[mqtt] failed, state="));
    Serial.print(mqtt.state());
    Serial.print(F(", retry in "));
    Serial.print(wait);
    Serial.println(F(" ms"));
  }
}

void flushBuffer(unsigned long now) {
  if (!wifiWasConnected || !mqtt.connected()) return;
  char payload[160];
  uint8_t sent = 0;
  while (bufCount > 0 && sent < MAX_PUBLISH_PER_PASS) {
    const Reading& r = bufferPeek();
    snprintf(payload, sizeof(payload),
             "{\"seq\":%lu,\"t\":%lu,\"age\":%lu,\"level\":%u,\"lamp\":%u,\"why\":\"%s\"}",
             (unsigned long)r.seq, (unsigned long)r.takenAt,
             (unsigned long)(now - r.takenAt), r.level, r.lamp, WHY_NAMES[r.why]);
    if (!mqtt.publish(topicReading, payload)) break;   // keep it, retry later
    bufferPop();
    publishedCount++;
    sent++;
  }
  if (sent > 1) {
    Serial.print(F("[mqtt] sent "));
    Serial.print(sent);
    Serial.print(F(" buffered reading(s), "));
    Serial.print(bufCount);
    Serial.println(F(" left"));
  }
}

// Health is a snapshot, not history: it is published only while online
// (retained, so a dashboard that opens later still sees the latest one).
void publishHealth(unsigned long now) {
  if (!wifiWasConnected || !mqtt.connected()) return;
  char payload[320];
  time_t ts = time(nullptr);
  snprintf(payload, sizeof(payload),
           "{\"fw\":\"%s\",\"ts\":%lu,\"up\":%lu,\"rssi\":%d,\"heap\":%lu,\"cpu\":%lu,"
           "\"reset\":\"%s\",\"boots\":%lu,\"buffered\":%u,\"dropped\":%lu,"
           "\"published\":%lu,\"wifi_reconnects\":%lu,\"mqtt_reconnects\":%lu,\"creds\":\"%s\"}",
           FW_VERSION, (unsigned long)(ts > 1700000000 ? ts : 0), now / 1000,
           (int)WiFi.RSSI(), (unsigned long)ESP.getFreeHeap(), (unsigned long)getCpuFrequencyMhz(),
           resetReason, (unsigned long)bootCount, bufCount, (unsigned long)droppedReadings,
           (unsigned long)publishedCount, (unsigned long)wifiReconnects,
           (unsigned long)mqttReconnects, credSource);
  mqtt.publish(topicHealth, payload, true);
}

void printStats() {
  Serial.print(F("[stats] level="));    Serial.print(level);
  Serial.print(F(" lamp="));            Serial.print(brightness);
  Serial.print(F(" wifi="));            Serial.print(wifiWasConnected ? F("up") : F("DOWN"));
  Serial.print(F(" mqtt="));            Serial.print((wifiWasConnected && mqtt.connected()) ? F("up") : F("DOWN"));
  Serial.print(F(" buffered="));        Serial.print(bufCount);
  Serial.print(F(" published="));       Serial.print(publishedCount);
  Serial.print(F(" dropped="));         Serial.println(droppedReadings);
}

void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(CPU_MHZ);
  pinMode(LAMP_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  resetReason = describeReset(esp_reset_reason());
  prefs.begin("sys", false);
  bootCount = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", bootCount);
  prefs.end();

  int first = analogRead(SENSOR_PIN);
  for (uint8_t i = 0; i < AVG_SAMPLES; i++) samples[i] = first;
  sampleSum = (long)first * AVG_SAMPLES;
  level = first;

  WiFi.mode(WIFI_STA);              // must come before reading the MAC
  WiFi.setAutoReconnect(false);     // manageWifi() owns reconnects
  WiFi.setSleep(true);              // modem sleep between beacons
  makeDeviceId();
  randomSeed(esp_random());
  loadCredentials();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);
  mqtt.setBufferSize(512);

  Serial.print(F("Night Lamp IoT v" FW_VERSION ", device "));
  Serial.print(deviceId);
  Serial.print(F(", boot #"));
  Serial.print(bootCount);
  Serial.print(F(", reset="));
  Serial.print(resetReason);
  Serial.print(F(", creds from "));
  Serial.println(credSource);
  Serial.println(F("Type `help` for provisioning commands."));

  setupWatchdog();
  unsigned long now = millis();
  lastSampleAt = lastEvalAt = lastHealthAt = now;
}

void loop() {
  esp_task_wdt_reset();             // "still alive"; a hang longer than 30 s reboots
  unsigned long now = millis();

  pollSerial();
  checkButton(now);
  if (due(lastSampleAt, SAMPLE_MS, now)) sampleAndDrive();
  if (due(lastEvalAt, EVAL_MS, now))     evaluateReading(now);

  manageWifi(now);
  manageMqtt(now);
  flushBuffer(now);

  if (due(lastHealthAt, HEALTH_MS, now)) { healthDue = true; printStats(); }
  if (healthDue && wifiWasConnected && mqtt.connected()) {
    publishHealth(now);
    healthDue = false;
  }
}
