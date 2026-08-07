#include "EspNowFrame.h"

#include <cstring>

namespace nhos {

namespace {

void putU16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xff);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

uint16_t getU16(const uint8_t* in) {
  return static_cast<uint16_t>(in[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(in[1]) << 8);
}

}  // namespace

uint8_t EspNowFragmenter::fragment(const uint8_t* data, size_t len,
                                    uint16_t frameId, uint8_t frameType,
                                    EspNowFragment* out, uint8_t outCapacity) {
  if (data == nullptr || len == 0 || len > 0xFFFFu || out == nullptr ||
      outCapacity == 0) {
    return 0;
  }

  const size_t fragCountSz =
      (len + kEspNowFragMaxPayload - 1) / kEspNowFragMaxPayload;
  if (fragCountSz > kEspNowMaxFragCount || fragCountSz > outCapacity) {
    return 0;
  }

  const uint8_t fragCount = static_cast<uint8_t>(fragCountSz);
  const uint16_t totalLen = static_cast<uint16_t>(len);

  size_t offset = 0;
  for (uint8_t i = 0; i < fragCount; ++i) {
    const size_t payloadLen = (offset + kEspNowFragMaxPayload <= len)
                                   ? kEspNowFragMaxPayload
                                   : (len - offset);
    EspNowFragment& frag = out[i];
    frag.bytes[0] = kEspNowFragMagic;
    frag.bytes[1] = kEspNowFragVersion;
    frag.bytes[2] = frameType;
    putU16(frag.bytes + 3, frameId);
    frag.bytes[5] = i;
    frag.bytes[6] = fragCount;
    putU16(frag.bytes + 7, totalLen);
    frag.bytes[9] = static_cast<uint8_t>(payloadLen);
    memcpy(frag.bytes + kEspNowFragHeaderLen, data + offset, payloadLen);
    frag.len = kEspNowFragHeaderLen + payloadLen;
    offset += payloadLen;
  }
  return fragCount;
}

EspNowReassembler::EspNowReassembler(uint8_t* scratch, size_t scratchCapacity)
    : scratch_(scratch), scratchCapacity_(scratchCapacity) {}

void EspNowReassembler::reset() {
  active_ = false;
  currentFrameId_ = 0;
  currentFrameType_ = 0;
  totalLen_ = 0;
  fragCount_ = 0;
  receivedMask_ = 0;
}

bool EspNowReassembler::onFragment(const uint8_t* data, size_t len,
                                    ReassembledFrame* out) {
  if (data == nullptr || out == nullptr || len < kEspNowFragHeaderLen) {
    return false;
  }
  if (data[0] != kEspNowFragMagic || data[1] != kEspNowFragVersion) {
    return false;
  }

  const uint8_t frameType = data[2];
  const uint16_t frameId = getU16(data + 3);
  const uint8_t fragIndex = data[5];
  const uint8_t fragCount = data[6];
  const uint16_t totalLen = getU16(data + 7);
  const uint8_t payloadLen = data[9];

  if (fragCount == 0 || fragCount > kEspNowMaxFragCount ||
      fragIndex >= fragCount) {
    return false;
  }
  if (static_cast<size_t>(payloadLen) != len - kEspNowFragHeaderLen) {
    return false;
  }
  if (totalLen == 0 || totalLen > scratchCapacity_) {
    return false;
  }

  const size_t offset =
      static_cast<size_t>(fragIndex) * kEspNowFragMaxPayload;
  const size_t expectedPayloadLen =
      (offset + kEspNowFragMaxPayload <= totalLen)
          ? kEspNowFragMaxPayload
          : (totalLen - offset);
  if (offset >= totalLen || payloadLen != expectedPayloadLen) {
    return false;
  }

  if (fragIndex == 0) {
    // A frame's first fragment always (re)starts reassembly, discarding any
    // prior in-flight frame. Data frames are lossy-by-design (see header
    // comment) so an abandoned partial frame is the expected outcome of a
    // dropped fragment, not an error to recover from.
    active_ = true;
    currentFrameId_ = frameId;
    currentFrameType_ = frameType;
    totalLen_ = totalLen;
    fragCount_ = fragCount;
    receivedMask_ = 0;
  } else if (!active_ || frameId != currentFrameId_ ||
             fragCount != fragCount_ || totalLen != totalLen_) {
    // Fragment belongs to a frame whose start we never saw, or no longer
    // matches the frame currently being reassembled -- drop it.
    return false;
  }

  memcpy(scratch_ + offset, data + kEspNowFragHeaderLen, payloadLen);
  receivedMask_ |= (1u << fragIndex);

  // `1u << 32` is undefined behaviour (shift >= width of uint32_t) -- on
  // ESP32 it wraps to `1u << 0`, making fullMask 0, which would make the
  // check below pass on the very first fragment and hand up a
  // half-filled buffer. kEspNowMaxFragCount is exactly 32, so this case
  // is reachable and must be special-cased rather than assumed away.
  const uint32_t fullMask =
      (fragCount_ >= 32) ? 0xFFFFFFFFu : ((1u << fragCount_) - 1u);
  if ((receivedMask_ & fullMask) != fullMask) {
    return false;
  }

  out->data = scratch_;
  out->len = totalLen_;
  out->frameId = currentFrameId_;
  out->frameType = currentFrameType_;
  active_ = false;
  return true;
}

}  // namespace nhos
