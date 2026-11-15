#include "AppLoad.h"

namespace nhos {

namespace {

uint32_t permille(uint32_t part, uint32_t whole) {
  if (whole == 0) return 0;
  const uint64_t value = static_cast<uint64_t>(part) * 1000U / whole;
  return value > 1000U ? 1000U : static_cast<uint32_t>(value);
}

// One step of an exponentially weighted average with time constant `span`
// seconds, in value << 4 fixed point.
uint32_t ewma(uint32_t averageFx, uint32_t sample, uint32_t span) {
  const int64_t target = static_cast<int64_t>(sample) << 4;
  const int64_t current = static_cast<int64_t>(averageFx);
  return static_cast<uint32_t>(current + (target - current) / static_cast<int64_t>(span));
}

}  // namespace

void AppLoad::settle(uint32_t wallUs, uint32_t pressureEvents) {
  const uint32_t cpuSample = permille(windowUsedUs, wallUs);
  // No delivery in the window (nothing it subscribes to happened): idle, not
  // unknown -- 0 of nothing.
  const uint32_t budgetSample = permille(windowUsedUs, windowAllowedUs);
  windowUsedUs = 0;
  windowAllowedUs = 0;

  if (!primed) {
    // Starting from the first second rather than from zero, so a busy app does
    // not read as idle for its first minute.
    cpu10Fx = cpu60Fx = cpuSample << 4;
    budget10Fx = budgetSample << 4;
    primed = true;
  } else {
    cpu10Fx = ewma(cpu10Fx, cpuSample, 10);
    cpu60Fx = ewma(cpu60Fx, cpuSample, 60);
    budget10Fx = ewma(budget10Fx, budgetSample, 10);
  }
  cpu1 = static_cast<uint16_t>(cpuSample);
  cpu10 = static_cast<uint16_t>((cpu10Fx + 8) >> 4);
  cpu60 = static_cast<uint16_t>((cpu60Fx + 8) >> 4);
  budget10 = static_cast<uint16_t>((budget10Fx + 8) >> 4);

  if (pressureEvents > 0) {
    pressureAgeS = 0;
  } else if (pressureAgeS < 0xFF) {
    ++pressureAgeS;
  }

  uint32_t score = budget10 / 10U;
  if (score > 100) score = 100;
  const bool pressured = pressureAgeS < kPressureHoldS;
  if (pressured && score < 90) score = 90;
  load = static_cast<uint8_t>(score);
  level = pressured ? Overloaded : score >= 80 ? High : score >= 50 ? Medium : Low;
}

const char* AppLoad::levelName(uint8_t level) {
  switch (level) {
    case Medium: return "medium";
    case High: return "high";
    case Overloaded: return "overloaded";
    case Low:
    default: return "low";
  }
}

}  // namespace nhos
