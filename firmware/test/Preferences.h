// Fake ESP32 Preferences (NVS). Storage is global so it survives a
// simulated reboot, like real flash.
#pragma once
#include "Arduino.h"
#include <map>
#include <string>

extern std::map<std::string, std::map<std::string, std::string>> fake_nvs;

class Preferences {
  std::string ns;
  bool ro = true;
 public:
  bool begin(const char* name, bool readOnly = false) { ns = name; ro = readOnly; return true; }
  void end() {}
  size_t getString(const char* key, char* value, size_t maxLen) {
    auto& m = fake_nvs[ns];
    auto it = m.find(key);
    if (it == m.end()) { if (maxLen) value[0] = '\0'; return 0; }
    strlcpy(value, it->second.c_str(), maxLen);
    return it->second.size();
  }
  size_t putString(const char* key, const char* value) {
    if (ro) return 0;
    fake_nvs[ns][key] = value;
    return std::strlen(value);
  }
  uint32_t getUInt(const char* key, uint32_t def = 0) {
    auto& m = fake_nvs[ns];
    auto it = m.find(key);
    return it == m.end() ? def : (uint32_t)std::stoul(it->second);
  }
  size_t putUInt(const char* key, uint32_t v) {
    if (ro) return 0;
    fake_nvs[ns][key] = std::to_string(v);
    return 4;
  }
  bool clear() { if (ro) return false; fake_nvs[ns].clear(); return true; }
};
