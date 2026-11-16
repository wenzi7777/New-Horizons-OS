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

float conductanceUs(float voutMv, float vrefMv, float rfbOhm, float offsetMv) {
  if (!(rfbOhm > 0.0f) || !(vrefMv > 0.0f)) {
    return 0.0f;
  }
  const float signal = voutMv - (vrefMv + offsetMv);
  if (!(signal > 0.0f)) {
    return 0.0f;
  }
  // mV / (mV * ohm) = siemens; x 1e6 for microsiemens.
  return signal / (vrefMv * rfbOhm) * 1.0e6f;
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

float autoRangeNextVrefMv(const AutoRangeInput& in) {
  const float current = in.currentVrefMv;
  if (in.saturatedRows > 0) {
    const float next = current * kAutoRangeLowerFactor;
    return next < in.floorMv ? in.floorMv : next;
  }
  if (current > in.ceilingMv) {
    return in.ceilingMv;
  }
  if (in.quietMs < kAutoRangeRaiseHoldMs || in.windowMs < kAutoRangeRaiseHoldMs ||
      !(in.windowPeakMv > 0.0f) || !(current > 0.0f)) {
    return current;
  }
  float next = current * (kAutoRangeTargetPeakMv / in.windowPeakMv);
  if (!(next >= current * kAutoRangeMinRaiseFactor)) {
    return current;  // already near (or past) the target: hold
  }
  const float maxStep = current * kAutoRangeMaxRaiseFactor;
  next = next > maxStep ? maxStep : next;
  return next > in.ceilingMv ? in.ceilingMv : next;
}

float autoRangeCeilingMv(float limitMv, float floorMv) {
  float ceiling = limitMv < kAutoRangeMaxVrefMv ? limitMv : kAutoRangeMaxVrefMv;
  return ceiling < floorMv ? floorMv : ceiling;
}

float rfbFitTopMv(float biggestRfbOhm, float limitMv, float strapCeilingMv) {
  float top = biggestRfbOhm > 0.0f ? 2600.0f / (1.0f + biggestRfbOhm / kCalResistorOhm) : 2600.0f;
  top = top > limitMv ? limitMv : top;
  top = top > strapCeilingMv ? strapCeilingMv : top;
  return top < 0.0f ? 0.0f : top;
}

size_t encodeRangeTlv(bool conductance, bool autoRange, uint16_t vrefMv, uint32_t saturatedRows,
                      uint8_t* out, size_t capacity) {
  const size_t total = kPacketExtensionTlvHeaderLen + kRangeTlvLen;
  if (out == nullptr || capacity < total) {
    return 0;
  }
  out[0] = kPacketExtRange;
  out[1] = static_cast<uint8_t>(kRangeTlvLen);
  out[2] = conductance ? kRangeUnitUs : kRangeUnitMv;
  out[3] = autoRange ? kRangeFlagAuto : 0;
  out[4] = static_cast<uint8_t>(vrefMv & 0xFFU);
  out[5] = static_cast<uint8_t>(vrefMv >> 8);
  for (int i = 0; i < 4; ++i) {
    out[6 + i] = static_cast<uint8_t>((saturatedRows >> (8 * i)) & 0xFFU);
  }
  return total;
}

}  // namespace nhos
