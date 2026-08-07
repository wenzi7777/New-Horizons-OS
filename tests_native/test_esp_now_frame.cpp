// Host-native (no Arduino toolchain) unit tests for EspNowFrame. Build/run
// with tests_native/run.sh -- see that script for the exact g++ invocation.
//
// Deliberately dependency-free (no gtest) so it can compile with a plain
// host g++/clang++ the way this repo's other tooling scripts run.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../firmware/newhorizons_os/EspNowFrame.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

std::vector<uint8_t> makePayload(size_t len, uint8_t seed) {
  std::vector<uint8_t> data(len);
  for (size_t i = 0; i < len; ++i) {
    data[i] = static_cast<uint8_t>((seed + i) & 0xff);
  }
  return data;
}

void testRoundTripSingleFragment() {
  auto payload = makePayload(24, 1);  // fits in one fragment, like a heartbeat
  nhos::EspNowFragment frags[nhos::kEspNowMaxFragCount];
  uint8_t count = nhos::EspNowFragmenter::fragment(
      payload.data(), payload.size(), /*frameId=*/7, nhos::kEspNowFragTypeData,
      frags, nhos::kEspNowMaxFragCount);
  CHECK(count == 1);

  uint8_t scratch[nhos::kEspNowMaxFragCount * nhos::kEspNowFragMaxPayload];
  nhos::EspNowReassembler reassembler(scratch, sizeof(scratch));
  nhos::ReassembledFrame out;
  bool complete = reassembler.onFragment(frags[0].bytes, frags[0].len, &out);
  CHECK(complete);
  CHECK(out.len == payload.size());
  CHECK(out.frameId == 7);
  CHECK(out.frameType == nhos::kEspNowFragTypeData);
  CHECK(std::memcmp(out.data, payload.data(), payload.size()) == 0);
}

void testRoundTripWorstCaseFrame() {
  // ~1884B matches the largest current NHO/Arduino/1 frame (GCU V2.3.D,
  // 15x15 matrix, raw ADC + IMU + mag + battery + HMAC all enabled).
  auto payload = makePayload(1884, 42);
  nhos::EspNowFragment frags[nhos::kEspNowMaxFragCount];
  uint8_t count = nhos::EspNowFragmenter::fragment(
      payload.data(), payload.size(), /*frameId=*/1000,
      nhos::kEspNowFragTypeData, frags, nhos::kEspNowMaxFragCount);
  CHECK(count == 8);  // ceil(1884 / 240)

  uint8_t scratch[nhos::kEspNowMaxFragCount * nhos::kEspNowFragMaxPayload];
  nhos::EspNowReassembler reassembler(scratch, sizeof(scratch));
  nhos::ReassembledFrame out;
  bool complete = false;
  for (uint8_t i = 0; i < count; ++i) {
    complete = reassembler.onFragment(frags[i].bytes, frags[i].len, &out);
    if (i + 1 < count) {
      CHECK(!complete);
    }
  }
  CHECK(complete);
  CHECK(out.len == payload.size());
  CHECK(std::memcmp(out.data, payload.data(), payload.size()) == 0);
}

void testOutOfOrderReassembly() {
  auto payload = makePayload(700, 5);
  nhos::EspNowFragment frags[nhos::kEspNowMaxFragCount];
  uint8_t count = nhos::EspNowFragmenter::fragment(
      payload.data(), payload.size(), /*frameId=*/3, nhos::kEspNowFragTypeData,
      frags, nhos::kEspNowMaxFragCount);
  CHECK(count == 3);  // ceil(700/240)

  uint8_t scratch[nhos::kEspNowMaxFragCount * nhos::kEspNowFragMaxPayload];
  nhos::EspNowReassembler reassembler(scratch, sizeof(scratch));
  nhos::ReassembledFrame out;

  // Feed fragment 0 first (reassembly always needs the start), then the
  // rest out of order.
  CHECK(!reassembler.onFragment(frags[0].bytes, frags[0].len, &out));
  CHECK(!reassembler.onFragment(frags[2].bytes, frags[2].len, &out));
  bool complete = reassembler.onFragment(frags[1].bytes, frags[1].len, &out);
  CHECK(complete);
  CHECK(out.len == payload.size());
  CHECK(std::memcmp(out.data, payload.data(), payload.size()) == 0);
}

void testNewFrameAbandonsStalePartial() {
  auto first = makePayload(700, 1);
  auto second = makePayload(300, 2);
  nhos::EspNowFragment firstFrags[nhos::kEspNowMaxFragCount];
  nhos::EspNowFragment secondFrags[nhos::kEspNowMaxFragCount];
  uint8_t firstCount = nhos::EspNowFragmenter::fragment(
      first.data(), first.size(), /*frameId=*/10, nhos::kEspNowFragTypeData,
      firstFrags, nhos::kEspNowMaxFragCount);
  uint8_t secondCount = nhos::EspNowFragmenter::fragment(
      second.data(), second.size(), /*frameId=*/11, nhos::kEspNowFragTypeData,
      secondFrags, nhos::kEspNowMaxFragCount);
  CHECK(firstCount >= 2);
  CHECK(secondCount >= 1);

  uint8_t scratch[nhos::kEspNowMaxFragCount * nhos::kEspNowFragMaxPayload];
  nhos::EspNowReassembler reassembler(scratch, sizeof(scratch));
  nhos::ReassembledFrame out;

  // Start frame 10 but never finish it (simulates a dropped tail fragment).
  CHECK(!reassembler.onFragment(firstFrags[0].bytes, firstFrags[0].len, &out));

  // Frame 11 arrives next (frame 10 is abandoned, not corrupting frame 11).
  bool complete = false;
  for (uint8_t i = 0; i < secondCount; ++i) {
    complete = reassembler.onFragment(secondFrags[i].bytes, secondFrags[i].len, &out);
  }
  CHECK(complete);
  CHECK(out.frameId == 11);
  CHECK(out.len == second.size());
  CHECK(std::memcmp(out.data, second.data(), second.size()) == 0);

  // A stray leftover fragment from the abandoned frame 10 must not be
  // mistaken for part of frame 11.
  CHECK(!reassembler.onFragment(firstFrags[1].bytes, firstFrags[1].len, &out));
}

void testOversizedFrameRejected() {
  std::vector<uint8_t> huge(nhos::kEspNowMaxFragCount * nhos::kEspNowFragMaxPayload + 1, 0xAB);
  nhos::EspNowFragment frags[nhos::kEspNowMaxFragCount];
  uint8_t count = nhos::EspNowFragmenter::fragment(
      huge.data(), huge.size(), /*frameId=*/1, nhos::kEspNowFragTypeData,
      frags, nhos::kEspNowMaxFragCount);
  CHECK(count == 0);
}

void testMalformedFragmentDropped() {
  uint8_t scratch[nhos::kEspNowMaxFragCount * nhos::kEspNowFragMaxPayload];
  nhos::EspNowReassembler reassembler(scratch, sizeof(scratch));
  nhos::ReassembledFrame out;

  uint8_t tooShort[4] = {nhos::kEspNowFragMagic, nhos::kEspNowFragVersion, 0, 0};
  CHECK(!reassembler.onFragment(tooShort, sizeof(tooShort), &out));

  uint8_t badMagic[nhos::kEspNowFragHeaderLen] = {0};
  badMagic[0] = 0x00;  // wrong magic
  CHECK(!reassembler.onFragment(badMagic, sizeof(badMagic), &out));
}

void testControlFrameTypePreserved() {
  auto payload = makePayload(50, 9);
  nhos::EspNowFragment frags[nhos::kEspNowMaxFragCount];
  uint8_t count = nhos::EspNowFragmenter::fragment(
      payload.data(), payload.size(), /*frameId=*/99,
      nhos::kEspNowFragTypeControl, frags, nhos::kEspNowMaxFragCount);
  CHECK(count == 1);

  uint8_t scratch[nhos::kEspNowMaxFragCount * nhos::kEspNowFragMaxPayload];
  nhos::EspNowReassembler reassembler(scratch, sizeof(scratch));
  nhos::ReassembledFrame out;
  CHECK(reassembler.onFragment(frags[0].bytes, frags[0].len, &out));
  CHECK(out.frameType == nhos::kEspNowFragTypeControl);
}

// Regression: a frame using the *full* kEspNowMaxFragCount budget must
// round-trip. This is the boundary that silently broke ESP-NOW control
// responses: EspNowReassembler computes its completion mask as
// `(1u << fragCount_) - 1u`, and with kEspNowMaxFragCount == 32 that shift
// is undefined behaviour (shift >= width of uint32_t). On ESP32 it wraps
// to `1u << 0`, making fullMask 0, so the "have I got every fragment?"
// check passes on the very first fragment and hands up a mostly-empty
// buffer. Guards both the shift fix and the max-fragment size itself.
void testRoundTripMaxFragmentCountFrame() {
  const size_t fullSize = nhos::kEspNowMaxFragCount * nhos::kEspNowFragMaxPayload;
  auto payload = makePayload(fullSize, 31);
  nhos::EspNowFragment frags[nhos::kEspNowMaxFragCount];
  uint8_t count = nhos::EspNowFragmenter::fragment(
      payload.data(), payload.size(), /*frameId=*/1234,
      nhos::kEspNowFragTypeControl, frags, nhos::kEspNowMaxFragCount);
  CHECK(count == nhos::kEspNowMaxFragCount);

  uint8_t scratch[nhos::kEspNowMaxFragCount * nhos::kEspNowFragMaxPayload];
  nhos::EspNowReassembler reassembler(scratch, sizeof(scratch));
  nhos::ReassembledFrame out;

  // Every fragment but the last must report "not complete yet" -- under the
  // UB this returned true immediately on fragment 0.
  for (uint8_t i = 0; i < count - 1; ++i) {
    CHECK(!reassembler.onFragment(frags[i].bytes, frags[i].len, &out));
  }
  CHECK(reassembler.onFragment(frags[count - 1].bytes, frags[count - 1].len, &out));
  CHECK(out.len == payload.size());
  CHECK(out.frameType == nhos::kEspNowFragTypeControl);
  CHECK(std::memcmp(out.data, payload.data(), payload.size()) == 0);
}

// A response one byte past the budget must be rejected up front (count 0)
// rather than partially sent. EspNowPairing::serviceCommand() relies on
// this to substitute its `response_too_large` error instead of going
// silent -- the exact failure that made every `status` command time out.
void testFrameOneByteOverBudgetRejected() {
  const size_t overSize = nhos::kEspNowMaxFragCount * nhos::kEspNowFragMaxPayload + 1;
  auto payload = makePayload(overSize, 7);
  nhos::EspNowFragment frags[nhos::kEspNowMaxFragCount];
  uint8_t count = nhos::EspNowFragmenter::fragment(
      payload.data(), payload.size(), /*frameId=*/5,
      nhos::kEspNowFragTypeControl, frags, nhos::kEspNowMaxFragCount);
  CHECK(count == 0);
}

}  // namespace

int main() {
  testRoundTripSingleFragment();
  testRoundTripWorstCaseFrame();
  testOutOfOrderReassembly();
  testNewFrameAbandonsStalePartial();
  testOversizedFrameRejected();
  testMalformedFragmentDropped();
  testControlFrameTypePreserved();
  testRoundTripMaxFragmentCountFrame();
  testFrameOneByteOverBudgetRejected();

  if (g_failures == 0) {
    std::printf("OK: all EspNowFrame tests passed\n");
    return 0;
  }
  std::fprintf(stderr, "%d EspNowFrame test(s) failed\n", g_failures);
  return 1;
}
