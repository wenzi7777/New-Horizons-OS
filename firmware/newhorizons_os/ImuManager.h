#pragma once

#include <Arduino.h>

#include "Arduino_BMI270_BMM150.h"

namespace nhos {

class ImuManager {
 public:
  void begin(bool enabled);
  void setEnabled(bool enabled);
  void service(uint32_t nowUs);
  bool copyLatestSample(float out7[7]) const;
  String statusJson() const;

 private:
  static constexpr uint32_t kDefaultServiceIntervalUs = 10000;

  bool enabled_ = true;
  bool initialized_ = false;
  String lastError_;
  uint32_t heapBefore_ = 0;
  uint32_t heapAfter_ = 0;
  uint32_t serviceIntervalUs_ = kDefaultServiceIntervalUs;
  uint32_t lastServiceAtUs_ = 0;
  uint32_t lastSampleAtUs_ = 0;
  uint32_t lastSampleAtMs_ = 0;
  uint32_t lastReadDurationUs_ = 0;
  uint16_t sampleRateHz_ = 100;
  float sample_[7] = {0};
  bool sampleValid_ = false;
};

}  // namespace nhos
