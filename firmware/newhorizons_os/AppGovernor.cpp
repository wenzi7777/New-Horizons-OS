#include "AppGovernor.h"

#include "AppManager.h"
#include "MatrixScanner.h"

namespace nhos {

void AppGovernor::setCeilingPermille(uint16_t permille) {
  if (permille > 900) {
    permille = 900;  // never promise the apps more than the scan can spare
  }
  ceilingPermille_ = permille;
}

void AppGovernor::apply(uint32_t allowanceUs, uint32_t nowMs) {
  if (allowanceUs == allowanceUs_) {
    return;
  }
  if (allowanceUs == 0) {
    suspendedAtMs_ = nowMs;
  }
  allowanceUs_ = allowanceUs;
  if (apps_ != nullptr) {
    apps_->setTotalBudgetUs(allowanceUs_);
  }
}

void AppGovernor::update(const ScanHealth& health, uint32_t nowMs) {
  const uint16_t targetFps = health.targetFps != 0 ? health.targetFps : 60;
  const uint32_t periodUs = 1000000UL / targetFps;
  ceilingUs_ = (periodUs * ceilingPermille_) / 1000;

  if (!primed_) {
    primed_ = true;
    windowStartedMs_ = nowMs;
    lastOverrunFrames_ = health.overrunFrames;
    apply(ceilingUs_, nowMs);
    return;
  }

  if (nowMs - windowStartedMs_ < kWindowMs) {
    return;
  }
  windowStartedMs_ = nowMs;

  const uint32_t overruns = health.overrunFrames;
  const bool missedDeadlines = overruns > lastOverrunFrames_;
  lastOverrunFrames_ = overruns;
  // A scan running measurably below its target is the same signal arriving by
  // a different route -- frames that were never attempted rather than frames
  // that ran late.
  const bool belowTarget = health.active && health.actualScanFps != 0 &&
                           health.actualScanFps + (targetFps / 10) < targetFps;

  if (missedDeadlines || belowTarget) {
    ++shrinks_;
    const uint32_t reduced = allowanceUs_ >> kDecreaseShift;
    // Below a useful floor there is no point pretending: take it all away and
    // let the scan recover cleanly.
    apply(reduced > 100 ? reduced : 0, nowMs);
    return;
  }

  if (allowanceUs_ >= ceilingUs_) {
    allowanceUs_ = ceilingUs_;
    return;
  }
  if (allowanceUs_ == 0 && (nowMs - suspendedAtMs_) < kMinSuspendMs) {
    return;  // hysteresis: do not resume straight back into the same overload
  }
  ++grows_;
  const uint32_t step = ceilingUs_ / kIncreaseSteps;
  const uint32_t grown = allowanceUs_ + (step > 0 ? step : 1);
  apply(grown < ceilingUs_ ? grown : ceilingUs_, nowMs);
}

String AppGovernor::statusJson() const {
  String json = "{\"ceiling_permille\":";
  json += String(ceilingPermille_);
  json += ",\"ceiling_us\":";
  json += String(ceilingUs_);
  json += ",\"allowance_us\":";
  json += String(allowanceUs_);
  json += ",\"shrinks\":";
  json += String(shrinks_);
  json += ",\"grows\":";
  json += String(grows_);
  json += "}";
  return json;
}

}  // namespace nhos
