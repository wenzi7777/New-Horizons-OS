#pragma once

#include <Arduino.h>

#include "BoardConfig.h"
#include "DeviceConfig.h"
#include "LedController.h"
#include "MatrixScanner.h"

#if NHOS_BOARD_HAS_EXT_LED
#include <Adafruit_NeoPixel.h>
#else
using neoPixelType = uint16_t;

class Adafruit_NeoPixel {
 public:
  Adafruit_NeoPixel(uint16_t, int16_t, neoPixelType) {}

  void begin() {}
  void clear() {}
  void show() {}
  void setPixelColor(uint16_t, uint32_t) {}
  uint32_t Color(uint8_t, uint8_t, uint8_t) const { return 0; }
};

static constexpr neoPixelType NEO_GRB = 0;
static constexpr neoPixelType NEO_KHZ800 = 0;
#endif

namespace nhos {

enum class PowerAnimation : uint8_t;

class ExternalLedController {
 public:
  ExternalLedController();

  void begin(const ExternalLedConfig& config);
  void apply(const ExternalLedConfig& config);
  void identify();
  void startPowerAnimation(PowerAnimation animation);
  void servicePowerAnimation(uint32_t nowMs);
  bool powerAnimationActive() const;
  void sleep();
  void wake();
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
  bool sleeping_ = false;
  uint8_t powerAnimation_ = 0;
  uint32_t powerAnimationStartedMs_ = 0;
  String activePreset_ = "off";
  uint32_t identifyStartedMs_ = 0;
  uint32_t lastShowMs_ = 0;
  String lastError_ = "";
};

}  // namespace nhos
