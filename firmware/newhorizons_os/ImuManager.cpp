#include "ImuManager.h"

namespace nhos {

void ImuManager::begin(bool enabled) {
  enabled_ = enabled;
  initialized_ = false;
  sampleValid_ = false;
  lastError_ = "";
  lastServiceAtUs_ = 0;
  lastSampleAtUs_ = 0;
  lastSampleAtMs_ = 0;
  lastReadDurationUs_ = 0;
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
  IMU.setContinuousMode();
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

void ImuManager::service(uint32_t nowUs) {
  if (!enabled_ || !initialized_) {
    return;
  }
  if (lastServiceAtUs_ && static_cast<uint32_t>(nowUs - lastServiceAtUs_) < serviceIntervalUs_) {
    return;
  }
  lastServiceAtUs_ = nowUs;

  float gx = sample_[3];
  float gy = sample_[4];
  float gz = sample_[5];
  float ax = sample_[0];
  float ay = sample_[1];
  float az = sample_[2];

  const uint32_t readStartedUs = micros();
  const bool gyroUpdated = IMU.readGyroscope(gx, gy, gz);
  const bool accelUpdated = IMU.readAcceleration(ax, ay, az);
  lastReadDurationUs_ = micros() - readStartedUs;

  if (!gyroUpdated && !accelUpdated) {
    if (!sampleValid_) {
      lastError_ = "bmi270_sample_unavailable";
    }
    return;
  }

  sample_[0] = ax;
  sample_[1] = ay;
  sample_[2] = az;
  sample_[3] = gx;
  sample_[4] = gy;
  sample_[5] = gz;
  sample_[6] = 0.0f;
  sampleValid_ = true;
  lastSampleAtUs_ = nowUs;
  lastSampleAtMs_ = millis();
  lastError_ = "";
}

bool ImuManager::copyLatestSample(float out7[7]) const {
  if (!enabled_ || !initialized_ || !sampleValid_ || !out7) {
    return false;
  }
  for (uint8_t i = 0; i < 7; ++i) {
    out7[i] = sample_[i];
  }
  return true;
}

String ImuManager::statusJson() const {
  const uint32_t nowMs = millis();
  const uint32_t cacheAgeMs = sampleValid_ ? nowMs - lastSampleAtMs_ : 0;
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
  out += ",\"sample_rate_hz\":";
  out += sampleRateHz_;
  out += ",\"cache_age_ms\":";
  out += cacheAgeMs;
  out += ",\"last_read_duration_us\":";
  out += lastReadDurationUs_;
  out += ",\"last_sample_at_ms\":";
  out += lastSampleAtMs_;
  out += "}";
  return out;
}

}  // namespace nhos
