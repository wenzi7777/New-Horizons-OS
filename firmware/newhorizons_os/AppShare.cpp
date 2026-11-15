#include "AppShare.h"

namespace nhos {

uint32_t AppShare::lendable(uint32_t baseUs, uint32_t averageUs, bool hungry) {
  if (hungry) return 0;
  const uint64_t keep = static_cast<uint64_t>(averageUs) * (100U + kHeadroomPercent) / 100U;
  return keep >= baseUs ? 0 : static_cast<uint32_t>(baseUs - keep);
}

uint32_t AppShare::loan(uint32_t poolUs, uint16_t borrowers, uint32_t baseUs, uint32_t capUs) {
  if (borrowers == 0 || capUs <= baseUs) return 0;
  const uint32_t each = poolUs / borrowers;
  const uint32_t room = capUs - baseUs;
  return each < room ? each : room;
}

uint32_t AppShare::lent(uint32_t lendableUs, uint32_t borrowedTotalUs, uint32_t poolUs) {
  if (poolUs == 0 || borrowedTotalUs == 0) return 0;
  if (borrowedTotalUs >= poolUs) return lendableUs;
  // Rounded up, so the lenders always give at least what was handed out.
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(lendableUs) * borrowedTotalUs + poolUs - 1) / poolUs);
}

}  // namespace nhos
