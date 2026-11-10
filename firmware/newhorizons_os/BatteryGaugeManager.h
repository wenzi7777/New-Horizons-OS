#pragma once

#include <Arduino.h>
#include "BatteryGaugeSyncPolicy.h"
#include "BatteryManualProfile.h"
#include "BatteryProfile.h"
#include "BatterySocScaler.h"

namespace nhos {
class PowerManager;
// socCentiPercent is what everything reports (scaled to the charger's full,
// 0..100%); rawSocCentiPercent is the gauge register as read, for diagnostics.
struct BatteryGaugeSample { bool valid = false; uint8_t status = 0; uint8_t fault = 0; uint16_t vbatMv = 0; uint16_t socCentiPercent = 0; int16_t rate = 0; uint16_t rawSocCentiPercent = 0; };

enum class GaugeResyncResult : uint8_t {
  Started = 0,
  InProgress,
  Unavailable,
  Failed,
};

class BatteryGaugeManager {
 public:
  void begin();
  void setPowerManager(PowerManager& power);
  void setManualProfile(const ManualBatteryProfile& profile);
  // v2.0.F: the result of the one boot-time BAT_ID read. `readOk` false means
  // the pin could not be read (treated like no ID).
  void setBatteryId(const BatteryIdResult& id, bool readOk);
  void service(uint32_t nowMs);
  GaugeResyncResult requestResync(uint32_t nowMs);
  bool copyLatestSample(BatteryGaugeSample& out) const;
  const BatteryProfile& profile() const { return profile_; }
  const ManualBatteryProfile& manualProfile() const { return manualProfile_; }
  String statusJson() const;
 private:
  bool readGauge(BatteryGaugeSample& sample);
  bool readWord(uint8_t reg, uint16_t& value);
  bool writeWord(uint8_t reg, uint16_t value);
  bool startQuickStart(uint32_t nowMs);
  bool writeQuickStart(uint32_t nowMs);
  void resumeChargingIfPaused();
  void checkGaugeReset(uint32_t nowMs);
  void applySample(const BatteryGaugeSample& sample);
  void markReadFailed(const char* diagnostic);
  void updateProfile(const BatteryProfile& profile);
  BatteryGaugeSample sample_;
  BatteryProfile profile_{false, true, 0, kUnconfiguredBatteryChargeLimitMa};
  ManualBatteryProfile manualProfile_{};
  BatteryIdResult batteryId_{};
  bool batteryIdReadOk_ = false;
  bool detected_ = false;
  String diagnostic_;
  uint32_t lastPollMs_ = 0;
  PowerManager* power_ = nullptr;
  bool hasAppliedChargeLimit_ = false;
  uint16_t appliedChargeLimitMa_ = 0;
  BatteryGaugeSyncPolicy syncPolicy_;
  BatterySocScaler socScaler_;
  // Quick-start reads VCELL as the open-circuit voltage, which the charge
  // current pushes up; with a charger attached, charging is paused and the
  // write waits until relaxUntilMs_.
  bool quickStartPending_ = false;
  uint32_t relaxUntilMs_ = 0;
  bool pausedCharging_ = false;
  bool resetFlagChecked_ = false;
};
}  // namespace nhos
