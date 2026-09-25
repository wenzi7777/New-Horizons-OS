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

void buildCurve(const float* levels, const float* relativeRaws, size_t count, Curve& out) {
  out.raws.clear();
  out.levels.clear();
  out.tangents.clear();

  struct SamplePoint {
    float raw;
    float level;
  };
  std::vector<SamplePoint> points;
  points.reserve(count + 1);
  points.push_back({0.0f, 0.0f});
  for (size_t i = 0; i < count; ++i) {
    const float raw = relativeRaws[i];
    if (std::isnan(raw) || raw <= 0.0001f) {
      continue;
    }
    points.push_back({raw, levels[i]});
  }

  std::sort(points.begin(), points.end(), [](const SamplePoint& a, const SamplePoint& b) {
    if (a.raw == b.raw) {
      return a.level < b.level;
    }
    return a.raw < b.raw;
  });

  out.raws.reserve(points.size());
  out.levels.reserve(points.size());
  out.tangents.assign(points.size(), 0.0f);
  for (const SamplePoint& point : points) {
    out.raws.push_back(point.raw);
    out.levels.push_back(point.level);
  }
  if (points.size() <= 1) {
    return;
  }

  std::vector<float> slopes(points.size() - 1, 0.0f);
  for (size_t i = 0; i < points.size() - 1; ++i) {
    const float dx = points[i + 1].raw - points[i].raw;
    slopes[i] = (dx < 0.0001f) ? 0.0f : (points[i + 1].level - points[i].level) / dx;
  }
  out.tangents[0] = slopes[0];
  out.tangents[points.size() - 1] = slopes[points.size() - 2];
  for (size_t i = 1; i < points.size() - 1; ++i) {
    if (slopes[i - 1] * slopes[i] <= 0.0f) {
      out.tangents[i] = 0.0f;
    } else {
      out.tangents[i] = 2.0f / (1.0f / slopes[i - 1] + 1.0f / slopes[i]);
    }
  }
}

bool evaluateCurve(const Curve& curve, float relativeRaw, float& out) {
  if (curve.raws.empty() || curve.levels.empty() || curve.tangents.empty()) {
    return false;
  }
  if (curve.raws.size() == 1 || relativeRaw <= curve.raws.front()) {
    out = curve.levels.front();
    return true;
  }
  if (relativeRaw >= curve.raws.back()) {
    out = curve.levels.back();
    return true;
  }
  const auto upper = std::lower_bound(curve.raws.begin() + 1, curve.raws.end(), relativeRaw);
  const size_t i = static_cast<size_t>(upper - curve.raws.begin());
  const float h = curve.raws[i] - curve.raws[i - 1];
  if (h < 0.0001f) {
    out = std::min(curve.levels[i - 1], curve.levels[i]);
    return true;
  }
  const float t = (relativeRaw - curve.raws[i - 1]) / h;
  const float t2 = t * t;
  const float t3 = t2 * t;
  float value = (2.0f * t3 - 3.0f * t2 + 1.0f) * curve.levels[i - 1]
              + (t3 - 2.0f * t2 + t) * h * curve.tangents[i - 1]
              + (-2.0f * t3 + 3.0f * t2) * curve.levels[i]
              + (t3 - t2) * h * curve.tangents[i];
  out = value < 0.0f ? 0.0f : value;
  return true;
}

void toRelative(std::vector<float>& levelValues, const std::vector<float>& tare) {
  for (size_t i = 0; i < levelValues.size(); ++i) {
    if (std::isnan(levelValues[i])) {
      continue;
    }
    const float tareValue = i < tare.size() ? tare[i] : NAN;
    levelValues[i] = std::isnan(tareValue) ? NAN : levelValues[i] - tareValue;
  }
}

}  // namespace calibration_curve
}  // namespace nhos
