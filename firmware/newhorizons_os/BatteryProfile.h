#pragma once

#include <cstdint>

#include "BatteryManualProfile.h"

namespace nhos {

// The battery is identified by hand only (set_battery_profile). The current
// PCB's BAT_ID line cannot be trusted, so nothing is read from it; automatic
// identification returns with the next board revision.
struct BatteryProfile {
  bool resolved;
  bool required;
  uint16_t capacityMah;
  uint16_t maxChargeCurrentMa;
};

// Charge ceiling while no battery has been configured.
constexpr uint16_t kUnconfiguredBatteryChargeLimitMa = 100;

BatteryProfile resolveBatteryProfile(const ManualBatteryProfile& manualProfile);
uint16_t max17048RawVcellToMv(uint16_t raw);
uint16_t max17048RawSocToCentiPercent(uint16_t raw);
int16_t max17048RawRateToCentiPercentPerHour(uint16_t raw);
bool isValidBatteryVoltageMv(uint16_t mv);

}  // namespace nhos
