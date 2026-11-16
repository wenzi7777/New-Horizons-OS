#pragma once

#include <stdint.h>

namespace nhos {

// FindMe rounds and which offers an attached device takes. Kept free of
// Arduino types so tests_native can drive it round by round.
//
// Attachment is a lease. A device that is not attached (or whose relay is
// lost, or that is being transferred) *searches*: a broadcast discover every
// search interval (3 s). Once attached it *renews*: every lease interval (5 s)
// a discover goes unicast to its relay, naming it (`current_gateway_id`), and
// every beaconEveryN-th renewal is also broadcast (a beacon) so the relay that
// owns the device hears where it is even when it is not the current one. The
// relay answers each renewal with an offer; kFindMeStickyMissedRounds (3)
// renewals in a row without one and the relay is lost (decided when the next
// renewal would be due: 15-20 s after the relay stopped answering), and the
// device searches again -- still streaming to the old relay until it takes a
// new offer.
//
// Offers while attached: one is taken only when it is
//   (a) from the current relay (it renewing the lease),
//   (b) part of a transfer the device was asked to make (matching claim), or
//   (c) the first offer of a round after the current relay is lost.
// A strictly higher priority from another relay never steals the device on
// its own. Not attached: any offer attaches it.
constexpr uint8_t kFindMeStickyMissedRounds = 3;

enum class FindMeRoundKind : uint8_t {
  Search,  // broadcast, without current_gateway_id
  Renew,   // unicast to the current relay, naming it
  Beacon,  // a renewal that is also broadcast
};

struct FindMeRoundPlan {
  FindMeRoundKind kind = FindMeRoundKind::Search;
  // When the next round is due, from now.
  uint32_t nextInMs = 0;
};

class FindMeOfferPolicy {
 public:
  FindMeOfferPolicy(uint32_t searchIntervalMs = 3000, uint32_t leaseIntervalMs = 5000,
                    uint8_t beaconEveryN = 3)
      : searchIntervalMs_(searchIntervalMs),
        leaseIntervalMs_(leaseIntervalMs),
        beaconEveryN_(beaconEveryN ? beaconEveryN : 1) {}

  // A discover round begins: counts the previous round as missed when the
  // device was attached and its relay did not answer, then says what this
  // round sends and when the next one is due.
  FindMeRoundPlan startRound(bool attached, bool transferring);
  // A new discover round begins (startRound() without the plan).
  void onRoundStart(bool attached);
  // An offer was accepted: the device's relay (old or new) answered this
  // round, so the miss streak resets.
  void onAccepted();
  // A transfer ended without moving: the rounds it spent searching do not
  // count against the relay the device still has, which gets a fresh lease
  // (3 renewals to answer).
  void restartLease() { onAccepted(); }
  bool shouldAccept(bool attached, bool transferring, bool sameGateway,
                    bool firstOfRound) const;
  bool currentGatewayLost() const { return missedRounds_ >= kFindMeStickyMissedRounds; }
  // Whether discovers may name the current relay (renewals, beacons, probe
  // answers): attached, its relay not lost, and not leaving it.
  bool namesCurrentGateway(bool attached, bool transferring) const {
    return attached && !transferring && !currentGatewayLost();
  }
  uint8_t missedRounds() const { return missedRounds_; }
  uint32_t leaseIntervalMs() const { return leaseIntervalMs_; }
  uint32_t searchIntervalMs() const { return searchIntervalMs_; }
  void reset();

 private:
  uint32_t searchIntervalMs_;
  uint32_t leaseIntervalMs_;
  uint8_t beaconEveryN_;
  uint8_t missedRounds_ = 0;
  bool answeredThisRound_ = false;
  // Renewals since the last beacon.
  uint8_t renewalsSinceBeacon_ = 0;
};

}  // namespace nhos
