#pragma once

#include <Arduino.h>

#include "BoardConfig.h"
#include "MagCalibration.h"
#include "MagnetometerSampleCache.h"
#if NHOS_BOARD_MAG_MODEL == 2
#include "Bmm350SensorApiAdapter.h"
#endif

namespace nhos {

class MagnetometerManager {
 public:
  // enabled: the persisted switch (mag.enabled) and no operator stop.
  // bmm150HostReady: BMM150 is hosted by the Arduino combined IMU driver, so
  // on GCU boards it can only run when the IMU actually came up.
  void begin(bool enabled, bool bmm150HostReady = false);
  // The one rule for "should it run now": called by set_mag, set_imu (GCU:
  // the IMU hosts the BMM150), soft-off resume and service start/stop.
  // Re-begins the chip when it has to; puts it to sleep/suspend when off.
  void applyState(bool enabled, bool bmm150HostReady);
  void service(uint32_t nowUs);

  // Chip rate and poll interval for this scan rate (SensorRatePolicy). The
  // poll interval is the scan interval; the chip runs at the lowest rate that
  // keeps up (BMM150: 30 Hz at most -- repeats above that are accepted).
  void setRateForScan(uint16_t fps, uint32_t scanIntervalUs);

  // Hard/soft-iron calibration applied to every sample before it is cached,
  // so the stream, sensor_sample and apps' mag() all see calibrated values.
  void setCalibration(const MagCalibrationData& cal);
  const MagCalibrationData& calibration() const { return calibration_; }
  void setCalibrationIntervalDays(uint16_t days) { intervalDays_ = days; }

  bool copyLatestSample(float out[3]) const;     // calibrated
  bool copyLatestRawSample(float out[3]) const;  // factory-compensated only
  // Exposed for ServiceManager's health probe.
  bool initialized() const { return initialized_; }
  bool enabled() const { return enabled_; }
  String statusJson() const;

 private:
  void beginChip(bool bmm150HostReady);
  void stopChip();

  bool enabled_ = false;
  bool initialized_ = false;
  bool ready_ = false;
  bool compensationAvailable_ = false;
  bool blockedByImu_ = false;
  MagnetometerSampleCache sample_;
  MagnetometerSampleCache rawSample_;
  String error_;
  uint32_t lastPollUs_ = 0;
  uint32_t pollIntervalUs_ = 50000;
  uint16_t odrHz_ = 0;
  uint8_t averaging_ = 0;
  uint32_t rateWindowStartMs_ = 0;
  uint32_t rateWindowCount_ = 0;
  float measuredRateHz_ = 0.0f;
  MagCalibrationData calibration_;
  uint16_t intervalDays_ = 30;
  magcal::MagCalibrationDriftMonitor drift_;
#if NHOS_BOARD_MAG_MODEL == 2
  Bmm350SensorApiAdapter bmm350_;
#endif
};

}  // namespace nhos
