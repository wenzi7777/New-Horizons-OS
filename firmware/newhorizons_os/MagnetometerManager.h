#pragma once
#include <Arduino.h>
namespace nhos {
class MagnetometerManager { public: void begin(); void service(uint32_t nowMs); bool copyLatestSample(float out[3]) const; String statusJson() const; private: bool readBmm350(float out[3]); bool ready_ = false; bool valid_ = false; float sample_[3] = {0}; String error_; uint32_t lastPollMs_ = 0; };
}  // namespace nhos
