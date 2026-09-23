#include "DisplayManager.h"

#include <Wire.h>

#include "BoardConfig.h"
#include "Config.h"
#include "PowerAnimation.h"

namespace nhos {
namespace {
constexpr int16_t kOledWidth = 128;
constexpr int16_t kOledHeight = 32;
constexpr uint8_t kPrimaryAddress = 0x3C;
constexpr uint8_t kFallbackAddress = 0x3D;
constexpr uint32_t kShutdownAnimationMs = 600;
constexpr uint32_t kWakeAnimationMs = 500;

String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out;
}
}  // namespace

// The last two arguments are the bus clock during a transfer and the one to
// leave behind. The library's default for the second is 100kHz, so every
// refresh used to drop the shared bus -- IMU, magnetometer, fuel gauge -- to a
// quarter of its speed (or a tenth, on a 1MHz board) until something else
// happened to set it back. 400kHz is the SSD1306's own ceiling.
DisplayManager::DisplayManager()
    : display_(kOledWidth, kOledHeight, &Wire, -1, 400000UL, NHOS_BOARD_I2C_HZ) {}

void DisplayManager::begin(const OledConfig& config) {
  apply(config);
}

void DisplayManager::apply(const OledConfig& config) {
  config_ = config;
  invalidateShadow();
#if !NHOS_BOARD_HAS_OLED
  enabled_ = false;
  detected_ = false;
  initialized_ = false;
  sleeping_ = false;
  lastError_ = "";
  return;
#endif
  if (!DeviceConfig::validOledMode(config_.mode)) {
    config_.mode = "off";
  }
  if (config_.page.isEmpty()) {
    config_.page = "live_status";
  }
  if (config_.updateHz < 1) {
    config_.updateHz = 1;
  } else if (config_.updateHz > 5) {
    config_.updateHz = 5;
  }

  if (config_.mode == "off") {
    enabled_ = false;
    detected_ = false;
    sleeping_ = false;
    lastError_ = "";
    if (initialized_) {
      display_.clearDisplay();
      display_.display();
      display_.ssd1306_command(SSD1306_DISPLAYOFF);
    }
    return;
  }

  enabled_ = configure();
  if (!enabled_ && config_.mode == "auto") {
    lastError_ = "";
  }
}

void DisplayManager::startPowerAnimation(PowerAnimation animation) {
  powerAnimation_ = static_cast<uint8_t>(PowerAnimation::None);
  powerAnimationStartedMs_ = 0;
  if (!initialized_ || !enabled_) {
    return;
  }
  sleeping_ = false;
  display_.ssd1306_command(SSD1306_DISPLAYON);
  powerAnimation_ = static_cast<uint8_t>(animation);
  powerAnimationStartedMs_ = millis();
}

void DisplayManager::servicePowerAnimation(uint32_t nowMs) {
  if (!powerAnimationActive() || !initialized_ || !enabled_) {
    return;
  }
  const PowerAnimation animation = static_cast<PowerAnimation>(powerAnimation_);
  const uint32_t elapsedMs = nowMs - powerAnimationStartedMs_;
  const uint32_t durationMs = animation == PowerAnimation::Shutdown ? kShutdownAnimationMs : kWakeAnimationMs;
  if (elapsedMs >= durationMs) {
    powerAnimation_ = static_cast<uint8_t>(PowerAnimation::None);
    powerAnimationStartedMs_ = 0;
    lastUpdateMs_ = 0;
    return;
  }

  if (animation == PowerAnimation::Shutdown) {
    renderPowerAnimation("Powering off", elapsedMs, durationMs);
  } else if (animation == PowerAnimation::Wake) {
    renderPowerAnimation("Waking", elapsedMs, durationMs);
  }
}

bool DisplayManager::powerAnimationActive() const {
  return powerAnimation_ != static_cast<uint8_t>(PowerAnimation::None);
}

void DisplayManager::sleep() {
  invalidateShadow();
  if (!initialized_) {
    return;
  }
  display_.clearDisplay();
  display_.display();
  display_.ssd1306_command(SSD1306_DISPLAYOFF);
  sleeping_ = true;
}

void DisplayManager::wake() {
  sleeping_ = false;
  invalidateShadow();
  if (!initialized_ || !enabled_) {
    return;
  }
  display_.ssd1306_command(SSD1306_DISPLAYON);
  display_.ssd1306_command(SSD1306_SETCONTRAST);
  display_.ssd1306_command(config_.contrast);
  lastUpdateMs_ = 0;
}

bool DisplayManager::refreshDue(uint32_t nowMs) const {
  if (!enabled_ || sleeping_ || powerAnimationActive()) {
    return false;
  }
  if (dirtyPages_ != 0) {
    return true;
  }
  const uint8_t hz = config_.updateHz ? config_.updateHz : 1;
  const uint32_t intervalMs = 1000UL / hz;
  return !lastUpdateMs_ || nowMs - lastUpdateMs_ >= intervalMs;
}

void DisplayManager::service(uint32_t nowMs, const String& ip, const String& gatewayIp, const ScanHealth& health, uint32_t heapFree, uint32_t heapTotal) {
  if (!refreshDue(nowMs)) {
    return;
  }
  // Finish sending the last frame before drawing the next one.
  if (dirtyPages_ != 0) {
    flushOnePage();
    return;
  }
  lastUpdateMs_ = nowMs;

  display_.clearDisplay();
  display_.setTextSize(1);
  display_.setTextColor(SSD1306_WHITE);
  display_.setCursor(0, 0);
  if (config_.page == "sensor_snapshot") {
    renderSensorSnapshot(health);
  } else if (config_.page == "recording_status") {
    renderRecordingStatus(health);
  } else if (config_.page == "app") {
    renderAppPage();
  } else {
    renderLiveStatus(ip, gatewayIp, health, heapFree, heapTotal);
  }
  const uint8_t* buffer = display_.getBuffer();
  if (buffer == nullptr) {
    return;
  }
  for (uint8_t page = 0; page < kPages; ++page) {
    const uint8_t* now = buffer + page * kPageBytes;
    if (!shadowValid_ || memcmp(now, shadow_ + page * kPageBytes, kPageBytes) != 0) {
      dirtyPages_ |= static_cast<uint8_t>(1u << page);
    }
  }
  shadowValid_ = true;
  if (dirtyPages_ != 0) {
    flushOnePage();
  }
}

void DisplayManager::flushOnePage() {
  const uint8_t* buffer = display_.getBuffer();
  if (buffer == nullptr || address_ == 0) {
    dirtyPages_ = 0;
    return;
  }
  uint8_t page = 0;
  while (page < kPages && (dirtyPages_ & (1u << page)) == 0) {
    ++page;
  }
  if (page >= kPages) {
    dirtyPages_ = 0;
    return;
  }
  // Horizontal addressing (set by begin()): bound the write to this page and
  // the full width, then stream its 128 bytes as data.
  display_.ssd1306_command(0x22);  // SSD1306_PAGEADDR
  display_.ssd1306_command(page);
  display_.ssd1306_command(page);
  display_.ssd1306_command(0x21);  // SSD1306_COLUMNADDR
  display_.ssd1306_command(0);
  display_.ssd1306_command(kOledWidth - 1);
  const uint8_t* bytes = buffer + page * kPageBytes;
  // 64 at a time: the control byte plus data must fit the Wire buffer.
  constexpr uint16_t kChunk = 64;
  for (uint16_t offset = 0; offset < kPageBytes; offset += kChunk) {
    Wire.beginTransmission(address_);
    Wire.write(static_cast<uint8_t>(0x40));  // Co=0, D/C#=1: data follows
    Wire.write(bytes + offset, kChunk);
    Wire.endTransmission();
  }
  memcpy(shadow_ + page * kPageBytes, bytes, kPageBytes);
  dirtyPages_ = static_cast<uint8_t>(dirtyPages_ & ~(1u << page));
}

void DisplayManager::renderPowerAnimation(const char* label, uint32_t elapsedMs, uint32_t durationMs) {
  invalidateShadow();
  // label == "Powering off" / label == "Waking" are the only supported variants.
  const bool isPoweringOff = String(label) == "Powering off";
  const bool isWaking = String(label) == "Waking";
  display_.clearDisplay();
  display_.setTextSize(1);
  display_.setTextColor(SSD1306_WHITE);
  display_.setCursor(20, 4);
  display_.print(label);

  const uint8_t steps = 8;
  const float angleStep = 6.2831853f / static_cast<float>(steps);
  const uint8_t active = static_cast<uint8_t>((elapsedMs / 70U) % steps);
  const int16_t centerX = 64;
  const int16_t centerY = 23;
  const int16_t radius = 8;
  for (uint8_t i = 0; i < steps; ++i) {
    const float angle = angleStep * static_cast<float>(i);
    const int16_t x = static_cast<int16_t>(centerX + cosf(angle) * radius);
    const int16_t y = static_cast<int16_t>(centerY + sinf(angle) * radius);
    const uint8_t dotRadius = i == active ? 2 : 1;
    display_.fillCircle(x, y, dotRadius, SSD1306_WHITE);
  }
  display_.drawRoundRect(12, 0, 104, 32, 4, SSD1306_WHITE);
  if (isPoweringOff || isWaking) {
    display_.fillRect(14, 26, 2, 4, SSD1306_WHITE);
  }
  display_.drawLine(16, 28, static_cast<int16_t>(16 + ((96UL * elapsedMs) / durationMs)), 28, SSD1306_WHITE);
  display_.display();
}

String DisplayManager::statusJson() const {
  String out = "{";
  out += "\"mode\":\"";
  out += jsonEscape(config_.mode);
  out += "\",\"enabled\":";
  out += enabled_ ? "true" : "false";
  out += ",\"sleeping\":";
  out += sleeping_ ? "true" : "false";
  out += ",\"detected\":";
  out += detected_ ? "true" : "false";
  out += ",\"addr\":\"";
  out += addressString();
  out += "\",\"page\":\"";
  out += jsonEscape(config_.page);
  out += "\",\"update_hz\":";
  out += String(static_cast<unsigned int>(config_.updateHz));
  out += ",\"contrast\":";
  out += String(static_cast<unsigned int>(config_.contrast));
  out += ",\"rotation\":";
  out += String(static_cast<unsigned int>(config_.rotation));
  out += ",\"last_error\":\"";
  out += jsonEscape(lastError_);
  out += "\"}";
  return out;
}

bool DisplayManager::configure() {
#if !NHOS_BOARD_HAS_OLED
  return false;
#endif
  uint8_t nextAddress = 0;
  if (probeAddress(kPrimaryAddress)) {
    nextAddress = kPrimaryAddress;
  } else if (probeAddress(kFallbackAddress)) {
    nextAddress = kFallbackAddress;
  }

  if (!nextAddress) {
    detected_ = false;
    enabled_ = false;
    address_ = 0;
    lastError_ = config_.mode == "enabled" ? "oled_not_found" : "";
    return false;
  }

  detected_ = true;
  address_ = nextAddress;
  if (!display_.begin(SSD1306_SWITCHCAPVCC, address_)) {
    initialized_ = false;
    enabled_ = false;
    lastError_ = "oled_init_failed";
    return false;
  }
  initialized_ = true;
  enabled_ = true;
  lastError_ = "";
  sleeping_ = false;
  display_.setRotation(config_.rotation);
  display_.ssd1306_command(SSD1306_DISPLAYON);
  display_.dim(false);
  display_.ssd1306_command(SSD1306_SETCONTRAST);
  display_.ssd1306_command(config_.contrast);
  display_.clearDisplay();
  display_.display();
  invalidateShadow();
  return true;
}

bool DisplayManager::probeAddress(uint8_t address) const {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

OledMode DisplayManager::parseMode(const String& mode) const {
  if (mode == "auto") {
    return OledMode::Auto;
  }
  if (mode == "enabled") {
    return OledMode::Enabled;
  }
  return OledMode::Off;
}

void DisplayManager::renderLiveStatus(const String& ip, const String& gatewayIp, const ScanHealth& health, uint32_t heapFree, uint32_t heapTotal) {
  display_.print("NHOS ");
  display_.println(kFirmwareVersion);
  display_.print("IP ");
  display_.println(ip);
  display_.print("GW ");
  display_.println(gatewayIp.isEmpty() ? "-" : gatewayIp);
  display_.print("FPS");
  display_.print(health.actualScanFps);
  display_.print("/");
  display_.print(health.targetFps);
  display_.print(" RAM");
  display_.print(heapFree / 1024);
  display_.print("/");
  display_.print(heapTotal / 1024);
  display_.println("K");
}

void DisplayManager::renderSensorSnapshot(const ScanHealth& health) {
  display_.println("Sensor snapshot");
  display_.print("Pts ");
  display_.print(health.pointCount);
  display_.print(" Grid ");
  display_.print(health.rows);
  display_.print("x");
  display_.println(health.cols);
  display_.print("Scan ");
  display_.print(health.lastScanDurationUs);
  display_.println("us");
  display_.print("Over budget ");
  display_.println(health.overrunFrames);
}

void DisplayManager::renderRecordingStatus(const ScanHealth& health) {
  display_.println("Stream status");
  display_.print("Sent ");
  display_.print(health.udpSentFrames);
  display_.println(" packets");
  display_.print("Fail ");
  display_.print(health.udpSendFailures);
  display_.println(" packets");
  display_.print("UDP ");
  display_.print(health.lastUdpSendUs);
  display_.println("us");
}

void DisplayManager::renderAppPage() {
  bool drewAny = false;
  for (uint8_t row = 0; row < kOledRows; ++row) {
    AppDisplayLine line;
    if (appLineSource_ == nullptr || !appLineSource_(row, line)) {
      continue;
    }
    drewAny = true;
    const int16_t y = static_cast<int16_t>(row * kOledRowPx);
    display_.setCursor(0, y);
    if (line.kind == AppDisplayKind::Text) {
      char text[kOledCols + 1];
      formatOledTextLine(line.label, line.value, line.digits, text);
      display_.print(text);
    } else {
      const size_t labelLen = strlen(line.label);
      display_.print(line.label);
      const OledBarGeometry bar = oledBarGeometry(
          static_cast<uint8_t>(labelLen), line.value, line.lo, line.hi);
      display_.drawRect(bar.x0, y, bar.width, kOledRowPx - 1, SSD1306_WHITE);
      if (bar.fillPx > 0) {
        display_.fillRect(static_cast<int16_t>(bar.x0 + 1), static_cast<int16_t>(y + 1),
                          bar.fillPx, kOledRowPx - 3, SSD1306_WHITE);
      }
    }
  }
  if (!drewAny) {
    // Said plainly, so a blank panel is not mistaken for a dead one.
    display_.println("App page");
    display_.println("No running app is");
    display_.println("drawing on the OLED.");
  }
}

String DisplayManager::addressString() const {
  if (!address_) {
    return "";
  }
  char buffer[5];
  snprintf(buffer, sizeof(buffer), "0x%02X", address_);
  return String(buffer);
}

}  // namespace nhos
