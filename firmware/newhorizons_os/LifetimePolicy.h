#pragma once

#include <stddef.h>
#include <stdint.h>

namespace nhos {

// Pure bookkeeping behind LifetimeStats -- no Arduino, no NVS -- so the
// arithmetic can be unit tested on the host (tests_native/).

constexpr uint16_t kLifetimeSchema = 1;
// One slot per esp_reset_reason_t value (ESP_RST_UNKNOWN .. ESP_RST_CPU_LOCKUP
// in ESP-IDF 5.5). Anything newer lands in slot 0 ("unknown").
constexpr uint8_t kLifetimeResetSlots = 16;
// How often accumulated time is written back to NVS. A power cut loses at
// most this much; planned restarts flush first (see LifetimeStats::flush()).
constexpr uint32_t kLifetimeFlushIntervalMs = 10UL * 60UL * 1000UL;
constexpr int16_t kLifetimeTempUnset = INT16_MIN;
// A crash this many boots ago or fewer still counts as "recent".
constexpr uint32_t kLifetimeRecentCrashBoots = 10;
// Chip temperature (tenths of a degree C) above which health warns.
constexpr int16_t kLifetimeHotTempC10 = 800;
// Lowest free heap (bytes) on the running firmware below which health warns.
// v1.3.0 bottomed out around 82 KB on a v1.5.F, so this is far past normal.
constexpr uint32_t kLifetimeLowHeapBytes = 16UL * 1024UL;

// Persisted as a single NVS blob. Append-only: new fields go at the end and
// bump kLifetimeSchema, so an older blob is still read as a valid prefix.
struct LifetimeCounters {
  uint16_t schema = kLifetimeSchema;
  uint16_t reserved = 0;
  uint32_t awakeS = 0;
  uint32_t softOffS = 0;
  uint32_t scanS = 0;             // matrix scanner running = sensor in use
  uint32_t longestSessionS = 0;   // longest single boot-to-reset run
  uint32_t trackedSinceBoot = 0;  // boot_count when these counters were created
  uint32_t firstSeenUnix = 0;     // first wall-clock time seen; 0 = never synced
  uint32_t otaSuccess = 0;
  uint32_t otaRollback = 0;
  uint32_t firmwareChanges = 0;   // OTA or serial flash, anything that changed the version
  uint32_t wifiDisconnects = 0;
  uint32_t buttonPresses = 0;
  uint32_t minFreeHeap = 0;       // running firmware only; 0 = not sampled yet
  int16_t tempMaxC10 = kLifetimeTempUnset;
  int16_t tempMinC10 = kLifetimeTempUnset;
  uint16_t resetCounts[kLifetimeResetSlots] = {};
  char firstFirmware[24] = {0};
  char lastFirmware[24] = {0};
};

// Sub-second leftovers, RAM only: a flush every 10 minutes must not round
// away a second per tick.
struct LifetimeRemainder {
  uint32_t awakeMs = 0;
  uint32_t softOffMs = 0;
  uint32_t scanMs = 0;
};

uint32_t lifetimePowerOnS(const LifetimeCounters& counters);

// Folds deltaMs of elapsed time into the counters. `awake` is false while the
// device sits in soft-off; `scanning` is counted independently of it.
void lifetimeAccumulate(LifetimeCounters& counters, LifetimeRemainder& remainder,
                        uint32_t deltaMs, bool awake, bool scanning);

// Once per boot. `fresh` is true when no stored counters existed: the
// firmware becomes the tracking baseline instead of counting as a change.
void lifetimeNoteBoot(LifetimeCounters& counters, uint8_t resetReason, uint32_t bootCount,
                      const char* firmwareVersion, bool fresh);

void lifetimeNoteSession(LifetimeCounters& counters, uint32_t sessionS);
// Returns true when a new extreme was recorded. NaN / implausible readings
// are ignored.
bool lifetimeNoteTemperature(LifetimeCounters& counters, float celsius);
bool lifetimeNoteMinHeap(LifetimeCounters& counters, uint32_t minFreeHeap);

bool lifetimeFlushDue(uint32_t nowMs, uint32_t lastFlushMs);

enum class LifetimeVerdict : uint8_t { Ok, Warn, Fail };

// Bit flags explaining a non-Ok verdict.
constexpr uint8_t kLifetimeReasonSafeMode = 1 << 0;
constexpr uint8_t kLifetimeReasonRecentCrash = 1 << 1;
constexpr uint8_t kLifetimeReasonRolledBack = 1 << 2;
constexpr uint8_t kLifetimeReasonHighTemperature = 1 << 3;
constexpr uint8_t kLifetimeReasonLowHeap = 1 << 4;
constexpr uint8_t kLifetimeReasonCount = 5;

struct LifetimeHealthInputs {
  bool safeMode = false;           // repeated failed boots forced safe maintenance
  uint32_t bootCount = 0;
  uint32_t lastCrashBootId = 0;    // 0 = no crash on record
  bool rolledBack = false;         // an OTA image was reverted and not superseded
  int16_t currentTempC10 = kLifetimeTempUnset;
  uint32_t minFreeHeap = 0;        // running firmware; 0 = unknown
};

uint8_t lifetimeHealthReasons(const LifetimeHealthInputs& inputs);
LifetimeVerdict lifetimeVerdictFor(uint8_t reasons);
const char* lifetimeVerdictName(LifetimeVerdict verdict);
// Name of one reason bit (index 0 .. kLifetimeReasonCount-1).
const char* lifetimeReasonName(uint8_t index);

}  // namespace nhos
