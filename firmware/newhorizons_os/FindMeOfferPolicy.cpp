#include "FindMeOfferPolicy.h"

namespace nhos {

void FindMeOfferPolicy::onRoundStart(bool attached) {
  if (!attached) {
    missedRounds_ = 0;
  } else if (!answeredThisRound_ && missedRounds_ < 255) {
    ++missedRounds_;
  }
  answeredThisRound_ = false;
}

void FindMeOfferPolicy::onAccepted() {
  missedRounds_ = 0;
  answeredThisRound_ = true;
}

bool FindMeOfferPolicy::shouldAccept(bool attached, bool transferring, bool sameGateway,
                                     bool firstOfRound) const {
  if (!attached) {
    return true;
  }
  return sameGateway || transferring || (currentGatewayLost() && firstOfRound);
}

void FindMeOfferPolicy::reset() {
  missedRounds_ = 0;
  answeredThisRound_ = false;
}

}  // namespace nhos
