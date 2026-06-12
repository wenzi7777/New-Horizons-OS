#include "BoardPins.h"

namespace nhos {

#if defined(NHOS_BOARD_GCU_V23D_LTS)

const uint8_t kRowAdcPins[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

const uint8_t kColPins[] = {16, 17, 18, 19, 20, 21, 35, 36, 37, 39, 40, 41, 42, 45, 46};

const uint8_t kI2cScl = 47;
const uint8_t kI2cSda = 48;
const uint8_t kStatusLedPin = 38;

#else

const uint8_t kRowAdcPins[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

const uint8_t kColPins[] = {
  13, 14, 15, 16, 17, 18, 19, 20, 21, 26,
  47, 33, 34, 48, 35, 36, 37, 38, 39, 40, 41
};

const uint8_t kI2cScl = 42;
const uint8_t kI2cSda = 45;
const uint8_t kStatusLedPin = 11;

#if NHOS_BOARD_HAS_EXT_LED
const uint8_t kExternalLedPin = 12;
#endif

#if NHOS_BOARD_HAS_BUTTON
const uint8_t kActionButtonPin = 46;
#endif

#endif

namespace {

bool hasPin(const uint8_t* pins, size_t count, uint8_t pin) {
  for (size_t i = 0; i < count; ++i) {
    if (pins[i] == pin) {
      return true;
    }
  }
  return false;
}

bool isAllowedBoardPin(uint8_t pin) {
  if (pin == kI2cScl || pin == kI2cSda || pin == kStatusLedPin) {
    return false;
  }
#if NHOS_BOARD_HAS_EXT_LED
  if (pin == kExternalLedPin) {
    return false;
  }
#endif
#if NHOS_BOARD_HAS_BUTTON
  if (pin == kActionButtonPin) {
    return false;
  }
#endif
  return true;
}

}  // namespace

bool isAllowedRowPin(uint8_t pin) {
  return hasPin(kRowAdcPins, kRowAdcPinCount, pin);
}

bool isAllowedColPin(uint8_t pin) {
  return hasPin(kColPins, kColPinCount, pin);
}

bool validatePinMap() {
  for (size_t r = 0; r < kRowAdcPinCount; ++r) {
    if (!isAllowedBoardPin(kRowAdcPins[r])) {
      return false;
    }
    for (size_t c = 0; c < kColPinCount; ++c) {
      if (kRowAdcPins[r] == kColPins[c]) {
        return false;
      }
    }
  }
  for (size_t c = 0; c < kColPinCount; ++c) {
    if (!isAllowedBoardPin(kColPins[c])) {
      return false;
    }
  }
  return true;
}

}  // namespace nhos
