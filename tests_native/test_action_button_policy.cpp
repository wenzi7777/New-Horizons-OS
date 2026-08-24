#include <cassert>
#include <iostream>

#include "ActionButtonPolicy.h"

int main() {
  using namespace nhos;

  assert(actionButtonActionFromName("none") == ActionButtonAction::None);
  assert(actionButtonActionFromName("identify") == ActionButtonAction::Identify);
  assert(actionButtonActionFromName("toggle_external_led") == ActionButtonAction::ToggleExternalLed);
  assert(actionButtonActionFromName("soft_off") == ActionButtonAction::SoftOff);
  assert(actionButtonActionFromName("not_a_real_action") == ActionButtonAction::Invalid);
  assert(actionButtonActionAllowedForGesture(ActionButtonAction::Identify, ActionButtonGesture::ShortPress));
  assert(!actionButtonActionAllowedForGesture(ActionButtonAction::SoftOff, ActionButtonGesture::ShortPress));
  assert(actionButtonActionAllowedForGesture(ActionButtonAction::SoftOff, ActionButtonGesture::LongPress));
  assert(actionButtonActionDefault(ActionButtonGesture::ShortPress) == ActionButtonAction::None);
  assert(actionButtonActionDefault(ActionButtonGesture::LongPress) == ActionButtonAction::SoftOff);

  std::cout << "Action Button policy tests passed\n";
  return 0;
}
