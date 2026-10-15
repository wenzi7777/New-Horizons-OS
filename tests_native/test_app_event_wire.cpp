// The app_event extension TLV (AppEventWire) and a packet that carries it
// (PacketV5Codec): layout, the string limits and the extension budget.
#include <cstdio>
#include <cstring>

#include "AppEventWire.h"
#include "PacketV5Codec.h"

namespace {
int failures = 0;
void expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

uint32_t u32(const uint8_t* at) {
  return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
         (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}
}  // namespace

int main() {
  // Layout of a plain event.
  nhos::AppEventFields event;
  event.seq = 7;
  event.frameSeq = 2689;
  event.uptimeMs = 54802;
  event.app = "flow";
  event.event = "north";
  event.detail = "rise";
  uint8_t out[160] = {0};
  const size_t length = nhos::encodeAppEventTlv(event, out, sizeof(out));
  expect(length == nhos::appEventTlvBytes(event), "encoded length matches appEventTlvBytes");
  expect(length == 2 + 13 + 3 + 4 + 5 + 4, "seq/frame/uptime/flags + three strings");
  expect(out[0] == nhos::kPacketExtAppEvent && out[1] == length - 2, "TLV type and value length");
  expect(u32(out + 2) == 7 && u32(out + 6) == 2689 && u32(out + 10) == 54802, "little-endian u32s");
  expect(out[14] == 0, "no value flag");
  expect(out[15] == 4 && std::memcmp(out + 16, "flow", 4) == 0, "app as length + bytes");
  expect(out[20] == 5 && std::memcmp(out + 21, "north", 5) == 0, "event as length + bytes");
  expect(out[26] == 4 && std::memcmp(out + 27, "rise", 4) == 0, "detail as length + bytes");

  // A value adds its float right after the flags.
  event.hasValue = true;
  event.value = 0.75f;
  const size_t valued = nhos::encodeAppEventTlv(event, out, sizeof(out));
  float value = 0;
  std::memcpy(&value, out + 15, sizeof(value));
  expect(valued == length + 4 && out[14] == nhos::kAppEventFlagValue && value == 0.75f, "value follows flags");

  // Strings are cut to the record's limits, so a TLV never exceeds 107 bytes.
  const char* longText = "0123456789012345678901234567890123456789012345678901234567890123456789";
  nhos::AppEventFields longest;
  longest.hasValue = true;
  longest.app = longText;
  longest.event = longText;
  longest.detail = longText;
  expect(nhos::appEventTlvBytes(longest) == 2 + 13 + 4 + 3 + 15 + 23 + 47, "strings cut to 15/23/47");
  expect(nhos::appEventTlvBytes(longest) <= nhos::kPacketExtensionBudget, "one event always fits the budget");

  // Not enough room writes nothing.
  expect(nhos::encodeAppEventTlv(event, out, valued - 1) == 0, "too small a buffer writes nothing");

  // A packet carrying it: flag set, TLVs after the battery block, length counted.
  const uint8_t uid[6] = {1, 2, 3, 4, 5, 6};
  const float matrix[2] = {1.0f, 2.0f};
  nhos::PacketBatteryBlock battery;
  nhos::PacketV5BuildInput input;
  input.deviceUid = uid;
  input.sequence = 2690;
  input.matrixValues = matrix;
  input.pointCount = 2;
  input.battery = &battery;
  input.extensions = out;
  input.extensionBytes = valued;
  uint8_t packet[512];
  const size_t packetLength = nhos::buildPacketV5(input, packet, sizeof(packet));
  expect(packetLength == nhos::kPacketHeaderLen + 8 + nhos::kPacketBatteryBytes + valued, "packet length counts the TLVs");
  expect((packet[3] & nhos::kPacketFlagExtensions) != 0, "extensions flag set");
  expect(std::memcmp(packet + nhos::kPacketHeaderLen + 8 + nhos::kPacketBatteryBytes, out, valued) == 0,
         "TLVs follow the battery block");

  // Over budget or malformed: the build refuses rather than send a bad frame.
  uint8_t big[nhos::kPacketExtensionBudget + 2] = {0};
  big[0] = nhos::kPacketExtAppEvent;
  big[1] = static_cast<uint8_t>(sizeof(big) - 2);
  input.extensions = big;
  input.extensionBytes = sizeof(big);
  expect(nhos::buildPacketV5(input, packet, sizeof(packet)) == 0, "over-budget extensions are refused");
  uint8_t malformed[3] = {nhos::kPacketExtAppEvent, 9, 0};
  input.extensions = malformed;
  input.extensionBytes = sizeof(malformed);
  expect(nhos::buildPacketV5(input, packet, sizeof(packet)) == 0, "malformed TLVs are refused");

  // Without extensions nothing changes.
  input.extensions = nullptr;
  input.extensionBytes = 0;
  expect(nhos::buildPacketV5(input, packet, sizeof(packet)) == nhos::kPacketHeaderLen + 8 + nhos::kPacketBatteryBytes &&
             (packet[3] & nhos::kPacketFlagExtensions) == 0,
         "no extensions, no flag, same length as before");

  if (failures) {
    std::fprintf(stderr, "test_app_event_wire: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_app_event_wire: ok\n");
  return 0;
}
