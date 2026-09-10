#pragma once

#include <Arduino.h>

namespace nhos {

// Where the current wall-clock reading came from.
enum class TimeSource : uint8_t {
  None = 0,  // free-running; only monotonic uptime is meaningful
  Sntp = 1,  // disciplined by ESP-IDF's SNTP client (WiFi mode only)
  Host = 2,  // pushed in by whoever can reach us -- see setEpochMs()
};

// The device clock service: monotonic uptime plus, when available, wall time.
//
// Under WiFi the ESP32's system clock is disciplined by SNTP (including
// ESP-IDF's periodic background re-sync), so callers just read the current
// real time directly per packet -- no anchor tracking needed.
//
// ESP-NOW/Direct devices never associate with an AP and so can never reach an
// NTP server: every packet they emitted used to carry epochValid=false, which
// for a research device means sensor data with no wall-clock timestamp at all.
// setEpochMs() closes that gap -- it is driven by the ordinary `set_time`
// control command, so the same path works whether it arrives over UDP from a
// Gateway or relayed through a Hub, with no new protocol message either way.
class TimeSync {
 public:
  void begin();

  bool hasSynced() const;
  uint64_t nowEpochMs() const;
  // Monotonic since boot; valid even when no wall time is known.
  uint32_t uptimeMs() const { return millis(); }
  TimeSource source() const { return source_; }
  static const char* sourceName(TimeSource source);

  // Adopts an externally supplied wall time. Rejects values below
  // kTimeSyncValidEpochS so a zero/garbage push cannot destroy a good clock.
  bool setEpochMs(uint64_t epochMs, TimeSource source);

  // Signed difference between the last pushed time and what this device
  // believed at that moment -- the only drift signal available without a
  // reference clock of our own.
  int64_t lastAdjustMs() const { return lastAdjustMs_; }

  String statusJson() const;

  // Called from the SNTP sync callback; not for direct use elsewhere.
  void recordSync();

 private:
  bool started_ = false;
  volatile bool synced_ = false;
  TimeSource source_ = TimeSource::None;
  uint32_t lastSetMs_ = 0;
  int64_t lastAdjustMs_ = 0;
  uint32_t adjustCount_ = 0;
};

}  // namespace nhos
