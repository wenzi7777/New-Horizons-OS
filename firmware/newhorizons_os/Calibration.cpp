#include "Calibration.h"

#include <FS.h>
#include <SPIFFS.h>

#include <algorithm>
#include <cmath>

#include "Storage.h"

namespace nhos {

namespace {

constexpr char kCalibrationMetaPath[] = "/cal/meta";
constexpr char kCalibrationDirPath[] = "/cal";
constexpr char kCalibrationTarePath[] = "/cal/tare";
// Profiles from before the fitted model (interpolated curves, and the older
// /calibration layout) are not migrated: a device on this firmware starts
// uncalibrated and is calibrated again.
constexpr int kCalibrationFormat = 2;
constexpr char kLegacyCalibrationDirPath[] = "/calibration";
// Failed sensors listed by index in status; the rest are only counted, so a
// fresh 225-sensor board does not blow up every status reply.
constexpr size_t kMaxListedFitFailures = 16;
// GPIO11 and up are on ADC2 (ESP32-S3: ADC1 = GPIO1-10, ADC2 = GPIO11-20).
constexpr uint8_t kFirstAdc2Gpio = 11;

String trimFloat(float value, unsigned int decimals = 3) {
  String out(value, decimals);
  if (out.indexOf('.') >= 0) {
    while (out.endsWith("0")) {
      out.remove(out.length() - 1);
    }
    if (out.endsWith(".")) {
      out.remove(out.length() - 1);
    }
  }
  return out.isEmpty() ? String("0") : out;
}

// Readout and fit coefficients span many decades (c2 ~ 1e-4), so they are
// written in scientific notation rather than trimFloat's fixed 3 decimals.
String sciFloat(float value) {
  if (std::isnan(value)) {
    return "null";
  }
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%.7g", static_cast<double>(value));
  return buffer;
}

void parsePinCsv(const String& csv, uint8_t* out, size_t& count, size_t maxCount) {
  count = 0;
  int cursor = 0;
  while (cursor <= csv.length() && count < maxCount) {
    int sep = csv.indexOf(',', cursor);
    if (sep < 0) {
      sep = csv.length();
    }
    String token = csv.substring(cursor, sep);
    token.trim();
    if (token.length()) {
      out[count++] = static_cast<uint8_t>(token.toInt());
    }
    cursor = sep + 1;
  }
}

String pinCsv(const uint8_t* pins, size_t count) {
  String out;
  for (size_t i = 0; i < count; ++i) {
    if (i) {
      out += ",";
    }
    out += String(static_cast<unsigned int>(pins[i]));
  }
  return out;
}

void parseFloatCsv(const String& content, int cursor, std::vector<float>& out) {
  size_t index = 0;
  while (cursor <= content.length() && index < out.size()) {
    int sep = content.indexOf(',', cursor);
    if (sep < 0) {
      sep = content.length();
    }
    String token = content.substring(cursor, sep);
    token.trim();
    if (token.length()) {
      out[index] = token.toFloat();
    }
    ++index;
    cursor = sep + 1;
  }
}

String floatCsv(const std::vector<float>& values) {
  String content;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) {
      content += ",";
    }
    if (!std::isnan(values[i])) {
      content += trimFloat(values[i]);
    }
  }
  return content;
}

}  // namespace

void Calibration::begin(Storage& storage) {
  storage_ = &storage;
  loadFromStorage();
  logEvaluationCost();
}

void Calibration::setLayout(const uint8_t* analogPins, size_t analogCount, const uint8_t* selectPins, size_t selectCount) {
  analogCount_ = std::min(analogCount, static_cast<size_t>(kRows));
  selectCount_ = std::min(selectCount, static_cast<size_t>(kCols));
  memset(analogPins_, 0, sizeof(analogPins_));
  memset(selectPins_, 0, sizeof(selectPins_));
  if (analogPins && analogCount_ > 0) {
    memcpy(analogPins_, analogPins, analogCount_);
  }
  if (selectPins && selectCount_ > 0) {
    memcpy(selectPins_, selectPins, selectCount_);
  }
  if (!tare_.empty() && tare_.size() != totalPointCount()) {
    tare_.assign(totalPointCount(), NAN);
    enabled_ = false;
  }
  refreshSavedStateCache();
}

String Calibration::statusJson(bool modeActive) const {
  // Status keys intentionally include "draft_levels", "draft_tare",
  // "draft_fit" and "metadata" for the WebUI maintenance workbench.
  String out = "{";
  out += "\"enabled\":";
  out += enabled_ ? "true" : "false";
  out += ",\"mode_active\":";
  out += modeActive ? "true" : "false";
  out += ",\"session_active\":";
  out += sessionActive_ ? "true" : "false";
  out += ",\"complete\":";
  out += complete() ? "true" : "false";
  out += ",\"tare_complete\":";
  out += savedTareComplete_ ? "true" : "false";
  out += ",\"tare_enabled\":";
  out += tareEnabled_ ? "true" : "false";
  out += ",\"output_mode\":\"";
  out += calibration_curve::outputModeName(outputMode());
  out += "\",\"tare\":";
  out += tareSummaryJson(tare_, "saved");
  out += ",\"draft_tare\":";
  out += tareSummaryJson(draftTare_, "draft");
  out += ",\"levels\":";
  out += levelsSummaryJson(levels_, "saved");
  out += ",\"draft_levels\":";
  out += levelsSummaryJson(draftLevels_, "draft");
  out += ",\"fit\":";
  out += fitSummaryJson(fits_);
  out += ",\"draft_fit\":";
  if (sessionActive_) {
    // Fitted on demand: it is what Save would produce, so the workbench can
    // show which sensors still need levels before committing.
    FitSet draftFits;
    fitAll(draftTare_, draftLevels_, draftFits);
    out += fitSummaryJson(draftFits);
  } else {
    out += "null";
  }
  out += ",\"fit_settings\":";
  out += fitSettingsJson();
  out += ",\"metadata\":";
  out += metadataJson();
  out += "}";
  return out;
}

bool Calibration::sessionBegin() {
  if (sessionActive_) {
    return true;
  }
  draftTare_ = tare_;
  draftLevels_ = levels_;
  sessionActive_ = true;
  return true;
}

void Calibration::sessionAbort() {
  draftTare_.clear();
  draftLevels_.clear();
  sessionActive_ = false;
}

bool Calibration::sessionCommit(bool autoEnable, String& outError) {
  if (!sessionActive_) {
    outError = "calibration_session_required";
    return false;
  }

  if (autoEnable) {
    FitSet draftFits;
    fitAll(draftTare_, draftLevels_, draftFits);
    if (!tareComplete(draftTare_) || draftFits.fitted < totalPointCount()) {
      outError = "calibration_incomplete";
      return false;
    }
  }

  tare_ = draftTare_;
  levels_ = draftLevels_;
  draftTare_.clear();
  draftLevels_.clear();
  sessionActive_ = false;
  refreshSavedStateCache();

  if (!runtimeReady_) {
    enabled_ = false;
  } else if (autoEnable) {
    enabled_ = true;
  }

  if (createdAtMs_ == 0) {
    createdAtMs_ = millis();
  }
  updatedAtMs_ = millis();
  if (!saveToStorage()) {
    outError = "calibration_save_failed";
    return false;
  }
  return true;
}

bool Calibration::sessionActive() const {
  return sessionActive_;
}

bool Calibration::enabled() const {
  return enabled_;
}

bool Calibration::complete() const {
  return runtimeReady_;
}

bool Calibration::setEnabled(bool enabled, String& outError) {
  if (enabled && !complete()) {
    outError = "calibration_incomplete";
    return false;
  }
  enabled_ = enabled;
  updatedAtMs_ = millis();
  if (!saveToStorage()) {
    outError = "calibration_save_failed";
    return false;
  }
  return true;
}

bool Calibration::clearProfile() {
  enabled_ = false;
  tareEnabled_ = false;
  tare_.clear();
  draftTare_.clear();
  levels_.clear();
  draftLevels_.clear();
  sessionActive_ = false;
  createdAtMs_ = 0;
  updatedAtMs_ = millis();
  refreshSavedStateCache();
  return saveToStorage();
}

bool Calibration::deleteLevel(float level) {
  std::vector<LevelData>& target = sessionActive_ ? draftLevels_ : levels_;
  const int32_t key = levelKey(level);
  const size_t before = target.size();
  target.erase(
      std::remove_if(target.begin(), target.end(), [key](const LevelData& item) { return item.key == key; }),
      target.end());
  if (target.size() == before) {
    return false;
  }
  if (!sessionActive_) {
    refreshSavedStateCache();
    if (!complete()) {
      enabled_ = false;
    }
    updatedAtMs_ = millis();
    if (!saveToStorage()) {
      return false;
    }
  }
  return true;
}

bool Calibration::dumpTareJson(String& out) const {
  const std::vector<float>* saved = tare_.empty() ? nullptr : &tare_;
  const std::vector<float>* draft = draftTare_.empty() ? nullptr : &draftTare_;
  if (!saved && !draft) {
    return false;
  }
  out = "{";
  out += "\"total_points\":";
  out += String(static_cast<unsigned int>(totalPointCount()));
  out += ",\"saved\":";
  out += tareLayerJson(saved);
  out += ",\"draft\":";
  out += tareLayerJson(draft);
  out += ",\"session_active\":";
  out += sessionActive_ ? "true" : "false";
  out += "}";
  return true;
}

bool Calibration::dumpLevelJson(float level, String& out) const {
  const LevelData* saved = findLevel(levels_, level);
  const LevelData* draft = findLevel(draftLevels_, level);
  if (!saved && !draft) {
    return false;
  }
  auto appendLevel = [this](String& buffer, const LevelData* item) {
    if (!item) {
      buffer += "null";
      return;
    }
    const size_t total = totalPointCount();
    const size_t captured = capturedCount(*item);
    buffer += "{\"level\":";
    buffer += floatLabel(keyLevel(item->key));
    buffer += ",\"reference\":";
    buffer += floatLabel(item->reference);
    buffer += ",\"captured_points\":";
    buffer += String(static_cast<unsigned int>(captured));
    buffer += ",\"total_points\":";
    buffer += String(static_cast<unsigned int>(total));
    buffer += ",\"complete\":";
    buffer += captured >= total && total > 0 ? "true" : "false";
    buffer += ",\"cells\":[";
    for (size_t sensorIndex = 0; sensorIndex < total; ++sensorIndex) {
      if (sensorIndex) {
        buffer += ",";
      }
      const float value = sensorIndex < item->values.size() ? item->values[sensorIndex] : NAN;
      buffer += "{\"sensor_index\":";
      buffer += String(static_cast<unsigned int>(sensorIndex));
      buffer += ",\"row\":";
      buffer += String(static_cast<unsigned int>(analogCount_ ? (sensorIndex % analogCount_) : 0));
      buffer += ",\"col\":";
      buffer += String(static_cast<unsigned int>(analogCount_ ? (sensorIndex / analogCount_) : 0));
      buffer += ",\"calibrated\":";
      buffer += !std::isnan(value) ? "true" : "false";
      buffer += ",\"value\":";
      buffer += std::isnan(value) ? "null" : floatLabel(value);
      buffer += "}";
    }
    buffer += "]}";
  };

  out = "{";
  out += "\"level\":";
  out += floatLabel(keyLevel(levelKey(level)));
  out += ",\"total_points\":";
  out += String(static_cast<unsigned int>(totalPointCount()));
  out += ",\"saved\":";
  appendLevel(out, saved);
  out += ",\"draft\":";
  appendLevel(out, draft);
  out += ",\"session_active\":";
  out += sessionActive_ ? "true" : "false";
  out += "}";
  return true;
}

String Calibration::dumpFitJson() const {
  const size_t total = totalPointCount();
  String out = "{\"model\":\"a+b*sqrt(x)+c*x\",\"total_points\":";
  out += String(static_cast<unsigned int>(total));
  out += ",\"settings\":";
  out += fitSettingsJson();
  out += ",\"cells\":[";
  for (size_t sensorIndex = 0; sensorIndex < total; ++sensorIndex) {
    if (sensorIndex) {
      out += ",";
    }
    const calibration_curve::FitModel model =
        sensorIndex < fits_.models.size() ? fits_.models[sensorIndex] : calibration_curve::FitModel();
    const FitResult result = sensorIndex < fits_.results.size() ? fits_.results[sensorIndex] : FitResult::NoTare;
    out += "{\"sensor_index\":";
    out += String(static_cast<unsigned int>(sensorIndex));
    out += ",\"adc\":";
    out += String(static_cast<unsigned int>(adcIndexForSensor(sensorIndex) + 1));
    out += ",\"result\":\"";
    out += fitResultName(result);
    out += "\",\"a\":";
    out += sciFloat(model.a);
    out += ",\"b\":";
    out += sciFloat(model.b);
    out += ",\"c\":";
    out += sciFloat(model.c);
    out += "}";
  }
  out += "]}";
  return out;
}

bool Calibration::captureTare(const float* values, size_t count) {
  if (!sessionActive_ || !values) {
    return false;
  }
  const size_t total = totalPointCount();
  if (total == 0 || count < total) {
    return false;
  }
  draftTare_.assign(values, values + total);
  return true;
}

bool Calibration::captureCell(uint16_t sensorIndex, float level, float reference, float value) {
  if (!sessionActive_ || sensorIndex >= totalPointCount() || !draftTareCaptured(sensorIndex)) {
    return false;
  }
  LevelData* item = mutableLevel(draftLevels_, level, true);
  if (!item) {
    return false;
  }
  item->reference = reference;
  item->values[sensorIndex] = value - draftTare_[sensorIndex];
  return true;
}

bool Calibration::captureAll(float level, float reference, const float* values, size_t count) {
  if (!sessionActive_ || !values) {
    return false;
  }
  const size_t total = totalPointCount();
  if (total == 0 || count < total) {
    return false;
  }
  if (!draftTareCaptured()) {
    return false;
  }
  LevelData* item = mutableLevel(draftLevels_, level, true);
  if (!item) {
    return false;
  }
  item->reference = reference;
  for (size_t i = 0; i < total; ++i) {
    item->values[i] = values[i] - draftTare_[i];
  }
  return true;
}

bool Calibration::setReference(float level, float reference) {
  if (std::isnan(reference)) {
    return false;
  }
  std::vector<LevelData>& target = sessionActive_ ? draftLevels_ : levels_;
  LevelData* item = mutableLevel(target, level, false);
  if (!item) {
    return false;
  }
  item->reference = reference;
  if (!sessionActive_) {
    refreshSavedStateCache();
    if (!complete()) {
      enabled_ = false;
    }
    updatedAtMs_ = millis();
    return saveToStorage();
  }
  return true;
}

bool Calibration::draftTareCaptured(int sensorIndex) const {
  if (!sessionActive_) {
    return false;
  }
  if (sensorIndex < 0) {
    return tareComplete(draftTare_);
  }
  const size_t index = static_cast<size_t>(sensorIndex);
  return index < draftTare_.size() && !std::isnan(draftTare_[index]);
}

bool Calibration::applyTareDirect(const float* values, size_t count) {
  const size_t total = totalPointCount();
  if (total == 0 || !values || count < total || sessionActive_) {
    return false;
  }
  tare_.assign(values, values + total);
  tareEnabled_ = true;
  refreshSavedStateCache();
  if (!complete()) {
    enabled_ = false;
  }
  updatedAtMs_ = millis();
  return saveToStorage();
}

bool Calibration::clearTare() {
  if (sessionActive_) {
    return false;
  }
  tareEnabled_ = false;
  updatedAtMs_ = millis();
  return saveToStorage();
}

bool Calibration::tareEnabled() const {
  return tareEnabled_;
}

bool Calibration::setFitSettings(const FitSettings& settings) {
  for (const calibration_curve::Readout& readout : settings.readout) {
    if (std::isnan(readout.c1) || std::isnan(readout.c2) || readout.c1 <= 0.0f) {
      return false;
    }
  }
  if (std::isnan(settings.minLevel) || std::isnan(settings.clipMv[0]) || std::isnan(settings.clipMv[1])) {
    return false;
  }
  fitSettings_ = settings;
  refreshSavedStateCache();
  if (!complete()) {
    enabled_ = false;
  }
  updatedAtMs_ = millis();
  return saveToStorage();
}

calibration_curve::OutputMode Calibration::outputMode() const {
  return calibration_curve::resolveOutputMode(enabled_, runtimeReady_, tareEnabled_, savedTareComplete_);
}

bool Calibration::apply(float rawMv, uint16_t sensorIndex, float& outValue) const {
  outValue = rawMv;
  if (sensorIndex >= totalPointCount() || sensorIndex >= tare_.size()) {
    return false;
  }
  const float tareValue = tare_[sensorIndex];
  if (std::isnan(tareValue)) {
    return false;
  }
  switch (outputMode()) {
    case calibration_curve::OutputMode::Calibrated: {
      if (sensorIndex >= fits_.models.size()) {
        return false;
      }
      const float x = calibration_curve::readoutX(calibration_curve::applyTare(rawMv, tareValue),
                                                  fitSettings_.readout[adcIndexForSensor(sensorIndex)]);
      outValue = calibration_curve::evaluateModel(fits_.models[sensorIndex], x);
      return true;
    }
    case calibration_curve::OutputMode::Tared:
      outValue = calibration_curve::applyTare(rawMv, tareValue);
      return true;
    case calibration_curve::OutputMode::Raw:
    default:
      return false;
  }
}

Calibration::LevelData* Calibration::mutableLevel(std::vector<LevelData>& levels, float level, bool createIfMissing) {
  const int32_t key = levelKey(level);
  for (LevelData& item : levels) {
    if (item.key == key) {
      if (item.values.size() < totalPointCount()) {
        item.values.resize(totalPointCount(), NAN);
      }
      return &item;
    }
  }
  if (!createIfMissing) {
    return nullptr;
  }
  LevelData item;
  item.key = key;
  item.reference = keyLevel(key);
  item.values.assign(totalPointCount(), NAN);
  levels.push_back(item);
  std::sort(levels.begin(), levels.end(), [](const LevelData& a, const LevelData& b) { return a.key < b.key; });
  for (LevelData& entry : levels) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

const Calibration::LevelData* Calibration::findLevel(const std::vector<LevelData>& levels, float level) const {
  const int32_t key = levelKey(level);
  for (const LevelData& item : levels) {
    if (item.key == key) {
      return &item;
    }
  }
  return nullptr;
}

bool Calibration::tareComplete(const std::vector<float>& tare) const {
  const size_t total = totalPointCount();
  return total > 0 && capturedCount(tare) >= total;
}

void Calibration::fitAll(const std::vector<float>& tare, const std::vector<LevelData>& levels, FitSet& out) const {
  const size_t total = totalPointCount();
  out.models.assign(total, calibration_curve::FitModel());
  out.results.assign(total, FitResult::NoTare);
  out.fitted = 0;
  std::vector<float> xs;
  std::vector<float> pressures;
  xs.reserve(levels.size());
  pressures.reserve(levels.size());
  for (size_t sensorIndex = 0; sensorIndex < total; ++sensorIndex) {
    if (sensorIndex >= tare.size() || std::isnan(tare[sensorIndex])) {
      continue;
    }
    const size_t adc = adcIndexForSensor(sensorIndex);
    xs.clear();
    pressures.clear();
    // The same point selection as make_figures.py: levels from the nominal
    // minimum up, no clipped reading, and a positive response.
    for (const LevelData& item : levels) {
      if (keyLevel(item.key) < fitSettings_.minLevel || sensorIndex >= item.values.size()) {
        continue;
      }
      const float relative = item.values[sensorIndex];
      if (std::isnan(relative) || tare[sensorIndex] + relative >= fitSettings_.clipMv[adc]) {
        continue;
      }
      const float x = calibration_curve::readoutX(relative, fitSettings_.readout[adc]);
      if (std::isnan(x) || x <= 0.0f) {
        continue;
      }
      xs.push_back(x);
      pressures.push_back(item.reference);
    }
    if (xs.size() < calibration_curve::kMinFitPoints) {
      out.results[sensorIndex] = FitResult::TooFewPoints;
      continue;
    }
    if (!calibration_curve::fitModel(xs.data(), pressures.data(), xs.size(), out.models[sensorIndex])) {
      out.results[sensorIndex] = FitResult::Singular;
      continue;
    }
    out.results[sensorIndex] = FitResult::Fitted;
    ++out.fitted;
  }
}

size_t Calibration::adcIndexForSensor(size_t sensorIndex) const {
  if (analogCount_ == 0) {
    return 0;
  }
  return analogPins_[sensorIndex % analogCount_] >= kFirstAdc2Gpio ? 1 : 0;
}

void Calibration::refreshSavedStateCache() {
  savedTareComplete_ = tareComplete(tare_);
  fitAll(tare_, levels_, fits_);
  runtimeReady_ = savedTareComplete_ && totalPointCount() > 0 && fits_.fitted == totalPointCount();
}

void Calibration::logEvaluationCost() const {
  // What apply() costs per sample, measured on this board: the readout
  // inversion plus the model, with the paper's (non-identity) readout so the
  // square root in the inversion is included.
  calibration_curve::FitModel model;
  model.a = -1.77f;
  model.b = 1.44f;
  model.c = -0.0141f;
  calibration_curve::Readout readout;
  readout.c1 = 2.6657f;
  readout.c2 = -9.447e-05f;
  constexpr int kRuns = 1000;
  volatile float sink = 0;
  const uint32_t startedUs = micros();
  for (int i = 0; i < kRuns; ++i) {
    sink = sink + calibration_curve::evaluateModel(model, calibration_curve::readoutX(10.0f + i, readout));
  }
  const uint32_t elapsedUs = micros() - startedUs;
  Serial.print(F("[calibration] eval_us_per_cell="));
  Serial.println(static_cast<float>(elapsedUs) / kRuns, 3);
}

bool Calibration::loadFromStorage() {
  enabled_ = false;
  tareEnabled_ = false;
  sessionActive_ = false;
  savedTareComplete_ = false;
  runtimeReady_ = false;
  tare_.clear();
  draftTare_.clear();
  levels_.clear();
  draftLevels_.clear();
  createdAtMs_ = 0;
  updatedAtMs_ = 0;

  String meta;
  const bool hasMeta = SPIFFS.exists(kCalibrationMetaPath) && storage_ && storage_->readTextFile(kCalibrationMetaPath, meta);
  int format = 0;
  if (hasMeta) {
    const int at = meta.indexOf("format=");
    format = at >= 0 ? meta.substring(at + 7).toInt() : 0;
  }
  if (format != kCalibrationFormat) {
    if (hasMeta || SPIFFS.exists(kCalibrationTarePath) || SPIFFS.exists(String(kLegacyCalibrationDirPath) + "/profile.meta")) {
      Serial.println(F("cal_profile_discarded reason=old_format"));
    }
    wipeStoredProfile();
    refreshSavedStateCache();
    return true;
  }

  int cursor = 0;
  while (cursor <= meta.length()) {
    int end = meta.indexOf('\n', cursor);
    if (end < 0) {
      end = meta.length();
    }
    String line = meta.substring(cursor, end);
    line.trim();
    const int sep = line.indexOf('=');
    if (sep > 0) {
      String key = line.substring(0, sep);
      String value = line.substring(sep + 1);
      key.trim();
      value.trim();
      if (key == "enabled") {
        enabled_ = value == "1";
      } else if (key == "tare_enabled") {
        tareEnabled_ = value == "1";
      } else if (key == "created_at_ms") {
        createdAtMs_ = static_cast<uint32_t>(value.toInt());
      } else if (key == "updated_at_ms") {
        updatedAtMs_ = static_cast<uint32_t>(value.toInt());
      } else if (key == "analog_pins" && analogCount_ == 0) {
        parsePinCsv(value, analogPins_, analogCount_, kRows);
      } else if (key == "select_pins" && selectCount_ == 0) {
        parsePinCsv(value, selectPins_, selectCount_, kCols);
      } else if (key == "fit_min_level") {
        fitSettings_.minLevel = value.toFloat();
      } else if (key == "adc1_c1") {
        fitSettings_.readout[0].c1 = value.toFloat();
      } else if (key == "adc1_c2") {
        fitSettings_.readout[0].c2 = value.toFloat();
      } else if (key == "adc2_c1") {
        fitSettings_.readout[1].c1 = value.toFloat();
      } else if (key == "adc2_c2") {
        fitSettings_.readout[1].c2 = value.toFloat();
      } else if (key == "adc1_clip_mv") {
        fitSettings_.clipMv[0] = value.toFloat();
      } else if (key == "adc2_clip_mv") {
        fitSettings_.clipMv[1] = value.toFloat();
      }
    }
    cursor = end + 1;
  }

  File root = SPIFFS.open(kCalibrationDirPath);
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      const String name = file.path();
      file.close();
      if (name.startsWith(String(kCalibrationDirPath) + "/l")) {
        const String base = name.substring(name.lastIndexOf('/') + 2);
        const long magnitude = base.substring(1).toInt();
        loadLevelFile(name, static_cast<int32_t>(base.startsWith("n") ? -magnitude : magnitude));
      } else if (name == kCalibrationTarePath) {
        loadTareFile(name);
      }
      file = root.openNextFile();
    }
  }

  refreshSavedStateCache();
  if (!complete()) {
    enabled_ = false;
  }
  Serial.print(F("cal_loaded tare_points="));
  Serial.print(static_cast<unsigned int>(capturedCount(tare_)));
  Serial.print(F(" levels="));
  Serial.print(static_cast<unsigned int>(levels_.size()));
  Serial.print(F(" fitted="));
  Serial.print(static_cast<unsigned int>(fits_.fitted));
  Serial.print(F("/"));
  Serial.print(static_cast<unsigned int>(totalPointCount()));
  Serial.print(F(" enabled="));
  Serial.println(enabled_ ? F("true") : F("false"));
  return true;
}

bool Calibration::saveToStorage() {
  if (!storage_) {
    return false;
  }
  if (!ensureCalibrationDir()) {
    return false;
  }
  if (!tare_.empty() && !writeTareFile(tare_)) {
    return false;
  }
  for (const LevelData& item : levels_) {
    if (!writeLevelFile(item)) {
      return false;
    }
  }
  String meta;
  meta += "format=";
  meta += String(kCalibrationFormat);
  meta += "\nenabled=";
  meta += enabled_ ? "1" : "0";
  meta += "\ntare_enabled=";
  meta += tareEnabled_ ? "1" : "0";
  meta += "\ncreated_at_ms=";
  meta += String(createdAtMs_);
  meta += "\nupdated_at_ms=";
  meta += String(updatedAtMs_);
  meta += "\nanalog_pins=";
  meta += pinCsv(analogPins_, analogCount_);
  meta += "\nselect_pins=";
  meta += pinCsv(selectPins_, selectCount_);
  meta += "\nfit_min_level=";
  meta += sciFloat(fitSettings_.minLevel);
  meta += "\nadc1_c1=";
  meta += sciFloat(fitSettings_.readout[0].c1);
  meta += "\nadc1_c2=";
  meta += sciFloat(fitSettings_.readout[0].c2);
  meta += "\nadc2_c1=";
  meta += sciFloat(fitSettings_.readout[1].c1);
  meta += "\nadc2_c2=";
  meta += sciFloat(fitSettings_.readout[1].c2);
  meta += "\nadc1_clip_mv=";
  meta += sciFloat(fitSettings_.clipMv[0]);
  meta += "\nadc2_clip_mv=";
  meta += sciFloat(fitSettings_.clipMv[1]);
  meta += "\n";
  if (!storage_->writeTextFileAtomic(kCalibrationMetaPath, meta)) {
    return false;
  }
  removeObsoleteStoredFiles();
  return true;
}

bool Calibration::ensureCalibrationDir() const {
  SPIFFS.mkdir(kCalibrationDirPath);
  File root = SPIFFS.open(kCalibrationDirPath);
  return root && root.isDirectory();
}

void Calibration::wipeStoredProfile() const {
  // SPIFFS is flat: a "directory" is a path prefix, so every file under
  // either prefix goes.
  for (const char* dirPath : {kCalibrationDirPath, kLegacyCalibrationDirPath}) {
    File root = SPIFFS.open(dirPath);
    if (!root || !root.isDirectory()) {
      continue;
    }
    std::vector<String> names;
    File file = root.openNextFile();
    while (file) {
      names.push_back(file.path());
      file.close();
      file = root.openNextFile();
    }
    for (const String& name : names) {
      SPIFFS.remove(name);
    }
  }
}

void Calibration::removeObsoleteStoredFiles() const {
  File root = SPIFFS.open(kCalibrationDirPath);
  if (!root || !root.isDirectory()) {
    return;
  }
  File file = root.openNextFile();
  while (file) {
    const String name = file.path();
    file.close();
    bool keep = name == kCalibrationMetaPath || (!tare_.empty() && name == kCalibrationTarePath);
    for (const LevelData& item : levels_) {
      if (name == levelPath(item.key)) {
        keep = true;
        break;
      }
    }
    if (!keep && (name == kCalibrationTarePath || name.startsWith(String(kCalibrationDirPath) + "/l") || name.endsWith(".tmp"))) {
      SPIFFS.remove(name);
    }
    file = root.openNextFile();
  }
}

bool Calibration::writeTareFile(const std::vector<float>& tare) const {
  if (!storage_) {
    return false;
  }
  return storage_->writeTextFileAtomic(tarePath(), floatCsv(tare));
}

bool Calibration::writeLevelFile(const LevelData& level) const {
  if (!storage_) {
    return false;
  }
  // First line: the reference pressure; second: tare-relative mV per sensor.
  String content = "reference=";
  content += sciFloat(level.reference);
  content += "\n";
  content += floatCsv(level.values);
  return storage_->writeTextFileAtomic(levelPath(level.key), content);
}

bool Calibration::loadTareFile(const String& path) {
  String content;
  if (!storage_ || !storage_->readTextFile(path, content)) {
    return false;
  }
  tare_.assign(totalPointCount(), NAN);
  parseFloatCsv(content, 0, tare_);
  return true;
}

bool Calibration::loadLevelFile(const String& path, int32_t key) {
  String content;
  if (!storage_ || !storage_->readTextFile(path, content)) {
    return false;
  }
  const int newline = content.indexOf('\n');
  if (!content.startsWith("reference=") || newline < 0) {
    return false;
  }
  LevelData item;
  item.key = key;
  item.reference = content.substring(10, newline).toFloat();
  item.values.assign(totalPointCount(), NAN);
  parseFloatCsv(content, newline + 1, item.values);
  levels_.push_back(item);
  std::sort(levels_.begin(), levels_.end(), [](const LevelData& a, const LevelData& b) { return a.key < b.key; });
  return true;
}

String Calibration::tareSummaryJson(const std::vector<float>& tare, const char* source) const {
  const size_t total = totalPointCount();
  const size_t captured = capturedCount(tare);
  String out = "{";
  out += "\"captured_points\":";
  out += String(static_cast<unsigned int>(captured));
  out += ",\"total_points\":";
  out += String(static_cast<unsigned int>(total));
  out += ",\"missing_points\":";
  out += String(static_cast<unsigned int>(total > captured ? total - captured : 0));
  out += ",\"complete\":";
  out += captured >= total && total > 0 ? "true" : "false";
  out += ",\"source\":\"";
  out += source;
  out += "\"}";
  return out;
}

String Calibration::tareLayerJson(const std::vector<float>* tare) const {
  if (!tare) {
    return "null";
  }
  const size_t total = totalPointCount();
  const size_t captured = capturedCount(*tare);
  String out = "{";
  out += "\"captured_points\":";
  out += String(static_cast<unsigned int>(captured));
  out += ",\"total_points\":";
  out += String(static_cast<unsigned int>(total));
  out += ",\"complete\":";
  out += captured >= total && total > 0 ? "true" : "false";
  out += ",\"cells\":[";
  for (size_t sensorIndex = 0; sensorIndex < total; ++sensorIndex) {
    if (sensorIndex) {
      out += ",";
    }
    const float value = sensorIndex < tare->size() ? (*tare)[sensorIndex] : NAN;
    out += "{\"sensor_index\":";
    out += String(static_cast<unsigned int>(sensorIndex));
    out += ",\"row\":";
    out += String(static_cast<unsigned int>(analogCount_ ? (sensorIndex % analogCount_) : 0));
    out += ",\"col\":";
    out += String(static_cast<unsigned int>(analogCount_ ? (sensorIndex / analogCount_) : 0));
    out += ",\"calibrated\":";
    out += !std::isnan(value) ? "true" : "false";
    out += ",\"value\":";
    out += std::isnan(value) ? "null" : floatLabel(value);
    out += "}";
  }
  out += "]}";
  return out;
}

String Calibration::levelsSummaryJson(const std::vector<LevelData>& levels, const char* source) const {
  String out = "[";
  const size_t total = totalPointCount();
  for (size_t i = 0; i < levels.size(); ++i) {
    const LevelData& item = levels[i];
    if (i) {
      out += ",";
    }
    const size_t captured = capturedCount(item);
    out += "{\"level\":";
    out += floatLabel(keyLevel(item.key));
    out += ",\"reference\":";
    out += floatLabel(item.reference);
    out += ",\"captured_points\":";
    out += String(static_cast<unsigned int>(captured));
    out += ",\"total_points\":";
    out += String(static_cast<unsigned int>(total));
    out += ",\"missing_points\":";
    out += String(static_cast<unsigned int>(total > captured ? total - captured : 0));
    out += ",\"complete\":";
    out += captured >= total && total > 0 ? "true" : "false";
    out += ",\"source\":\"";
    out += source;
    out += "\"}";
  }
  out += "]";
  return out;
}

String Calibration::fitSummaryJson(const FitSet& fits) const {
  const size_t total = totalPointCount();
  size_t counts[4] = {0, 0, 0, 0};
  String failed = "[";
  size_t listed = 0;
  for (size_t i = 0; i < fits.results.size(); ++i) {
    const FitResult result = fits.results[i];
    ++counts[static_cast<size_t>(result)];
    if (result == FitResult::Fitted || listed >= kMaxListedFitFailures) {
      continue;
    }
    if (listed++) {
      failed += ",";
    }
    failed += "{\"sensor_index\":";
    failed += String(static_cast<unsigned int>(i));
    failed += ",\"reason\":\"";
    failed += fitResultName(result);
    failed += "\"}";
  }
  failed += "]";
  String out = "{\"cells_total\":";
  out += String(static_cast<unsigned int>(total));
  out += ",\"cells_fitted\":";
  out += String(static_cast<unsigned int>(fits.fitted));
  out += ",\"complete\":";
  out += total > 0 && fits.fitted == total ? "true" : "false";
  out += ",\"failures\":{\"no_tare\":";
  out += String(static_cast<unsigned int>(counts[static_cast<size_t>(FitResult::NoTare)]));
  out += ",\"too_few_points\":";
  out += String(static_cast<unsigned int>(counts[static_cast<size_t>(FitResult::TooFewPoints)]));
  out += ",\"singular\":";
  out += String(static_cast<unsigned int>(counts[static_cast<size_t>(FitResult::Singular)]));
  out += "},\"failed\":";
  out += failed;
  out += "}";
  return out;
}

String Calibration::fitSettingsJson() const {
  String out = "{\"min_points\":";
  out += String(static_cast<unsigned int>(calibration_curve::kMinFitPoints));
  out += ",\"min_level\":";
  out += sciFloat(fitSettings_.minLevel);
  out += ",\"readout\":[";
  for (size_t adc = 0; adc < 2; ++adc) {
    if (adc) {
      out += ",";
    }
    out += "{\"adc\":";
    out += String(static_cast<unsigned int>(adc + 1));
    out += ",\"c1\":";
    out += sciFloat(fitSettings_.readout[adc].c1);
    out += ",\"c2\":";
    out += sciFloat(fitSettings_.readout[adc].c2);
    out += ",\"clip_mv\":";
    out += sciFloat(fitSettings_.clipMv[adc]);
    out += "}";
  }
  out += "]}";
  return out;
}

String Calibration::metadataJson() const {
  float maxLevel = 0;
  for (const LevelData& level : levels_) {
    maxLevel = std::max(maxLevel, keyLevel(level.key));
  }
  String out = "{";
  out += "\"rows\":";
  out += String(static_cast<unsigned int>(analogCount_));
  out += ",\"cols\":";
  out += String(static_cast<unsigned int>(selectCount_));
  out += ",\"point_count\":";
  out += String(static_cast<unsigned int>(totalPointCount()));
  out += ",\"created_at_ms\":";
  out += String(createdAtMs_);
  out += ",\"updated_at_ms\":";
  out += String(updatedAtMs_);
  out += ",\"analog_pins\":";
  out += arrayJson(analogPins_, analogCount_);
  out += ",\"select_pins\":";
  out += arrayJson(selectPins_, selectCount_);
  out += ",\"max_level\":";
  out += floatLabel(maxLevel);
  out += ",\"tare_complete\":";
  out += savedTareComplete_ ? "true" : "false";
  out += "}";
  return out;
}

String Calibration::arrayJson(const uint8_t* values, size_t count) const {
  String out = "[";
  for (size_t i = 0; i < count; ++i) {
    if (i) {
      out += ",";
    }
    out += String(static_cast<unsigned int>(values[i]));
  }
  out += "]";
  return out;
}

size_t Calibration::capturedCount(const LevelData& level) const {
  return capturedCount(level.values);
}

size_t Calibration::capturedCount(const std::vector<float>& values) const {
  size_t total = 0;
  const size_t expected = totalPointCount();
  for (size_t i = 0; i < expected && i < values.size(); ++i) {
    if (!std::isnan(values[i])) {
      ++total;
    }
  }
  return total;
}

size_t Calibration::totalPointCount() const {
  return analogCount_ * selectCount_;
}

String Calibration::tarePath() const {
  return kCalibrationTarePath;
}

String Calibration::levelPath(int32_t key) const {
  String path = kCalibrationDirPath;
  path += "/l";
  if (key < 0) {
    path += "n";
    path += String(static_cast<long>(-key));
  } else {
    path += "p";
    path += String(static_cast<long>(key));
  }
  return path;
}

int32_t Calibration::levelKey(float level) {
  return static_cast<int32_t>(roundf(level * 1000.0f));
}

float Calibration::keyLevel(int32_t key) {
  return static_cast<float>(key) / 1000.0f;
}

String Calibration::floatLabel(float value) {
  return trimFloat(value);
}

const char* Calibration::fitResultName(FitResult result) {
  switch (result) {
    case FitResult::Fitted:
      return "fitted";
    case FitResult::TooFewPoints:
      return "too_few_points";
    case FitResult::Singular:
      return "singular";
    case FitResult::NoTare:
    default:
      return "no_tare";
  }
}

}  // namespace nhos
