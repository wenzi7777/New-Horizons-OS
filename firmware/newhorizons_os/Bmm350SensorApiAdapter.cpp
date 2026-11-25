#include "BoardConfig.h"

#if NHOS_BOARD_MAG_MODEL == 2
#include "Bmm350SensorApiAdapter.h"

#include <Wire.h>

#include "Bmm350I2cTransport.h"

namespace nhos {
namespace {
constexpr uint8_t kBmm350Address = BMM350_I2C_ADSEL_SET_LOW;
class WireBmm350Bus final : public Bmm350I2cBus {
 public:
  bool write(uint8_t address, uint8_t reg, const uint8_t* data, uint8_t length) override {
    Wire.beginTransmission(address);
    Wire.write(reg);
    return Wire.write(data, length) == length && Wire.endTransmission() == 0;
  }

  uint8_t read(uint8_t address, uint8_t reg, uint8_t* data, uint8_t length) override {
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, length) != length) return 0;
    for (uint8_t i = 0; i < length; ++i) {
      if (!Wire.available()) return i;
      data[i] = static_cast<uint8_t>(Wire.read());
    }
    return length;
  }
};

enum bmm350_data_rates odrCode(uint16_t hz) {
  if (hz >= 200) return BMM350_DATA_RATE_200HZ;
  if (hz >= 100) return BMM350_DATA_RATE_100HZ;
  if (hz >= 50) return BMM350_DATA_RATE_50HZ;
  return BMM350_DATA_RATE_25HZ;
}

enum bmm350_performance_parameters averagingCode(uint8_t samples) {
  if (samples >= 8) return BMM350_AVERAGING_8;
  if (samples >= 4) return BMM350_AVERAGING_4;
  if (samples >= 2) return BMM350_AVERAGING_2;
  return BMM350_NO_AVERAGING;
}
}

bool Bmm350SensorApiAdapter::begin() {
  device_ = {};
  device_.intf_ptr = this;
  device_.read = readCallback;
  device_.write = writeCallback;
  device_.delay_us = delayCallback;
  initialized_ = false;
  normalMode_ = false;

  lastResult_ = bmm350_init(&device_);  // Includes official OTP trim dump.
  if (lastResult_ != BMM350_OK) return false;
  lastResult_ = bmm350_set_odr_performance(odrCode(odrHz_),
                                           averagingCode(averaging_), &device_);
  if (lastResult_ != BMM350_OK) return false;
  lastResult_ = bmm350_enable_axes(BMM350_X_EN, BMM350_Y_EN, BMM350_Z_EN,
                                   &device_);
  if (lastResult_ != BMM350_OK) return false;
  lastResult_ = bmm350_set_powermode(BMM350_NORMAL_MODE, &device_);
  if (lastResult_ != BMM350_OK) return false;
  initialized_ = true;
  normalMode_ = true;
  return true;
}

bool Bmm350SensorApiAdapter::setOdr(uint16_t hz, uint8_t averaging) {
  odrHz_ = hz;
  averaging_ = averaging;
  if (!initialized_) return true;  // applied by the next begin()
  lastResult_ = bmm350_set_odr_performance(odrCode(odrHz_), averagingCode(averaging_),
                                           &device_);
  if (lastResult_ != BMM350_OK) return false;
  if (!normalMode_) return true;  // suspended: applied on wake
  lastResult_ = bmm350_set_powermode(BMM350_NORMAL_MODE, &device_);
  return lastResult_ == BMM350_OK;
}

bool Bmm350SensorApiAdapter::setAwake(bool awake) {
  if (!initialized_) return !awake;
  if (awake == normalMode_) return true;
  lastResult_ = bmm350_set_powermode(awake ? BMM350_NORMAL_MODE : BMM350_SUSPEND_MODE,
                                     &device_);
  if (lastResult_ != BMM350_OK) return false;
  normalMode_ = awake;
  return true;
}

bool Bmm350SensorApiAdapter::readCompensatedMicroTesla(float out[3]) {
  if (!out || !initialized_ || !normalMode_) return false;
  bmm350_mag_temp_data data{};
  lastResult_ = bmm350_get_compensated_mag_xyz_temp_data(&data, &device_);
  if (lastResult_ != BMM350_OK) return false;
  out[0] = data.x;
  out[1] = data.y;
  out[2] = data.z;
  return true;
}

BMM350_INTF_RET_TYPE Bmm350SensorApiAdapter::readCallback(uint8_t reg, uint8_t* data,
                                                           uint32_t length, void* context) {
  return static_cast<Bmm350SensorApiAdapter*>(context)->read(reg, data, length);
}

BMM350_INTF_RET_TYPE Bmm350SensorApiAdapter::writeCallback(uint8_t reg, const uint8_t* data,
                                                            uint32_t length, void* context) {
  return static_cast<Bmm350SensorApiAdapter*>(context)->write(reg, data, length);
}

void Bmm350SensorApiAdapter::delayCallback(uint32_t periodUs, void*) {
  while (periodUs > 1000) {
    delayMicroseconds(1000);
    periodUs -= 1000;
  }
  if (periodUs) delayMicroseconds(periodUs);
}

BMM350_INTF_RET_TYPE Bmm350SensorApiAdapter::read(uint8_t reg, uint8_t* data,
                                                   uint32_t length) {
  WireBmm350Bus bus;
  const Bmm350TransportResult result =
      Bmm350I2cTransport::read(bus, kBmm350Address, reg, data, length);
  return result == Bmm350TransportResult::Success ? BMM350_INTF_RET_SUCCESS
      : result == Bmm350TransportResult::NullPointer ? BMM350_E_NULL_PTR
      : BMM350_E_COM_FAIL;
}

BMM350_INTF_RET_TYPE Bmm350SensorApiAdapter::write(uint8_t reg, const uint8_t* data,
                                                    uint32_t length) {
  WireBmm350Bus bus;
  const Bmm350TransportResult result =
      Bmm350I2cTransport::write(bus, kBmm350Address, reg, data, length);
  return result == Bmm350TransportResult::Success ? BMM350_INTF_RET_SUCCESS
      : result == Bmm350TransportResult::NullPointer ? BMM350_E_NULL_PTR
      : BMM350_E_COM_FAIL;
}

}  // namespace nhos
#endif  // NHOS_BOARD_MAG_MODEL == 2
