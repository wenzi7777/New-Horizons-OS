#include "EspNowOtaReceiver.h"

#include <cstring>
#include <esp_now.h>
#include <Update.h>

#include "Config.h"
#include "EspNowFrame.h"
#include "JsonUtils.h"

namespace nhos {

void EspNowOtaReceiver::begin(DeviceConfig& deviceConfig, const uint8_t hubMac[6]) {
  deviceConfig_ = &deviceConfig;
  memcpy(hubMac_, hubMac, 6);
  manifestUrl_ = deviceConfig_->data().ota.manifestUrl;
}

void EspNowOtaReceiver::startBootCheck() {
  if (bootCheckStarted_ || deviceConfig_ == nullptr) return;
  bootCheckStarted_ = true;
  if (!deviceConfig_->data().ota.autoApplyOnBoot) {
    Serial.println(F("[espnow_ota] auto_ota_disabled"));
    phase_ = Phase::kFinished;
    return;
  }
  bootCheckPending_ = true;
}

void EspNowOtaReceiver::sendHubRequest(const String& json) {
  // static, not a stack local: kEspNowMaxFragCount * sizeof(EspNowFragment)
  // is ~8KB, far too much for this task's stack (the same mistake is
  // documented in EspNowPairing.h's responseFrags_ comment, where a ~4KB
  // stack array was enough to corrupt the heap). Safe as static because
  // this only ever runs on the main loop task, never from a recv callback.
  static EspNowFragment frags[kEspNowDataFragCount];
  const uint8_t count = EspNowFragmenter::fragment(
      reinterpret_cast<const uint8_t*>(json.c_str()), json.length(), 0,
      kEspNowFragTypeHubRequest, frags, kEspNowDataFragCount);
  for (uint8_t i = 0; i < count; ++i) {
    esp_now_send(hubMac_, frags[i].bytes, frags[i].len);
  }
}

void EspNowOtaReceiver::sendManifestRequest() {
  String req = "{";
  bool first = true;
  jsonStringField(req, "hub_req", "fetch_manifest", first);
  jsonStringField(req, "manifest_url", manifestUrl_, first);
  req += "}";
  sendHubRequest(req);
}

void EspNowOtaReceiver::sendRelayStartRequest() {
  String req = "{";
  bool first = true;
  jsonStringField(req, "hub_req", "ota_relay_start", first);
  jsonStringField(req, "url", pendingUrl_, first);
  jsonStringField(req, "sha256", pendingSha256_, first);
  jsonUnsignedField(req, "size", static_cast<unsigned long>(pendingSize_), first);
  req += "}";
  sendHubRequest(req);
}

void EspNowOtaReceiver::sendChunkAck(uint16_t chunkIndex) {
  const uint8_t payload[kOtaChunkAckLen] = {
      kOtaChunkAckMagic,
      static_cast<uint8_t>(chunkIndex & 0xFF),
      static_cast<uint8_t>((chunkIndex >> 8) & 0xFF),
  };
  esp_now_send(hubMac_, payload, sizeof(payload));
}

void EspNowOtaReceiver::service() {
  if (phase_ == Phase::kIdle) {
    if (bootCheckPending_) {
      bootCheckPending_ = false;
      sendManifestRequest();
      phase_ = Phase::kAwaitingManifestReply;
      requestSentMs_ = millis();
      requestAttempts_ = 1;
    }
    return;
  }

  if (phase_ == Phase::kAwaitingManifestReply || phase_ == Phase::kAwaitingRelayStartReply) {
    if (millis() - requestSentMs_ < kHubRequestResendIntervalMs) return;
    if (requestAttempts_ >= kHubRequestMaxAttempts) {
      Serial.println(F("[espnow_ota] hub_request_timeout giving_up_until_next_boot"));
      phase_ = Phase::kFinished;
      return;
    }
    if (phase_ == Phase::kAwaitingManifestReply) {
      sendManifestRequest();
    } else {
      sendRelayStartRequest();
    }
    requestSentMs_ = millis();
    ++requestAttempts_;
    return;
  }

  // kRelaying / kFinished: nothing to drive here -- kRelaying advances
  // entirely from handleOtaChunkFrame() (chunk-by-chunk, Hub-paced
  // stop-and-wait), kFinished is terminal for this boot.
}

void EspNowOtaReceiver::handleHubRequestFrame(const uint8_t* data, size_t len) {
  const String payload(reinterpret_cast<const char*>(data), len);
  String hubReq;
  jsonExtractString(payload, "hub_req", hubReq);
  if (hubReq == "fetch_manifest") {
    if (phase_ != Phase::kAwaitingManifestReply) return;  // stale/duplicate reply -- ignore
    handleManifestReply(payload);
    return;
  }
  if (hubReq == "ota_relay_start") {
    if (phase_ != Phase::kAwaitingRelayStartReply) return;
    handleRelayStartReply(payload);
    return;
  }
}

void EspNowOtaReceiver::handleManifestReply(const String& payload) {
  bool ok = false;
  jsonExtractBool(payload, "ok", ok);
  if (!ok) {
    String err;
    jsonExtractString(payload, "error", err);
    Serial.print(F("[espnow_ota] fetch_manifest_failed error="));
    Serial.println(err);
    phase_ = Phase::kFinished;
    return;
  }
  String manifest;
  if (!jsonExtractObject(payload, "manifest", manifest)) {
    Serial.println(F("[espnow_ota] fetch_manifest_reply_malformed"));
    phase_ = Phase::kFinished;
    return;
  }
  const String protocol = jsonExtractString(manifest, "protocol", "");
  const String model = jsonExtractString(manifest, "model", "");
  if (protocol != kProtocolName) {
    Serial.println(F("[espnow_ota] protocol_mismatch"));
    phase_ = Phase::kFinished;
    return;
  }
  if (model != kHardwareModel) {
    Serial.println(F("[espnow_ota] model_mismatch"));
    phase_ = Phase::kFinished;
    return;
  }
  const String version = jsonExtractString(manifest, "latest", "");
  const String url = jsonExtractString(manifest, "url", "");
  const String sha256 = jsonExtractString(manifest, "sha256", "");
  long sizeLong = 0;
  jsonExtractInt(manifest, "size", sizeLong);
  if (version.isEmpty() || url.isEmpty() || sha256.length() != 64 || sizeLong <= 0) {
    Serial.println(F("[espnow_ota] manifest_missing_fields"));
    phase_ = Phase::kFinished;
    return;
  }
  if (compareVersion(version, kFirmwareVersion) <= 0) {
    Serial.println(F("[espnow_ota] auto_ota_no_update"));
    phase_ = Phase::kFinished;
    return;
  }
  pendingVersion_ = version;
  pendingUrl_ = url;
  pendingSha256_ = sha256;
  pendingSize_ = static_cast<size_t>(sizeLong);
  Serial.print(F("[espnow_ota] update_available version="));
  Serial.println(pendingVersion_);
  sendRelayStartRequest();
  phase_ = Phase::kAwaitingRelayStartReply;
  requestSentMs_ = millis();
  requestAttempts_ = 1;
}

void EspNowOtaReceiver::handleRelayStartReply(const String& payload) {
  bool ok = false;
  jsonExtractBool(payload, "ok", ok);
  if (!ok) {
    String err;
    jsonExtractString(payload, "error", err);
    Serial.print(F("[espnow_ota] ota_relay_start_failed error="));
    Serial.println(err);
    phase_ = Phase::kFinished;
    return;
  }
  long totalChunksLong = 0;
  jsonExtractInt(payload, "total_chunks", totalChunksLong);
  if (totalChunksLong <= 0) {
    Serial.println(F("[espnow_ota] relay_start_reply_malformed"));
    phase_ = Phase::kFinished;
    return;
  }
  totalChunks_ = static_cast<uint16_t>(totalChunksLong);
  if (!Update.begin(pendingSize_)) {
    Serial.println(F("[espnow_ota] update_begin_failed"));
    phase_ = Phase::kFinished;
    return;
  }
  mbedtls_sha256_init(&shaCtx_);
  mbedtls_sha256_starts(&shaCtx_, 0);
  nextExpectedChunk_ = 0;
  hasWrittenAnyChunk_ = false;
  phase_ = Phase::kRelaying;
  Serial.print(F("[espnow_ota] relay_starting total_chunks="));
  Serial.println(totalChunks_);
}

void EspNowOtaReceiver::handleOtaChunkFrame(const uint8_t* data, size_t len) {
  if (phase_ != Phase::kRelaying) return;

  OtaChunkHeader header;
  if (!parseOtaChunkHeader(data, len, &header)) return;  // malformed -- drop
  const uint8_t* chunkData = data + kOtaChunkSubHeaderLen;

  switch (classifyOtaChunk(header.chunkIndex, nextExpectedChunk_, hasWrittenAnyChunk_)) {
    case OtaChunkDecision::kDuplicateAckOnly:
      // Our ack for this (already-written) chunk was lost and the Hub
      // resent it. Only re-ack; re-calling Update.write() here would
      // append the same bytes a second time and corrupt the image.
      sendChunkAck(header.chunkIndex);
      return;
    case OtaChunkDecision::kIgnore:
      return;  // out of order/stale -- Hub's own resend/timeout recovers
    case OtaChunkDecision::kWrite:
      break;
  }

  if (Update.write(const_cast<uint8_t*>(chunkData), header.chunkLen) != header.chunkLen) {
    abortRelay("update_write_failed");
    return;
  }
  mbedtls_sha256_update(&shaCtx_, chunkData, header.chunkLen);
  hasWrittenAnyChunk_ = true;
  ++nextExpectedChunk_;
  sendChunkAck(header.chunkIndex);

  if (nextExpectedChunk_ >= totalChunks_) {
    finishRelay();
  }
}

void EspNowOtaReceiver::finishRelay() {
  uint8_t digest[32];
  mbedtls_sha256_finish(&shaCtx_, digest);
  mbedtls_sha256_free(&shaCtx_);
  char hex[65];
  for (uint8_t i = 0; i < 32; ++i) {
    snprintf(hex + (i * 2), 3, "%02x", digest[i]);
  }
  hex[64] = '\0';
  if (!pendingSha256_.equalsIgnoreCase(hex)) {
    Update.abort();
    Serial.println(F("[espnow_ota] sha256_mismatch"));
    phase_ = Phase::kFinished;
    return;
  }
  if (!Update.end(true) || !Update.isFinished()) {
    Serial.println(F("[espnow_ota] update_end_failed"));
    phase_ = Phase::kFinished;
    return;
  }
  Serial.println(F("[espnow_ota] update_applied restarting"));
  phase_ = Phase::kFinished;
  delay(50);
  ESP.restart();
}

void EspNowOtaReceiver::abortRelay(const char* reason) {
  Update.abort();
  mbedtls_sha256_free(&shaCtx_);
  Serial.print(F("[espnow_ota] relay_aborted reason="));
  Serial.println(reason);
  phase_ = Phase::kFinished;
}

int EspNowOtaReceiver::compareVersion(const String& remote, const String& local) const {
  int r[3] = {0, 0, 0};
  int l[3] = {0, 0, 0};
  sscanf(remote.c_str(), "v%d.%d.%d", &r[0], &r[1], &r[2]);
  sscanf(local.c_str(), "v%d.%d.%d", &l[0], &l[1], &l[2]);
  for (uint8_t i = 0; i < 3; ++i) {
    if (r[i] > l[i]) return 1;
    if (r[i] < l[i]) return -1;
  }
  return 0;
}

}  // namespace nhos
