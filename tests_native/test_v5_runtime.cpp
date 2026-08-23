#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "BatteryLedPolicy.h"
#include "BatteryManualProfile.h"
#include "PacketV5Codec.h"

namespace {

uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

void testValidOptionalSamplesProduceV5Blocks() {
  using namespace nhos;
  const uint8_t uid[6] = {1, 2, 3, 4, 5, 6};
  const float matrix[] = {1.25f, 2.5f};
  const float imu[] = {10.f, 11.f, 12.f, 13.f, 14.f, 15.f, 16.f};
  const float mag[] = {21.f, 22.f, 23.f};
  const PacketBatteryBlock battery{3, 0, 3777, 6543};
  const PacketV5BuildInput input{
      uid, 99, 123456789ULL, true, matrix, nullptr, 2, false, imu, mag,
      &battery};
  uint8_t packet[256] = {};

  const size_t len = buildPacketV5(input, packet, sizeof(packet));
  const size_t expectedPayload =
      2 * sizeof(float) + kPacketBaseImuBytes + kPacketMagBytes +
      kPacketBatteryBytes;
  assert(len == kPacketHeaderLen + expectedPayload);
  assert(readU16(packet) == kPacketMagic);
  assert(packet[2] == kPacketVersion);
  assert((packet[3] & (kPacketFlagImu | kPacketFlagMag | kPacketFlagBattery |
                       kPacketFlagEpochValid)) ==
         (kPacketFlagImu | kPacketFlagMag | kPacketFlagBattery |
          kPacketFlagEpochValid));
  assert(readU16(packet + 22) == expectedPayload);

  const size_t batteryOffset = kPacketHeaderLen + 2 * sizeof(float) +
                               kPacketBaseImuBytes + kPacketMagBytes;
  assert(packet[batteryOffset] == 3);
  assert(packet[batteryOffset + 1] == 0);
  assert(readU16(packet + batteryOffset + 2) == 3777);
  assert(readU16(packet + batteryOffset + 4) == 6543);
}

void testMissingOptionalSamplesOmitTheirV5Blocks() {
  using namespace nhos;
  const uint8_t uid[6] = {};
  const float matrix[] = {1.f};
  const PacketV5BuildInput input{
      uid, 1, 0, false, matrix, nullptr, 1, false, nullptr, nullptr, nullptr};
  uint8_t packet[128] = {};

  const size_t len = buildPacketV5(input, packet, sizeof(packet));
  assert(len == kPacketHeaderLen + sizeof(float));
  assert((packet[3] & (kPacketFlagImu | kPacketFlagMag | kPacketFlagBattery)) ==
         0);
  assert(readU16(packet + 22) == sizeof(float));
}

void testExtensionsAreBoundedAndMalformedTlvIsRejected() {
  using namespace nhos;
  const uint8_t skippableUnknown[] = {0x7f, 2, 0xaa, 0xbb};
  const uint8_t truncatedValue[] = {0x7f, 3, 0xaa};
  const uint8_t truncatedHeader[] = {0x7f};
  assert(packetExtensionsAreWellFormed(skippableUnknown,
                                       sizeof(skippableUnknown)));
  assert(!packetExtensionsAreWellFormed(truncatedValue, sizeof(truncatedValue)));
  assert(!packetExtensionsAreWellFormed(truncatedHeader, sizeof(truncatedHeader)));
  assert(packetExtensionBytesFit(100, kPacketExtensionBudget,
                                 kPacketHeaderLen + 100 +
                                     kPacketExtensionBudget));
  assert(!packetExtensionBytesFit(100, kPacketExtensionBudget + 1,
                                  kPacketHeaderLen + 100 +
                                      kPacketExtensionBudget + 1));
}

void testManualBatteryProfileOnlyAcceptsSafeHardwareSteps() {
  using namespace nhos;
  assert(validManualBatteryProfile(500, 100));
  assert(validManualBatteryProfile(500, 350));
  assert(!validManualBatteryProfile(0, 100));
  assert(!validManualBatteryProfile(500, 90));
  assert(!validManualBatteryProfile(500, 105));
  assert(!validManualBatteryProfile(500, 360));

  const ManualBatteryProfile manual{500, 250, true};
  assert(manualBatteryProfileIsUsable(manual));
}

void testBatteryPixelUsesSocAndChargingStateWithoutFakingSamples() {
  using namespace nhos;
  const BatteryLedColor off = batteryLedColor(false, 9000, false, 0);
  assert(off.r == 0 && off.g == 0 && off.b == 0);

  const BatteryLedColor green = batteryLedColor(true, 5001, false, 0);
  assert(green.g > green.r && green.g > green.b);
  const BatteryLedColor amber = batteryLedColor(true, 1500, false, 0);
  assert(amber.r > amber.g && amber.g > amber.b);
  const BatteryLedColor lowOn = batteryLedColor(true, 900, false, 0);
  const BatteryLedColor lowOff = batteryLedColor(true, 900, false, 500);
  assert(lowOn.r > 0 && lowOn.g == 0 && lowOn.b == 0);
  assert(lowOff.r == 0 && lowOff.g == 0 && lowOff.b == 0);
  // Charging takes the breathing path even for a red critical state, so the
  // pack never appears absent merely because the low-battery blink is off.
  const BatteryLedColor chargingCritical = batteryLedColor(true, 900, true, 500);
  assert(chargingCritical.r > 0 && chargingCritical.g == 0 &&
         chargingCritical.b == 0);

  const BatteryLedColor chargingLow = batteryLedColor(true, 7000, true, 0);
  const BatteryLedColor chargingHigh = batteryLedColor(true, 7000, true, 800);
  assert(chargingLow.g < chargingHigh.g);
}

}  // namespace

int main() {
  testValidOptionalSamplesProduceV5Blocks();
  testMissingOptionalSamplesOmitTheirV5Blocks();
  testExtensionsAreBoundedAndMalformedTlvIsRejected();
  testManualBatteryProfileOnlyAcceptsSafeHardwareSteps();
  testBatteryPixelUsesSocAndChargingStateWithoutFakingSamples();
  std::cout << "v5 runtime tests passed\n";
  return 0;
}
