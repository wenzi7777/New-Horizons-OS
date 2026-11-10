#pragma once

#include <stdint.h>

namespace nhos {

// Which FindMe offers an attached device takes. Kept free of Arduino types so
// tests_native can drive it round by round.
//
// The rule: a device that is attached sticks to its Gateway. Before, every
// discover round took the first offer in the read window even when already
// attached, so two hosts that both offered the device (NH Studio and NH
// Pocket on one LAN) made it switch on reply timing alone. Now, once
// attached, an offer is taken only when it is
//   (a) from the current Gateway (it refreshing itself),
//   (b) part of a transfer the device was asked to make (matching claim), or
//   (c) the first offer of a round after the current Gateway has not answered
//       for kFindMeStickyMissedRounds rounds in a row (~9 s) -- it is gone.
// A strictly higher priority from another Gateway no longer steals the device
// on its own. Not attached: unchanged, any offer attaches it (and from then
// on it is attached, so the first one wins).
constexpr uint8_t kFindMeStickyMissedRounds = 3;

class FindMeOfferPolicy {
 public:
  // A new discover round begins. Counts the previous round as missed when the
  // device was attached and its Gateway did not offer during it.
  void onRoundStart(bool attached);
  // An offer was accepted: the device's Gateway (old or new) answered this
  // round, so the miss streak resets.
  void onAccepted();
  bool shouldAccept(bool attached, bool transferring, bool sameGateway,
                    bool firstOfRound) const;
  bool currentGatewayLost() const { return missedRounds_ >= kFindMeStickyMissedRounds; }
  uint8_t missedRounds() const { return missedRounds_; }
  void reset();

 private:
  uint8_t missedRounds_ = 0;
  bool answeredThisRound_ = false;
};

}  // namespace nhos
