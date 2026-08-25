#pragma once

#include <cstdint>

namespace nhos {

enum class GaugeSyncState : uint8_t {
  Ready = 0,
  Syncing,
  Error,
};

enum class GaugeSyncReason : uint8_t {
  Boot = 0,
  AutomaticSwap,
  Manual,
};

enum class GaugeSyncStartResult : uint8_t {
  Started = 0,
  InProgress,
};

class BatteryGaugeSyncPolicy {
 public:
  void begin(uint32_t nowMs);
  void observeCharger(uint32_t nowMs, bool detected);
  bool observeVoltage(uint32_t nowMs, bool valid, uint16_t vbatMv);
  GaugeSyncStartResult requestManual(uint32_t nowMs);
  void recordQuickStartWrite(uint32_t nowMs, bool succeeded);
  bool pollDue(uint32_t nowMs) const;
  void recordPoll(uint32_t nowMs, bool succeeded);

  GaugeSyncState state() const { return state_; }
  GaugeSyncReason lastReason() const { return lastReason_; }
  uint32_t syncCount() const { return syncCount_; }

 private:
  void start(uint32_t nowMs, GaugeSyncReason reason);
  bool chargerStable(uint32_t nowMs) const;
  bool automaticCooldownElapsed(uint32_t nowMs) const;

  static constexpr uint32_t kChargerStableMs = 2000;
  static constexpr uint16_t kVoltageStepMv = 250;
  static constexpr uint32_t kFirstPollDelayMs = 1000;
  static constexpr uint32_t kRetryPollMs = 250;
  static constexpr uint32_t kSyncDeadlineMs = 3000;
  static constexpr uint32_t kAutomaticCooldownMs = 30000;

  GaugeSyncState state_ = GaugeSyncState::Ready;
  GaugeSyncReason lastReason_ = GaugeSyncReason::Boot;
  bool chargerDetected_ = false;
  uint32_t chargerDetectedAtMs_ = 0;
  bool hasPreviousVoltage_ = false;
  uint16_t previousVoltageMv_ = 0;
  bool hasCompletedSync_ = false;
  uint32_t lastSyncCompletedMs_ = 0;
  uint32_t syncStartedMs_ = 0;
  uint32_t nextPollMs_ = 0;
  uint32_t syncCount_ = 0;
};

const char* gaugeSyncStateName(GaugeSyncState state);
const char* gaugeSyncReasonName(GaugeSyncReason reason);

}  // namespace nhos
