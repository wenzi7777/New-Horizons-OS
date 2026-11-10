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

void testFit() {
  // The measured board (ADC1): off = 1.002*V - 9.96, on = 3.0102*V + 7.62 for a
  // 20k Rfb. The single-point value at 330 mV was 2.5 % high; the fit is not.
  const float gOff = 1.002f, eOff = -9.96f, gOn = 3.0102f, eOn = 7.62f;
  const float v[5] = {200.0f, 350.0f, 500.0f, 650.0f, 800.0f};
  float off[5], on[5];
  for (int i = 0; i < 5; ++i) {
    off[i] = gOff * v[i] + eOff;
    on[i] = gOn * v[i] + eOn;
  }
  float offset = 0.0f;
  assert(near(rfbFromPoints(v, off, on, 5, &offset), 20042.0f, 40.0f));
  assert(near(offset, -9.96f + 0.002f * 500.0f, 0.5f));
  assert(rfbFromCalibration(gOn * 330 + eOn, gOff * 330 + eOff) > 20400.0f);
  // Scatter of a few mV on the ends does not move it much (the reason for five points).
  float noisyOff[5], noisyOn[5];
  const float jitter[5] = {4.0f, -3.0f, 2.0f, -5.0f, 6.0f};
  for (int i = 0; i < 5; ++i) {
    noisyOff[i] = off[i] + jitter[i];
    noisyOn[i] = on[i] - jitter[i] * 1.5f;
  }
  const float noisy = rfbFromPoints(v, noisyOff, noisyOn, 5, nullptr);
  assert(near(noisy, 20000.0f, 600.0f));
  // Ideal ADC: exact.
  const float ideal[3] = {100.0f, 300.0f, 500.0f};
  const float idealOn[3] = {300.0f, 900.0f, 1500.0f};
  assert(near(rfbFromPoints(ideal, ideal, idealOn, 3, nullptr), 20000.0f, 1.0f));
  // Unusable: too few, too close, a clipped point, no gain.
  assert(rfbFromPoints(ideal, ideal, idealOn, 1, nullptr) == 0.0f);
  const float close[2] = {300.0f, 340.0f};
  const float closeOn[2] = {900.0f, 1020.0f};
  assert(rfbFromPoints(close, close, closeOn, 2, nullptr) == 0.0f);
  const float railOn[3] = {300.0f, 900.0f, 3100.0f};
  assert(rfbFromPoints(ideal, ideal, railOn, 3, nullptr) == 0.0f);
  assert(rfbFromPoints(ideal, ideal, ideal, 3, nullptr) == 0.0f);
}

void testOffsets() {
  // A row whose idle reading sits 9 mV above Vref: that is where "no signal" is.
  assert(near(virtualizeMv(339.0f, 330.0f, 20000.0f, 330.0f, 20000.0f, 9.0f), 330.0f, 0.01f));
  assert(near(virtualizeMv(339.0f + 660.0f, 330.0f, 20000.0f, 330.0f, 20000.0f, 9.0f), 990.0f, 0.5f));
  assert(leakMv(339.0f, 330.0f, 20000.0f, kLeakResistorOhm, 9.0f) == 0.0f);
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
  testFit();
  testOffsets();
  testSaturation();
  testAutoRange();
  std::cout << "range math tests passed\n";
  return 0;
}
