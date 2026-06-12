#include "PowerManager.h"

#include <Wire.h>

#include "BoardConfig.h"

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
constexpr PowerManager::ChargeProfileConfig kTinyProfile = {
    ChargeProfile::Tiny,
    "tiny",
    100, 500, 0x25, 0x05,
};

constexpr PowerManager::ChargeProfileConfig kSmallProfile = {
    ChargeProfile::Small,
    "small",
    200, 500, 0x2F, 0x05,
};

constexpr PowerManager::ChargeProfileConfig kCompatibleProfile = {
    ChargeProfile::Compatible,
    "compatible",
    250, 500, 0x34, 0x05,
};

constexpr PowerManager::ChargeProfileConfig kFastProfile = {
    ChargeProfile::Fast,
    "fast",
    300, 500, 0x39, 0x05,
};

constexpr PowerManager::ChargeProfileConfig kMaxProfile = {
    ChargeProfile::Max,
    "max",
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
  applyProfileByName(profileName);
  service(millis());
}

void PowerManager::service(uint32_t nowMs) {
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

uint8_t PowerManager::lastStat0() const {
  return stat0_;
}

bool PowerManager::applyProfile(ChargeProfile profile) {
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
  if (profileName == "tiny") {
    return applyProfile(ChargeProfile::Tiny);
  }
  if (profileName == "small") {
    return applyProfile(ChargeProfile::Small);
  }
  if (profileName == "fast" || profileName == "standard") {
    return applyProfile(ChargeProfile::Fast);
  }
  if (profileName == "max") {
    return applyProfile(ChargeProfile::Max);
  }
  if (profileName == "compatible" || profileName.isEmpty()) {
    return applyProfile(ChargeProfile::Compatible);
  }
  return failProfile("invalid_charge_profile");
}

String PowerManager::profileName() const {
  return String(profileConfig().name);
}

String PowerManager::statusJson() const {
  String out = "{";
  out += "\"state\":\"";
  out += chargeStateName();
  out += "\",\"detail\":\"";
  out += chargeDetailName();
  out += "\",\"charge_state\":\"";
  out += chargeStateName();
  out += "\",\"charger\":\"bq25180\",\"detected\":";
  out += detected_ ? "true" : "false";
  out += ",\"charger_detected\":";
  out += chargerDetected() ? "true" : "false";
  out += ",\"soft_off_recommended\":";
  out += softOffRecommended() ? "true" : "false";
  out += ",\"configured\":";
  out += configured_ ? "true" : "false";
  out += ",\"profile\":\"";
  out += profileName();
  out += "\",\"charge_current_ma\":";
  out += String(chargeCurrentMa_);
  out += ",\"input_limit_ma\":";
  out += String(inputLimitMa_);
  out += ",\"vbat_reg_mv\":";
  out += String(vbatRegMv_);
  out += ",\"termination_percent\":";
  out += String(terminationPercent_);
  out += ",\"precharge_percent\":";
  out += String(prechargePercent_);
  out += ",\"safety_timer_hours\":";
  out += String(safetyTimerHours_);
  out += ",\"stat0\":";
  out += String(stat0_);
  out += ",\"last_stat0\":";
  out += String(stat0_);
  out += ",\"last_error\":\"";
  out += lastError_;
  out += "\",\"config_error\":\"";
  out += lastConfigError_;
  out += "\"}";
  return out;
}

const PowerManager::ChargeProfileConfig& PowerManager::profileConfig() const {
  return configForProfile(profile_);
}

const PowerManager::ChargeProfileConfig& PowerManager::configForProfile(ChargeProfile profile) const {
  switch (profile) {
    case ChargeProfile::Tiny:
      return kTinyProfile;
    case ChargeProfile::Small:
      return kSmallProfile;
    case ChargeProfile::Fast:
      return kFastProfile;
    case ChargeProfile::Max:
      return kMaxProfile;
    case ChargeProfile::Compatible:
    default:
      return kCompatibleProfile;
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
