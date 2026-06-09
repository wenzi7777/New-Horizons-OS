#pragma once

#include <Arduino.h>

namespace nhos {

enum class PowerAnimation : uint8_t {
  None = 0,
  Shutdown,
  Wake,
};

}  // namespace nhos
