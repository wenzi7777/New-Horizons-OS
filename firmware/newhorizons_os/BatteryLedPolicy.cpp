#include "BatteryLedPolicy.h"

namespace nhos {
namespace {

constexpr BatteryLedColor kOff{0, 0, 0};
constexpr BatteryLedColor kGreen{0, 80, 0};
constexpr BatteryLedColor kYellowGreen{64, 96, 0};
constexpr BatteryLedColor kAmber{128, 48, 0};
constexpr BatteryLedColor kRed{128, 0, 0};

BatteryLedColor scale(BatteryLedColor color, uint8_t level) {
  return {
      static_cast<uint8_t>((static_cast<uint16_t>(color.r) * level) / 255),
      static_cast<uint8_t>((static_cast<uint16_t>(color.g) * level) / 255),
      static_cast<uint8_t>((static_cast<uint16_t>(color.b) * level) / 255),
  };
}

BatteryLedColor baseColor(uint16_t socCentiPercent) {
  if (socCentiPercent > 5000) return kGreen;
  if (socCentiPercent >= 2000) return kYellowGreen;
  if (socCentiPercent >= 1000) return kAmber;
  return kRed;
}

}  // namespace

BatteryLedColor batteryLedColor(bool sampleValid, uint16_t socCentiPercent,
                                bool charging, uint32_t nowMs) {
  if (!sampleValid) return kOff;
  const BatteryLedColor color = baseColor(socCentiPercent);
  // A critical battery blinks only while discharging. While charging, the
  // red breathing animation below still communicates both charge activity and
  // the critical state.
  if (socCentiPercent < 1000 && !charging) {
    return (nowMs / 500U) % 2U == 0U ? color : kOff;
  }
  if (!charging) return color;

  constexpr uint32_t kBreathePeriodMs = 1600;
  constexpr uint8_t kMinLevel = 48;
  const uint32_t phase = nowMs % kBreathePeriodMs;
  const uint32_t half = kBreathePeriodMs / 2;
  const uint32_t ramp = phase < half ? phase : kBreathePeriodMs - phase;
  const uint8_t level = static_cast<uint8_t>(
      kMinLevel + ((255U - kMinLevel) * ramp) / half);
  return scale(color, level);
}

}  // namespace nhos
