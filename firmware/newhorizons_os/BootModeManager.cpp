#include "BootModeManager.h"

#include "BoardConfig.h"
#include "BoardPins.h"

namespace nhos {

void BootModeManager::begin() {
  prefs_.begin("nhos_boot", false);
  uint8_t bootFailures = prefs_.getUChar("boot_fail", 0);
  prefs_.putUChar("boot_fail", static_cast<uint8_t>(bootFailures + 1));
#if NHOS_BOARD_HAS_BUTTON
  pinMode(kActionButtonPin, INPUT_PULLUP);
  wifiSetupRequested_ = sampleWifiSetupButtonWindow();
  if (wifiSetupRequested_) {
    Serial.println(F("boot_action_button_setup_requested"));
  }
#else
  wifiSetupRequested_ = sampleMultiCycleSetupTrigger();
  if (wifiSetupRequested_) {
    Serial.println(F("boot_multi_cycle_setup_requested"));
  }
#endif
  if (bootFailures + 1 >= kSafeModeBootFailures) {
    mode_ = RunMode::SafeMaintenance;
  } else if (prefs_.getBool("maint", false)) {
    mode_ = RunMode::Maintenance;
  } else {
    mode_ = RunMode::Normal;
  }
}

RunMode BootModeManager::mode() const {
  return mode_;
}

const char* BootModeManager::modeName() const {
  if (mode_ == RunMode::SafeMaintenance) {
    return "safe_maintenance";
  }
  if (mode_ == RunMode::Maintenance) {
    return "maintenance";
  }
  return "normal";
}

void BootModeManager::enterMaintenance(bool safe) {
  mode_ = safe ? RunMode::SafeMaintenance : RunMode::Maintenance;
  prefs_.putBool("maint", true);
}

void BootModeManager::exitMaintenance() {
  mode_ = RunMode::Normal;
  prefs_.putBool("maint", false);
}

void BootModeManager::markBootOk() {
  prefs_.putUChar("boot_fail", 0);
}

void BootModeManager::markWifiConnected() {
#if !NHOS_BOARD_HAS_BUTTON
  prefs_.putUChar("prov_cnt", 0);
#endif
}

void BootModeManager::requestReboot() {
  rebootRequested_ = true;
}

bool BootModeManager::rebootRequested() const {
  return rebootRequested_;
}

bool BootModeManager::wifiSetupRequested() const {
  return wifiSetupRequested_;
}

#if NHOS_BOARD_HAS_BUTTON
bool BootModeManager::sampleWifiSetupButtonWindow() const {
  const uint32_t started = millis();
  while (millis() - started < kBootWifiSetupWindowMs) {
    if (digitalRead(kActionButtonPin) == LOW) {
      return true;
    }
    delay(10);
  }
  return false;
}
#else
bool BootModeManager::sampleMultiCycleSetupTrigger() {
  uint8_t count = prefs_.getUChar("prov_cnt", 0);
  ++count;
  if (count >= kMultiCycleSetupCount) {
    prefs_.putUChar("prov_cnt", 0);
    return true;
  }
  prefs_.putUChar("prov_cnt", count);
  return false;
}
#endif

}  // namespace nhos
