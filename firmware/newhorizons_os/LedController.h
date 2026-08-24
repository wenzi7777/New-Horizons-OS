#pragma once

#include <Arduino.h>

namespace nhos {

struct LedColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

namespace LedPalette {
static constexpr LedColor Off{0, 0, 0};
static constexpr LedColor Boot{0, 0, 28};
static constexpr LedColor WifiSetup{32, 9, 0};
// Direct transport searches for a Gateway in blue. ESP-NOW searches for a
// Hub in orange, so an operator can distinguish the two at a glance.
static constexpr LedColor WifiConnecting{0, 20, 128};
static constexpr LedColor HubSearching{80, 30, 0};
static constexpr LedColor FindMePending{0, 20, 128};
static constexpr LedColor Online{0, 80, 0};
static constexpr LedColor Maintenance{32, 18, 0};
static constexpr LedColor SafeMode{32, 0, 32};
static constexpr LedColor Ota{0, 18, 24};
static constexpr LedColor Error{160, 0, 0};
static constexpr LedColor Warning{28, 18, 0};
static constexpr LedColor White{24, 24, 24};
// Command acknowledgement and completion must remain visually distinct from
// the normal online-green base state. WS2812B is GRB on the wire but accepts
// these logical RGB values through Adafruit_NeoPixel's NEO_GRB configuration.
static constexpr LedColor CommandReceived{16, 48, 180};
static constexpr LedColor CommandSuccess{0, 160, 90};
static constexpr LedColor ChargeDone{0x39, 0xc5, 0xbb};
}  // namespace LedPalette

enum class LedSignal : uint8_t {
  Off = 0,
  Boot,
  WifiSetup,
  WifiConnecting,
  // New Horizons Direct (ESP-NOW/Hub transport): not yet paired with the
  // Hub (still channel-scanning/sending HELLO). Same color/pattern as
  // WifiConnecting (both mean "still trying to establish connectivity"),
  // kept as a distinct value rather than reusing WifiConnecting so an
  // ESP-NOW device's logs/state aren't misleadingly labeled "WiFi" --
  // mirrors the existing precedent of UplinkDegraded below (Hub-only, but
  // still lives in this shared enum rather than a per-sketch copy).
  EspNowConnecting,
  FindMePending,
  Online,
  Maintenance,
  SafeMode,
  OtaActive,
  OtaSuccess,
  OtaError,
  Error,
  ScanWarning,
  RamDanger,
  ChargingOrMissing,
  ChargeDone,
  SoftOffTransition,
  SoftOffCharging,
  SoftOffChargeDone,
  SoftOffChargeIdle,
  PowerTransitionShutdown,
  PowerTransitionWake,
  CommandReceived,
  CommandSuccess,
  CommandFailed,
  ActionButtonIdentify,
  // New Horizons Hub only (newhorizons_hub/): uplink to the Desktop
  // Backend is down but the Hub is still polling/serving its paired
  // ESP-NOW devices locally -- distinct from WifiConnecting (no Wi-Fi at
  // all) and Error (unrecoverable). Declared here rather than in a
  // Hub-only copy of this enum so the shared LedController.cpp pattern
  // table (mirrored into newhorizons_hub/) stays a single source of
  // truth for both sketches.
  UplinkDegraded,
};

enum class PatternMode : uint8_t {
  Off = 0,
  Solid,
  Breathe,
  BlinkBurst,
  AlternateBurst,
};

class LedController {
 public:
  void begin();
  void service(uint32_t nowMs);
  void setSignal(LedSignal signal);
  void showEvent(LedSignal signal);
  void setBrightness(float brightness);
  void setStatus(LedColor color);
  void setBatteryStatus(bool sampleValid, uint16_t socCentiPercent, bool charging,
                        uint8_t lowBatteryThresholdPercent = 10);
  void pulse(LedColor color, uint16_t delayMs);

 private:
  struct Pattern {
    PatternMode mode;
    LedColor color;
    LedColor alternate;
    uint16_t intervalMs;
    uint16_t onMs;
    uint16_t gapMs;
    uint8_t flashes;
    uint16_t eventDurationMs;
    uint8_t minLevel;
    uint8_t maxLevel;
  };

  Pattern patternFor(LedSignal signal) const;
  LedColor colorFor(LedSignal signal, uint32_t nowMs) const;
  LedColor batteryColorFor(uint32_t nowMs) const;
  LedColor scaleColor(LedColor color, uint8_t level) const;
  LedColor applyBrightness(LedColor color) const;
  void enqueueEvent(LedSignal signal);
  bool startNextEvent(uint32_t nowMs);
  void writeStatusPixels(LedColor system, LedColor battery);
  void writePixel(uint8_t pin, LedColor color);

  static constexpr uint32_t kBootSolidDurationMs = 3000;
  static constexpr uint8_t kPendingEventCapacity = 4;
  LedSignal baseSignal_ = LedSignal::Boot;
  LedSignal eventSignal_ = LedSignal::Off;
  uint32_t eventStartedMs_ = 0;
  uint32_t bootStartedMs_ = 0;
  LedSignal pendingEvents_[kPendingEventCapacity] = {};
  uint8_t pendingEventHead_ = 0;
  uint8_t pendingEventTail_ = 0;
  uint8_t pendingEventCount_ = 0;
  float brightness_ = 0.30f;
  LedColor currentSystem_{255, 255, 255};
  LedColor currentBattery_{0, 0, 0};
  bool batterySampleValid_ = false;
  uint16_t batterySocCentiPercent_ = 0;
  bool batteryCharging_ = false;
  uint8_t lowBatteryThresholdPercent_ = 10;
};

}  // namespace nhos
