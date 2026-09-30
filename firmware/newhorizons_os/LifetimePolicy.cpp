#include "LifetimePolicy.h"

#include <math.h>
#include <string.h>

namespace nhos {
namespace {

void addSaturating(uint32_t& counter, uint32_t amount) {
  counter = (UINT32_MAX - counter < amount) ? UINT32_MAX : counter + amount;
}

// Moves whole seconds out of `ms` into `seconds`.
void carry(uint32_t& seconds, uint32_t& ms, uint32_t deltaMs) {
  const uint64_t total = static_cast<uint64_t>(ms) + deltaMs;
  addSaturating(seconds, static_cast<uint32_t>(total / 1000));
  ms = static_cast<uint32_t>(total % 1000);
}

void copyVersion(char* dest, size_t destSize, const char* src) {
  if (src == nullptr) {
    src = "";
  }
  strncpy(dest, src, destSize - 1);
  dest[destSize - 1] = '\0';
}

}  // namespace

uint32_t lifetimePowerOnS(const LifetimeCounters& counters) {
  uint32_t total = counters.awakeS;
  addSaturating(total, counters.softOffS);
  return total;
}

void lifetimeAccumulate(LifetimeCounters& counters, LifetimeRemainder& remainder,
                        uint32_t deltaMs, bool awake, bool scanning) {
  if (awake) {
    carry(counters.awakeS, remainder.awakeMs, deltaMs);
  } else {
    carry(counters.softOffS, remainder.softOffMs, deltaMs);
  }
  if (scanning) {
    carry(counters.scanS, remainder.scanMs, deltaMs);
  }
}

void lifetimeNoteBoot(LifetimeCounters& counters, uint8_t resetReason, uint32_t bootCount,
                      const char* firmwareVersion, bool fresh) {
  const uint8_t slot = resetReason < kLifetimeResetSlots ? resetReason : 0;
  if (counters.resetCounts[slot] != UINT16_MAX) {
    ++counters.resetCounts[slot];
  }
  if (fresh) {
    counters.trackedSinceBoot = bootCount;
    copyVersion(counters.firstFirmware, sizeof(counters.firstFirmware), firmwareVersion);
    copyVersion(counters.lastFirmware, sizeof(counters.lastFirmware), firmwareVersion);
    return;
  }
  char current[sizeof(counters.lastFirmware)];
  copyVersion(current, sizeof(current), firmwareVersion);
  if (strcmp(counters.lastFirmware, current) != 0) {
    addSaturating(counters.firmwareChanges, 1);
    copyVersion(counters.lastFirmware, sizeof(counters.lastFirmware), current);
    // The low-water mark belongs to the image that produced it.
    counters.minFreeHeap = 0;
  }
}

void lifetimeNoteSession(LifetimeCounters& counters, uint32_t sessionS) {
  if (sessionS > counters.longestSessionS) {
    counters.longestSessionS = sessionS;
  }
}

bool lifetimeNoteTemperature(LifetimeCounters& counters, float celsius) {
  if (isnan(celsius) || celsius < -40.0f || celsius > 125.0f) {
    return false;
  }
  const int16_t c10 = static_cast<int16_t>(lroundf(celsius * 10.0f));
  bool changed = false;
  if (counters.tempMaxC10 == kLifetimeTempUnset || c10 > counters.tempMaxC10) {
    counters.tempMaxC10 = c10;
    changed = true;
  }
  if (counters.tempMinC10 == kLifetimeTempUnset || c10 < counters.tempMinC10) {
    counters.tempMinC10 = c10;
    changed = true;
  }
  return changed;
}

bool lifetimeNoteMinHeap(LifetimeCounters& counters, uint32_t minFreeHeap) {
  if (minFreeHeap == 0) {
    return false;
  }
  if (counters.minFreeHeap == 0 || minFreeHeap < counters.minFreeHeap) {
    counters.minFreeHeap = minFreeHeap;
    return true;
  }
  return false;
}

bool lifetimeFlushDue(uint32_t nowMs, uint32_t lastFlushMs) {
  return nowMs - lastFlushMs >= kLifetimeFlushIntervalMs;
}

uint8_t lifetimeHealthReasons(const LifetimeHealthInputs& inputs) {
  uint8_t reasons = 0;
  if (inputs.safeMode) {
    reasons |= kLifetimeReasonSafeMode;
  }
  if (inputs.lastCrashBootId != 0 && inputs.bootCount >= inputs.lastCrashBootId &&
      inputs.bootCount - inputs.lastCrashBootId <= kLifetimeRecentCrashBoots) {
    reasons |= kLifetimeReasonRecentCrash;
  }
  if (inputs.rolledBack) {
    reasons |= kLifetimeReasonRolledBack;
  }
  if (inputs.currentTempC10 != kLifetimeTempUnset && inputs.currentTempC10 >= kLifetimeHotTempC10) {
    reasons |= kLifetimeReasonHighTemperature;
  }
  if (inputs.minFreeHeap != 0 && inputs.minFreeHeap < kLifetimeLowHeapBytes) {
    reasons |= kLifetimeReasonLowHeap;
  }
  return reasons;
}

LifetimeVerdict lifetimeVerdictFor(uint8_t reasons) {
  if (reasons & kLifetimeReasonSafeMode) {
    return LifetimeVerdict::Fail;
  }
  return reasons == 0 ? LifetimeVerdict::Ok : LifetimeVerdict::Warn;
}

const char* lifetimeVerdictName(LifetimeVerdict verdict) {
  switch (verdict) {
    case LifetimeVerdict::Fail: return "fail";
    case LifetimeVerdict::Warn: return "warn";
    case LifetimeVerdict::Ok:
    default: return "ok";
  }
}

const char* lifetimeReasonName(uint8_t index) {
  switch (index) {
    case 0: return "safe_mode";
    case 1: return "recent_crash";
    case 2: return "ota_rolled_back";
    case 3: return "high_temperature";
    case 4: return "low_heap";
    default: return "unknown";
  }
}

}  // namespace nhos
