#include "OtaChunkCodec.h"

namespace nhos {

bool parseOtaChunkHeader(const uint8_t* data, size_t len, OtaChunkHeader* out) {
  if (data == nullptr || out == nullptr || len < kOtaChunkSubHeaderLen) {
    return false;
  }
  const uint16_t chunkIndex =
      static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
  const uint16_t chunkLen =
      static_cast<uint16_t>(data[2]) | static_cast<uint16_t>(static_cast<uint16_t>(data[3]) << 8);
  if (kOtaChunkSubHeaderLen + chunkLen != len) {
    return false;
  }
  out->chunkIndex = chunkIndex;
  out->chunkLen = chunkLen;
  return true;
}

OtaChunkDecision classifyOtaChunk(uint16_t chunkIndex, uint16_t nextExpectedChunk,
                                   bool hasWrittenAnyChunk) {
  if (hasWrittenAnyChunk && chunkIndex == static_cast<uint16_t>(nextExpectedChunk - 1)) {
    return OtaChunkDecision::kDuplicateAckOnly;
  }
  if (chunkIndex == nextExpectedChunk) {
    return OtaChunkDecision::kWrite;
  }
  return OtaChunkDecision::kIgnore;
}

}  // namespace nhos
