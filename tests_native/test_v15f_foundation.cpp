#include <cassert>
#include <cstdint>
#include <iostream>

#include "BatteryProfile.h"

namespace {

void testAutoPogoProfilesRequireHealthyGauge() {
  using namespace nhos;

  const BatteryProfile pogo1k = resolveBatteryProfile(
      true, true, BatteryIdClass::Ohm1k, BatteryProfileId::None);
  assert(pogo1k.id == BatteryProfileId::Pogo1k);
  assert(pogo1k.source == BatteryProfileSource::Auto);
  assert(pogo1k.resolved);
  assert(!pogo1k.required);
  assert(pogo1k.capacityMah == 200);
  assert(pogo1k.maxChargeCurrentMa == 100);

  const BatteryProfile pogo10k = resolveBatteryProfile(
      true, true, BatteryIdClass::Ohm10k, BatteryProfileId::None);
  assert(pogo10k.id == BatteryProfileId::Pogo10k);
  assert(pogo10k.capacityMah == 400);
  assert(pogo10k.maxChargeCurrentMa == 200);

  const BatteryProfile unsafeFloat = resolveBatteryProfile(
      false, false, BatteryIdClass::Ohm10k, BatteryProfileId::None);
  assert(unsafeFloat.id == BatteryProfileId::Unknown);
  assert(unsafeFloat.source == BatteryProfileSource::Pending);
  assert(unsafeFloat.required);
  assert(unsafeFloat.maxChargeCurrentMa == 100);
}

void testUnknownAndJstNeverRaiseTheSafetyLimit() {
  using namespace nhos;
  const BatteryProfile id100k = resolveBatteryProfile(
      true, true, BatteryIdClass::Ohm100k, BatteryProfileId::None);
  assert(id100k.id == BatteryProfileId::Unknown);
  assert(id100k.required);
  assert(id100k.maxChargeCurrentMa == 100);

  const BatteryProfile jstPending = resolveBatteryProfile(
      true, true, BatteryIdClass::JstOrUnknown, BatteryProfileId::None);
  assert(jstPending.id == BatteryProfileId::Jst);
  assert(jstPending.source == BatteryProfileSource::Pending);
  assert(jstPending.required);
  assert(jstPending.maxChargeCurrentMa == 100);

  const BatteryProfile manualJst = resolveBatteryProfile(
      false, false, BatteryIdClass::Unknown, BatteryProfileId::Jst);
  assert(manualJst.source == BatteryProfileSource::Manual);
  assert(manualJst.resolved);
  assert(!manualJst.required);
  assert(manualJst.maxChargeCurrentMa == 100);
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
  testAutoPogoProfilesRequireHealthyGauge();
  testUnknownAndJstNeverRaiseTheSafetyLimit();
  testMax17048RawConversions();
  std::cout << "v1.5.F foundation tests passed\n";
  return 0;
}
