#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "LifetimePolicy.h"

namespace nhos {

class BootModeManager;
class FaultRecorder;
class TimeSync;

// Device health counters: power-on time, reset history, OTA history,
// temperature and heap extremes. Served by the `health` command and
// /proc/health -- deliberately not folded into `status`, whose compact
// (ESP-NOW) form has little headroom left under the 7680 B response cap.
//
// Everything lives in RAM and is written back to NVS as one blob:
//   - once at boot (the reset reason / firmware change just counted),
//   - every kLifetimeFlushIntervalMs,
//   - on entering soft-off,
//   - on any esp_restart() (a shutdown handler covers every ESP.restart()
//     call site: reboot, OTA apply, pairing fallback, ...).
// A power cut or crash therefore loses at most one flush interval of time.
// The counters are never reset by any command.
class LifetimeStats {
 public:
  // After FaultRecorder::begin() and BootModeManager::begin().
  void begin(const FaultRecorder& faults, const BootModeManager& boot);
  void setClock(const TimeSync* clock) { clock_ = clock; }

  // Scheduler task, alwaysRun so soft-off time is counted too.
  void service(uint32_t nowMs, bool awake, bool scanning);

  void noteOtaConfirmed();
  void noteWifiDisconnect();
  void noteButtonPress();

  bool flush();

  String healthJson() const;  // `health` command payload
  String procText() const;    // /proc/health

 private:
  static void onShutdown();
  uint8_t healthReasons() const;
  uint32_t crashTotal() const;
  uint32_t sessionS() const { return millis() / 1000; }

  Preferences prefs_;
  bool ready_ = false;
  LifetimeCounters counters_;
  LifetimeRemainder remainder_;
  const FaultRecorder* faults_ = nullptr;
  const BootModeManager* boot_ = nullptr;
  const TimeSync* clock_ = nullptr;
  uint32_t lastTickMs_ = 0;
  uint32_t lastFlushMs_ = 0;
  uint32_t lastSampleMs_ = 0;
  bool sampled_ = false;
  bool wasAwake_ = true;
  int16_t currentTempC10_ = kLifetimeTempUnset;
};

}  // namespace nhos
