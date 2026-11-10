#include "RangeMath.h"

namespace nhos {

float rfbFromCalibration(float calOnMv, float vrefMv) {
  if (!(vrefMv > 50.0f) || !(calOnMv > 0.0f)) {
    return 0.0f;
  }
  // Nothing fitted: the output sits at Vref (2 % margin for noise and offset).
  if (calOnMv < vrefMv * 1.02f) {
    return 0.0f;
  }
  if (isSaturatedMv(calOnMv)) {
    return 0.0f;
  }
  return kCalResistorOhm * (calOnMv / vrefMv - 1.0f);
}

float virtualizeMv(float voutMv, float vrefMv, float rfbOhm, float refVrefMv, float refRfbOhm) {
  if (!(rfbOhm > 0.0f) || !(vrefMv > 0.0f) || !(refRfbOhm > 0.0f) || !(refVrefMv > 0.0f)) {
    return voutMv;
  }
  const float signal = voutMv - vrefMv;
  if (signal <= 0.0f) {
    return refVrefMv;
  }
  return refVrefMv + signal * (refVrefMv * refRfbOhm) / (vrefMv * rfbOhm);
}

float leakMv(float v13Mv, float vrefMv, float rfbOhm, float leakOhm) {
  if (!(rfbOhm > 0.0f) || !(vrefMv > 0.0f) || !(leakOhm > 0.0f)) {
    return 0.0f;
  }
  const float signal = v13Mv - vrefMv;
  if (signal <= 0.0f) {
    return 0.0f;  // column 13's sensor is open: it leaks nothing
  }
  const float rs13 = rfbOhm * vrefMv / signal;
  return vrefMv * rfbOhm / (rs13 + leakOhm);
}

float autoRangeNextVrefMv(float currentVrefMv, size_t saturatedRows, uint32_t quietMs,
                          float safeVrefMv, float minVrefMv) {
  if (saturatedRows > 0) {
    float next = currentVrefMv * 0.8f;
    return next < minVrefMv ? minVrefMv : next;
  }
  // Quiet for 20 s: walk back toward the safe value.
  if (quietMs >= 20000 && currentVrefMv < safeVrefMv) {
    float next = currentVrefMv * 1.1f;
    return next > safeVrefMv ? safeVrefMv : next;
  }
  return currentVrefMv;
}

}  // namespace nhos
