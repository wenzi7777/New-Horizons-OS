#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

#include "ClockCore.h"

using namespace nhos;

namespace {

constexpr int64_t kEpoch = 1760000000LL * 1000000LL;  // 2025-10, in µs
constexpr int64_t kSec = 1000000LL;

ClockSample sample(TimeSource source, int64_t monoUs, int64_t offsetUs, uint32_t uncertaintyUs = 1000) {
  ClockSample s;
  s.source = source;
  s.monoUs = monoUs;
  s.offsetUs = offsetUs;
  s.uncertaintyUs = uncertaintyUs;
  return s;
}

void testUnsyncedReadsZero() {
  ClockCore core;
  assert(!core.synced());
  assert(core.epochUsAt(5 * kSec) == 0);
  assert(core.uncertaintyUsAt(5 * kSec) == UINT32_MAX);
  assert(core.source() == TimeSource::None);
}

void testRejectsPre2020Time() {
  ClockCore core;
  // A zero/garbage push must never become the clock.
  assert(core.submit(sample(TimeSource::Host, 10 * kSec, 0), 10 * kSec) == ClockSubmitResult::Rejected);
  assert(core.submit(sample(TimeSource::Hub, kSec, 1500000000LL * kSec), kSec) == ClockSubmitResult::Rejected);
  assert(!core.synced());
}

void testFirstSampleSteps() {
  ClockCore core;
  assert(core.submit(sample(TimeSource::Hub, 10 * kSec, kEpoch), 10 * kSec) == ClockSubmitResult::Stepped);
  assert(core.synced());
  assert(core.source() == TimeSource::Hub);
  assert(core.epochUsAt(10 * kSec) == kEpoch + 10 * kSec);
  assert(core.epochUsAt(11 * kSec) == kEpoch + 11 * kSec);
  assert(core.stepCount() == 1);
  assert(core.lastAdjustUs() == 0);
}

void testLargeErrorSteps() {
  ClockCore core;
  core.submit(sample(TimeSource::Hub, 10 * kSec, kEpoch), 10 * kSec);
  const int64_t off = kEpoch + 250000;  // 250 ms off: beyond the step threshold
  assert(core.submit(sample(TimeSource::Hub, 20 * kSec, off), 20 * kSec) == ClockSubmitResult::Stepped);
  assert(core.epochUsAt(20 * kSec) == 20 * kSec + off);
  assert(core.lastAdjustUs() == 250000);
  assert(core.stepCount() == 2);
}

void testSmallErrorSlewsAndNeverRunsBackwards() {
  ClockCore core;
  core.submit(sample(TimeSource::Hub, 10 * kSec, kEpoch), 10 * kSec);
  // 20 ms behind the reference: slewed in at 500 ppm = 40 s.
  const int64_t target = kEpoch - 20000;
  assert(core.submit(sample(TimeSource::Hub, 20 * kSec, target), 20 * kSec) == ClockSubmitResult::Slewed);
  assert(core.lastAdjustUs() == -20000);
  // Nothing moves at the anchor.
  assert(core.epochUsAt(20 * kSec) == kEpoch + 20 * kSec);
  int64_t previous = core.epochUsAt(20 * kSec);
  for (int64_t t = 20 * kSec; t <= 80 * kSec; t += 1000) {
    const int64_t now = core.epochUsAt(t);
    assert(now >= previous);
    previous = now;
  }
  // Halfway through the slew, half the correction has been applied.
  assert(core.pendingSlewUsAt(40 * kSec) == -10000);
  // Done after 40 s: exactly on target.
  assert(core.epochUsAt(60 * kSec) == 60 * kSec + target);
  assert(core.epochUsAt(70 * kSec) == 70 * kSec + target);
  assert(core.pendingSlewUsAt(70 * kSec) == 0);
}

void testResubmitMidSlewStartsFromCurrentReading() {
  ClockCore core;
  core.submit(sample(TimeSource::Hub, 0, kEpoch), 0);
  core.submit(sample(TimeSource::Hub, 0, kEpoch + 40000), 0);  // slew +40 ms
  const int64_t readingAt20 = core.epochUsAt(20 * kSec);        // +10 ms applied
  assert(readingAt20 == kEpoch + 20 * kSec + 10000);
  // A fresh sample now says +30 ms; the reading must not jump.
  assert(core.submit(sample(TimeSource::Hub, 20 * kSec, kEpoch + 30000), 20 * kSec) == ClockSubmitResult::Slewed);
  assert(core.epochUsAt(20 * kSec) == readingAt20);
  assert(core.pendingSlewUsAt(20 * kSec) == 20000);
}

void testLowerPrioritySourceWaitsForStaleness() {
  ClockCore core;
  core.submit(sample(TimeSource::Hub, 0, kEpoch), 0);
  // An automatic host push cannot override a fresh Hub sync...
  assert(core.submit(sample(TimeSource::Host, kSec, kEpoch + 5000), kSec) == ClockSubmitResult::Ignored);
  assert(core.source() == TimeSource::Hub);
  // ...unless it is an explicit user action...
  assert(core.submit(sample(TimeSource::Host, kSec, kEpoch + 5000), kSec, true) == ClockSubmitResult::Slewed);
  assert(core.source() == TimeSource::Host);
  // ...or the better source has gone quiet.
  ClockCore other;
  other.submit(sample(TimeSource::Sntp, 0, kEpoch), 0);
  const int64_t later = kClockSourceStaleUs + kSec;
  assert(other.submit(sample(TimeSource::Host, later, kEpoch), later) == ClockSubmitResult::Slewed);
  // A better source always takes over.
  assert(other.submit(sample(TimeSource::Sntp, later + kSec, kEpoch), later + kSec) == ClockSubmitResult::Slewed);
  assert(other.source() == TimeSource::Sntp);
}

void testGatewayOutranksNtpUntilItGoesQuiet() {
  ClockCore core;
  core.submit(sample(TimeSource::Gateway, 0, kEpoch), 0);
  // NTP disagreeing by 3 ms does not pull a device away from its Gateway...
  assert(core.submit(sample(TimeSource::Sntp, kSec, kEpoch + 3000), kSec) == ClockSubmitResult::Ignored);
  assert(core.source() == TimeSource::Gateway);
  // ...but takes over once the Gateway has been silent long enough.
  const int64_t later = kClockSourceStaleUs + kSec;
  assert(core.submit(sample(TimeSource::Sntp, later, kEpoch + 3000), later) == ClockSubmitResult::Slewed);
  assert(core.source() == TimeSource::Sntp);
  // And the Gateway coming back wins immediately.
  assert(core.submit(sample(TimeSource::Gateway, later + kSec, kEpoch), later + kSec) == ClockSubmitResult::Slewed);
  assert(core.source() == TimeSource::Gateway);
  assert(std::string(timeSourceName(TimeSource::Gateway)) == "gateway");
}

void testUncertaintyGrowsInHoldover() {
  ClockCore core;
  core.submit(sample(TimeSource::Hub, 0, kEpoch, 2000), 0);
  assert(core.uncertaintyUsAt(0) == 2000);
  // 50 ppm over 100 s = 5 ms.
  assert(core.uncertaintyUsAt(100 * kSec) == 2000 + 5000);
}

void testOldSampleCountsItsAge() {
  ClockCore core;
  // A filtered sample taken 20 s ago has been free-running for 20 s.
  core.submit(sample(TimeSource::Hub, 0, kEpoch, 1000), 20 * kSec);
  assert(core.uncertaintyUsAt(20 * kSec) == 1000 + 1000);
}

void testFilterPicksSmallestDelay() {
  ClockFilter filter;
  ClockSample out;
  assert(!filter.best(&out));
  filter.add(sample(TimeSource::Hub, 1, kEpoch + 900), 1800);
  filter.add(sample(TimeSource::Hub, 2, kEpoch + 100), 200);
  filter.add(sample(TimeSource::Hub, 3, kEpoch + 500), 1000);
  uint32_t delay = 0;
  assert(filter.best(&out, &delay));
  assert(out.offsetUs == kEpoch + 100);
  assert(delay == 200);
}

void testFilterWindowForgetsOldSamples() {
  ClockFilter filter;
  filter.add(sample(TimeSource::Hub, 1, kEpoch), 10);
  for (size_t i = 0; i < ClockFilter::kWindow; ++i) {
    filter.add(sample(TimeSource::Hub, 2 + static_cast<int64_t>(i), kEpoch + 1000), 500);
  }
  ClockSample out;
  assert(filter.best(&out));
  assert(out.offsetUs == kEpoch + 1000);
  assert(filter.count() == ClockFilter::kWindow);
}

void testFilterFlushesWhenReferenceJumps() {
  ClockFilter filter;
  filter.add(sample(TimeSource::Hub, 1, kEpoch), 10);
  // The Hub stepped by 2 s: the low-delay pre-step sample must not win.
  filter.add(sample(TimeSource::Hub, 2, kEpoch + 2 * kSec), 3000);
  ClockSample out;
  assert(filter.best(&out));
  assert(out.offsetUs == kEpoch + 2 * kSec);
  assert(filter.count() == 1);
}

}  // namespace

int main() {
  testUnsyncedReadsZero();
  testRejectsPre2020Time();
  testFirstSampleSteps();
  testLargeErrorSteps();
  testSmallErrorSlewsAndNeverRunsBackwards();
  testResubmitMidSlewStartsFromCurrentReading();
  testLowerPrioritySourceWaitsForStaleness();
  testGatewayOutranksNtpUntilItGoesQuiet();
  testUncertaintyGrowsInHoldover();
  testOldSampleCountsItsAge();
  testFilterPicksSmallestDelay();
  testFilterWindowForgetsOldSamples();
  testFilterFlushesWhenReferenceJumps();
  std::cout << "Clock core tests passed\n";
  return 0;
}
