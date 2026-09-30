#include "BatteryProfile.h"

namespace nhos {

BatteryProfile resolveBatteryProfile(const ManualBatteryProfile& manualProfile) {
  if (manualBatteryProfileIsUsable(manualProfile)) {
    return {true, false, manualProfile.capacityMah, manualProfile.maxChargeCurrentMa};
  }
  return {false, true, 0, kUnconfiguredBatteryChargeLimitMa};
}

uint16_t max17048RawVcellToMv(uint16_t raw) { return static_cast<uint16_t>((static_cast<uint32_t>(raw) * 5U + 32U) / 64U); }
uint16_t max17048RawSocToCentiPercent(uint16_t raw) { return static_cast<uint16_t>((static_cast<uint32_t>(raw) * 100U + 128U) / 256U); }
int16_t max17048RawRateToCentiPercentPerHour(uint16_t raw) { return static_cast<int16_t>((static_cast<int32_t>(static_cast<int16_t>(raw)) * 208) / 10); }
bool isValidBatteryVoltageMv(uint16_t mv) { return mv >= 2500 && mv <= 5000; }

}  // namespace nhos
