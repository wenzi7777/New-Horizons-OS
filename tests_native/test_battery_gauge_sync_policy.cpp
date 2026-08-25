#include <cassert>
#include <cstdint>
#include <iostream>

#include "BatteryGaugeSyncPolicy.h"

namespace {

using nhos::BatteryGaugeSyncPolicy;
using nhos::GaugeSyncReason;
using nhos::GaugeSyncStartResult;
using nhos::GaugeSyncState;

void testAutomaticSyncRequiresStableChargerAnd250MvStep() {
  BatteryGaugeSyncPolicy policy;
  policy.begin(0);
  policy.observeCharger(0, true);

  assert(!policy.observeVoltage(0, true, 3700));
  assert(!policy.observeVoltage(1999, true, 3950));
  assert(policy.observeVoltage(2000, true, 4200));
  assert(policy.state() == GaugeSyncState::Syncing);
  assert(policy.lastReason() == GaugeSyncReason::AutomaticSwap);
}

void testAutomaticSyncRejectsSubthresholdAndInvalidSamples() {
  BatteryGaugeSyncPolicy policy;
  policy.begin(0);
  policy.observeCharger(0, true);

  assert(!policy.observeVoltage(0, true, 3700));
  assert(!policy.observeVoltage(2000, false, 0));
  assert(!policy.observeVoltage(2250, true, 3949));
  assert(policy.state() == GaugeSyncState::Ready);
  assert(policy.observeVoltage(2500, true, 4199));
}

void testSyncPollsAfterOneSecondThenRetriesUntilThreeSecondDeadline() {
  BatteryGaugeSyncPolicy policy;
  policy.begin(0);
  assert(policy.requestManual(100) == GaugeSyncStartResult::Started);

  assert(!policy.pollDue(1099));
  assert(policy.pollDue(1100));
  policy.recordPoll(1100, false);
  assert(policy.state() == GaugeSyncState::Syncing);
  assert(!policy.pollDue(1349));
  assert(policy.pollDue(1350));

  policy.recordPoll(3100, false);
  assert(policy.state() == GaugeSyncState::Error);
  assert(!policy.pollDue(3350));
}

void testSuccessfulSyncPublishesReadyStateAndIncrementsCount() {
  BatteryGaugeSyncPolicy policy;
  policy.begin(0);
  assert(policy.requestManual(50) == GaugeSyncStartResult::Started);
  assert(policy.requestManual(60) == GaugeSyncStartResult::InProgress);

  policy.recordPoll(1050, true);
  assert(policy.state() == GaugeSyncState::Ready);
  assert(policy.lastReason() == GaugeSyncReason::Manual);
  assert(policy.syncCount() == 1);
  assert(policy.requestManual(1051) == GaugeSyncStartResult::Started);
}

void testAutomaticSyncHonorsThirtySecondCooldown() {
  BatteryGaugeSyncPolicy policy;
  policy.begin(0);
  policy.observeCharger(0, true);
  assert(!policy.observeVoltage(0, true, 3700));
  assert(policy.observeVoltage(2000, true, 3950));
  policy.recordPoll(3000, true);

  assert(!policy.observeVoltage(4000, true, 4200));
  assert(policy.state() == GaugeSyncState::Ready);
  assert(!policy.observeVoltage(32999, true, 3900));
  assert(policy.observeVoltage(33000, true, 4200));
}

void testQuickStartWriteFailureEndsSyncAndStartsCooldownAtFailure() {
  BatteryGaugeSyncPolicy policy;
  policy.begin(0);
  policy.observeCharger(0, true);
  assert(!policy.observeVoltage(0, true, 3700));
  assert(policy.observeVoltage(2000, true, 3950));
  policy.recordQuickStartWrite(2100, false);
  assert(policy.state() == GaugeSyncState::Error);
  assert(policy.syncCount() == 0);
  assert(!policy.observeVoltage(32099, true, 4200));
  assert(policy.observeVoltage(32100, true, 3900));
}

}  // namespace

int main() {
  testAutomaticSyncRequiresStableChargerAnd250MvStep();
  testAutomaticSyncRejectsSubthresholdAndInvalidSamples();
  testSyncPollsAfterOneSecondThenRetriesUntilThreeSecondDeadline();
  testSuccessfulSyncPublishesReadyStateAndIncrementsCount();
  testAutomaticSyncHonorsThirtySecondCooldown();
  testQuickStartWriteFailureEndsSyncAndStartsCooldownAtFailure();
  std::cout << "Battery gauge sync policy tests passed\n";
  return 0;
}
