// MagCalibration: applying, validating and the drift (suspect) monitor.
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "MagCalibration.h"

namespace {
int failures = 0;
void expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}
bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }

using nhos::MagCalibrationData;
using nhos::MagCalibrationMode;
namespace magcal = nhos::magcal;

MagCalibrationData fullCal() {
  MagCalibrationData cal;
  cal.mode = MagCalibrationMode::Full;
  cal.offset[0] = 10.0f;
  cal.offset[1] = -20.0f;
  cal.offset[2] = 5.0f;
  const float m[9] = {1.1f, 0.05f, 0.0f, 0.05f, 0.9f, 0.02f, 0.0f, 0.02f, 1.0f};
  for (int i = 0; i < 9; ++i) cal.matrix[i] = m[i];
  cal.fieldUt = 47.0f;
  cal.residualPct = 1.5f;
  cal.calibratedAtMs = 1764000000000ULL;
  return cal;
}
}  // namespace

int main() {
  // apply: None passes through; otherwise M * (raw - b).
  {
    MagCalibrationData none;
    const float raw[3] = {1.0f, 2.0f, 3.0f};
    float out[3];
    magcal::apply(none, raw, out);
    expect(out[0] == 1.0f && out[1] == 2.0f && out[2] == 3.0f, "mode none passes through");

    const MagCalibrationData cal = fullCal();
    const float raw2[3] = {20.0f, -10.0f, 15.0f};  // minus offset = (10, 10, 10)
    magcal::apply(cal, raw2, out);
    expect(near(out[0], 1.1f * 10 + 0.05f * 10), "row 0");
    expect(near(out[1], 0.05f * 10 + 0.9f * 10 + 0.02f * 10), "row 1");
    expect(near(out[2], 0.02f * 10 + 1.0f * 10), "row 2");

    MagCalibrationData hard;
    hard.mode = MagCalibrationMode::HardIron;
    hard.offset[0] = 1.0f;
    hard.offset[1] = 2.0f;
    hard.offset[2] = 3.0f;
    hard.fieldUt = 40.0f;
    magcal::apply(hard, raw, out);
    expect(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f, "hard iron subtracts the offset");
  }

  // validate.
  {
    expect(magcal::validate(MagCalibrationData()) == magcal::ValidateResult::Ok, "cleared is valid");
    expect(magcal::validate(fullCal()) == magcal::ValidateResult::Ok, "a sane full fit is valid");

    MagCalibrationData c = fullCal();
    c.offset[1] = NAN;
    expect(magcal::validate(c) == magcal::ValidateResult::NotFinite, "NaN offset rejected");

    c = fullCal();
    c.matrix[4] = INFINITY;
    expect(magcal::validate(c) == magcal::ValidateResult::NotFinite, "inf matrix rejected");

    c = fullCal();
    c.offset[2] = 600.0f;
    expect(magcal::validate(c) == magcal::ValidateResult::OffsetTooLarge, "offset > 500 uT rejected");

    c = fullCal();
    c.matrix[0] = 11.0f;
    expect(magcal::validate(c) == magcal::ValidateResult::MatrixEntryTooLarge, "entry > 10 rejected");

    c = fullCal();
    const float singular[9] = {1, 2, 3, 2, 4, 6, 0, 0, 1};
    for (int i = 0; i < 9; ++i) c.matrix[i] = singular[i];
    expect(magcal::validate(c) == magcal::ValidateResult::MatrixSingular, "singular rejected");

    c = fullCal();
    const float mirrored[9] = {-1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) c.matrix[i] = mirrored[i];
    expect(magcal::validate(c) == magcal::ValidateResult::MatrixSingular,
           "negative determinant (a mirror) rejected");

    c = fullCal();
    const float squashed[9] = {5, 0, 0, 0, 5, 0, 0, 0, 0.1f};
    for (int i = 0; i < 9; ++i) c.matrix[i] = squashed[i];
    expect(magcal::validate(c) == magcal::ValidateResult::MatrixIllConditioned,
           "a 50:1 stretch rejected");

    c = fullCal();
    c.fieldUt = 0.0f;
    expect(magcal::validate(c) == magcal::ValidateResult::FieldOutOfRange, "zero field rejected");

    c = fullCal();
    c.mode = MagCalibrationMode::HardIron;
    expect(magcal::validate(c) == magcal::ValidateResult::HardIronMatrixNotIdentity,
           "hard iron must carry the identity");
  }

  // Drift monitor.
  {
    using M = magcal::MagCalibrationDriftMonitor;
    M monitor;
    monitor.reset();
    uint32_t t = 0;
    // Steady at the fitted field: never suspect.
    for (; t < 10UL * 60UL * 1000UL; t += 50) monitor.feed(t, 47.0f, 47.0f);
    expect(!monitor.suspect(), "steady field: not suspect");
    expect(near(monitor.medianUt(), 47.0f, 0.01f), "median tracks the field");

    // 30% off: not yet suspect before the hold time, suspect after it.
    const uint32_t driftStart = t;
    for (; t < driftStart + 4UL * 60UL * 1000UL; t += 50) monitor.feed(t, 61.0f, 47.0f);
    expect(!monitor.suspect(), "off for 4 min: not yet");
    for (; t < driftStart + 7UL * 60UL * 1000UL; t += 50) monitor.feed(t, 61.0f, 47.0f);
    expect(monitor.suspect(), "off for 7 min: suspect");

    // Back within range: stays suspect until the hold time passes again.
    const uint32_t backStart = t;
    for (; t < backStart + 3UL * 60UL * 1000UL; t += 50) monitor.feed(t, 47.0f, 47.0f);
    expect(monitor.suspect(), "back for 3 min: still suspect");
    for (; t < backStart + 8UL * 60UL * 1000UL; t += 50) monitor.feed(t, 47.0f, 47.0f);
    expect(!monitor.suspect(), "back for 8 min: cleared");

    // A brief magnet (1 min) never trips it.
    const uint32_t blipStart = t;
    for (; t < blipStart + 60UL * 1000UL; t += 50) monitor.feed(t, 200.0f, 47.0f);
    for (; t < blipStart + 10UL * 60UL * 1000UL; t += 50) monitor.feed(t, 47.0f, 47.0f);
    expect(!monitor.suspect(), "a one-minute magnet does not trip it");

    // No calibration (field 0): ignored.
    M empty;
    empty.reset();
    for (uint32_t u = 0; u < 20UL * 60UL * 1000UL; u += 100) empty.feed(u, 10.0f, 0.0f);
    expect(!empty.suspect(), "no calibration: never suspect");
  }

  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_mag_calibration: ok\n");
  return 0;
}
