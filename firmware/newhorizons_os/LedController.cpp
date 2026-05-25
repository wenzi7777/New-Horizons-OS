#include "LedController.h"

#include "BoardPins.h"

namespace nhos {

void LedController::begin() {
  pinMode(kStatusLedPin, OUTPUT);
  pinMode(kExternalLedPin, OUTPUT);
  setStatus(LedPalette::Boot);
}

void LedController::setStatus(LedColor color) {
  writePixel(kStatusLedPin, color);
}

void LedController::setExternal(uint8_t, LedColor color) {
  writePixel(kExternalLedPin, color);
}

void LedController::pulse(LedColor color, uint16_t delayMs) {
  setStatus(color);
  delay(delayMs);
  setStatus(LedPalette::Off);
  delay(delayMs);
}

void LedController::writePixel(uint8_t pin, LedColor color) {
#if defined(ESP_ARDUINO_VERSION)
  neopixelWrite(pin, color.r, color.g, color.b);
#else
  digitalWrite(pin, (color.r || color.g || color.b) ? HIGH : LOW);
#endif
}

}  // namespace nhos
