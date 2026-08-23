#include "PowerManager.h"

#include <Wire.h>

#include "BoardConfig.h"
#include "PowerStatusJson.h"

namespace nhos {
namespace {
constexpr uint8_t kBq25180Address = 0x6A;
constexpr uint8_t kBq25180Stat0Register = 0x00;
constexpr uint8_t kBq25180VbatCtrlRegister = 0x03;
constexpr uint8_t kBq25180IchgCtrlRegister = 0x04;
constexpr uint8_t kBq25180ChargeCtrl0Register = 0x05;
constexpr uint8_t kBq25180IcCtrlRegister = 0x07;
constexpr uint8_t kBq25180TmrIlimRegister = 0x08;
constexpr uint8_t kBq25180Vbat4200Mv = 0x46;
constexpr uint32_t kPowerStatusPollMs = 1000;

// ICHG register encoding derived from known data points:
// 0x34 -> 250mA, 0x39 -> 300mA (BQ25180 measured), linear interpolation 10mA/step
constexpr PowerManager::ChargeProfileConfig kUltraSlowProfile = {
    ChargeProfile::UltraSlow,
    "ultra_slow",
    100, 500, 0x25, 0x05,
};

constexpr PowerManager::ChargeProfileConfig kSlowProfile = {
    ChargeProfile::Slow,
    "slow",
    200, 500, 0x2F, 0x05,
};

constexpr PowerManager::ChargeProfileConfig kBalancedProfile = {
    ChargeProfile::Balanced,
    "balanced",
    250, 500, 0x34, 0x05,
};

constexpr PowerManager::ChargeProfileConfig kFastProfile = {
    ChargeProfile::Fast,
    "fast",
    300, 500, 0x39, 0x05,
};

constexpr PowerManager::ChargeProfileConfig kExtremeProfile = {
    ChargeProfile::Extreme,
    "extreme",
    350, 500, 0x3E, 0x05,
};

void setPowerBusClock() {
  Wire.setClock(NHOS_BOARD_BQ25180_I2C_HZ);
}

void restoreBoardBusClock() {
  Wire.setClock(NHOS_BOARD_I2C_HZ);
}
}

void PowerManager::begin(const String& profileName) {
  lastReadMs_ = 0;
  stat0_ = 0;
  chargeState_ = ChargeState::NotCharging;
  detected_ = false;
  configured_ = false;
  lastError_ = "";
  lastConfigError_ = "";
  if (!supportsChargeProfiles()) {
    profile_ = ChargeProfile::Balanced;
    chargeCurrentMa_ = 0;
    inputLimitMa_ = 0;
    vbatRegMv_ = 0;
    terminationPercent_ = 0;
    prechargePercent_ = 0;
    safetyTimerHours_ = 0;
    service(millis());
    return;
  }
  applyProfileByName(profileName);
  service(millis());
}

void PowerManager::service(uint32_t nowMs) {
  if (!supportsChargeProfiles()) {
    lastReadMs_ = nowMs;
    detected_ = false;
    chargeState_ = ChargeState::NotCharging;
    stat0_ = 0;
    lastError_ = "";
    return;
  }
  if (lastReadMs_ != 0 && nowMs - lastReadMs_ < kPowerStatusPollMs) {
    return;
  }
  lastReadMs_ = nowMs;

  uint8_t stat0 = 0;
  if (!readStat0(stat0)) {
    detected_ = false;
    chargeState_ = ChargeState::NotCharging;
    return;
  }

  detected_ = true;
  stat0_ = stat0;
  lastError_ = "";
  const uint8_t chg = (stat0 >> 5) & 0x03;
  if (chg == 0x01 || chg == 0x02) {
    chargeState_ = ChargeState::ChargingOrMissing;
  } else if (chg == 0x03) {
    chargeState_ = ChargeState::ChargeDone;
  } else {
    chargeState_ = ChargeState::NotCharging;
  }
}

ChargeState PowerManager::chargeState() const {
  return chargeState_;
}

bool PowerManager::chargerDetected() const {
  return detected_ && (stat0_ & 0x01) != 0;
}

bool PowerManager::softOffRecommended() const {
  return chargerDetected() || chargeState_ != ChargeState::NotCharging;
}

bool PowerManager::supportsChargeProfiles() const {
  return NHOS_BOARD_HAS_BQ25180 != 0;
}

uint8_t PowerManager::lastStat0() const {
  return stat0_;
}

bool PowerManager::applyProfile(ChargeProfile profile) {
  if (!supportsChargeProfiles()) {
    return failProfile("charge_profile_unsupported");
  }
  const ChargeProfileConfig& config = configForProfile(profile);
  lastConfigError_ = "";

  if (!writeRegister(kBq25180VbatCtrlRegister, kBq25180Vbat4200Mv)) {
    return failProfile("bq25180_vbat_write_failed");
  }
  if (!writeRegister(kBq25180IchgCtrlRegister, config.ichgRegisterValue)) {
    return failProfile("bq25180_ichg_write_failed");
  }
  if (!updateRegister(kBq25180ChargeCtrl0Register, 0x70, 0x20)) {
    return failProfile("bq25180_chargectrl0_write_failed");
  }
  if (!updateRegister(kBq25180IcCtrlRegister, 0x0C, 0x04)) {
    return failProfile("bq25180_ic_ctrl_write_failed");
  }
  if (!updateRegister(kBq25180TmrIlimRegister, 0x07, config.inputLimitBits)) {
    return failProfile("bq25180_tmr_ilim_write_failed");
  }

  uint8_t value = 0;
  if (!readRegister(kBq25180VbatCtrlRegister, value) || (value & 0x7F) != kBq25180Vbat4200Mv) {
    return failProfile("bq25180_vbat_verify_failed");
  }
  if (!readRegister(kBq25180IchgCtrlRegister, value) || (value & 0x7F) != config.ichgRegisterValue) {
    return failProfile("bq25180_ichg_verify_failed");
  }
  if (!readRegister(kBq25180ChargeCtrl0Register, value) || (value & 0x70) != 0x20) {
    return failProfile("bq25180_chargectrl0_verify_failed");
  }
  if (!readRegister(kBq25180IcCtrlRegister, value) || (value & 0x0C) != 0x04) {
    return failProfile("bq25180_ic_ctrl_verify_failed");
  }
  if (!readRegister(kBq25180TmrIlimRegister, value) || (value & 0x07) != config.inputLimitBits) {
    return failProfile("bq25180_tmr_ilim_verify_failed");
  }

  profile_ = profile;
  configured_ = true;
  detected_ = true;
  chargeCurrentMa_ = config.chargeCurrentMa;
  inputLimitMa_ = config.inputLimitMa;
  vbatRegMv_ = 4200;
  terminationPercent_ = 10;
  prechargePercent_ = 20;
  safetyTimerHours_ = 6;
  lastError_ = "";
  return true;
}

bool PowerManager::applyProfileByName(const String& profileName) {
  if (profileName == "ultra_slow") {
    return applyProfile(ChargeProfile::UltraSlow);
  }
  if (profileName == "slow") {
    return applyProfile(ChargeProfile::Slow);
  }
  if (profileName == "fast") {
    return applyProfile(ChargeProfile::Fast);
  }
  if (profileName == "extreme") {
    return applyProfile(ChargeProfile::Extreme);
  }
  if (profileName == "balanced" || profileName.isEmpty()) {
    return applyProfile(ChargeProfile::Balanced);
  }
  return failProfile("invalid_charge_profile");
}

bool PowerManager::applyBatteryChargeLimit(uint16_t requestedMa, uint16_t& actualMa) {
  // BQ25180's documented ICHG range is linear here: 0x25 is 100 mA and each
  // following code adds 10 mA.  Verify every register before reporting the
  // exact requested limit as active.
  if (!supportsChargeProfiles() || requestedMa < 100 || requestedMa > 350 ||
      requestedMa % 10 != 0) {
    actualMa = 0;
    return failProfile("battery_charge_limit_invalid");
  }
  const uint8_t ichg = static_cast<uint8_t>(0x25 + (requestedMa - 100) / 10);
  lastConfigError_ = "";
  if (!writeRegister(kBq25180VbatCtrlRegister, kBq25180Vbat4200Mv) ||
      !writeRegister(kBq25180IchgCtrlRegister, ichg) ||
      !updateRegister(kBq25180ChargeCtrl0Register, 0x70, 0x20) ||
      !updateRegister(kBq25180IcCtrlRegister, 0x0C, 0x04) ||
      !updateRegister(kBq25180TmrIlimRegister, 0x07, 0x05)) {
    actualMa = 0;
    return failProfile("battery_charge_limit_write_failed");
  }
  uint8_t value = 0;
  if (!readRegister(kBq25180VbatCtrlRegister, value) ||
      (value & 0x7F) != kBq25180Vbat4200Mv ||
      !readRegister(kBq25180IchgCtrlRegister, value) ||
      (value & 0x7F) != ichg ||
      !readRegister(kBq25180ChargeCtrl0Register, value) ||
      (value & 0x70) != 0x20 ||
      !readRegister(kBq25180IcCtrlRegister, value) ||
      (value & 0x0C) != 0x04 ||
      !readRegister(kBq25180TmrIlimRegister, value) ||
      (value & 0x07) != 0x05) {
    actualMa = 0;
    return failProfile("battery_charge_limit_verify_failed");
  }
  chargeCurrentMa_ = requestedMa;
  inputLimitMa_ = 500;
  vbatRegMv_ = 4200;
  terminationPercent_ = 10;
  prechargePercent_ = 20;
  safetyTimerHours_ = 6;
  configured_ = true;
  detected_ = true;
  lastError_ = "";
  actualMa = requestedMa;
  return true;
}

String PowerManager::profileName() const {
  return String(profileConfig().name);
}

String PowerManager::statusJson() const {
  const std::string json = formatPowerStatusJson({
      chargeStateName(), chargeDetailName(),
      supportsChargeProfiles() ? "bq25180" : "none", supportsChargeProfiles(),
      detected_, chargerDetected(), configured_, profileConfig().name,
      chargeCurrentMa_, inputLimitMa_, vbatRegMv_, terminationPercent_,
      prechargePercent_, safetyTimerHours_, stat0_, lastError_.c_str(),
      lastConfigError_.c_str()});
  return String(json.c_str());
}

const PowerManager::ChargeProfileConfig& PowerManager::profileConfig() const {
  return configForProfile(profile_);
}

const PowerManager::ChargeProfileConfig& PowerManager::configForProfile(ChargeProfile profile) const {
  switch (profile) {
    case ChargeProfile::UltraSlow:
      return kUltraSlowProfile;
    case ChargeProfile::Slow:
      return kSlowProfile;
    case ChargeProfile::Fast:
      return kFastProfile;
    case ChargeProfile::Extreme:
      return kExtremeProfile;
    case ChargeProfile::Balanced:
    default:
      return kBalancedProfile;
  }
}

bool PowerManager::readRegister(uint8_t reg, uint8_t& value) {
  setPowerBusClock();
  Wire.beginTransmission(kBq25180Address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    restoreBoardBusClock();
    lastError_ = "bq25180_register_select_failed";
    return false;
  }

  const uint8_t read = Wire.requestFrom(kBq25180Address, static_cast<uint8_t>(1));
  if (read != 1 || !Wire.available()) {
    restoreBoardBusClock();
    lastError_ = "bq25180_register_read_failed";
    return false;
  }
  value = Wire.read();
  restoreBoardBusClock();
  return true;
}

bool PowerManager::writeRegister(uint8_t reg, uint8_t value) {
  setPowerBusClock();
  Wire.beginTransmission(kBq25180Address);
  Wire.write(reg);
  Wire.write(value);
  if (Wire.endTransmission(true) != 0) {
    restoreBoardBusClock();
    lastError_ = "bq25180_register_write_failed";
    return false;
  }
  restoreBoardBusClock();
  return true;
}

bool PowerManager::updateRegister(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t current = 0;
  if (!readRegister(reg, current)) {
    return false;
  }
  current = static_cast<uint8_t>((current & ~mask) | (value & mask));
  return writeRegister(reg, current);
}

bool PowerManager::readStat0(uint8_t& stat0) {
  if (!readRegister(kBq25180Stat0Register, stat0)) {
    lastError_ = "bq25180_stat0_read_failed";
    return false;
  }
  return true;
}

bool PowerManager::failProfile(const String& message) {
  configured_ = false;
  lastConfigError_ = message;
  lastError_ = message;
  return false;
}

const char* PowerManager::chargeStateName() const {
  switch (chargeState_) {
    case ChargeState::ChargingOrMissing:
      return "charging";
    case ChargeState::ChargeDone:
      return "charge_done";
    case ChargeState::NotCharging:
    default:
      return "not_charging";
  }
}

const char* PowerManager::chargeDetailName() const {
  switch (chargeState_) {
    case ChargeState::ChargingOrMissing:
      return "charging_or_missing";
    case ChargeState::ChargeDone:
      return "charge_done";
    case ChargeState::NotCharging:
    default:
      return "not_charging";
  }
}

}  // namespace nhos
