#include "Calibration.h"

#include <FS.h>
#include <SPIFFS.h>

#include <algorithm>
#include <cmath>

#include "Storage.h"

namespace nhos {

namespace {

constexpr char kCalibrationMetaPath[] = "/calibration/profile.meta";
constexpr char kCalibrationDirPath[] = "/calibration";
constexpr char kCalibrationTarePath[] = "/calibration/tare.csv";

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
  out += ",\"tare\":";
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
  if (!sessionActive_ || sensorIndex >= totalPointCount()) {
    return false;
  }
  LevelData* item = mutableLevel(draftLevels_, level, true);
  if (!item) {
    return false;
  }
  if (item->values.size() < totalPointCount()) {
    item->values.resize(totalPointCount(), NAN);
  }
  item->values[sensorIndex] = value;
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
  LevelData* item = mutableLevel(draftLevels_, level, true);
  if (!item) {
    return false;
  }
  item->values.assign(values, values + total);
  return true;
}

bool Calibration::applyTareDirect(const float* values, size_t count) {
  const size_t total = totalPointCount();
  if (total == 0 || !values || count < total) {
    return false;
  }
  tare_.assign(values, values + total);
  refreshSavedStateCache();
  if (!complete()) {
    enabled_ = false;
  }
  updatedAtMs_ = millis();
  return saveToStorage();
}

bool Calibration::apply(float rawMv, uint16_t sensorIndex, float& outValue) const {
  outValue = rawMv;
  if (!enabled_ || !runtimeReady_ || sensorIndex >= totalPointCount() || sensorIndex >= tare_.size() || sensorIndex >= runtimeCurves_.size()) {
    return false;
  }
  const float tareValue = tare_[sensorIndex];
  if (std::isnan(tareValue)) {
    return false;
  }

  const float adjustedRaw = std::max(0.0f, rawMv - tareValue);
  const RuntimeCurve& curve = runtimeCurves_[sensorIndex];
  if (curve.raws.empty() || curve.levels.empty() || curve.tangents.empty()) {
    return false;
  }
  if (curve.raws.size() == 1 || adjustedRaw <= curve.raws.front()) {
    outValue = curve.levels.front();
    return true;
  }
  if (adjustedRaw >= curve.raws.back()) {
    outValue = curve.levels.back();
    return true;
  }
  const auto upper = std::lower_bound(curve.raws.begin() + 1, curve.raws.end(), adjustedRaw);
  const size_t i = static_cast<size_t>(upper - curve.raws.begin());
  const float h = curve.raws[i] - curve.raws[i - 1];
  if (h < 0.0001f) {
    outValue = std::min(curve.levels[i - 1], curve.levels[i]);
    return true;
  }
  const float t = (adjustedRaw - curve.raws[i - 1]) / h;
  const float t2 = t * t;
  const float t3 = t2 * t;
  outValue = (2.0f * t3 - 3.0f * t2 + 1.0f) * curve.levels[i - 1]
           + (t3 - 2.0f * t2 + t) * h * curve.tangents[i - 1]
           + (-2.0f * t3 + 3.0f * t2) * curve.levels[i]
           + (t3 - t2) * h * curve.tangents[i];
  if (outValue < 0.0f) {
    outValue = 0.0f;
  }
  return true;
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

void Calibration::rebuildRuntimeCurves() {
  runtimeCurves_.clear();
  const size_t total = totalPointCount();
  if (total == 0) {
    return;
  }

  runtimeCurves_.resize(total);
  for (size_t sensorIndex = 0; sensorIndex < total; ++sensorIndex) {
    RuntimeCurve& curve = runtimeCurves_[sensorIndex];
    if (sensorIndex >= tare_.size()) {
      continue;
    }

    const float tareValue = tare_[sensorIndex];
    if (std::isnan(tareValue)) {
      continue;
    }

    struct SamplePoint {
      float raw;
      float level;
    };

    std::vector<SamplePoint> points;
    points.reserve(levels_.size() + 1);
    points.push_back({0.0f, 0.0f});

    for (const LevelData& item : levels_) {
      if (sensorIndex >= item.values.size()) {
        continue;
      }
      const float raw = item.values[sensorIndex];
      if (std::isnan(raw)) {
        continue;
      }
      const float adjustedLevelRaw = raw - tareValue;
      if (adjustedLevelRaw <= 0.0001f) {
        continue;
      }
      points.push_back({adjustedLevelRaw, item.level});
    }

    std::sort(points.begin(), points.end(), [](const SamplePoint& a, const SamplePoint& b) {
      if (a.raw == b.raw) {
        return a.level < b.level;
      }
      return a.raw < b.raw;
    });

    curve.raws.reserve(points.size());
    curve.levels.reserve(points.size());
    curve.tangents.assign(points.size(), 0.0f);
    for (const SamplePoint& point : points) {
      curve.raws.push_back(point.raw);
      curve.levels.push_back(point.level);
    }

    if (points.size() <= 1) {
      continue;
    }

    std::vector<float> slopes(points.size() - 1, 0.0f);
    for (size_t i = 0; i < points.size() - 1; ++i) {
      const float dx = points[i + 1].raw - points[i].raw;
      slopes[i] = (dx < 0.0001f) ? 0.0f : (points[i + 1].level - points[i].level) / dx;
    }

    curve.tangents[0] = slopes[0];
    curve.tangents[points.size() - 1] = slopes[points.size() - 2];
    for (size_t i = 1; i < points.size() - 1; ++i) {
      if (slopes[i - 1] * slopes[i] <= 0.0f) {
        curve.tangents[i] = 0.0f;
      } else {
        curve.tangents[i] = 2.0f / (1.0f / slopes[i - 1] + 1.0f / slopes[i]);
      }
    }
  }
}

bool Calibration::loadFromStorage() {
  enabled_ = false;
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

  if (!SPIFFS.exists(kCalibrationMetaPath)) {
    return true;
  }
  String meta;
  if (!storage_ || !storage_->readTextFile(kCalibrationMetaPath, meta)) {
    return false;
  }
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

  File root = SPIFFS.open(kCalibrationDirPath);
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      const String name = file.name();
      file.close();
      if (name.endsWith(".lvl")) {
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
      } else if (name == kCalibrationTarePath || name.endsWith("/tare.csv")) {
        loadTareFile(name);
      }
      file = root.openNextFile();
    }
  }

  refreshSavedStateCache();
  if (!complete()) {
    enabled_ = false;
  }
  return true;
}

bool Calibration::saveToStorage() {
  if (!storage_) {
    return false;
  }
  if (!removeStoredLevels()) {
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
  meta += "\n";
  return storage_->writeTextFileAtomic(kCalibrationMetaPath, meta);
}

bool Calibration::removeStoredLevels() const {
  File root = SPIFFS.open(kCalibrationDirPath);
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      const String name = file.name();
      file.close();
      if (name.endsWith(".lvl") || name.endsWith("/tare.csv") || name == kCalibrationTarePath) {
        SPIFFS.remove(name);
      }
      file = root.openNextFile();
    }
  }
  SPIFFS.remove(kCalibrationMetaPath);
  SPIFFS.remove(kCalibrationTarePath);
  return true;
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
  String path = "/calibration/level_";
  if (key < 0) {
    path += "n";
    path += String(static_cast<long>(-key));
  } else {
    path += "p";
    path += String(static_cast<long>(key));
  }
  path += ".lvl";
  return path;
}

int32_t Calibration::levelKey(float level) {
  return static_cast<int32_t>(roundf(level * 1000.0f));
}

String Calibration::floatLabel(float value) {
  return trimFloat(value);
}

}  // namespace nhos
