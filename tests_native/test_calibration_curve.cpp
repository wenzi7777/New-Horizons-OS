#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "CalibrationCurve.h"

namespace {

namespace cc = nhos::calibration_curve;

bool near(float a, float b, float eps = 0.01f) {
  return std::fabs(a - b) <= eps;
}

// One sensor, two pressure levels captured over a 300 mV baseline:
// 10 kPa at 500 mV and 20 kPa at 900 mV, stored relative to that baseline.
cc::Curve twoLevelCurve() {
  const float levels[] = {10.0f, 20.0f};
  const float relative[] = {200.0f, 600.0f};
  cc::Curve curve;
  cc::buildCurve(levels, relative, 2, curve);
  return curve;
}

void testCurvePassesThroughCapturedLevelsAndIsAnchoredAtZero() {
  const cc::Curve curve = twoLevelCurve();
  float out = -1;
  assert(cc::evaluateCurve(curve, 0.0f, out) && near(out, 0.0f));
  assert(cc::evaluateCurve(curve, 200.0f, out) && near(out, 10.0f));
  assert(cc::evaluateCurve(curve, 600.0f, out) && near(out, 20.0f));
  // Clamped beyond the highest captured level.
  assert(cc::evaluateCurve(curve, 5000.0f, out) && near(out, 20.0f));
  // Monotone in between.
  float lower = 0, upper = 0;
  cc::evaluateCurve(curve, 300.0f, lower);
  cc::evaluateCurve(curve, 400.0f, upper);
  assert(lower > 10.0f && upper > lower && upper < 20.0f);
}

void testCurveSkipsMissingAndNonPositivePoints() {
  const float levels[] = {5.0f, 10.0f, 20.0f};
  const float relative[] = {NAN, -3.0f, 400.0f};
  cc::Curve curve;
  cc::buildCurve(levels, relative, 3, curve);
  assert(curve.raws.size() == 2);  // (0,0) anchor + the one usable point
  float out = 0;
  assert(cc::evaluateCurve(curve, 400.0f, out) && near(out, 20.0f));
}

void testEmptyCurveDoesNotEvaluate() {
  cc::Curve curve;
  float out = 42.0f;
  assert(!cc::evaluateCurve(curve, 100.0f, out));
  assert(out == 42.0f);
}

void testRezeroShiftsOnlyTheZeroPoint() {
  // Baseline drifts from 300 to 350 mV, then the operator zeroes again. The
  // same 10 kPa load now reads 550 mV; with the new tare it must still map to
  // 10 kPa. Absolute storage (the pre-v1.5.1 model) gave 7.5 kPa here.
  const cc::Curve curve = twoLevelCurve();
  const float newTare = 350.0f;
  float out = 0;
  assert(cc::evaluateCurve(curve, cc::applyTare(550.0f, newTare), out));
  assert(near(out, 10.0f));
}

void testApplyTareClampsAtZeroAndIgnoresMissingTare() {
  assert(near(cc::applyTare(320.0f, 300.0f), 20.0f));
  assert(near(cc::applyTare(290.0f, 300.0f), 0.0f));
  assert(near(cc::applyTare(290.0f, NAN), 290.0f));
}

void testOutputModeResolution() {
  using cc::OutputMode;
  // Calibrated wins only when enabled and ready.
  assert(cc::resolveOutputMode(true, true, true, true) == OutputMode::Calibrated);
  assert(cc::resolveOutputMode(true, false, true, true) == OutputMode::Tared);
  // A stand-alone tare applies without any pressure calibration.
  assert(cc::resolveOutputMode(false, false, true, true) == OutputMode::Tared);
  // Nothing is applied until the operator zeroes: upgrades change no output.
  assert(cc::resolveOutputMode(false, false, false, true) == OutputMode::Raw);
  assert(cc::resolveOutputMode(false, false, true, false) == OutputMode::Raw);
  assert(std::string(cc::outputModeName(OutputMode::Tared)) == "tared");
}

void testLegacyAbsoluteLevelsConvertAgainstTheirTare() {
  std::vector<float> level = {500.0f, 900.0f, NAN};
  const std::vector<float> tare = {300.0f, NAN, 100.0f};
  cc::toRelative(level, tare);
  assert(near(level[0], 200.0f));
  assert(std::isnan(level[1]));  // no tare for this cell: unusable
  assert(std::isnan(level[2]));  // was never captured
}

}  // namespace

int main() {
  testCurvePassesThroughCapturedLevelsAndIsAnchoredAtZero();
  testCurveSkipsMissingAndNonPositivePoints();
  testEmptyCurveDoesNotEvaluate();
  testRezeroShiftsOnlyTheZeroPoint();
  testApplyTareClampsAtZeroAndIgnoresMissingTare();
  testOutputModeResolution();
  testLegacyAbsoluteLevelsConvertAgainstTheirTare();
  std::cout << "Calibration curve tests passed\n";
  return 0;
}
