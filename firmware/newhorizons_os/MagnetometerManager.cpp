#include "MagnetometerManager.h"

#include <Wire.h>

#include "BoardConfig.h"
#include "MagnetometerSamplePolicy.h"

#if NHOS_BOARD_MAG_MODEL == 1
#include "Arduino_BMI270_BMM150.h"
#endif

namespace nhos {
namespace {
constexpr uint8_t kBmm350Address = 0x14;
constexpr uint8_t kBmm350ChipId = 0x00;
constexpr uint8_t kBmm350PmuCmd = 0x06;
constexpr uint8_t kBmm350NormalMode = 0x01;
}  // namespace

void MagnetometerManager::begin(bool bmm150HostReady) {
  initialized_ = false;
  ready_ = false;
  calibrationAvailable_ = false;
  valid_ = false;
  error_ = "not_initialized";
  lastPollMs_ = 0;

#if NHOS_BOARD_MAG_MODEL == 2
  Wire.beginTransmission(kBmm350Address);
  Wire.write(kBmm350ChipId);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(kBmm350Address, static_cast<uint8_t>(1)) != 1 ||
      !Wire.available() || Wire.read() != 0x33) {
    error_ = "bmm350_not_detected";
    return;
  }
  Wire.beginTransmission(kBmm350Address);
  Wire.write(kBmm350PmuCmd);
  Wire.write(kBmm350NormalMode);
  if (Wire.endTransmission() != 0) {
    error_ = "bmm350_normal_mode_failed";
    return;
  }
  // BMM350 output needs Bosch trim compensation.  Do not advertise raw ADC
  // counts as BMM150-compatible magnetic-field values while it is absent.
  initialized_ = true;
  error_ = "bmm350_calibration_unavailable";
#elif NHOS_BOARD_MAG_MODEL == 1
  initialized_ = bmm150HostReady;
  ready_ = bmm150HostReady;
  calibrationAvailable_ = bmm150HostReady;
  error_ = bmm150HostReady ? "" : "bmm150_host_not_ready";
#else
  error_ = "magnetometer_not_supported";
#endif
}

void MagnetometerManager::service(uint32_t nowMs) {
  if (!ready_ || (lastPollMs_ && nowMs - lastPollMs_ < 50)) {
    return;
  }
  lastPollMs_ = nowMs;

#if NHOS_BOARD_MAG_MODEL == 1
  float next[3];
  const bool readSucceeded = IMU.readMagneticField(next[0], next[1], next[2]);
  if (!magnetometerCanPublish(MagnetometerModel::Bmm150, initialized_,
                              calibrationAvailable_, readSucceeded)) {
    valid_ = false;
    error_ = "bmm150_read_failed";
    return;
  }
  sample_[0] = next[0];
  sample_[1] = next[1];
  sample_[2] = next[2];
  valid_ = true;
  error_ = "";
#endif
}

bool MagnetometerManager::copyLatestSample(float out[3]) const {
  if (!out || !valid_) {
    return false;
  }
  out[0] = sample_[0];
  out[1] = sample_[1];
  out[2] = sample_[2];
  return true;
}

String MagnetometerManager::statusJson() const {
  String out = "{\"model\":\"";
#if NHOS_BOARD_MAG_MODEL == 2
  out += "BMM350";
#elif NHOS_BOARD_MAG_MODEL == 1
  out += "BMM150";
#else
  out += "none";
#endif
  out += "\",\"initialized\":";
  out += initialized_ ? "true" : "false";
  out += ",\"ready\":";
  out += ready_ ? "true" : "false";
  out += ",\"calibration\":\"";
  out += calibrationAvailable_ ? "available" : "unavailable";
  out += "\",\"sample_valid\":";
  out += valid_ ? "true" : "false";
  out += ",\"last_error\":\"";
  out += error_;
  out += "\"}";
  return out;
}

}  // namespace nhos
