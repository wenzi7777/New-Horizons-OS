#include "BatterySocScaler.h"

namespace nhos {

void BatterySocScaler::reset() {
  learned_ = false;
  fullCentiPercent_ = kNominalFullCentiPercent;
  armed_ = false;
  fullLatched_ = false;
}

uint16_t BatterySocScaler::apply(uint16_t rawCentiPercent, bool chargeDone,
                                 bool chargerPresent) {
  if (!chargerPresent) {
    fullLatched_ = false;
  }

  if (!chargeDone) {
    armed_ = true;
  } else if (armed_) {
    armed_ = false;
    if (rawCentiPercent >= kMinFullCentiPercent &&
        rawCentiPercent <= kMaxFullCentiPercent) {
      if (learned_) {
        // Average with the previous termination so one odd reading moves
        // the scale only halfway.
        fullCentiPercent_ = static_cast<uint16_t>(
            (static_cast<uint32_t>(fullCentiPercent_) + rawCentiPercent + 1U) / 2U);
      } else {
        fullCentiPercent_ = rawCentiPercent;
        learned_ = true;
      }
    }
  }

  if (chargeDone && chargerPresent) {
    fullLatched_ = true;
  }
  if (fullLatched_) {
    return kNominalFullCentiPercent;
  }
  return scale(rawCentiPercent);
}

uint16_t BatterySocScaler::scale(uint16_t rawCentiPercent) const {
  const uint32_t scaled =
      (static_cast<uint32_t>(rawCentiPercent) * kNominalFullCentiPercent +
       fullCentiPercent_ / 2U) /
      fullCentiPercent_;
  return scaled > kNominalFullCentiPercent
             ? kNominalFullCentiPercent
             : static_cast<uint16_t>(scaled);
}

}  // namespace nhos
