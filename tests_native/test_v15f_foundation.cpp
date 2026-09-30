#include <cassert>
#include <cstdint>
#include <iostream>

#include "BatteryProfile.h"

namespace {

void testUnconfiguredBatteryStaysAtTheSafeCeiling() {
  using namespace nhos;
  const BatteryProfile unset = resolveBatteryProfile(ManualBatteryProfile{});
  assert(!unset.resolved);
  assert(unset.required);
  assert(unset.capacityMah == 0);
  assert(unset.maxChargeCurrentMa == kUnconfiguredBatteryChargeLimitMa);
  assert(kUnconfiguredBatteryChargeLimitMa == 100);

  // A stored-but-invalid profile is treated as no profile at all.
  const BatteryProfile invalid = resolveBatteryProfile(ManualBatteryProfile{600, 355, true});
  assert(!invalid.resolved);
  assert(invalid.maxChargeCurrentMa == 100);
  const BatteryProfile notConfigured = resolveBatteryProfile(ManualBatteryProfile{600, 250, false});
  assert(!notConfigured.resolved);
}

void testManualProfileIsTheOnlySource() {
  using namespace nhos;
  const BatteryProfile manual = resolveBatteryProfile(ManualBatteryProfile{600, 250, true});
  assert(manual.resolved);
  assert(!manual.required);
  assert(manual.capacityMah == 600);
  assert(manual.maxChargeCurrentMa == 250);
}

void testMax17048RawConversions() {
  using namespace nhos;
  assert(max17048RawVcellToMv(0xB800) == 3680);
  assert(max17048RawSocToCentiPercent(0x6400) == 10000);
  assert(max17048RawRateToCentiPercentPerHour(0x000A) == 208);
  assert(max17048RawRateToCentiPercentPerHour(0xFFF6) == -208);
  assert(isValidBatteryVoltageMv(2500));
  assert(isValidBatteryVoltageMv(5000));
  assert(!isValidBatteryVoltageMv(2499));
  assert(!isValidBatteryVoltageMv(5001));
}

}  // namespace

int main() {
  testUnconfiguredBatteryStaysAtTheSafeCeiling();
  testManualProfileIsTheOnlySource();
  testMax17048RawConversions();
  std::cout << "v1.5.F foundation tests passed\n";
  return 0;
}
