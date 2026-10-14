#pragma once

#include <cstdint>
#include <string>

namespace nhos {

struct PowerStatusJsonSnapshot {
  const char* state;
  const char* detail;
  const char* charger;
  bool supported;
  bool detected;
  bool chargerDetected;
  bool configured;
  const char* profile;
  uint16_t chargeCurrentMa;
  uint16_t inputLimitMa;
  uint16_t vbatRegMv;
  uint8_t terminationPercent;
  uint8_t prechargePercent;
  uint8_t safetyTimerHours;
  uint8_t stat0;
  const char* lastError;
  const char* configError;
};

std::string formatPowerStatusJson(const PowerStatusJsonSnapshot& snapshot);

}  // namespace nhos
