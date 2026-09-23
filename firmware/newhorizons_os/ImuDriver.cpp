#include "ImuDriver.h"

namespace nhos {

namespace {
// Must match the library's own conversion factors (BMI270.cpp): it configures
// the accelerometer at +-4g and the gyroscope at +-2000dps.
constexpr float kInt16ToG = 8192.0f;
constexpr float kInt16ToDps = 16.384f;
}  // namespace

ImuDriver imuDriver(Wire);

int8_t ImuDriver::configure_sensor(struct bmi2_dev* dev) {
  dev_ = dev;
  return BoschSensorClass::configure_sensor(dev);
}

bool ImuDriver::readAccelGyro(float acc[3], float gyr[3]) {
  if (dev_ == nullptr) {
    return false;
  }
  struct bmi2_sens_data data;
  if (bmi2_get_sensor_data(&data, dev_) != BMI2_OK) {
    return false;
  }
  // Same orientation as the library on every non-Nano33BLE target.
  acc[0] = data.acc.x / kInt16ToG;
  acc[1] = data.acc.y / kInt16ToG;
  acc[2] = data.acc.z / kInt16ToG;
  gyr[0] = data.gyr.x / kInt16ToDps;
  gyr[1] = data.gyr.y / kInt16ToDps;
  gyr[2] = data.gyr.z / kInt16ToDps;
  return true;
}

}  // namespace nhos
