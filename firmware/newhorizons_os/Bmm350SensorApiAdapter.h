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
  // Output data rate (25/50/100/200 Hz) and averaging (8/4/2 samples), see
  // SensorRatePolicy. Remembered for the next begin(); written at once when
  // the chip is up (then back to normal mode, as the SensorAPI requires).
  bool setOdr(uint16_t hz, uint8_t averaging);
  uint16_t odrHz() const { return odrHz_; }
  uint8_t averaging() const { return averaging_; }
  // Suspend (the chip's lowest-power state) or back to normal mode.
  bool setAwake(bool awake);
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
  uint16_t odrHz_ = 25;
  uint8_t averaging_ = 8;
  int8_t lastResult_ = BMM350_E_COM_FAIL;
};

}  // namespace nhos
#endif  // NHOS_BOARD_MAG_MODEL == 2
