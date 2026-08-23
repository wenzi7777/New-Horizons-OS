#include "PacketBuilder.h"

namespace nhos {

void PacketBuilder::setDeviceUid(const uint8_t uid[6]) {
  memcpy(deviceUid_, uid, 6);
}

size_t PacketBuilder::build(const MatrixFrame& frame, uint64_t epochMs, bool epochValid, uint8_t* out, size_t capacity, const float* imuData, const float* magData, const BatterySample* battery) {
  if (frame.pointCount > kMaxSensors) {
    return 0;
  }
  const PacketV5BuildInput input{
      deviceUid_, frame.seq, epochMs, epochValid, frame.values, frame.rawValues,
      frame.pointCount, frame.hasRaw, imuData, magData, battery};
  return buildPacketV5(input, out, capacity);
}

size_t PacketBuilder::buildMatrixPacketHeader(const MatrixFrame& frame, uint64_t epochMs, bool epochValid, uint8_t* out, size_t capacity, size_t matrixPayloadBytes, const float* imuData, const float* magData, const BatterySample* battery) {
  if (frame.pointCount > kMaxSensors) {
    return 0;
  }
  const PacketV5BuildInput input{
      deviceUid_, frame.seq, epochMs, epochValid, nullptr, frame.rawValues,
      frame.pointCount, frame.hasRaw, imuData, magData, battery};
  return buildPacketV5HeaderAndTail(input, out, capacity, matrixPayloadBytes);
}

size_t PacketBuilder::buildHeartbeat(uint32_t seq, uint64_t epochMs, bool epochValid, uint8_t* out, size_t capacity) {
  if (!out || capacity < kPacketHeaderLen) {
    return 0;
  }

  out[0] = static_cast<uint8_t>(kPacketMagic & 0xff);
  out[1] = static_cast<uint8_t>((kPacketMagic >> 8) & 0xff);
  out[2] = kPacketVersion;
  out[3] = kPacketFlagHeartbeat | (epochValid ? kPacketFlagEpochValid : 0);
  memcpy(out + 4, deviceUid_, 6);
  for (uint8_t i = 0; i < 4; ++i) out[10 + i] = static_cast<uint8_t>((seq >> (8 * i)) & 0xff);
  const uint64_t timestamp = epochValid ? epochMs : 0;
  for (uint8_t i = 0; i < 8; ++i) out[14 + i] = static_cast<uint8_t>((timestamp >> (8 * i)) & 0xff);
  out[22] = 0;
  out[23] = 0;
  return kPacketHeaderLen;
}

}  // namespace nhos
