// ReplyPacing: one UDP reply chunk per gap between scans, never starved.
#include <cstdint>
#include <cstdio>

#include "ReplyPacing.h"

namespace {
int failures = 0;
void expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}
using P = nhos::ReplyPacing;
}  // namespace

int main() {
  const uint32_t ready = 1000000;

  expect(!P::maySend(ready - 1, ready, false, 0), "not before the inter-chunk gap ends");
  expect(P::maySend(ready, ready, false, 0), "scanner stopped: as soon as the gap ends");

  expect(P::maySend(ready, ready, true, P::kScanHeadroomUs), "enough headroom before the next scan");
  expect(P::maySend(ready, ready, true, 16000), "right after a scan at 60 fps");
  expect(!P::maySend(ready, ready, true, P::kScanHeadroomUs - 1), "too close to the next scan");
  expect(!P::maySend(ready, ready, true, -500), "a scan is already due: let it run first");

  expect(!P::maySend(ready + P::kMaxDeferUs - 1, ready, true, 100), "still deferring");
  expect(P::maySend(ready + P::kMaxDeferUs, ready, true, 100), "never deferred past kMaxDeferUs");
  expect(P::maySend(ready + P::kMaxDeferUs, ready, true, -100000), "even if the scanner is overdue");

  // micros() wraps every ~71 minutes.
  const uint32_t nearWrap = 0xFFFFF000u;
  expect(!P::maySend(nearWrap, nearWrap + 0x2000u, true, 10000), "gap respected across a wrap");
  expect(P::maySend(nearWrap + 0x2000u, nearWrap + 0x2000u, true, 10000), "eligible after a wrap");

  if (failures != 0) {
    return 1;
  }
  std::printf("test_reply_pacing: OK\n");
  return 0;
}
