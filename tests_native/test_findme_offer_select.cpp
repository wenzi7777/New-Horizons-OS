#include <cassert>
#include <iostream>

#include "FindMeOfferSelect.h"

using nhos::FindMeOfferRank;
using nhos::findMeOfferBetter;
using nhos::findMeOfferRtt;

static FindMeOfferRank rank(int32_t priority, bool online, uint32_t rtt, uint32_t served) {
  FindMeOfferRank r;
  r.priority = priority;
  r.upstreamOnline = online;
  r.rttMs = rtt;
  r.servedDevices = served;
  return r;
}

int main() {
  // 1. The owner (200) beats any fallback (100), however slow or busy it is.
  assert(findMeOfferBetter(rank(200, false, 500, 30), rank(100, true, 5, 0)));
  assert(!findMeOfferBetter(rank(100, true, 5, 0), rank(200, false, 500, 30)));
  // A claim (+10000) beats the owner.
  assert(findMeOfferBetter(rank(10100, false, 300, 9), rank(200, true, 3, 0)));

  // 2. Same priority: a relay connected to the server first.
  assert(findMeOfferBetter(rank(100, true, 200, 10), rank(100, false, 5, 0)));

  // 3. Then RTT, when it differs by more than the tolerance.
  assert(findMeOfferBetter(rank(100, true, 10, 9), rank(100, true, 80, 0)));

  // 4. RTTs within the tolerance are equal: the lighter load wins.
  assert(findMeOfferBetter(rank(100, true, 30, 1), rank(100, true, 10, 4)));
  assert(!findMeOfferBetter(rank(100, true, 10, 4), rank(100, true, 30, 1)));

  // 5. All equal but the RTT: the lower one; identical never "better".
  assert(findMeOfferBetter(rank(100, true, 10, 2), rank(100, true, 20, 2)));
  assert(!findMeOfferBetter(rank(100, true, 10, 2), rank(100, true, 10, 2)));

  // RTT: arrival minus send, minus what the relay held back, never negative,
  // across a millis() wrap.
  assert(findMeOfferRtt(1000, 1300, 250) == 50);
  assert(findMeOfferRtt(1000, 1100, 250) == 0);
  assert(findMeOfferRtt(0xFFFFFF00u, 0x00000040u, 0) == 0x140);

  std::cout << "test_findme_offer_select: ok\n";
  return 0;
}
