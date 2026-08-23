#pragma once

#include <cstddef>
#include <cstdint>

#include "PacketWire.h"

namespace nhos {

constexpr uint8_t kPacketSensorFlagImu = kPacketFlagImu;
constexpr uint8_t kPacketSensorFlagBattery = kPacketFlagBattery;
constexpr uint8_t kPacketSensorFlagMag = kPacketFlagMag;

struct PacketSensorBlocks {
  bool hasImu;
  bool hasMag;
  bool hasBattery;
};

size_t packetSensorPayloadBytes(PacketSensorBlocks blocks);
uint8_t packetSensorFlags(PacketSensorBlocks blocks);

}  // namespace nhos
