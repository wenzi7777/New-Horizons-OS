#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "CalibrationCurve.h"

namespace {

namespace cc = nhos::calibration_curve;

bool near(double a, double b, double eps) {
  return std::fabs(a - b) <= eps;
}

bool nearRel(double a, double b, double rel) {
  return std::fabs(a - b) <= rel * std::max(std::fabs(a), std::fabs(b));
}

// Real data from the E-Textiles 2026 paper: installation #6 (L3I8), upward
// sweep, levels 3-20 kPa as make_figures.py fits them. G in uS, p is the
// measured bladder gauge pressure. Expected coefficients are numpy's
// polyfit(sqrt(G), p, 2) on exactly these values.
const std::vector<float> kPressures = {2.876f, 3.907f, 4.831f, 5.827f, 6.783f, 7.796f, 8.852f, 10.229f,
                                       11.622f, 13.137f, 14.63f, 16.129f, 17.438f, 18.589f, 19.431f};
const std::vector<float> kCell16G = {9.538f, 21.654f, 28.428f, 31.469f, 43.31f, 54.169f, 56.938f, 56.703f,
                                     112.221f, 141.675f, 184.318f, 214.588f, 250.075f, 292.167f, 296.691f};
const std::vector<float> kCell25G = {13.901f, 16.221f, 23.167f, 31.574f, 46.096f, 52.353f, 53.336f, 88.61f,
                                     128.622f, 263.423f, 272.847f, 355.917f, 242.413f, 266.995f, 471.315f};
// The paper's ADC1 readout curve (analysis/r01b_adc1_fit.json, Rfb 8.2 kOhm).
const cc::Readout kPaperAdc1 = {2.6656778887724184f, -9.447395342319497e-05f};

void testFitMatchesNumpyPolyfitOnPaperData() {
  cc::FitModel model;
  assert(cc::fitModel(kCell16G.data(), kPressures.data(), kPressures.size(), model));
  assert(nearRel(model.a, -1.770854238137579, 1e-4));
  assert(nearRel(model.b, 1.4399544897910037, 1e-4));
  assert(nearRel(model.c, -0.014086035677100647, 1e-4));

  assert(cc::fitModel(kCell25G.data(), kPressures.data(), kPressures.size(), model));
  assert(nearRel(model.a, -2.0154277181354625, 1e-4));
  assert(nearRel(model.b, 1.5498622477543933, 1e-4));
  assert(nearRel(model.c, -0.027591859844725553, 1e-4));
}

void testReadoutInvertsThePaperCurve() {
  // dV = c1*G + c2*G^2 on the device; readoutX must give G back.
  for (float g : kCell25G) {
    const float dv = kPaperAdc1.c1 * g + kPaperAdc1.c2 * g * g;
    assert(nearRel(cc::readoutX(dv, kPaperAdc1), g, 1e-4));
  }
  // The default readout is the identity: x is the tare-relative mV.
  assert(near(cc::readoutX(123.5f, cc::Readout()), 123.5, 1e-4));
  assert(cc::readoutX(0.0f, kPaperAdc1) == 0.0f);
  assert(cc::readoutX(-5.0f, kPaperAdc1) == 0.0f);
  assert(std::isnan(cc::readoutX(NAN, kPaperAdc1)));
  // Past the parabola's turning point (c1^2 + 4*c2*dV < 0) no G exists; like
  // the paper's mv2g it returns the turning point -c1 / (2*c2).
  assert(nearRel(cc::readoutX(20000.0f, kPaperAdc1), -kPaperAdc1.c1 / (2.0 * kPaperAdc1.c2), 1e-5));
}

void testEndToEndFromMillivoltsMatchesThePaper() {
  // What the device actually holds: tare-relative mV. Through the readout
  // curve and the fit it must land on the paper's coefficients.
  std::vector<float> xs;
  for (float g : kCell16G) {
    xs.push_back(cc::readoutX(kPaperAdc1.c1 * g + kPaperAdc1.c2 * g * g, kPaperAdc1));
  }
  cc::FitModel model;
  assert(cc::fitModel(xs.data(), kPressures.data(), kPressures.size(), model));
  assert(nearRel(model.a, -1.770854238137579, 1e-3));
  assert(nearRel(model.b, 1.4399544897910037, 1e-3));
  assert(nearRel(model.c, -0.014086035677100647, 1e-3));
  // And evaluating at a captured point reproduces the fitted value.
  const double expected = -1.770854238137579 + 1.4399544897910037 * std::sqrt(184.318) -
                          0.014086035677100647 * 184.318;
  assert(near(cc::evaluateModel(model, xs[10]), expected, 1e-2));
}

void testFitNeedsThreeDistinctPoints() {
  cc::FitModel model;
  const float x2[] = {10.0f, 40.0f};
  const float p2[] = {5.0f, 10.0f};
  assert(!cc::fitModel(x2, p2, 2, model));
  assert(!cc::fitValid(model));
  // Three points but only two distinct x: singular.
  const float x3[] = {10.0f, 10.0f, 40.0f};
  const float p3[] = {5.0f, 5.5f, 10.0f};
  assert(!cc::fitModel(x3, p3, 3, model));
  // Points with x <= 0 or NaN do not count.
  const float x4[] = {0.0f, NAN, 10.0f, 40.0f};
  const float p4[] = {1.0f, 2.0f, 5.0f, 10.0f};
  assert(!cc::fitModel(x4, p4, 4, model));
  // Exactly three distinct points fit exactly.
  const float x5[] = {4.0f, 16.0f, 64.0f};
  const float p5[] = {1.0f + 2.0f * 2.0f + 0.5f * 4.0f, 1.0f + 2.0f * 4.0f + 0.5f * 16.0f, 1.0f + 2.0f * 8.0f + 0.5f * 64.0f};
  assert(cc::fitModel(x5, p5, 3, model));
  assert(near(model.a, 1.0, 1e-3) && near(model.b, 2.0, 1e-3) && near(model.c, 0.5, 1e-4));
}

void testEvaluateModelFloorsAtZero() {
  cc::FitModel model;
  model.a = -2.0f;
  model.b = 1.0f;
  model.c = 0.0f;
  assert(cc::evaluateModel(model, 0.0f) == 0.0f);
  assert(cc::evaluateModel(model, -3.0f) == 0.0f);
  assert(cc::evaluateModel(model, 1.0f) == 0.0f);  // -2 + 1 < 0
  assert(near(cc::evaluateModel(model, 16.0f), 2.0, 1e-5));
  // No clamp above the captured range: the polynomial carries on.
  assert(near(cc::evaluateModel(model, 400.0f), 18.0, 1e-4));
  assert(cc::evaluateModel(cc::FitModel(), 100.0f) == 0.0f);
}

void testTareAndOutputMode() {
  assert(near(cc::applyTare(510.0f, 300.0f), 210.0, 1e-6));
  assert(cc::applyTare(290.0f, 300.0f) == 0.0f);
  assert(near(cc::applyTare(510.0f, NAN), 510.0, 1e-6));
  assert(cc::resolveOutputMode(true, true, false, false) == cc::OutputMode::Calibrated);
  assert(cc::resolveOutputMode(true, false, true, true) == cc::OutputMode::Tared);
  assert(cc::resolveOutputMode(false, true, false, true) == cc::OutputMode::Raw);
  assert(std::string(cc::outputModeName(cc::OutputMode::Calibrated)) == "calibrated");
}

void testCaptureBlocksAndMedian() {
  assert(cc::captureBlockCount(0) == 1);
  assert(cc::captureBlockCount(400) == 1);
  assert(cc::captureBlockCount(1000) == 2);
  assert(cc::captureBlockCount(3000) == 6);
  assert(cc::captureBlockCount(10000) == 20);
  assert(cc::captureBlockCount(600000) == cc::kMaxCaptureBlocks);

  float odd[] = {5.0f, 1.0f, 900.0f, 3.0f, 4.0f};  // one valve spike
  assert(cc::medianInPlace(odd, 5) == 4.0f);
  float even[] = {4.0f, 1.0f, 3.0f, 2.0f};
  assert(cc::medianInPlace(even, 4) == 2.5f);
  float one[] = {7.0f};
  assert(cc::medianInPlace(one, 1) == 7.0f);
  assert(std::isnan(cc::medianInPlace(nullptr, 0)));
}

}  // namespace

void testPointClassificationMatchesTheFitSelection() {
  const cc::Readout identity;
  float x = -1.0f;
  // Captured, above min level, below clip, above baseline: a fit point.
  assert(cc::classifyPoint(10.0f, 1.0f, 1000.0f, 50.0f, 3150.0f, identity, &x) == cc::PointStatus::Used);
  assert(near(x, 50.0, 1e-6));
  assert(cc::classifyPoint(10.0f, 1.0f, 1000.0f, NAN, 3150.0f, identity, nullptr) == cc::PointStatus::NotCaptured);
  assert(cc::classifyPoint(10.0f, 1.0f, NAN, 50.0f, 3150.0f, identity, nullptr) == cc::PointStatus::NotCaptured);
  assert(cc::classifyPoint(0.5f, 1.0f, 1000.0f, 50.0f, 3150.0f, identity, nullptr) == cc::PointStatus::BelowMinLevel);
  // Clipped: the ADC hit its ceiling, so the reading says nothing.
  assert(cc::classifyPoint(120.0f, 1.0f, 3000.0f, 150.0f, 3150.0f, identity, nullptr) == cc::PointStatus::Saturated);
  // Not above the sensor's own baseline: no response at this pressure.
  assert(cc::classifyPoint(43.3f, 1.0f, 1000.0f, 0.0f, 3150.0f, identity, nullptr) == cc::PointStatus::NoResponse);
  assert(cc::classifyPoint(43.3f, 1.0f, 1000.0f, -12.0f, 3150.0f, identity, nullptr) == cc::PointStatus::NoResponse);
  assert(cc::pointStatusCode(cc::PointStatus::Used) == 'u');
  assert(cc::pointStatusCode(cc::PointStatus::Saturated) == 's');
  assert(cc::pointStatusCode(cc::PointStatus::NoResponse) == 'n');
  assert(cc::pointStatusCode(cc::PointStatus::BelowMinLevel) == 'b');
  assert(cc::pointStatusCode(cc::PointStatus::NotCaptured) == '-');
}

void testClampedOutputHoldsAtTheTopOfTheCalibratedRange() {
  // A concave fit (c < 0) that would turn back down past its peak.
  cc::FitModel model;
  model.a = 0.0f;
  model.b = 10.0f;
  model.c = -0.5f;
  const float maxX = 50.0f;
  const float maxPressure = cc::evaluateModel(model, maxX);
  // Inside the range: the model itself.
  assert(near(cc::evaluateClamped(model, 16.0f, maxX, maxPressure), cc::evaluateModel(model, 16.0f), 1e-5));
  // Past it: held, even where the raw polynomial would have fallen.
  assert(cc::evaluateModel(model, 300.0f) < maxPressure);
  assert(near(cc::evaluateClamped(model, 300.0f, maxX, maxPressure), maxPressure, 1e-5));
  assert(near(cc::evaluateClamped(model, maxX, maxX, maxPressure), maxPressure, 1e-5));
  // Never above the highest calibrated pressure.
  assert(cc::evaluateClamped(model, 49.0f, maxX, 20.0f) <= 20.0f);
  // Unfitted sensors (a partial profile) read 0.
  assert(cc::evaluateClamped(cc::FitModel(), 30.0f, maxX, maxPressure) == 0.0f);
  // No known limit: plain model.
  assert(near(cc::evaluateClamped(model, 300.0f, NAN, NAN), cc::evaluateModel(model, 300.0f), 1e-5));
}

int main() {
  testPointClassificationMatchesTheFitSelection();
  testClampedOutputHoldsAtTheTopOfTheCalibratedRange();
  testFitMatchesNumpyPolyfitOnPaperData();
  testReadoutInvertsThePaperCurve();
  testEndToEndFromMillivoltsMatchesThePaper();
  testFitNeedsThreeDistinctPoints();
  testEvaluateModelFloorsAtZero();
  testTareAndOutputMode();
  testCaptureBlocksAndMedian();
  std::cout << "Calibration curve tests passed" << std::endl;
  return 0;
}
