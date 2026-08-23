#include "BatteryLedPolicy.h"

namespace nhos {
namespace {

constexpr BatteryLedColor kOff{0, 0, 0};
constexpr BatteryLedColor kGreen{0, 80, 0};
constexpr BatteryLedColor kRed{80, 0, 0};

BatteryLedColor baseColor(uint16_t socCentiPercent) {
  // The dedicated v1.5.F battery pixel is a continuous fuel indicator:
  // red at 0%, green at 100%. The board-wide LED brightness is applied later.
  const uint32_t soc = socCentiPercent > 10000U ? 10000U : socCentiPercent;
  const uint32_t remaining = 10000U - soc;
  return {
      static_cast<uint8_t>((kRed.r * remaining + kGreen.r * soc) / 10000U),
      static_cast<uint8_t>((kRed.g * remaining + kGreen.g * soc) / 10000U),
      static_cast<uint8_t>((kRed.b * remaining + kGreen.b * soc) / 10000U),
  };
}

BatteryLedColor scale(BatteryLedColor color, uint8_t percent) {
  return {
      static_cast<uint8_t>((static_cast<uint16_t>(color.r) * percent) / 100U),
      static_cast<uint8_t>((static_cast<uint16_t>(color.g) * percent) / 100U),
      static_cast<uint8_t>((static_cast<uint16_t>(color.b) * percent) / 100U),
  };
}

}  // namespace

BatteryLedColor batteryLedColor(bool sampleValid, uint16_t socCentiPercent,
                                bool charging, uint32_t nowMs,
                                uint8_t lowBatteryThresholdPercent) {
  if (!sampleValid) return kOff;
  const BatteryLedColor color = baseColor(socCentiPercent);
  if (charging) {
    // A triangular 1.6s breathe preserves the actual SoC hue and never drops
    // the pixel to black, making charging distinguishable from a warning.
    constexpr uint32_t kChargeBreathePeriodMs = 1600;
    constexpr uint32_t kChargeBreatheHalfMs = kChargeBreathePeriodMs / 2;
    constexpr uint8_t kChargeBreatheMinPercent = 25;
    const uint32_t phase = nowMs % kChargeBreathePeriodMs;
    const uint32_t distanceFromDim = phase < kChargeBreatheHalfMs
        ? kChargeBreatheHalfMs - phase
        : phase - kChargeBreatheHalfMs;
    const uint8_t percent = static_cast<uint8_t>(
        kChargeBreatheMinPercent +
        ((100U - kChargeBreatheMinPercent) * distanceFromDim) /
            kChargeBreatheHalfMs);
    return scale(color, percent);
  }

  const uint16_t thresholdCentiPercent =
      static_cast<uint16_t>(lowBatteryThresholdPercent) * 100U;
  if (lowBatteryThresholdPercent > 0 &&
      socCentiPercent <= thresholdCentiPercent) {
    constexpr uint32_t kLowBatteryBlinkPeriodMs = 400;
    constexpr uint32_t kLowBatteryBlinkOnMs = 200;
    return (nowMs % kLowBatteryBlinkPeriodMs) < kLowBatteryBlinkOnMs
        ? color
        : kOff;
  }
  return color;
}

}  // namespace nhos
