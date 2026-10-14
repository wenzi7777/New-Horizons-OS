#include "ClockCore.h"

namespace nhos {

namespace {
int64_t absI64(int64_t v) { return v < 0 ? -v : v; }
}  // namespace

const char* timeSourceName(TimeSource source) {
  switch (source) {
    case TimeSource::Sntp: return "sntp";
    case TimeSource::Host: return "host";
    case TimeSource::Hub: return "hub";
    case TimeSource::Backend: return "backend";
    case TimeSource::Gateway: return "gateway";
    case TimeSource::None:
    default: return "none";
  }
}

const char* clockSubmitResultName(ClockSubmitResult result) {
  switch (result) {
    case ClockSubmitResult::Ignored: return "ignored";
    case ClockSubmitResult::Stepped: return "stepped";
    case ClockSubmitResult::Slewed: return "slewed";
    case ClockSubmitResult::Rejected:
    default: return "rejected";
  }
}

uint8_t ClockCore::priority(TimeSource source) {
  switch (source) {
    case TimeSource::Gateway: return 4;
    case TimeSource::Sntp:
    case TimeSource::Hub: return 3;
    case TimeSource::Backend: return 2;
    case TimeSource::Host: return 1;
    case TimeSource::None:
    default: return 0;
  }
}

void ClockCore::reset() { *this = ClockCore(); }

int64_t ClockCore::appliedSlewUsAt(int64_t monoUs) const {
  if (slewTotalUs_ == 0 || monoUs <= slewStartMonoUs_) {
    return 0;
  }
  const int64_t elapsed = monoUs - slewStartMonoUs_;
  // Overflow-safe: elapsed (µs) * 500 stays far inside int64 for ~584 years.
  const int64_t budget = elapsed * kClockMaxSlewPpm / 1000000LL;
  if (budget >= absI64(slewTotalUs_)) {
    return slewTotalUs_;
  }
  return slewTotalUs_ < 0 ? -budget : budget;
}

int64_t ClockCore::offsetUsAt(int64_t monoUs) const {
  return baseOffsetUs_ + appliedSlewUsAt(monoUs);
}

int64_t ClockCore::pendingSlewUsAt(int64_t monoUs) const {
  return slewTotalUs_ - appliedSlewUsAt(monoUs);
}

int64_t ClockCore::epochUsAt(int64_t monoUs) const {
  if (!synced_) {
    return 0;
  }
  return monoUs + offsetUsAt(monoUs);
}

uint32_t ClockCore::uncertaintyUsAt(int64_t monoUs) const {
  if (!synced_) {
    return UINT32_MAX;
  }
  const int64_t age = monoUs > lastSyncMonoUs_ ? monoUs - lastSyncMonoUs_ : 0;
  const int64_t total = static_cast<int64_t>(syncUncertaintyUs_) +
                        age * kClockHoldoverPpm / 1000000LL +
                        absI64(pendingSlewUsAt(monoUs));
  return total >= static_cast<int64_t>(UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(total);
}

ClockSubmitResult ClockCore::submit(const ClockSample& sample, int64_t nowMonoUs, bool force) {
  if (sample.monoUs + sample.offsetUs < kClockMinValidEpochUs) {
    return ClockSubmitResult::Rejected;
  }
  if (synced_ && !force && priority(sample.source) < priority(source_) &&
      nowMonoUs - lastSyncMonoUs_ < kClockSourceStaleUs) {
    return ClockSubmitResult::Ignored;
  }

  // Fold whatever part of an earlier slew has already elapsed into the base,
  // so the new correction starts from exactly what this clock reads now.
  const int64_t currentOffset = synced_ ? offsetUsAt(nowMonoUs) : 0;
  const int64_t delta = sample.offsetUs - currentOffset;
  ClockSubmitResult result;
  if (!synced_ || absI64(delta) > kClockStepThresholdUs) {
    baseOffsetUs_ = sample.offsetUs;
    slewTotalUs_ = 0;
    ++stepCount_;
    result = ClockSubmitResult::Stepped;
  } else {
    baseOffsetUs_ = currentOffset;
    slewTotalUs_ = delta;
    result = ClockSubmitResult::Slewed;
  }
  slewStartMonoUs_ = nowMonoUs;
  lastAdjustUs_ = synced_ ? delta : 0;
  synced_ = true;
  source_ = sample.source;
  lastSyncMonoUs_ = nowMonoUs;
  // A filtered sample can be older than now; it has been free-running since.
  const int64_t sampleAge = nowMonoUs > sample.monoUs ? nowMonoUs - sample.monoUs : 0;
  const int64_t uncertainty = static_cast<int64_t>(sample.uncertaintyUs) +
                              sampleAge * kClockHoldoverPpm / 1000000LL;
  syncUncertaintyUs_ = uncertainty >= static_cast<int64_t>(UINT32_MAX)
                           ? UINT32_MAX
                           : static_cast<uint32_t>(uncertainty);
  ++adjustCount_;
  return result;
}

void ClockFilter::reset() {
  next_ = 0;
  count_ = 0;
}

void ClockFilter::add(const ClockSample& sample, uint32_t delayUs) {
  ClockSample current;
  if (best(&current) && absI64(sample.offsetUs - current.offsetUs) > kResetOnJumpUs) {
    reset();
  }
  samples_[next_] = sample;
  delays_[next_] = delayUs;
  next_ = (next_ + 1) % kWindow;
  if (count_ < kWindow) {
    ++count_;
  }
}

bool ClockFilter::best(ClockSample* out, uint32_t* delayUs) const {
  if (count_ == 0) {
    return false;
  }
  size_t bestIndex = SIZE_MAX;
  for (size_t i = 0; i < count_; ++i) {
    if (bestIndex == SIZE_MAX || delays_[i] < delays_[bestIndex]) {
      bestIndex = i;
    }
  }
  if (out != nullptr) *out = samples_[bestIndex];
  if (delayUs != nullptr) *delayUs = delays_[bestIndex];
  return true;
}

}  // namespace nhos
