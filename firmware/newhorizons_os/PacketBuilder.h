#pragma once

#include <Arduino.h>

#include "Config.h"
#include "MatrixScanner.h"
#include "PacketSensorBlocks.h"

namespace nhos {

struct BatterySample {
  uint8_t status = 0;
  uint8_t fault = 0;
  uint16_t vbatMv = 0;
};

class PacketBuilder {
 public:
  void setDeviceUid(const uint8_t uid[6]);
  size_t build(const MatrixFrame& frame, uint64_t epochMs, bool epochValid, uint8_t* out, size_t capacity, const float* imuData = nullptr, const float* magData = nullptr, const BatterySample* battery = nullptr);
  size_t buildMatrixPacketHeader(const MatrixFrame& frame, uint64_t epochMs, bool epochValid, uint8_t* out, size_t capacity, size_t matrixPayloadBytes, const float* imuData = nullptr, const float* magData = nullptr);
  size_t buildHeartbeat(uint32_t seq, uint64_t epochMs, bool epochValid, uint8_t* out, size_t capacity);

 private:
  void putU16(uint8_t* out, uint16_t value);
  void putU32(uint8_t* out, uint32_t value);
  void putU64(uint8_t* out, uint64_t value);
  void putFloat(uint8_t* out, float value);

  uint8_t deviceUid_[6] = {0xA5, 0x5A, 0, 0, 0, 1};
};

}  // namespace nhos
