#include "PacketV5Codec.h"

#include <cstring>
#include <limits>

namespace nhos {
namespace {

void putU16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xff);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void putU32(uint8_t* out, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) {
    out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
  }
}

void putU64(uint8_t* out, uint64_t value) {
  for (uint8_t i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
  }
}

void putFloat(uint8_t* out, float value) {
  std::memcpy(out, &value, sizeof(value));
}

bool matrixBytesFor(const PacketV5BuildInput& input, size_t& matrixBytes) {
  matrixBytes = static_cast<size_t>(input.pointCount) * sizeof(float);
  return true;
}

uint8_t flagsFor(const PacketV5BuildInput& input) {
  uint8_t flags = input.epochValid ? kPacketFlagEpochValid : 0;
  if (input.hasRaw) flags |= kPacketFlagRawAdc;
  if (input.imuData) flags |= kPacketFlagImu;
  if (input.magData) flags |= kPacketFlagMag;
  if (input.battery) flags |= kPacketFlagBattery;
  return flags;
}

bool headerAndCapacityAreValid(const PacketV5BuildInput& input, uint8_t* out,
                               size_t capacity, size_t matrixPayloadBytes,
                               size_t& payloadBytes) {
  payloadBytes = packetV5PayloadBytes(input);
  if (!out || !input.deviceUid || matrixPayloadBytes !=
      static_cast<size_t>(input.pointCount) * sizeof(float) ||
      payloadBytes > std::numeric_limits<uint16_t>::max() ||
      kPacketHeaderLen > capacity || payloadBytes > capacity - kPacketHeaderLen) {
    return false;
  }
  return true;
}

void writeHeader(const PacketV5BuildInput& input, uint8_t* out,
                 size_t payloadBytes) {
  putU16(out, kPacketMagic);
  out[2] = kPacketVersion;
  out[3] = flagsFor(input);
  std::memcpy(out + 4, input.deviceUid, 6);
  putU32(out + 10, input.sequence);
  putU64(out + 14, input.epochValid ? input.epochMs : 0);
  putU16(out + 22, static_cast<uint16_t>(payloadBytes));
}

size_t writeTrailingBlocks(const PacketV5BuildInput& input, uint8_t* out,
                           size_t offset) {
  if (input.hasRaw) {
    for (uint16_t i = 0; i < input.pointCount; ++i) {
      putFloat(out + offset, input.rawValues[i]);
      offset += sizeof(float);
    }
  }
  if (input.imuData) {
    for (uint8_t i = 0; i < kPacketBaseImuFloatCount; ++i) {
      putFloat(out + offset, input.imuData[i]);
      offset += sizeof(float);
    }
  }
  if (input.magData) {
    for (uint8_t i = 0; i < kPacketMagFloatCount; ++i) {
      putFloat(out + offset, input.magData[i]);
      offset += sizeof(float);
    }
  }
  if (input.battery) {
    out[offset++] = input.battery->status;
    out[offset++] = input.battery->fault;
    putU16(out + offset, input.battery->vbatMv);
    offset += sizeof(uint16_t);
    putU16(out + offset, input.battery->socCentiPercent);
    offset += sizeof(uint16_t);
  }
  return offset;
}

}  // namespace

bool packetExtensionsAreWellFormed(const uint8_t* data, size_t length) {
  if (!data && length != 0) {
    return false;
  }
  size_t offset = 0;
  while (offset < length) {
    if (length - offset < kPacketExtensionTlvHeaderLen) {
      return false;
    }
    const size_t valueLength = data[offset + 1];
    offset += kPacketExtensionTlvHeaderLen;
    if (valueLength > length - offset) {
      return false;
    }
    // Type is intentionally ignored: consumers skip unknown values by length.
    offset += valueLength;
  }
  return true;
}

bool packetExtensionBytesFit(size_t basePayloadBytes, size_t extensionBytes,
                             size_t packetCapacity) {
  if (extensionBytes > kPacketExtensionBudget ||
      basePayloadBytes > std::numeric_limits<uint16_t>::max() ||
      extensionBytes > std::numeric_limits<uint16_t>::max() - basePayloadBytes) {
    return false;
  }
  const size_t payloadBytes = basePayloadBytes + extensionBytes;
  return kPacketHeaderLen <= packetCapacity &&
         payloadBytes <= packetCapacity - kPacketHeaderLen;
}

size_t packetV5PayloadBytes(const PacketV5BuildInput& input) {
  size_t matrixBytes = 0;
  if (!matrixBytesFor(input, matrixBytes)) {
    return 0;
  }
  size_t payloadBytes = matrixBytes;
  if (input.hasRaw) payloadBytes += matrixBytes;
  if (input.imuData) payloadBytes += kPacketBaseImuBytes;
  if (input.magData) payloadBytes += kPacketMagBytes;
  if (input.battery) payloadBytes += kPacketBatteryBytes;
  return payloadBytes;
}

size_t buildPacketV5(const PacketV5BuildInput& input, uint8_t* out,
                     size_t capacity) {
  size_t matrixBytes = 0;
  if (!matrixBytesFor(input, matrixBytes) || !input.matrixValues ||
      (input.hasRaw && !input.rawValues)) {
    return 0;
  }
  size_t payloadBytes = 0;
  if (!headerAndCapacityAreValid(input, out, capacity, matrixBytes,
                                 payloadBytes)) {
    return 0;
  }
  writeHeader(input, out, payloadBytes);
  size_t offset = kPacketHeaderLen;
  for (uint16_t i = 0; i < input.pointCount; ++i) {
    putFloat(out + offset, input.matrixValues[i]);
    offset += sizeof(float);
  }
  return writeTrailingBlocks(input, out, offset);
}

size_t buildPacketV5HeaderAndTail(const PacketV5BuildInput& input,
                                  uint8_t* out, size_t capacity,
                                  size_t matrixPayloadBytes) {
  if (input.hasRaw && !input.rawValues) {
    return 0;
  }
  size_t payloadBytes = 0;
  if (!headerAndCapacityAreValid(input, out, capacity, matrixPayloadBytes,
                                 payloadBytes)) {
    return 0;
  }
  writeHeader(input, out, payloadBytes);
  return writeTrailingBlocks(input, out, kPacketHeaderLen + matrixPayloadBytes);
}

}  // namespace nhos
