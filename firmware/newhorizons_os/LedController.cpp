#include "LedController.h"

#include "BoardPins.h"

namespace nhos {

void LedController::begin() {
  pinMode(kStatusLedPin, OUTPUT);
  setSignal(LedSignal::Boot);
  service(millis());
}

void LedController::service(uint32_t nowMs) {
  LedSignal active = baseSignal_;
  uint32_t patternMs = nowMs;
  if (eventSignal_ != LedSignal::Off) {
    const Pattern eventPattern = patternFor(eventSignal_);
    if (eventPattern.eventDurationMs && nowMs - eventStartedMs_ <= eventPattern.eventDurationMs) {
      active = eventSignal_;
      patternMs = nowMs - eventStartedMs_;
    } else {
      eventSignal_ = LedSignal::Off;
    }
  }

  LedColor next = colorFor(active, patternMs);
  if (next.r == current_.r && next.g == current_.g && next.b == current_.b) {
    return;
  }
  current_ = next;
  writePixel(kStatusLedPin, current_);
}

void LedController::setSignal(LedSignal signal) {
  baseSignal_ = signal;
}

void LedController::showEvent(LedSignal signal) {
  eventSignal_ = signal;
  eventStartedMs_ = millis();
}

void LedController::setStatus(LedColor color) {
  current_ = color;
  writePixel(kStatusLedPin, color);
}

void LedController::pulse(LedColor color, uint16_t delayMs) {
  setStatus(color);
  delay(delayMs);
  setStatus(LedPalette::Off);
  delay(delayMs);
}

LedController::Pattern LedController::patternFor(LedSignal signal) const {
  switch (signal) {
    case LedSignal::Boot:
      return {LedPalette::Boot, LedPalette::Off, 5000, 80, 0, 1, 0, false};
    case LedSignal::WifiSetup:
      return {LedPalette::WifiSetup, LedPalette::Off, 5000, 80, 120, 2, 0, false};
    case LedSignal::WifiConnecting:
      return {LedPalette::WifiConnecting, LedPalette::Off, 5000, 80, 0, 1, 0, false};
    case LedSignal::FindMePending:
      return {LedPalette::FindMePending, LedPalette::Off, 5000, 80, 0, 1, 0, false};
    case LedSignal::Online:
      return {LedPalette::Off, LedPalette::Off, 1000, 0, 0, 0, 0, false};
    case LedSignal::Maintenance:
      return {LedPalette::Maintenance, LedPalette::Off, 3000, 100, 0, 1, 0, false};
    case LedSignal::SafeMode:
      return {LedPalette::SafeMode, LedPalette::Off, 2000, 120, 0, 1, 0, false};
    case LedSignal::OtaActive:
      return {LedPalette::Ota, LedPalette::Off, 1000, 80, 0, 1, 0, false};
    case LedSignal::OtaSuccess:
      return {LedPalette::Online, LedPalette::Off, 700, 80, 100, 3, 900, false};
    case LedSignal::OtaError:
      return {LedPalette::Error, LedPalette::Off, 5000, 100, 120, 3, 0, false};
    case LedSignal::Error:
      return {LedPalette::Error, LedPalette::Off, 1000, 200, 0, 1, 0, false};
    case LedSignal::ScanWarning:
      return {LedPalette::Warning, LedPalette::Off, 10000, 80, 120, 2, 0, false};
    case LedSignal::RamDanger:
      return {LedPalette::Error, LedPalette::Warning, 5000, 100, 120, 2, 0, true};
    case LedSignal::ChargingOrMissing:
      return {LedPalette::Maintenance, LedPalette::Off, 10000, 80, 120, 2, 0, false};
    case LedSignal::ChargeDone:
      return {LedPalette::ChargeDone, LedPalette::Off, 5000, 80, 0, 1, 0, false};
    case LedSignal::CommandReceived:
      return {LedPalette::White, LedPalette::Off, 1000, 30, 0, 1, 150, false};
    case LedSignal::CommandSuccess:
      return {LedPalette::Online, LedPalette::Off, 1000, 60, 0, 1, 500, false};
    case LedSignal::CommandFailed:
      return {LedPalette::Error, LedPalette::Off, 1000, 80, 120, 3, 1400, false};
    case LedSignal::Off:
    default:
      return {LedPalette::Off, LedPalette::Off, 1000, 0, 0, 0, 0, false};
  }
}

LedColor LedController::colorFor(LedSignal signal, uint32_t nowMs) const {
  const Pattern pattern = patternFor(signal);
  if (!pattern.flashes || !pattern.onMs) {
    return LedPalette::Off;
  }

  const uint32_t cycleMs = pattern.intervalMs ? nowMs % pattern.intervalMs : nowMs;
  const uint32_t stepMs = static_cast<uint32_t>(pattern.onMs) + pattern.gapMs;
  for (uint8_t i = 0; i < pattern.flashes; ++i) {
    const uint32_t startMs = static_cast<uint32_t>(i) * stepMs;
    if (cycleMs >= startMs && cycleMs < startMs + pattern.onMs) {
      if (pattern.alternateColor && (i % 2) == 1) {
        return pattern.alternate;
      }
      return pattern.color;
    }
  }
  return LedPalette::Off;
}

void LedController::writePixel(uint8_t pin, LedColor color) {
#if defined(ESP_ARDUINO_VERSION)
  neopixelWrite(pin, color.r, color.g, color.b);
#else
  digitalWrite(pin, (color.r || color.g || color.b) ? HIGH : LOW);
#endif
}

}  // namespace nhos
