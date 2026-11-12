#include "FindMeOfferSelect.h"

namespace nhos {

uint32_t findMeOfferRtt(uint32_t discoverSentMs, uint32_t receivedMs, uint32_t heldMs) {
  const uint32_t elapsed = receivedMs - discoverSentMs;  // wraps like millis()
  return elapsed > heldMs ? elapsed - heldMs : 0;
}

bool findMeOfferBetter(const FindMeOfferRank& a, const FindMeOfferRank& b) {
  if (a.priority != b.priority) {
    return a.priority > b.priority;
  }
  if (a.upstreamOnline != b.upstreamOnline) {
    return a.upstreamOnline;
  }
  const uint32_t diff = a.rttMs > b.rttMs ? a.rttMs - b.rttMs : b.rttMs - a.rttMs;
  if (diff > kFindMeRttToleranceMs) {
    return a.rttMs < b.rttMs;
  }
  if (a.servedDevices != b.servedDevices) {
    return a.servedDevices < b.servedDevices;
  }
  return a.rttMs < b.rttMs;
}

}  // namespace nhos
