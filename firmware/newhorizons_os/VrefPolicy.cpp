#include "VrefPolicy.h"

namespace nhos {

uint16_t vrefMvToDacCode(uint16_t mv) {
  const uint32_t code =
      (static_cast<uint32_t>(mv) * 4096U + kDacSupplyMv / 2U) / kDacSupplyMv;
  return code > kDacMaxCode ? kDacMaxCode : static_cast<uint16_t>(code);
}

uint16_t dacCodeToVrefMv(uint16_t code) {
  if (code > kDacMaxCode) {
    code = kDacMaxCode;
  }
  return static_cast<uint16_t>(
      (static_cast<uint32_t>(code) * kDacSupplyMv + 2048U) / 4096U);
}

uint16_t clampVrefLimitMv(uint32_t requestedMv) {
  if (requestedMv < kVrefSafeMv) {
    return kVrefSafeMv;
  }
  if (requestedMv > kVrefAbsoluteMaxMv) {
    return kVrefAbsoluteMaxMv;
  }
  return static_cast<uint16_t>(requestedMv);
}

VrefRequest checkVrefRequest(uint32_t requestedMv, uint16_t limitMv) {
  if (requestedMv < kVrefMinMv) {
    return VrefRequest::BelowMinimum;
  }
  if (requestedMv > limitMv) {
    return VrefRequest::AboveLimit;
  }
  return VrefRequest::Ok;
}

bool dacEepromNeedsWrite(uint16_t eepromCode) {
  return eepromCode != vrefMvToDacCode(kVrefSafeMv);
}

bool decodeMcp4725Read(const uint8_t* buf, size_t len, Mcp4725Read& out) {
  if (buf == nullptr || len < 5) {
    return false;
  }
  out.ready = (buf[0] & 0x80U) != 0;
  out.por = (buf[0] & 0x40U) != 0;
  out.dacCode = static_cast<uint16_t>((static_cast<uint16_t>(buf[1]) << 4) | (buf[2] >> 4));
  out.eepromCode = static_cast<uint16_t>(((buf[3] & 0x0FU) << 8) | buf[4]);
  return true;
}

size_t encodeMcp4725Write(uint16_t code, bool persist, uint8_t* out) {
  if (code > kDacMaxCode) {
    code = kDacMaxCode;
  }
  if (!persist) {
    // Fast mode: 0 0 PD1 PD0 D11..D8, then D7..D0 (normal operation, PD = 00).
    out[0] = static_cast<uint8_t>(code >> 8);
    out[1] = static_cast<uint8_t>(code & 0xFFU);
    return 2;
  }
  // C2 C1 C0 = 0 1 1 (DAC + EEPROM), PD = 00; D11..D4, then D3..D0 << 4.
  out[0] = 0x60;
  out[1] = static_cast<uint8_t>(code >> 4);
  out[2] = static_cast<uint8_t>((code & 0x0FU) << 4);
  return 3;
}

uint8_t pca9536OutputByte(bool calibrationOn) {
  return calibrationOn ? 0xFF : 0xFE;
}

uint8_t pca9536ConfigByte() { return 0xFE; }

}  // namespace nhos
