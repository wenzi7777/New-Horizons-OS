// ScanLateness: start-lateness histogram, gap handling and the rolling
// "recent max" window.
#include <cstdio>

#include "ScanLateness.h"

namespace {
int failures = 0;
void expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}
}  // namespace

int main() {
  nhos::ScanLateness late;
  uint32_t now = 1000;
  late.record(0, now);        // on time
  late.record(499, now);      // still bucket 0
  late.record(500, now);      // bucket 1
  late.record(1999, now);     // bucket 1
  late.record(2000, now);     // bucket 2
  late.record(4999, now);     // bucket 2
  late.record(5000, now);     // bucket 3
  late.record(249999, now);   // bucket 3, just below a gap
  expect(late.samples == 8, "eight samples");
  expect(late.hist[0] == 2 && late.hist[1] == 2 && late.hist[2] == 2 && late.hist[3] == 2,
         "bucket boundaries are lower-inclusive");
  expect(late.lateStarts() == 6, "late = everything from 0.5 ms up");
  expect(late.maxUs == 249999, "max tracks the worst sample");

  late.record(250000, now);   // a pause, not jitter
  late.record(10000000, now);
  expect(late.gaps == 2, "pauses are counted as gaps");
  expect(late.samples == 8 && late.maxUs == 249999, "gaps never touch the histogram or max");

  // recentMaxUs reports the previous complete 5 s window.
  expect(late.recentMaxUs == 0, "no window has completed yet");
  now += nhos::ScanLateness::kWindowUs;
  late.record(100, now);
  expect(late.recentMaxUs == 249999, "first window's worst start");
  late.record(3000, now + 10);
  now += nhos::ScanLateness::kWindowUs;
  late.record(0, now);
  expect(late.recentMaxUs == 3000, "second window's worst start");

  // micros() wraps at 2^32 us (~71 min); the window must survive it.
  nhos::ScanLateness wrap;
  wrap.record(0, 0xFFFFFF00u);
  wrap.record(700, 0xFFFFFF10u);
  wrap.record(0, 0xFFFFFF00u + nhos::ScanLateness::kWindowUs);
  expect(wrap.recentMaxUs == 700, "window rolls across a micros() wrap");

  if (failures != 0) {
    return 1;
  }
  std::printf("test_scan_lateness: OK\n");
  return 0;
}
