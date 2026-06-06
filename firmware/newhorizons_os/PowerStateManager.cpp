#include "PowerStateManager.h"

#include <driver/gpio.h>
#include <esp_sleep.h>

#include "BoardPins.h"

namespace nhos {

void PowerStateManager::begin() {
  pinMode(kActionButtonPin, INPUT_PULLUP);
}

void PowerStateManager::service(uint32_t nowMs, bool chargerDetected, ChargeState chargeState) {
  chargerDetected_ = chargerDetected;
  chargeState_ = chargeState;

  const bool pressed = digitalRead(kActionButtonPin) == LOW;
  if (pressed && !buttonDown_) {
    buttonDown_ = true;
    buttonPressedAtMs_ = nowMs;
    longPressHandled_ = false;
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
    buttonDown_ = false;
    buttonPressedAtMs_ = 0;
    longPressHandled_ = false;
  }

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
  state_ = nextState;
  transitionPending_ = true;
  softOffReason_ = reason;
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

bool PowerStateManager::shouldRunServices() const {
  return state_ == PowerState::Normal;
}

bool PowerStateManager::consumeTransition() {
  const bool pending = transitionPending_;
  transitionPending_ = false;
  return pending;
}

void PowerStateManager::lightSleep() {
  if (state_ == PowerState::Normal) {
    return;
  }
  const uint64_t timerUs = state_ == PowerState::SoftOffCharging ? kSoftOffChargingSleepUs : kSoftOffBatterySleepUs;
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  gpio_wakeup_enable(static_cast<gpio_num_t>(kActionButtonPin), GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(timerUs);
  esp_light_sleep_start();
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    setWakeSource("button");
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    setWakeSource("timer");
  } else {
    setWakeSource("unknown");
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

const char* PowerStateManager::stateName() const {
  switch (state_) {
    case PowerState::SoftOffBattery:
      return "soft_off_battery";
    case PowerState::SoftOffCharging:
      return "soft_off_charging";
    case PowerState::Normal:
    default:
      return "normal";
  }
}

const char* PowerStateManager::wakeSourceName() const {
  return wakeSource_.c_str();
}

const char* PowerStateManager::chargeStateName() const {
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

}  // namespace nhos
