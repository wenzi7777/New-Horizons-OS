#pragma once

#include <stdint.h>

// How late each matrix scan STARTS relative to when it was due.
//
// overrunFrames cannot see this: it only counts scans whose own duration
// exceeds the frame interval, and scheduleNextScan() quietly rebases the
// schedule onto a late start. A loop that blocks for a few ms (a flash
// write, an OLED page, a synchronous UDP send) therefore delays the next
// scan without any counter moving. This is the measurement that decides
// whether scanning needs its own real-time task; it changes nothing about
// how scanning is scheduled.
//
// Plain C++ (no Arduino) so tests_native exercises it directly.

namespace nhos {

struct ScanLateness {
  // Histogram bucket upper bounds: <0.5 ms, <2 ms, <5 ms, and >= 5 ms.
  static constexpr uint32_t kBucketBoundsUs[3] = {500, 2000, 5000};
  static constexpr uint8_t kBucketCount = 4;
  // A start this late is a pause, not jitter: the stream gate was closed
  // (WiFi down, airtime arbiter, maintenance) or the loop sat in something
  // that long on purpose. Counted apart so one 10 s WiFi outage does not
  // read as a 10 s scheduling fault.
  static constexpr uint32_t kGapUs = 250000;
  // recentMaxUs is the worst start of the last complete window of this
  // length, so a test run can read "worst lately" without a reset command.
  static constexpr uint32_t kWindowUs = 5000000;

  uint32_t samples = 0;
  uint32_t gaps = 0;
  uint32_t maxUs = 0;
  uint32_t hist[kBucketCount] = {0, 0, 0, 0};
  uint32_t recentMaxUs = 0;

  // Starts at least the first bucket bound late.
  uint32_t lateStarts() const { return hist[1] + hist[2] + hist[3]; }

  void record(uint32_t latenessUs, uint32_t nowUs) {
    rollWindow(nowUs);
    if (latenessUs >= kGapUs) {
      ++gaps;
      return;
    }
    ++samples;
    uint8_t bucket = 0;
    while (bucket < kBucketCount - 1 && latenessUs >= kBucketBoundsUs[bucket]) {
      ++bucket;
    }
    ++hist[bucket];
    if (latenessUs > maxUs) {
      maxUs = latenessUs;
    }
    if (latenessUs > windowMaxUs_) {
      windowMaxUs_ = latenessUs;
    }
  }

 private:
  void rollWindow(uint32_t nowUs) {
    if (!windowStarted_) {
      windowStarted_ = true;
      windowStartUs_ = nowUs;
      return;
    }
    if (static_cast<uint32_t>(nowUs - windowStartUs_) >= kWindowUs) {
      recentMaxUs = windowMaxUs_;
      windowMaxUs_ = 0;
      windowStartUs_ = nowUs;
    }
  }

  bool windowStarted_ = false;
  uint32_t windowStartUs_ = 0;
  uint32_t windowMaxUs_ = 0;
};

}  // namespace nhos
