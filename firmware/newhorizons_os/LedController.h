#pragma once

#include <Arduino.h>

namespace nhos {

struct LedColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

namespace LedPalette {
static constexpr LedColor Off{0, 0, 0};
static constexpr LedColor Boot{0, 0, 24};
static constexpr LedColor WifiSetup{24, 0, 0};
static constexpr LedColor Online{0, 24, 0};
static constexpr LedColor Maintenance{24, 12, 0};
static constexpr LedColor SafeMode{24, 0, 24};
static constexpr LedColor Ota{0, 12, 24};
static constexpr LedColor Error{24, 0, 0};
}  // namespace LedPalette

class LedController {
 public:
  void begin();
  void setStatus(LedColor color);
  void setExternal(uint8_t index, LedColor color);
  void pulse(LedColor color, uint16_t delayMs);

 private:
  void writePixel(uint8_t pin, LedColor color);
};

}  // namespace nhos
