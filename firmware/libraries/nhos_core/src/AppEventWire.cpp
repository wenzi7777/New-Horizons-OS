#include "AppEventWire.h"

#include <string.h>

namespace nhos {
namespace {

constexpr size_t kFixedBytes = 4 + 4 + 4 + 1;  // seq, frame_seq, uptime_ms, flags

size_t boundedLength(const char* text, size_t limit) {
  if (text == nullptr) {
    return 0;
  }
  size_t length = 0;
  while (length < limit && text[length] != '\0') {
    ++length;
  }
  return length;
}

void putU32(uint8_t* out, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) {
    out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
  }
}

size_t putString(uint8_t* out, const char* text, size_t length) {
  out[0] = static_cast<uint8_t>(length);
  if (length) {
    memcpy(out + 1, text, length);
  }
  return 1 + length;
}

}  // namespace

size_t appEventTlvBytes(const AppEventFields& fields) {
  return kPacketExtensionTlvHeaderLen + kFixedBytes + (fields.hasValue ? sizeof(float) : 0) +
         3 + boundedLength(fields.app, kAppEventMaxApp) +
         boundedLength(fields.event, kAppEventMaxEvent) +
         boundedLength(fields.detail, kAppEventMaxDetail);
}

size_t encodeAppEventTlv(const AppEventFields& fields, uint8_t* out, size_t capacity) {
  const size_t total = appEventTlvBytes(fields);
  // The TLV length byte holds the value length, so it must stay under 256;
  // the limits keep it at most 105.
  if (out == nullptr || total > capacity || total - kPacketExtensionTlvHeaderLen > 0xff) {
    return 0;
  }
  size_t offset = 0;
  out[offset++] = kPacketExtAppEvent;
  out[offset++] = static_cast<uint8_t>(total - kPacketExtensionTlvHeaderLen);
  putU32(out + offset, fields.seq);
  offset += 4;
  putU32(out + offset, fields.frameSeq);
  offset += 4;
  putU32(out + offset, fields.uptimeMs);
  offset += 4;
  out[offset++] = fields.hasValue ? kAppEventFlagValue : 0;
  if (fields.hasValue) {
    memcpy(out + offset, &fields.value, sizeof(float));
    offset += sizeof(float);
  }
  offset += putString(out + offset, fields.app, boundedLength(fields.app, kAppEventMaxApp));
  offset += putString(out + offset, fields.event, boundedLength(fields.event, kAppEventMaxEvent));
  offset += putString(out + offset, fields.detail, boundedLength(fields.detail, kAppEventMaxDetail));
  return offset;
}

}  // namespace nhos
