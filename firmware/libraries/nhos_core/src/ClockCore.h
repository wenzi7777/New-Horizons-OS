#pragma once

#include <stddef.h>
#include <stdint.h>

namespace nhos {

// The arithmetic behind ClockService -- no Arduino, no ESP-IDF -- so it can
// be unit tested on the host (tests_native/test_clock_core.cpp).
//
// Shared with New-Horizons-Hub as a byte-identical copy (see that repo's
// README "Shared files"): the Hub disciplines its own clock with the same
// core before serving time to devices over ESP-NOW.

// Where the current wall-clock reading came from. Values are on the wire
// (the high nibble of an ESP-NOW TIME_RESP flags byte), so never renumber.
enum class TimeSource : uint8_t {
  None = 0,     // free-running; only monotonic uptime is meaningful
  Sntp = 1,     // an NTP server, via ESP-IDF's SNTP client
  Host = 2,     // pushed in with the `set_time` command
  Hub = 3,      // a Hub, over ESP-NOW (EspNowTimeWire)
  Backend = 4,  // the Desktop backend, over the Hub's uplink (Hub only)
  Gateway = 5,  // the Gateway host, over UDP (EspNowTimeWire's exchange)
};

const char* timeSourceName(TimeSource source);

// One measurement of the local clock against a reference: at local
// monotonic time `monoUs`, wall time was `monoUs + offsetUs`.
struct ClockSample {
  TimeSource source = TimeSource::None;
  int64_t monoUs = 0;
  int64_t offsetUs = 0;
  uint32_t uncertaintyUs = 0;
};

enum class ClockSubmitResult : uint8_t {
  Rejected = 0,  // not a plausible time (before kClockMinValidEpochUs)
  Ignored = 1,   // a better source is in charge
  Stepped = 2,   // first sync or too far off: jumped
  Slewed = 3,    // small correction: spread out so time never runs backwards
};

const char* clockSubmitResultName(ClockSubmitResult result);

// ~2020-09-13. Anything earlier is a zero/garbage clock, not a real time.
constexpr int64_t kClockMinValidEpochUs = 1600000000LL * 1000000LL;
// Corrections bigger than this jump instead of slewing -- slewing 100 ms at
// kClockMaxSlewPpm would already take over three minutes.
constexpr int64_t kClockStepThresholdUs = 100000;
// Slew rate cap: 500 ppm = 0.5 ms per second, far below any rate at which a
// sample-to-sample interval could appear to run backwards.
constexpr int64_t kClockMaxSlewPpm = 500;
// Assumed worst-case crystal error while free-running between syncs; drives
// the reported uncertainty growth.
constexpr int64_t kClockHoldoverPpm = 50;
// A lower-priority source is only allowed to take over once the source in
// charge has been silent this long.
constexpr int64_t kClockSourceStaleUs = 10LL * 60LL * 1000000LL;

class ClockCore {
 public:
  // `nowMonoUs` is the local monotonic time at the moment of the call; it
  // anchors any slew so readings taken before it are never disturbed.
  // `force` bypasses the source-priority check (an explicit user action).
  // Priority: Gateway > SNTP = Hub > Backend > Host. The Gateway outranks
  // NTP on purpose: every device behind one Gateway then shares a single
  // reference, which matters more for multi-device recordings than the
  // last few ms of absolute accuracy; NTP takes over once the Gateway has
  // been silent for kClockSourceStaleUs.
  ClockSubmitResult submit(const ClockSample& sample, int64_t nowMonoUs, bool force = false);
  void reset();

  bool synced() const { return synced_; }
  TimeSource source() const { return source_; }
  // 0 when not synced.
  int64_t epochUsAt(int64_t monoUs) const;
  uint32_t uncertaintyUsAt(int64_t monoUs) const;

  int64_t lastSyncMonoUs() const { return lastSyncMonoUs_; }
  // Signed correction applied by the last accepted sample (target minus
  // what this clock believed at that moment).
  int64_t lastAdjustUs() const { return lastAdjustUs_; }
  uint32_t adjustCount() const { return adjustCount_; }
  uint32_t stepCount() const { return stepCount_; }
  // Correction still waiting to be slewed in, as of `monoUs`.
  int64_t pendingSlewUsAt(int64_t monoUs) const;

  static uint8_t priority(TimeSource source);

 private:
  int64_t offsetUsAt(int64_t monoUs) const;
  int64_t appliedSlewUsAt(int64_t monoUs) const;

  bool synced_ = false;
  TimeSource source_ = TimeSource::None;
  int64_t baseOffsetUs_ = 0;
  int64_t slewStartMonoUs_ = 0;
  int64_t slewTotalUs_ = 0;
  int64_t lastSyncMonoUs_ = 0;
  uint32_t syncUncertaintyUs_ = 0;
  int64_t lastAdjustUs_ = 0;
  uint32_t adjustCount_ = 0;
  uint32_t stepCount_ = 0;
};

// NTP's clock filter, cut down: of the last few round-trip samples, the one
// with the smallest round-trip delay has the least room for path asymmetry,
// so it is the one to trust.
class ClockFilter {
 public:
  // Kept short on purpose: an old sample's offset is stale by the crystal
  // drift since it was taken (20 ppm * 60 s = 1.2 ms at a 15 s cadence).
  static constexpr size_t kWindow = 4;
  // A sample this far from the current best means the reference itself
  // stepped (e.g. the Hub re-synced); the window is flushed so stale
  // pre-step samples cannot win on delay.
  static constexpr int64_t kResetOnJumpUs = 50000;

  void reset();
  void add(const ClockSample& sample, uint32_t delayUs);
  bool best(ClockSample* out, uint32_t* delayUs = nullptr) const;
  size_t count() const { return count_; }

 private:
  ClockSample samples_[kWindow];
  uint32_t delays_[kWindow] = {};
  size_t next_ = 0;
  size_t count_ = 0;
};

}  // namespace nhos
