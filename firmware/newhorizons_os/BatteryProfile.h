#pragma once

#include <cstdint>

#include "BatteryManualProfile.h"

namespace nhos {

enum class BatteryIdClass : uint8_t {
  Unknown,
  Ohm1k,
  Ohm10k,
  Ohm100k,
  NoId,
  JstOrUnknown,
};
enum class BatteryProfileId : uint8_t { None, Pogo1k, Pogo10k, Jst, Manual, Unknown };
enum class BatteryProfileSource : uint8_t { Auto, Manual, Pending };

struct BatteryProfile {
  BatteryProfileId id;
  BatteryProfileSource source;
  bool resolved;
  bool required;
  uint16_t capacityMah;
  uint16_t maxChargeCurrentMa;
};

BatteryProfile resolveBatteryProfile(bool gaugeResponsive, bool voltageValid,
                                    BatteryIdClass detectedId,
                                    BatteryProfileId manualProfile);
BatteryProfile resolveBatteryProfile(bool gaugeResponsive, bool voltageValid,
                                    BatteryIdClass detectedId,
                                    const ManualBatteryProfile& manualProfile);
uint16_t max17048RawVcellToMv(uint16_t raw);
uint16_t max17048RawSocToCentiPercent(uint16_t raw);
int16_t max17048RawRateToCentiPercentPerHour(uint16_t raw);
bool isValidBatteryVoltageMv(uint16_t mv);
BatteryIdClass classifyBatteryIdAdcRaw(int raw);
const char* batteryIdClassName(BatteryIdClass id);
const char* batteryProfileIdName(BatteryProfileId id);
const char* batteryProfileSourceName(BatteryProfileSource source);

}  // namespace nhos
