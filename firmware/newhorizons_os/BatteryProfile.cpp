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

BatteryProfile resolveBatteryProfile(bool gaugeResponsive, bool voltageValid,
                                    BatteryIdClass detectedId,
                                    const ManualBatteryProfile& manualProfile) {
  // A verified POGO identifier always wins on the next healthy gauge poll.
  // That prevents a stale manual setting from raising a hardware-selected
  // charging limit. The 100k ID is still auto-detected but remains at the
  // 100mA safe ceiling because its capacity has not been provisioned.
  if (gaugeResponsive && voltageValid) {
    if (detectedId == BatteryIdClass::Ohm1k) {
      return profile(BatteryProfileId::Pogo1k, BatteryProfileSource::Auto,
                     true, false, 200, 100);
    }
    if (detectedId == BatteryIdClass::Ohm10k) {
      return profile(BatteryProfileId::Pogo10k, BatteryProfileSource::Auto,
                     true, false, 400, 200);
    }
    if (detectedId == BatteryIdClass::Ohm100k) {
      return profile(BatteryProfileId::Unknown, BatteryProfileSource::Auto,
                     false, true, 0, 100);
    }
    if (detectedId == BatteryIdClass::NoId ||
        detectedId == BatteryIdClass::JstOrUnknown ||
        detectedId == BatteryIdClass::Unknown) {
      if (manualBatteryProfileIsUsable(manualProfile)) {
        return profile(BatteryProfileId::Manual, BatteryProfileSource::Manual,
                       true, false, manualProfile.capacityMah,
                       manualProfile.maxChargeCurrentMa);
      }
      return profile(BatteryProfileId::Manual, BatteryProfileSource::Manual,
                     false, true, 0, 100);
    }
  }
  if (manualBatteryProfileIsUsable(manualProfile)) {
    return profile(BatteryProfileId::Manual, BatteryProfileSource::Manual,
                   true, false, manualProfile.capacityMah,
                   manualProfile.maxChargeCurrentMa);
  }
  return resolveBatteryProfile(gaugeResponsive, voltageValid, detectedId,
                               BatteryProfileId::None);
}

uint16_t max17048RawVcellToMv(uint16_t raw) { return static_cast<uint16_t>((static_cast<uint32_t>(raw) * 5U + 32U) / 64U); }
uint16_t max17048RawSocToCentiPercent(uint16_t raw) { return static_cast<uint16_t>((static_cast<uint32_t>(raw) * 100U + 128U) / 256U); }
int16_t max17048RawRateToCentiPercentPerHour(uint16_t raw) { return static_cast<int16_t>((static_cast<int32_t>(static_cast<int16_t>(raw)) * 208) / 10); }
bool isValidBatteryVoltageMv(uint16_t mv) { return mv >= 2500 && mv <= 5000; }

BatteryIdClass classifyBatteryIdAdcRaw(int raw) {
  // v1.5.F BAT_ID is pulled up to 3.3V through R12=100k. A small-board
  // resistor to GND therefore centers around raw 40 (1k), 372 (10k), or
  // 2048 (100k) on the ESP32-S3's 12-bit ADC. An open POGO/JST pin is near
  // full scale and is deliberately distinguishable from a populated ID.
  if (raw < 0 || raw > 4095) return BatteryIdClass::Unknown;
  if (raw <= 150) return BatteryIdClass::Ohm1k;
  if (raw <= 750) return BatteryIdClass::Ohm10k;
  if (raw >= 3600) return BatteryIdClass::NoId;
  if (raw >= 1500 && raw <= 2600) return BatteryIdClass::Ohm100k;
  return BatteryIdClass::Unknown;
}

const char* batteryIdClassName(BatteryIdClass id) {
  switch (id) {
    case BatteryIdClass::Ohm1k: return "pogo_1k";
    case BatteryIdClass::Ohm10k: return "pogo_10k";
    case BatteryIdClass::Ohm100k: return "pogo_100k";
    case BatteryIdClass::NoId: return "no_id";
    case BatteryIdClass::JstOrUnknown: return "jst_or_unknown";
    default: return "unknown";
  }
}

const char* batteryProfileIdName(BatteryProfileId id) { switch (id) { case BatteryProfileId::Pogo1k: return "pogo_1k"; case BatteryProfileId::Pogo10k: return "pogo_10k"; case BatteryProfileId::Jst: return "jst"; case BatteryProfileId::Manual: return "manual"; case BatteryProfileId::Unknown: return "unknown"; default: return "none"; } }
const char* batteryProfileSourceName(BatteryProfileSource source) { switch (source) { case BatteryProfileSource::Auto: return "auto"; case BatteryProfileSource::Manual: return "manual"; default: return "pending"; } }
}  // namespace nhos
