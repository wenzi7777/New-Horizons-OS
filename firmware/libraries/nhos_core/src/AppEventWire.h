#pragma once

#include <stddef.h>
#include <stdint.h>

#include "PacketWireConstants.h"

namespace nhos {

// One app event, as it rides a stream packet's extensions block
// (spec/PROTOCOL.md 3.1, TLV type kPacketExtAppEvent). Value layout,
// little-endian: seq u32, frame_seq u32, uptime_ms u32, flags u8,
// [value f32 when flags & kAppEventFlagValue], then app, event, detail each
// as a u8 length and that many bytes (no terminator), cut to
// kAppEventMaxApp / kAppEventMaxEvent / kAppEventMaxDetail.
struct AppEventFields {
  uint32_t seq = 0;
  uint32_t frameSeq = 0;
  uint32_t uptimeMs = 0;
  bool hasValue = false;
  float value = 0;
  const char* app = "";
  const char* event = "";
  const char* detail = "";
};

// Bytes encodeAppEventTlv() writes for these fields, TLV header included.
size_t appEventTlvBytes(const AppEventFields& fields);

// Writes one TLV at out; returns its length, or 0 (nothing written) when it
// does not fit in capacity.
size_t encodeAppEventTlv(const AppEventFields& fields, uint8_t* out, size_t capacity);

}  // namespace nhos
