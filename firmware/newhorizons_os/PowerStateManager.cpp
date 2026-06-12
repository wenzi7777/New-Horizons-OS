#include "PowerStateManager.h"

#include <driver/gpio.h>
#include <esp_sleep.h>

#include "BoardConfig.h"
#include "BoardPins.h"

namespace nhos {

void PowerStateManager::begin() {
#if NHOS_BOARD_HAS_BUTTON
  pinMode(kActionButtonPin, INPUT_PULLUP);
#endif
}

void PowerStateManager::service(uint32_t nowMs, bool chargerDetected, ChargeState chargeState) {
  chargerDetected_ = chargerDetected;
  chargeState_ = chargeState;

#if NHOS_BOARD_HAS_BUTTON
  const bool pressed = digitalRead(kActionButtonPin) == LOW;
  if (pressed && !buttonDown_) {
    buttonDown_ = true;
    buttonPressedAtMs_ = nowMs;
    longPressHandled_ = false;
    logButtonDown();
  }

  if (state_ == PowerState::Normal) {
    if (pressed && buttonDown_ && !longPressHandled_ && nowMs - buttonPressedAtMs_ >= kLongPressMs) {
      longPressHandled_ = true;
      requestState(
          chargerDetected_ ? PowerState::SoftOffCharging : PowerState::SoftOffBattery,
          "action_button_long_press");
    }
  } else if (!pressed && buttonDown_) {
    const uint32_t heldMs = nowMs - buttonPressedAtMs_;
    if (heldMs >= kShortPressMinMs && heldMs <= kShortPressMaxMs) {
      requestState(PowerState::Normal, "action_button_short_press");
      setWakeSource("button");
    }
  }

  if (!pressed && buttonDown_) {
    logButtonUp(nowMs - buttonPressedAtMs_);
    buttonDown_ = false;
    buttonPressedAtMs_ = 0;
    longPressHandled_ = false;
  }
#endif

  if (state_ == PowerState::SoftOffBattery && chargerDetected_) {
    requestState(PowerState::SoftOffCharging, "charger_connected");
    setWakeSource("charger");
  } else if (state_ == PowerState::SoftOffCharging && !chargerDetected_) {
    requestState(PowerState::SoftOffBattery, "charger_removed");
    setWakeSource("charger");
  }
}

void PowerStateManager::requestState(PowerState nextState, const String& reason) {
  if (state_ == nextState && softOffReason_ == reason) {
    return;
  }
  const PowerState previousState = state_;
  state_ = nextState;
  transitionPending_ = true;
  softOffReason_ = reason;
  logTransitionRequested(previousState, nextState, reason);
  if (previousState == PowerState::Normal && nextState != PowerState::Normal) {
    transitionPhase_ = PowerTransitionPhase::ShutdownAnimationPending;
    transitionStartedMs_ = 0;
    sleepLogPending_ = true;
  } else if (previousState != PowerState::Normal && nextState == PowerState::Normal) {
    transitionPhase_ = PowerTransitionPhase::WakeAnimationRunning;
    transitionStartedMs_ = 0;
  }
}

bool PowerStateManager::requestStateByName(const String& name, bool chargerDetected) {
  if (name == "normal") {
    requestState(PowerState::Normal, "command");
    setWakeSource("command");
    return true;
  }
  if (name == "soft_off" || name == "soft_off_auto") {
    requestState(chargerDetected ? PowerState::SoftOffCharging : PowerState::SoftOffBattery, "command");
    setWakeSource("command");
    return true;
  }
  if (name == "soft_off_battery") {
    requestState(PowerState::SoftOffBattery, "command");
    setWakeSource("command");
    return true;
  }
  if (name == "soft_off_charging") {
    requestState(PowerState::SoftOffCharging, "command");
    setWakeSource("command");
    return true;
  }
  return false;
}

PowerState PowerStateManager::state() const {
  return state_;
}

PowerTransitionPhase PowerStateManager::transitionPhase() const {
  return transitionPhase_;
}

bool PowerStateManager::shouldRunServices() const {
  return state_ == PowerState::Normal;
}

bool PowerStateManager::consumeTransition() {
  const bool pending = transitionPending_;
  transitionPending_ = false;
  return pending;
}

void PowerStateManager::beginShutdownAnimation() {
  transitionPhase_ = PowerTransitionPhase::ShutdownAnimationRunning;
  transitionStartedMs_ = millis();
}

void PowerStateManager::beginWakeAnimation() {
  transitionPhase_ = PowerTransitionPhase::WakeAnimationRunning;
  transitionStartedMs_ = millis();
}

void PowerStateManager::finishPowerTransition() {
  transitionPhase_ = PowerTransitionPhase::None;
  transitionStartedMs_ = 0;
}

uint32_t PowerStateManager::transitionDurationMs() const {
  switch (transitionPhase_) {
    case PowerTransitionPhase::ShutdownAnimationPending:
    case PowerTransitionPhase::ShutdownAnimationRunning:
      return 600;
    case PowerTransitionPhase::WakeAnimationRunning:
      return 500;
    case PowerTransitionPhase::None:
    default:
      return 0;
  }
}

bool PowerStateManager::transitionTimedOut(uint32_t nowMs) const {
  const uint32_t durationMs = transitionDurationMs();
  return durationMs && (nowMs - transitionStartedMs_ >= durationMs);
}

void PowerStateManager::lightSleep() {
  if (state_ == PowerState::Normal) {
    return;
  }
  const bool logThisCycle = sleepLogPending_;
  if (sleepLogPending_) {
    Serial.print(F("soft_off_sleep_enter state="));
    Serial.println(stateName());
    sleepLogPending_ = false;
  }
  uint64_t timerUs = state_ == PowerState::SoftOffCharging ? kSoftOffChargingSleepUs : kSoftOffBatterySleepUs;
#if NHOS_BOARD_HAS_BUTTON
  if (buttonDown_) {
    timerUs = kButtonTrackSleepUs;
  }
#endif
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#if NHOS_BOARD_SUPPORTS_GPIO_WAKE && NHOS_BOARD_HAS_BUTTON
  gpio_wakeup_enable(static_cast<gpio_num_t>(kActionButtonPin), GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
#endif
  esp_sleep_enable_timer_wakeup(timerUs);
  Serial.flush();
  esp_light_sleep_start();
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    setWakeSource("button");
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    setWakeSource("timer");
  } else {
    setWakeSource("unknown");
  }
  if (logThisCycle || cause != ESP_SLEEP_WAKEUP_TIMER) {
    Serial.print(F("soft_off_wake cause="));
    Serial.println(wakeSourceName());
  }
}

String PowerStateManager::statusJson() const {
  String out = "{";
  out += "\"state\":\"";
  out += stateName();
  out += "\",\"wake_source\":\"";
  out += wakeSourceName();
  out += "\",\"soft_off_reason\":\"";
  out += softOffReason_;
  out += "\",\"charger_present\":";
  out += chargerDetected_ ? "true" : "false";
  out += ",\"charge_state\":\"";
  out += chargeStateName();
  out += "\"}";
  return out;
}

const char* PowerStateManager::stateName(PowerState state) const {
  switch (state) {
    case PowerState::SoftOffBattery:
      return "soft_off_battery";
    case PowerState::SoftOffCharging:
      return "soft_off_charging";
    case PowerState::Normal:
    default:
      return "normal";
  }
}

const char* PowerStateManager::stateName() const {
  return stateName(state_);
}

const char* PowerStateManager::wakeSourceName() const {
  return wakeSource_.c_str();
}

const char* PowerStateManager::chargeStateName() const {
  switch (state_) {
    case PowerState::SoftOffBattery:
    case PowerState::SoftOffCharging:
    case PowerState::Normal:
      break;
  }
  switch (chargeState_) {
    case ChargeState::ChargingOrMissing:
      return "charging";
    case ChargeState::ChargeDone:
      return "charge_done";
    case ChargeState::NotCharging:
    default:
      return "not_charging";
  }
}

void PowerStateManager::setWakeSource(const char* source) {
  wakeSource_ = source;
}

void PowerStateManager::logButtonDown() const {
  Serial.println(F("power_button_down"));
}

void PowerStateManager::logButtonUp(uint32_t heldMs) const {
  Serial.print(F("power_button_up held_ms="));
  Serial.println(heldMs);
}

void PowerStateManager::logTransitionRequested(PowerState from, PowerState to, const String& reason) const {
  Serial.print(F("power_transition_requested from="));
  Serial.print(stateName(from));
  Serial.print(F(" to="));
  Serial.print(stateName(to));
  Serial.print(F(" reason="));
  Serial.println(reason);
}

}  // namespace nhos
