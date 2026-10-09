#include "CalibrationCurve.h"

#include <algorithm>
#include <cmath>

namespace nhos {
namespace calibration_curve {

const char* outputModeName(OutputMode mode) {
  switch (mode) {
    case OutputMode::Calibrated:
      return "calibrated";
    case OutputMode::Tared:
      return "tared";
    case OutputMode::Raw:
    default:
      return "raw";
  }
}

OutputMode resolveOutputMode(bool calibrationEnabled, bool calibrationReady, bool tareEnabled, bool tareComplete) {
  if (calibrationEnabled && calibrationReady) {
    return OutputMode::Calibrated;
  }
  if (tareEnabled && tareComplete) {
    return OutputMode::Tared;
  }
  return OutputMode::Raw;
}

float applyTare(float raw, float tare) {
  if (std::isnan(tare)) {
    return raw;
  }
  return std::max(0.0f, raw - tare);
}

float readoutX(float relativeMv, const Readout& readout) {
  if (std::isnan(relativeMv)) {
    return NAN;
  }
  if (relativeMv <= 0.0f) {
    return 0.0f;
  }
  const float disc = std::max(0.0f, readout.c1 * readout.c1 + 4.0f * readout.c2 * relativeMv);
  const float denominator = readout.c1 + std::sqrt(disc);
  if (denominator <= 0.0f) {
    return NAN;
  }
  if (disc == 0.0f && readout.c2 < 0.0f) {
    return -readout.c1 / (2.0f * readout.c2);  // the turning point, exactly
  }
  return 2.0f * relativeMv / denominator;
}

bool fitValid(const FitModel& model) {
  return !std::isnan(model.a) && !std::isnan(model.b) && !std::isnan(model.c);
}

bool fitModel(const float* xs, const float* pressures, size_t count, FitModel& out) {
  out = FitModel();
  if (!xs || !pressures || count < kMinFitPoints) {
    return false;
  }
  // Normal equations for the basis (1, u, u^2), u = sqrt(x), in double: the
  // sums reach ~1e7 for a 1000 uS sensor, far past float's 7 digits.
  double m[3][4] = {{0}};
  size_t distinct = 0;
  double firstU = NAN;
  double secondU = NAN;
  for (size_t i = 0; i < count; ++i) {
    const double x = xs[i];
    const double p = pressures[i];
    if (std::isnan(x) || std::isnan(p) || x <= 0.0) {
      continue;
    }
    const double u = std::sqrt(x);
    const double basis[3] = {1.0, u, u * u};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        m[r][c] += basis[r] * basis[c];
      }
      m[r][3] += basis[r] * p;
    }
    if (distinct == 0) {
      firstU = u;
      distinct = 1;
    } else if (distinct == 1 && u != firstU) {
      secondU = u;
      distinct = 2;
    } else if (distinct == 2 && u != firstU && u != secondU) {
      distinct = 3;
    }
  }
  if (distinct < kMinFitPoints) {
    return false;
  }

  double scale = 0.0;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      scale = std::max(scale, std::fabs(m[r][c]));
    }
  }
  // Gaussian elimination with partial pivoting.
  for (int col = 0; col < 3; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 3; ++r) {
      if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) {
        pivot = r;
      }
    }
    if (std::fabs(m[pivot][col]) <= scale * 1e-14) {
      return false;
    }
    if (pivot != col) {
      for (int c = 0; c < 4; ++c) {
        std::swap(m[col][c], m[pivot][c]);
      }
    }
    for (int r = col + 1; r < 3; ++r) {
      const double factor = m[r][col] / m[col][col];
      for (int c = col; c < 4; ++c) {
        m[r][c] -= factor * m[col][c];
      }
    }
  }
  double coefficient[3] = {0};
  for (int r = 2; r >= 0; --r) {
    double sum = m[r][3];
    for (int c = r + 1; c < 3; ++c) {
      sum -= m[r][c] * coefficient[c];
    }
    coefficient[r] = sum / m[r][r];
  }
  if (std::isnan(coefficient[0]) || std::isnan(coefficient[1]) || std::isnan(coefficient[2]) ||
      std::isinf(coefficient[0]) || std::isinf(coefficient[1]) || std::isinf(coefficient[2])) {
    return false;
  }
  out.a = static_cast<float>(coefficient[0]);
  out.b = static_cast<float>(coefficient[1]);
  out.c = static_cast<float>(coefficient[2]);
  return true;
}

float evaluateModel(const FitModel& model, float x) {
  if (!fitValid(model) || std::isnan(x) || x <= 0.0f) {
    return 0.0f;
  }
  const float value = model.a + model.b * std::sqrt(x) + model.c * x;
  return value > 0.0f ? value : 0.0f;
}

uint32_t captureBlockCount(uint32_t durationMs) {
  const uint32_t blocks = (durationMs + kCaptureBlockMs - 1) / kCaptureBlockMs;
  return std::min(kMaxCaptureBlocks, std::max<uint32_t>(1, blocks));
}

float medianInPlace(float* values, size_t count) {
  if (!values || count == 0) {
    return NAN;
  }
  const size_t mid = count / 2;
  std::nth_element(values, values + mid, values + count);
  const float upper = values[mid];
  if (count % 2) {
    return upper;
  }
  const float lower = *std::max_element(values, values + mid);
  return (lower + upper) / 2.0f;
}

}  // namespace calibration_curve
}  // namespace nhos
