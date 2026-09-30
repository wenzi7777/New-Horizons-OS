#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

#include "LifetimePolicy.h"

using namespace nhos;

namespace {

void testAccumulateCarriesSubSecondRemainders() {
  LifetimeCounters counters;
  LifetimeRemainder remainder;
  for (int i = 0; i < 4; ++i) {
    lifetimeAccumulate(counters, remainder, 250, true, true);
  }
  assert(counters.awakeS == 1);
  assert(counters.scanS == 1);
  assert(remainder.awakeMs == 0);

  lifetimeAccumulate(counters, remainder, 1999, false, false);
  assert(counters.softOffS == 1);
  assert(remainder.softOffMs == 999);
  assert(counters.awakeS == 1);
  assert(counters.scanS == 1);
  assert(lifetimePowerOnS(counters) == 2);
}

void testAccumulateSaturates() {
  LifetimeCounters counters;
  LifetimeRemainder remainder;
  counters.awakeS = UINT32_MAX - 1;
  lifetimeAccumulate(counters, remainder, 5000, true, false);
  assert(counters.awakeS == UINT32_MAX);
  counters.softOffS = 10;
  assert(lifetimePowerOnS(counters) == UINT32_MAX);
}

void testFreshBootSetsBaselineWithoutCountingAChange() {
  LifetimeCounters counters;
  lifetimeNoteBoot(counters, 1, 42, "v1.9.0", true);
  assert(counters.resetCounts[1] == 1);
  assert(counters.trackedSinceBoot == 42);
  assert(std::strcmp(counters.firstFirmware, "v1.9.0") == 0);
  assert(std::strcmp(counters.lastFirmware, "v1.9.0") == 0);
  assert(counters.firmwareChanges == 0);
}

void testFirmwareChangeIsCountedAndResetsHeapLowWater() {
  LifetimeCounters counters;
  lifetimeNoteBoot(counters, 1, 1, "v1.9.0", true);
  assert(lifetimeNoteMinHeap(counters, 90000));
  lifetimeNoteBoot(counters, 3, 2, "v1.9.0", false);
  assert(counters.firmwareChanges == 0);
  assert(counters.minFreeHeap == 90000);

  lifetimeNoteBoot(counters, 3, 3, "v1.9.1", false);
  assert(counters.firmwareChanges == 1);
  assert(std::strcmp(counters.lastFirmware, "v1.9.1") == 0);
  assert(std::strcmp(counters.firstFirmware, "v1.9.0") == 0);
  assert(counters.minFreeHeap == 0);
  assert(counters.resetCounts[3] == 2);
}

void testOutOfRangeResetReasonLandsInUnknown() {
  LifetimeCounters counters;
  lifetimeNoteBoot(counters, 200, 1, "v1.9.0", true);
  assert(counters.resetCounts[0] == 1);
}

void testTemperatureAndHeapExtremes() {
  LifetimeCounters counters;
  assert(!lifetimeNoteTemperature(counters, NAN));
  assert(!lifetimeNoteTemperature(counters, 400.0f));
  assert(counters.tempMaxC10 == kLifetimeTempUnset);
  assert(lifetimeNoteTemperature(counters, 41.26f));
  assert(counters.tempMaxC10 == 413);
  assert(counters.tempMinC10 == 413);
  assert(lifetimeNoteTemperature(counters, 30.0f));
  assert(counters.tempMinC10 == 300);
  assert(counters.tempMaxC10 == 413);
  assert(!lifetimeNoteTemperature(counters, 35.0f));

  assert(!lifetimeNoteMinHeap(counters, 0));
  assert(lifetimeNoteMinHeap(counters, 80000));
  assert(!lifetimeNoteMinHeap(counters, 81000));
  assert(lifetimeNoteMinHeap(counters, 79000));
  assert(counters.minFreeHeap == 79000);
}

void testSessionKeepsLongest() {
  LifetimeCounters counters;
  lifetimeNoteSession(counters, 100);
  lifetimeNoteSession(counters, 50);
  assert(counters.longestSessionS == 100);
}

void testFlushDueHandlesMillisWrap() {
  assert(!lifetimeFlushDue(1000, 0));
  assert(lifetimeFlushDue(kLifetimeFlushIntervalMs, 0));
  const uint32_t last = UINT32_MAX - 1000;
  assert(!lifetimeFlushDue(last + 5000, last));
  assert(lifetimeFlushDue(last + kLifetimeFlushIntervalMs, last));
}

void testHealthVerdict() {
  LifetimeHealthInputs inputs;
  inputs.bootCount = 50;
  assert(lifetimeHealthReasons(inputs) == 0);
  assert(lifetimeVerdictFor(0) == LifetimeVerdict::Ok);

  inputs.lastCrashBootId = 39;  // 11 boots ago: no longer recent
  assert(lifetimeHealthReasons(inputs) == 0);
  inputs.lastCrashBootId = 40;
  assert(lifetimeHealthReasons(inputs) == kLifetimeReasonRecentCrash);
  assert(lifetimeVerdictFor(kLifetimeReasonRecentCrash) == LifetimeVerdict::Warn);

  inputs.lastCrashBootId = 0;
  inputs.currentTempC10 = kLifetimeHotTempC10;
  inputs.minFreeHeap = kLifetimeLowHeapBytes - 1;
  inputs.rolledBack = true;
  const uint8_t reasons = lifetimeHealthReasons(inputs);
  assert(reasons == (kLifetimeReasonHighTemperature | kLifetimeReasonLowHeap | kLifetimeReasonRolledBack));
  assert(lifetimeVerdictFor(reasons) == LifetimeVerdict::Warn);

  inputs.safeMode = true;
  assert(lifetimeVerdictFor(lifetimeHealthReasons(inputs)) == LifetimeVerdict::Fail);

  assert(std::strcmp(lifetimeVerdictName(LifetimeVerdict::Ok), "ok") == 0);
  assert(std::strcmp(lifetimeVerdictName(LifetimeVerdict::Warn), "warn") == 0);
  assert(std::strcmp(lifetimeVerdictName(LifetimeVerdict::Fail), "fail") == 0);
  for (uint8_t i = 0; i < kLifetimeReasonCount; ++i) {
    assert(std::strcmp(lifetimeReasonName(i), "unknown") != 0);
  }
}

}  // namespace

int main() {
  testAccumulateCarriesSubSecondRemainders();
  testAccumulateSaturates();
  testFreshBootSetsBaselineWithoutCountingAChange();
  testFirmwareChangeIsCountedAndResetsHeapLowWater();
  testOutOfRangeResetReasonLandsInUnknown();
  testTemperatureAndHeapExtremes();
  testSessionKeepsLongest();
  testFlushDueHandlesMillisWrap();
  testHealthVerdict();
  std::cout << "Lifetime policy tests passed\n";
  return 0;
}
