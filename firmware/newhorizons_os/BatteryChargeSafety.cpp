#include "BatteryChargeSafety.h"

namespace nhos {

bool shouldApplyBatteryChargeLimit(bool hasAppliedLimit, uint16_t appliedLimitMa,
                                   uint16_t requestedLimitMa) {
  return !hasAppliedLimit || appliedLimitMa != requestedLimitMa;
}

}  // namespace nhos
