#pragma once

#include <Arduino.h>
#include <vector>

#include "Config.h"

namespace nhos {

class Storage;

class Calibration {
 public:
  void begin(Storage& storage);
  void setLayout(const uint8_t* analogPins, size_t analogCount, const uint8_t* selectPins, size_t selectCount);

  String statusJson(bool modeActive) const;
  bool sessionBegin();
  void sessionAbort();
  bool sessionCommit(bool autoEnable, String& outError);
  bool sessionActive() const;
  bool enabled() const;
  bool complete() const;
  bool setEnabled(bool enabled, String& outError);
  bool clearProfile();
  bool deleteLevel(float level);
  bool dumpTareJson(String& out) const;
  bool dumpLevelJson(float level, String& out) const;
  bool captureTare(const float* values, size_t count);
  bool captureCell(uint16_t sensorIndex, float level, float value);
  bool captureAll(float level, const float* values, size_t count);
  bool applyTareDirect(const float* values, size_t count);
  bool apply(float rawMv, uint16_t sensorIndex, float& outValue) const;

 private:
  struct LevelData {
    int32_t key = 0;
    float level = 0;
    std::vector<float> values;
  };

  LevelData* mutableLevel(std::vector<LevelData>& levels, float level, bool createIfMissing);
  const LevelData* findLevel(const std::vector<LevelData>& levels, float level) const;
  bool tareComplete(const std::vector<float>& tare) const;
  bool levelsComplete(const std::vector<LevelData>& levels) const;
  bool loadFromStorage();
  bool saveToStorage();
  bool removeStoredLevels() const;
  bool writeTareFile(const std::vector<float>& tare) const;
  bool writeLevelFile(const LevelData& level) const;
  bool loadTareFile(const String& path);
  bool loadLevelFile(const String& path, float level);
  String tareSummaryJson(const std::vector<float>& tare, const char* source) const;
  String tareLayerJson(const std::vector<float>* tare) const;
  String levelsSummaryJson(const std::vector<LevelData>& levels, const char* source) const;
  String metadataJson() const;
  String arrayJson(const uint8_t* values, size_t count) const;
  size_t capturedCount(const LevelData& level) const;
  size_t capturedCount(const std::vector<float>& values) const;
  size_t totalPointCount() const;
  String tarePath() const;
  String levelPath(int32_t key) const;
  static int32_t levelKey(float level);
  static String floatLabel(float value);

  Storage* storage_ = nullptr;
  bool enabled_ = false;
  bool sessionActive_ = false;
  uint8_t analogPins_[kRows] = {0};
  uint8_t selectPins_[kCols] = {0};
  size_t analogCount_ = 0;
  size_t selectCount_ = 0;
  uint32_t createdAtMs_ = 0;
  uint32_t updatedAtMs_ = 0;
  bool legacyMissingTare_ = false;
  std::vector<float> tare_;
  std::vector<float> draftTare_;
  std::vector<LevelData> levels_;
  std::vector<LevelData> draftLevels_;
};

}  // namespace nhos
