#pragma once

#include <stdint.h>

namespace nhos {

// The arithmetic the v1.6.0 flow ops share with the App Library's
// sdk/lib/flowmath.mjs. No Arduino dependency on purpose: the library's
// firmware-contract test compiles this file on a desktop and checks it value
// for value against the JavaScript, so the simulator and the device cannot
// quietly disagree about which cells a region covers or what angle a tilt is.

// `rel` bits on a region node: rows and/or columns given in percent.
static constexpr uint8_t kRegionRelRows = 1;
static constexpr uint8_t kRegionRelCols = 2;
static constexpr uint8_t kMaxRegionPercent = 100;

// imu(field) / mag(field), in wire order. Mirrored as IMU_FIELDS / MAG_FIELDS.
enum class ImuField : uint8_t {
  Ax = 0, Ay, Az, Gx, Gy, Gz, AccMag, GyroMag, Pitch, Roll,
  Count,
};
enum class MagField : uint8_t {
  Mx = 0, My, Mz, Strength, Heading,
  Count,
};

struct FlowSpan {
  uint16_t lo;
  uint16_t hi;  // an empty span has hi < lo
};

// The rows (or columns) a region spans on a matrix `count` long. Absolute
// bounds pass through (the sweep clips them); relative ones are percentages:
// first = floor(a% of count), last = ceil(b% of count) - 1, never empty on a
// non-empty matrix. `0..50` and `50..100` on an odd count share the middle
// row, which keeps a left/right split symmetric.
FlowSpan flowResolveSpan(uint8_t a, uint8_t b, bool relative, uint16_t count);

float flowSqrt(float x);           // 0 for x <= 0
float flowAtan2Deg(float y, float x);  // -180..180

// `sample` is ax, ay, az (g), gx, gy, gz (deg/s).
float flowImuField(const float* sample, uint8_t field);
// `sample` is mx, my, mz (microtesla).
float flowMagField(const float* sample, uint8_t field);

}  // namespace nhos
