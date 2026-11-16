#pragma once

#include <Arduino.h>
#include <vector>

#include "CalibrationCurve.h"
#include "Config.h"

namespace nhos {

class Storage;
class RangeCompensator;

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

  // NoRfb: a conductance profile's sensor on a row without a measured Rfb
  // (it reads 0); not a failure. Only ever reported, never stored.
  enum class FitResult : uint8_t { Fitted = 0, NoTare, TooFewPoints, Singular, NoRfb };

  void begin(Storage& storage);
  // v2.0.F: what the device outputs (mV or µS) and which rows have an Rfb.
  // Without it every profile is in mV.
  void setRangeCompensator(const RangeCompensator* range) { range_ = range; }
  // The unit the device outputs now; a profile in another unit is kept but
  // not applied (output_mode reads "raw") until the unit matches again.
  calibration_curve::Unit activeUnit() const;
  calibration_curve::Unit unit() const { return unit_; }
  // An empty profile (no tare, no levels) has no unit to disagree with.
  bool unitMatches() const { return unit_ == activeUnit() || profileEmpty(); }
  // False once the output unit changed under an open session: its draft
  // captures are in the old unit.
  bool draftUnitMatches() const { return !sessionActive_ || draftUnit_ == activeUnit(); }
  // A stand-alone tare in the active unit would sit under levels captured in
  // the other one: refused until the profile is cleared.
  bool tareUnitConflict() const { return !unitMatches() && !levels_.empty(); }
  bool profileEmpty() const { return levels_.empty() && capturedCount(tare_) == 0; }
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
    // Relative to the tare active at capture, in the profile's unit; NaN where
    // not captured.
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
  void fitAll(const std::vector<float>& tare, const std::vector<LevelData>& levels, calibration_curve::Unit unit,
              FitSet& out) const;
  // The readout curve and clip a point is classified with: the board's in mV,
  // identity and kConductanceClipUs in µS.
  calibration_curve::Readout readoutFor(size_t adc, calibration_curve::Unit unit) const;
  float clipFor(size_t adc, calibration_curve::Unit unit) const;
  // A µS sensor on a row without Rfb: reported as no_rfb, not as a failure.
  bool sensorWithoutRfb(size_t sensorIndex, calibration_curve::Unit unit) const;
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
                        const std::vector<LevelData>& levels, calibration_curve::Unit unit, bool points) const;
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
  const RangeCompensator* range_ = nullptr;
  calibration_curve::Unit unit_ = calibration_curve::Unit::Mv;
  calibration_curve::Unit draftUnit_ = calibration_curve::Unit::Mv;
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
