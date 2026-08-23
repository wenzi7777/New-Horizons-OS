#include "PacketSensorBlocks.h"

namespace nhos {

size_t packetSensorPayloadBytes(PacketSensorBlocks blocks) {
  size_t bytes = blocks.hasImu ? kPacketBaseImuBytes : 0;
  bytes += blocks.hasMag ? kPacketMagBytes : 0;
  bytes += blocks.hasBattery ? kPacketBatteryBytes : 0;
  return bytes;
}

uint8_t packetSensorFlags(PacketSensorBlocks blocks) {
  uint8_t flags = 0;
  if (blocks.hasImu) flags |= kPacketSensorFlagImu;
  if (blocks.hasMag) flags |= kPacketSensorFlagMag;
  if (blocks.hasBattery) flags |= kPacketSensorFlagBattery;
  return flags;
}

}  // namespace nhos
