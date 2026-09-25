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
constexpr char kLegacyCalibrationMetaPath[] = "/calibration/profile.meta";
constexpr char kLegacyCalibrationDirPath[] = "/calibration";
constexpr char kLegacyCalibrationTarePath[] = "/calibration/tare.csv";

String trimFloat(float value) {
  String out(value, 3);
  while (out.endsWith("0")) {
    out.remove(out.length() - 1);
  }
  if (out.endsWith(".")) {
    out.remove(out.length() - 1);
  }
  return out.isEmpty() ? String("0") : out;
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

}  // namespace

void Calibration::begin(Storage& storage) {
  storage_ = &storage;
  loadFromStorage();
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
  // Status keys intentionally include "draft_levels", "draft_tare", and
  // "metadata" for the WebUI maintenance workbench.
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
  out += ",\"levels_complete\":";
  out += savedLevelsComplete_ ? "true" : "false";
  out += ",\"legacy_missing_tare\":";
  out += legacyMissingTare_ ? "true" : "false";
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

  const std::vector<float> nextTare = draftTare_;
  const std::vector<LevelData> nextLevels = draftLevels_;
  const bool nextTareComplete = tareComplete(nextTare);
  const bool nextLevelsComplete = levelsComplete(nextLevels);
  const bool nextComplete = nextTareComplete && nextLevelsComplete;

  if (autoEnable && !nextComplete) {
    outError = "calibration_incomplete";
    return false;
  }

  tare_ = nextTare;
  levels_ = nextLevels;
  convertAbsoluteLevels(levels_, tare_);
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
    outError = legacyMissingTare_ ? "calibration_missing_tare" : "calibration_incomplete";
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
    updatedAtMs_ = millis();
    if (!saveToStorage()) {
      return false;
    }
    if (!complete()) {
      enabled_ = false;
      saveToStorage();
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
    buffer += floatLabel(item->level);
    buffer += ",\"captured_points\":";
    buffer += String(static_cast<unsigned int>(captured));
    buffer += ",\"total_points\":";
    buffer += String(static_cast<unsigned int>(total));
    buffer += ",\"complete\":";
    buffer += captured >= total && total > 0 ? "true" : "false";
    buffer += ",\"relative\":";
    buffer += item->relative ? "true" : "false";
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
  out += floatLabel(level);
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

bool Calibration::captureCell(uint16_t sensorIndex, float level, float value) {
  if (!sessionActive_ || sensorIndex >= totalPointCount() || !draftTareCaptured(sensorIndex)) {
    return false;
  }
  LevelData* item = mutableLevel(draftLevels_, level, true);
  if (!item) {
    return false;
  }
  if (item->values.size() < totalPointCount()) {
    item->values.resize(totalPointCount(), NAN);
  }
  if (!item->relative) {
    calibration_curve::toRelative(item->values, draftTare_);
    item->relative = true;
  }
  item->values[sensorIndex] = value - draftTare_[sensorIndex];
  return true;
}

bool Calibration::captureAll(float level, const float* values, size_t count) {
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
  item->values.assign(values, values + total);
  calibration_curve::toRelative(item->values, draftTare_);
  item->relative = true;
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
  // A pre-v1.5.1 profile that never had a tare becomes usable with this one,
  // exactly as capturing a tare for it used to do.
  convertAbsoluteLevels(levels_, tare_);
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
    case calibration_curve::OutputMode::Calibrated:
      if (sensorIndex >= runtimeCurves_.size()) {
        return false;
      }
      return calibration_curve::evaluateCurve(runtimeCurves_[sensorIndex], calibration_curve::applyTare(rawMv, tareValue), outValue);
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
      item.level = level;
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
  item.level = level;
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

bool Calibration::levelsComplete(const std::vector<LevelData>& levels) const {
  const size_t total = totalPointCount();
  if (total == 0 || levels.empty()) {
    return false;
  }
  for (const LevelData& level : levels) {
    if (capturedCount(level) < total) {
      return false;
    }
  }
  return true;
}

void Calibration::refreshSavedStateCache() {
  savedTareComplete_ = tareComplete(tare_);
  savedLevelsComplete_ = levelsComplete(levels_);
  legacyMissingTare_ = !levels_.empty() && !savedTareComplete_;
  rebuildRuntimeCurves();
  runtimeReady_ = !legacyMissingTare_ && savedTareComplete_ && savedLevelsComplete_ && runtimeCurves_.size() == totalPointCount();
}

bool Calibration::convertAbsoluteLevels(std::vector<LevelData>& levels, const std::vector<float>& tare) {
  if (tare.empty()) {
    return false;
  }
  bool changed = false;
  for (LevelData& item : levels) {
    if (!item.relative) {
      calibration_curve::toRelative(item.values, tare);
      item.relative = true;
      changed = true;
    }
  }
  return changed;
}

void Calibration::rebuildRuntimeCurves() {
  runtimeCurves_.clear();
  const size_t total = totalPointCount();
  if (total == 0) {
    return;
  }

  runtimeCurves_.resize(total);
  std::vector<float> levelValues;
  std::vector<float> relativeRaws;
  levelValues.reserve(levels_.size());
  relativeRaws.reserve(levels_.size());
  for (size_t sensorIndex = 0; sensorIndex < total; ++sensorIndex) {
    if (sensorIndex >= tare_.size() || std::isnan(tare_[sensorIndex])) {
      continue;
    }
    levelValues.clear();
    relativeRaws.clear();
    for (const LevelData& item : levels_) {
      if (!item.relative || sensorIndex >= item.values.size()) {
        continue;
      }
      levelValues.push_back(item.level);
      relativeRaws.push_back(item.values[sensorIndex]);
    }
    calibration_curve::buildCurve(levelValues.data(), relativeRaws.data(), levelValues.size(), runtimeCurves_[sensorIndex]);
  }
}

bool Calibration::loadFromStorage() {
  enabled_ = false;
  tareEnabled_ = false;
  sessionActive_ = false;
  legacyMissingTare_ = false;
  savedTareComplete_ = false;
  savedLevelsComplete_ = false;
  runtimeReady_ = false;
  tare_.clear();
  draftTare_.clear();
  levels_.clear();
  draftLevels_.clear();
  runtimeCurves_.clear();
  createdAtMs_ = 0;
  updatedAtMs_ = 0;

  if (SPIFFS.exists(kCalibrationMetaPath)) {
    return loadFromStoragePath(kCalibrationMetaPath, kCalibrationDirPath, kCalibrationTarePath);
  }
  if (SPIFFS.exists(kLegacyCalibrationMetaPath)) {
    return loadFromStoragePath(kLegacyCalibrationMetaPath, kLegacyCalibrationDirPath, kLegacyCalibrationTarePath);
  }
  return true;
}

bool Calibration::loadFromStoragePath(const char* metaPath, const char* dirPath, const char* tarePath) {
  String meta;
  if (!storage_ || !storage_->readTextFile(metaPath, meta)) {
    return false;
  }
  // Absent in profiles saved before v1.5.1, whose levels are absolute mV.
  bool levelsRelative = false;
  int cursor = 0;
  while (cursor <= meta.length()) {
    int end = meta.indexOf('\n', cursor);
    if (end < 0) {
      end = meta.length();
    }
    String line = meta.substring(cursor, end);
    line.trim();
    if (line.length()) {
      const int sep = line.indexOf('=');
      if (sep > 0) {
        String key = line.substring(0, sep);
        String value = line.substring(sep + 1);
        key.trim();
        value.trim();
        if (key == "enabled") {
          enabled_ = value == "1" || value == "true";
        } else if (key == "tare_enabled") {
          tareEnabled_ = value == "1" || value == "true";
        } else if (key == "levels_relative") {
          levelsRelative = value == "1" || value == "true";
        } else if (key == "created_at_ms") {
          createdAtMs_ = static_cast<uint32_t>(value.toInt());
        } else if (key == "updated_at_ms") {
          updatedAtMs_ = static_cast<uint32_t>(value.toInt());
        } else if (key == "analog_pins" && analogCount_ == 0) {
          parsePinCsv(value, analogPins_, analogCount_, kRows);
        } else if (key == "select_pins" && selectCount_ == 0) {
          parsePinCsv(value, selectPins_, selectCount_, kCols);
        }
      }
    }
    cursor = end + 1;
  }

  const bool legacyLayout = String(dirPath) == kLegacyCalibrationDirPath;
  File root = SPIFFS.open(dirPath);
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      const String name = file.path();
      file.close();
      if (!legacyLayout && name.startsWith(String(dirPath) + "/l")) {
        String base = name.substring(name.lastIndexOf('/') + 2);
        float level = 0;
        if (base.startsWith("n")) {
          level = -static_cast<float>(base.substring(1).toInt()) / 1000.0f;
        } else {
          if (base.startsWith("p")) {
            base.remove(0, 1);
          }
          level = static_cast<float>(base.toInt()) / 1000.0f;
        }
        loadLevelFile(name, level);
      } else if (legacyLayout && name.endsWith(".lvl")) {
        String base = name.substring(name.lastIndexOf('/') + 1);
        base.remove(base.length() - 4);
        base.replace("level_", "");
        float level = 0;
        if (base.startsWith("n")) {
          level = -static_cast<float>(base.substring(1).toInt()) / 1000.0f;
        } else {
          if (base.startsWith("p")) {
            base.remove(0, 1);
          }
          level = static_cast<float>(base.toInt()) / 1000.0f;
        }
        loadLevelFile(name, level);
      } else if (name == tarePath || (legacyLayout && name.endsWith("/tare.csv"))) {
        loadTareFile(name);
      }
      file = root.openNextFile();
    }
  }

  if (legacyLayout) {
    levelsRelative = false;
  }
  for (LevelData& item : levels_) {
    item.relative = levelsRelative;
  }
  const bool migrated = convertAbsoluteLevels(levels_, tare_);

  refreshSavedStateCache();
  if (!complete()) {
    enabled_ = false;
  }
  if (migrated) {
    Serial.println(F("cal_levels_migrated_to_relative"));
    saveToStorage();
  }
  Serial.print(F("cal_loaded tare_points="));
  Serial.print(static_cast<unsigned int>(capturedCount(tare_)));
  Serial.print(F(" levels="));
  Serial.print(static_cast<unsigned int>(levels_.size()));
  Serial.print(F(" complete="));
  Serial.print(complete() ? F("true") : F("false"));
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
  meta += "enabled=";
  meta += enabled_ ? "1" : "0";
  meta += "\ncreated_at_ms=";
  meta += String(createdAtMs_);
  meta += "\nupdated_at_ms=";
  meta += String(updatedAtMs_);
  meta += "\nanalog_pins=";
  meta += pinCsv(analogPins_, analogCount_);
  meta += "\nselect_pins=";
  meta += pinCsv(selectPins_, selectCount_);
  meta += "\nlegacy_missing_tare=";
  meta += legacyMissingTare_ ? "1" : "0";
  meta += "\ntare_enabled=";
  meta += tareEnabled_ ? "1" : "0";
  // Every saved level shares one representation: new captures are relative,
  // and absolute ones are converted the moment a tare exists (load, commit,
  // applyTareDirect). Only a tare-less legacy profile stays absolute.
  bool levelsRelative = true;
  for (const LevelData& item : levels_) {
    levelsRelative = levelsRelative && item.relative;
  }
  meta += "\nlevels_relative=";
  meta += levelsRelative ? "1" : "0";
  meta += "\n";
  if (!storage_->writeTextFileAtomic(kCalibrationMetaPath, meta)) {
    return false;
  }
  removeObsoleteStoredFiles();
  removeLegacyCalibrationProfile();
  return true;
}

bool Calibration::ensureCalibrationDir() const {
  SPIFFS.mkdir(kCalibrationDirPath);
  File root = SPIFFS.open(kCalibrationDirPath);
  return root && root.isDirectory();
}

bool Calibration::removeStoredLevels(const char* dirPath, bool includeTare) const {
  File root = SPIFFS.open(dirPath);
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      const String name = file.path();
      file.close();
      const bool isLevel = name.endsWith(".lvl") || name.startsWith(String(dirPath) + "/l");
      const bool isTare = name.endsWith("/tare.csv") || name == kCalibrationTarePath || name == kLegacyCalibrationTarePath;
      if (isLevel || (includeTare && isTare)) {
        SPIFFS.remove(name);
      }
      file = root.openNextFile();
    }
  }
  return true;
}

void Calibration::removeLegacyCalibrationProfile() const {
  removeStoredLevels(kLegacyCalibrationDirPath, true);
  SPIFFS.remove(kLegacyCalibrationMetaPath);
  SPIFFS.remove(kLegacyCalibrationTarePath);
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
  String content;
  for (size_t i = 0; i < tare.size(); ++i) {
    if (i) {
      content += ",";
    }
    if (!std::isnan(tare[i])) {
      content += trimFloat(tare[i]);
    }
  }
  return storage_->writeTextFileAtomic(tarePath(), content);
}

bool Calibration::writeLevelFile(const LevelData& level) const {
  if (!storage_) {
    return false;
  }
  String content;
  for (size_t i = 0; i < level.values.size(); ++i) {
    if (i) {
      content += ",";
    }
    if (!std::isnan(level.values[i])) {
      content += trimFloat(level.values[i]);
    }
  }
  return storage_->writeTextFileAtomic(levelPath(level.key), content);
}

bool Calibration::loadTareFile(const String& path) {
  String content;
  if (!storage_ || !storage_->readTextFile(path, content)) {
    return false;
  }
  tare_.assign(totalPointCount(), NAN);
  size_t index = 0;
  int cursor = 0;
  while (cursor <= content.length() && index < tare_.size()) {
    int sep = content.indexOf(',', cursor);
    if (sep < 0) {
      sep = content.length();
    }
    String token = content.substring(cursor, sep);
    token.trim();
    if (token.length()) {
      tare_[index] = token.toFloat();
    }
    ++index;
    cursor = sep + 1;
  }
  return true;
}

bool Calibration::loadLevelFile(const String& path, float level) {
  String content;
  if (!storage_ || !storage_->readTextFile(path, content)) {
    return false;
  }
  LevelData item;
  item.key = levelKey(level);
  item.level = level;
  item.values.assign(totalPointCount(), NAN);
  size_t index = 0;
  int cursor = 0;
  while (cursor <= content.length() && index < item.values.size()) {
    int sep = content.indexOf(',', cursor);
    if (sep < 0) {
      sep = content.length();
    }
    String token = content.substring(cursor, sep);
    token.trim();
    if (token.length()) {
      item.values[index] = token.toFloat();
    }
    ++index;
    cursor = sep + 1;
  }
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
    out += floatLabel(item.level);
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

String Calibration::metadataJson() const {
  float maxLevel = 0;
  for (const LevelData& level : levels_) {
    if (level.level > maxLevel) {
      maxLevel = level.level;
    }
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
  out += ",\"levels_complete\":";
  out += savedLevelsComplete_ ? "true" : "false";
  out += ",\"legacy_missing_tare\":";
  out += legacyMissingTare_ ? "true" : "false";
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

String Calibration::floatLabel(float value) {
  return trimFloat(value);
}

}  // namespace nhos
