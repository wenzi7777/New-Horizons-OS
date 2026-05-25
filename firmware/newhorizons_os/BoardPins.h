#pragma once

#include <Arduino.h>
#include <stddef.h>

namespace nhos {

extern const uint8_t kRowAdcPins[];
extern const uint8_t kColPins[];
extern const uint8_t kI2cScl;
extern const uint8_t kI2cSda;
extern const uint8_t kStatusLedPin;
extern const uint8_t kExternalLedPin;
extern const uint8_t kActionButtonPin;

static constexpr size_t kRowAdcPinCount = 10;
static constexpr size_t kColPinCount = 21;
static constexpr uint16_t kStatusLedCount = 1;
static constexpr uint16_t kExternalLedCount = 3;

bool validatePinMap();
bool isAllowedRowPin(uint8_t pin);
bool isAllowedColPin(uint8_t pin);

}  // namespace nhos
