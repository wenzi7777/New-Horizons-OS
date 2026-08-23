#pragma once

#include <Arduino.h>
#include "BatteryProfile.h"

namespace nhos {
class PowerManager;
struct BatteryGaugeSample { bool valid = false; uint8_t status = 0; uint8_t fault = 0; uint16_t vbatMv = 0; uint16_t socCentiPercent = 0; int16_t rate = 0; };

class BatteryGaugeManager {
 public:
  void begin();
  void setPowerManager(PowerManager& power);
  void service(uint32_t nowMs);
  bool copyLatestSample(BatteryGaugeSample& out) const;
  const BatteryProfile& profile() const { return profile_; }
  String statusJson() const;
 private:
  bool readWord(uint8_t reg, uint16_t& value);
  BatteryIdClass readBatteryId() const;
  void updateProfile(const BatteryProfile& profile);
  BatteryGaugeSample sample_;
  BatteryProfile profile_{BatteryProfileId::Unknown, BatteryProfileSource::Pending, false, true, 0, 100};
  bool detected_ = false;
  String diagnostic_;
  uint32_t lastPollMs_ = 0;
  PowerManager* power_ = nullptr;
  bool hasAppliedChargeLimit_ = false;
  uint16_t appliedChargeLimitMa_ = 0;
};
}  // namespace nhos
