#pragma once

#include <stdint.h>

namespace nhos {

// Which FindMe offer a device takes when it is choosing a relay: not attached
// yet, or its relay has gone silent (FindMeOfferPolicy.h). Every offer of the
// round's read window is collected and the best one wins, in this order:
//   1. priority, highest first. A relay's base is 100; the relay that owns the
//      device (it adopted it, or took it over last) offers at 200 or more,
//      and a claim (a takeover in flight) adds 10000.
//   2. upstream_status "online" first: a relay connected to the server.
//   3. RTT, discover sent -> offer received, lowest first (minus held_ms, the
//      time a relay deliberately held its answer back). RTTs within
//      kFindMeRttToleranceMs count as equal...
//   4. ...and then the lighter load wins: fewer served_devices.
//   5. Still equal: the lower RTT.
// Kept free of Arduino types so tests_native can drive it.

// Offers below this version are ignored: the retired Python NH Gateway sent 1.
constexpr int32_t kFindMeMinOfferVersion = 2;
constexpr uint32_t kFindMeRttToleranceMs = 30;

struct FindMeOfferRank {
  int32_t priority = 0;
  bool upstreamOnline = false;
  uint32_t rttMs = 0;
  uint32_t servedDevices = 0;
};

// The RTT an offer is ranked by: arrival minus the discover send, minus what
// the relay said it held the answer back (never below 0).
uint32_t findMeOfferRtt(uint32_t discoverSentMs, uint32_t receivedMs, uint32_t heldMs);

// True when `a` should be taken over `b`.
bool findMeOfferBetter(const FindMeOfferRank& a, const FindMeOfferRank& b);

}  // namespace nhos
