#include "DisplayManager.h"

#include <Wire.h>

#include "Config.h"

namespace nhos {
namespace {
constexpr int16_t kOledWidth = 128;
constexpr int16_t kOledHeight = 32;
constexpr uint8_t kPrimaryAddress = 0x3C;
constexpr uint8_t kFallbackAddress = 0x3D;

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

DisplayManager::DisplayManager()
    : display_(kOledWidth, kOledHeight, &Wire, -1) {}

void DisplayManager::begin(const OledConfig& config) {
  apply(config);
}

void DisplayManager::apply(const OledConfig& config) {
  config_ = config;
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

void DisplayManager::service(uint32_t nowMs, const String& ip, const String& gatewayIp, const ScanHealth& health, uint32_t heapFree, uint32_t heapTotal) {
  if (!enabled_) {
    return;
  }
  const uint8_t hz = config_.updateHz ? config_.updateHz : 1;
  const uint32_t intervalMs = 1000UL / hz;
  if (lastUpdateMs_ && nowMs - lastUpdateMs_ < intervalMs) {
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
  } else {
    renderLiveStatus(ip, gatewayIp, health, heapFree, heapTotal);
  }
  display_.display();
}

String DisplayManager::statusJson() const {
  String out = "{";
  out += "\"mode\":\"";
  out += jsonEscape(config_.mode);
  out += "\",\"enabled\":";
  out += enabled_ ? "true" : "false";
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
  out += ",\"last_error\":\"";
  out += jsonEscape(lastError_);
  out += "\"}";
  return out;
}

bool DisplayManager::configure() {
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
  display_.ssd1306_command(SSD1306_DISPLAYON);
  display_.dim(false);
  display_.ssd1306_command(SSD1306_SETCONTRAST);
  display_.ssd1306_command(config_.contrast);
  display_.clearDisplay();
  display_.display();
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

String DisplayManager::addressString() const {
  if (!address_) {
    return "";
  }
  char buffer[5];
  snprintf(buffer, sizeof(buffer), "0x%02X", address_);
  return String(buffer);
}

}  // namespace nhos
