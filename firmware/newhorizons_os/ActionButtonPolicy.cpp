#include "ActionButtonPolicy.h"

#include <cstring>

namespace nhos {

ActionButtonAction actionButtonActionFromName(const char* name) {
  if (!name) return ActionButtonAction::Invalid;
  if (std::strcmp(name, "none") == 0) return ActionButtonAction::None;
  if (std::strcmp(name, "identify") == 0) return ActionButtonAction::Identify;
  if (std::strcmp(name, "toggle_external_led") == 0) return ActionButtonAction::ToggleExternalLed;
  if (std::strcmp(name, "soft_off") == 0) return ActionButtonAction::SoftOff;
  return ActionButtonAction::Invalid;
}

const char* actionButtonActionName(ActionButtonAction action) {
  switch (action) {
    case ActionButtonAction::None:
      return "none";
    case ActionButtonAction::Identify:
      return "identify";
    case ActionButtonAction::ToggleExternalLed:
      return "toggle_external_led";
    case ActionButtonAction::SoftOff:
      return "soft_off";
    case ActionButtonAction::Invalid:
    default:
      return "invalid";
  }
}

bool actionButtonActionAllowedForGesture(ActionButtonAction action, ActionButtonGesture gesture) {
  if (action == ActionButtonAction::Invalid) return false;
  return gesture == ActionButtonGesture::LongPress || action != ActionButtonAction::SoftOff;
}

ActionButtonAction actionButtonActionDefault(ActionButtonGesture gesture) {
  return gesture == ActionButtonGesture::LongPress ? ActionButtonAction::SoftOff : ActionButtonAction::None;
}

}  // namespace nhos
