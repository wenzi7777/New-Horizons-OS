#include "BatteryGaugeManager.h"

#include <Wire.h>

#include "BatteryChargeSafety.h"
#include "BoardConfig.h"
#include "BoardPins.h"
#include "PowerManager.h"

namespace nhos {
namespace {
constexpr uint8_t kMax17048Address = 0x36;
constexpr uint8_t kMax17048VcellRegister = 0x02;
constexpr uint8_t kMax17048SocRegister = 0x04;
constexpr uint8_t kMax17048ModeRegister = 0x06;
constexpr uint8_t kMax17048CrateRegister = 0x16;
constexpr uint16_t kMax17048QuickStart = 0x4000;
constexpr uint32_t kNormalPollMs = 1000;
constexpr uint32_t kChargingPollMs = 250;
}  // namespace

void BatteryGaugeManager::begin() {
  sample_ = BatteryGaugeSample();
  detected_ = false;
  diagnostic_ = "not_polled";
  lastPollMs_ = 0;
  hasAppliedChargeLimit_ = false;
  lastBatteryId_ = BatteryIdClass::Unknown;
  syncPolicy_.begin(millis());
  updateProfile(resolveBatteryProfile(false, false, lastBatteryId_, manualProfile_));
}

void BatteryGaugeManager::setPowerManager(PowerManager& power) {
  power_ = &power;
  hasAppliedChargeLimit_ = false;
}

void BatteryGaugeManager::setManualProfile(const ManualBatteryProfile& profile) {
  manualProfile_ = profile;
  manualProfile_.configured = manualBatteryProfileIsUsable(profile);
  updateProfile(resolveBatteryProfile(detected_, sample_.valid, lastBatteryId_, manualProfile_));
}

void BatteryGaugeManager::service(uint32_t nowMs) {
#if !NHOS_BOARD_HAS_MAX17048
  if (lastPollMs_ && nowMs - lastPollMs_ < kNormalPollMs) {
    return;
  }
  lastPollMs_ = nowMs;
  sample_ = BatteryGaugeSample();
  detected_ = false;
  lastBatteryId_ = BatteryIdClass::Unknown;
  diagnostic_ = "max17048_not_supported";
  updateProfile(resolveBatteryProfile(false, false, lastBatteryId_, manualProfile_));
  return;
#else
  const bool chargerDetected = power_ && power_->chargerDetected();
  syncPolicy_.observeCharger(nowMs, chargerDetected);

  if (syncPolicy_.state() == GaugeSyncState::Syncing) {
    if (!syncPolicy_.pollDue(nowMs)) {
      return;
    }
    BatteryGaugeSample next;
    const bool readSucceeded = readGauge(next) && next.valid;
    syncPolicy_.recordPoll(nowMs, readSucceeded);
    if (readSucceeded) {
      applySample(next);
      diagnostic_ = "ok";
      lastPollMs_ = nowMs;
      return;
    }
    sample_.valid = false;
    sample_.status = 0;
    sample_.fault = 1;
    diagnostic_ = syncPolicy_.state() == GaugeSyncState::Error
                      ? "max17048_resync_timeout"
                      : "max17048_resyncing";
    return;
  }

  const uint32_t pollMs = chargerDetected ? kChargingPollMs : kNormalPollMs;
  if (lastPollMs_ && nowMs - lastPollMs_ < pollMs) {
    return;
  }
  lastPollMs_ = nowMs;

  BatteryGaugeSample next;
  if (!readGauge(next)) {
    markReadFailed("max17048_read_failed");
    return;
  }

  if (syncPolicy_.state() == GaugeSyncState::Error) {
    // A failed resync leaves SOC untrusted. Keep sampling VCELL only so a
    // later high-confidence swap can start a fresh automatic resync; do not
    // republish percentage data until that resync succeeds.
    if (syncPolicy_.observeVoltage(nowMs, next.valid, next.vbatMv)) {
      startQuickStart(nowMs);
    } else {
      sample_.valid = false;
      sample_.status = 0;
      sample_.fault = 1;
      diagnostic_ = "max17048_resync_error";
    }
    return;
  }

  applySample(next);
  diagnostic_ = sample_.valid ? "ok" : "max17048_invalid_vcell";

  if (syncPolicy_.observeVoltage(nowMs, sample_.valid, sample_.vbatMv)) {
    startQuickStart(nowMs);
  }
#endif
}

bool BatteryGaugeManager::detectNow(uint32_t nowMs) {
  // Battery profile detection remains distinct from fuel-gauge resync.
  lastPollMs_ = 0;
  service(nowMs);
  return sample_.valid;
}

GaugeResyncResult BatteryGaugeManager::requestResync(uint32_t nowMs) {
#if !NHOS_BOARD_HAS_MAX17048
  (void)nowMs;
  return GaugeResyncResult::Unavailable;
#else
  if (syncPolicy_.requestManual(nowMs) == GaugeSyncStartResult::InProgress) {
    return GaugeResyncResult::InProgress;
  }
  return startQuickStart(nowMs) ? GaugeResyncResult::Started
                                : GaugeResyncResult::Failed;
#endif
}

bool BatteryGaugeManager::copyLatestSample(BatteryGaugeSample& out) const {
  if (!sample_.valid || syncPolicy_.state() != GaugeSyncState::Ready) {
    return false;
  }
  out = sample_;
  return true;
}

bool BatteryGaugeManager::readGauge(BatteryGaugeSample& sample) {
  uint16_t vcell = 0;
  uint16_t soc = 0;
  uint16_t rate = 0;
  if (!readWord(kMax17048VcellRegister, vcell) ||
      !readWord(kMax17048SocRegister, soc) ||
      !readWord(kMax17048CrateRegister, rate)) {
    return false;
  }
  sample.vbatMv = max17048RawVcellToMv(vcell);
  sample.socCentiPercent = max17048RawSocToCentiPercent(soc);
  sample.rate = max17048RawRateToCentiPercentPerHour(rate);
  sample.valid = isValidBatteryVoltageMv(sample.vbatMv);
  sample.status = sample.valid ? 1 : 0;
  sample.fault = sample.valid ? 0 : 1;
  return true;
}

bool BatteryGaugeManager::readWord(uint8_t reg, uint16_t& value) {
  Wire.beginTransmission(kMax17048Address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(kMax17048Address, static_cast<uint8_t>(2)) != 2 ||
      Wire.available() != 2) {
    return false;
  }
  value = static_cast<uint16_t>(Wire.read()) << 8;
  value |= static_cast<uint16_t>(Wire.read());
  return true;
}

bool BatteryGaugeManager::writeWord(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(kMax17048Address);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(value >> 8));
  Wire.write(static_cast<uint8_t>(value & 0xFF));
  return Wire.endTransmission() == 0;
}

bool BatteryGaugeManager::startQuickStart(uint32_t nowMs) {
  (void)nowMs;
  sample_.valid = false;
  sample_.status = 0;
  sample_.fault = 1;
  const bool written = writeWord(kMax17048ModeRegister, kMax17048QuickStart);
  syncPolicy_.recordQuickStartWrite(nowMs, written);
  diagnostic_ = written ? "max17048_resyncing" : "max17048_quick_start_failed";
  return written;
}

void BatteryGaugeManager::applySample(const BatteryGaugeSample& sample) {
  sample_ = sample;
  detected_ = true;
  lastBatteryId_ = readBatteryId();
  updateProfile(resolveBatteryProfile(true, sample_.valid, lastBatteryId_, manualProfile_));
}

void BatteryGaugeManager::markReadFailed(const char* diagnostic) {
  sample_.valid = false;
  sample_.status = 0;
  sample_.fault = 1;
  detected_ = false;
  lastBatteryId_ = BatteryIdClass::Unknown;
  diagnostic_ = diagnostic;
  updateProfile(resolveBatteryProfile(false, false, lastBatteryId_, manualProfile_));
}

BatteryIdClass BatteryGaugeManager::readBatteryId() const {
  return classifyBatteryIdAdcRaw(analogRead(kBatteryIdAdcPin));
}

void BatteryGaugeManager::updateProfile(const BatteryProfile& profile) {
  profile_ = profile;
  if (!power_ || !shouldApplyBatteryChargeLimit(
                     hasAppliedChargeLimit_, appliedChargeLimitMa_,
                     profile_.maxChargeCurrentMa)) {
    return;
  }
  uint16_t actualMa = 0;
  if (power_->applyBatteryChargeLimit(profile_.maxChargeCurrentMa, actualMa)) {
    appliedChargeLimitMa_ = actualMa;
    hasAppliedChargeLimit_ = true;
    return;
  }
  hasAppliedChargeLimit_ = false;
  diagnostic_ += "_charge_limit_apply_failed";
}

String BatteryGaugeManager::statusJson() const {
  String out = "{\"gauge\":\"max17048\",\"detected\":";
  out += detected_ ? "true" : "false";
  out += ",\"sample_valid\":";
  out += sample_.valid ? "true" : "false";
  out += ",\"battery_present\":";
  out += sample_.valid ? "true" : "null";
  out += ",\"vbat_mv\":";
  out += sample_.valid ? String(sample_.vbatMv) : "null";
  out += ",\"soc_centi_percent\":";
  out += sample_.valid ? String(sample_.socCentiPercent) : "null";
  out += ",\"rate\":";
  out += sample_.valid ? String(sample_.rate) : "null";
  out += ",\"gauge_sync_state\":\"";
  out += gaugeSyncStateName(syncPolicy_.state());
  out += "\",\"last_sync_reason\":\"";
  out += gaugeSyncReasonName(syncPolicy_.lastReason());
  out += "\",\"gauge_sync_count\":";
  out += String(syncPolicy_.syncCount());
  out += ",\"battery_id\":\"" + String(batteryIdClassName(lastBatteryId_));
  out += "\",\"battery_profile\":\"" + String(batteryProfileIdName(profile_.id));
  out += "\",\"profile_source\":\"" + String(batteryProfileSourceName(profile_.source));
  out += "\",\"profile_resolved\":";
  out += profile_.resolved ? "true" : "false";
  out += ",\"profile_required\":";
  out += profile_.required ? "true" : "false";
  out += ",\"capacity_mah\":" + String(profile_.capacityMah);
  out += ",\"max_charge_current_ma\":" + String(profile_.maxChargeCurrentMa);
  out += ",\"charge_limit_applied\":";
  out += hasAppliedChargeLimit_ ? "true" : "false";
  out += ",\"applied_charge_limit_ma\":" + String(appliedChargeLimitMa_);
  out += ",\"diagnostic\":\"" + diagnostic_ + "\"}";
  return out;
}

}  // namespace nhos
