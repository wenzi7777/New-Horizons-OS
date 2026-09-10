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
  // Confirms the running image so the bootloader stops treating it as on
  // probation. Must only be called once a boot is known-good -- see
  // verifyRollbackLater() in newhorizons_os.ino for why NHOS takes over
  // this decision from the Arduino core. Returns true if this call is what
  // actually cancelled the rollback.
  bool confirmFirmwareValid();
  // True when this boot is the first one after an OTA and the image has
  // not been confirmed yet: a reset before confirmFirmwareValid() reverts
  // to the previous slot.
  bool otaPendingVerify() const { return otaPendingVerify_; }
  bool firmwareConfirmed() const { return firmwareConfirmed_; }
  // Version of the image the bootloader last reverted away from, or empty
  // if this device has never been rolled back. Persisted across boots.
  const String& rolledBackFrom() const { return rolledBackFrom_; }
  String otaRollbackStatusJson() const;
  void markWifiConnected();
  void requestReboot();
  bool rebootRequested() const;
  bool wifiSetupRequested() const;

 private:
  void evaluateOtaRollbackState();
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
  bool otaPendingVerify_ = false;
  bool firmwareConfirmed_ = false;
  String rolledBackFrom_;
};

}  // namespace nhos
