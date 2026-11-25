#pragma once

#include <Arduino.h>

namespace nhos {

class ImuManager;
class MagnetometerManager;
class MatrixScanner;

// The one place the scan rate reaches the IMU and the magnetometer: poll
// interval = scan interval, chip output data rate = the lowest that keeps up
// (SensorRatePolicy). Call it wherever the scan timing changes.
void applySensorRates(const MatrixScanner& scanner, ImuManager* imu,
                      MagnetometerManager* magnetometer);

// Whether the magnetometer should run now: the persisted switch, no operator
// stop -- and, on GCU boards where the IMU driver hosts the BMM150, an IMU
// that is up. Call it after anything that changes one of those.
void syncMagnetometer(MagnetometerManager* magnetometer, bool configEnabled,
                      bool operatorStopped, const ImuManager* imu);

}  // namespace nhos
