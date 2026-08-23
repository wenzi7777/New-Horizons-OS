#pragma once

#include "BoardConfig.h"
#if NHOS_BOARD_MAG_MODEL == 2
#include <Arduino.h>
extern "C" {
#include "third_party/bmm350/bmm350.h"
}

namespace nhos {

// Wire transport adapter for Bosch Sensortec's official BMM350 SensorAPI.
// The SensorAPI obtains OTP trim coefficients during bmm350_init(); samples
// returned here are compensated microtesla, never raw ADC counts.
class Bmm350SensorApiAdapter {
 public:
  bool begin();
  bool readCompensatedMicroTesla(float out[3]);
  bool initialized() const { return initialized_; }
  bool normalMode() const { return normalMode_; }
  int8_t lastResult() const { return lastResult_; }

 private:
  static BMM350_INTF_RET_TYPE readCallback(uint8_t reg, uint8_t* data,
                                           uint32_t length, void* context);
  static BMM350_INTF_RET_TYPE writeCallback(uint8_t reg, const uint8_t* data,
                                            uint32_t length, void* context);
  static void delayCallback(uint32_t periodUs, void* context);
  BMM350_INTF_RET_TYPE read(uint8_t reg, uint8_t* data, uint32_t length);
  BMM350_INTF_RET_TYPE write(uint8_t reg, const uint8_t* data, uint32_t length);

  bmm350_dev device_{};
  bool initialized_ = false;
  bool normalMode_ = false;
  int8_t lastResult_ = BMM350_E_COM_FAIL;
};

}  // namespace nhos
#endif  // NHOS_BOARD_MAG_MODEL == 2
