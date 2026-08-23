#include "BatteryManualProfile.h"

namespace nhos {

bool validManualBatteryProfile(uint16_t capacityMah, uint16_t maxChargeCurrentMa) {
  return capacityMah > 0 && maxChargeCurrentMa >= 100 &&
         maxChargeCurrentMa <= 350 && maxChargeCurrentMa % 10 == 0;
}

bool manualBatteryProfileIsUsable(const ManualBatteryProfile& profile) {
  return profile.configured &&
         validManualBatteryProfile(profile.capacityMah,
                                   profile.maxChargeCurrentMa);
}

}  // namespace nhos
