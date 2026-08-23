#include "MagnetometerManager.h"

#include <Wire.h>

#include "BoardConfig.h"
#include "Bmm350BridgePolicy.h"
#include "MagnetometerSamplePolicy.h"

#if NHOS_BOARD_MAG_MODEL == 1
#include "Arduino_BMI270_BMM150.h"
#endif

namespace nhos {
namespace {
}  // namespace

void MagnetometerManager::begin(bool bmm150HostReady) {
  initialized_ = false;
  ready_ = false;
  calibrationAvailable_ = false;
  sample_.clear();
  error_ = "not_initialized";
  lastPollMs_ = 0;

#if NHOS_BOARD_MAG_MODEL == 2
  initialized_ = bmm350_.begin();
  ready_ = initialized_ && bmm350_.normalMode();
  calibrationAvailable_ = initialized_;
  if (!ready_) {
    error_ = String("bmm350_sensorapi_init_failed_") + bmm350_.lastResult();
  }
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
    sample_.clear();
    error_ = "bmm150_read_failed";
    return;
  }
  sample_.store(next);
  error_ = "";
#elif NHOS_BOARD_MAG_MODEL == 2
  float next[3];
  const bool readSucceeded = bmm350_.readCompensatedMicroTesla(next);
  if (!bmm350CanPublishCompensatedSample(bmm350_.initialized(),
                                         bmm350_.normalMode(), readSucceeded) ||
      !magnetometerCanPublish(MagnetometerModel::Bmm350, initialized_,
                              calibrationAvailable_, readSucceeded)) {
    sample_.clear();
    error_ = String("bmm350_compensated_read_failed_") + bmm350_.lastResult();
    return;
  }
  sample_.store(next);
  error_ = "";
#endif
}

bool MagnetometerManager::copyLatestSample(float out[3]) const {
  return sample_.copy(out);
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
  out += sample_.valid() ? "true" : "false";
  out += ",\"last_error\":\"";
  out += error_;
  out += "\"}";
  return out;
}

}  // namespace nhos
