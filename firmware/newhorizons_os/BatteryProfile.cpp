#include "BatteryProfile.h"

namespace nhos {
namespace {
BatteryProfile profile(BatteryProfileId id, BatteryProfileSource source, bool resolved,
                       bool required, uint16_t capacityMah, uint16_t maxMa) {
  return {id, source, resolved, required, capacityMah, maxMa};
}
}

BatteryProfile resolveBatteryProfile(bool gaugeResponsive, bool voltageValid,
                                    BatteryIdClass detectedId,
                                    BatteryProfileId manualProfile) {
  if (manualProfile != BatteryProfileId::None && manualProfile != BatteryProfileId::Unknown) {
    if (manualProfile == BatteryProfileId::Pogo1k) return profile(manualProfile, BatteryProfileSource::Manual, true, false, 200, 100);
    if (manualProfile == BatteryProfileId::Pogo10k) return profile(manualProfile, BatteryProfileSource::Manual, true, false, 400, 200);
    return profile(BatteryProfileId::Jst, BatteryProfileSource::Manual, true, false, 0, 100);
  }
  // An unresponsive gauge or invalid VBAT makes BAT_ID electrically untrusted.
  if (!gaugeResponsive || !voltageValid) return profile(BatteryProfileId::Unknown, BatteryProfileSource::Pending, false, true, 0, 100);
  if (detectedId == BatteryIdClass::Ohm1k) return profile(BatteryProfileId::Pogo1k, BatteryProfileSource::Auto, true, false, 200, 100);
  if (detectedId == BatteryIdClass::Ohm10k) return profile(BatteryProfileId::Pogo10k, BatteryProfileSource::Auto, true, false, 400, 200);
  if (detectedId == BatteryIdClass::JstOrUnknown) return profile(BatteryProfileId::Jst, BatteryProfileSource::Pending, false, true, 0, 100);
  return profile(BatteryProfileId::Unknown, BatteryProfileSource::Pending, false, true, 0, 100);
}

uint16_t max17048RawVcellToMv(uint16_t raw) { return static_cast<uint16_t>((static_cast<uint32_t>(raw) * 5U + 32U) / 64U); }
uint16_t max17048RawSocToCentiPercent(uint16_t raw) { return static_cast<uint16_t>((static_cast<uint32_t>(raw) * 100U + 128U) / 256U); }
int16_t max17048RawRateToCentiPercentPerHour(uint16_t raw) { return static_cast<int16_t>((static_cast<int32_t>(static_cast<int16_t>(raw)) * 208) / 10); }
bool isValidBatteryVoltageMv(uint16_t mv) { return mv >= 2500 && mv <= 5000; }

const char* batteryProfileIdName(BatteryProfileId id) { switch (id) { case BatteryProfileId::Pogo1k: return "pogo_1k"; case BatteryProfileId::Pogo10k: return "pogo_10k"; case BatteryProfileId::Jst: return "jst"; case BatteryProfileId::Unknown: return "unknown"; default: return "none"; } }
const char* batteryProfileSourceName(BatteryProfileSource source) { switch (source) { case BatteryProfileSource::Auto: return "auto"; case BatteryProfileSource::Manual: return "manual"; default: return "pending"; } }
}  // namespace nhos
