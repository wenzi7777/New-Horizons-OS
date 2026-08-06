// Host-native (no Arduino toolchain) unit tests for OtaChunkCodec. Build/run
// with tests_native/run.sh -- see that script for the exact g++ invocation.
//
// Deliberately dependency-free (no gtest), matching test_esp_now_frame.cpp's
// own style.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../firmware/newhorizons_os/OtaChunkCodec.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

std::vector<uint8_t> makeChunkRecord(uint16_t chunkIndex, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out(nhos::kOtaChunkSubHeaderLen + payload.size());
  out[0] = static_cast<uint8_t>(chunkIndex & 0xFF);
  out[1] = static_cast<uint8_t>((chunkIndex >> 8) & 0xFF);
  const uint16_t len = static_cast<uint16_t>(payload.size());
  out[2] = static_cast<uint8_t>(len & 0xFF);
  out[3] = static_cast<uint8_t>((len >> 8) & 0xFF);
  std::memcpy(out.data() + nhos::kOtaChunkSubHeaderLen, payload.data(), payload.size());
  return out;
}

void testParseValidHeader() {
  std::vector<uint8_t> payload(100, 0xAB);
  auto record = makeChunkRecord(42, payload);
  nhos::OtaChunkHeader header;
  CHECK(nhos::parseOtaChunkHeader(record.data(), record.size(), &header));
  CHECK(header.chunkIndex == 42);
  CHECK(header.chunkLen == 100);
}

void testParseChunkIndexHighByte() {
  // chunkIndex above 255 exercises the second (high) byte of the LE u16 --
  // a bug here would silently wrap/truncate the index.
  std::vector<uint8_t> payload(10, 0x01);
  auto record = makeChunkRecord(300, payload);
  nhos::OtaChunkHeader header;
  CHECK(nhos::parseOtaChunkHeader(record.data(), record.size(), &header));
  CHECK(header.chunkIndex == 300);
}

void testParseTooShortForSubHeaderRejected() {
  uint8_t tooShort[3] = {0, 0, 0};
  nhos::OtaChunkHeader header;
  CHECK(!nhos::parseOtaChunkHeader(tooShort, sizeof(tooShort), &header));
}

void testParseDeclaredLenMismatchRejected() {
  // Sub-header declares chunk_len=100 but only 10 bytes of payload actually
  // follow -- a truncated/malformed frame that must be dropped, not
  // partially trusted.
  std::vector<uint8_t> payload(10, 0xCD);
  auto record = makeChunkRecord(5, payload);
  record[2] = 100;  // lie about chunk_len
  record[3] = 0;
  nhos::OtaChunkHeader header;
  CHECK(!nhos::parseOtaChunkHeader(record.data(), record.size(), &header));
}

void testParseNullArgsRejected() {
  nhos::OtaChunkHeader header;
  CHECK(!nhos::parseOtaChunkHeader(nullptr, 10, &header));
  uint8_t buf[nhos::kOtaChunkSubHeaderLen] = {0};
  CHECK(!nhos::parseOtaChunkHeader(buf, sizeof(buf), nullptr));
}

void testClassifyInOrderChunkIsWritten() {
  // Nothing written yet -- chunk 0 arriving is the expected next write.
  CHECK(nhos::classifyOtaChunk(0, /*nextExpectedChunk=*/0, /*hasWrittenAnyChunk=*/false) ==
        nhos::OtaChunkDecision::kWrite);
  // After chunk 0 is written, chunk 1 is the expected next write.
  CHECK(nhos::classifyOtaChunk(1, /*nextExpectedChunk=*/1, /*hasWrittenAnyChunk=*/true) ==
        nhos::OtaChunkDecision::kWrite);
}

void testClassifyDuplicateChunkIsAckOnly() {
  // Chunk 4 was already written (nextExpectedChunk is now 5); the Hub
  // resent chunk 4 because our ack was lost. Must NOT be classified as
  // kWrite -- a second Update.write() for the same bytes would corrupt the
  // flash image.
  CHECK(nhos::classifyOtaChunk(4, /*nextExpectedChunk=*/5, /*hasWrittenAnyChunk=*/true) ==
        nhos::OtaChunkDecision::kDuplicateAckOnly);
}

void testClassifyChunkZeroNeverTreatedAsDuplicateOfNothing() {
  // Before anything has been written, nextExpectedChunk==0 too -- without
  // the hasWrittenAnyChunk guard, chunk 0 could be misread as a "duplicate
  // of chunk -1" (uint16_t underflow) instead of the legitimate first
  // write. This is the exact bug the hasWrittenAnyChunk parameter exists
  // to prevent.
  CHECK(nhos::classifyOtaChunk(0, /*nextExpectedChunk=*/0, /*hasWrittenAnyChunk=*/false) ==
        nhos::OtaChunkDecision::kWrite);
}

void testClassifyOutOfOrderChunkIsIgnored() {
  // Expecting chunk 5, but chunk 7 arrives (e.g. Hub-side session reset
  // mid-transfer, or a stray fragment from an earlier attempt) -- must be
  // dropped, not written out of position.
  CHECK(nhos::classifyOtaChunk(7, /*nextExpectedChunk=*/5, /*hasWrittenAnyChunk=*/true) ==
        nhos::OtaChunkDecision::kIgnore);
  // A chunk further behind than the duplicate-of-last-written case (stale
  // retransmit from further back than one chunk ago) is also ignored, not
  // treated as a duplicate-ack case.
  CHECK(nhos::classifyOtaChunk(2, /*nextExpectedChunk=*/5, /*hasWrittenAnyChunk=*/true) ==
        nhos::OtaChunkDecision::kIgnore);
}

}  // namespace

int main() {
  testParseValidHeader();
  testParseChunkIndexHighByte();
  testParseTooShortForSubHeaderRejected();
  testParseDeclaredLenMismatchRejected();
  testParseNullArgsRejected();
  testClassifyInOrderChunkIsWritten();
  testClassifyDuplicateChunkIsAckOnly();
  testClassifyChunkZeroNeverTreatedAsDuplicateOfNothing();
  testClassifyOutOfOrderChunkIsIgnored();

  if (g_failures == 0) {
    std::printf("OK: all OtaChunkCodec tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d OtaChunkCodec test(s) failed\n", g_failures);
  return 1;
}
