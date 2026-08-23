#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "BatteryLedPolicy.h"
#include "BatteryManualProfile.h"
#include "BatteryStatusJsonMerge.h"
#include "PacketV5Codec.h"

namespace {

uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

size_t countJsonKey(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t count = 0;
  size_t offset = 0;
  while ((offset = json.find(needle, offset)) != std::string::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
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

void testBatteryProfileCommandCapabilityGatesNvsEligiblePath() {
  using namespace nhos;
  assert(!batteryProfileCommandSupported(false));
  assert(batteryProfileCommandSupported(true));
}

void testBatteryStatusMergePreservesDistinctProfileKeys() {
  using namespace nhos;
  const std::string charger =
      "{\"profile\":\"balanced\",\"charge_profile\":\"balanced\","
      "\"temperature_monitoring\":\"bypassed\"}";
  const std::string gauge =
      "{\"battery_profile\":\"manual\",\"profile_source\":\"manual\","
      "\"profile_resolved\":true}";
  const std::string merged = mergeBatteryStatusJson(charger, gauge);

  assert(merged ==
         "{\"profile\":\"balanced\",\"charge_profile\":\"balanced\","
         "\"temperature_monitoring\":\"bypassed\","
         "\"battery_profile\":\"manual\",\"profile_source\":\"manual\","
         "\"profile_resolved\":true}");
  assert(countJsonKey(merged, "profile") == 1);
  assert(countJsonKey(merged, "battery_profile") == 1);
  assert(mergeBatteryStatusJson(charger, "not-an-object") == charger);
}

void testBatteryPixelUsesSocAndChargingStateWithoutFakingSamples() {
  using namespace nhos;
  const BatteryLedColor off = batteryLedColor(false, 9000, false, 0);
  assert(off.r == 0 && off.g == 0 && off.b == 0);

  // Discharging is always visible and moves continuously from orange (0%) to
  // green (100%); there are no threshold jumps or a distracting low-battery
  // blink on the dedicated battery pixel.
  const BatteryLedColor full = batteryLedColor(true, 10000, false, 0);
  assert(full.r == 0 && full.g == 80 && full.b == 0);
  const BatteryLedColor empty = batteryLedColor(true, 0, false, 0);
  assert(empty.r == 128 && empty.g == 48 && empty.b == 0);
  const BatteryLedColor half = batteryLedColor(true, 5000, false, 0);
  assert(half.r == 64 && half.g == 64 && half.b == 0);
  const BatteryLedColor critical = batteryLedColor(true, 900, false, 500);
  assert(critical.r > 0 && critical.g > 0 && critical.b == 0);

  // Charging flashes the exact same SoC hue rather than changing it to a
  // generic charging colour. The off phase only communicates charge activity.
  const BatteryLedColor chargingOn = batteryLedColor(true, 7000, true, 0);
  const BatteryLedColor chargingOff = batteryLedColor(true, 7000, true, 750);
  const BatteryLedColor chargingNextOn = batteryLedColor(true, 7000, true, 1200);
  assert(chargingOn.r == chargingNextOn.r && chargingOn.g == chargingNextOn.g &&
         chargingOn.b == chargingNextOn.b);
  assert(chargingOff.r == 0 && chargingOff.g == 0 && chargingOff.b == 0);
}

}  // namespace

int main() {
  testValidOptionalSamplesProduceV5Blocks();
  testMissingOptionalSamplesOmitTheirV5Blocks();
  testExtensionsAreBoundedAndMalformedTlvIsRejected();
  testManualBatteryProfileOnlyAcceptsSafeHardwareSteps();
  testBatteryProfileCommandCapabilityGatesNvsEligiblePath();
  testBatteryStatusMergePreservesDistinctProfileKeys();
  testBatteryPixelUsesSocAndChargingStateWithoutFakingSamples();
  std::cout << "v5 runtime tests passed\n";
  return 0;
}
