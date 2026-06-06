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
      return {PatternMode::Breathe, LedPalette::Boot, LedPalette::Off, 2200, 0, 0, 0, 0, 48, 255};
    case LedSignal::WifiSetup:
      return {PatternMode::Solid, LedPalette::WifiSetup, LedPalette::Off, 0, 0, 0, 0, 0, 0, 255};
    case LedSignal::WifiConnecting:
      return {PatternMode::Breathe, LedPalette::WifiConnecting, LedPalette::Off, 1800, 0, 0, 0, 0, 40, 255};
    case LedSignal::FindMePending:
      return {PatternMode::Breathe, LedPalette::FindMePending, LedPalette::Off, 2200, 0, 0, 0, 0, 40, 255};
    case LedSignal::Online:
      return {PatternMode::Solid, LedPalette::Online, LedPalette::Off, 0, 0, 0, 0, 0, 0, 255};
    case LedSignal::Maintenance:
      return {PatternMode::Solid, LedPalette::Maintenance, LedPalette::Off, 0, 0, 0, 0, 0, 0, 255};
    case LedSignal::SafeMode:
      return {PatternMode::Solid, LedPalette::SafeMode, LedPalette::Off, 0, 0, 0, 0, 0, 0, 255};
    case LedSignal::OtaActive:
      return {PatternMode::Breathe, LedPalette::Ota, LedPalette::Off, 1300, 0, 0, 0, 0, 48, 255};
    case LedSignal::OtaSuccess:
      return {PatternMode::BlinkBurst, LedPalette::Online, LedPalette::Off, 700, 80, 100, 3, 900, 0, 255};
    case LedSignal::OtaError:
      return {PatternMode::BlinkBurst, LedPalette::Error, LedPalette::Off, 700, 100, 120, 3, 1400, 0, 255};
    case LedSignal::Error:
      return {PatternMode::Solid, LedPalette::Error, LedPalette::Off, 0, 0, 0, 0, 0, 0, 255};
    case LedSignal::ScanWarning:
      return {PatternMode::BlinkBurst, LedPalette::Warning, LedPalette::Off, 650, 90, 120, 2, 850, 0, 255};
    case LedSignal::RamDanger:
      return {PatternMode::Solid, LedPalette::Warning, LedPalette::Off, 0, 0, 0, 0, 0, 0, 255};
    case LedSignal::ChargingOrMissing:
      return {PatternMode::Solid, LedPalette::Maintenance, LedPalette::Off, 0, 0, 0, 0, 0, 0, 255};
    case LedSignal::ChargeDone:
      return {PatternMode::Solid, LedPalette::ChargeDone, LedPalette::Off, 0, 0, 0, 0, 0, 0, 255};
    case LedSignal::SoftOffTransition:
      return {PatternMode::BlinkBurst, LedPalette::White, LedPalette::Off, 1000, 120, 0, 1, 600, 0, 255};
    case LedSignal::SoftOffCharging:
      return {PatternMode::Solid, LedPalette::Maintenance, LedPalette::Off, 0, 0, 0, 0, 0, 0, 255};
    case LedSignal::SoftOffChargeDone:
      return {PatternMode::Solid, LedPalette::ChargeDone, LedPalette::Off, 0, 0, 0, 0, 0, 0, 255};
    case LedSignal::SoftOffChargeIdle:
      return {PatternMode::BlinkBurst, LedPalette::White, LedPalette::Off, 1000, 80, 0, 1, 500, 0, 255};
    case LedSignal::CommandReceived:
      return {PatternMode::BlinkBurst, LedPalette::White, LedPalette::Off, 1000, 30, 0, 1, 150, 0, 255};
    case LedSignal::CommandSuccess:
      return {PatternMode::BlinkBurst, LedPalette::Online, LedPalette::Off, 1000, 60, 0, 1, 500, 0, 255};
    case LedSignal::CommandFailed:
      return {PatternMode::BlinkBurst, LedPalette::Error, LedPalette::Off, 800, 80, 120, 3, 1400, 0, 255};
    case LedSignal::Off:
    default:
      return {PatternMode::Off, LedPalette::Off, LedPalette::Off, 0, 0, 0, 0, 0, 0, 0};
  }
}

LedColor LedController::colorFor(LedSignal signal, uint32_t nowMs) const {
  const Pattern pattern = patternFor(signal);
  switch (pattern.mode) {
    case PatternMode::Solid:
      return pattern.color;
    case PatternMode::Breathe: {
      if (pattern.intervalMs < 2) {
        return pattern.color;
      }
      const uint32_t cycleMs = nowMs % pattern.intervalMs;
      const uint32_t halfMs = pattern.intervalMs / 2;
      if (!halfMs) {
        return pattern.color;
      }
      const uint32_t rampMs = cycleMs < halfMs ? cycleMs : (pattern.intervalMs - cycleMs);
      const uint32_t levelRange = static_cast<uint32_t>(pattern.maxLevel) - pattern.minLevel;
      const uint32_t level = pattern.minLevel + ((levelRange * rampMs) / halfMs);
      return scaleColor(pattern.color, static_cast<uint8_t>(level));
    }
    case PatternMode::BlinkBurst:
    case PatternMode::AlternateBurst: {
      if (!pattern.flashes || !pattern.onMs) {
        return LedPalette::Off;
      }
      const uint32_t cycleMs = pattern.intervalMs ? nowMs % pattern.intervalMs : nowMs;
      const uint32_t stepMs = static_cast<uint32_t>(pattern.onMs) + pattern.gapMs;
      for (uint8_t i = 0; i < pattern.flashes; ++i) {
        const uint32_t startMs = static_cast<uint32_t>(i) * stepMs;
        if (cycleMs >= startMs && cycleMs < startMs + pattern.onMs) {
          if (pattern.mode == PatternMode::AlternateBurst && (i % 2) == 1) {
            return pattern.alternate;
          }
          return pattern.color;
        }
      }
      return LedPalette::Off;
    }
    case PatternMode::Off:
    default:
      return LedPalette::Off;
  }
}

LedColor LedController::scaleColor(LedColor color, uint8_t level) const {
  if (level >= 255) {
    return color;
  }
  return {
      static_cast<uint8_t>((static_cast<uint16_t>(color.r) * level) / 255),
      static_cast<uint8_t>((static_cast<uint16_t>(color.g) * level) / 255),
      static_cast<uint8_t>((static_cast<uint16_t>(color.b) * level) / 255),
  };
}

void LedController::writePixel(uint8_t pin, LedColor color) {
#if defined(ESP_ARDUINO_VERSION)
  neopixelWrite(pin, color.r, color.g, color.b);
#else
  digitalWrite(pin, (color.r || color.g || color.b) ? HIGH : LOW);
#endif
}

}  // namespace nhos
