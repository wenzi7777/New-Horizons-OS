#include "BatteryLedPolicy.h"

namespace nhos {
namespace {

constexpr BatteryLedColor kOff{0, 0, 0};
constexpr BatteryLedColor kGreen{0, 80, 0};
constexpr BatteryLedColor kOrange{128, 48, 0};

BatteryLedColor baseColor(uint16_t socCentiPercent) {
  // The dedicated v1.5.F battery pixel is a continuous fuel indicator:
  // orange at 0%, green at 100%.  Avoid threshold colors and low-battery
  // flashing here; the user can read its level at a glance without glare.
  const uint32_t soc = socCentiPercent > 10000U ? 10000U : socCentiPercent;
  const uint32_t remaining = 10000U - soc;
  return {
      static_cast<uint8_t>((kOrange.r * remaining + kGreen.r * soc) / 10000U),
      static_cast<uint8_t>((kOrange.g * remaining + kGreen.g * soc) / 10000U),
      static_cast<uint8_t>((kOrange.b * remaining + kGreen.b * soc) / 10000U),
  };
}

}  // namespace

BatteryLedColor batteryLedColor(bool sampleValid, uint16_t socCentiPercent,
                                bool charging, uint32_t nowMs) {
  if (!sampleValid) return kOff;
  const BatteryLedColor color = baseColor(socCentiPercent);
  if (!charging) return color;

  // Keep the actual SoC hue while charging. A calm on/off cadence communicates
  // charge activity without making the status pixel compete with the system
  // state or shining into the user's eyes.
  constexpr uint32_t kChargeBlinkPeriodMs = 1200;
  constexpr uint32_t kChargeBlinkOnMs = 650;
  return (nowMs % kChargeBlinkPeriodMs) < kChargeBlinkOnMs ? color : kOff;
}

}  // namespace nhos
