#pragma once

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#include "DeviceConfig.h"
#include "LedController.h"
#include "MatrixScanner.h"

namespace nhos {

class ExternalLedController {
 public:
  ExternalLedController();

  void begin(const ExternalLedConfig& config);
  void apply(const ExternalLedConfig& config);
  void identify();
  void service(uint32_t nowMs, const ScanHealth& health, LedSignal systemSignal);
  String statusJson() const;

 private:
  void clear();
  void showIdentify(uint32_t elapsedMs, uint32_t nowMs);
  void showSolid(LedColor color, uint32_t nowMs);
  void showPulse(LedColor color, uint8_t flashes, uint16_t intervalMs, uint16_t onMs, uint16_t gapMs, uint32_t nowMs);
  uint32_t color(LedColor color) const;
  uint8_t scale(uint8_t value) const;

  Adafruit_NeoPixel pixels_;
  ExternalLedConfig config_;
  bool initialized_ = false;
  String activePreset_ = "off";
  uint32_t identifyStartedMs_ = 0;
  uint32_t lastShowMs_ = 0;
  String lastError_ = "";
};

}  // namespace nhos
