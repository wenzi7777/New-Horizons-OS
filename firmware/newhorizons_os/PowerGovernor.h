#pragma once

#include <Arduino.h>

namespace nhos {

enum class PowerProfile : uint8_t {
  Performance = 0,  // always full speed; today's behaviour
  Balanced = 1,     // drop to 160 MHz when the scanner is idle
  PowerSave = 2,    // drop to 80 MHz when the scanner is idle
};

// Sole owner of CPU frequency.
//
// The two setCpuFrequencyMhz() calls that used to sit inline in the sketch's
// soft-off path are now decisions this makes, so there is one place that
// knows what clock the device should be running at and why.
//
// Hard constraint, and the reason this is deliberately less aggressive than a
// general-purpose governor: THE CLOCK IS NEVER LOWERED WHILE THE MATRIX
// SCANNER IS RUNNING. The scan path is delayMicroseconds(settleUs) followed
// by analogRead() per cell; the delay is frequency-compensated but the loop
// overhead around it is not, so downclocking mid-session would shift sample
// timing -- and silently changing the measurements on a research instrument
// to save power is not a trade this device gets to make. Idle here means the
// scanner is genuinely stopped: maintenance mode, no layout configured, or
// the runtime parked in soft-off.
class PowerGovernor {
 public:
  static constexpr uint32_t kFullMhz = 240;
  static constexpr uint32_t kBalancedIdleMhz = 160;
  // WiFi needs >=80MHz, so this is the floor regardless of profile.
  static constexpr uint32_t kPowerSaveIdleMhz = 80;

  void begin(PowerProfile profile);
  void setProfile(PowerProfile profile);
  PowerProfile profile() const { return profile_; }

  // scannerActive: the scanner is producing frames right now.
  // runtimeActive: false while parked in soft-off.
  void service(bool scannerActive, bool runtimeActive);

  uint32_t currentMhz() const { return currentMhz_; }
  uint32_t transitions() const { return transitions_; }
  static const char* profileName(PowerProfile profile);
  static PowerProfile profileFromName(const char* name);
  String statusJson() const;

 private:
  uint32_t targetMhz(bool scannerActive, bool runtimeActive) const;
  void applyMhz(uint32_t mhz);

  PowerProfile profile_ = PowerProfile::Performance;
  uint32_t currentMhz_ = kFullMhz;
  uint32_t transitions_ = 0;
};

}  // namespace nhos
