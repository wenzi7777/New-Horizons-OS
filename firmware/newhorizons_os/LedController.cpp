#include "LedController.h"

#include "BatteryLedPolicy.h"
#include "BoardPins.h"

#if defined(NHOS_BOARD_V15F)
#include <Adafruit_NeoPixel.h>
#endif

namespace nhos {
namespace {

#if defined(NHOS_BOARD_V15F)
Adafruit_NeoPixel statusPixels(kBoardStatusLedCount, kStatusLedPin,
                               NEO_GRB + NEO_KHZ800);
#endif

bool sameColor(LedColor lhs, LedColor rhs) {
  return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

}  // namespace

void LedController::begin() {
#if defined(NHOS_BOARD_V15F)
  statusPixels.begin();
#else
  pinMode(kStatusLedPin, OUTPUT);
#endif
  bootStartedMs_ = millis();
  setSignal(LedSignal::Boot);
  service(bootStartedMs_);
}

void LedController::service(uint32_t nowMs) {
  LedSignal active = baseSignal_;
  // Only the base pattern may give way to an app; the boot hold and every
  // event (command acks, OTA result, scan warnings) always show.
  bool baseShown = true;
  if (bootStartedMs_ && nowMs - bootStartedMs_ < kBootSolidDurationMs &&
      baseSignal_ != LedSignal::Error && baseSignal_ != LedSignal::RamDanger) {
    active = LedSignal::Boot;
    baseShown = false;
  }
  uint32_t patternMs = nowMs;
  if (eventSignal_ != LedSignal::Off) {
    const Pattern eventPattern = patternFor(eventSignal_);
    if (eventPattern.eventDurationMs && nowMs - eventStartedMs_ <= eventPattern.eventDurationMs) {
      active = eventSignal_;
      patternMs = nowMs - eventStartedMs_;
      baseShown = false;
    } else {
      eventSignal_ = LedSignal::Off;
    }
  }
  if (eventSignal_ == LedSignal::Off && startNextEvent(nowMs)) {
    active = eventSignal_;
    patternMs = 0;
    baseShown = false;
  }

  appShown_ = appHeld_ && baseShown && appMayOverride(active);
  shownSignal_ = active;
  const LedColor nextSystem = appShown_ ? appColor_ : colorFor(active, patternMs);
  shownColor_ = nextSystem;
  const LedColor nextBattery = batteryColorFor(nowMs);
  if (sameColor(nextSystem, currentSystem_) &&
      sameColor(nextBattery, currentBattery_)) {
    return;
  }
  currentSystem_ = nextSystem;
  currentBattery_ = nextBattery;
  // v1.5.F writes both chained WS2812B pixels in one show() transaction.
  // Older one-pixel boards keep their previous single-pixel write path.
  writeStatusPixels(currentSystem_, currentBattery_);
}

void LedController::setSignal(LedSignal signal) {
  baseSignal_ = signal;
}

void LedController::showEvent(LedSignal signal) {
  if (signal == LedSignal::Off) return;
  if (eventSignal_ == LedSignal::Off) {
    eventSignal_ = signal;
    eventStartedMs_ = millis();
    return;
  }
  enqueueEvent(signal);
}

void LedController::setBrightness(float brightness) {
  if (brightness < 0.0f) brightness = 0.0f;
  if (brightness > 1.0f) brightness = 1.0f;
  if (brightness_ == brightness) return;
  brightness_ = brightness;
  // Force the next service pass to refresh both chained pixels at the new
  // level, even if their logical colors did not change.
  currentSystem_ = {255, 255, 255};
  currentBattery_ = {255, 255, 255};
}

void LedController::setStatus(LedColor color) {
  currentSystem_ = color;
  writeStatusPixels(currentSystem_, currentBattery_);
}

void LedController::setAppOverlay(bool held, LedColor color) {
  appHeld_ = held;
  appColor_ = color;
}

// The base patterns that mean "healthy and online": Online itself, and on
// one-pixel boards the charging variants that stand in for it while plugged
// in (without them an app's colour would never show on a bench device).
// Everything else is the system telling the operator something.
bool LedController::appMayOverride(LedSignal signal) {
  switch (signal) {
    case LedSignal::Online:
    case LedSignal::ChargingOrMissing:
    case LedSignal::ChargeDone:
      return true;
    default:
      return false;
  }
}

const char* LedController::signalName(LedSignal signal) {
  switch (signal) {
    case LedSignal::Off: return "off";
    case LedSignal::Boot: return "boot";
    case LedSignal::WifiSetup: return "wifi_setup";
    case LedSignal::WifiConnecting: return "wifi_connecting";
    case LedSignal::EspNowConnecting: return "espnow_connecting";
    case LedSignal::FindMePending: return "findme_pending";
    case LedSignal::Online: return "online";
    case LedSignal::Maintenance: return "maintenance";
    case LedSignal::SafeMode: return "safe_mode";
    case LedSignal::OtaActive: return "ota_active";
    case LedSignal::OtaSuccess: return "ota_success";
    case LedSignal::OtaError: return "ota_error";
    case LedSignal::Error: return "error";
    case LedSignal::ScanWarning: return "scan_warning";
    case LedSignal::RamDanger: return "ram_danger";
    case LedSignal::ChargingOrMissing: return "charging";
    case LedSignal::ChargeDone: return "charge_done";
    case LedSignal::SoftOffTransition: return "soft_off_transition";
    case LedSignal::SoftOffCharging: return "soft_off_charging";
    case LedSignal::SoftOffChargeDone: return "soft_off_charge_done";
    case LedSignal::SoftOffChargeIdle: return "soft_off_charge_idle";
    case LedSignal::PowerTransitionShutdown: return "power_shutdown";
    case LedSignal::PowerTransitionWake: return "power_wake";
    case LedSignal::CommandReceived: return "command_received";
    case LedSignal::CommandSuccess: return "command_success";
    case LedSignal::CommandFailed: return "command_failed";
    case LedSignal::ActionButtonIdentify: return "identify";
    case LedSignal::UplinkDegraded: return "uplink_degraded";
  }
  return "unknown";
}

void LedController::setBatteryStatus(bool sampleValid, uint16_t socCentiPercent,
                                     bool charging, uint8_t lowBatteryThresholdPercent) {
  batterySampleValid_ = sampleValid;
  batterySocCentiPercent_ = socCentiPercent;
  batteryCharging_ = charging;
  lowBatteryThresholdPercent_ = lowBatteryThresholdPercent;
}

void LedController::pulse(LedColor color, uint16_t delayMs) {
  setStatus(color);
  delay(delayMs);
  setStatus(LedPalette::Off);
  delay(delayMs);
}

void LedController::enqueueEvent(LedSignal signal) {
  if (pendingEventCount_ >= kPendingEventCapacity) return;
  pendingEvents_[pendingEventTail_] = signal;
  pendingEventTail_ = (pendingEventTail_ + 1U) % kPendingEventCapacity;
  ++pendingEventCount_;
}

bool LedController::startNextEvent(uint32_t nowMs) {
  if (!pendingEventCount_) return false;
  eventSignal_ = pendingEvents_[pendingEventHead_];
  pendingEventHead_ = (pendingEventHead_ + 1U) % kPendingEventCapacity;
  --pendingEventCount_;
  eventStartedMs_ = nowMs;
  return true;
}

LedController::Pattern LedController::patternFor(LedSignal signal) const {
  switch (signal) {
    case LedSignal::Boot:
      return {PatternMode::Solid, LedPalette::Boot, LedPalette::Off, 0, 0, 0, 0, 0, 0, 255};
    case LedSignal::WifiSetup:
      return {PatternMode::Solid, LedPalette::WifiSetup, LedPalette::Off, 0, 0, 0, 0, 0, 0, 255};
    case LedSignal::WifiConnecting:
      return {PatternMode::Breathe, LedPalette::FindMePending, LedPalette::Off, 1800, 0, 0, 0, 0, 40, 255};
    case LedSignal::EspNowConnecting:
      return {PatternMode::Breathe, LedPalette::HubSearching, LedPalette::Off, 1800, 0, 0, 0, 0, 40, 255};
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
    case LedSignal::UplinkDegraded:
      return {PatternMode::Breathe, LedPalette::Warning, LedPalette::Off, 2200, 0, 0, 0, 0, 40, 255};
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
    case LedSignal::PowerTransitionShutdown:
      return {PatternMode::BlinkBurst, LedPalette::White, LedPalette::Off, 1000, 90, 0, 1, 600, 0, 255};
    case LedSignal::PowerTransitionWake:
      return {PatternMode::BlinkBurst, LedPalette::Online, LedPalette::Off, 1000, 140, 0, 1, 500, 0, 255};
    case LedSignal::CommandReceived:
      return {PatternMode::BlinkBurst, LedPalette::CommandReceived, LedPalette::Off, 1000, 140, 0, 1, 240, 0, 255};
    case LedSignal::CommandSuccess:
      return {PatternMode::BlinkBurst, LedPalette::CommandSuccess, LedPalette::Off, 1000, 260, 0, 1, 420, 0, 255};
    case LedSignal::CommandFailed:
      return {PatternMode::BlinkBurst, LedPalette::Error, LedPalette::Off, 800, 80, 120, 3, 1400, 0, 255};
    case LedSignal::ActionButtonIdentify:
      return {PatternMode::BlinkBurst, LedPalette::White, LedPalette::Off, 900, 120, 100, 3, 920, 0, 255};
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

LedColor LedController::batteryColorFor(uint32_t nowMs) const {
  const BatteryLedColor color = batteryLedColor(
      batterySampleValid_, batterySocCentiPercent_, batteryCharging_, nowMs,
      lowBatteryThresholdPercent_);
  return {color.r, color.g, color.b};
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

LedColor LedController::applyBrightness(LedColor color) const {
  const uint8_t level = static_cast<uint8_t>(brightness_ * 255.0f + 0.5f);
  return scaleColor(color, level);
}

void LedController::writePixel(uint8_t pin, LedColor color) {
#if defined(ESP_ARDUINO_VERSION)
  neopixelWrite(pin, color.r, color.g, color.b);
#else
  digitalWrite(pin, (color.r || color.g || color.b) ? HIGH : LOW);
#endif
}

void LedController::writeStatusPixels(LedColor system, LedColor battery) {
  const LedColor appliedSystem = applyBrightness(system);
  const LedColor appliedBattery = applyBrightness(battery);
#if defined(NHOS_BOARD_V15F)
  statusPixels.setPixelColor(kSystemStatusLedPixelIndex, appliedSystem.r,
                             appliedSystem.g, appliedSystem.b);
  statusPixels.setPixelColor(kBatteryStatusLedPixelIndex, appliedBattery.r,
                             appliedBattery.g, appliedBattery.b);
  statusPixels.show();
#else
  (void)appliedBattery;
  writePixel(kStatusLedPin, appliedSystem);
#endif
}

}  // namespace nhos
