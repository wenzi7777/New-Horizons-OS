#include "BatteryGaugeManager.h"

#include <Wire.h>

#include "BatteryChargeSafety.h"
#include "BoardConfig.h"
#include "PowerManager.h"

namespace nhos {
namespace {
constexpr uint8_t kMax17048Address = 0x36;
constexpr uint8_t kMax17048VcellRegister = 0x02;
constexpr uint8_t kMax17048SocRegister = 0x04;
constexpr uint8_t kMax17048ModeRegister = 0x06;
constexpr uint8_t kMax17048CrateRegister = 0x16;
constexpr uint16_t kMax17048QuickStart = 0x4000;
constexpr uint8_t kMax17048StatusRegister = 0x1A;
constexpr uint16_t kMax17048StatusResetIndicator = 0x0100;  // RI
// How long the cell rests with charging paused before quick-start.
constexpr uint32_t kChargeRelaxMs = 1500;
constexpr uint32_t kNormalPollMs = 1000;
constexpr uint32_t kChargingPollMs = 250;
}  // namespace

void BatteryGaugeManager::begin() {
  sample_ = BatteryGaugeSample();
  detected_ = false;
  diagnostic_ = "not_polled";
  lastPollMs_ = 0;
  hasAppliedChargeLimit_ = false;
  syncPolicy_.begin(millis());
  socScaler_.reset();
  quickStartPending_ = false;
  resetFlagChecked_ = false;
  // A restart in the middle of a sync must never leave charging off.
  resumeChargingIfPaused();
  updateProfile(resolveBatteryProfile(manualProfile_, batteryId_));
}

void BatteryGaugeManager::setPowerManager(PowerManager& power) {
  power_ = &power;
  hasAppliedChargeLimit_ = false;
}

void BatteryGaugeManager::setManualProfile(const ManualBatteryProfile& profile) {
  manualProfile_ = profile;
  manualProfile_.configured = manualBatteryProfileIsUsable(profile);
  // A new battery setting usually means a different cell.
  socScaler_.reset();
  updateProfile(resolveBatteryProfile(manualProfile_, batteryId_));
}

void BatteryGaugeManager::setBatteryId(const BatteryIdResult& id, bool readOk) {
  batteryId_ = id;
  batteryIdReadOk_ = readOk;
  // A different cell than the one the scaler learned "full" for.
  socScaler_.reset();
  updateProfile(resolveBatteryProfile(manualProfile_, batteryId_));
}

void BatteryGaugeManager::service(uint32_t nowMs) {
#if !NHOS_BOARD_HAS_MAX17048
  if (lastPollMs_ && nowMs - lastPollMs_ < kNormalPollMs) {
    return;
  }
  lastPollMs_ = nowMs;
  sample_ = BatteryGaugeSample();
  detected_ = false;
  diagnostic_ = "max17048_not_supported";
  return;
#else
  const bool chargerDetected = power_ && power_->chargerDetected();
  syncPolicy_.observeCharger(nowMs, chargerDetected);

  if (syncPolicy_.state() == GaugeSyncState::Syncing) {
    if (quickStartPending_) {
      if (static_cast<int32_t>(nowMs - relaxUntilMs_) < 0) {
        return;
      }
      writeQuickStart(nowMs);
      if (syncPolicy_.state() != GaugeSyncState::Syncing) {
        resumeChargingIfPaused();
      }
      return;
    }
    if (!syncPolicy_.pollDue(nowMs)) {
      return;
    }
    BatteryGaugeSample next;
    const bool readSucceeded = readGauge(next) && next.valid;
    syncPolicy_.recordPoll(nowMs, readSucceeded);
    if (syncPolicy_.state() != GaugeSyncState::Syncing) {
      resumeChargingIfPaused();
    }
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

  // Not syncing: charging must be on. Retried every pass if the write failed.
  resumeChargingIfPaused();

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
    return;
  }
  checkGaugeReset(nowMs);
#endif
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
  sample_.valid = false;
  sample_.status = 0;
  sample_.fault = 1;
  // Whatever triggered a resync, the old full-charge scale no longer holds.
  socScaler_.reset();
  quickStartPending_ = false;
  if (power_ && power_->chargerDetected()) {
    if (pausedCharging_ || power_->setChargingPaused(true)) {
      pausedCharging_ = true;
      quickStartPending_ = true;
      relaxUntilMs_ = nowMs + kChargeRelaxMs;
      diagnostic_ = "max17048_relaxing";
      return true;
    }
    // Could not pause: quick-start under charge as before (reads high).
  }
  return writeQuickStart(nowMs);
}

bool BatteryGaugeManager::writeQuickStart(uint32_t nowMs) {
  quickStartPending_ = false;
  const bool written = writeWord(kMax17048ModeRegister, kMax17048QuickStart);
  syncPolicy_.recordQuickStartWrite(nowMs, written);
  diagnostic_ = written ? "max17048_resyncing" : "max17048_quick_start_failed";
  return written;
}

void BatteryGaugeManager::resumeChargingIfPaused() {
  if (pausedCharging_ && power_ && power_->setChargingPaused(false)) {
    pausedCharging_ = false;
  }
}

void BatteryGaugeManager::checkGaugeReset(uint32_t nowMs) {
  uint16_t status = 0;
  if (!readWord(kMax17048StatusRegister, status) ||
      (status & kMax17048StatusResetIndicator) == 0) {
    resetFlagChecked_ = true;
    return;
  }
  // RI: the gauge lost power since it was last cleared -- the cell was
  // pulled (CELL and VDD both hang off VBAT). Clear it so the next pull
  // shows up again.
  if (!writeWord(kMax17048StatusRegister,
                 static_cast<uint16_t>(status & ~kMax17048StatusResetIndicator))) {
    return;
  }
  socScaler_.reset();
  const bool atBoot = !resetFlagChecked_;
  resetFlagChecked_ = true;
  // At boot RI is set after any power-up; the gauge's own power-on estimate
  // stands. While running it means a swap, and the gauge's power-on
  // estimate was likely taken under charge, so redo it relaxed.
  if (!atBoot &&
      syncPolicy_.requestGaugeReset(nowMs) == GaugeSyncStartResult::Started) {
    startQuickStart(nowMs);
  }
}

void BatteryGaugeManager::applySample(const BatteryGaugeSample& sample) {
  sample_ = sample;
  sample_.rawSocCentiPercent = sample.socCentiPercent;
  const bool chargeDone = power_ && power_->chargeState() == ChargeState::ChargeDone;
  const bool chargerPresent = power_ && power_->chargerDetected();
  sample_.socCentiPercent =
      socScaler_.apply(sample.socCentiPercent, chargeDone, chargerPresent);
  detected_ = true;
}

void BatteryGaugeManager::markReadFailed(const char* diagnostic) {
  sample_.valid = false;
  sample_.status = 0;
  sample_.fault = 1;
  detected_ = false;
  diagnostic_ = diagnostic;
}

void BatteryGaugeManager::updateProfile(const BatteryProfile& profile) {
  profile_ = profile;
  if (!power_ || !shouldApplyBatteryChargeLimit(
                     hasAppliedChargeLimit_, appliedChargeLimitMa_,
                     profile_.maxChargeCurrentMa)) {
    return;
  }
  // The battery's maximum is a ceiling over the charge profile: a slower
  // profile keeps its own lower current underneath it.
  uint16_t actualMa = 0;
  if (power_->setChargeCeilingMa(profile_.maxChargeCurrentMa, actualMa)) {
    appliedChargeLimitMa_ = profile_.maxChargeCurrentMa;
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
  out += ",\"soc_raw_centi_percent\":";
  out += sample_.valid ? String(sample_.rawSocCentiPercent) : "null";
  out += ",\"soc_full_centi_percent\":" + String(socScaler_.fullCentiPercent());
  out += ",\"soc_full_learned\":";
  out += socScaler_.learned() ? "true" : "false";
  out += ",\"charging_paused_for_sync\":";
  out += pausedCharging_ ? "true" : "false";
  out += ",\"rate\":";
  out += sample_.valid ? String(sample_.rate) : "null";
  out += ",\"gauge_sync_state\":\"";
  out += gaugeSyncStateName(syncPolicy_.state());
  out += "\",\"last_sync_reason\":\"";
  out += gaugeSyncReasonName(syncPolicy_.lastReason());
  out += "\",\"gauge_sync_count\":";
  out += String(syncPolicy_.syncCount());
  // `battery_profile` / `profile_source` are what older Desktops read: "manual"
  // unless v2.0.F recognised the battery's ID resistor ("auto").
  out += ",\"battery_profile\":\"";
  out += profile_.resolved ? (profile_.automatic ? "auto" : "manual") : "none";
  out += "\",\"profile_source\":\"";
  out += profile_.automatic ? "auto" : "manual";
  out += "\",\"profile_resolved\":";
  out += profile_.resolved ? "true" : "false";
  out += ",\"profile_required\":";
  out += profile_.required ? "true" : "false";
  out += ",\"capacity_mah\":" + String(profile_.capacityMah);
  out += ",\"max_charge_current_ma\":" + String(profile_.maxChargeCurrentMa);
#if NHOS_BOARD_HAS_BATID
  out += ",\"battery_id\":{\"read_ok\":";
  out += batteryIdReadOk_ ? "true" : "false";
  out += ",\"kind\":\"";
  out += batteryIdKindName(batteryId_.kind);
  out += "\",\"mv\":" + String(batteryId_.mv);
  out += ",\"ohm\":" + String(static_cast<unsigned long>(batteryId_.resistanceOhm));
  out += "}";
#endif
  out += ",\"charge_limit_applied\":";
  out += hasAppliedChargeLimit_ ? "true" : "false";
  out += ",\"applied_charge_limit_ma\":" + String(appliedChargeLimitMa_);
  out += ",\"diagnostic\":\"" + diagnostic_ + "\"}";
  return out;
}

}  // namespace nhos
