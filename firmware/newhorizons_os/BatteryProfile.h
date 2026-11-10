#pragma once

#include <cstdint>

#include "BatteryIdPolicy.h"
#include "BatteryManualProfile.h"

namespace nhos {

// The battery is identified by hand (set_battery_profile). Up to v1.5.F the
// BAT_ID line could not be trusted, so nothing was read from it. v2.0.F reads
// it once at boot (BatteryIdPolicy.h): a recognised ID resistor sets the
// profile itself (`automatic`); anything else -- no resistor, a reserved or
// shorted value -- leaves the manual profile in charge.
struct BatteryProfile {
  bool resolved;
  bool required;
  uint16_t capacityMah;
  uint16_t maxChargeCurrentMa;
  bool automatic = false;
};

// Charge ceiling while no battery has been configured.
constexpr uint16_t kUnconfiguredBatteryChargeLimitMa = 100;

BatteryProfile resolveBatteryProfile(const ManualBatteryProfile& manualProfile);
// A recognised battery ID wins over the manual profile.
BatteryProfile resolveBatteryProfile(const ManualBatteryProfile& manualProfile,
                                     const BatteryIdResult& batteryId);
uint16_t max17048RawVcellToMv(uint16_t raw);
uint16_t max17048RawSocToCentiPercent(uint16_t raw);
int16_t max17048RawRateToCentiPercentPerHour(uint16_t raw);
bool isValidBatteryVoltageMv(uint16_t mv);

}  // namespace nhos
