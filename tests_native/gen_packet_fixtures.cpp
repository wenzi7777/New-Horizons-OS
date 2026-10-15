// Generates spec/fixtures/packets.json: NHO/Arduino/1 v5 stream packets built
// by the firmware's own encoder (PacketV5Codec.cpp), each with the values it
// was built from. The Desktop backend and the Gateway parse these bytes in
// their tests, so all three agree on the wire format without anyone
// re-deriving it by hand.
//
// run.sh rebuilds this and fails if the output differs from the committed
// file. After an intended wire change, regenerate with:
//   tests_native/.build/gen_packet_fixtures > spec/fixtures/packets.json
#include <cstdio>
#include <string>
#include <vector>

#include "AppEventWire.h"
#include "PacketV5Codec.h"
#include "PacketWire.h"

namespace {

const uint8_t kUid[6] = {0x3C, 0xDC, 0x75, 0x45, 0xCC, 0xD0};

struct Case {
  const char* name;
  uint32_t sequence;
  uint64_t epochMs;
  bool epochValid;
  std::vector<float> matrix;
  std::vector<float> raw;  // empty = no raw block
  std::vector<float> imu;  // empty = no IMU block (else 7 floats)
  std::vector<float> mag;  // empty = no MAG block (else 3 floats)
  bool hasBattery;
  nhos::PacketBatteryBlock battery;
  std::vector<nhos::AppEventFields> events;  // app_event extension TLVs
};

std::string jsonString(const char* text) {
  std::string out = "\"";
  for (const char* c = text; *c; ++c) {
    if (*c == '"' || *c == '\\') out += '\\';
    out += *c;
  }
  return out + "\"";
}

std::string floats(const std::vector<float>& values) {
  std::string out = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    char buf[32];
    // Every fixture value is exactly representable, so %.9g round-trips.
    std::snprintf(buf, sizeof(buf), "%s%.9g", i ? ", " : "", static_cast<double>(values[i]));
    out += buf;
  }
  return out + "]";
}

std::string orNull(const std::vector<float>& values) { return values.empty() ? "null" : floats(values); }

}  // namespace

int main() {
  const std::vector<Case> cases = {
      {"matrix_only", 1, 1700000000123ULL, true, {0.0f, 0.5f, 1.25f, 1000.0f}, {}, {}, {}, false, {}, {}},
      {"epoch_not_valid", 2, 1700000000123ULL, false, {2.5f, 3.75f}, {}, {}, {}, false, {}, {}},
      {"imu", 3, 1700000000456ULL, true, {1.0f, 2.0f, 3.0f},
       {}, {0.5f, -0.25f, 9.75f, 0.125f, -0.0625f, 1.5f, 36.5f}, {}, false, {}, {}},
      {"imu_mag_battery", 4, 1700000000789ULL, true, {10.5f, 20.25f},
       {}, {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 25.0f}, {7.0f, 8.0f, -9.5f},
       true, {1, 2, 4175, 7350}, {}},
      {"raw_adc", 5, 1700000001000ULL, true, {0.5f, 1.5f, 2.5f}, {512.0f, 1024.0f, 2047.5f}, {}, {}, false, {}, {}},
      {"everything", 4294967295u, 1700000009999ULL, true, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f},
       {100.0f, 200.0f, 300.0f, 400.0f, 500.0f, 600.0f},
       {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 30.0f}, {-1.5f, 2.25f, 40.0f},
       true, {0, 0, 3700, 10000}, {}},
      // Events ride the packet after their frame's, so their frame_seq is
      // older than the packet's sequence.
      {"app_events", 2690, 1792056866178ULL, true, {1.0f, 2.0f},
       {}, {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 25.0f}, {}, true, {0, 0, 3900, 8000},
       {{3, 2689, 54802, false, 0.0f, "flow1", "slouch", "rise"},
        {4, 2689, 54803, true, 0.75f, "balance", "lean", "0.75"}}},
  };

  std::printf("{\n  \"description\": \"NHO/Arduino/1 v5 stream packets from the firmware encoder "
              "(tests_native/gen_packet_fixtures.cpp). Regenerate, never hand-edit.\",\n");
  std::printf("  \"packet_version\": %u,\n  \"header_len\": %zu,\n  \"packets\": [\n",
              static_cast<unsigned>(nhos::kPacketVersion), nhos::kPacketHeaderLen);
  for (size_t c = 0; c < cases.size(); ++c) {
    const Case& k = cases[c];
    nhos::PacketV5BuildInput input;
    input.deviceUid = kUid;
    input.sequence = k.sequence;
    input.epochMs = k.epochMs;
    input.epochValid = k.epochValid;
    input.matrixValues = k.matrix.data();
    input.pointCount = static_cast<uint16_t>(k.matrix.size());
    input.hasRaw = !k.raw.empty();
    input.rawValues = k.raw.empty() ? nullptr : k.raw.data();
    input.imuData = k.imu.empty() ? nullptr : k.imu.data();
    input.magData = k.mag.empty() ? nullptr : k.mag.data();
    input.battery = k.hasBattery ? &k.battery : nullptr;
    uint8_t extensions[nhos::kPacketExtensionBudget];
    size_t extensionBytes = 0;
    for (const nhos::AppEventFields& event : k.events) {
      const size_t written = nhos::encodeAppEventTlv(event, extensions + extensionBytes,
                                                     sizeof(extensions) - extensionBytes);
      if (written == 0) {
        std::fprintf(stderr, "encodeAppEventTlv overflowed for %s\n", k.name);
        return 1;
      }
      extensionBytes += written;
    }
    input.extensions = extensionBytes ? extensions : nullptr;
    input.extensionBytes = extensionBytes;

    uint8_t buffer[1024];
    const size_t length = nhos::buildPacketV5(input, buffer, sizeof(buffer));
    if (length == 0) {
      std::fprintf(stderr, "buildPacketV5 failed for %s\n", k.name);
      return 1;
    }
    std::string hex;
    for (size_t i = 0; i < length; ++i) {
      char byte[3];
      std::snprintf(byte, sizeof(byte), "%02x", buffer[i]);
      hex += byte;
    }
    std::printf("    {\n      \"name\": \"%s\",\n      \"hex\": \"%s\",\n", k.name, hex.c_str());
    std::printf("      \"expect\": {\n");
    std::printf("        \"flags\": %u,\n", static_cast<unsigned>(buffer[3]));
    std::printf("        \"device_uid\": \"3CDC7545CCD0\",\n");
    std::printf("        \"sequence\": %u,\n", static_cast<unsigned>(k.sequence));
    std::printf("        \"epoch_valid\": %s,\n", k.epochValid ? "true" : "false");
    std::printf("        \"epoch_ms\": %llu,\n",
                static_cast<unsigned long long>(k.epochValid ? k.epochMs : 0));
    std::printf("        \"point_count\": %zu,\n", k.matrix.size());
    std::printf("        \"matrix\": %s,\n", floats(k.matrix).c_str());
    std::printf("        \"raw\": %s,\n", orNull(k.raw).c_str());
    std::printf("        \"imu\": %s,\n", orNull(k.imu).c_str());
    std::printf("        \"mag\": %s,\n", orNull(k.mag).c_str());
    std::printf("        \"app_events\": [");
    for (size_t e = 0; e < k.events.size(); ++e) {
      const nhos::AppEventFields& event = k.events[e];
      char value[32];
      std::snprintf(value, sizeof(value), "%.9g", static_cast<double>(event.value));
      std::printf("%s{\"seq\": %u, \"frame_seq\": %u, \"ms\": %u, \"app\": %s, \"event\": %s, "
                  "\"detail\": %s, \"value\": %s}",
                  e ? ", " : "", static_cast<unsigned>(event.seq), static_cast<unsigned>(event.frameSeq),
                  static_cast<unsigned>(event.uptimeMs), jsonString(event.app).c_str(),
                  jsonString(event.event).c_str(), jsonString(event.detail).c_str(),
                  event.hasValue ? value : "null");
    }
    std::printf("],\n");
    if (k.hasBattery) {
      std::printf("        \"battery\": {\"status\": %u, \"fault\": %u, \"vbat_mv\": %u, "
                  "\"soc_centi_percent\": %u}\n",
                  static_cast<unsigned>(k.battery.status), static_cast<unsigned>(k.battery.fault),
                  static_cast<unsigned>(k.battery.vbatMv),
                  static_cast<unsigned>(k.battery.socCentiPercent));
    } else {
      std::printf("        \"battery\": null\n");
    }
    std::printf("      }\n    }%s\n", c + 1 < cases.size() ? "," : "");
  }
  std::printf("  ]\n}\n");
  return 0;
}
