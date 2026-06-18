#include "DeviceConfig.h"

#include "BoardPins.h"

namespace nhos {
namespace {
constexpr char kDeviceConfigPath[] = "/config/device.json";

String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

String objectForKey(const String& json, const char* key) {
  const String pattern = "\"" + String(key) + "\"";
  const int keyIndex = json.indexOf(pattern);
  if (keyIndex < 0) {
    return "";
  }
  const int start = json.indexOf('{', keyIndex + pattern.length());
  if (start < 0) {
    return "";
  }
  int depth = 0;
  for (int i = start; i < json.length(); ++i) {
    const char c = json.charAt(i);
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) {
        return json.substring(start, i + 1);
      }
    }
  }
  return "";
}

String extractString(const String& json, const char* key, const String& fallback) {
  const String pattern = "\"" + String(key) + "\"";
  const int keyIndex = json.indexOf(pattern);
  if (keyIndex < 0) {
    return fallback;
  }
  const int colon = json.indexOf(':', keyIndex + pattern.length());
  const int start = json.indexOf('"', colon + 1);
  const int end = json.indexOf('"', start + 1);
  if (colon < 0 || start < 0 || end < 0) {
    return fallback;
  }
  return json.substring(start + 1, end);
}

int extractInt(const String& json, const char* key, int fallback) {
  const String pattern = "\"" + String(key) + "\"";
  const int keyIndex = json.indexOf(pattern);
  if (keyIndex < 0) {
    return fallback;
  }
  const int colon = json.indexOf(':', keyIndex + pattern.length());
  int end = json.indexOf(',', colon + 1);
  if (end < 0) {
    end = json.indexOf('}', colon + 1);
  }
  if (colon < 0 || end < 0) {
    return fallback;
  }
  return json.substring(colon + 1, end).toInt();
}

float extractFloat(const String& json, const char* key, float fallback) {
  const String pattern = "\"" + String(key) + "\"";
  const int keyIndex = json.indexOf(pattern);
  if (keyIndex < 0) {
    return fallback;
  }
  const int colon = json.indexOf(':', keyIndex + pattern.length());
  int end = json.indexOf(',', colon + 1);
  if (end < 0) {
    end = json.indexOf('}', colon + 1);
  }
  if (colon < 0 || end < 0) {
    return fallback;
  }
  return json.substring(colon + 1, end).toFloat();
}

bool extractBool(const String& json, const char* key, bool fallback) {
  const String pattern = "\"" + String(key) + "\"";
  const int keyIndex = json.indexOf(pattern);
  if (keyIndex < 0) {
    return fallback;
  }
  const int colon = json.indexOf(':', keyIndex + pattern.length());
  int end = json.indexOf(',', colon + 1);
  if (end < 0) {
    end = json.indexOf('}', colon + 1);
  }
  if (colon < 0 || end < 0) {
    return fallback;
  }
  String value = json.substring(colon + 1, end);
  value.trim();
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  return fallback;
}

size_t extractArray(const String& json, const char* key, uint8_t* out, size_t maxCount) {
  const String pattern = "\"" + String(key) + "\"";
  const int keyIndex = json.indexOf(pattern);
  if (keyIndex < 0) {
    return 0;
  }
  const int start = json.indexOf('[', keyIndex);
  const int end = json.indexOf(']', start + 1);
  if (start < 0 || end < 0) {
    return 0;
  }
  size_t count = 0;
  int cursor = start + 1;
  while (cursor < end && count < maxCount) {
    int sep = json.indexOf(',', cursor);
    if (sep < 0 || sep > end) {
      sep = end;
    }
    String token = json.substring(cursor, sep);
    token.trim();
    if (token.length()) {
      out[count++] = static_cast<uint8_t>(token.toInt());
    }
    cursor = sep + 1;
  }
  return count;
}

void appendArray(String& out, const uint8_t* pins, size_t count) {
  out += "[";
  for (size_t i = 0; i < count; ++i) {
    if (i) {
      out += ",";
    }
    out += static_cast<unsigned int>(pins[i]);
  }
  out += "]";
}

float clampBrightness(float value) {
  if (value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

uint8_t clampByte(int value, uint8_t minValue, uint8_t maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return static_cast<uint8_t>(value);
}

uint8_t streamBufferDepthForMode(const String& mode) {
  if (mode == "extended") {
    return kExtendedScanRingFrames;
  }
  return kStandardScanRingFrames;
}

bool validFilterMedian(uint8_t median) {
  return median == 1 || median == 3 || median == 5;
}

void defaultMatrixLayout(MatrixLayoutConfig& layout) {
  memset(layout.analogPins, 0, sizeof(layout.analogPins));
  memset(layout.selectPins, 0, sizeof(layout.selectPins));
  memcpy(layout.analogPins, kRowAdcPins, kRowAdcPinCount);
  memcpy(layout.selectPins, kColPins, kColPinCount);
  layout.analogCount = kRowAdcPinCount;
  layout.selectCount = kColPinCount;
}
}  // namespace

bool DeviceConfig::load(Storage& storage) {
  setDefaults();
  String json;
  if (!storage.readTextFile(kDeviceConfigPath, json)) {
    loaded_ = false;
    lastError_ = "config_missing";
    return false;
  }
  if (!applyJson(json)) {
    loaded_ = false;
    if (lastError_.isEmpty()) {
      lastError_ = "config_invalid";
    }
    setDefaults();
    return false;
  }
  loaded_ = true;
  lastError_ = "";
  return true;
}

bool DeviceConfig::save(Storage& storage) {
  if (!storage.writeTextFileAtomic(kDeviceConfigPath, toJson())) {
    lastError_ = "config_write_failed";
    return false;
  }
  loaded_ = true;
  lastError_ = "";
  return true;
}

const DeviceConfigData& DeviceConfig::data() const {
  return data_;
}

DeviceConfigData& DeviceConfig::mutableData() {
  return data_;
}

bool DeviceConfig::setMatrixLayout(const uint8_t* analogPins, size_t analogCount, const uint8_t* selectPins, size_t selectCount) {
  if (analogCount == 0 || selectCount == 0) {
    if (analogCount != 0 || selectCount != 0) {
      return false;
    }
    data_.matrixLayout.analogCount = 0;
    data_.matrixLayout.selectCount = 0;
    memset(data_.matrixLayout.analogPins, 0, sizeof(data_.matrixLayout.analogPins));
    memset(data_.matrixLayout.selectPins, 0, sizeof(data_.matrixLayout.selectPins));
    return true;
  }
  if (!analogPins || !selectPins || analogCount > kRows || selectCount > kCols) {
    return false;
  }
  for (size_t i = 0; i < analogCount; ++i) {
    if (!isAllowedRowPin(analogPins[i])) {
      return false;
    }
  }
  for (size_t i = 0; i < selectCount; ++i) {
    if (!isAllowedColPin(selectPins[i])) {
      return false;
    }
  }
  memcpy(data_.matrixLayout.analogPins, analogPins, analogCount);
  memcpy(data_.matrixLayout.selectPins, selectPins, selectCount);
  data_.matrixLayout.analogCount = analogCount;
  data_.matrixLayout.selectCount = selectCount;
  return true;
}

bool DeviceConfig::setScanTiming(uint16_t targetFps, uint16_t settleUs, uint16_t sendEveryNFrames) {
  if (targetFps == 0 || targetFps > kMaxTargetFps || settleUs > 500 || sendEveryNFrames == 0 || sendEveryNFrames > 120) {
    return false;
  }
  data_.scanTiming.targetFps = targetFps;
  data_.scanTiming.settleUs = settleUs;
  data_.scanTiming.sendEveryNFrames = sendEveryNFrames;
  return true;
}

bool DeviceConfig::setStreamBuffer(bool enabled, const String& mode) {
  if (!validStreamBufferMode(mode)) {
    return false;
  }
  data_.streamBuffer.enabled = enabled;
  data_.streamBuffer.mode = mode;
  data_.streamBuffer.depthFrames = enabled ? streamBufferDepthForMode(mode) : 0;
  return true;
}

bool DeviceConfig::setFilter(bool enabled, uint8_t median, float alpha) {
  if (!validFilterMedian(median) || alpha < kFilterAlphaMin || alpha > kFilterAlphaMax) {
    return false;
  }
  data_.filter.enabled = enabled;
  data_.filter.median = median;
  data_.filter.alpha = alpha;
  return true;
}

bool DeviceConfig::setImuEnabled(bool enabled) {
  data_.imuEnabled = enabled;
  return true;
}

bool DeviceConfig::setLogging(bool enabled, const String& level, const String& mode, size_t maxBytes) {
  if (!validLogLevel(level) || !validLogMode(mode)) {
    return false;
  }
  data_.logging.enabled = enabled;
  data_.logging.level = level;
  data_.logging.mode = mode;
  if (mode == "extended") {
    data_.logging.maxBytes = maxBytes > 0 ? min(maxBytes, kExtendedLogMaxBytes) : kExtendedLogMaxBytes;
  } else {
    data_.logging.maxBytes = maxBytes > 0 ? min(maxBytes, kDefaultLogMaxBytes) : kDefaultLogMaxBytes;
  }
  return true;
}

bool DeviceConfig::setOtaConfig(bool autoApplyOnBoot, const String& manifestUrl) {
  data_.ota.autoApplyOnBoot = autoApplyOnBoot;
  data_.ota.manifestUrl = manifestUrl.isEmpty() ? String(kDefaultUpdateManifestUrl) : manifestUrl;
  return true;
}

bool DeviceConfig::setExternalLed(const String& mode, const String& preset, float brightness) {
  if (!validExternalLedMode(mode)) {
    return false;
  }
  data_.externalLed.mode = mode;
  if (!preset.isEmpty()) {
    data_.externalLed.preset = preset;
  }
  data_.externalLed.brightness = clampBrightness(brightness);
  return true;
}

bool DeviceConfig::setOled(const String& mode, const String& page, uint8_t updateHz, uint8_t contrast, uint8_t rotation) {
  if (!validOledMode(mode)) {
    return false;
  }
  data_.oled.mode = mode;
  if (!page.isEmpty()) {
    data_.oled.page = page;
  }
  data_.oled.updateHz = clampByte(updateHz, 1, 5);
  data_.oled.contrast = contrast;
  data_.oled.rotation = clampByte(rotation, 0, 3);
  return true;
}

String DeviceConfig::statusJson() const {
  String out = "{\"schema_version\":";
  out += String(static_cast<unsigned int>(data_.schemaVersion));
  out += ",\"loaded\":";
  out += loaded_ ? "true" : "false";
  out += ",\"last_error\":\"";
  out += jsonEscape(lastError_);
  out += "\"}";
  return out;
}

String DeviceConfig::filterJson() const {
  String out = "{\"enabled\":";
  out += data_.filter.enabled ? "true" : "false";
  out += ",\"median\":";
  out += String(static_cast<unsigned int>(data_.filter.median));
  out += ",\"alpha\":";
  out += String(data_.filter.alpha, 3);
  out += "}";
  return out;
}

String DeviceConfig::loggingJson() const {
  String out = "{\"enabled\":";
  out += data_.logging.enabled ? "true" : "false";
  out += ",\"level\":\"";
  out += jsonEscape(data_.logging.level);
  out += "\",\"mode\":\"";
  out += jsonEscape(data_.logging.mode);
  out += "\",\"max_bytes\":";
  out += String(static_cast<unsigned int>(data_.logging.maxBytes));
  out += ",\"effective_total_bytes\":";
  out += String(static_cast<unsigned int>(data_.logging.maxBytes * 2));
  out += "}";
  return out;
}

String DeviceConfig::otaJson() const {
  String out = "{\"auto_apply_on_boot\":";
  out += data_.ota.autoApplyOnBoot ? "true" : "false";
  out += ",\"manifest_url\":\"";
  out += jsonEscape(data_.ota.manifestUrl);
  out += "\"}";
  return out;
}

String DeviceConfig::streamBufferJson() const {
  String out = "{\"enabled\":";
  out += data_.streamBuffer.enabled ? "true" : "false";
  out += ",\"mode\":\"";
  out += jsonEscape(data_.streamBuffer.mode);
  out += "\",\"depth_frames\":";
  out += String(static_cast<unsigned int>(data_.streamBuffer.depthFrames));
  out += "}";
  return out;
}

String DeviceConfig::configJson() const {
  return toJson();
}

bool DeviceConfig::validLogLevel(const String& level) {
  return level == "error" || level == "warn" || level == "info" || level == "debug";
}

bool DeviceConfig::validLogMode(const String& mode) {
  return mode == "standard" || mode == "extended";
}

bool DeviceConfig::validStreamBufferMode(const String& mode) {
  return mode == "standard" || mode == "extended";
}

bool DeviceConfig::validExternalLedMode(const String& mode) {
  return mode == "off" || mode == "enabled";
}

bool DeviceConfig::validOledMode(const String& mode) {
  return mode == "off" || mode == "auto" || mode == "enabled";
}

void DeviceConfig::setDefaults() {
  data_.schemaVersion = 3;
  defaultMatrixLayout(data_.matrixLayout);
  data_.scanTiming.targetFps = kDefaultTargetFps;
  data_.scanTiming.settleUs = kDefaultSettleUs;
  data_.scanTiming.sendEveryNFrames = kDefaultSendEveryNFrames;
  data_.streamBuffer.enabled = true;
  data_.streamBuffer.mode = "standard";
  data_.streamBuffer.depthFrames = 3;
  data_.filter.enabled = false;
  data_.filter.median = kFilterDefaultMedian;
  data_.filter.alpha = kFilterDefaultAlpha;
  data_.imuEnabled = true;
  data_.logging.enabled = true;
  data_.logging.maxBytes = kDefaultLogMaxBytes;
  data_.logging.level = "error";
  data_.logging.mode = "standard";
  data_.ota.autoApplyOnBoot = true;
  data_.ota.manifestUrl = kDefaultUpdateManifestUrl;
  data_.externalLed.mode = "off";
  data_.externalLed.preset = "stream_health";
  data_.externalLed.brightness = 0.35f;
  data_.oled.mode = "off";
  data_.oled.page = "live_status";
  data_.oled.updateHz = 1;
  data_.oled.contrast = 128;
  data_.oled.rotation = 0;
}

bool DeviceConfig::applyJson(const String& json) {
  if (!json.startsWith("{")) {
    lastError_ = "config_invalid_json";
    return false;
  }
  const uint8_t storedSchemaVersion = static_cast<uint8_t>(extractInt(json, "schema_version", 1));
  data_.schemaVersion = 3;

  const String matrix = objectForKey(json, "matrix_layout");
  if (!matrix.isEmpty()) {
    uint8_t analog[kRows] = {0};
    uint8_t select[kCols] = {0};
    const size_t analogCount = extractArray(matrix, "analog_pins", analog, kRows);
    const size_t selectCount = extractArray(matrix, "select_pins", select, kCols);
    const bool configured = extractBool(matrix, "configured", false) || (storedSchemaVersion < 2 && analogCount && selectCount);
    if (configured && analogCount && selectCount) {
      if (!setMatrixLayout(analog, analogCount, select, selectCount)) {
        defaultMatrixLayout(data_.matrixLayout);
        lastError_ = "matrix_layout_invalid_fallback_default";
      }
    } else {
      defaultMatrixLayout(data_.matrixLayout);
    }
  }

  const String timing = objectForKey(json, "scan_timing");
  if (!timing.isEmpty()) {
    setScanTiming(
        static_cast<uint16_t>(extractInt(timing, "target_fps", data_.scanTiming.targetFps)),
        static_cast<uint16_t>(extractInt(timing, "settle_us", data_.scanTiming.settleUs)),
        static_cast<uint16_t>(extractInt(timing, "send_every_n_frames", data_.scanTiming.sendEveryNFrames)));
  }

  const String streamBuffer = objectForKey(json, "stream_buffer");
  if (!streamBuffer.isEmpty()) {
    const bool enabled = extractBool(streamBuffer, "enabled", data_.streamBuffer.enabled);
    String mode = extractString(streamBuffer, "mode", data_.streamBuffer.mode);
    if (validStreamBufferMode(mode)) {
      setStreamBuffer(enabled, mode);
    }
  } else if (storedSchemaVersion < 3) {
    setStreamBuffer(true, "standard");
  }

  const String filter = objectForKey(json, "filter");
  if (!filter.isEmpty()) {
    const bool enabled = extractBool(filter, "enabled", data_.filter.enabled);
    const uint8_t median = static_cast<uint8_t>(extractInt(filter, "median", data_.filter.median));
    const float alpha = extractFloat(filter, "alpha", data_.filter.alpha);
    if (!setFilter(enabled, median, alpha)) {
      setFilter(false, kFilterDefaultMedian, kFilterDefaultAlpha);
      lastError_ = "filter_invalid_fallback_default";
    }
  }

  const String imu = objectForKey(json, "imu");
  if (!imu.isEmpty()) {
    data_.imuEnabled = extractBool(imu, "enabled", data_.imuEnabled);
  }

  const String logging = objectForKey(json, "logging");
  if (!logging.isEmpty()) {
    const String level = extractString(logging, "level", data_.logging.level);
    const String mode = extractString(logging, "mode", data_.logging.mode);
    if (validLogLevel(level) && validLogMode(mode)) {
      setLogging(
          extractBool(logging, "enabled", data_.logging.enabled),
          level,
          mode,
          static_cast<size_t>(extractInt(logging, "max_bytes", data_.logging.maxBytes)));
    }
  }

  const String ota = objectForKey(json, "ota");
  if (!ota.isEmpty()) {
    setOtaConfig(
        extractBool(ota, "auto_apply_on_boot", data_.ota.autoApplyOnBoot),
        extractString(ota, "manifest_url", data_.ota.manifestUrl));
  }

  const String indicators = objectForKey(json, "indicators");
  const String external = objectForKey(indicators, "external_led");
  if (!external.isEmpty()) {
    const String mode = extractString(external, "mode", data_.externalLed.mode);
    const String preset = extractString(external, "preset", data_.externalLed.preset);
    if (validExternalLedMode(mode)) {
      setExternalLed(mode, preset, extractFloat(external, "brightness", data_.externalLed.brightness));
    }
  }
  const String oled = objectForKey(indicators, "oled");
  if (!oled.isEmpty()) {
    const String mode = extractString(oled, "mode", data_.oled.mode);
    if (validOledMode(mode)) {
      setOled(
          mode,
          extractString(oled, "page", data_.oled.page),
          static_cast<uint8_t>(extractInt(oled, "update_hz", data_.oled.updateHz)),
          static_cast<uint8_t>(extractInt(oled, "contrast", data_.oled.contrast)),
          static_cast<uint8_t>(extractInt(oled, "rotation", data_.oled.rotation)));
    }
  }

  return true;
}

String DeviceConfig::toJson() const {
  String out = "{";
  out += "\"schema_version\":";
  out += String(static_cast<unsigned int>(data_.schemaVersion));
  out += ",\"matrix_layout\":{\"configured\":";
  out += data_.matrixLayout.analogCount > 0 && data_.matrixLayout.selectCount > 0 ? "true" : "false";
  out += ",\"analog_pins\":";
  appendArray(out, data_.matrixLayout.analogPins, data_.matrixLayout.analogCount);
  out += ",\"select_pins\":";
  appendArray(out, data_.matrixLayout.selectPins, data_.matrixLayout.selectCount);
  out += "},\"scan_timing\":{\"target_fps\":";
  out += data_.scanTiming.targetFps;
  out += ",\"settle_us\":";
  out += data_.scanTiming.settleUs;
  out += ",\"send_every_n_frames\":";
  out += data_.scanTiming.sendEveryNFrames;
  out += "},\"stream_buffer\":{\"enabled\":";
  out += data_.streamBuffer.enabled ? "true" : "false";
  out += ",\"mode\":\"";
  out += jsonEscape(data_.streamBuffer.mode);
  out += "\",\"depth_frames\":";
  out += String(static_cast<unsigned int>(data_.streamBuffer.depthFrames));
  out += "},\"filter\":";
  out += filterJson();
  out += ",\"imu\":{\"enabled\":";
  out += data_.imuEnabled ? "true" : "false";
  out += "},\"logging\":";
  out += loggingJson();
  out += ",\"ota\":";
  out += otaJson();
  out += ",\"indicators\":{\"external_led\":{\"mode\":\"";
  out += jsonEscape(data_.externalLed.mode);
  out += "\",\"preset\":\"";
  out += jsonEscape(data_.externalLed.preset);
  out += "\",\"brightness\":";
  out += String(data_.externalLed.brightness, 2);
  out += "},\"oled\":{\"mode\":\"";
  out += jsonEscape(data_.oled.mode);
  out += "\",\"page\":\"";
  out += jsonEscape(data_.oled.page);
  out += "\",\"update_hz\":";
  out += String(static_cast<unsigned int>(data_.oled.updateHz));
  out += ",\"contrast\":";
  out += String(static_cast<unsigned int>(data_.oled.contrast));
  out += ",\"rotation\":";
  out += String(static_cast<unsigned int>(data_.oled.rotation));
  out += "}}}";
  return out;
}

String DeviceConfig::lastErrorJson() const {
  String out = "\"";
  out += jsonEscape(lastError_);
  out += "\"";
  return out;
}

}  // namespace nhos
