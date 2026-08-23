#include "BatteryGaugeManager.h"
#include <Wire.h>
#include "BoardConfig.h"
#include "BoardPins.h"
#include "BatteryChargeSafety.h"
#include "PowerManager.h"

namespace nhos {
namespace { constexpr uint8_t kMax17048Address = 0x36; constexpr uint32_t kPollMs = 1000; }
void BatteryGaugeManager::begin() { sample_ = BatteryGaugeSample(); detected_ = false; diagnostic_ = "not_polled"; lastPollMs_ = 0; hasAppliedChargeLimit_ = false; lastBatteryId_ = BatteryIdClass::Unknown; updateProfile(resolveBatteryProfile(false, false, lastBatteryId_, manualProfile_)); }
void BatteryGaugeManager::setPowerManager(PowerManager& power) { power_ = &power; hasAppliedChargeLimit_ = false; }
void BatteryGaugeManager::setManualProfile(const ManualBatteryProfile& profile) {
  manualProfile_ = profile;
  manualProfile_.configured = manualBatteryProfileIsUsable(profile);
  updateProfile(resolveBatteryProfile(detected_, sample_.valid, lastBatteryId_, manualProfile_));
}
void BatteryGaugeManager::service(uint32_t nowMs) {
  if (lastPollMs_ && nowMs - lastPollMs_ < kPollMs) return;
  lastPollMs_ = nowMs;
#if !NHOS_BOARD_HAS_MAX17048
  sample_ = BatteryGaugeSample(); detected_ = false; lastBatteryId_ = BatteryIdClass::Unknown; diagnostic_ = "max17048_not_supported"; updateProfile(resolveBatteryProfile(false, false, lastBatteryId_, manualProfile_)); return;
#else
  uint16_t vcell = 0, soc = 0, rate = 0;
  if (!readWord(0x02, vcell) || !readWord(0x04, soc) || !readWord(0x16, rate)) { sample_ = BatteryGaugeSample(); detected_ = false; lastBatteryId_ = BatteryIdClass::Unknown; diagnostic_ = "max17048_read_failed"; updateProfile(resolveBatteryProfile(false, false, lastBatteryId_, manualProfile_)); return; }
  sample_.vbatMv = max17048RawVcellToMv(vcell); sample_.socCentiPercent = max17048RawSocToCentiPercent(soc); sample_.rate = max17048RawRateToCentiPercentPerHour(rate);
  sample_.valid = isValidBatteryVoltageMv(sample_.vbatMv); sample_.status = sample_.valid ? 1 : 0; sample_.fault = sample_.valid ? 0 : 1; detected_ = true;
  diagnostic_ = sample_.valid ? "ok" : "max17048_invalid_vcell";
  lastBatteryId_ = readBatteryId();
  updateProfile(resolveBatteryProfile(true, sample_.valid, lastBatteryId_, manualProfile_));
#endif
}
bool BatteryGaugeManager::copyLatestSample(BatteryGaugeSample& out) const { if (!sample_.valid) return false; out = sample_; return true; }
bool BatteryGaugeManager::readWord(uint8_t reg, uint16_t& value) { Wire.beginTransmission(kMax17048Address); Wire.write(reg); if (Wire.endTransmission(false) != 0) return false; if (Wire.requestFrom(kMax17048Address, static_cast<uint8_t>(2)) != 2 || Wire.available() != 2) return false; value = static_cast<uint16_t>(Wire.read()) << 8; value |= static_cast<uint16_t>(Wire.read()); return true; }
BatteryIdClass BatteryGaugeManager::readBatteryId() const { const int raw = analogRead(kBatteryIdAdcPin); if (raw < 0) return BatteryIdClass::Unknown; if (raw < 700) return BatteryIdClass::Ohm1k; if (raw < 1900) return BatteryIdClass::Ohm10k; if (raw < 3300) return BatteryIdClass::Ohm100k; return BatteryIdClass::JstOrUnknown; }
void BatteryGaugeManager::updateProfile(const BatteryProfile& profile) { profile_ = profile; if (!power_ || !shouldApplyBatteryChargeLimit(hasAppliedChargeLimit_, appliedChargeLimitMa_, profile_.maxChargeCurrentMa)) return; uint16_t actualMa = 0; if (power_->applyBatteryChargeLimit(profile_.maxChargeCurrentMa, actualMa)) { appliedChargeLimitMa_ = actualMa; hasAppliedChargeLimit_ = true; return; } hasAppliedChargeLimit_ = false; diagnostic_ += "_charge_limit_apply_failed"; }
String BatteryGaugeManager::statusJson() const { String out = "{\"gauge\":\"max17048\",\"detected\":"; out += detected_ ? "true" : "false"; out += ",\"sample_valid\":"; out += sample_.valid ? "true" : "false"; out += ",\"battery_present\":"; out += sample_.valid ? "true" : "false"; out += ",\"vbat_mv\":" + String(sample_.vbatMv) + ",\"soc_centi_percent\":" + String(sample_.socCentiPercent) + ",\"rate\":" + String(sample_.rate); out += ",\"battery_profile\":\"" + String(batteryProfileIdName(profile_.id)) + "\",\"profile_source\":\"" + String(batteryProfileSourceName(profile_.source)) + "\",\"profile_resolved\":"; out += profile_.resolved ? "true" : "false"; out += ",\"profile_required\":"; out += profile_.required ? "true" : "false"; out += ",\"capacity_mah\":" + String(profile_.capacityMah) + ",\"max_charge_current_ma\":" + String(profile_.maxChargeCurrentMa) + ",\"charge_limit_applied\":"; out += hasAppliedChargeLimit_ ? "true" : "false"; out += ",\"applied_charge_limit_ma\":" + String(appliedChargeLimitMa_) + ",\"diagnostic\":\"" + diagnostic_ + "\"}"; return out; }
}  // namespace nhos
