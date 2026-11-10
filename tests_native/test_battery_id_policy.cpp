#include <cassert>
#include <cstdint>
#include <iostream>

#include "BatteryIdPolicy.h"

using namespace nhos;

namespace {

uint16_t pinMvFor(uint32_t ohm) {
  return static_cast<uint16_t>((3300ULL * ohm) / (ohm + kBatteryIdPullUpOhm));
}

void expectKnown(uint32_t ohm, uint16_t capacity, uint16_t charge) {
  const BatteryIdResult r = classifyBatteryId(pinMvFor(ohm));
  assert(r.kind == BatteryIdKind::Known);
  assert(r.capacityMah == capacity);
  assert(r.maxChargeCurrentMa == charge);
}

void testAssignedValues() {
  expectKnown(39000, 200, 100);
  expectKnown(68000, 400, 200);
  expectKnown(100000, 500, 350);
  expectKnown(150000, 600, 350);
}

void testTolerance() {
  // 1 % resistors and +-30 mV of ADC error must not leave the band.
  const uint32_t nominal[] = {39000, 68000, 100000, 150000};
  const uint16_t capacity[] = {200, 400, 500, 600};
  for (size_t i = 0; i < 4; ++i) {
    for (int err = -30; err <= 30; err += 5) {
      const int mv = pinMvFor(nominal[i]) + err;
      const BatteryIdResult r = classifyBatteryId(static_cast<uint16_t>(mv));
      assert(r.kind == BatteryIdKind::Known);
      assert(r.capacityMah == capacity[i]);
    }
  }
}

void testReservedAndNoId() {
  // 220k..820k are reserved, never guessed at.
  const uint32_t reserved[] = {220000, 330000, 470000, 680000, 820000};
  for (uint32_t ohm : reserved) {
    assert(classifyBatteryId(pinMvFor(ohm)).kind == BatteryIdKind::Reserved);
  }
  // No resistor: the pin is at the rail (JST socket).
  assert(classifyBatteryId(3300).kind == BatteryIdKind::NoId);
  assert(classifyBatteryId(3150).kind == BatteryIdKind::NoId);
  assert(classifyBatteryId(3000).kind == BatteryIdKind::NoId);
  // 820k reads about 2.65 V, which is still an ID.
  assert(classifyBatteryId(2650).kind == BatteryIdKind::Reserved);
}

void testShort() {
  assert(classifyBatteryId(0).kind == BatteryIdKind::Invalid);
  assert(classifyBatteryId(100).kind == BatteryIdKind::Invalid);
  // 20k reads 0.30 V: the lowest value still accepted as a (bad) ID.
  assert(classifyBatteryId(310).kind != BatteryIdKind::Invalid);
}

void testNames() {
  assert(batteryIdKindName(BatteryIdKind::Known)[0] == 'k');
  assert(batteryIdKindName(BatteryIdKind::NoId)[0] == 'n');
}

}  // namespace

int main() {
  testAssignedValues();
  testTolerance();
  testReservedAndNoId();
  testShort();
  testNames();
  std::cout << "battery id policy tests passed\n";
  return 0;
}
