#pragma once

#include <Arduino.h>

namespace nhos {

enum class ChargeState : uint8_t {
  NotCharging = 0,
  ChargingOrMissing,
  ChargeDone,
};

enum class ChargeProfile : uint8_t {
  Compatible = 0,
  Fast,
  Tiny,
  Small,
  Max,
};

class PowerManager {
 public:
  void begin(const String& profileName = "compatible");
  void service(uint32_t nowMs);
  ChargeState chargeState() const;
  bool chargerDetected() const;
  bool softOffRecommended() const;
  bool supportsChargeProfiles() const;
  uint8_t lastStat0() const;
  bool applyProfile(ChargeProfile profile);
  bool applyProfileByName(const String& profileName);
  String profileName() const;
  String statusJson() const;

  struct ChargeProfileConfig {
    ChargeProfile profile;
    const char* name;
    uint16_t chargeCurrentMa;
    uint16_t inputLimitMa;
    uint8_t ichgRegisterValue;
    uint8_t inputLimitBits;
  };

 private:
  const ChargeProfileConfig& profileConfig() const;
  const ChargeProfileConfig& configForProfile(ChargeProfile profile) const;
  bool readRegister(uint8_t reg, uint8_t& value);
  bool writeRegister(uint8_t reg, uint8_t value);
  bool updateRegister(uint8_t reg, uint8_t mask, uint8_t value);
  bool readStat0(uint8_t& stat0);
  bool failProfile(const String& message);
  const char* chargeStateName() const;
  const char* chargeDetailName() const;

  ChargeState chargeState_ = ChargeState::NotCharging;
  ChargeProfile profile_ = ChargeProfile::Compatible;
  bool detected_ = false;
  bool configured_ = false;
  uint16_t chargeCurrentMa_ = 250;
  uint16_t inputLimitMa_ = 500;
  uint16_t vbatRegMv_ = 4200;
  uint8_t terminationPercent_ = 10;
  uint8_t prechargePercent_ = 20;
  uint8_t safetyTimerHours_ = 6;
  uint8_t stat0_ = 0;
  uint32_t lastReadMs_ = 0;
  String lastError_;
  String lastConfigError_;
};

}  // namespace nhos
