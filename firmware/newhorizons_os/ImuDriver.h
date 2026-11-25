#pragma once

#include <Arduino.h>

#include "Arduino_BMI270_BMM150.h"

namespace nhos {

// The Arduino BMI270 driver, with one addition: read accelerometer AND
// gyroscope in a single bus transaction.
//
// Why this exists. The library's readAcceleration() and readGyroscope() each
// call bmi2_get_sensor_data(), which reads BOTH sensors' data registers and
// then discards half. Calling the two back to back therefore read everything
// twice -- ~4.8ms per sample on v1.5.F, about a quarter of the CPU at 60Hz.
// The continuous (FIFO) mode the firmware enabled never helped either: the
// FIFO is only drained by accelerationAvailable()/gyroscopeAvailable(), which
// nothing called, so every read fell through to the one-shot path anyway.
//
// Why not read the registers directly. bmi2_get_sensor_data() applies the
// chip's factory cross-axis correction to gyro X (gyr.x -= zx * gyr.z / 512,
// with zx read from that chip's NVM at init) and the configured axis remap.
// A raw register read would silently drop both. Going through the Bosch API
// once keeps every compensation exactly as it was.
//
// configure_sensor() is the library's documented extension point ("can be
// modified by subclassing"), and it is handed the driver's own persistent
// bmi2_dev -- so capturing it there is safe for the life of the object.
class ImuDriver : public BoschSensorClass {
 public:
  explicit ImuDriver(TwoWire& wire = Wire) : BoschSensorClass(wire) {}

  // Results in g and degrees/second, identical in scale and orientation to
  // readAcceleration()/readGyroscope().
  bool readAccelGyro(float acc[3], float gyr[3]);

  // Output data rate of the accelerometer and gyroscope (25/50/100/200 Hz;
  // see SensorRatePolicy). Kept across begin(): configure_sensor() applies
  // it after the library's own setup, so a re-begun IMU keeps its rate.
  // Written to the chip at once when it is already up. Returns false when
  // the chip rejected it.
  bool setImuOdrHz(uint16_t hz);
  uint16_t imuOdrHz() const { return imuOdrHz_; }

  // BMM150 (GCU boards, hosted by this driver): output data rate in normal
  // mode, 10/15/20/25/30 Hz, and sleep/wake. Same persistence as above.
  bool setMagOdrHz(uint16_t hz);
  uint16_t magOdrHz() const { return magOdrHz_; }
  bool setMagAwake(bool awake);
  bool magHosted() const { return magDev_ != nullptr; }

 protected:
  using BoschSensorClass::configure_sensor;
  int8_t configure_sensor(struct bmi2_dev* dev) override;
  int8_t configure_sensor(struct bmm150_dev* dev) override;

 private:
  int8_t applyImuOdr();
  int8_t applyMagOdr();

  struct bmi2_dev* dev_ = nullptr;
  struct bmm150_dev* magDev_ = nullptr;
  uint16_t imuOdrHz_ = 100;  // the library's own default
  uint16_t magOdrHz_ = 10;   // the BMM150's reset default
  bool magAwake_ = true;
};

// The one instance. Both ImuManager and MagnetometerManager use it: on boards
// with a BMM150 the magnetometer is hosted by this same driver, so it must be
// the instance that was begun.
extern ImuDriver imuDriver;

}  // namespace nhos
