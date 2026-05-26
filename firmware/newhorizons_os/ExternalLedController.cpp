#include "ExternalLedController.h"

#include "BoardPins.h"

namespace nhos {

namespace {
constexpr uint16_t kIdentifyStepMs = 220;
constexpr uint16_t kIdentifyOnMs = 150;
constexpr uint16_t kIdentifyHoldOffMs = 420;
constexpr float kExternalLedMaxChannel = 96.0f;

uint16_t identifyDurationMs() {
  return static_cast<uint16_t>((static_cast<uint16_t>(kExternalLedCount) * kIdentifyStepMs) + kIdentifyHoldOffMs);
}

String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out;
}
}  // namespace

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
    identifyStartedMs_ = 0;
    clear();
  }
}

void ExternalLedController::identify() {
  if (!initialized_ || config_.mode != "enabled") {
    return;
  }
  identifyStartedMs_ = millis();
}

void ExternalLedController::service(uint32_t nowMs, const ScanHealth& health, LedSignal systemSignal) {
  if (!initialized_ || config_.mode != "enabled") {
    activePreset_ = "off";
    return;
  }

  if (identifyStartedMs_) {
    const uint32_t elapsedMs = nowMs - identifyStartedMs_;
    if (elapsedMs < identifyDurationMs()) {
      activePreset_ = "identify";
      showIdentify(elapsedMs, nowMs);
      return;
    }
    identifyStartedMs_ = 0;
  }

  if (config_.preset == "identify") {
    activePreset_ = "identify";
    showIdentify(nowMs % identifyDurationMs(), nowMs);
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
    showSolid(LedPalette::Online, nowMs);
  } else if (config_.preset == "recording_focus") {
    activePreset_ = "recording_focus";
    showSolid(LedPalette::Warning, nowMs);
  } else if (config_.preset == "calibration_focus") {
    activePreset_ = "calibration_focus";
    showSolid(LedPalette::Maintenance, nowMs);
  } else if (health.udpSendFailures || health.overrunFrames) {
    activePreset_ = "stream_health_warning";
    showSolid(LedPalette::Warning, nowMs);
  } else {
    activePreset_ = "stream_health";
    showSolid(LedPalette::FindMePending, nowMs);
  }
}

String ExternalLedController::statusJson() const {
  String out = "{";
  out += "\"mode\":\"";
  out += jsonEscape(config_.mode);
  out += "\",\"preset\":\"";
  out += jsonEscape(config_.preset);
  out += "\",\"active_preset\":\"";
  out += jsonEscape(activePreset_);
  out += "\",\"brightness\":";
  out += String(config_.brightness, 2);
  out += ",\"count\":";
  out += String(static_cast<unsigned int>(kExternalLedCount));
  out += ",\"pin\":";
  out += String(static_cast<unsigned int>(kExternalLedPin));
  out += ",\"initialized\":";
  out += initialized_ ? "true" : "false";
  out += ",\"last_show_ms\":";
  out += String(static_cast<unsigned long>(lastShowMs_));
  out += ",\"last_error\":\"";
  out += jsonEscape(lastError_);
  out += "\"";
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
  lastShowMs_ = millis();
}

void ExternalLedController::showIdentify(uint32_t elapsedMs, uint32_t nowMs) {
  pixels_.clear();
  const uint16_t step = elapsedMs / kIdentifyStepMs;
  const uint16_t phase = elapsedMs % kIdentifyStepMs;
  if (step < kExternalLedCount && phase < kIdentifyOnMs) {
    pixels_.setPixelColor(step, color(LedPalette::White));
  }
  pixels_.show();
  lastShowMs_ = nowMs;
}

void ExternalLedController::showSolid(LedColor colorValue, uint32_t nowMs) {
  const uint32_t next = color(colorValue);
  for (uint16_t i = 0; i < kExternalLedCount; ++i) {
    pixels_.setPixelColor(i, next);
  }
  pixels_.show();
  lastShowMs_ = nowMs;
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
  lastShowMs_ = nowMs;
}

uint32_t ExternalLedController::color(LedColor colorValue) const {
  return pixels_.Color(scale(colorValue.r), scale(colorValue.g), scale(colorValue.b));
}

uint8_t ExternalLedController::scale(uint8_t value) const {
  if (!value || config_.brightness <= 0.0f) {
    return 0;
  }
  const float normalized = value <= 32 ? static_cast<float>(value) / 32.0f : static_cast<float>(value) / 255.0f;
  const float scaled = normalized * config_.brightness * kExternalLedMaxChannel;
  if (scaled < 1.0f) {
    return 1;
  }
  if (scaled > 255.0f) {
    return 255;
  }
  return static_cast<uint8_t>(scaled);
}

}  // namespace nhos
