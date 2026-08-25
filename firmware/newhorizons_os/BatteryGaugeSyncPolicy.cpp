#include "BatteryGaugeSyncPolicy.h"

namespace nhos {

void BatteryGaugeSyncPolicy::begin(uint32_t nowMs) {
  state_ = GaugeSyncState::Ready;
  lastReason_ = GaugeSyncReason::Boot;
  chargerDetected_ = false;
  chargerDetectedAtMs_ = nowMs;
  hasPreviousVoltage_ = false;
  previousVoltageMv_ = 0;
  hasCompletedSync_ = false;
  lastSyncCompletedMs_ = nowMs;
  syncStartedMs_ = 0;
  nextPollMs_ = 0;
  syncCount_ = 0;
}

void BatteryGaugeSyncPolicy::observeCharger(uint32_t nowMs, bool detected) {
  if (detected == chargerDetected_) {
    return;
  }
  chargerDetected_ = detected;
  chargerDetectedAtMs_ = nowMs;
  hasPreviousVoltage_ = false;
}

bool BatteryGaugeSyncPolicy::observeVoltage(uint32_t nowMs, bool valid,
                                           uint16_t vbatMv) {
  if (!valid || state_ == GaugeSyncState::Syncing) {
    return false;
  }

  bool shouldStart = false;
  if (chargerStable(nowMs) && hasPreviousVoltage_ &&
      automaticCooldownElapsed(nowMs)) {
    const uint16_t delta = vbatMv > previousVoltageMv_
                               ? vbatMv - previousVoltageMv_
                               : previousVoltageMv_ - vbatMv;
    shouldStart = delta >= kVoltageStepMv;
  }
  previousVoltageMv_ = vbatMv;
  hasPreviousVoltage_ = true;

  if (shouldStart) {
    start(nowMs, GaugeSyncReason::AutomaticSwap);
  }
  return shouldStart;
}

GaugeSyncStartResult BatteryGaugeSyncPolicy::requestManual(uint32_t nowMs) {
  if (state_ == GaugeSyncState::Syncing) {
    return GaugeSyncStartResult::InProgress;
  }
  start(nowMs, GaugeSyncReason::Manual);
  return GaugeSyncStartResult::Started;
}

void BatteryGaugeSyncPolicy::recordQuickStartWrite(uint32_t nowMs,
                                                   bool succeeded) {
  if (state_ == GaugeSyncState::Syncing && !succeeded) {
    state_ = GaugeSyncState::Error;
    nextPollMs_ = 0;
    hasCompletedSync_ = true;
    lastSyncCompletedMs_ = nowMs;
  }
}

bool BatteryGaugeSyncPolicy::pollDue(uint32_t nowMs) const {
  return state_ == GaugeSyncState::Syncing &&
         static_cast<int32_t>(nowMs - nextPollMs_) >= 0;
}

void BatteryGaugeSyncPolicy::recordPoll(uint32_t nowMs, bool succeeded) {
  if (state_ != GaugeSyncState::Syncing) {
    return;
  }
  if (succeeded) {
    state_ = GaugeSyncState::Ready;
    nextPollMs_ = 0;
    hasCompletedSync_ = true;
    lastSyncCompletedMs_ = nowMs;
    ++syncCount_;
    return;
  }
  if (nowMs - syncStartedMs_ >= kSyncDeadlineMs) {
    state_ = GaugeSyncState::Error;
    nextPollMs_ = 0;
    hasCompletedSync_ = true;
    lastSyncCompletedMs_ = nowMs;
    return;
  }
  nextPollMs_ = nowMs + kRetryPollMs;
}

void BatteryGaugeSyncPolicy::start(uint32_t nowMs, GaugeSyncReason reason) {
  state_ = GaugeSyncState::Syncing;
  lastReason_ = reason;
  syncStartedMs_ = nowMs;
  nextPollMs_ = nowMs + kFirstPollDelayMs;
}

bool BatteryGaugeSyncPolicy::chargerStable(uint32_t nowMs) const {
  return chargerDetected_ && nowMs - chargerDetectedAtMs_ >= kChargerStableMs;
}

bool BatteryGaugeSyncPolicy::automaticCooldownElapsed(uint32_t nowMs) const {
  return !hasCompletedSync_ ||
         nowMs - lastSyncCompletedMs_ >= kAutomaticCooldownMs;
}

const char* gaugeSyncStateName(GaugeSyncState state) {
  switch (state) {
    case GaugeSyncState::Ready:
      return "ready";
    case GaugeSyncState::Syncing:
      return "syncing";
    case GaugeSyncState::Error:
      return "error";
  }
  return "error";
}

const char* gaugeSyncReasonName(GaugeSyncReason reason) {
  switch (reason) {
    case GaugeSyncReason::Boot:
      return "boot";
    case GaugeSyncReason::AutomaticSwap:
      return "automatic_swap";
    case GaugeSyncReason::Manual:
      return "manual";
  }
  return "boot";
}

}  // namespace nhos
