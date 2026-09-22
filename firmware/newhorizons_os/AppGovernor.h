#pragma once

#include <Arduino.h>

namespace nhos {

class AppManager;
struct ScanHealth;

// Decides how much time the apps may spend, by watching whether the SCANNER is
// meeting its deadlines.
//
// The priority is not negotiable: the device exists to sample the matrix, and
// apps are value added on top of that. So the apps' allowance is not a fixed
// configuration value -- it is whatever is left over, and it goes to zero (all
// apps suspended) rather than let the scan miss frames.
//
// Additive-increase / multiplicative-decrease, the same shape as congestion
// control and for the same reason: giving ground has to be fast, taking it
// back has to be slow, or the loop oscillates and the operator sees apps
// flapping between suspended and running.
class AppGovernor {
 public:
  // Ceiling, as a share of one dispatch period. 250 permille of a 120Hz frame
  // is ~2.1ms, which is generous for a dozen scalar ops and still leaves the
  // scan its margin.
  static constexpr uint16_t kDefaultCeilingPermille = 250;
  static constexpr uint32_t kWindowMs = 1000;
  // Give ground in halves, take it back a fifth of the ceiling at a time: five
  // clean windows to recover what one bad window costs.
  static constexpr uint8_t kDecreaseShift = 1;
  static constexpr uint8_t kIncreaseSteps = 5;
  // Once suspended, stay suspended for at least this long even if the scan
  // recovers immediately -- otherwise a marginal app resumes straight into the
  // same overload and flaps.
  static constexpr uint32_t kMinSuspendMs = 5000;
  // Below this share of the frame period, the apps are not worth blaming:
  // taking their time away would give the scan almost nothing back, while
  // disabling them for a fault they had little part in.
  static constexpr uint16_t kMinAppSharePermille = 20;

  void begin(AppManager* apps) { apps_ = apps; }
  void setCeilingPermille(uint16_t permille);
  uint16_t ceilingPermille() const { return ceilingPermille_; }

  // Call once per apps tick. `health` is the authority on missed sampling
  // deadlines -- NOT Scheduler::busyPermille(), which legitimately reads high
  // on any tick that performs a scan and would fire constantly.
  void update(const ScanHealth& health, uint32_t nowMs);

  uint32_t allowanceUs() const { return allowanceUs_; }
  uint32_t ceilingUs() const { return ceilingUs_; }
  uint32_t shrinkEvents() const { return shrinks_; }
  String statusJson() const;

 private:
  void apply(uint32_t allowanceUs, uint32_t nowMs);

  AppManager* apps_ = nullptr;
  uint16_t ceilingPermille_ = kDefaultCeilingPermille;
  uint32_t ceilingUs_ = 0;
  uint32_t allowanceUs_ = 0;
  uint32_t windowStartedMs_ = 0;
  uint32_t suspendedAtMs_ = 0;
  uint32_t lastOverrunFrames_ = 0;
  uint32_t shrinks_ = 0;
  uint32_t grows_ = 0;
  bool primed_ = false;
};

}  // namespace nhos
