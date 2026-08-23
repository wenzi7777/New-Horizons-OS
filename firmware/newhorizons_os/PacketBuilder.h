#pragma once

#include <Arduino.h>

#include "Config.h"
#include "MatrixScanner.h"
#include "PacketV5Codec.h"

namespace nhos {

using BatterySample = PacketBatteryBlock;

class PacketBuilder {
 public:
  void setDeviceUid(const uint8_t uid[6]);
  size_t build(const MatrixFrame& frame, uint64_t epochMs, bool epochValid, uint8_t* out, size_t capacity, const float* imuData = nullptr, const float* magData = nullptr, const BatterySample* battery = nullptr);
  size_t buildMatrixPacketHeader(const MatrixFrame& frame, uint64_t epochMs, bool epochValid, uint8_t* out, size_t capacity, size_t matrixPayloadBytes, const float* imuData = nullptr, const float* magData = nullptr, const BatterySample* battery = nullptr);
  size_t buildHeartbeat(uint32_t seq, uint64_t epochMs, bool epochValid, uint8_t* out, size_t capacity);

 private:
  uint8_t deviceUid_[6] = {0xA5, 0x5A, 0, 0, 0, 1};
};

}  // namespace nhos
