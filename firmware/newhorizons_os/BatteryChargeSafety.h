#pragma once

#include <cstdint>

namespace nhos {

bool shouldApplyBatteryChargeLimit(bool hasAppliedLimit, uint16_t appliedLimitMa,
                                   uint16_t requestedLimitMa);

}  // namespace nhos
