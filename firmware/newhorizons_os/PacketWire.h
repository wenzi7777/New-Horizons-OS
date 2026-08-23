#pragma once

#include <cstddef>
#include <cstdint>

namespace nhos {

// NHO/Arduino/1 packet wire constants.  This header deliberately has no
// Arduino dependency so native tests can exercise the exact v5 layout.
constexpr uint16_t kPacketMagic = 0xA55A;
constexpr uint8_t kPacketVersion = 5;
constexpr uint8_t kPacketFlagImu = 0x01;
constexpr uint8_t kPacketFlagBattery = 0x02;
constexpr uint8_t kPacketFlagMag = 0x04;
constexpr uint8_t kPacketFlagRawAdc = 0x08;
constexpr uint8_t kPacketFlagEpochValid = 0x10;
constexpr uint8_t kPacketFlagExtensions = 0x20;
constexpr uint8_t kPacketFlagHmac = 0x40;
constexpr uint8_t kPacketFlagHeartbeat = 0x80;
constexpr size_t kPacketHeaderLen = 24;
constexpr size_t kPacketHmacLen = 16;

constexpr uint8_t kPacketBaseImuFloatCount = 7;
constexpr uint8_t kPacketMagFloatCount = 3;
constexpr size_t kPacketBaseImuBytes = kPacketBaseImuFloatCount * sizeof(float);
constexpr size_t kPacketMagBytes = kPacketMagFloatCount * sizeof(float);
constexpr size_t kPacketBatteryBytes = 6;

constexpr size_t kPacketExtensionTlvHeaderLen = 2;
// The v5 producer does not emit TLVs yet.  Reserve this bounded amount so a
// future producer cannot silently grow a data frame beyond the ESP-NOW budget.
constexpr size_t kPacketExtensionBudget = 128;
constexpr size_t kEspNowDataFrameBudget = 3840;

bool packetExtensionsAreWellFormed(const uint8_t* data, size_t length);
bool packetExtensionBytesFit(size_t basePayloadBytes, size_t extensionBytes,
                             size_t packetCapacity);

}  // namespace nhos
