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

float virtualizeMv(float voutMv, float vrefMv, float rfbOhm, float refVrefMv, float refRfbOhm,
                   float offsetMv) {
  if (!(rfbOhm > 0.0f) || !(vrefMv > 0.0f) || !(refRfbOhm > 0.0f) || !(refVrefMv > 0.0f)) {
    return voutMv;
  }
  const float signal = voutMv - (vrefMv + offsetMv);
  if (signal <= 0.0f) {
    return refVrefMv;
  }
  return refVrefMv + signal * (refVrefMv * refRfbOhm) / (vrefMv * rfbOhm);
}

float leakMv(float v13Mv, float vrefMv, float rfbOhm, float leakOhm, float offsetMv) {
  if (!(rfbOhm > 0.0f) || !(vrefMv > 0.0f) || !(leakOhm > 0.0f)) {
    return 0.0f;
  }
  const float signal = v13Mv - (vrefMv + offsetMv);
  if (signal <= 0.0f) {
    return 0.0f;  // column 13's sensor is open: it leaks nothing
  }
  const float rs13 = rfbOhm * vrefMv / signal;
  return vrefMv * rfbOhm / (rs13 + leakOhm);
}

float rfbFromPoints(const float* vrefMv, const float* offMv, const float* onMv, size_t count,
                    float* offsetMv) {
  if (offsetMv) {
    *offsetMv = 0.0f;
  }
  if (!vrefMv || !offMv || !onMv || count < 2) {
    return 0.0f;
  }
  float minV = vrefMv[0], maxV = vrefMv[0];
  float meanV = 0.0f, meanOff = 0.0f, meanOn = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    if (isSaturatedMv(onMv[i]) || isSaturatedMv(offMv[i])) {
      return 0.0f;
    }
    minV = vrefMv[i] < minV ? vrefMv[i] : minV;
    maxV = vrefMv[i] > maxV ? vrefMv[i] : maxV;
    meanV += vrefMv[i];
    meanOff += offMv[i];
    meanOn += onMv[i];
  }
  if (maxV - minV < 100.0f) {
    return 0.0f;
  }
  const float n = static_cast<float>(count);
  meanV /= n;
  meanOff /= n;
  meanOn /= n;
  float sxx = 0.0f, sxyOff = 0.0f, sxyOn = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    const float dx = vrefMv[i] - meanV;
    sxx += dx * dx;
    sxyOff += dx * (offMv[i] - meanOff);
    sxyOn += dx * (onMv[i] - meanOn);
  }
  if (!(sxx > 0.0f) || !(sxyOff > 0.0f)) {
    return 0.0f;
  }
  const float ratio = sxyOn / sxyOff;
  if (!(ratio > 1.02f)) {
    return 0.0f;  // no gain above unity: nothing fitted
  }
  if (offsetMv) {
    float e = 0.0f;
    for (size_t i = 0; i < count; ++i) {
      e += offMv[i] - vrefMv[i];
    }
    *offsetMv = e / n;
  }
  return kCalResistorOhm * (ratio - 1.0f);
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
