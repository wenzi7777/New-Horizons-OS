#include "FindMeOfferPolicy.h"

namespace nhos {

FindMeRoundPlan FindMeOfferPolicy::startRound(bool attached, bool transferring) {
  onRoundStart(attached);
  FindMeRoundPlan plan;
  if (!namesCurrentGateway(attached, transferring)) {
    plan.kind = FindMeRoundKind::Search;
    plan.nextInMs = searchIntervalMs_;
    return plan;
  }
  ++renewalsSinceBeacon_;
  if (renewalsSinceBeacon_ >= beaconEveryN_) {
    renewalsSinceBeacon_ = 0;
    plan.kind = FindMeRoundKind::Beacon;
  } else {
    plan.kind = FindMeRoundKind::Renew;
  }
  plan.nextInMs = leaseIntervalMs_;
  return plan;
}

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
  renewalsSinceBeacon_ = 0;
}

}  // namespace nhos
