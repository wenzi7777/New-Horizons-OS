#include "ImuManager.h"

#include <Wire.h>

#include "BoardConfig.h"

namespace nhos {

namespace {
constexpr uint8_t kBmi270I2cAddr = 0x68;
constexpr uint8_t kBmi270PwrCtrlReg = 0x7D;
}  // namespace

void ImuManager::begin(bool enabled) {
  enabled_ = enabled;
  initialized_ = false;
  sampleValid_ = false;
  lastError_ = "";
  lastServiceAtUs_ = 0;
  lastSampleAtUs_ = 0;
  lastSampleAtMs_ = 0;
  lastReadDurationUs_ = 0;
  sampleWindowStartMs_ = 0;
  sampleWindowCount_ = 0;
  measuredSampleRateHz_ = 0.0f;
  heapBefore_ = ESP.getFreeHeap();
  if (!enabled_) {
    heapAfter_ = ESP.getFreeHeap();
    return;
  }

#if NHOS_BOARD_HAS_MAG
  if (!IMU.begin()) {
    lastError_ = "bmi270_bmm150_init_failed";
#else
  if (!IMU.begin(BOSCH_ACCELEROMETER_ONLY)) {
    lastError_ = "bmi270_init_failed";
#endif
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
    if (initialized_) {
      Wire.beginTransmission(kBmi270I2cAddr);
      Wire.write(kBmi270PwrCtrlReg);
      Wire.write(0x00);
      Wire.endTransmission();
    }
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
#if NHOS_BOARD_HAS_MAG
  float mx = sample_[7];
  float my = sample_[8];
  float mz = sample_[9];
  IMU.readMagneticField(mx, my, mz);
#endif
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
#if NHOS_BOARD_HAS_MAG
  sample_[7] = mx;
  sample_[8] = my;
  sample_[9] = mz;
#endif
  sampleValid_ = true;
  lastSampleAtUs_ = nowUs;
  lastSampleAtMs_ = millis();
  lastError_ = "";

  if (!sampleWindowStartMs_) {
    sampleWindowStartMs_ = lastSampleAtMs_;
  }
  ++sampleWindowCount_;
  const uint32_t windowElapsedMs = lastSampleAtMs_ - sampleWindowStartMs_;
  if (windowElapsedMs >= 1000) {
    measuredSampleRateHz_ = sampleWindowCount_ * 1000.0f / windowElapsedMs;
    sampleWindowCount_ = 0;
    sampleWindowStartMs_ = lastSampleAtMs_;
  }
}

bool ImuManager::copyLatestSample(float out[kImuSampleFloats]) const {
  if (!enabled_ || !initialized_ || !sampleValid_ || !out) {
    return false;
  }
  for (uint8_t i = 0; i < kImuSampleFloats; ++i) {
    out[i] = sample_[i];
  }
  return true;
}

String ImuManager::statusJson() const {
  const uint32_t nowMs = millis();
  const uint32_t cacheAgeMs = sampleValid_ ? nowMs - lastSampleAtMs_ : 0;
  const uint32_t sampleRateHz = serviceIntervalUs_ > 0
      ? (1000000UL + serviceIntervalUs_ / 2) / serviceIntervalUs_
      : 0;
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
#if NHOS_BOARD_HAS_MAG
  out += "\",\"chip\":\"BMI270+BMM150\"";
#else
  out += "\",\"chip\":\"BMI270\"";
#endif
  out += ",\"last_error\":\"";
  out += lastError_;
  out += "\",\"heap_before\":";
  out += heapBefore_;
  out += ",\"heap_after\":";
  out += heapAfter_;
  out += ",\"sample_cached\":";
  out += sampleValid_ ? "true" : "false";
  out += ",\"sample_rate_hz\":";
  out += sampleRateHz;
  out += ",\"measured_rate_hz\":";
  out += measuredSampleRateHz_;
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
