#pragma once

#include <cstddef>
#include <cstdint>

#include "PacketWire.h"

namespace nhos {

struct PacketBatteryBlock {
  uint8_t status = 0;
  uint8_t fault = 0;
  uint16_t vbatMv = 0;
  uint16_t socCentiPercent = 0;
};

struct PacketV5BuildInput {
  const uint8_t* deviceUid = nullptr;
  uint32_t sequence = 0;
  uint64_t epochMs = 0;
  bool epochValid = false;
  const float* matrixValues = nullptr;
  const float* rawValues = nullptr;
  uint16_t pointCount = 0;
  bool hasRaw = false;
  const float* imuData = nullptr;
  const float* magData = nullptr;
  const PacketBatteryBlock* battery = nullptr;
};

size_t packetV5PayloadBytes(const PacketV5BuildInput& input);
size_t buildPacketV5(const PacketV5BuildInput& input, uint8_t* out,
                     size_t capacity);
// MatrixScanner has already written its matrix block when it calls this
// optimized form.  It writes the header and all optional trailing blocks.
size_t buildPacketV5HeaderAndTail(const PacketV5BuildInput& input,
                                  uint8_t* out, size_t capacity,
                                  size_t matrixPayloadBytes);

}  // namespace nhos
