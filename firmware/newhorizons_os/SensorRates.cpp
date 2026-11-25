#include "SensorRates.h"

#include "ImuManager.h"
#include "MagnetometerManager.h"
#include "MatrixScanner.h"

namespace nhos {

void applySensorRates(const MatrixScanner& scanner, ImuManager* imu,
                      MagnetometerManager* magnetometer) {
  const uint32_t intervalUs = scanner.scanIntervalUs();
  const uint16_t fps = scanner.targetFps();
  if (imu != nullptr) {
    imu->setServiceIntervalUs(intervalUs);
    imu->setOdrForFps(fps);
  }
  if (magnetometer != nullptr) {
    magnetometer->setRateForScan(fps, intervalUs);
  }
}

void syncMagnetometer(MagnetometerManager* magnetometer, bool configEnabled,
                      bool operatorStopped, const ImuManager* imu) {
  if (magnetometer == nullptr) return;
  const bool hostReady = imu != nullptr && imu->initialized();
  magnetometer->applyState(configEnabled && !operatorStopped, hostReady);
}

}  // namespace nhos
