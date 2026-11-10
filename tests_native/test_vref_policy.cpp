#include <cassert>
#include <cstdint>
#include <iostream>

#include "VrefPolicy.h"

using namespace nhos;

namespace {

void testCodeConversion() {
  assert(vrefMvToDacCode(0) == 0);
  assert(vrefMvToDacCode(330) == 410);   // 409.6 rounds up
  assert(vrefMvToDacCode(1650) == 2048);
  assert(vrefMvToDacCode(3300) == 4095);  // clamped, 4096 does not exist
  assert(vrefMvToDacCode(65535) == 4095);
  assert(dacCodeToVrefMv(410) == 330);
  assert(dacCodeToVrefMv(2048) == 1650);
  assert(dacCodeToVrefMv(5000) == dacCodeToVrefMv(4095));
  // The safe value survives a round trip within one LSB (0.8 mV).
  for (uint16_t mv = 100; mv <= 2000; mv += 7) {
    const uint16_t back = dacCodeToVrefMv(vrefMvToDacCode(mv));
    assert((back > mv ? back - mv : mv - back) <= 1);
  }
}

void testLimitAndRequest() {
  assert(clampVrefLimitMv(0) == kVrefSafeMv);
  assert(clampVrefLimitMv(800) == 800);
  assert(clampVrefLimitMv(50000) == kVrefAbsoluteMaxMv);
  assert(checkVrefRequest(99, 800) == VrefRequest::BelowMinimum);
  assert(checkVrefRequest(100, 800) == VrefRequest::Ok);
  assert(checkVrefRequest(800, 800) == VrefRequest::Ok);
  assert(checkVrefRequest(801, 800) == VrefRequest::AboveLimit);
  // The default ceiling keeps the GPIO45 reset strap safe.
  assert(kVrefDefaultLimitMv <= 800);
}

void testEepromDecision() {
  // Factory 1.65 V, a corrupted word and one LSB off are all rewritten.
  assert(dacEepromNeedsWrite(2048));
  assert(dacEepromNeedsWrite(0));
  assert(dacEepromNeedsWrite(409));
  assert(dacEepromNeedsWrite(411));
  // Already the safe code: never written again.
  assert(!dacEepromNeedsWrite(vrefMvToDacCode(kVrefSafeMv)));
}

void testReadDecode() {
  // Status RDY, DAC 0x19A (410) -> bytes 0x19, 0xA0; EEPROM 0x7FF.
  const uint8_t buf[5] = {0xC0, 0x19, 0xA0, 0x07, 0xFF};
  Mcp4725Read r;
  assert(decodeMcp4725Read(buf, 5, r));
  assert(r.ready && r.por);
  assert(r.dacCode == 410);
  assert(r.eepromCode == 0x7FF);
  const uint8_t busy[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
  assert(decodeMcp4725Read(busy, 5, r));
  assert(!r.ready && !r.por);
  assert(!decodeMcp4725Read(buf, 4, r));
  assert(!decodeMcp4725Read(nullptr, 5, r));
}

void testWriteEncode() {
  uint8_t out[3] = {};
  assert(encodeMcp4725Write(410, false, out) == 2);
  assert(out[0] == 0x01 && out[1] == 0x9A);          // PD = 00, D11..D8 = 1
  assert(encodeMcp4725Write(410, true, out) == 3);
  assert(out[0] == 0x60 && out[1] == 0x19 && out[2] == 0xA0);
  assert(encodeMcp4725Write(9999, false, out) == 2);
  assert(out[0] == 0x0F && out[1] == 0xFF);           // clamped to 4095
}

void testPca9536() {
  // Off must be written to the output register before IO0 becomes an output.
  assert(pca9536OutputByte(false) == 0xFE);
  assert(pca9536OutputByte(true) == 0xFF);
  assert(pca9536ConfigByte() == 0xFE);  // only IO0 is an output
  assert((pca9536OutputByte(false) & 0x01U) == 0);
}

}  // namespace

int main() {
  testCodeConversion();
  testLimitAndRequest();
  testEepromDecision();
  testReadDecode();
  testWriteEncode();
  testPca9536();
  std::cout << "vref policy tests passed\n";
  return 0;
}
