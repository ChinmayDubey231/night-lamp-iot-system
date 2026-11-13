// Fake ESP-IDF task watchdog. Matches both API generations:
//   ESP-IDF 4.x (Arduino-ESP32 2.x): esp_task_wdt_init(timeout_s, panic)
//   ESP-IDF 5.x (Arduino-ESP32 3.x): esp_task_wdt_init/reconfigure(&config)
#pragma once
#include <cstdint>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
typedef void* TaskHandle_t;

extern uint32_t fake_wdt_timeout_ms;
extern bool fake_wdt_panic;
extern int fake_wdt_tasks;
extern long fake_wdt_feeds;

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
typedef struct {
  uint32_t timeout_ms;
  uint32_t idle_core_mask;
  bool trigger_panic;
} esp_task_wdt_config_t;
inline esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t* c) {
  fake_wdt_timeout_ms = c->timeout_ms; fake_wdt_panic = c->trigger_panic; return ESP_OK;
}
inline esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t* c) { return esp_task_wdt_reconfigure(c); }
#else
inline esp_err_t esp_task_wdt_init(uint32_t timeout_s, bool panic) {
  fake_wdt_timeout_ms = timeout_s * 1000; fake_wdt_panic = panic; return ESP_OK;
}
#endif
inline esp_err_t esp_task_wdt_add(TaskHandle_t) { fake_wdt_tasks++; return ESP_OK; }
inline esp_err_t esp_task_wdt_reset() { fake_wdt_feeds++; return ESP_OK; }
