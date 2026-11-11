#pragma once

#include <cstddef>
#include <cstdint>

namespace nhos {

// v2.0.F: the reference voltage on the TIA front end ("Vref") comes from an
// MCP4725 12-bit DAC (0x60) running from the 3.3 V rail, and nothing on the
// board can read it back. Everything below is therefore about what the DAC is
// *told*; the safety rules here exist because of the GPIO45 strap: its 100k
// pull-down only wins at reset while Vref stays below about 0.8 V.

// The value the DAC's EEPROM must hold and that every reboot returns to: the
// same 0.33 V the 20k boards of the earlier revisions ran at.
static constexpr uint16_t kVrefSafeMv = 330;
// Lowest request set_vref accepts (the Vref sweep starts at 0.1 V).
static constexpr uint16_t kVrefMinMv = 100;
// Where the GPIO45 strap stops being safe: a reset that does not pass through
// the shutdown handler (watchdog, brown-out, EN) keeps the DAC's register
// value, and the 100k pull-down only wins below about this Vref.
static constexpr uint16_t kVrefStrapRiskMv = 800;
// Default ceiling for set_vref, with margin below kVrefStrapRiskMv; the
// operator may raise it, never above kVrefAbsoluteMaxMv, but accepts the
// reset-strap risk by doing so.
static constexpr uint16_t kVrefDefaultLimitMv = 700;
// rfb_measure's highest fit point, whatever the limit: below the strap risk.
static constexpr uint16_t kRfbFitMaxVrefMv = 750;
static constexpr uint16_t kVrefAbsoluteMaxMv = 2000;
// DAC full scale: the MCP4725's reference is its supply.
static constexpr uint16_t kDacSupplyMv = 3300;
static constexpr uint16_t kDacMaxCode = 4095;

uint16_t vrefMvToDacCode(uint16_t mv);
uint16_t dacCodeToVrefMv(uint16_t code);

// Limit stored by the operator, forced into [kVrefSafeMv, kVrefAbsoluteMaxMv].
uint16_t clampVrefLimitMv(uint32_t requestedMv);

enum class VrefRequest : uint8_t {
  Ok = 0,
  BelowMinimum,
  AboveLimit,
};
VrefRequest checkVrefRequest(uint32_t requestedMv, uint16_t limitMv);

// True when the DAC's EEPROM (power-on value) is not the safe code. Compared
// against the target, not against the factory 1.65 V, so a corrupted value is
// repaired as well and a correct one is never rewritten.
bool dacEepromNeedsWrite(uint16_t eepromCode);

// ---- MCP4725 wire format -------------------------------------------------

struct Mcp4725Read {
  bool ready = false;   // BSY/RDY bit: false while an EEPROM write runs
  bool por = false;     // power-on-reset flag
  uint16_t dacCode = 0;
  uint16_t eepromCode = 0;
};

// Five bytes: status, DAC high, DAC low, EEPROM high (PD + D11..D8), EEPROM low.
bool decodeMcp4725Read(const uint8_t* buf, size_t len, Mcp4725Read& out);

// "Write DAC register" (2 bytes) or, with persist, "write DAC register and
// EEPROM" (3 bytes). Returns the byte count written into out (needs 3).
size_t encodeMcp4725Write(uint16_t code, bool persist, uint8_t* out);

// ---- PCA9536 (0x41): IO0 drives the calibration switch Q1 ------------------

static constexpr uint8_t kPca9536OutputReg = 0x01;
static constexpr uint8_t kPca9536ConfigReg = 0x03;
// The output register powers up at 0xFF, i.e. IO0 high = switch on, so the
// output byte must be written low *before* IO0 is made an output.
uint8_t pca9536OutputByte(bool calibrationOn);
// IO0 output, IO1..IO3 inputs (they are not connected on v2.0.F).
uint8_t pca9536ConfigByte();

}  // namespace nhos
