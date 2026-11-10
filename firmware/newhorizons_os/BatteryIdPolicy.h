#pragma once

#include <cstdint>

namespace nhos {

// v2.0.F: the battery's contact board carries an ID resistor between the ID
// and GND contacts; the main board pulls GPIO15 (BAT_ID) up through 200k
// (R3 + R5) to 3V3, so the pin sits at 3V3 * R / (R + 200k). Without a
// resistor -- the JST socket, a cell without a contact board -- it reads
// about 3.3 V and the battery is set by hand, as on every earlier board.
static constexpr uint32_t kBatteryIdPullUpOhm = 200000;
static constexpr uint16_t kBatteryIdSupplyMv = 3300;

enum class BatteryIdKind : uint8_t {
  NoId = 0,   // pin high: nothing connected / JST / unknown cell
  Known,      // matched one of the lab's batteries
  Reserved,   // a resistor above the four assigned ones (220k..820k, not assigned yet)
  Invalid,    // too low to be any ID: short or damaged contact
};

struct BatteryIdResult {
  BatteryIdKind kind = BatteryIdKind::NoId;
  uint16_t mv = 0;              // calibrated reading of the pin
  uint32_t resistanceOhm = 0;   // derived; 0 for NoId / Invalid
  uint16_t capacityMah = 0;     // Known only
  uint16_t maxChargeCurrentMa = 0;
};

// R = pullUp * v / (supply - v). The four assigned values (39k, 68k, 100k,
// 150k) are matched by the geometric midpoints between neighbours, so 1 %
// resistors and ADC error cannot cross a boundary.
BatteryIdResult classifyBatteryId(uint16_t mv, uint16_t supplyMv = kBatteryIdSupplyMv);

const char* batteryIdKindName(BatteryIdKind kind);

}  // namespace nhos
