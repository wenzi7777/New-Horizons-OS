#pragma once

#include <Arduino.h>

#include "Config.h"
#include "RangeMath.h"

namespace nhos {

class Storage;
class VrefManager;

// What the scanner does with a raw row reading before the filter (and, in mV,
// the calibration curve) see it (v2.0.F; inert on every other board).
enum class RangeMode : uint8_t {
  Raw = 0,          // ADC mV as read: the old boards' way, and the default
  Conductance = 1,  // the cell's conductance in microsiemens (needs rfb_measure)
  Auto = 2,         // conductance, with Vref chosen by auto range
};

// What rfb_measure found on a row. Users solder Rfb only on the rows they use,
// so "no_feedback" is normal; "saturated" means fitted but too large to measure.
enum class RfbRowStatus : uint8_t {
  Unknown = 0,  // never measured
  Ok = 1,
  NoFeedback = 2,
  Saturated = 3,
  Open = 4,
  AdcFailed = 5,
};
const char* rfbRowStatusName(RfbRowStatus status);

class RangeCompensator {
 public:
  // Loads the saved mode, leak switch and measured Rfb table. A mode saved by
  // firmware before v2.2 (virtual / auto in mV) is dropped: the device starts in Raw.
  void begin(Storage& storage);
  bool enabled() const { return enabled_; }

  RangeMode mode() const { return mode_; }
  bool conductance() const { return enabled_ && mode_ != RangeMode::Raw; }
  bool autoRange() const { return enabled_ && mode_ == RangeMode::Auto; }
  bool leakCompensation() const { return leakComp_; }
  bool setMode(RangeMode mode);
  bool setLeakCompensation(bool on);

  // vref_sweep / rfb_measure read the unprocessed rows and keep auto range from
  // moving Vref.
  void setBypass(bool on) { bypass_ = on; }

  void setVrefMv(float mv);
  float vrefMv() const { return vrefMv_; }

  // Measured Rfb, ADC offset and status per row (Rfb 0 = unknown). Persisted.
  void setRfbTable(const float* ohm, const float* offsetMv, const RfbRowStatus* status, size_t rows);
  void clearRfb();
  float rfbOhm(size_t row) const { return row < kRows ? rfb_[row] : 0.0f; }
  float offsetMv(size_t row) const { return row < kRows ? offset_[row] : 0.0f; }
  size_t rfbKnownRows() const;

  // The scan path. One pass over a frame is bracketed by beginFrame/endFrame.
  // process() returns what goes on the wire (mV or µS); adcMv, when given,
  // receives the ADC reading as read (before leak compensation).
  void beginFrame() { frameSatMask_ = 0; }
  float process(size_t row, size_t col, uint8_t colPin, float rawMv, float* adcMv = nullptr);
  void endFrame(uint32_t nowMs);

  // Auto range: call often; moves Vref through `vref` when the mode is Auto.
  void service(uint32_t nowMs, VrefManager& vref);

  // The range TLV for the frame just scanned (0 bytes when not enabled).
  size_t encodeFrameTlv(uint8_t* out, size_t capacity) const;

  String statusJson() const;

 private:
  void saveSettings();
  void resetWindow(uint32_t nowMs);

  Storage* storage_ = nullptr;
  bool enabled_ = false;
  bool bypass_ = false;
  RangeMode mode_ = RangeMode::Raw;
  bool leakComp_ = true;
  float vrefMv_ = 330.0f;
  float rfb_[kRows] = {0};
  float offset_[kRows] = {0};
  RfbRowStatus status_[kRows] = {};
  float leakV13_[kRows] = {0};
  bool leakValid_[kRows] = {false};

  uint32_t frameSatMask_ = 0;
  uint32_t lastSatMask_ = 0;
  uint32_t satFrames_ = 0;
  uint32_t lastSatMs_ = 0;
  bool haveFrame_ = false;
  uint32_t lastFrameMs_ = 0;
  uint32_t lastAutoStepMs_ = 0;
  uint32_t autoSteps_ = 0;
  // Highest measured-row reading since Vref last moved (auto range's raise).
  float framePeakMv_ = 0.0f;
  float windowPeakMv_ = 0.0f;
  uint32_t windowStartMs_ = 0;
  float autoCeilingMv_ = kAutoRangeMaxVrefMv;
  float frameVrefMv_ = 330.0f;
};

}  // namespace nhos
