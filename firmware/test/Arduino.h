// Minimal fake of the Arduino-ESP32 API for host-side tests. See run_tests.sh.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <sstream>
#include <string>

#define OUTPUT 1
#define INPUT_PULLUP 2
#define HIGH 1
#define LOW 0
#define F(s) (s)

inline long constrain(long x, long a, long b) { return x < a ? a : (x > b ? b : x); }
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
inline size_t fake_strlcpy(char* d, const char* s, size_t n) {
  size_t len = std::strlen(s);
  if (n) { size_t c = len < n - 1 ? len : n - 1; std::memcpy(d, s, c); d[c] = '\0'; }
  return len;
}
#define strlcpy fake_strlcpy

extern unsigned long fake_millis;
extern int fake_analog;
extern int fake_button;
extern int fake_pwm;
extern uint32_t fake_cpu_mhz;
extern int fake_config_time_calls;

inline unsigned long millis() { return fake_millis; }
inline unsigned long micros() { return fake_millis * 1000; }
inline int analogRead(uint8_t) { return fake_analog; }
inline void analogWrite(uint8_t, int v) { fake_pwm = v; }
inline int digitalRead(uint8_t) { return fake_button; }
inline void pinMode(uint8_t, uint8_t) {}
inline long random(long lo, long hi) { return hi <= lo ? lo : lo + std::rand() % (hi - lo); }
inline void randomSeed(unsigned long) { std::srand(42); }
inline uint32_t esp_random() { return 0x00abcdefu; }
inline bool setCpuFrequencyMhz(uint32_t m) { fake_cpu_mhz = m; return true; }
inline uint32_t getCpuFrequencyMhz() { return fake_cpu_mhz; }
inline void configTime(long, int, const char*) { fake_config_time_calls++; }

typedef enum {
  ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC,
  ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT
} esp_reset_reason_t;
extern esp_reset_reason_t fake_reset_reason;
inline esp_reset_reason_t esp_reset_reason() { return fake_reset_reason; }

struct FakeESP {
  int restarts = 0;
  uint32_t getFreeHeap() { return 214000; }
  void restart() { restarts++; }
};
extern FakeESP ESP;

// Serial: captures output, and the test can type input into it.
struct FakeSerial {
  bool echo = false;
  std::string out;
  std::deque<char> in;
  void begin(long) {}
  void type(const std::string& s) { for (char c : s) in.push_back(c); }
  int available() { return (int)in.size(); }
  int read() { if (in.empty()) return -1; char c = in.front(); in.pop_front(); return c; }
  void print(unsigned char v) { print((unsigned)v); }   // Arduino prints uint8_t as a number
  template <typename T> void print(T v) { std::ostringstream o; o << v; out += o.str(); if (echo) std::printf("%s", o.str().c_str()); }
  template <typename T> void println(T v) { print(v); print("\n"); }
};
extern FakeSerial Serial;
