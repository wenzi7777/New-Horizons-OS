#include <cassert>
#include <cstdint>
#include <iostream>

#include "BatterySocScaler.h"

namespace {

using nhos::BatterySocScaler;

void testUnlearnedClampsRawToHundred() {
  BatterySocScaler s;
  s.reset();
  assert(s.apply(5000, false, false) == 5000);
  assert(s.apply(10350, false, true) == 10000);
  assert(s.apply(25599, false, false) == 10000);
  assert(!s.learned());
}

void testLowTerminationScalesUp() {
  BatterySocScaler s;
  s.reset();
  s.apply(8800, false, true);           // charging
  assert(s.apply(9200, true, true) == 10000);  // done, latched
  assert(s.learned());
  assert(s.fullCentiPercent() == 9200);
  // Unplugged: back to the scaled reading, which starts at 100%.
  assert(s.apply(9200, false, false) == 10000);
  assert(s.apply(4600, false, false) == 5000);
}

void testHighTerminationScalesDown() {
  BatterySocScaler s;
  s.reset();
  s.apply(10000, false, true);
  s.apply(10800, true, true);
  assert(s.fullCentiPercent() == 10800);
  assert(s.apply(10800, false, false) == 10000);
  assert(s.apply(5400, false, false) == 5000);
}

void testOutOfWindowTerminationIsIgnored() {
  BatterySocScaler s;
  s.reset();
  s.apply(8000, false, true);
  s.apply(8500, true, true);
  assert(!s.learned());
  s.apply(11500, false, true);
  s.apply(11500, true, true);
  assert(!s.learned());
  assert(s.apply(8500, false, false) == 8500);
}

void testLearningNeedsARealTransition() {
  BatterySocScaler s;
  s.reset();
  // First sample after a reset is already charge_done: no learning.
  assert(s.apply(9300, true, true) == 10000);
  assert(!s.learned());
  // Still latched while plugged, even through a top-up recharge.
  assert(s.apply(9250, false, true) == 10000);
  // That recharge armed it: the next termination is learned.
  s.apply(9400, true, true);
  assert(s.learned());
  assert(s.fullCentiPercent() == 9400);
}

void testLaterTerminationsAverage() {
  BatterySocScaler s;
  s.reset();
  s.apply(9000, false, true);
  s.apply(9200, true, true);
  s.apply(9000, false, false);
  s.apply(9000, false, true);
  s.apply(9600, true, true);
  assert(s.fullCentiPercent() == 9400);
}

void testResetDropsLearningAndLatch() {
  BatterySocScaler s;
  s.reset();
  s.apply(9000, false, true);
  s.apply(9200, true, true);
  assert(s.learned() && s.fullLatched());
  s.reset();
  assert(!s.learned());
  assert(!s.fullLatched());
  assert(s.fullCentiPercent() == 10000);
  assert(s.apply(9200, false, true) == 9200);
}

}  // namespace

int main() {
  testUnlearnedClampsRawToHundred();
  testLowTerminationScalesUp();
  testHighTerminationScalesDown();
  testOutOfWindowTerminationIsIgnored();
  testLearningNeedsARealTransition();
  testLaterTerminationsAverage();
  testResetDropsLearningAndLatch();
  std::cout << "Battery SoC scaler tests passed\n";
  return 0;
}
