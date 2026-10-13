#include <cassert>
#include <cstdint>
#include <iostream>

#include "EspNowFrame.h"
#include "EspNowTimeWire.h"

using namespace nhos;

namespace {

constexpr int64_t kEpoch = 1760000000LL * 1000000LL;

void testRequestRoundTrip() {
  EspNowTimeRequest req;
  req.seq = 0xA7;
  req.t1MonoUs = 123456789012LL;
  uint8_t buf[kEspNowTimeReqLen];
  assert(encodeEspNowTimeRequest(req, buf, sizeof(buf)) == kEspNowTimeReqLen);
  assert(buf[0] == kEspNowTimeReqMagic);
  EspNowTimeRequest back;
  assert(decodeEspNowTimeRequest(buf, sizeof(buf), &back));
  assert(back.seq == 0xA7);
  assert(back.t1MonoUs == req.t1MonoUs);
  // Too small a buffer encodes nothing.
  assert(encodeEspNowTimeRequest(req, buf, kEspNowTimeReqLen - 1) == 0);
}

void testResponseRoundTrip() {
  EspNowTimeResponse resp;
  resp.seq = 3;
  resp.hubSynced = true;
  resp.hubSource = 1;
  resp.t1MonoUs = 42;
  resp.t2EpochUs = kEpoch + 1000;
  resp.t3EpochUs = kEpoch + 1300;
  resp.hubUncertaintyMs = 0x1234;
  uint8_t buf[kEspNowTimeRespLen];
  assert(encodeEspNowTimeResponse(resp, buf, sizeof(buf)) == kEspNowTimeRespLen);
  EspNowTimeResponse back;
  assert(decodeEspNowTimeResponse(buf, sizeof(buf), &back));
  assert(back.seq == 3);
  assert(back.hubSynced);
  assert(back.hubSource == 1);
  assert(back.t1MonoUs == 42);
  assert(back.t2EpochUs == kEpoch + 1000);
  assert(back.t3EpochUs == kEpoch + 1300);
  assert(back.hubUncertaintyMs == 0x1234);
}

void testDecodeIsStrict() {
  uint8_t buf[kEspNowTimeRespLen];
  EspNowTimeResponse resp;
  encodeEspNowTimeResponse(resp, buf, sizeof(buf));
  EspNowTimeResponse out;
  assert(!decodeEspNowTimeResponse(buf, sizeof(buf) - 1, &out));
  buf[1] = kEspNowTimeWireVersion + 1;
  assert(!decodeEspNowTimeResponse(buf, sizeof(buf), &out));
  // A request is never mistaken for a response, nor vice versa.
  uint8_t req[kEspNowTimeReqLen];
  encodeEspNowTimeRequest(EspNowTimeRequest(), req, sizeof(req));
  assert(!decodeEspNowTimeResponse(req, sizeof(req), &out));
  EspNowTimeRequest reqOut;
  encodeEspNowTimeResponse(resp, buf, sizeof(buf));
  assert(!decodeEspNowTimeRequest(buf, sizeof(buf), &reqOut));
}

void testSymmetricPathRecoversOffsetExactly() {
  // Device mono 10 s behind... offset = kEpoch; 2 ms each way, Hub took 300 µs.
  const int64_t offset = kEpoch;
  const int64_t t1 = 10000000;
  const int64_t t2 = t1 + 2000 + offset;
  const int64_t t3 = t2 + 300;
  const int64_t t4 = (t3 - offset) + 2000;
  const EspNowTimeExchange x = computeEspNowTimeExchange(t1, t2, t3, t4);
  assert(x.offsetUs == offset);
  assert(x.delayUs == 4000);
}

void testAsymmetryErrorIsBoundedByHalfDelay() {
  const int64_t offset = kEpoch + 777;
  const int64_t t1 = 5000000;
  const int64_t t2 = t1 + 3000 + offset;  // 3 ms out
  const int64_t t3 = t2 + 100;
  const int64_t t4 = (t3 - offset) + 1000;  // 1 ms back
  const EspNowTimeExchange x = computeEspNowTimeExchange(t1, t2, t3, t4);
  assert(x.delayUs == 4000);
  const int64_t error = x.offsetUs - offset;
  assert(error == 1000);
  assert(error <= x.delayUs / 2);
}

// The compatibility claim in EspNowTimeWire.h: a Hub that predates the time
// service feeds a TIME_REQ into its fragment reassemblers, which must drop it.
void testOldReassemblerDropsTimeMessages() {
  uint8_t scratch[512];
  EspNowReassembler reassembler(scratch, sizeof(scratch));
  ReassembledFrame frame;
  uint8_t req[kEspNowTimeReqLen];
  encodeEspNowTimeRequest(EspNowTimeRequest(), req, sizeof(req));
  assert(!reassembler.onFragment(req, sizeof(req), &frame));
  uint8_t resp[kEspNowTimeRespLen];
  encodeEspNowTimeResponse(EspNowTimeResponse(), resp, sizeof(resp));
  assert(!reassembler.onFragment(resp, sizeof(resp), &frame));
}

}  // namespace

int main() {
  testRequestRoundTrip();
  testResponseRoundTrip();
  testDecodeIsStrict();
  testSymmetricPathRecoversOffsetExactly();
  testAsymmetryErrorIsBoundedByHalfDelay();
  testOldReassemblerDropsTimeMessages();
  std::cout << "ESP-NOW time wire tests passed\n";
  return 0;
}
