#pragma once

#include <Arduino.h>

#include "PowerManager.h"

namespace nhos {

enum class PowerState : uint8_t {
  Normal = 0,
  SoftOffBattery,
  SoftOffCharging,
};

enum class PowerTransitionPhase : uint8_t {
  None = 0,
  ShutdownAnimationPending,
  ShutdownAnimationRunning,
  WakeAnimationRunning,
};

class PowerStateManager {
 public:
  void begin();
  void service(uint32_t nowMs, bool chargerDetected, ChargeState chargeState);
  void requestState(PowerState nextState, const String& reason);
  bool requestStateByName(const String& name, bool chargerDetected);
  PowerState state() const;
  PowerTransitionPhase transitionPhase() const;
  bool shouldRunServices() const;
  bool consumeTransition();
  void beginShutdownAnimation();
  void beginWakeAnimation();
  void finishPowerTransition();
  uint32_t transitionDurationMs() const;
  bool transitionTimedOut(uint32_t nowMs) const;
  void lightSleep();
  String statusJson() const;

 private:
  static constexpr uint32_t kLongPressMs = 1500;
  static constexpr uint32_t kShortPressMinMs = 50;
  static constexpr uint32_t kShortPressMaxMs = 500;
  static constexpr uint64_t kSoftOffBatterySleepUs = 2000000ULL;
  static constexpr uint64_t kSoftOffChargingSleepUs = 300000ULL;
  static constexpr uint64_t kButtonTrackSleepUs = 50000ULL;

  const char* stateName(PowerState state) const;
  const char* stateName() const;
  const char* wakeSourceName() const;
  const char* chargeStateName() const;
  void setWakeSource(const char* source);
  void logButtonDown() const;
  void logButtonUp(uint32_t heldMs) const;
  void logTransitionRequested(PowerState from, PowerState to, const String& reason) const;

  PowerState state_ = PowerState::Normal;
  bool transitionPending_ = false;
  bool chargerDetected_ = false;
  ChargeState chargeState_ = ChargeState::NotCharging;
  bool buttonDown_ = false;
  uint32_t buttonPressedAtMs_ = 0;
  bool longPressHandled_ = false;
  String softOffReason_ = "";
  String wakeSource_ = "boot";
  PowerTransitionPhase transitionPhase_ = PowerTransitionPhase::None;
  uint32_t transitionStartedMs_ = 0;
  bool sleepLogPending_ = false;
};

}  // namespace nhos
