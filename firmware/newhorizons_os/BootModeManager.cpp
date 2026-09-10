#include "BootModeManager.h"

#include <esp_ota_ops.h>

#include "BoardConfig.h"
#include "BoardPins.h"
#include "JsonUtils.h"

namespace nhos {

void BootModeManager::begin() {
  prefs_.begin("nhos_boot", false);
  evaluateOtaRollbackState();
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

// Mirrors what the Arduino core would have done in initArduino(), except
// deferred to the point where this boot has actually proven itself. See
// verifyRollbackLater() in newhorizons_os.ino.
bool BootModeManager::confirmFirmwareValid() {
  if (!otaPendingVerify_ || firmwareConfirmed_) {
    return false;
  }
  if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
    return false;
  }
  firmwareConfirmed_ = true;
  // Probation is over -- the next boot must not mistake this image for one
  // the bootloader reverted away from (see evaluateOtaRollbackState()).
  prefs_.remove("pend_ver");
  return true;
}

String BootModeManager::otaRollbackStatusJson() const {
  String json = "{";
  json += "\"pending_verify\":";
  json += otaPendingVerify_ ? "true" : "false";
  json += ",\"confirmed\":";
  json += firmwareConfirmed_ ? "true" : "false";
  json += ",\"rolled_back_from\":\"";
  json += jsonEscape(rolledBackFrom_);
  json += "\"}";
  return json;
}

// Rollback bookkeeping, run once per boot before anything else touches
// nhos_boot. The bootloader itself leaves no "you were reverted" flag, so
// this reconstructs it: while an image is on probation its version is
// parked in `pend_ver`; if a later boot finds `pend_ver` naming a version
// that is not the one now running, the bootloader must have reverted us.
void BootModeManager::evaluateOtaRollbackState() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK) {
    otaPendingVerify_ = (state == ESP_OTA_IMG_PENDING_VERIFY);
  }

  const String pending = prefs_.getString("pend_ver", "");
  if (otaPendingVerify_) {
    if (pending != kFirmwareVersion) {
      prefs_.putString("pend_ver", String(kFirmwareVersion));
    }
  } else if (pending.length() > 0) {
    if (pending == kFirmwareVersion) {
      // Confirmed on an earlier boot; nothing was reverted.
      prefs_.remove("pend_ver");
    } else {
      prefs_.putString("rb_from", pending);
      prefs_.remove("pend_ver");
    }
  }
  rolledBackFrom_ = prefs_.getString("rb_from", "");
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
