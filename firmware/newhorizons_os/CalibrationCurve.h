#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

// The arithmetic half of Calibration, kept free of Arduino so the host-native
// tests can pin it. It is the model the E-Textiles 2026 paper fits per sensor
// (etextile-2026-paper/revision/make_figures.py):
//
//   x = G(raw - tare)            the readout curve V = V0 + c1*G + c2*G^2,
//                                inverted, with the tare as V0
//   p = a + b*sqrt(x) + c*x      least squares over the sensor's levels
//
// With the default readout (c1 = 1, c2 = 0) x is the tare-relative mV itself,
// so a board whose readout curve is unknown still gets the same model shape.
//
// Levels are stored RELATIVE to the tare that was active when they were
// captured (raw - tare, in mV). That is what lets a later re-zero move only
// the zero point: the live input is raw - currentTare, and the fitted model
// never changes.

namespace nhos {
namespace calibration_curve {

enum class OutputMode : unsigned char {
  Raw = 0,         // streamed as measured
  Tared = 1,       // max(0, raw - tare): zeroed, still in mV
  Calibrated = 2,  // through the per-sensor model
};

const char* outputModeName(OutputMode mode);

OutputMode resolveOutputMode(bool calibrationEnabled, bool calibrationReady, bool tareEnabled, bool tareComplete);

// max(0, raw - tare); a NaN tare leaves the reading untouched.
float applyTare(float raw, float tare);

// The readout curve of one ADC: tare-relative mV = c1*x + c2*x^2.
struct Readout {
  float c1 = 1.0f;
  float c2 = 0.0f;
};

// x for a tare-relative reading: the root of c1*x + c2*x^2 = relativeMv
// nearest zero, in the form 2*dV / (c1 + sqrt(c1^2 + 4*c2*dV)), which stays
// exact as c2 -> 0 (x = dV / c1). 0 for dV <= 0. Past the curve's turning
// point no conductance gives that voltage; like the paper's mv2g (which
// clips the discriminant at 0) it then returns the turning point itself.
float readoutX(float relativeMv, const Readout& readout);

struct FitModel {
  float a = NAN;
  float b = NAN;
  float c = NAN;
};

bool fitValid(const FitModel& model);

// The model has three coefficients, so a sensor needs this many levels.
constexpr size_t kMinFitPoints = 3;

// Least squares of p = a + b*sqrt(x) + c*x over (xs[i], pressures[i]), the
// same fit as numpy.polyfit(sqrt(x), p, 2). Callers drop unusable points
// (x <= 0 or NaN) first. False -- leaving `out` invalid -- with fewer than
// kMinFitPoints distinct x or a singular system.
bool fitModel(const float* xs, const float* pressures, size_t count, FitModel& out);

// p at x: 0 for x <= 0 and never negative. Above the captured range the
// polynomial carries on, as the paper's fit does.
float evaluateModel(const FitModel& model, float x);

// A capture is split into blocks of about kCaptureBlockMs; each block is
// averaged and the capture reports the median of those block means. The
// paper takes the median of every frame in its window, which needs far more
// RAM than the device has (10 s x 60 fps x 225 cells); block means keep the
// median's robustness to the valve's pressure pulses at a fraction of it.
constexpr uint32_t kCaptureBlockMs = 500;
constexpr uint32_t kMaxCaptureBlocks = 40;
uint32_t captureBlockCount(uint32_t durationMs);

// Median of values[0..count), reordering them. Even counts average the two
// middle values. NaN for count 0.
float medianInPlace(float* values, size_t count);

}  // namespace calibration_curve
}  // namespace nhos
