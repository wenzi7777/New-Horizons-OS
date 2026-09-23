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

 protected:
  using BoschSensorClass::configure_sensor;
  int8_t configure_sensor(struct bmi2_dev* dev) override;

 private:
  struct bmi2_dev* dev_ = nullptr;
};

// The one instance. Both ImuManager and MagnetometerManager use it: on boards
// with a BMM150 the magnetometer is hosted by this same driver, so it must be
// the instance that was begun.
extern ImuDriver imuDriver;

}  // namespace nhos
