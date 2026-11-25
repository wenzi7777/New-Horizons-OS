#include "MagnetometerManager.h"

#include <Wire.h>
#include <math.h>

#include "BoardConfig.h"
#include "Bmm350BridgePolicy.h"
#include "MagnetometerSamplePolicy.h"
#include "SensorRatePolicy.h"

#if NHOS_BOARD_MAG_MODEL == 1
#include "Arduino_BMI270_BMM150.h"
#include "ImuDriver.h"
#endif

namespace nhos {

namespace {
void appendFloatArray(String& out, const float* values, uint8_t count, uint8_t digits) {
  out += "[";
  for (uint8_t i = 0; i < count; ++i) {
    if (i) out += ",";
    out += String(values[i], static_cast<unsigned int>(digits));
  }
  out += "]";
}

// uint64_t has no String constructor on every core; print it by hand.
void appendU64(String& out, uint64_t value) {
  char buf[21];
  uint8_t pos = sizeof(buf) - 1;
  buf[pos] = '\0';
  do {
    buf[--pos] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value && pos);
  out += &buf[pos];
}
}  // namespace

void MagnetometerManager::begin(bool enabled, bool bmm150HostReady) {
  enabled_ = enabled;
  initialized_ = false;
  ready_ = false;
  compensationAvailable_ = false;
  blockedByImu_ = false;
  sample_.clear();
  rawSample_.clear();
  error_ = "";
  lastPollUs_ = 0;
  rateWindowStartMs_ = 0;
  rateWindowCount_ = 0;
  measuredRateHz_ = 0.0f;
  drift_.reset();
#if NHOS_BOARD_MAG_MODEL == 0
  error_ = "magnetometer_not_supported";
  return;
#else
  if (!enabled_) {
    stopChip();
    return;
  }
  beginChip(bmm150HostReady);
#endif
}

void MagnetometerManager::beginChip(bool bmm150HostReady) {
#if NHOS_BOARD_MAG_MODEL == 2
  (void)bmm150HostReady;
  initialized_ = bmm350_.begin();
  ready_ = initialized_ && bmm350_.normalMode();
  compensationAvailable_ = initialized_;
  if (!ready_) {
    error_ = String("bmm350_sensorapi_init_failed_") + bmm350_.lastResult();
  }
#elif NHOS_BOARD_MAG_MODEL == 1
  if (!bmm150HostReady) {
    blockedByImu_ = true;
    error_ = "bmm150_host_not_ready";
    return;
  }
  initialized_ = imuDriver.setMagAwake(true);
  ready_ = initialized_;
  compensationAvailable_ = initialized_;
  error_ = initialized_ ? "" : "bmm150_wake_failed";
#else
  (void)bmm150HostReady;
#endif
}

void MagnetometerManager::stopChip() {
  // Off means off: the chip goes to its lowest-power state, not just unread.
#if NHOS_BOARD_MAG_MODEL == 2
  if (bmm350_.initialized()) bmm350_.setAwake(false);
#elif NHOS_BOARD_MAG_MODEL == 1
  // Before the IMU has begun this only records it; configure_sensor() then
  // leaves the BMM150 asleep.
  imuDriver.setMagAwake(false);
#endif
  initialized_ = false;
  ready_ = false;
  sample_.clear();
  rawSample_.clear();
  measuredRateHz_ = 0.0f;
  rateWindowStartMs_ = 0;
  rateWindowCount_ = 0;
}

void MagnetometerManager::applyState(bool enabled, bool bmm150HostReady) {
#if NHOS_BOARD_MAG_MODEL == 0
  (void)enabled;
  (void)bmm150HostReady;
  return;
#else
  if (!enabled) {
    enabled_ = false;
    blockedByImu_ = false;
    error_ = "";
    stopChip();
    drift_.reset();
    return;
  }
  const bool wasRunning = enabled_ && ready_;
  enabled_ = true;
#if NHOS_BOARD_MAG_MODEL == 1
  if (!bmm150HostReady) {
    // The IMU (which hosts the BMM150) is off: nothing to read until it is
    // back. Remember the wish; set_imu calls this again.
    initialized_ = false;
    ready_ = false;
    blockedByImu_ = true;
    error_ = "bmm150_host_not_ready";
    sample_.clear();
    rawSample_.clear();
    return;
  }
  blockedByImu_ = false;
#endif
  if (wasRunning) return;
#if NHOS_BOARD_MAG_MODEL == 2
  if (bmm350_.initialized()) {
    // Suspended earlier: wake it rather than re-running the whole init.
    ready_ = bmm350_.setAwake(true);
    initialized_ = ready_;
    compensationAvailable_ = initialized_;
    error_ = ready_ ? "" : String("bmm350_wake_failed_") + bmm350_.lastResult();
  } else {
    beginChip(bmm150HostReady);
  }
#else
  beginChip(bmm150HostReady);
#endif
  lastPollUs_ = 0;
  drift_.reset();
#endif
}

void MagnetometerManager::setRateForScan(uint16_t fps, uint32_t scanIntervalUs) {
  pollIntervalUs_ = scanIntervalUs ? scanIntervalUs : 50000;
#if NHOS_BOARD_MAG_MODEL == 2
  const uint16_t hz = SensorRatePolicy::bmm350OdrHzForFps(fps);
  const uint8_t avg = SensorRatePolicy::bmm350AveragingForOdr(hz);
  odrHz_ = hz;
  averaging_ = avg;
  if (hz != bmm350_.odrHz() || avg != bmm350_.averaging()) {
    if (!bmm350_.setOdr(hz, avg)) {
      error_ = String("bmm350_odr_write_failed_") + bmm350_.lastResult();
    }
  }
#elif NHOS_BOARD_MAG_MODEL == 1
  const uint16_t hz = SensorRatePolicy::bmm150OdrHzForFps(fps);
  odrHz_ = hz;
  averaging_ = 0;
  if (hz != imuDriver.magOdrHz() && !imuDriver.setMagOdrHz(hz)) {
    error_ = "bmm150_odr_write_failed";
  }
#else
  (void)fps;
#endif
}

void MagnetometerManager::setCalibration(const MagCalibrationData& cal) {
  calibration_ = cal;
  drift_.reset();
}

void MagnetometerManager::service(uint32_t nowUs) {
  if (!enabled_ || !ready_) {
    return;
  }
  if (lastPollUs_ && static_cast<uint32_t>(nowUs - lastPollUs_) < pollIntervalUs_) {
    return;
  }
  lastPollUs_ = nowUs;

  float raw[3];
#if NHOS_BOARD_MAG_MODEL == 1
  const bool readSucceeded = imuDriver.readMagneticField(raw[0], raw[1], raw[2]);
  if (!magnetometerCanPublish(MagnetometerModel::Bmm150, initialized_,
                              compensationAvailable_, readSucceeded)) {
    sample_.clear();
    rawSample_.clear();
    error_ = "bmm150_read_failed";
    return;
  }
#elif NHOS_BOARD_MAG_MODEL == 2
  const bool readSucceeded = bmm350_.readCompensatedMicroTesla(raw);
  if (!bmm350CanPublishCompensatedSample(bmm350_.initialized(),
                                         bmm350_.normalMode(), readSucceeded) ||
      !magnetometerCanPublish(MagnetometerModel::Bmm350, initialized_,
                              compensationAvailable_, readSucceeded)) {
    sample_.clear();
    rawSample_.clear();
    error_ = String("bmm350_compensated_read_failed_") + bmm350_.lastResult();
    return;
  }
#else
  return;
#endif
#if NHOS_BOARD_MAG_MODEL != 0
  float calibrated[3];
  magcal::apply(calibration_, raw, calibrated);
  rawSample_.store(raw);
  sample_.store(calibrated);
  error_ = "";

  const uint32_t nowMs = millis();
  if (calibration_.mode != MagCalibrationMode::None) {
    const float magnitude = sqrtf(calibrated[0] * calibrated[0] +
                                  calibrated[1] * calibrated[1] +
                                  calibrated[2] * calibrated[2]);
    drift_.feed(nowMs, magnitude, calibration_.fieldUt);
  }
  ++rateWindowCount_;
  if (!rateWindowStartMs_) {
    rateWindowStartMs_ = nowMs;
  }
  const uint32_t windowElapsedMs = nowMs - rateWindowStartMs_;
  if (windowElapsedMs >= 1000) {
    measuredRateHz_ = rateWindowCount_ * 1000.0f / windowElapsedMs;
    rateWindowCount_ = 0;
    rateWindowStartMs_ = nowMs;
  }
#endif
}

bool MagnetometerManager::copyLatestSample(float out[3]) const {
  return enabled_ && sample_.copy(out);
}

bool MagnetometerManager::copyLatestRawSample(float out[3]) const {
  return enabled_ && rawSample_.copy(out);
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
  out += "\",\"enabled\":";
  out += enabled_ ? "true" : "false";
  out += ",\"state\":\"";
  if (!enabled_) {
    out += "disabled";
  } else if (blockedByImu_) {
    out += "blocked";
  } else if (ready_) {
    out += "ready";
  } else {
    out += "error";
  }
  out += "\"";
  if (blockedByImu_) {
    out += ",\"blocked_by\":\"imu\"";
  }
  out += ",\"initialized\":";
  out += initialized_ ? "true" : "false";
  out += ",\"ready\":";
  out += ready_ ? "true" : "false";
  // The chip's own factory trim (BMM350 OTP / BMM150 trim registers) -- not
  // the hard/soft-iron calibration below.
  out += ",\"compensation\":\"";
  out += compensationAvailable_ ? "factory" : "unavailable";
  out += "\",\"calibration\":{\"mode\":\"";
  out += magCalibrationModeName(calibration_.mode);
  out += "\"";
  if (calibration_.mode != MagCalibrationMode::None) {
    out += ",\"offset\":";
    appendFloatArray(out, calibration_.offset, 3, 3);
    out += ",\"matrix\":";
    appendFloatArray(out, calibration_.matrix, 9, 5);
    out += ",\"field_ut\":";
    out += String(calibration_.fieldUt, 2);
    out += ",\"residual_pct\":";
    out += String(calibration_.residualPct, 2);
    out += ",\"calibrated_at_ms\":";
    appendU64(out, calibration_.calibratedAtMs);
    out += ",\"suspect\":";
    out += drift_.suspect() ? "true" : "false";
    out += ",\"field_median_ut\":";
    out += String(drift_.medianUt(), 2);
  }
  out += ",\"interval_days\":";
  out += intervalDays_;
  out += "}";
  out += ",\"odr_hz\":";
  out += odrHz_;
  if (averaging_) {
    out += ",\"averaging\":";
    out += averaging_;
  }
  out += ",\"poll_interval_us\":";
  out += pollIntervalUs_;
  out += ",\"measured_rate_hz\":";
  out += String(measuredRateHz_, 1);
  out += ",\"sample_valid\":";
  out += sample_.valid() ? "true" : "false";
  out += ",\"last_error\":\"";
  out += error_;
  out += "\"}";
  return out;
}

}  // namespace nhos
