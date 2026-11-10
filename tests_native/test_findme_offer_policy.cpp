#include <cassert>
#include <iostream>

#include "FindMeOfferPolicy.h"

int main() {
  using namespace nhos;

  // Not attached: any offer attaches the device.
  {
    FindMeOfferPolicy p;
    p.onRoundStart(false);
    assert(p.shouldAccept(false, false, false, true));
    assert(p.shouldAccept(false, false, false, false));
  }

  // Attached and the current Gateway keeps answering: another host's offer,
  // even first of the round, never steals the device.
  {
    FindMeOfferPolicy p;
    p.onRoundStart(false);
    p.onAccepted();  // attached to A
    for (int round = 0; round < 10; ++round) {
      p.onRoundStart(true);
      assert(!p.shouldAccept(true, false, false, true));  // B first this round
      assert(p.shouldAccept(true, false, true, false));   // A refreshes
      p.onAccepted();
    }
    assert(p.missedRounds() == 0);
  }

  // A transfer with a matching claim is always taken.
  {
    FindMeOfferPolicy p;
    p.onAccepted();
    p.onRoundStart(true);
    assert(p.shouldAccept(true, true, false, false));
  }

  // The current Gateway goes silent: after 3 missed rounds the first offer of
  // a round is taken, but not a later one in that round.
  {
    FindMeOfferPolicy p;
    p.onAccepted();
    p.onRoundStart(true);  // round 1 (A answers)
    p.onAccepted();
    p.onRoundStart(true);  // round 2: A silent
    assert(!p.shouldAccept(true, false, false, true));
    p.onRoundStart(true);  // round 3: A silent (missed 1)
    assert(!p.shouldAccept(true, false, false, true));
    p.onRoundStart(true);  // round 4 (missed 2)
    assert(!p.shouldAccept(true, false, false, true));
    p.onRoundStart(true);  // round 5 (missed 3)
    assert(p.currentGatewayLost());
    assert(!p.shouldAccept(true, false, false, false));
    assert(p.shouldAccept(true, false, false, true));
    p.onAccepted();  // moved to B; streak resets
    assert(!p.currentGatewayLost());
    p.onRoundStart(true);
    assert(!p.shouldAccept(true, false, false, true));
  }

  // A late answer from the current Gateway resets the streak.
  {
    FindMeOfferPolicy p;
    p.onAccepted();
    p.onRoundStart(true);
    p.onRoundStart(true);
    p.onRoundStart(true);
    assert(p.missedRounds() == 2);
    p.onAccepted();
    p.onRoundStart(true);
    assert(p.missedRounds() == 0);
  }

  std::cout << "FindMe offer policy tests passed\n";
  return 0;
}
