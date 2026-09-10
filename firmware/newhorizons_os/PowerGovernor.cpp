#include "PowerGovernor.h"

namespace nhos {

void PowerGovernor::begin(PowerProfile profile) {
  profile_ = profile;
  currentMhz_ = getCpuFrequencyMhz();
  applyMhz(kFullMhz);
}

void PowerGovernor::setProfile(PowerProfile profile) { profile_ = profile; }

uint32_t PowerGovernor::targetMhz(bool scannerActive, bool runtimeActive) const {
  if (!runtimeActive) {
    // Soft-off: nothing is sampling, so the floor is always safe. This is the
    // case the sketch used to handle with an inline setCpuFrequencyMhz(80).
    return kPowerSaveIdleMhz;
  }
  if (scannerActive) {
    return kFullMhz;  // never trade sample timing for power -- see the header
  }
  switch (profile_) {
    case PowerProfile::PowerSave: return kPowerSaveIdleMhz;
    case PowerProfile::Balanced: return kBalancedIdleMhz;
    case PowerProfile::Performance:
    default: return kFullMhz;
  }
}

void PowerGovernor::applyMhz(uint32_t mhz) {
  if (mhz == currentMhz_) {
    return;
  }
  setCpuFrequencyMhz(mhz);
  currentMhz_ = mhz;
  ++transitions_;
}

void PowerGovernor::service(bool scannerActive, bool runtimeActive) {
  applyMhz(targetMhz(scannerActive, runtimeActive));
}

const char* PowerGovernor::profileName(PowerProfile profile) {
  switch (profile) {
    case PowerProfile::Balanced: return "balanced";
    case PowerProfile::PowerSave: return "powersave";
    case PowerProfile::Performance:
    default: return "performance";
  }
}

PowerProfile PowerGovernor::profileFromName(const char* name) {
  if (name == nullptr) {
    return PowerProfile::Performance;
  }
  if (strcmp(name, "balanced") == 0) {
    return PowerProfile::Balanced;
  }
  if (strcmp(name, "powersave") == 0) {
    return PowerProfile::PowerSave;
  }
  return PowerProfile::Performance;
}

String PowerGovernor::statusJson() const {
  String json = "{\"profile\":\"";
  json += profileName(profile_);
  json += "\",\"cpu_mhz\":";
  json += String(currentMhz_);
  json += ",\"transitions\":";
  json += String(transitions_);
  json += "}";
  return json;
}

}  // namespace nhos
