#pragma once

#include <Arduino.h>

#include "Config.h"
#include "RangeMath.h"

namespace nhos {

class Storage;
class VrefManager;

// What the scanner does with a raw row reading before the filter and the
// calibration curve see it (v2.0.F; inert on every other board).
enum class RangeMode : uint8_t {
  Raw = 0,      // as read (the reading moves with Vref and Rfb)
  Virtual = 1,  // re-expressed as a reference Vref / Rfb would have read it
  Auto = 2,     // Virtual, with the Vref chosen to keep rows out of the rail
};

// The reference the Virtual mode speaks: the same 0.33 V the 20k boards ran at.
static constexpr uint32_t kDefaultRefRfbOhm = 20000;

class RangeCompensator {
 public:
  // Loads the saved mode, reference Rfb, leak switch and measured Rfb table.
  void begin(Storage& storage);
  bool enabled() const { return enabled_; }

  RangeMode mode() const { return mode_; }
  uint32_t refRfbOhm() const { return refRfbOhm_; }
  bool leakCompensation() const { return leakComp_; }
  bool setMode(RangeMode mode);
  bool setRefRfbOhm(uint32_t ohm);
  bool setLeakCompensation(bool on);

  // vref_sweep reads the unprocessed rows and keeps auto range from moving Vref.
  void setBypass(bool on) { bypass_ = on; }

  void setVrefMv(float mv) { vrefMv_ = mv; }
  float vrefMv() const { return vrefMv_; }

  // Measured Rfb per row (0 = unknown). Persisted.
  void setRfbTable(const float* ohm, size_t rows);
  void clearRfb();
  float rfbOhm(size_t row) const { return row < kRows ? rfb_[row] : 0.0f; }
  size_t rfbKnownRows() const;

  // The scan path. One pass over a frame is bracketed by beginFrame/endFrame.
  void beginFrame() { frameSatMask_ = 0; }
  float process(size_t row, size_t col, uint8_t colPin, float rawMv);
  void endFrame(uint32_t nowMs);

  // Auto range: call often; moves Vref through `vref` when the mode is Auto.
  void service(uint32_t nowMs, VrefManager& vref);

  String statusJson() const;

 private:
  void saveSettings();

  Storage* storage_ = nullptr;
  bool enabled_ = false;
  bool bypass_ = false;
  RangeMode mode_ = RangeMode::Raw;
  uint32_t refRfbOhm_ = kDefaultRefRfbOhm;
  bool leakComp_ = true;
  float vrefMv_ = 330.0f;
  float rfb_[kRows] = {0};
  float leakV13_[kRows] = {0};
  bool leakValid_[kRows] = {false};

  uint32_t frameSatMask_ = 0;
  uint32_t lastSatMask_ = 0;
  uint32_t satFrames_ = 0;
  uint32_t lastSatMs_ = 0;
  uint32_t lastAutoStepMs_ = 0;
  uint32_t autoSteps_ = 0;
};

}  // namespace nhos
