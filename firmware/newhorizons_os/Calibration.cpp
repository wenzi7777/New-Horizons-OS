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
}

String Calibration::statusJson(bool modeActive) const {
  // Status keys intentionally include "draft_levels" and "metadata" for the WebUI workbench.
  String out = "{";
  out += "\"enabled\":";
  out += enabled_ ? "true" : "false";
  out += ",\"mode_active\":";
  out += modeActive ? "true" : "false";
  out += ",\"session_active\":";
  out += sessionActive_ ? "true" : "false";
  out += ",\"complete\":";
  out += complete() ? "true" : "false";
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
  draftLevels_ = levels_;
  sessionActive_ = true;
  return true;
}

void Calibration::sessionAbort() {
  draftLevels_.clear();
  sessionActive_ = false;
}

bool Calibration::sessionCommit(bool autoEnable, String& outError) {
  if (!sessionActive_) {
    outError = "calibration_session_required";
    return false;
  }
  const std::vector<LevelData> nextLevels = draftLevels_;
  const bool nextComplete = !nextLevels.empty() && [this, &nextLevels]() {
    const size_t total = totalPointCount();
    if (total == 0) {
      return false;
    }
    for (const LevelData& level : nextLevels) {
      if (capturedCount(level) < total) {
        return false;
      }
    }
    return true;
  }();
  if (autoEnable && !nextComplete) {
    outError = "calibration_incomplete";
    return false;
  }
  levels_ = nextLevels;
  draftLevels_.clear();
  sessionActive_ = false;
  if (!nextComplete) {
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
  const size_t total = totalPointCount();
  if (total == 0 || levels_.empty()) {
    return false;
  }
  for (const LevelData& level : levels_) {
    if (capturedCount(level) < total) {
      return false;
    }
  }
  return true;
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
  levels_.clear();
  draftLevels_.clear();
  sessionActive_ = false;
  createdAtMs_ = 0;
  updatedAtMs_ = millis();
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

bool Calibration::apply(float rawMv, uint16_t sensorIndex, float& outValue) const {
  outValue = rawMv;
  if (!enabled_ || !complete() || sensorIndex >= totalPointCount()) {
    return false;
  }
  struct SamplePoint {
    float raw;
    float level;
  };
  std::vector<SamplePoint> points;
  points.reserve(levels_.size());
  for (const LevelData& item : levels_) {
    if (sensorIndex >= item.values.size()) {
      continue;
    }
    const float raw = item.values[sensorIndex];
    if (std::isnan(raw)) {
      continue;
    }
    points.push_back({raw, item.level});
  }
  if (points.empty()) {
    return false;
  }
  std::sort(points.begin(), points.end(), [](const SamplePoint& a, const SamplePoint& b) { return a.raw < b.raw; });
  if (points.size() == 1 || rawMv <= points.front().raw) {
    outValue = points.front().level;
    return true;
  }
  if (rawMv >= points.back().raw) {
    outValue = points.back().level;
    return true;
  }
  for (size_t i = 1; i < points.size(); ++i) {
    if (rawMv > points[i].raw) {
      continue;
    }
    const float x0 = points[i - 1].raw;
    const float x1 = points[i].raw;
    const float y0 = points[i - 1].level;
    const float y1 = points[i].level;
    if (std::fabs(x1 - x0) < 0.0001f) {
      outValue = y1;
      return true;
    }
    const float ratio = (rawMv - x0) / (x1 - x0);
    outValue = y0 + (y1 - y0) * ratio;
    return true;
  }
  return false;
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

bool Calibration::loadFromStorage() {
  enabled_ = false;
  levels_.clear();
  draftLevels_.clear();
  sessionActive_ = false;
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
        } else if (key == "analog_pins") {
          parsePinCsv(value, analogPins_, analogCount_, kRows);
        } else if (key == "select_pins") {
          parsePinCsv(value, selectPins_, selectCount_, kCols);
        }
      }
    }
    cursor = end + 1;
  }

  File root = SPIFFS.open(kCalibrationDirPath);
  if (!root || !root.isDirectory()) {
    return true;
  }
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
    }
    file = root.openNextFile();
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
      if (name.endsWith(".lvl")) {
        SPIFFS.remove(name);
      }
      file = root.openNextFile();
    }
  }
  SPIFFS.remove(kCalibrationMetaPath);
  return true;
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
  size_t total = 0;
  const size_t expected = totalPointCount();
  for (size_t i = 0; i < expected && i < level.values.size(); ++i) {
    if (!std::isnan(level.values[i])) {
      ++total;
    }
  }
  return total;
}

size_t Calibration::totalPointCount() const {
  return analogCount_ * selectCount_;
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
