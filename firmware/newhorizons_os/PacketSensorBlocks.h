#pragma once

#include <cstddef>
#include <cstdint>

namespace nhos {

constexpr uint8_t kPacketSensorFlagImu = 0x01;
constexpr uint8_t kPacketSensorFlagBattery = 0x02;
constexpr uint8_t kPacketSensorFlagMag = 0x04;
constexpr uint8_t kPacketBaseImuFloatCount = 7;
constexpr uint8_t kPacketMagFloatCount = 3;

struct PacketSensorBlocks {
  bool hasImu;
  bool hasMag;
  bool hasBattery;
};

size_t packetSensorPayloadBytes(PacketSensorBlocks blocks);
uint8_t packetSensorFlags(PacketSensorBlocks blocks);

}  // namespace nhos
