#include "BoardPins.h"

namespace nhos {

const uint8_t kRowAdcPins[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

const uint8_t kColPins[] = {
  13, 14, 15, 16, 17, 18, 19, 20, 21, 26,
  47, 33, 34, 48, 35, 36, 37, 38, 39, 40, 41
};

const uint8_t kI2cScl = 42;
const uint8_t kI2cSda = 45;
const uint8_t kStatusLedPin = 11;
const uint8_t kExternalLedPin = 12;
const uint8_t kActionButtonPin = 46;

bool isAllowedRowPin(uint8_t pin) {
  for (size_t i = 0; i < kRowAdcPinCount; ++i) {
    if (kRowAdcPins[i] == pin) {
      return true;
    }
  }
  return false;
}

bool isAllowedColPin(uint8_t pin) {
  for (size_t i = 0; i < kColPinCount; ++i) {
    if (kColPins[i] == pin) {
      return true;
    }
  }
  return false;
}

bool validatePinMap() {
  for (size_t r = 0; r < kRowAdcPinCount; ++r) {
    for (size_t c = 0; c < kColPinCount; ++c) {
      if (kRowAdcPins[r] == kColPins[c]) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace nhos
