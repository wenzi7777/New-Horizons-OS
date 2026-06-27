#include "ExternalLedController.h"

#include <math.h>

#include "BoardConfig.h"
#include "BoardPins.h"
#include "PowerAnimation.h"

namespace nhos {

namespace {
constexpr uint16_t kIdentifyStepMs = 220;
constexpr uint16_t kIdentifyOnMs = 150;
constexpr uint16_t kIdentifyHoldOffMs = 420;
constexpr float kExternalLedMaxChannel = 96.0f;
constexpr uint32_t kShutdownAnimationMs = 600;
constexpr uint32_t kWakeAnimationMs = 500;
// Streaming activity windows (ms). Derived from ScanHealth deltas so they
// reflect the current situation and recover on their own.
constexpr uint32_t kStreamActiveMs = 1500;
constexpr uint32_t kStreamWarnMs = 3000;

uint16_t identifyDurationMs() {
#if NHOS_BOARD_HAS_EXT_LED
  return static_cast<uint16_t>((static_cast<uint16_t>(kExternalLedCount) * kIdentifyStepMs) + kIdentifyHoldOffMs);
#else
  return 0;
#endif
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
#if NHOS_BOARD_HAS_EXT_LED
    : pixels_(kExternalLedCount, kExternalLedPin, NEO_GRB + NEO_KHZ800) {}
#else
    : pixels_(0, 0, NEO_GRB + NEO_KHZ800) {}
#endif

void ExternalLedController::begin(const ExternalLedConfig& config) {
#if !NHOS_BOARD_HAS_EXT_LED
  initialized_ = false;
  sleeping_ = false;
  activePreset_ = "off";
  config_ = config;
  return;
#endif
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

void ExternalLedController::startPowerAnimation(PowerAnimation animation) {
  powerAnimation_ = static_cast<uint8_t>(PowerAnimation::None);
  powerAnimationStartedMs_ = 0;
  if (!initialized_ || config_.mode != "enabled") {
    return;
  }
  sleeping_ = false;
  powerAnimation_ = static_cast<uint8_t>(animation);
  powerAnimationStartedMs_ = millis();
}

void ExternalLedController::servicePowerAnimation(uint32_t nowMs) {
  if (!powerAnimationActive() || !initialized_) {
    return;
  }
  const PowerAnimation animation = static_cast<PowerAnimation>(powerAnimation_);
  const uint32_t elapsedMs = nowMs - powerAnimationStartedMs_;
  const uint32_t durationMs = animation == PowerAnimation::Shutdown ? kShutdownAnimationMs : kWakeAnimationMs;
  if (elapsedMs >= durationMs) {
    powerAnimation_ = static_cast<uint8_t>(PowerAnimation::None);
    powerAnimationStartedMs_ = 0;
    clear();
    return;
  }

  pixels_.clear();
  if (animation == PowerAnimation::Shutdown) {
    const uint16_t index = static_cast<uint16_t>((elapsedMs / 200U) % kExternalLedCount);
    pixels_.setPixelColor(index, color(LedPalette::White));
  } else if (animation == PowerAnimation::Wake) {
    const uint16_t lit = static_cast<uint16_t>((elapsedMs * (kExternalLedCount + 1)) / durationMs);
    for (uint16_t i = 0; i < kExternalLedCount; ++i) {
      if (i <= lit) {
        pixels_.setPixelColor(i, color(LedPalette::FindMePending));
      }
    }
  }
  pixels_.show();
  lastShowMs_ = nowMs;
}

bool ExternalLedController::powerAnimationActive() const {
  return powerAnimation_ != static_cast<uint8_t>(PowerAnimation::None);
}

void ExternalLedController::sleep() {
  sleeping_ = true;
  clear();
}

void ExternalLedController::wake() {
  sleeping_ = false;
}

void ExternalLedController::service(uint32_t nowMs, const ScanHealth& health, const ExternalLedInputs& in) {
  if (sleeping_) {
    activePreset_ = "off";
    return;
  }
  if (powerAnimationActive()) {
    activePreset_ = "power_transition";
    return;
  }
  if (!initialized_ || config_.mode != "enabled") {
    activePreset_ = "off";
    return;
  }

  // Derive current streaming activity / warnings from cumulative counters by
  // tracking their deltas, so state reflects "now" and recovers by itself.
  const uint32_t failTotal = health.udpSendFailures + health.overrunFrames;
  if (!streamCountersInit_) {
    lastUdpSentFrames_ = health.udpSentFrames;
    lastStreamFailTotal_ = failTotal;
    streamCountersInit_ = true;
  } else {
    if (health.udpSentFrames != lastUdpSentFrames_) {
      lastUdpSentFrames_ = health.udpSentFrames;
      lastStreamFrameMs_ = nowMs ? nowMs : 1;
    }
    if (failTotal != lastStreamFailTotal_) {
      lastStreamFailTotal_ = failTotal;
      lastStreamWarnMs_ = nowMs ? nowMs : 1;
    }
  }
  const bool streamingActive = lastStreamFrameMs_ && (nowMs - lastStreamFrameMs_ < kStreamActiveMs);
  const bool recentWarning = lastStreamWarnMs_ && (nowMs - lastStreamWarnMs_ < kStreamWarnMs);

  // One-shot identify (button / Test / save) takes priority over presets.
  if (identifyStartedMs_) {
    const uint32_t elapsedMs = nowMs - identifyStartedMs_;
    if (elapsedMs < identifyDurationMs()) {
      activePreset_ = "identify";
      showIdentify(elapsedMs, nowMs);
      return;
    }
    identifyStartedMs_ = 0;
  }

  const String& preset = config_.preset;

  if (preset == "off") {
    activePreset_ = "off";
    showSolid(LedPalette::Off, nowMs);
    return;
  }

  if (preset == "identify") {
    activePreset_ = "identify";
    showIdentify(nowMs % identifyDurationMs(), nowMs);
    return;
  }

  if (preset == "solid_marker") {
    activePreset_ = "solid_marker";
    showSolid(markerColor(config_.color), nowMs);
    return;
  }

  if (preset == "connectivity") {
    activePreset_ = "connectivity";
    LedColor seg[3];
    seg[0] = in.wifiConnected ? LedPalette::Online : (in.wifiBusy ? LedPalette::Warning : LedPalette::Error);
    seg[1] = !in.wifiConnected ? LedPalette::Off : (in.hasGateway ? LedPalette::Online : LedPalette::Error);
    seg[2] = !in.hasGateway ? LedPalette::Off : (streamingActive ? LedPalette::Online : LedPalette::Warning);
    showSegments(seg, 3, nowMs);
    return;
  }

  if (preset == "pressure_meter") {
    activePreset_ = "pressure_meter";
    float p = in.pressure01;
    if (p < 0.0f) {
      p = 0.0f;
    } else if (p > 1.0f) {
      p = 1.0f;
    }
    uint8_t lit = static_cast<uint8_t>(ceilf(p * static_cast<float>(kExternalLedCount)));
    if (lit > kExternalLedCount) {
      lit = kExternalLedCount;
    }
    showMeter(lit, LedPalette::Online, LedPalette::Error, nowMs);
    return;
  }

  if (preset == "stream_heartbeat") {
    activePreset_ = "stream_heartbeat";
    if (streamingActive) {
      showPulse(LedPalette::FindMePending, 1, 1000, 120, 0, nowMs);
    } else {
      showSolid(LedPalette::Off, nowMs);
    }
    return;
  }

  if (preset == "calibration_auto") {
    activePreset_ = "calibration_auto";
    if (in.calibrating) {
      showPulse(LedPalette::Maintenance, 1, 1800, 900, 0, nowMs);
    } else {
      showSolid(LedPalette::Off, nowMs);
    }
    return;
  }

  // Default and canonical fallback: system_status.
  activePreset_ = "system_status";
  renderSystemStatus(in, recentWarning, nowMs);
}

void ExternalLedController::renderSystemStatus(const ExternalLedInputs& in, bool recentWarning, uint32_t nowMs) {
  switch (in.systemSignal) {
    case LedSignal::Error:
    case LedSignal::OtaError:
    case LedSignal::RamDanger:
      showPulse(LedPalette::Error, 2, 5000, 80, 120, nowMs);
      return;
    case LedSignal::Maintenance:
    case LedSignal::SafeMode:
      showPulse(LedPalette::Maintenance, 1, 4000, 80, 0, nowMs);
      return;
    case LedSignal::WifiSetup:
      showSolid(LedPalette::WifiSetup, nowMs);
      return;
    case LedSignal::WifiConnecting:
      showSolid(LedPalette::WifiConnecting, nowMs);
      return;
    case LedSignal::FindMePending:
      showSolid(LedPalette::FindMePending, nowMs);
      return;
    case LedSignal::ChargeDone:
      showSolid(LedPalette::ChargeDone, nowMs);
      return;
    case LedSignal::ChargingOrMissing:
      showSolid(LedPalette::Warning, nowMs);
      return;
    case LedSignal::Online:
    default:
      showSolid(recentWarning ? LedPalette::Warning : LedPalette::Online, nowMs);
      return;
  }
}

String ExternalLedController::statusJson() const {
  String out = "{";
  out += "\"mode\":\"";
  out += jsonEscape(config_.mode);
  out += "\",\"preset\":\"";
  out += jsonEscape(config_.preset);
  out += "\",\"color\":\"";
  out += jsonEscape(config_.color);
  out += "\",\"active_preset\":\"";
  out += jsonEscape(activePreset_);
  out += "\",\"brightness\":";
  out += String(config_.brightness, 2);
  out += ",\"count\":";
#if NHOS_BOARD_HAS_EXT_LED
  out += String(static_cast<unsigned int>(kExternalLedCount));
#else
  out += "0";
#endif
  out += ",\"pin\":";
#if NHOS_BOARD_HAS_EXT_LED
  out += String(static_cast<unsigned int>(kExternalLedPin));
#else
  out += "0";
#endif
  out += ",\"initialized\":";
  out += initialized_ ? "true" : "false";
  out += ",\"sleeping\":";
  out += sleeping_ ? "true" : "false";
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

void ExternalLedController::showSegments(const LedColor* colors, size_t count, uint32_t nowMs) {
  pixels_.clear();
  for (uint16_t i = 0; i < kExternalLedCount; ++i) {
    const LedColor c = (colors && i < count) ? colors[i] : LedPalette::Off;
    pixels_.setPixelColor(i, color(c));
  }
  pixels_.show();
  lastShowMs_ = nowMs;
}

void ExternalLedController::showMeter(uint8_t litCount, LedColor low, LedColor high, uint32_t nowMs) {
  pixels_.clear();
  for (uint16_t i = 0; i < kExternalLedCount; ++i) {
    if (i >= litCount) {
      pixels_.setPixelColor(i, 0);
      continue;
    }
    const float t = (kExternalLedCount > 1) ? static_cast<float>(i) / static_cast<float>(kExternalLedCount - 1) : 0.0f;
    LedColor c;
    c.r = static_cast<uint8_t>(low.r + (static_cast<int>(high.r) - low.r) * t);
    c.g = static_cast<uint8_t>(low.g + (static_cast<int>(high.g) - low.g) * t);
    c.b = static_cast<uint8_t>(low.b + (static_cast<int>(high.b) - low.b) * t);
    pixels_.setPixelColor(i, color(c));
  }
  pixels_.show();
  lastShowMs_ = nowMs;
}

LedColor ExternalLedController::markerColor(const String& name) {
  if (name == "green") {
    return LedColor{0, 24, 0};
  }
  if (name == "blue") {
    return LedColor{0, 6, 28};
  }
  if (name == "purple") {
    return LedColor{24, 0, 28};
  }
  if (name == "amber") {
    return LedColor{30, 16, 0};
  }
  if (name == "red") {
    return LedColor{30, 0, 0};
  }
  if (name == "white") {
    return LedColor{22, 22, 22};
  }
  return LedColor{0, 20, 22};  // teal (default)
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
