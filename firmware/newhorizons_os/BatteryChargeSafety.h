#pragma once

#include <cstdint>

namespace nhos {

bool shouldApplyBatteryChargeLimit(bool hasAppliedLimit, uint16_t appliedLimitMa,
                                   uint16_t requestedLimitMa);

// The charge profile picks a speed; the battery's maximum charge current
// caps it. ceilingMa == 0 means no battery ceiling (boards without a gauge).
uint16_t effectiveChargeCurrentMa(uint16_t profileMa, uint16_t ceilingMa);

// BQ25180 ICHG code for 100..350 mA in 10 mA steps (0x25 = 100 mA).
// Returns false for anything outside that range.
bool bq25180IchgCodeForMa(uint16_t ma, uint8_t& code);

}  // namespace nhos
