#include "RangeCompensator.h"

#include <Preferences.h>

#include "BoardConfig.h"
#include "JsonUtils.h"
#include "Storage.h"
#include "VrefManager.h"

namespace nhos {
namespace {
constexpr const char* kModeKey = "rng_mode";
constexpr const char* kRefRfbKey = "rng_ref_rfb";
constexpr const char* kLeakKey = "rng_leak";
constexpr const char* kRfbKey = "rfb_tbl";
constexpr uint32_t kAutoStepMinMs = 400;
}  // namespace

void RangeCompensator::begin(Storage& storage) {
  storage_ = &storage;
#if NHOS_BOARD_HAS_DAC_VREF
  enabled_ = true;
  const uint32_t mode = storage.getUInt(kModeKey, 0);
  mode_ = mode <= 2 ? static_cast<RangeMode>(mode) : RangeMode::Raw;
  const uint32_t ref = storage.getUInt(kRefRfbKey, kDefaultRefRfbOhm);
  refRfbOhm_ = (ref >= 1000 && ref <= 1000000) ? ref : kDefaultRefRfbOhm;
  leakComp_ = storage.getUInt(kLeakKey, 1) != 0;
  // Rfb per row, then the ADC offset per row. A table from before the offsets
  // existed has the wrong length and is ignored (measure again).
  float table[2 * kRows] = {0};
  Preferences& prefs = storage.prefs();
  if (prefs.getBytesLength(kRfbKey) == sizeof(table) &&
      prefs.getBytes(kRfbKey, table, sizeof(table)) == sizeof(table)) {
    for (size_t i = 0; i < kRows; ++i) {
      const bool good = table[i] > 0.0f && table[i] < 10000000.0f;
      rfb_[i] = good ? table[i] : 0.0f;
      offset_[i] = (good && table[kRows + i] > -100.0f && table[kRows + i] < 100.0f) ? table[kRows + i] : 0.0f;
    }
  }
#endif
}

void RangeCompensator::saveSettings() {
  if (storage_ == nullptr) {
    return;
  }
  storage_->putUInt(kModeKey, static_cast<uint32_t>(mode_));
  storage_->putUInt(kRefRfbKey, refRfbOhm_);
  storage_->putUInt(kLeakKey, leakComp_ ? 1 : 0);
}

bool RangeCompensator::setMode(RangeMode mode) {
  if (!enabled_) {
    return false;
  }
  mode_ = mode;
  lastSatMs_ = millis();
  saveSettings();
  return true;
}

bool RangeCompensator::setRefRfbOhm(uint32_t ohm) {
  if (!enabled_ || ohm < 1000 || ohm > 1000000) {
    return false;
  }
  refRfbOhm_ = ohm;
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

void RangeCompensator::setRfbTable(const float* ohm, const float* offsetMv, size_t rows) {
  float table[2 * kRows] = {0};
  for (size_t i = 0; i < kRows; ++i) {
    rfb_[i] = (i < rows && ohm[i] > 0.0f) ? ohm[i] : 0.0f;
    offset_[i] = (rfb_[i] > 0.0f && i < rows) ? offsetMv[i] : 0.0f;
    table[i] = rfb_[i];
    table[kRows + i] = offset_[i];
  }
  if (storage_ != nullptr) {
    storage_->prefs().putBytes(kRfbKey, table, sizeof(table));
  }
}

void RangeCompensator::clearRfb() {
  for (size_t i = 0; i < kRows; ++i) {
    rfb_[i] = 0.0f;
    offset_[i] = 0.0f;
  }
  if (storage_ != nullptr) {
    storage_->prefs().remove(kRfbKey);
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

float RangeCompensator::process(size_t row, size_t col, uint8_t colPin, float rawMv) {
  (void)col;
  if (!enabled_ || bypass_ || row >= kRows) {
    return rawMv;
  }
  if (rawMv >= kSaturationMv) {
    frameSatMask_ |= (1UL << row);
  }
  const float rfb = rfb_[row];
  if (colPin == kLeakColumnPin) {
    // This row, with column 13 itself driven: what its idle self leaks later.
    leakV13_[row] = rawMv;
    leakValid_[row] = true;
  } else if (leakComp_ && rfb > 0.0f && leakValid_[row]) {
    rawMv -= leakMv(leakV13_[row], vrefMv_, rfb, kLeakResistorOhm, offset_[row]);
  }
  if (mode_ != RangeMode::Raw && rfb > 0.0f) {
    rawMv = virtualizeMv(rawMv, vrefMv_, rfb, static_cast<float>(kVrefSafeMv),
                         static_cast<float>(refRfbOhm_), offset_[row]);
  }
  return rawMv;
}

void RangeCompensator::endFrame(uint32_t nowMs) {
  lastSatMask_ = frameSatMask_;
  haveFrame_ = true;
  lastFrameMs_ = nowMs;
  if (frameSatMask_ != 0) {
    ++satFrames_;
    lastSatMs_ = nowMs;
  }
}

void RangeCompensator::service(uint32_t nowMs, VrefManager& vref) {
  if (!enabled_ || bypass_ || !vref.available()) {
    return;
  }
  // Follow whoever else moved Vref (set_vref, a lowered ceiling).
  vrefMv_ = static_cast<float>(vref.vrefMv());
  if (mode_ != RangeMode::Auto || rfbKnownRows() == 0) {
    return;
  }
  if (nowMs - lastAutoStepMs_ < kAutoStepMinMs) {
    return;
  }
  // No frame for a while (scan stopped or stalled): the saturation mask is
  // stale, so it must not keep lowering Vref.
  if (!autoRangeFrameFresh(haveFrame_, nowMs - lastFrameMs_)) {
    return;
  }
  size_t saturatedRows = 0;
  for (size_t i = 0; i < kRows; ++i) {
    if (lastSatMask_ & (1UL << i)) {
      ++saturatedRows;
    }
  }
  const float next = autoRangeNextVrefMv(vrefMv_, saturatedRows, nowMs - lastSatMs_,
                                         static_cast<float>(kVrefSafeMv),
                                         static_cast<float>(kVrefMinMv));
  if (next > vrefMv_ + 0.5f || next < vrefMv_ - 0.5f) {
    const char* failure = "";
    if (vref.setVrefMv(static_cast<uint32_t>(next + 0.5f), failure)) {
      vrefMv_ = static_cast<float>(vref.vrefMv());
      lastAutoStepMs_ = nowMs;
      ++autoSteps_;
    }
  }
}

String RangeCompensator::statusJson() const {
  String out = "{";
  bool first = true;
  jsonBoolField(out, "supported", enabled_, first);
  if (!enabled_) {
    out += "}";
    return out;
  }
  const char* modeName = mode_ == RangeMode::Raw ? "raw" : (mode_ == RangeMode::Virtual ? "virtual" : "auto");
  jsonStringField(out, "mode", modeName, first);
  jsonUnsignedField(out, "ref_rfb_ohm", refRfbOhm_, first);
  jsonUnsignedField(out, "ref_vref_mv", kVrefSafeMv, first);
  jsonBoolField(out, "leak_compensation", leakComp_, first);
  jsonUnsignedField(out, "rfb_known_rows", rfbKnownRows(), first);
  String table = "[";
  for (size_t i = 0; i < kRows; ++i) {
    if (i) {
      table += ",";
    }
    table += String(static_cast<unsigned long>(rfb_[i] + 0.5f));
  }
  table += "]";
  jsonRawField(out, "rfb_ohm", table, first);
  String offsets = "[";
  for (size_t i = 0; i < kRows; ++i) {
    if (i) {
      offsets += ",";
    }
    offsets += String(offset_[i], 1);
  }
  offsets += "]";
  jsonRawField(out, "offset_mv", offsets, first);
  jsonUnsignedField(out, "saturated_rows", __builtin_popcount(lastSatMask_), first);
  jsonUnsignedField(out, "saturated_rows_mask", lastSatMask_, first);
  jsonUnsignedField(out, "saturated_frames", satFrames_, first);
  jsonUnsignedField(out, "auto_steps", autoSteps_, first);
  out += "}";
  return out;
}

}  // namespace nhos
