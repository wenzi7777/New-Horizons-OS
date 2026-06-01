#include "ImuManager.h"

namespace nhos {

void ImuManager::begin(bool enabled) {
  enabled_ = enabled;
  initialized_ = false;
  sampleValid_ = false;
  lastError_ = "";
  heapBefore_ = ESP.getFreeHeap();
  if (!enabled_) {
    heapAfter_ = ESP.getFreeHeap();
    return;
  }

  if (!IMU.begin(BOSCH_ACCELEROMETER_ONLY)) {
    lastError_ = "bmi270_init_failed";
    heapAfter_ = ESP.getFreeHeap();
    return;
  }
  initialized_ = true;
  heapAfter_ = ESP.getFreeHeap();
}

void ImuManager::setEnabled(bool enabled) {
  if (enabled == enabled_) {
    if (enabled && !initialized_) {
      begin(true);
    }
    return;
  }
  if (!enabled) {
    enabled_ = false;
    initialized_ = false;
    sampleValid_ = false;
    lastError_ = "";
    heapAfter_ = ESP.getFreeHeap();
    return;
  }
  begin(true);
}

bool ImuManager::readSample(float out7[7]) {
  if (!enabled_ || !initialized_ || !out7) {
    return false;
  }

  float gx = sample_[3];
  float gy = sample_[4];
  float gz = sample_[5];
  float ax = sample_[0];
  float ay = sample_[1];
  float az = sample_[2];

  bool updated = false;
  if (IMU.gyroscopeAvailable() && IMU.readGyroscope(gx, gy, gz)) {
    updated = true;
  }
  if (IMU.accelerationAvailable() && IMU.readAcceleration(ax, ay, az)) {
    updated = true;
  }
  if (!updated && !sampleValid_) {
    return false;
  }

  sample_[0] = ax;
  sample_[1] = ay;
  sample_[2] = az;
  sample_[3] = gx;
  sample_[4] = gy;
  sample_[5] = gz;
  sample_[6] = 0.0f;
  sampleValid_ = true;
  for (uint8_t i = 0; i < 7; ++i) {
    out7[i] = sample_[i];
  }
  return true;
}

String ImuManager::statusJson() const {
  String out = "{\"enabled\":";
  out += enabled_ ? "true" : "false";
  out += ",\"runtime_enabled\":";
  out += initialized_ ? "true" : "false";
  out += ",\"state\":\"";
  if (!enabled_) {
    out += "disabled";
  } else if (initialized_) {
    out += "ready";
  } else {
    out += "error";
  }
  out += "\",\"chip\":\"BMI270\"";
  out += ",\"last_error\":\"";
  out += lastError_;
  out += "\",\"heap_before\":";
  out += heapBefore_;
  out += ",\"heap_after\":";
  out += heapAfter_;
  out += ",\"sample_cached\":";
  out += sampleValid_ ? "true" : "false";
  out += "}";
  return out;
}

}  // namespace nhos
