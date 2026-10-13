#pragma once

#include <Arduino.h>
#include <vector>

#include "CalibrationCurve.h"
#include "Config.h"

namespace nhos {

class Storage;

// Pressure calibration: per-sensor least-squares fits of the paper's model
// (see CalibrationCurve.h) over captured pressure levels.
//
// Points are collected either for every sensor at once (a pressure chamber)
// or one sensor at a time (a weight on one pad), so each sensor may end up
// with its own set of levels. A sensor is calibrated once it has enough
// usable levels to fit; the profile is ready when every sensor is.
class Calibration {
 public:
  // How captured readings become fit inputs. Persisted with the profile but
  // kept by clearProfile(): it describes the board, not one calibration.
  struct FitSettings {
    // Readout curve per ADC unit (index 0 = ADC1, GPIO1-10; 1 = ADC2,
    // GPIO11-20). The default identity fits in tare-relative mV.
    calibration_curve::Readout readout[2];
    // Levels whose nominal value is below this are left out of the fit; the
    // paper drops 2-2.5 kPa as under the sensors' turn-on threshold.
    float minLevel = 3.0f;
    // Readings at or above these are clipped by the ADC and left out.
    float clipMv[2] = {3150.0f, 3100.0f};
  };

  enum class FitResult : uint8_t { Fitted = 0, NoTare, TooFewPoints, Singular };

  void begin(Storage& storage);
  void setLayout(const uint8_t* analogPins, size_t analogCount, const uint8_t* selectPins, size_t selectCount);

  String statusJson(bool modeActive) const;
  // Starts a draft from the saved profile. Idempotent: calling it again
  // while a session is open keeps the draft, so a retried command cannot
  // discard captures.
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
  // Every sensor's fitted coefficients and fit result, saved profile.
  String dumpFitJson() const;
  bool captureTare(const float* values, size_t count);
  // `level` is the nominal pressure that names the level -- capturing the
  // same one again replaces it -- and `reference` the pressure actually
  // applied, which is what the fit uses.
  bool captureCell(uint16_t sensorIndex, float level, float reference, float value);
  bool captureAll(float level, float reference, const float* values, size_t count);
  // Replaces a level's reference pressure. A pressure chamber measures the
  // applied pressure over the same window the capture ran in, so it only
  // knows the reference once the capture has finished.
  bool setReference(float level, float reference);
  // True once the draft holds a baseline for `sensorIndex` (or, with -1, for
  // every sensor). Level captures are stored relative to it, so they need it.
  bool draftTareCaptured(int sensorIndex = -1) const;
  // Zeroes the device outside a session: the saved tare is replaced and
  // applied even without a pressure calibration (output_mode "tared").
  bool applyTareDirect(const float* values, size_t count);
  // Stops applying a stand-alone tare; the tare itself stays for any profile.
  bool clearTare();
  bool tareEnabled() const;
  const FitSettings& fitSettings() const { return fitSettings_; }
  bool setFitSettings(const FitSettings& settings);
  calibration_curve::OutputMode outputMode() const;
  bool apply(float rawMv, uint16_t sensorIndex, float& outValue) const;

 private:
  struct LevelData {
    // levelKey() of the nominal level.
    int32_t key = 0;
    // The pressure actually applied when the level was captured.
    float reference = 0;
    // mV relative to the tare active at capture; NaN where not captured.
    std::vector<float> values;
  };

  struct FitSet {
    std::vector<calibration_curve::FitModel> models;
    std::vector<FitResult> results;
    // Per sensor, the top of its calibrated range: the largest fit x and the
    // highest reference pressure among its usable points. apply() holds the
    // output there instead of extrapolating (evaluateClamped).
    std::vector<float> maxX;
    std::vector<float> maxPressure;
    size_t fitted = 0;
  };

  LevelData* mutableLevel(std::vector<LevelData>& levels, float level, bool createIfMissing);
  const LevelData* findLevel(const std::vector<LevelData>& levels, float level) const;
  bool tareComplete(const std::vector<float>& tare) const;
  void fitAll(const std::vector<float>& tare, const std::vector<LevelData>& levels, FitSet& out) const;
  size_t adcIndexForSensor(size_t sensorIndex) const;
  void refreshSavedStateCache();
  void logEvaluationCost() const;
  bool loadFromStorage();
  bool saveToStorage();
  bool ensureCalibrationDir() const;
  void wipeStoredProfile() const;
  void removeObsoleteStoredFiles() const;
  bool writeTareFile(const std::vector<float>& tare) const;
  bool writeLevelFile(const LevelData& level) const;
  bool loadTareFile(const String& path);
  bool loadLevelFile(const String& path, int32_t key);
  String tareSummaryJson(const std::vector<float>& tare, const char* source) const;
  String tareLayerJson(const std::vector<float>* tare) const;
  String levelsSummaryJson(const std::vector<LevelData>& levels, const char* source) const;
  // `points` adds the per-sensor, per-level PointStatus string (draft only:
  // it is what the workbench marks levels and sensors red from).
  String fitSummaryJson(const FitSet& fits, const std::vector<float>& tare,
                        const std::vector<LevelData>& levels, bool points) const;
  String fitSettingsJson() const;
  String metadataJson() const;
  String arrayJson(const uint8_t* values, size_t count) const;
  size_t capturedCount(const LevelData& level) const;
  size_t capturedCount(const std::vector<float>& values) const;
  size_t totalPointCount() const;
  String tarePath() const;
  String levelPath(int32_t key) const;
  static int32_t levelKey(float level);
  static float keyLevel(int32_t key);
  static String floatLabel(float value);
  static const char* fitResultName(FitResult result);

  Storage* storage_ = nullptr;
  bool enabled_ = false;
  bool tareEnabled_ = false;
  bool sessionActive_ = false;
  uint8_t analogPins_[kRows] = {0};
  uint8_t selectPins_[kCols] = {0};
  size_t analogCount_ = 0;
  size_t selectCount_ = 0;
  uint32_t createdAtMs_ = 0;
  uint32_t updatedAtMs_ = 0;
  bool savedTareComplete_ = false;
  bool runtimeReady_ = false;
  FitSettings fitSettings_;
  std::vector<float> tare_;
  std::vector<float> draftTare_;
  std::vector<LevelData> levels_;
  std::vector<LevelData> draftLevels_;
  // The saved profile's fits, rebuilt whenever it changes and read on
  // every sample by apply() -- never refit per reading.
  FitSet fits_;
};

}  // namespace nhos
