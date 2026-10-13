#pragma once

// ESP-NOW time service: a device asks its Hub for the time with a classic
// four-timestamp (NTP-style) exchange, so the radio round trip is measured
// and cancelled instead of silently added to the device's clock.
//
//   device                         Hub
//   t1 (mono) ── TIME_REQ ──────▶  t2 (epoch, on receive)
//   t4 (mono) ◀── TIME_RESP ─────  t3 (epoch, just before send)
//
//   offset = ((t2 - t1) + (t3 - t4)) / 2     (epoch - device mono)
//   delay  = (t4 - t1) - (t3 - t2)           (time actually on the air)
//
// Why its own message instead of the relayed `set_time` command: a relayed
// command crosses the Backend WebSocket, the Hub and an ESP-NOW control
// exchange with no way to measure any of it, and every pending command
// pauses the Hub's sensor relay for all devices.
//
// The same exchange also runs over UDP between a WiFi-mode device and its
// Gateway (New-Horizons-Gateway's time_service.py); the bytes are identical,
// only the transport differs.
//
// Compatibility, by construction:
//   * Only the device initiates. An old device never sends TIME_REQ, so it
//     never sees TIME_RESP.
//   * An old Hub routes a TIME_REQ into its fragment reassemblers, which
//     drop it (first byte is not kEspNowFragMagic); the device notices the
//     silence and backs off.
//   * An old Gateway sees a datagram that is neither JSON nor an NHO
//     packet (no 0xA55A magic, so no device_uid) and neither answers nor
//     forwards it; the device again backs off.
//   * HELLO/PAIRED/POLL stay exactly one byte: devices match them with
//     `len == 1`, so lengthening them would break pairing on old firmware.
//
// Framework-free; shared byte-identical with New-Horizons-Hub. Unit tested
// in tests_native/test_espnow_time_wire.cpp.

#include <cstddef>
#include <cstdint>

namespace nhos {

// Must not collide with HELLO 0xE1 / POLL 0xE2 / PAIRED 0xE3 / fragment
// 0xE5 / OTA chunk ack 0xE6 / control ack 0xE7.
static constexpr uint8_t kEspNowTimeReqMagic = 0xE8;
static constexpr uint8_t kEspNowTimeRespMagic = 0xE9;
static constexpr uint8_t kEspNowTimeWireVersion = 1;

// magic(1) version(1) seq(1) flags(1) t1(8)
static constexpr size_t kEspNowTimeReqLen = 12;
// magic(1) version(1) seq(1) flags(1) t1(8) t2(8) t3(8) hubUncertaintyMs(2) reserved(2)
static constexpr size_t kEspNowTimeRespLen = 32;

// TIME_RESP flags: bit0 = Hub clock synced; high nibble = Hub TimeSource.
static constexpr uint8_t kEspNowTimeFlagHubSynced = 0x01;

struct EspNowTimeRequest {
  uint8_t seq = 0;
  int64_t t1MonoUs = 0;
};

struct EspNowTimeResponse {
  uint8_t seq = 0;
  bool hubSynced = false;
  uint8_t hubSource = 0;  // TimeSource value
  int64_t t1MonoUs = 0;   // echoed from the request
  int64_t t2EpochUs = 0;  // Hub wall time when the request arrived
  int64_t t3EpochUs = 0;  // Hub wall time just before the reply was sent
  uint16_t hubUncertaintyMs = 0;
};

struct EspNowTimeExchange {
  int64_t offsetUs = 0;  // epoch minus device monotonic time
  int64_t delayUs = 0;   // round trip minus the Hub's own turnaround
};

// Return the encoded length, or 0 if `capacity` is too small.
size_t encodeEspNowTimeRequest(const EspNowTimeRequest& req, uint8_t* out, size_t capacity);
size_t encodeEspNowTimeResponse(const EspNowTimeResponse& resp, uint8_t* out, size_t capacity);
// Exact length, magic and version are all checked.
bool decodeEspNowTimeRequest(const uint8_t* data, size_t len, EspNowTimeRequest* out);
bool decodeEspNowTimeResponse(const uint8_t* data, size_t len, EspNowTimeResponse* out);

EspNowTimeExchange computeEspNowTimeExchange(int64_t t1MonoUs, int64_t t2EpochUs,
                                             int64_t t3EpochUs, int64_t t4MonoUs);

}  // namespace nhos
