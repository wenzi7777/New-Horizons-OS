#pragma once

#include <cstddef>
#include <vector>

// The arithmetic half of Calibration, kept free of Arduino so the host-native
// tests can pin it: how a sensor's captured pressure levels become a curve,
// how a live reading is mapped through it, and which output a device streams.
//
// Levels are stored RELATIVE to the tare that was active when they were
// captured (raw - tare, in mV). That is what lets a later re-zero move only
// the zero point: the live input is raw - currentTare, the curve's x-points
// never change, so a baseline drift no longer skews an existing calibration.

namespace nhos {
namespace calibration_curve {

struct Curve {
  std::vector<float> raws;      // tare-relative mV, ascending, starting at 0
  std::vector<float> levels;    // calibrated value at each raw (e.g. kPa)
  std::vector<float> tangents;  // monotone-cubic (Fritsch-Carlson) tangents
};

enum class OutputMode : unsigned char {
  Raw = 0,         // streamed as measured
  Tared = 1,       // max(0, raw - tare): zeroed, still in mV
  Calibrated = 2,  // through the per-sensor curve
};

const char* outputModeName(OutputMode mode);

OutputMode resolveOutputMode(bool calibrationEnabled, bool calibrationReady, bool tareEnabled, bool tareComplete);

// max(0, raw - tare); a NaN tare leaves the reading untouched.
float applyTare(float raw, float tare);

// Builds one sensor's curve from the (level, relativeRaw) pairs it has.
// Pairs whose relativeRaw is NaN or not above zero are skipped, and the curve
// is anchored at (0, 0). `count` is the length of both arrays.
void buildCurve(const float* levels, const float* relativeRaws, size_t count, Curve& out);

// Maps a tare-relative reading through the curve. Returns false (and leaves
// `out` alone) when the curve has no points.
bool evaluateCurve(const Curve& curve, float relativeRaw, float& out);

// Converts a legacy level captured as absolute mV into tare-relative mV, cell
// by cell. A cell without a tare cannot be converted and becomes NaN.
void toRelative(std::vector<float>& levelValues, const std::vector<float>& tare);

}  // namespace calibration_curve
}  // namespace nhos
