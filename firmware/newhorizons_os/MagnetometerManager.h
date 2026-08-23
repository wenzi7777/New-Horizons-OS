#pragma once

#include <Arduino.h>

#include "BoardConfig.h"
#include "MagnetometerSampleCache.h"
#if NHOS_BOARD_MAG_MODEL == 2
#include "Bmm350SensorApiAdapter.h"
#endif

namespace nhos {

class MagnetometerManager {
 public:
  // BMM150 is hosted by the Arduino combined IMU driver, so its readiness
  // must follow the actual IMU initialization result rather than a board flag.
  void begin(bool bmm150HostReady = false);
  void service(uint32_t nowMs);
  bool copyLatestSample(float out[3]) const;
  String statusJson() const;

 private:
  bool initialized_ = false;
  bool ready_ = false;
  bool calibrationAvailable_ = false;
  MagnetometerSampleCache sample_;
  String error_;
  uint32_t lastPollMs_ = 0;
#if NHOS_BOARD_MAG_MODEL == 2
  Bmm350SensorApiAdapter bmm350_;
#endif
};

}  // namespace nhos
