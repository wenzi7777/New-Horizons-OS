#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

#include "RangeMath.h"

using namespace nhos;

namespace {

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

void testRfbFromCalibration() {
  // Rfb 20k at Vref 330 mV: Vout = 330 * (1 + 20k/10k) = 990 mV.
  assert(near(rfbFromCalibration(990.0f, 330.0f), 20000.0f, 1.0f));
  assert(near(rfbFromCalibration(330.0f * 1.5f, 330.0f), 5000.0f, 1.0f));  // Rfb 5k
  // Not fitted: output stays at Vref.
  assert(rfbFromCalibration(330.0f, 330.0f) == 0.0f);
  assert(rfbFromCalibration(333.0f, 330.0f) == 0.0f);  // inside the 2 % margin
  // Rfb too big for this Vref: the row saturates.
  assert(rfbFromCalibration(3100.0f, 330.0f) == 0.0f);
  assert(rfbFromCalibration(0.0f, 330.0f) == 0.0f);
  assert(rfbFromCalibration(990.0f, 0.0f) == 0.0f);
}

void testVirtualize() {
  // Same Vref and Rfb: the identity.
  assert(near(virtualizeMv(1200.0f, 330.0f, 20000.0f, 330.0f, 20000.0f), 1200.0f, 0.01f));
  // Rs = 10k at Vref 330, Rfb 5k: Vout = 330 * 1.5 = 495. As the 20k board
  // (Vref 330) would read it: 330 * (1 + 20k/10k) = 990.
  assert(near(virtualizeMv(495.0f, 330.0f, 5000.0f, 330.0f, 20000.0f), 990.0f, 0.5f));
  // Halving Vref with the same Rfb: Rs = 10k, Rfb 20k, Vref 165 -> 495; to 330/20k -> 990.
  assert(near(virtualizeMv(495.0f, 165.0f, 20000.0f, 330.0f, 20000.0f), 990.0f, 0.5f));
  // No signal maps to the reference baseline; unknown Rfb passes through.
  assert(near(virtualizeMv(330.0f, 330.0f, 5000.0f, 330.0f, 20000.0f), 330.0f, 0.01f));
  assert(near(virtualizeMv(300.0f, 330.0f, 5000.0f, 330.0f, 20000.0f), 330.0f, 0.01f));
  assert(near(virtualizeMv(777.0f, 330.0f, 0.0f, 330.0f, 20000.0f), 777.0f, 0.01f));
}

void testLeak() {
  // Column 13 shorted (Rs13 -> 0): leak = Vref * Rfb / 100k = 0.2 Vref for 20k.
  // A reading of Vref * (1 + 20k / 1) is far past the rail, so use a large V13.
  assert(near(leakMv(330.0f * (1.0f + 20000.0f / 10.0f), 330.0f, 20000.0f), 66.0f, 0.5f));
  // Rs13 = 100k, Rfb 20k, Vref 330: V13 = 330 * 1.2 = 396; leak = 330*20k/200k = 33.
  assert(near(leakMv(396.0f, 330.0f, 20000.0f), 33.0f, 0.2f));
  // Open sensor on column 13: no leak. Unknown Rfb: none.
  assert(leakMv(330.0f, 330.0f, 20000.0f) == 0.0f);
  assert(leakMv(396.0f, 330.0f, 0.0f) == 0.0f);
  // The correction removes the leak from a real reading.
  const float vref = 330.0f, rfb = 20000.0f;
  const float rsActive = 10000.0f, rs13 = 100000.0f;
  const float trueV = vref * (1.0f + rfb / rsActive);                       // 990
  const float measured = trueV + vref * rfb / (rs13 + kLeakResistorOhm);    // + leak
  const float v13 = vref * (1.0f + rfb / rs13);
  assert(near(measured - leakMv(v13, vref, rfb), trueV, 0.5f));
}

void testSaturation() {
  assert(!isSaturatedMv(2999.0f));
  assert(isSaturatedMv(3000.0f));
  assert(isSaturatedMv(4983.0f));
}

void testAutoRange() {
  // A clipped row lowers Vref by 20 %, never below the floor.
  assert(near(autoRangeNextVrefMv(330.0f, 3, 0, 330.0f, 100.0f), 264.0f, 0.01f));
  assert(near(autoRangeNextVrefMv(110.0f, 1, 0, 330.0f, 100.0f), 100.0f, 0.01f));
  assert(near(autoRangeNextVrefMv(100.0f, 1, 0, 330.0f, 100.0f), 100.0f, 0.01f));
  // It waits 20 s of quiet before creeping back, and never passes the safe value.
  assert(near(autoRangeNextVrefMv(264.0f, 0, 19999, 330.0f, 100.0f), 264.0f, 0.01f));
  assert(near(autoRangeNextVrefMv(264.0f, 0, 20000, 330.0f, 100.0f), 290.4f, 0.1f));
  assert(near(autoRangeNextVrefMv(320.0f, 0, 60000, 330.0f, 100.0f), 330.0f, 0.01f));
  assert(near(autoRangeNextVrefMv(330.0f, 0, 60000, 330.0f, 100.0f), 330.0f, 0.01f));
}

}  // namespace

int main() {
  testRfbFromCalibration();
  testVirtualize();
  testLeak();
  testSaturation();
  testAutoRange();
  std::cout << "range math tests passed\n";
  return 0;
}
