#pragma once

#include <stdint.h>

namespace nhos {

// How busy one running app has been lately (v2.1.0): the device's answer to
// "which app is slowing things down?", the way an activity monitor answers it.
//
// AppManager charges every delivery's measured time (I2C / UART waits
// included -- they are inside the timed call) and the slice it was given, and
// settles once a second. From each settled second come exponentially weighted
// averages over ~10 s and ~60 s, like a Unix load average: fixed size, no
// history kept on the device. The Desktop draws the history from its polls.
//
// Pure C++, so the host tests drive it directly.
struct AppLoad {
  enum Level : uint8_t { Low = 0, Medium = 1, High = 2, Overloaded = 3 };

  // Seconds with a suspension or a skipped frame stay "recent" this long.
  static constexpr uint8_t kPressureHoldS = 10;

  // --- accumulating, between settles ---
  uint32_t windowUsedUs = 0;
  uint32_t windowAllowedUs = 0;

  // --- settled ---
  uint16_t cpu1 = 0;       // permille of wall time, last second
  uint16_t cpu10 = 0;      // permille, ~10 s average
  uint16_t cpu60 = 0;      // permille, ~60 s average
  uint16_t budget10 = 0;   // permille of its own slices, ~10 s average
  uint8_t load = 0;        // 0..100, what the operator is shown
  uint8_t level = Low;
  uint8_t pressureAgeS = 0xFF;  // seconds since the last suspension / skip
  bool primed = false;

  void charge(uint32_t usedUs, uint32_t allowedUs) {
    windowUsedUs += usedUs;
    windowAllowedUs += allowedUs;
  }

  // Closes the window. `wallUs` is how long it lasted; `pressureEvents` how
  // many suspensions and skipped frames happened in it.
  void settle(uint32_t wallUs, uint32_t pressureEvents);

  // Fixed-point state behind cpu10 / cpu60 / budget10 (value << 4).
  uint32_t cpu10Fx = 0;
  uint32_t cpu60Fx = 0;
  uint32_t budget10Fx = 0;

  static const char* levelName(uint8_t level);
};

}  // namespace nhos
