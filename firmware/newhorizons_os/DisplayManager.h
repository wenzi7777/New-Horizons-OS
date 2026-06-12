#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "BoardConfig.h"
#include "DeviceConfig.h"
#include "MatrixScanner.h"

#if NHOS_BOARD_HAS_OLED
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#else
class Adafruit_SSD1306 {
 public:
  Adafruit_SSD1306(int16_t, int16_t, TwoWire*, int8_t) {}

  bool begin(uint8_t, uint8_t) { return false; }
  void clearDisplay() {}
  void display() {}
  void ssd1306_command(uint8_t) {}
  void setTextSize(uint8_t) {}
  void setTextColor(uint16_t) {}
  void setCursor(int16_t, int16_t) {}
  void setRotation(uint8_t) {}
  void dim(bool) {}
  void fillCircle(int16_t, int16_t, int16_t, uint16_t) {}
  void drawRoundRect(int16_t, int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void fillRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void drawLine(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  template <typename T>
  size_t print(const T&) { return 0; }
  template <typename T>
  size_t println(const T&) { return 0; }
};

static constexpr uint8_t SSD1306_SWITCHCAPVCC = 0;
static constexpr uint8_t SSD1306_DISPLAYOFF = 0;
static constexpr uint8_t SSD1306_DISPLAYON = 0;
static constexpr uint8_t SSD1306_SETCONTRAST = 0;
static constexpr uint16_t SSD1306_WHITE = 1;
#endif

namespace nhos {

enum class OledMode : uint8_t {
  Off = 0,
  Auto,
  Enabled,
};

enum class PowerAnimation : uint8_t;

class DisplayManager {
 public:
  DisplayManager();

  void begin(const OledConfig& config);
  void apply(const OledConfig& config);
  void startPowerAnimation(PowerAnimation animation);
  void servicePowerAnimation(uint32_t nowMs);
  bool powerAnimationActive() const;
  void sleep();
  void wake();
  void service(uint32_t nowMs, const String& ip, const String& gatewayIp, const ScanHealth& health, uint32_t heapFree, uint32_t heapTotal);
  String statusJson() const;

 private:
  bool configure();
  void renderPowerAnimation(const char* label, uint32_t elapsedMs, uint32_t durationMs);
  bool probeAddress(uint8_t address) const;
  OledMode parseMode(const String& mode) const;
  void renderLiveStatus(const String& ip, const String& gatewayIp, const ScanHealth& health, uint32_t heapFree, uint32_t heapTotal);
  void renderSensorSnapshot(const ScanHealth& health);
  void renderRecordingStatus(const ScanHealth& health);
  String addressString() const;

  Adafruit_SSD1306 display_;
  OledConfig config_;
  bool initialized_ = false;
  bool enabled_ = false;
  bool detected_ = false;
  uint8_t address_ = 0;
  uint32_t lastUpdateMs_ = 0;
  bool sleeping_ = false;
  uint8_t powerAnimation_ = 0;
  uint32_t powerAnimationStartedMs_ = 0;
  String lastError_;
};

}  // namespace nhos
