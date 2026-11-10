#pragma once

#include <Arduino.h>
#include <stddef.h>

#include "BoardConfig.h"

namespace nhos {

extern const uint8_t kRowAdcPins[];
extern const uint8_t kColPins[];
extern const uint8_t kI2cScl;
extern const uint8_t kI2cSda;
extern const uint8_t kStatusLedPin;

#if NHOS_BOARD_HAS_EXT_LED
extern const uint8_t kExternalLedPin;
#else
static constexpr uint8_t kExternalLedPin = 0;
#endif

#if NHOS_BOARD_HAS_BUTTON
extern const uint8_t kActionButtonPin;
#else
static constexpr uint8_t kActionButtonPin = 0xFF;
#endif

#if NHOS_BOARD_HAS_BATID
extern const uint8_t kBatteryIdPin;
#endif

static constexpr size_t kRowAdcPinCount = NHOS_BOARD_ROWS;
static constexpr size_t kColPinCount = NHOS_BOARD_COLS;
static constexpr uint16_t kBoardStatusLedCount = NHOS_BOARD_STATUS_LED_COUNT;
static constexpr uint16_t kStatusLedCount = kBoardStatusLedCount;
static constexpr uint8_t kSystemStatusLedPixelIndex = 0;
static constexpr bool kHasBatteryStatusLedPixel = kBoardStatusLedCount > 1;
static constexpr uint8_t kBatteryStatusLedPixelIndex =
    kHasBatteryStatusLedPixel ? 1 : 0xFF;

#if NHOS_BOARD_HAS_EXT_LED
static constexpr uint16_t kExternalLedCount = NHOS_BOARD_EXTERNAL_LED_COUNT;
#else
static constexpr uint16_t kExternalLedCount = 0;
#endif

bool validatePinMap();
bool isAllowedRowPin(uint8_t pin);
bool isAllowedColPin(uint8_t pin);

}  // namespace nhos
