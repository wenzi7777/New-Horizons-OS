#pragma once

#include <Arduino.h>

#include "BatteryIdPolicy.h"

namespace nhos {

// Reads BAT_ID (GPIO15, ADC2) once, as a calibrated pin voltage. ADC2 is
// shared with Wi-Fi, so this belongs in boot before the radio starts; a read
// that loses arbitration comes back as 0xFFFF and is discarded. The matrix
// scanner's curve (AdcCurveFit) is used instead of analogReadMilliVolts():
// an open pin sits right at the top of the range, where the core's own curve
// overflows.
bool readBatteryIdMv(uint8_t pin, uint16_t& mv);

}  // namespace nhos
