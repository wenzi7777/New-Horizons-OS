#pragma once

#include <Arduino.h>

#include "Arduino_BMI270_BMM150.h"

namespace nhos {

class ImuManager {
 public:
  void begin(bool enabled);
  void setEnabled(bool enabled);
  bool readSample(float out7[7]);
  String statusJson() const;

 private:
  bool enabled_ = true;
  bool initialized_ = false;
  String lastError_;
  uint32_t heapBefore_ = 0;
  uint32_t heapAfter_ = 0;
  float sample_[7] = {0};
  bool sampleValid_ = false;
};

}  // namespace nhos
