#include "AirtimeArbiter.h"

#include <cstring>

namespace nhos {

void AirtimeArbiter::registerClaimant(AirtimeClass cls, ActivePredicate fn, void* ctx) {
  const uint8_t index = static_cast<uint8_t>(cls);
  if (index >= static_cast<uint8_t>(AirtimeClass::Count)) {
    return;
  }
  claimants_[index].fn = fn;
  claimants_[index].ctx = ctx;
}

AirtimeClass AirtimeArbiter::holder() const {
  for (uint8_t i = 0; i < static_cast<uint8_t>(AirtimeClass::Count); ++i) {
    const Claimant& claimant = claimants_[i];
    if (claimant.fn != nullptr && claimant.fn(claimant.ctx)) {
      return static_cast<AirtimeClass>(i);
    }
  }
  return AirtimeClass::Count;
}

bool AirtimeArbiter::canClaim(AirtimeClass cls) const {
  const AirtimeClass current = holder();
  if (current == AirtimeClass::Count) {
    return true;
  }
  // Strictly higher priority blocks; a class never blocks itself.
  return static_cast<uint8_t>(current) >= static_cast<uint8_t>(cls);
}

esp_err_t AirtimeArbiter::send(AirtimeClass cls, const uint8_t* mac, const uint8_t* data,
                               size_t len) {
  const esp_err_t err = esp_now_send(mac, data, len);
  const uint8_t index = static_cast<uint8_t>(cls);
  if (index < static_cast<uint8_t>(AirtimeClass::Count)) {
    Counters& counters = counters_[index];
    if (err == ESP_OK) {
      ++counters.packets;
      counters.bytes += static_cast<uint32_t>(len);
    } else {
      // ESP_OK here only means "accepted into the driver queue"; the radio
      // may still drop it. Callers that care register a send callback.
      ++counters.failures;
    }
  }
  return err;
}

bool AirtimeArbiter::beginPacedBurst(AirtimeClass cls, const uint8_t* mac,
                                     const EspNowFragment* frags, uint8_t count,
                                     uint32_t windowUs) {
  if (burstActive() || frags == nullptr || mac == nullptr || count == 0 ||
      count > kEspNowDataFragCount) {
    return false;
  }
  for (uint8_t i = 0; i < count; ++i) {
    burstFrags_[i] = frags[i];
  }
  memcpy(burstMac_, mac, sizeof(burstMac_));
  burstClass_ = cls;
  burstCount_ = count;
  burstSent_ = 0;
  burstIntervalUs_ = windowUs / count;
  burstNextDueUs_ = micros();
  return true;
}

bool AirtimeArbiter::beginPacedBurstFromBytes(AirtimeClass cls, const uint8_t* mac,
                                             const uint8_t* data, size_t len,
                                             uint8_t frameType, uint16_t frameId,
                                             uint32_t windowUs) {
  if (burstActive() || mac == nullptr || data == nullptr || len == 0) {
    return false;
  }
  const uint8_t count = EspNowFragmenter::fragment(data, len, frameId, frameType, burstFrags_,
                                                   kEspNowDataFragCount);
  if (count == 0) {
    return false;
  }
  memcpy(burstMac_, mac, sizeof(burstMac_));
  burstClass_ = cls;
  burstCount_ = count;
  burstSent_ = 0;
  burstIntervalUs_ = windowUs / count;
  burstNextDueUs_ = micros();
  return true;
}

void AirtimeArbiter::service() {
  if (!burstActive()) {
    return;
  }
  const uint32_t nowUs = micros();
  if (static_cast<int32_t>(nowUs - burstNextDueUs_) < 0) {
    return;
  }
  send(burstClass_, burstMac_, burstFrags_[burstSent_].bytes, burstFrags_[burstSent_].len);
  ++burstSent_;
  burstNextDueUs_ += burstIntervalUs_;
  if (!burstActive()) {
    burstCount_ = 0;
    burstSent_ = 0;
  }
}

const char* AirtimeArbiter::className(AirtimeClass cls) {
  switch (cls) {
    case AirtimeClass::OtaRelay: return "ota_relay";
    case AirtimeClass::CommandResponse: return "command_response";
    case AirtimeClass::Pairing: return "pairing";
    case AirtimeClass::SensorStream: return "sensor_stream";
    case AirtimeClass::Count:
    default: return "idle";
  }
}

String AirtimeArbiter::statusJson() const {
  String json = "{\"holder\":\"";
  json += className(holder());
  json += "\",\"burst_active\":";
  json += burstActive() ? "true" : "false";
  json += ",\"burst_remaining\":";
  json += String(burstActive() ? burstCount_ - burstSent_ : 0);
  json += ",\"classes\":[";
  for (uint8_t i = 0; i < static_cast<uint8_t>(AirtimeClass::Count); ++i) {
    if (i != 0) {
      json += ",";
    }
    json += "{\"name\":\"";
    json += className(static_cast<AirtimeClass>(i));
    json += "\",\"registered\":";
    json += claimants_[i].fn != nullptr ? "true" : "false";
    json += ",\"packets\":";
    json += String(counters_[i].packets);
    json += ",\"bytes\":";
    json += String(counters_[i].bytes);
    json += ",\"failures\":";
    json += String(counters_[i].failures);
    json += "}";
  }
  json += "]}";
  return json;
}

String AirtimeArbiter::netText() const {
  String out = "CLASS               PRI  CLAIMANT   PACKETS      BYTES  FAILURES\n";
  for (uint8_t i = 0; i < static_cast<uint8_t>(AirtimeClass::Count); ++i) {
    String name(className(static_cast<AirtimeClass>(i)));
    while (name.length() < 20) {
      name += ' ';
    }
    out += name;
    out += String(i);
    out += "    ";
    out += claimants_[i].fn != nullptr ? "yes      " : "-        ";
    String packets(counters_[i].packets);
    while (packets.length() < 9) {
      packets = " " + packets;
    }
    out += packets;
    String bytes(counters_[i].bytes);
    while (bytes.length() < 11) {
      bytes = " " + bytes;
    }
    out += bytes;
    String failures(counters_[i].failures);
    while (failures.length() < 10) {
      failures = " " + failures;
    }
    out += failures;
    out += '\n';
  }
  out += "\nholder=";
  out += className(holder());
  out += '\n';
  return out;
}

}  // namespace nhos
