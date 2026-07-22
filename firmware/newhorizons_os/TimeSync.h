#pragma once

#include <Arduino.h>

namespace nhos {

// Syncs wall-clock time over NTP once WiFi is up. Once synced, the ESP32's own
// system clock is disciplined (including ESP-IDF's periodic background
// re-sync), so callers just read the current real time directly per packet —
// no anchor tracking needed.
class TimeSync {
 public:
  void begin();

  bool hasSynced() const;
  uint64_t nowEpochMs() const;

  // Called from the SNTP sync callback; not for direct use elsewhere.
  void recordSync();

 private:
  bool started_ = false;
  volatile bool synced_ = false;
};

}  // namespace nhos
