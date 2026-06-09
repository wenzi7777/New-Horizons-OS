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
static constexpr LedColor Boot{0, 0, 16};
static constexpr LedColor WifiSetup{32, 9, 0};
static constexpr LedColor WifiConnecting{24, 18, 0};
static constexpr LedColor FindMePending{0, 18, 24};
static constexpr LedColor Online{0, 24, 0};
static constexpr LedColor Maintenance{32, 18, 0};
static constexpr LedColor SafeMode{32, 0, 32};
static constexpr LedColor Ota{0, 18, 24};
static constexpr LedColor Error{32, 0, 0};
static constexpr LedColor Warning{28, 18, 0};
static constexpr LedColor White{24, 24, 24};
static constexpr LedColor ChargeDone{0x39, 0xc5, 0xbb};
}  // namespace LedPalette

enum class LedSignal : uint8_t {
  Off = 0,
  Boot,
  WifiSetup,
  WifiConnecting,
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
  void setStatus(LedColor color);
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
  LedColor scaleColor(LedColor color, uint8_t level) const;
  void writePixel(uint8_t pin, LedColor color);

  LedSignal baseSignal_ = LedSignal::Boot;
  LedSignal eventSignal_ = LedSignal::Off;
  uint32_t eventStartedMs_ = 0;
  LedColor current_{255, 255, 255};
};

}  // namespace nhos
