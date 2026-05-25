#include "PacketBuilder.h"

namespace nhos {

void PacketBuilder::setDeviceUid(const uint8_t uid[6]) {
  memcpy(deviceUid_, uid, 6);
}

size_t PacketBuilder::build(const MatrixFrame& frame, uint8_t* out, size_t capacity, const float* imu7, const BatterySample* battery) {
  const size_t matrixBytes = static_cast<size_t>(frame.pointCount) * sizeof(float);
  const size_t imuBytes = imu7 ? 7 * sizeof(float) : 0;
  const size_t batteryBytes = battery ? 4 : 0;
  const size_t payloadLen = matrixBytes + imuBytes + batteryBytes;
  const size_t totalLen = kPacketHeaderLen + payloadLen;
  if (!out || capacity < totalLen || frame.pointCount > kMaxSensors) {
    return 0;
  }

  uint8_t flags = 0;
  if (imu7) {
    flags |= kPacketFlagImu;
  }
  if (battery) {
    flags |= kPacketFlagBattery;
  }

  putU16(out, kPacketMagic);
  out[2] = kPacketVersion;
  out[3] = flags;
  memcpy(out + 4, deviceUid_, 6);
  putU32(out + 10, frame.seq);
  putU32(out + 14, frame.timestampMs);
  putU16(out + 18, static_cast<uint16_t>(payloadLen));

  size_t offset = kPacketHeaderLen;
  for (uint16_t i = 0; i < frame.pointCount; ++i) {
    putFloat(out + offset, frame.values[i]);
    offset += sizeof(float);
  }
  if (imu7) {
    for (uint8_t i = 0; i < 7; ++i) {
      putFloat(out + offset, imu7[i]);
      offset += sizeof(float);
    }
  }
  if (battery) {
    out[offset++] = battery->status;
    out[offset++] = battery->fault;
    putU16(out + offset, battery->vbatMv);
    offset += sizeof(uint16_t);
  }
  return offset;
}

size_t PacketBuilder::buildMatrixPacketHeader(const MatrixFrame& frame, uint8_t* out, size_t capacity, size_t matrixPayloadBytes) {
  const size_t expectedMatrixBytes = static_cast<size_t>(frame.pointCount) * sizeof(float);
  const size_t totalLen = kPacketHeaderLen + matrixPayloadBytes;
  if (!out || capacity < totalLen || frame.pointCount > kMaxSensors || matrixPayloadBytes != expectedMatrixBytes) {
    return 0;
  }

  putU16(out, kPacketMagic);
  out[2] = kPacketVersion;
  out[3] = 0;
  memcpy(out + 4, deviceUid_, 6);
  putU32(out + 10, frame.seq);
  putU32(out + 14, frame.timestampMs);
  putU16(out + 18, static_cast<uint16_t>(matrixPayloadBytes));
  return totalLen;
}

size_t PacketBuilder::buildHeartbeat(uint32_t seq, uint32_t timestampMs, uint8_t* out, size_t capacity) {
  if (!out || capacity < kPacketHeaderLen) {
    return 0;
  }

  putU16(out, kPacketMagic);
  out[2] = kPacketVersion;
  out[3] = kPacketFlagHeartbeat;
  memcpy(out + 4, deviceUid_, 6);
  putU32(out + 10, seq);
  putU32(out + 14, timestampMs);
  putU16(out + 18, 0);
  return kPacketHeaderLen;
}

void PacketBuilder::putU16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xff);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void PacketBuilder::putU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xff);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

void PacketBuilder::putFloat(uint8_t* out, float value) {
  memcpy(out, &value, sizeof(float));
}

}  // namespace nhos
