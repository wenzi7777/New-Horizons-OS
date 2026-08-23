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

void testBatteryIdDividerDistinguishesNoIdFromKnownResistors() {
  using namespace nhos;

  // v1.5.F BAT_ID has a 100k pull-up. These representative 12-bit ADC
  // samples correspond to 1k, 10k, 100k-to-GND and an open/no-ID POGO pin.
  assert(classifyBatteryIdAdcRaw(40) == BatteryIdClass::Ohm1k);
  assert(classifyBatteryIdAdcRaw(372) == BatteryIdClass::Ohm10k);
  assert(classifyBatteryIdAdcRaw(2048) == BatteryIdClass::Ohm100k);
  assert(classifyBatteryIdAdcRaw(4090) == BatteryIdClass::NoId);
  assert(classifyBatteryIdAdcRaw(-1) == BatteryIdClass::Unknown);
  assert(classifyBatteryIdAdcRaw(1200) == BatteryIdClass::Unknown);
}

void testDetectedIdsOverrideManualCurrentButNoIdUsesManualDefault() {
  using namespace nhos;
  const ManualBatteryProfile manual{600, 250, true};

  const BatteryProfile auto10k = resolveBatteryProfile(
      true, true, BatteryIdClass::Ohm10k, manual);
  assert(auto10k.source == BatteryProfileSource::Auto);
  assert(auto10k.maxChargeCurrentMa == 200);

  const BatteryProfile auto100k = resolveBatteryProfile(
      true, true, BatteryIdClass::Ohm100k, manual);
  assert(auto100k.source == BatteryProfileSource::Auto);
  assert(auto100k.maxChargeCurrentMa == 100);

  const BatteryProfile noIdManual = resolveBatteryProfile(
      true, true, BatteryIdClass::NoId, manual);
  assert(noIdManual.source == BatteryProfileSource::Manual);
  assert(noIdManual.maxChargeCurrentMa == 250);

  const BatteryProfile noIdDefault = resolveBatteryProfile(
      true, true, BatteryIdClass::NoId, ManualBatteryProfile{});
  assert(noIdDefault.source == BatteryProfileSource::Manual);
  assert(noIdDefault.maxChargeCurrentMa == 100);
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
  testBatteryIdDividerDistinguishesNoIdFromKnownResistors();
  testDetectedIdsOverrideManualCurrentButNoIdUsesManualDefault();
  testMax17048RawConversions();
  std::cout << "v1.5.F foundation tests passed\n";
  return 0;
}
