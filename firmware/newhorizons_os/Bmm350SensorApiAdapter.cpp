#include "Bmm350SensorApiAdapter.h"

#include <Wire.h>

namespace nhos {
namespace {
constexpr uint8_t kBmm350Address = BMM350_I2C_ADSEL_SET_LOW;
constexpr uint8_t kWireChunkBytes = 24;
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
  lastResult_ = bmm350_set_odr_performance(BMM350_DATA_RATE_25HZ,
                                           BMM350_AVERAGING_8, &device_);
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
  if (!data) return BMM350_E_NULL_PTR;
  uint32_t offset = 0;
  while (offset < length) {
    const uint8_t count = static_cast<uint8_t>(
        (length - offset) > kWireChunkBytes ? kWireChunkBytes : length - offset);
    Wire.beginTransmission(kBmm350Address);
    Wire.write(static_cast<uint8_t>(reg + offset));
    if (Wire.endTransmission(false) != 0 ||
        Wire.requestFrom(kBmm350Address, count) != count) return BMM350_E_COM_FAIL;
    for (uint8_t i = 0; i < count; ++i) {
      if (!Wire.available()) return BMM350_E_COM_FAIL;
      data[offset + i] = static_cast<uint8_t>(Wire.read());
    }
    offset += count;
  }
  return BMM350_INTF_RET_SUCCESS;
}

BMM350_INTF_RET_TYPE Bmm350SensorApiAdapter::write(uint8_t reg, const uint8_t* data,
                                                    uint32_t length) {
  if (!data) return BMM350_E_NULL_PTR;
  uint32_t offset = 0;
  while (offset < length) {
    const uint8_t count = static_cast<uint8_t>(
        (length - offset) > kWireChunkBytes ? kWireChunkBytes : length - offset);
    Wire.beginTransmission(kBmm350Address);
    Wire.write(static_cast<uint8_t>(reg + offset));
    if (Wire.write(data + offset, count) != count || Wire.endTransmission() != 0) {
      return BMM350_E_COM_FAIL;
    }
    offset += count;
  }
  return BMM350_INTF_RET_SUCCESS;
}

}  // namespace nhos
