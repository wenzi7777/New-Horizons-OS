#pragma once

#include <stdint.h>

namespace nhos {

// How the apps' time is divided (v2.1.0): a guaranteed share for everyone,
// and spare time lent to the apps that asked for more.
//
//  * base: the total the governor allows per delivery, split evenly among the
//    running apps. Every app gets it whenever it needs it.
//  * cap: no single delivery may take more than kCapPercent of the total
//    (all of it when only one app runs), so one app cannot hold the loop.
//  * lending, once a second: an app that used well under its base (with
//    kHeadroomPercent to spare) lends the rest; an app that declared `burst`
//    and kept running out of slice borrows it, split evenly, up to the cap.
//    Lenders give up only what is actually borrowed, so the shares never add
//    up to more than the total.
//  * nothing is promised: a lender that starts needing its time stops lending
//    at the next settle, and any change in the total (a new app, the governor
//    cutting back) drops every loan at once.
//
// Pure arithmetic, so the host tests drive it directly.
struct AppShare {
  static constexpr uint8_t kCapPercent = 50;
  static constexpr uint8_t kHeadroomPercent = 25;
  // An app using this much of its allocation counts as needing it all.
  static constexpr uint8_t kHungryPercent = 90;

  static uint32_t base(uint32_t totalUs, uint16_t running) {
    return running == 0 ? 0 : totalUs / running;
  }
  static uint32_t cap(uint32_t totalUs, uint16_t running) {
    return running <= 1 ? totalUs : totalUs / 100U * kCapPercent;
  }
  // What an app averaging `averageUs` per delivery can spare of `baseUs`.
  static uint32_t lendable(uint32_t baseUs, uint32_t averageUs, bool hungry);
  // Whether an app needs more than it has: it was suspended or skipped frames,
  // or used nearly all of its allocation.
  static bool hungry(uint32_t averageUs, uint32_t allocatedUs, bool pressured) {
    return pressured ||
           (allocatedUs != 0 && averageUs >= allocatedUs / 100U * kHungryPercent);
  }
  // What each of `borrowers` gets from a pool of `poolUs`, given that none may
  // pass `capUs` and each already holds `baseUs`.
  static uint32_t loan(uint32_t poolUs, uint16_t borrowers, uint32_t baseUs, uint32_t capUs);
  // What one lender gives up: its share of what was actually borrowed.
  static uint32_t lent(uint32_t lendableUs, uint32_t borrowedTotalUs, uint32_t poolUs);
};

}  // namespace nhos
