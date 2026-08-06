#pragma once

// Pure chunk-header parsing + idempotency logic for EspNowOtaReceiver,
// split into its own framework-free (no Arduino.h/String) header so it can
// be host-unit-tested via tests_native/ -- see
// tests_native/test_ota_chunk_codec.cpp. EspNowOtaReceiver.cpp itself pulls
// in Update.h/mbedtls/Arduino String and can't be built for the host, the
// same reason EspNowFrame.h/.cpp is split out on its own (see that file's
// header comment).

#include <cstddef>
#include <cstdint>

namespace nhos {

// 4-byte sub-header (chunk_index u16 LE + chunk_len u16 LE) EspNowOtaRelay.h
// (Hub side, New-Horizons-Hub repo) prepends before each chunk's payload,
// ahead of handing the record to EspNowFragmenter -- EspNowFrame's own
// fragmentation only orders fragments *within* one reassembled frame, not
// across the ~365 separate frames making up a full firmware image.
constexpr size_t kOtaChunkSubHeaderLen = 4;

struct OtaChunkHeader {
  uint16_t chunkIndex = 0;
  uint16_t chunkLen = 0;
};

// Parses the sub-header out of a reassembled kEspNowFragTypeOta frame's
// bytes. Returns false (frame must be dropped, not partially trusted) if
// `len` is too short to hold the sub-header, or if the declared chunk_len
// doesn't exactly account for the remaining bytes in `len` -- either
// indicates a malformed/truncated frame.
bool parseOtaChunkHeader(const uint8_t* data, size_t len, OtaChunkHeader* out);

enum class OtaChunkDecision {
  kWrite,             // new chunk, in order -- call Update.write() then ack
  kDuplicateAckOnly,  // already-written chunk, our ack was lost -- re-ack, don't rewrite
  kIgnore,            // out of order/stale -- drop silently, Hub's own resend/timeout recovers
};

// Idempotency/ordering decision for a chunk given what's already been
// written. `hasWrittenAnyChunk` disambiguates chunkIndex==0 from "nothing
// written yet" (nextExpectedChunk==0 in both cases) -- without it, chunk 0
// would look like a duplicate of "chunk -1".
OtaChunkDecision classifyOtaChunk(uint16_t chunkIndex, uint16_t nextExpectedChunk,
                                   bool hasWrittenAnyChunk);

}  // namespace nhos
