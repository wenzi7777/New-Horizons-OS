#pragma once

#include <cstdint>

namespace nhos {

// Maps the MAX17048's raw state of charge onto "100% = the charger said full".
//
// The gauge has no coulomb counter and never learns that the charger
// terminated: its 100% is the OCV point of a generic model, so a real full
// charge reads anywhere from ~90% to well over 100%. The BQ25180's
// charge_done (CV at 4.2 V, current down to the termination threshold) is
// the one trustworthy "full", so the raw reading at that moment becomes the
// divisor for everything reported afterwards.
//
// Batteries are swappable and the learned "full" is never tied to one
// (v1.5.F has no usable BAT_ID; v2.0.F reads an ID only once, at boot, and
// still accepts any cell on the JST), so nothing here is tied to a battery: the learned value lives in RAM only and reset() drops
// it on every sign that the cell may have changed. Before anything is
// learned the output is the raw reading clamped to 100%.
class BatterySocScaler {
 public:
  static constexpr uint16_t kNominalFullCentiPercent = 10000;
  // A termination reading outside this window means the gauge itself is
  // off (fresh POR estimate, bad model); learning from it would make
  // things worse, so it is ignored.
  static constexpr uint16_t kMinFullCentiPercent = 9000;
  static constexpr uint16_t kMaxFullCentiPercent = 11000;

  void reset();

  // Feeds one gauge sample; returns the state of charge to report, in
  // centi-percent, always within 0..10000.
  uint16_t apply(uint16_t rawCentiPercent, bool chargeDone, bool chargerPresent);

  bool learned() const { return learned_; }
  uint16_t fullCentiPercent() const { return fullCentiPercent_; }
  bool fullLatched() const { return fullLatched_; }

 private:
  uint16_t scale(uint16_t rawCentiPercent) const;

  bool learned_ = false;
  uint16_t fullCentiPercent_ = kNominalFullCentiPercent;
  // Learning needs a real charging -> charge_done transition: a sample taken
  // after reset() while the charger already reports done may come from a
  // cell that has been relaxing for hours.
  bool armed_ = false;
  // Once the charger reports done, show 100% until the charger goes away,
  // so the BQ25180's top-up recharge cycles do not make the number wobble.
  bool fullLatched_ = false;
};

}  // namespace nhos
