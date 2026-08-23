#pragma once

#include <cstdint>

namespace nhos {

struct ManualBatteryProfile {
  uint16_t capacityMah = 0;
  uint16_t maxChargeCurrentMa = 100;
  bool configured = false;
};

bool validManualBatteryProfile(uint16_t capacityMah, uint16_t maxChargeCurrentMa);
bool manualBatteryProfileIsUsable(const ManualBatteryProfile& profile);
// A board without MAX17048 cannot safely persist or apply this command.
bool batteryProfileCommandSupported(bool hasMax17048);

}  // namespace nhos
