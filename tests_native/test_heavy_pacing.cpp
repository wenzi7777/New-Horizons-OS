// HeavyCommandPacing: a heavy UDP command runs in the gap after a scan, runs
// at once when no scan is coming, and never waits more than kMaxDeferUs.
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
using H = nhos::HeavyCommandPacing;
}  // namespace

int main() {
  const uint32_t since = 5000000;
  // No scan coming (scanner stopped, gate closed): never wait for one.
  expect(H::mayRun(since, since, false, -100000), "gate closed: run now");
  expect(H::mayRun(since, since, false, 0), "scanner stopped: run now");
  // Right after a scan at 60 fps on a small matrix: ~14 ms to the next.
  expect(H::mayRun(since, since, true, 14600), "enough headroom: run now");
  expect(H::mayRun(since, since, true, H::kHeadroomUs), "exactly the headroom: run now");
  expect(!H::mayRun(since, since, true, H::kHeadroomUs - 1), "just short: wait");
  expect(!H::mayRun(since, since, true, 2000), "a scan is close: wait");
  expect(!H::mayRun(since, since, true, -300), "a scan is overdue: let it run first");
  // Bounded wait.
  expect(!H::mayRun(since + H::kMaxDeferUs - 1, since, true, 2000), "still within the bound");
  expect(H::mayRun(since + H::kMaxDeferUs, since, true, 2000), "never past kMaxDeferUs");
  // micros() wraps every ~71 minutes.
  const uint32_t nearWrap = 0xFFFFFF00u;
  expect(!H::mayRun(nearWrap + 0x100u, nearWrap, true, 100), "wait across a wrap");
  expect(H::mayRun(nearWrap + H::kMaxDeferUs, nearWrap, true, 100), "bound holds across a wrap");
  // The deferral must fit inside the reply pacing's own patience budget.
  expect(H::kHeadroomUs < 16667, "headroom fits a 60 fps frame");
  if (failures != 0) {
    return 1;
  }
  std::printf("test_heavy_pacing: OK\n");
  return 0;
}
