#pragma once

#include <cmath>
#include <cstdint>

namespace nhos {

// Hard/soft-iron calibration of the magnetometer:
//
//   m_cal = M * (m_raw - b)
//
// b (offset, uT) is the field the board itself carries -- parts, battery,
// enclosure, screws -- and M (3x3, row-major) undoes the stretching that
// turns the sphere of the earth's field into an ellipsoid. Both move with
// the device, so they are fitted once per mounting (the Desktop / Studio /
// Pocket wizard does the fitting) and stored here; M is normalised so the
// output stays in uT. Fields from the room (a steel desk, a motor) do not
// move with the device and no calibration removes them.
//
// Pure, no Arduino: tests_native/test_mag_calibration.cpp.
enum class MagCalibrationMode : uint8_t { None = 0, HardIron = 1, Full = 2 };

inline const char* magCalibrationModeName(MagCalibrationMode mode) {
  switch (mode) {
    case MagCalibrationMode::HardIron: return "hard_iron";
    case MagCalibrationMode::Full: return "full";
    default: return "none";
  }
}

inline bool parseMagCalibrationMode(const char* text, MagCalibrationMode& out) {
  if (!text) return false;
  auto eq = [](const char* a, const char* b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == *b;
  };
  if (eq(text, "none")) { out = MagCalibrationMode::None; return true; }
  if (eq(text, "hard_iron")) { out = MagCalibrationMode::HardIron; return true; }
  if (eq(text, "full")) { out = MagCalibrationMode::Full; return true; }
  return false;
}

struct MagCalibrationData {
  MagCalibrationMode mode = MagCalibrationMode::None;
  float offset[3] = {0.0f, 0.0f, 0.0f};
  float matrix[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  float fieldUt = 0.0f;       // fitted field strength, uT
  float residualPct = 0.0f;   // std(|m_cal|) / mean(|m_cal|) * 100 at fit time
  uint64_t calibratedAtMs = 0;  // host wall clock (epoch ms) at fit time
};

namespace magcal {

constexpr float kMaxOffsetUt = 500.0f;
constexpr float kMaxMatrixEntry = 10.0f;
// ||M||_F * ||M^-1||_F bounds the 2-norm condition number from above (by a
// factor up to 3 for 3x3), so 30 rejects anything worse than ~10-30: a
// matrix that stretched one axis that much is a bad fit, not a board.
constexpr float kMaxConditionEstimate = 30.0f;
constexpr float kMaxFieldUt = 200.0f;

inline bool finite(float v) { return std::isfinite(v); }

inline float det3(const float m[9]) {
  return m[0] * (m[4] * m[8] - m[5] * m[7]) -
         m[1] * (m[3] * m[8] - m[5] * m[6]) +
         m[2] * (m[3] * m[7] - m[4] * m[6]);
}

inline bool invert3(const float m[9], float out[9]) {
  const float d = det3(m);
  if (!finite(d) || std::fabs(d) < 1e-9f) return false;
  const float inv = 1.0f / d;
  out[0] = (m[4] * m[8] - m[5] * m[7]) * inv;
  out[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
  out[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
  out[3] = (m[5] * m[6] - m[3] * m[8]) * inv;
  out[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
  out[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
  out[6] = (m[3] * m[7] - m[4] * m[6]) * inv;
  out[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
  out[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
  return true;
}

inline float frobenius(const float m[9]) {
  float sum = 0.0f;
  for (uint8_t i = 0; i < 9; ++i) sum += m[i] * m[i];
  return std::sqrt(sum);
}

enum class ValidateResult : uint8_t {
  Ok = 0,
  NotFinite,
  OffsetTooLarge,
  MatrixEntryTooLarge,
  MatrixSingular,
  MatrixIllConditioned,
  FieldOutOfRange,
  HardIronMatrixNotIdentity,
};

inline const char* validateResultName(ValidateResult r) {
  switch (r) {
    case ValidateResult::Ok: return "ok";
    case ValidateResult::NotFinite: return "mag_calibration_not_finite";
    case ValidateResult::OffsetTooLarge: return "mag_calibration_offset_too_large";
    case ValidateResult::MatrixEntryTooLarge: return "mag_calibration_matrix_too_large";
    case ValidateResult::MatrixSingular: return "mag_calibration_matrix_singular";
    case ValidateResult::MatrixIllConditioned: return "mag_calibration_matrix_ill_conditioned";
    case ValidateResult::FieldOutOfRange: return "mag_calibration_field_out_of_range";
    case ValidateResult::HardIronMatrixNotIdentity: return "mag_calibration_hard_iron_needs_identity";
  }
  return "mag_calibration_invalid";
}

// Checked before a calibration is stored. Mode None is the cleared state
// and always valid (its numbers are ignored).
inline ValidateResult validate(const MagCalibrationData& cal) {
  if (cal.mode == MagCalibrationMode::None) return ValidateResult::Ok;
  for (uint8_t i = 0; i < 3; ++i) {
    if (!finite(cal.offset[i])) return ValidateResult::NotFinite;
  }
  for (uint8_t i = 0; i < 9; ++i) {
    if (!finite(cal.matrix[i])) return ValidateResult::NotFinite;
  }
  if (!finite(cal.fieldUt) || !finite(cal.residualPct)) return ValidateResult::NotFinite;
  for (uint8_t i = 0; i < 3; ++i) {
    if (std::fabs(cal.offset[i]) > kMaxOffsetUt) return ValidateResult::OffsetTooLarge;
  }
  for (uint8_t i = 0; i < 9; ++i) {
    if (std::fabs(cal.matrix[i]) > kMaxMatrixEntry) return ValidateResult::MatrixEntryTooLarge;
  }
  if (!(cal.fieldUt > 0.0f) || cal.fieldUt > kMaxFieldUt) return ValidateResult::FieldOutOfRange;
  if (cal.mode == MagCalibrationMode::HardIron) {
    static const float kIdentity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (uint8_t i = 0; i < 9; ++i) {
      if (std::fabs(cal.matrix[i] - kIdentity[i]) > 1e-4f) {
        return ValidateResult::HardIronMatrixNotIdentity;
      }
    }
  }
  const float d = det3(cal.matrix);
  if (!(d > 1e-6f)) return ValidateResult::MatrixSingular;
  float inverse[9];
  if (!invert3(cal.matrix, inverse)) return ValidateResult::MatrixSingular;
  if (frobenius(cal.matrix) * frobenius(inverse) > kMaxConditionEstimate) {
    return ValidateResult::MatrixIllConditioned;
  }
  return ValidateResult::Ok;
}

// m_cal = M * (m_raw - b). Mode None passes the sample through unchanged.
inline void apply(const MagCalibrationData& cal, const float raw[3], float out[3]) {
  if (cal.mode == MagCalibrationMode::None) {
    out[0] = raw[0];
    out[1] = raw[1];
    out[2] = raw[2];
    return;
  }
  const float x = raw[0] - cal.offset[0];
  const float y = raw[1] - cal.offset[1];
  const float z = raw[2] - cal.offset[2];
  const float* m = cal.matrix;
  out[0] = m[0] * x + m[1] * y + m[2] * z;
  out[1] = m[3] * x + m[4] * y + m[5] * z;
  out[2] = m[6] * x + m[7] * y + m[8] * z;
}

// "The calibration may no longer fit": the calibrated field strength drifts
// away from the fitted one for a while. Catches what a calendar cannot -- a
// battery swapped, a part magnetised by a magnet that came too close.
//
// Once a second, the mean |m_cal| of that second goes into a 60-entry ring;
// the median of the ring is compared with fieldUt. Off by more than
// kSuspectFraction for kHoldMs in a row raises the flag; within it for
// kHoldMs in a row lowers it again. RAM only: a reboot starts clean.
//
// A hint and nothing more -- bringing a magnet close on purpose trips it too.
class MagCalibrationDriftMonitor {
 public:
  static constexpr uint32_t kBucketMs = 1000;
  static constexpr uint8_t kBuckets = 60;
  static constexpr uint8_t kMinBuckets = 10;
  static constexpr float kSuspectFraction = 0.20f;
  static constexpr uint32_t kHoldMs = 5UL * 60UL * 1000UL;

  void reset() {
    count_ = 0;
    head_ = 0;
    bucketSum_ = 0.0f;
    bucketN_ = 0;
    bucketStartMs_ = 0;
    bucketStarted_ = false;
    suspect_ = false;
    stateSinceMs_ = 0;
    stateKnown_ = false;
    lastDeviating_ = false;
  }

  // fieldUt <= 0 (no calibration) never raises the flag.
  void feed(uint32_t nowMs, float magnitudeUt, float fieldUt) {
    if (!(fieldUt > 0.0f) || !std::isfinite(magnitudeUt)) return;
    if (!bucketStarted_) {
      bucketStarted_ = true;
      bucketStartMs_ = nowMs;
    }
    bucketSum_ += magnitudeUt;
    ++bucketN_;
    if (static_cast<uint32_t>(nowMs - bucketStartMs_) < kBucketMs) return;

    ring_[head_] = bucketSum_ / static_cast<float>(bucketN_);
    head_ = static_cast<uint8_t>((head_ + 1) % kBuckets);
    if (count_ < kBuckets) ++count_;
    bucketSum_ = 0.0f;
    bucketN_ = 0;
    bucketStartMs_ = nowMs;
    if (count_ < kMinBuckets) return;

    const float median = medianOfRing();
    const bool deviating = std::fabs(median - fieldUt) > kSuspectFraction * fieldUt;
    if (!stateKnown_ || deviating != lastDeviating_) {
      stateKnown_ = true;
      lastDeviating_ = deviating;
      stateSinceMs_ = nowMs;
    }
    if (static_cast<uint32_t>(nowMs - stateSinceMs_) >= kHoldMs) {
      suspect_ = deviating;
    }
  }

  bool suspect() const { return suspect_; }
  // The latest median, for status (0 until there are enough buckets).
  float medianUt() const { return count_ >= kMinBuckets ? medianOfRing() : 0.0f; }

 private:
  float medianOfRing() const {
    float sorted[kBuckets];
    for (uint8_t i = 0; i < count_; ++i) sorted[i] = ring_[i];
    // Insertion sort: at most 60 entries, once a second.
    for (uint8_t i = 1; i < count_; ++i) {
      const float v = sorted[i];
      int8_t j = static_cast<int8_t>(i) - 1;
      while (j >= 0 && sorted[j] > v) {
        sorted[j + 1] = sorted[j];
        --j;
      }
      sorted[j + 1] = v;
    }
    if (count_ % 2) return sorted[count_ / 2];
    return 0.5f * (sorted[count_ / 2 - 1] + sorted[count_ / 2]);
  }

  float ring_[kBuckets] = {0};
  uint8_t count_ = 0;
  uint8_t head_ = 0;
  float bucketSum_ = 0.0f;
  uint32_t bucketN_ = 0;
  uint32_t bucketStartMs_ = 0;
  bool bucketStarted_ = false;
  bool suspect_ = false;
  uint32_t stateSinceMs_ = 0;
  bool stateKnown_ = false;
  bool lastDeviating_ = false;
};

}  // namespace magcal
}  // namespace nhos
