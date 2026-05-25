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
  void requestReboot();
  bool rebootRequested() const;
  bool wifiSetupRequested() const;

 private:
  bool sampleWifiSetupButtonWindow() const;

  Preferences prefs_;
  RunMode mode_ = RunMode::Normal;
  bool rebootRequested_ = false;
  bool wifiSetupRequested_ = false;
};

}  // namespace nhos
