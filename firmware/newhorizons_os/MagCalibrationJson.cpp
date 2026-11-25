#include "MagCalibrationJson.h"

#include "JsonUtils.h"

namespace nhos {

namespace {
void appendFloats(String& out, const float* values, uint8_t count, uint8_t digits) {
  out += "[";
  for (uint8_t i = 0; i < count; ++i) {
    if (i) out += ",";
    out += String(values[i], static_cast<unsigned int>(digits));
  }
  out += "]";
}

void appendU64(String& out, uint64_t value) {
  char buf[21];
  uint8_t pos = sizeof(buf) - 1;
  buf[pos] = '\0';
  do {
    buf[--pos] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value && pos);
  out += &buf[pos];
}
}  // namespace

bool parseMagCalibrationJson(const String& json, MagCalibrationData& out, String& error) {
  MagCalibrationData parsed;
  const String modeText = jsonExtractString(json, "mode", "none");
  if (!parseMagCalibrationMode(modeText.c_str(), parsed.mode)) {
    error = "mag_calibration_mode_invalid";
    return false;
  }
  if (parsed.mode == MagCalibrationMode::None) {
    out = parsed;
    return true;
  }
  if (jsonExtractFloatArray(json, "offset", parsed.offset, 3) != 3) {
    error = "mag_calibration_offset_invalid";
    return false;
  }
  if (jsonExtractFloatArray(json, "matrix", parsed.matrix, 9) != 9) {
    error = "mag_calibration_matrix_invalid";
    return false;
  }
  if (!jsonExtractFloat(json, "field_ut", parsed.fieldUt)) {
    error = "mag_calibration_field_invalid";
    return false;
  }
  if (!jsonExtractFloat(json, "residual_pct", parsed.residualPct)) {
    parsed.residualPct = 0.0f;
  }
  if (!jsonExtractUInt64(json, "calibrated_at_ms", parsed.calibratedAtMs)) {
    parsed.calibratedAtMs = 0;
  }
  out = parsed;
  return true;
}

void appendMagCalibrationJson(String& out, const MagCalibrationData& cal) {
  out += "{\"mode\":\"";
  out += magCalibrationModeName(cal.mode);
  out += "\"";
  if (cal.mode != MagCalibrationMode::None) {
    out += ",\"offset\":";
    appendFloats(out, cal.offset, 3, 4);
    out += ",\"matrix\":";
    appendFloats(out, cal.matrix, 9, 6);
    out += ",\"field_ut\":";
    out += String(cal.fieldUt, 3);
    out += ",\"residual_pct\":";
    out += String(cal.residualPct, 3);
    out += ",\"calibrated_at_ms\":";
    appendU64(out, cal.calibratedAtMs);
  }
  out += "}";
}

}  // namespace nhos
