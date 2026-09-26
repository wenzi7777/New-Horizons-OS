#include "FlowMath.h"

#include <math.h>

namespace nhos {
namespace {

constexpr float kRadToDeg = 57.29578f;

}  // namespace

FlowSpan flowResolveSpan(uint8_t a, uint8_t b, bool relative, uint16_t count) {
  if (!relative) {
    return {a, b};
  }
  if (count == 0) {
    return {1, 0};
  }
  const uint32_t pa = a > kMaxRegionPercent ? kMaxRegionPercent : a;
  const uint32_t pb = b > kMaxRegionPercent ? kMaxRegionPercent : b;
  uint32_t lo = (pa * count) / 100;
  int32_t hi = static_cast<int32_t>((pb * count + 99) / 100) - 1;
  if (lo > static_cast<uint32_t>(count - 1)) lo = count - 1;
  if (hi > static_cast<int32_t>(count - 1)) hi = count - 1;
  if (hi < static_cast<int32_t>(lo)) hi = static_cast<int32_t>(lo);
  return {static_cast<uint16_t>(lo), static_cast<uint16_t>(hi)};
}

float flowSqrt(float x) { return x > 0 ? sqrtf(x) : 0.0f; }

float flowAtan2Deg(float y, float x) { return atan2f(y, x) * kRadToDeg; }

float flowImuField(const float* s, uint8_t field) {
  const float ax = s[0];
  const float ay = s[1];
  const float az = s[2];
  switch (static_cast<ImuField>(field)) {
    case ImuField::Ax: return ax;
    case ImuField::Ay: return ay;
    case ImuField::Az: return az;
    case ImuField::Gx: return s[3];
    case ImuField::Gy: return s[4];
    case ImuField::Gz: return s[5];
    case ImuField::AccMag: return flowSqrt(ax * ax + ay * ay + az * az);
    case ImuField::GyroMag: return flowSqrt(s[3] * s[3] + s[4] * s[4] + s[5] * s[5]);
    // Tilt from gravity alone, so only meaningful while the board is not
    // accelerating: pitch about y, roll about x, in the board's own axes.
    case ImuField::Pitch: return flowAtan2Deg(-ax, flowSqrt(ay * ay + az * az));
    case ImuField::Roll: return flowAtan2Deg(ay, az);
    default: return 0.0f;
  }
}

float flowMagField(const float* s, uint8_t field) {
  const float mx = s[0];
  const float my = s[1];
  const float mz = s[2];
  switch (static_cast<MagField>(field)) {
    case MagField::Mx: return mx;
    case MagField::My: return my;
    case MagField::Mz: return mz;
    case MagField::Strength: return flowSqrt(mx * mx + my * my + mz * mz);
    // Not tilt-compensated: the horizontal field's angle in the board's x-y
    // plane, 0 to 360. Only a compass while the board lies flat.
    case MagField::Heading: {
      const float angle = flowAtan2Deg(my, mx);
      return angle < 0 ? angle + 360.0f : angle;
    }
    default: return 0.0f;
  }
}

}  // namespace nhos
