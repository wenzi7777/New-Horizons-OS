#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>

#include "DeviceConfig.h"
#include "MatrixScanner.h"

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
