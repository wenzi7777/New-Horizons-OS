#include "ExternalLedController.h"

#include "BoardPins.h"

namespace nhos {

ExternalLedController::ExternalLedController()
    : pixels_(kExternalLedCount, kExternalLedPin, NEO_GRB + NEO_KHZ800) {}

void ExternalLedController::begin(const ExternalLedConfig& config) {
  pinMode(kExternalLedPin, OUTPUT);
  pixels_.begin();
  initialized_ = true;
  apply(config);
}

void ExternalLedController::apply(const ExternalLedConfig& config) {
  config_ = config;
  if (!DeviceConfig::validExternalLedMode(config_.mode)) {
    config_.mode = "off";
  }
  if (config_.preset.isEmpty()) {
    config_.preset = "stream_health";
  }
  if (config_.brightness < 0.0f) {
    config_.brightness = 0.0f;
  } else if (config_.brightness > 1.0f) {
    config_.brightness = 1.0f;
  }
  if (config_.mode == "off") {
    clear();
  }
}

void ExternalLedController::service(uint32_t nowMs, const ScanHealth& health, LedSignal systemSignal) {
  if (!initialized_ || config_.mode != "enabled") {
    activePreset_ = "off";
    return;
  }

  if (systemSignal == LedSignal::Error || systemSignal == LedSignal::OtaError || systemSignal == LedSignal::RamDanger) {
    activePreset_ = "system_warning";
    showPulse(LedPalette::Error, 2, 5000, 80, 120, nowMs);
    return;
  }
  if (systemSignal == LedSignal::Maintenance || systemSignal == LedSignal::SafeMode) {
    activePreset_ = "maintenance";
    showPulse(LedPalette::Maintenance, 1, 4000, 80, 0, nowMs);
    return;
  }

  if (config_.preset == "pressure_activity") {
    activePreset_ = "pressure_activity";
    showPulse(LedPalette::Online, 1, 3000, 50, 0, nowMs);
  } else if (config_.preset == "recording_focus") {
    activePreset_ = "recording_focus";
    showPulse(LedPalette::Warning, 1, 5000, 70, 0, nowMs);
  } else if (config_.preset == "calibration_focus") {
    activePreset_ = "calibration_focus";
    showPulse(LedPalette::Maintenance, 2, 5000, 60, 100, nowMs);
  } else {
    activePreset_ = health.udpSendFailures || health.overrunFrames ? "stream_health_warning" : "stream_health";
    showPulse(health.udpSendFailures || health.overrunFrames ? LedPalette::Warning : LedPalette::FindMePending, 1, 5000, 60, 0, nowMs);
  }
}

String ExternalLedController::statusJson() const {
  String out = "{";
  out += "\"mode\":\"";
  out += config_.mode;
  out += "\",\"preset\":\"";
  out += config_.preset;
  out += "\",\"active_preset\":\"";
  out += activePreset_;
  out += "\",\"brightness\":";
  out += String(config_.brightness, 2);
  out += ",\"count\":";
  out += String(static_cast<unsigned int>(kExternalLedCount));
  out += "}";
  return out;
}

void ExternalLedController::clear() {
  if (!initialized_) {
    return;
  }
  activePreset_ = "off";
  pixels_.clear();
  pixels_.show();
}

void ExternalLedController::showPulse(LedColor colorValue, uint8_t flashes, uint16_t intervalMs, uint16_t onMs, uint16_t gapMs, uint32_t nowMs) {
  const uint32_t cycleMs = intervalMs ? nowMs % intervalMs : nowMs;
  const uint32_t stepMs = static_cast<uint32_t>(onMs) + gapMs;
  bool on = false;
  for (uint8_t i = 0; i < flashes; ++i) {
    const uint32_t startMs = static_cast<uint32_t>(i) * stepMs;
    if (cycleMs >= startMs && cycleMs < startMs + onMs) {
      on = true;
      break;
    }
  }
  const uint32_t next = on ? color(colorValue) : 0;
  for (uint16_t i = 0; i < kExternalLedCount; ++i) {
    pixels_.setPixelColor(i, next);
  }
  pixels_.show();
}

uint32_t ExternalLedController::color(LedColor colorValue) const {
  return pixels_.Color(scale(colorValue.r), scale(colorValue.g), scale(colorValue.b));
}

uint8_t ExternalLedController::scale(uint8_t value) const {
  return static_cast<uint8_t>(static_cast<float>(value) * config_.brightness);
}

}  // namespace nhos
