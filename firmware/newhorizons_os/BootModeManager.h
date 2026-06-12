#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "Config.h"

namespace nhos {

class BootModeManager {
 public:
  void begin();
  RunMode mode() const;
  const char* modeName() const;
 void enterMaintenance(bool safe);
  void exitMaintenance();
  void markBootOk();
  void markWifiConnected();
  void requestReboot();
  bool rebootRequested() const;
  bool wifiSetupRequested() const;

 private:
#if NHOS_BOARD_HAS_BUTTON
  bool sampleWifiSetupButtonWindow() const;
#else
  bool sampleMultiCycleSetupTrigger();
  static constexpr uint8_t kMultiCycleSetupCount = 5;
#endif

  Preferences prefs_;
  RunMode mode_ = RunMode::Normal;
  bool rebootRequested_ = false;
  bool wifiSetupRequested_ = false;
};

}  // namespace nhos
