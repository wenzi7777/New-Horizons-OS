#include "ImuDriver.h"

namespace nhos {

namespace {
// Must match the library's own conversion factors (BMI270.cpp): it configures
// the accelerometer at +-4g and the gyroscope at +-2000dps.
constexpr float kInt16ToG = 8192.0f;
constexpr float kInt16ToDps = 16.384f;

uint8_t bmi2OdrCode(uint16_t hz, bool gyro) {
  if (hz >= 200) return gyro ? BMI2_GYR_ODR_200HZ : BMI2_ACC_ODR_200HZ;
  if (hz >= 100) return gyro ? BMI2_GYR_ODR_100HZ : BMI2_ACC_ODR_100HZ;
  if (hz >= 50) return gyro ? BMI2_GYR_ODR_50HZ : BMI2_ACC_ODR_50HZ;
  return gyro ? BMI2_GYR_ODR_25HZ : BMI2_ACC_ODR_25HZ;
}

uint8_t bmm150OdrCode(uint16_t hz) {
  if (hz >= 30) return BMM150_DATA_RATE_30HZ;
  if (hz >= 25) return BMM150_DATA_RATE_25HZ;
  if (hz >= 20) return BMM150_DATA_RATE_20HZ;
  if (hz >= 15) return BMM150_DATA_RATE_15HZ;
  return BMM150_DATA_RATE_10HZ;
}
}  // namespace

ImuDriver imuDriver(Wire);

int8_t ImuDriver::configure_sensor(struct bmi2_dev* dev) {
  dev_ = dev;
  const int8_t rslt = BoschSensorClass::configure_sensor(dev);
  if (rslt != BMI2_OK) return rslt;
  // The library always sets 100 Hz; replace it with the rate the scan needs.
  return applyImuOdr();
}

int8_t ImuDriver::configure_sensor(struct bmm150_dev* dev) {
  magDev_ = dev;
  // The library puts the BMM150 in normal mode at its reset rate (its preset
  // call is commented out). Keep that, then set the rate and power state.
  const int8_t rslt = BoschSensorClass::configure_sensor(dev);
  if (rslt != BMM150_OK) return rslt;
  const int8_t odr = applyMagOdr();
  if (odr != BMM150_OK) return odr;
  if (!magAwake_) {
    struct bmm150_settings settings {};
    settings.pwr_mode = BMM150_POWERMODE_SLEEP;
    return bmm150_set_op_mode(&settings, magDev_);
  }
  return BMM150_OK;
}

int8_t ImuDriver::applyImuOdr() {
  if (dev_ == nullptr) return BMI2_E_NULL_PTR;
  struct bmi2_sens_config cfg[2];
  cfg[0].type = BMI2_ACCEL;
  cfg[1].type = BMI2_GYRO;
  int8_t rslt = bmi2_get_sensor_config(cfg, 2, dev_);
  if (rslt != BMI2_OK) return rslt;
  cfg[0].cfg.acc.odr = bmi2OdrCode(imuOdrHz_, false);
  cfg[1].cfg.gyr.odr = bmi2OdrCode(imuOdrHz_, true);
  return bmi2_set_sensor_config(cfg, 2, dev_);
}

int8_t ImuDriver::applyMagOdr() {
  if (magDev_ == nullptr) return BMM150_E_NULL_PTR;
  struct bmm150_settings settings {};
  settings.data_rate = bmm150OdrCode(magOdrHz_);
  return bmm150_set_sensor_settings(BMM150_SEL_DATA_RATE, &settings, magDev_);
}

bool ImuDriver::setImuOdrHz(uint16_t hz) {
  imuOdrHz_ = hz;
  if (dev_ == nullptr) return true;  // applied by the next begin()
  return applyImuOdr() == BMI2_OK;
}

bool ImuDriver::setMagOdrHz(uint16_t hz) {
  magOdrHz_ = hz;
  if (magDev_ == nullptr) return true;
  return applyMagOdr() == BMM150_OK;
}

bool ImuDriver::setMagAwake(bool awake) {
  magAwake_ = awake;
  if (magDev_ == nullptr) return true;
  struct bmm150_settings settings {};
  settings.pwr_mode = awake ? BMM150_POWERMODE_NORMAL : BMM150_POWERMODE_SLEEP;
  if (bmm150_set_op_mode(&settings, magDev_) != BMM150_OK) return false;
  // The data-rate register survives sleep; it is written again on wake
  // anyway, so a rate set while asleep is never lost.
  return !awake || applyMagOdr() == BMM150_OK;
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
