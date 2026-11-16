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

  // Lease: not attached -> searching every 3 s, never naming a relay.
  {
    FindMeOfferPolicy p(3000, 5000, 3);
    for (int i = 0; i < 5; ++i) {
      const FindMeRoundPlan plan = p.startRound(false, false);
      assert(plan.kind == FindMeRoundKind::Search);
      assert(plan.nextInMs == 3000);
    }
  }

  // Lease: attached and answered -> renew every 5 s, every 3rd a beacon.
  {
    FindMeOfferPolicy p(3000, 5000, 3);
    p.startRound(false, false);
    p.onAccepted();  // attached
    FindMeRoundKind kinds[9];
    for (int i = 0; i < 9; ++i) {
      const FindMeRoundPlan plan = p.startRound(true, false);
      assert(plan.nextInMs == 5000);
      kinds[i] = plan.kind;
      p.onAccepted();  // the relay renews
    }
    for (int i = 0; i < 9; ++i) {
      assert(kinds[i] == ((i % 3 == 2) ? FindMeRoundKind::Beacon : FindMeRoundKind::Renew));
    }
    assert(p.namesCurrentGateway(true, false));
    assert(!p.namesCurrentGateway(true, true));   // transferring
    assert(!p.namesCurrentGateway(false, false));  // not attached
  }

  // Lease: a running transfer searches even while attached.
  {
    FindMeOfferPolicy p(3000, 5000, 3);
    p.onAccepted();
    const FindMeRoundPlan plan = p.startRound(true, true);
    assert(plan.kind == FindMeRoundKind::Search);
    assert(plan.nextInMs == 3000);
  }

  // Lease: 3 unanswered renewals -> lost, then searching; an accept (old or
  // new relay) resets it to renewing.
  {
    FindMeOfferPolicy p(3000, 5000, 3);
    p.onAccepted();
    assert(p.startRound(true, false).kind != FindMeRoundKind::Search);  // renewal 1, unanswered
    assert(p.startRound(true, false).kind != FindMeRoundKind::Search);  // renewal 2 (missed 1)
    assert(p.startRound(true, false).kind != FindMeRoundKind::Search);  // renewal 3 (missed 2)
    assert(!p.currentGatewayLost());
    FindMeRoundPlan plan = p.startRound(true, false);  // missed 3: lost
    assert(p.currentGatewayLost());
    assert(plan.kind == FindMeRoundKind::Search);
    assert(plan.nextInMs == 3000);
    assert(!p.namesCurrentGateway(true, false));
    plan = p.startRound(true, false);
    assert(plan.kind == FindMeRoundKind::Search);
    assert(p.shouldAccept(true, false, false, true));
    p.onAccepted();
    assert(!p.currentGatewayLost());
    plan = p.startRound(true, false);
    assert(plan.kind != FindMeRoundKind::Search);
    assert(plan.nextInMs == 5000);
  }

  // Lease: a late answer before the third miss keeps it renewing.
  {
    FindMeOfferPolicy p(3000, 5000, 3);
    p.onAccepted();
    p.startRound(true, false);
    p.startRound(true, false);
    p.startRound(true, false);  // missed 2
    p.onAccepted();
    for (int i = 0; i < 3; ++i) {
      assert(p.startRound(true, false).kind != FindMeRoundKind::Search);
    }
    assert(!p.currentGatewayLost());
  }

  // Lease: a transfer that times out (many unanswered search rounds) gives
  // the relay the device still has a fresh lease.
  {
    FindMeOfferPolicy p(3000, 5000, 3);
    p.onAccepted();
    for (int i = 0; i < 10; ++i) {
      assert(p.startRound(true, true).kind == FindMeRoundKind::Search);
    }
    assert(p.currentGatewayLost());
    p.restartLease();
    assert(!p.currentGatewayLost());
    assert(p.startRound(true, false).kind != FindMeRoundKind::Search);
  }

  // Lease: beaconEveryN = 1 makes every renewal a beacon.
  {
    FindMeOfferPolicy p(3000, 5000, 1);
    p.onAccepted();
    for (int i = 0; i < 3; ++i) {
      assert(p.startRound(true, false).kind == FindMeRoundKind::Beacon);
      p.onAccepted();
    }
  }

  std::cout << "FindMe offer policy tests passed\n";
  return 0;
}
