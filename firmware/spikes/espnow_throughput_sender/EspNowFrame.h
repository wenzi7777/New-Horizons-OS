#pragma once

// Fragmentation/reassembly layer for carrying opaque NHO/Arduino/1 byte
// blobs (built by PacketBuilder, up to kMaxPacketBytes ~1884B) over
// ESP-NOW's 250-byte-per-packet limit. This module knows nothing about the
// NHO/Arduino/1 wire format itself -- it only splits/rejoins byte buffers,
// so PacketBuilder/Config.h stay untouched.
//
// Deliberately framework-free (no Arduino.h/String) so it can be unit
// tested on the host without a device -- see
// NewHorizonsOS-OTA/tests_native/test_esp_now_frame.cpp.

#include <cstddef>
#include <cstdint>

namespace nhos {

// ESP-NOW's per-packet payload cap (esp_now_send() rejects longer buffers).
static constexpr size_t kEspNowMaxPacketBytes = 250;

static constexpr uint8_t kEspNowFragMagic = 0xE5;
static constexpr uint8_t kEspNowFragVersion = 1;

// magic(1) + version(1) + frameType(1) + frameId(2) + fragIndex(1) +
// fragCount(1) + totalLen(2) + payloadLen(1)
static constexpr size_t kEspNowFragHeaderLen = 10;
static constexpr size_t kEspNowFragMaxPayload =
    kEspNowMaxPacketBytes - kEspNowFragHeaderLen;

// Sensor/heartbeat data: fire-and-forget, no retry -- a stale in-flight
// frame is simply abandoned once a newer one starts (matches the existing
// UDP streaming philosophy; retrying data that's about to be superseded by
// the next ~60fps frame just adds latency).
static constexpr uint8_t kEspNowFragTypeData = 0x00;
// Pairing/command/ack traffic: the caller (EspNowPairing /
// EspNowCommandDispatcher) is expected to layer its own ack + retry on top
// of this envelope; the frame layer only tags the type so those messages
// can be told apart from data frames.
static constexpr uint8_t kEspNowFragTypeControl = 0x01;

// Upper bound on fragments per frame. 16 * kEspNowFragMaxPayload (240) =
// 3840B, well above the current worst-case ~1884B frame, leaving headroom
// for larger boards without changing the wire format.
static constexpr uint8_t kEspNowMaxFragCount = 16;

// One wire-ready ESP-NOW packet, sized to hand directly to esp_now_send().
struct EspNowFragment {
  uint8_t bytes[kEspNowMaxPacketBytes] = {0};
  size_t len = 0;
};

// Splits an opaque byte blob into <=kEspNowMaxPacketBytes ESP-NOW
// fragments. Stateless and allocation-free.
class EspNowFragmenter {
 public:
  // Fragments `data` (length `len`) into `out` (capacity `outCapacity`
  // entries). Returns the number of fragments written, or 0 if `len` is 0,
  // too large to express in a uint16_t totalLen, or would need more than
  // min(outCapacity, kEspNowMaxFragCount) fragments.
  static uint8_t fragment(const uint8_t* data, size_t len, uint16_t frameId,
                           uint8_t frameType, EspNowFragment* out,
                           uint8_t outCapacity);
};

struct ReassembledFrame {
  const uint8_t* data = nullptr;
  size_t len = 0;
  uint16_t frameId = 0;
  uint8_t frameType = 0;
};

// Reassembles fragments from a single peer. Callers that talk to multiple
// peers (e.g. the Hub, up to 4 paired devices) should hold one instance per
// peer so fragments never interleave across reassemblers.
class EspNowReassembler {
 public:
  // `scratch`/`scratchCapacity` must be sized to the largest frame this
  // reassembler will ever see (callers typically use kMaxPacketBytes from
  // Config.h). The reassembler does not own or allocate this buffer.
  EspNowReassembler(uint8_t* scratch, size_t scratchCapacity);

  // Feeds one received ESP-NOW packet (`data`/`len` as delivered by the
  // esp_now_recv callback). On completing a frame, fills `out` (whose
  // `data` pointer aliases the internal scratch buffer -- valid only until
  // the next onFragment()/reset() call) and returns true. Malformed
  // packets, and fragments that don't belong to the currently in-flight
  // frame, are silently dropped and this returns false.
  bool onFragment(const uint8_t* data, size_t len, ReassembledFrame* out);

  // Abandons any in-flight partial frame, e.g. after a peer timeout.
  void reset();

 private:
  uint8_t* scratch_;
  size_t scratchCapacity_;
  bool active_ = false;
  uint16_t currentFrameId_ = 0;
  uint8_t currentFrameType_ = 0;
  uint16_t totalLen_ = 0;
  uint8_t fragCount_ = 0;
  uint32_t receivedMask_ = 0;
};

}  // namespace nhos
