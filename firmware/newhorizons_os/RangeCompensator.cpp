#include "RangeCompensator.h"

#include <Preferences.h>

#include "BoardConfig.h"
#include "JsonUtils.h"
#include "Storage.h"
#include "VrefManager.h"
#include "VrefPolicy.h"

namespace nhos {
namespace {
// v2.2: raw / conductance / auto (conductance). The keys of the mV-only
// virtual / auto modes before it are removed at boot, never read.
constexpr const char* kModeKey = "rng_mode2";
constexpr const char* kLegacyModeKey = "rng_mode";
constexpr const char* kLegacyRefRfbKey = "rng_ref_rfb";
constexpr const char* kLeakKey = "rng_leak";
constexpr const char* kRfbKey = "rfb_tbl";
constexpr const char* kRfbStatusKey = "rfb_st";
constexpr uint32_t kAutoStepMinMs = 400;
static_assert(kRows <= 32, "the saturation mask is one bit per row");
}  // namespace

const char* rfbRowStatusName(RfbRowStatus status) {
  switch (status) {
    case RfbRowStatus::Ok: return "ok";
    case RfbRowStatus::NoFeedback: return "no_feedback";
    case RfbRowStatus::Saturated: return "saturated";
    case RfbRowStatus::Open: return "open";
    case RfbRowStatus::AdcFailed: return "adc_read_failed";
    case RfbRowStatus::Unknown:
    default: return "unknown";
  }
}

void RangeCompensator::begin(Storage& storage) {
  storage_ = &storage;
#if NHOS_BOARD_HAS_DAC_VREF
  enabled_ = true;
  Preferences& prefs = storage.prefs();
  // Before v2.2 the modes were raw / virtual / auto, all in mV. Whatever one was
  // saved, the device now starts in raw ADC mV and the user chooses again.
  if (prefs.isKey(kLegacyModeKey) || prefs.isKey(kLegacyRefRfbKey)) {
    prefs.remove(kLegacyModeKey);
    prefs.remove(kLegacyRefRfbKey);
  }
  const uint32_t mode = storage.getUInt(kModeKey, 0);
  mode_ = mode <= 2 ? static_cast<RangeMode>(mode) : RangeMode::Raw;
  leakComp_ = storage.getUInt(kLeakKey, 1) != 0;
  // Rfb per row, then the ADC offset per row. A table from before the offsets
  // existed has the wrong length and is ignored (measure again).
  float table[2 * kRows] = {0};
  if (prefs.getBytesLength(kRfbKey) == sizeof(table) &&
      prefs.getBytes(kRfbKey, table, sizeof(table)) == sizeof(table)) {
    for (size_t i = 0; i < kRows; ++i) {
      const bool good = table[i] > 0.0f && table[i] < 10000000.0f;
      rfb_[i] = good ? table[i] : 0.0f;
      offset_[i] = (good && table[kRows + i] > -100.0f && table[kRows + i] < 100.0f) ? table[kRows + i] : 0.0f;
    }
  }
  uint8_t statuses[kRows] = {0};
  const bool haveStatus = prefs.getBytesLength(kRfbStatusKey) == sizeof(statuses) &&
                          prefs.getBytes(kRfbStatusKey, statuses, sizeof(statuses)) == sizeof(statuses);
  for (size_t i = 0; i < kRows; ++i) {
    // A table measured before statuses were kept: what Rfb says is all we know.
    RfbRowStatus status = rfb_[i] > 0.0f ? RfbRowStatus::Ok : RfbRowStatus::Unknown;
    if (haveStatus && statuses[i] <= static_cast<uint8_t>(RfbRowStatus::AdcFailed)) {
      status = static_cast<RfbRowStatus>(statuses[i]);
    }
    // Never "ok" without an Rfb, never anything else with one.
    if ((status == RfbRowStatus::Ok) != (rfb_[i] > 0.0f)) {
      status = rfb_[i] > 0.0f ? RfbRowStatus::Ok : RfbRowStatus::Unknown;
    }
    status_[i] = status;
  }
  // Conductance needs a measured Rfb; a table lost since leaves the device in raw.
  if (mode_ != RangeMode::Raw && rfbKnownRows() == 0) {
    mode_ = RangeMode::Raw;
    saveSettings();
  }
  resetWindow(millis());
#endif
}

void RangeCompensator::saveSettings() {
  if (storage_ == nullptr) {
    return;
  }
  storage_->putUInt(kModeKey, static_cast<uint32_t>(mode_));
  storage_->putUInt(kLeakKey, leakComp_ ? 1 : 0);
}

void RangeCompensator::resetWindow(uint32_t nowMs) {
  windowPeakMv_ = 0.0f;
  windowStartMs_ = nowMs;
}

bool RangeCompensator::setMode(RangeMode mode) {
  if (!enabled_) {
    return false;
  }
  mode_ = mode;
  lastSatMs_ = millis();
  resetWindow(lastSatMs_);
  saveSettings();
  return true;
}

bool RangeCompensator::setLeakCompensation(bool on) {
  if (!enabled_) {
    return false;
  }
  leakComp_ = on;
  saveSettings();
  return true;
}

void RangeCompensator::setVrefMv(float mv) {
  if (mv > vrefMv_ + 0.5f || mv < vrefMv_ - 0.5f) {
    resetWindow(millis());  // the window's peak was read at the old Vref
  }
  vrefMv_ = mv;
}

void RangeCompensator::setRfbTable(const float* ohm, const float* offsetMv, const RfbRowStatus* status,
                                   size_t rows) {
  float table[2 * kRows] = {0};
  uint8_t statuses[kRows] = {0};
  for (size_t i = 0; i < kRows; ++i) {
    rfb_[i] = (i < rows && ohm[i] > 0.0f) ? ohm[i] : 0.0f;
    offset_[i] = (rfb_[i] > 0.0f && i < rows) ? offsetMv[i] : 0.0f;
    RfbRowStatus s = (i < rows && status != nullptr) ? status[i] : RfbRowStatus::Unknown;
    if ((s == RfbRowStatus::Ok) != (rfb_[i] > 0.0f)) {
      s = rfb_[i] > 0.0f ? RfbRowStatus::Ok : (i < rows ? RfbRowStatus::NoFeedback : RfbRowStatus::Unknown);
    }
    status_[i] = s;
    table[i] = rfb_[i];
    table[kRows + i] = offset_[i];
    statuses[i] = static_cast<uint8_t>(s);
  }
  if (storage_ != nullptr) {
    storage_->prefs().putBytes(kRfbKey, table, sizeof(table));
    storage_->prefs().putBytes(kRfbStatusKey, statuses, sizeof(statuses));
  }
  resetWindow(millis());
}

void RangeCompensator::clearRfb() {
  for (size_t i = 0; i < kRows; ++i) {
    rfb_[i] = 0.0f;
    offset_[i] = 0.0f;
    status_[i] = RfbRowStatus::Unknown;
  }
  if (storage_ != nullptr) {
    storage_->prefs().remove(kRfbKey);
    storage_->prefs().remove(kRfbStatusKey);
  }
}

size_t RangeCompensator::rfbKnownRows() const {
  size_t n = 0;
  for (size_t i = 0; i < kRows; ++i) {
    if (rfb_[i] > 0.0f) {
      ++n;
    }
  }
  return n;
}

float RangeCompensator::process(size_t row, size_t col, uint8_t colPin, float rawMv, float* adcMv) {
  (void)col;
  if (adcMv != nullptr) {
    *adcMv = rawMv;
  }
  if (!enabled_ || bypass_ || row >= kRows) {
    return rawMv;
  }
  const float rfb = rfb_[row];
  // Only a row with a measured Rfb says anything about the range: one without
  // has no feedback, sits at the rail, and must not drive auto range down.
  if (rfb > 0.0f) {
    if (rawMv >= kSaturationMv) {
      frameSatMask_ |= (1UL << row);
    }
    if (rawMv > framePeakMv_) {
      framePeakMv_ = rawMv;
    }
  }
  if (colPin == kLeakColumnPin) {
    // This row, with column 13 itself driven: what its idle self leaks later.
    leakV13_[row] = rawMv;
    leakValid_[row] = true;
  } else if (leakComp_ && rfb > 0.0f && leakValid_[row]) {
    rawMv -= leakMv(leakV13_[row], vrefMv_, rfb, kLeakResistorOhm, offset_[row]);
  }
  if (mode_ != RangeMode::Raw) {
    // A row without Rfb has no conductance to report: 0.
    return conductanceUs(rawMv, vrefMv_, rfb, offset_[row]);
  }
  return rawMv;
}

void RangeCompensator::endFrame(uint32_t nowMs) {
  lastSatMask_ = frameSatMask_;
  haveFrame_ = true;
  lastFrameMs_ = nowMs;
  frameVrefMv_ = vrefMv_;
  if (frameSatMask_ != 0) {
    ++satFrames_;
    lastSatMs_ = nowMs;
  }
  if (framePeakMv_ > windowPeakMv_) {
    windowPeakMv_ = framePeakMv_;
  }
  framePeakMv_ = 0.0f;
}

void RangeCompensator::service(uint32_t nowMs, VrefManager& vref) {
  if (!enabled_ || bypass_ || !vref.available()) {
    return;
  }
  // Follow whoever else moved Vref (set_vref, a lowered ceiling).
  setVrefMv(static_cast<float>(vref.vrefMv()));
  autoCeilingMv_ = autoRangeCeilingMv(static_cast<float>(vref.limitMv()), static_cast<float>(kVrefMinMv));
  if (mode_ != RangeMode::Auto || rfbKnownRows() == 0) {
    return;
  }
  if (nowMs - lastAutoStepMs_ < kAutoStepMinMs) {
    return;
  }
  // No frame for a while (scan stopped or stalled): the saturation mask and the
  // peak are stale, so they must not move Vref either way.
  if (!autoRangeFrameFresh(haveFrame_, nowMs - lastFrameMs_)) {
    return;
  }
  AutoRangeInput in;
  in.currentVrefMv = vrefMv_;
  in.saturatedRows = static_cast<size_t>(__builtin_popcount(lastSatMask_));
  in.quietMs = nowMs - lastSatMs_;
  in.windowPeakMv = windowPeakMv_;
  in.windowMs = nowMs - windowStartMs_;
  in.floorMv = static_cast<float>(kVrefMinMv);
  in.ceilingMv = autoCeilingMv_;
  const float next = autoRangeNextVrefMv(in);
  if (next > vrefMv_ + 0.5f || next < vrefMv_ - 0.5f) {
    const char* failure = "";
    if (vref.setVrefMv(static_cast<uint32_t>(next + 0.5f), failure)) {
      setVrefMv(static_cast<float>(vref.vrefMv()));
      resetWindow(nowMs);
      lastAutoStepMs_ = nowMs;
      ++autoSteps_;
    }
  }
}

size_t RangeCompensator::encodeFrameTlv(uint8_t* out, size_t capacity) const {
  if (!enabled_) {
    return 0;
  }
  const float mv = frameVrefMv_ < 0.0f ? 0.0f : (frameVrefMv_ > 65535.0f ? 65535.0f : frameVrefMv_);
  return encodeRangeTlv(conductance(), autoRange(), static_cast<uint16_t>(mv + 0.5f), lastSatMask_, out,
                        capacity);
}

String RangeCompensator::statusJson() const {
  String out = "{";
  bool first = true;
  jsonBoolField(out, "supported", enabled_, first);
  if (!enabled_) {
    out += "}";
    return out;
  }
  const char* modeName =
      mode_ == RangeMode::Raw ? "raw" : (mode_ == RangeMode::Conductance ? "conductance" : "auto");
  jsonStringField(out, "mode", modeName, first);
  jsonStringField(out, "output_unit", conductance() ? "uS" : "mV", first);
  jsonBoolField(out, "auto", autoRange(), first);
  // The mV calibration curves and tare are not applied to conductance.
  jsonBoolField(out, "leak_compensation", leakComp_, first);
  jsonUnsignedField(out, "vref_mv", static_cast<uint32_t>(vrefMv_ + 0.5f), first);
  jsonUnsignedField(out, "auto_ceiling_mv", static_cast<uint32_t>(autoCeilingMv_ + 0.5f), first);
  jsonUnsignedField(out, "rfb_known_rows", rfbKnownRows(), first);
  String table = "[";
  String offsets = "[";
  String statuses = "[";
  for (size_t i = 0; i < kRows; ++i) {
    if (i) {
      table += ",";
      offsets += ",";
      statuses += ",";
    }
    table += String(static_cast<unsigned long>(rfb_[i] + 0.5f));
    offsets += String(offset_[i], 1);
    statuses += "\"";
    statuses += rfbRowStatusName(status_[i]);
    statuses += "\"";
  }
  table += "]";
  offsets += "]";
  statuses += "]";
  jsonRawField(out, "rfb_ohm", table, first);
  jsonRawField(out, "offset_mv", offsets, first);
  jsonRawField(out, "rfb_status", statuses, first);
  jsonUnsignedField(out, "saturated_rows", __builtin_popcount(lastSatMask_), first);
  jsonUnsignedField(out, "saturated_rows_mask", lastSatMask_, first);
  jsonUnsignedField(out, "saturated_frames", satFrames_, first);
  jsonUnsignedField(out, "window_peak_mv", static_cast<uint32_t>(windowPeakMv_ + 0.5f), first);
  jsonUnsignedField(out, "auto_steps", autoSteps_, first);
  out += "}";
  return out;
}

}  // namespace nhos
