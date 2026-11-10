#include "BatteryIdPolicy.h"

namespace nhos {
namespace {

// Pin voltage at or above which no resistor is connected: 3.0 V is R >= 2 MOhm
// at a 200k pull-up, far above the 820k top ID.
constexpr uint32_t kNoIdFloorMv = 3000;
// Below this the pin is shorted (39k reads 0.54 V; 20k is 0.3 V).
constexpr uint32_t kShortCeilingOhm = 20000;

struct Tier {
  uint32_t upperOhm;     // exclusive upper bound of the band
  uint16_t capacityMah;
  uint16_t maxChargeCurrentMa;
};

// Same charge currents as the Desktop's battery presets.
constexpr Tier kTiers[] = {
    {51500, 200, 100},    // 39k  (midpoint with 68k  = sqrt(39*68)   ~ 51.5k)
    {82600, 400, 200},    // 68k  (midpoint with 100k = sqrt(68*100)  ~ 82.5k)
    {122500, 500, 350},   // 100k (midpoint with 150k = sqrt(100*150) ~ 122.5k)
    {181800, 600, 350},   // 150k (midpoint with 220k = sqrt(150*220) ~ 181.7k)
};

}  // namespace

BatteryIdResult classifyBatteryId(uint16_t mv, uint16_t supplyMv) {
  BatteryIdResult result;
  result.mv = mv;
  if (supplyMv == 0 || mv >= kNoIdFloorMv || mv >= supplyMv) {
    result.kind = BatteryIdKind::NoId;
    return result;
  }
  const uint32_t ohm = static_cast<uint32_t>(
      (static_cast<uint64_t>(kBatteryIdPullUpOhm) * mv) / (supplyMv - mv));
  result.resistanceOhm = ohm;
  if (ohm < kShortCeilingOhm) {
    result.kind = BatteryIdKind::Invalid;
    result.resistanceOhm = 0;
    return result;
  }
  for (const Tier& tier : kTiers) {
    if (ohm < tier.upperOhm) {
      result.kind = BatteryIdKind::Known;
      result.capacityMah = tier.capacityMah;
      result.maxChargeCurrentMa = tier.maxChargeCurrentMa;
      return result;
    }
  }
  result.kind = BatteryIdKind::Reserved;
  return result;
}

const char* batteryIdKindName(BatteryIdKind kind) {
  switch (kind) {
    case BatteryIdKind::NoId: return "none";
    case BatteryIdKind::Known: return "known";
    case BatteryIdKind::Reserved: return "reserved";
    case BatteryIdKind::Invalid: return "invalid";
  }
  return "none";
}

}  // namespace nhos
