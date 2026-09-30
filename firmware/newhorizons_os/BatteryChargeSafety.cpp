#include "BatteryChargeSafety.h"

namespace nhos {

bool shouldApplyBatteryChargeLimit(bool hasAppliedLimit, uint16_t appliedLimitMa,
                                   uint16_t requestedLimitMa) {
  return !hasAppliedLimit || appliedLimitMa != requestedLimitMa;
}

uint16_t effectiveChargeCurrentMa(uint16_t profileMa, uint16_t ceilingMa) {
  if (ceilingMa == 0 || profileMa <= ceilingMa) {
    return profileMa;
  }
  return ceilingMa;
}

bool bq25180IchgCodeForMa(uint16_t ma, uint8_t& code) {
  if (ma < 100 || ma > 350 || ma % 10 != 0) {
    return false;
  }
  code = static_cast<uint8_t>(0x25 + (ma - 100) / 10);
  return true;
}

}  // namespace nhos
