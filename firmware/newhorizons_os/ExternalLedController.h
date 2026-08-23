#pragma once

#include <Arduino.h>

#include "BoardConfig.h"
#include "BoardPins.h"
#include "DeviceConfig.h"
#include "LedController.h"
#include "MatrixScanner.h"

#if NHOS_BOARD_HAS_EXT_LED && !defined(NHOS_BOARD_V15F)
#include <Adafruit_NeoPixel.h>
#endif

namespace nhos {

enum class PowerAnimation : uint8_t;

// Runtime signals fed into the external LED service each loop. The single
// systemSignal is already prioritized by updateLedState(); the remaining
// fields drive the per-pixel / metering presets.
struct ExternalLedInputs {
  LedSignal systemSignal = LedSignal::Online;
  bool wifiConnected = false;
  bool wifiBusy = false;   // WiFi setup or connecting in progress
  bool hasGateway = false;
  float pressure01 = 0.0f;  // current matrix peak, normalized 0..1
  bool calibrating = false;
};

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
  void service(uint32_t nowMs, const ScanHealth& health, const ExternalLedInputs& inputs);
  String statusJson() const;

 private:
  void clear();
  void showIdentify(uint32_t elapsedMs, uint32_t nowMs);
  void showSolid(LedColor color, uint32_t nowMs);
  void showSegments(const LedColor* colors, size_t count, uint32_t nowMs);
  void showMeter(uint8_t litCount, LedColor low, LedColor high, uint32_t nowMs);
  void showPulse(LedColor color, uint8_t flashes, uint16_t intervalMs, uint16_t onMs, uint16_t gapMs, uint32_t nowMs);
  void renderSystemStatus(const ExternalLedInputs& inputs, bool recentWarning, uint32_t nowMs);
  void setPixelColor(uint16_t index, uint32_t rgb);
  void clearPixels();
  bool showPixels();
  static LedColor markerColor(const String& name);
  uint32_t color(LedColor color) const;
  uint8_t scale(uint8_t value) const;

  // v1.5.F's FPC 3.0 uses WS2812B-2020-V6 and needs a dedicated RMT channel:
  // Adafruit_NeoPixel's ESP-IDF v5 backend owns one static channel for every
  // instance, which makes GPIO16 and the two-pixel GPIO46 chain interfere.
#if defined(NHOS_BOARD_V15F)
  static constexpr size_t kPixelByteCount =
      NHOS_BOARD_EXTERNAL_LED_COUNT * 3U;
  uint8_t pixelBytes_[kPixelByteCount] = {};
#elif NHOS_BOARD_HAS_EXT_LED
  // v1.0.F FPC 2.0 keeps its established Adafruit WS2812 path and timing.
  Adafruit_NeoPixel pixels_{kExternalLedCount, kExternalLedPin,
                            NEO_GRB + NEO_KHZ800};
#endif
  ExternalLedConfig config_;
  bool initialized_ = false;
  bool rmtReady_ = false;
  bool sleeping_ = false;
  uint8_t powerAnimation_ = 0;
  uint32_t powerAnimationStartedMs_ = 0;
  String activePreset_ = "off";
  uint32_t identifyStartedMs_ = 0;
  uint32_t lastShowMs_ = 0;
  String lastError_ = "";

  // Streaming activity tracked from ScanHealth deltas (current, not cumulative).
  bool streamCountersInit_ = false;
  uint32_t lastUdpSentFrames_ = 0;
  uint32_t lastStreamFailTotal_ = 0;
  uint32_t lastStreamFrameMs_ = 0;
  uint32_t lastStreamWarnMs_ = 0;
};

}  // namespace nhos
