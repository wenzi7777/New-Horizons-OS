#pragma once

#include <Arduino.h>
#include "BatteryGaugeSyncPolicy.h"
#include "BatteryManualProfile.h"
#include "BatteryProfile.h"

namespace nhos {
class PowerManager;
struct BatteryGaugeSample { bool valid = false; uint8_t status = 0; uint8_t fault = 0; uint16_t vbatMv = 0; uint16_t socCentiPercent = 0; int16_t rate = 0; };

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
  void service(uint32_t nowMs);
  bool detectNow(uint32_t nowMs);
  GaugeResyncResult requestResync(uint32_t nowMs);
  bool copyLatestSample(BatteryGaugeSample& out) const;
  const BatteryProfile& profile() const { return profile_; }
  const ManualBatteryProfile& manualProfile() const { return manualProfile_; }
  BatteryIdClass detectedBatteryId() const { return lastBatteryId_; }
  String statusJson() const;
 private:
  bool readGauge(BatteryGaugeSample& sample);
  bool readWord(uint8_t reg, uint16_t& value);
  bool writeWord(uint8_t reg, uint16_t value);
  bool startQuickStart(uint32_t nowMs);
  void applySample(const BatteryGaugeSample& sample);
  void markReadFailed(const char* diagnostic);
  BatteryIdClass readBatteryId() const;
  void updateProfile(const BatteryProfile& profile);
  BatteryGaugeSample sample_;
  BatteryProfile profile_{BatteryProfileId::Unknown, BatteryProfileSource::Pending, false, true, 0, 100};
  ManualBatteryProfile manualProfile_{};
  BatteryIdClass lastBatteryId_ = BatteryIdClass::Unknown;
  bool detected_ = false;
  String diagnostic_;
  uint32_t lastPollMs_ = 0;
  PowerManager* power_ = nullptr;
  bool hasAppliedChargeLimit_ = false;
  uint16_t appliedChargeLimitMa_ = 0;
  BatteryGaugeSyncPolicy syncPolicy_;
};
}  // namespace nhos
