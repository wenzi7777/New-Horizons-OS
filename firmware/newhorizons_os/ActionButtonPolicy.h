#pragma once

#include <stdint.h>

namespace nhos {

enum class ActionButtonAction : uint8_t {
  None,
  Identify,
  ToggleExternalLed,
  SoftOff,
  Invalid,
};

enum class ActionButtonGesture : uint8_t {
  None,
  ShortPress,
  LongPress,
};

ActionButtonAction actionButtonActionFromName(const char* name);
const char* actionButtonActionName(ActionButtonAction action);
bool actionButtonActionAllowedForGesture(ActionButtonAction action, ActionButtonGesture gesture);
ActionButtonAction actionButtonActionDefault(ActionButtonGesture gesture);

}  // namespace nhos
