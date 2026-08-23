#pragma once

#include <cstdint>

namespace nhos {

struct BatteryLedColor {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

BatteryLedColor batteryLedColor(bool sampleValid, uint16_t socCentiPercent,
                                bool charging, uint32_t nowMs);

}  // namespace nhos
