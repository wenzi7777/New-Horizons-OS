#include "EspNowTimeWire.h"

namespace nhos {

namespace {

void putU64(uint8_t* out, uint64_t v) {
  for (uint8_t i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
}

uint64_t getU64(const uint8_t* in) {
  uint64_t v = 0;
  for (uint8_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(in[i]) << (8 * i);
  return v;
}

}  // namespace

size_t encodeEspNowTimeRequest(const EspNowTimeRequest& req, uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < kEspNowTimeReqLen) return 0;
  out[0] = kEspNowTimeReqMagic;
  out[1] = kEspNowTimeWireVersion;
  out[2] = req.seq;
  out[3] = 0;
  putU64(out + 4, static_cast<uint64_t>(req.t1MonoUs));
  return kEspNowTimeReqLen;
}

size_t encodeEspNowTimeResponse(const EspNowTimeResponse& resp, uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < kEspNowTimeRespLen) return 0;
  out[0] = kEspNowTimeRespMagic;
  out[1] = kEspNowTimeWireVersion;
  out[2] = resp.seq;
  out[3] = static_cast<uint8_t>((resp.hubSynced ? kEspNowTimeFlagHubSynced : 0) |
                                ((resp.hubSource & 0x0f) << 4));
  putU64(out + 4, static_cast<uint64_t>(resp.t1MonoUs));
  putU64(out + 12, static_cast<uint64_t>(resp.t2EpochUs));
  putU64(out + 20, static_cast<uint64_t>(resp.t3EpochUs));
  out[28] = static_cast<uint8_t>(resp.hubUncertaintyMs & 0xff);
  out[29] = static_cast<uint8_t>(resp.hubUncertaintyMs >> 8);
  out[30] = 0;
  out[31] = 0;
  return kEspNowTimeRespLen;
}

bool decodeEspNowTimeRequest(const uint8_t* data, size_t len, EspNowTimeRequest* out) {
  if (data == nullptr || out == nullptr || len != kEspNowTimeReqLen) return false;
  if (data[0] != kEspNowTimeReqMagic || data[1] != kEspNowTimeWireVersion) return false;
  out->seq = data[2];
  out->t1MonoUs = static_cast<int64_t>(getU64(data + 4));
  return true;
}

bool decodeEspNowTimeResponse(const uint8_t* data, size_t len, EspNowTimeResponse* out) {
  if (data == nullptr || out == nullptr || len != kEspNowTimeRespLen) return false;
  if (data[0] != kEspNowTimeRespMagic || data[1] != kEspNowTimeWireVersion) return false;
  out->seq = data[2];
  out->hubSynced = (data[3] & kEspNowTimeFlagHubSynced) != 0;
  out->hubSource = static_cast<uint8_t>(data[3] >> 4);
  out->t1MonoUs = static_cast<int64_t>(getU64(data + 4));
  out->t2EpochUs = static_cast<int64_t>(getU64(data + 12));
  out->t3EpochUs = static_cast<int64_t>(getU64(data + 20));
  out->hubUncertaintyMs = static_cast<uint16_t>(data[28] | (static_cast<uint16_t>(data[29]) << 8));
  return true;
}

EspNowTimeExchange computeEspNowTimeExchange(int64_t t1MonoUs, int64_t t2EpochUs,
                                             int64_t t3EpochUs, int64_t t4MonoUs) {
  EspNowTimeExchange x;
  // Each half-difference is computed before summing: the operands are
  // epoch-scale (~1.7e15 µs), and pairing them this way keeps every
  // intermediate far from int64 limits.
  x.offsetUs = ((t2EpochUs - t1MonoUs) + (t3EpochUs - t4MonoUs)) / 2;
  x.delayUs = (t4MonoUs - t1MonoUs) - (t3EpochUs - t2EpochUs);
  return x;
}

}  // namespace nhos
