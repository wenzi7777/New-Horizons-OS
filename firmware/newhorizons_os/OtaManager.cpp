#include "OtaManager.h"

#include <HTTPClient.h>
#include <NetworkClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include "mbedtls/sha256.h"

#include "Config.h"

namespace nhos {
namespace {
constexpr size_t kOtaChunkSize = 4096;
uint8_t gOtaChunk[kOtaChunkSize];
}

void OtaManager::begin(Storage& storage) {
  storage_ = &storage;
}

UpdateInfo OtaManager::checkUpdate(const String& manifestUrl) {
  UpdateInfo info;
  String payload;
  const String url = manifestUrl.isEmpty() ? String(kDefaultUpdateManifestUrl) : manifestUrl;
  lastPhase_ = "manifest";
  lastError_ = "";
  if (!fetchManifest(url, payload)) {
    info.error = lastError_;
    return info;
  }
  if (!parseManifest(payload, info)) {
    if (info.error.isEmpty()) {
      info.error = "manifest_invalid";
    }
    lastError_ = info.error;
    return info;
  }
  info.available = compareVersion(info.version, kFirmwareVersion) > 0;
  lastPhase_ = info.available ? "ready" : "current";
  return info;
}

bool OtaManager::applyUpdate(const String& manifestUrl) {
  UpdateInfo info = checkUpdate(manifestUrl);
  if (!info.error.isEmpty() || !info.available) {
    return false;
  }
  return downloadAndApply(info);
}

String OtaManager::lastStatusJson() const {
  String out = "{\"phase\":\"";
  out += lastPhase_;
  out += "\",\"error\":\"";
  out += lastError_;
  out += "\"}";
  return out;
}

bool OtaManager::fetchManifest(const String& url, String& payload) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, url)) {
    lastError_ = "manifest_http_begin_failed";
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    lastError_ = "manifest_http_" + String(code);
    http.end();
    return false;
  }
  payload = http.getString();
  http.end();
  return true;
}

bool OtaManager::parseManifest(const String& payload, UpdateInfo& out) {
  const String protocol = extractString(payload, "protocol");
  const String model = extractString(payload, "model");
  out.version = extractString(payload, "latest");
  out.url = extractString(payload, "url");
  out.sha256 = extractString(payload, "sha256");
  out.size = extractSize(payload, "size");
  if (protocol != kProtocolName) {
    out.error = "protocol_mismatch";
    return false;
  }
  if (model != kHardwareModel) {
    out.error = "model_mismatch";
    return false;
  }
  if (out.version.isEmpty() || out.url.isEmpty() || out.sha256.length() != 64) {
    out.error = "manifest_missing_fields";
    return false;
  }
  return true;
}

bool OtaManager::downloadAndApply(const UpdateInfo& info) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  lastPhase_ = "download";
  if (!http.begin(client, info.url)) {
    lastError_ = "firmware_http_begin_failed";
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    lastError_ = "firmware_http_" + String(code);
    http.end();
    return false;
  }
  const int total = http.getSize();
  if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN)) {
    lastError_ = "update_begin_failed";
    http.end();
    return false;
  }

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);

  NetworkClient* stream = http.getStreamPtr();
  size_t written = 0;
  while (http.connected()) {
    size_t available = stream->available();
    if (!available) {
      if (total > 0 && written >= static_cast<size_t>(total)) {
        break;
      }
      delay(1);
      continue;
    }
    if (available > kOtaChunkSize) {
      available = kOtaChunkSize;
    }
    int readBytes = stream->readBytes(gOtaChunk, available);
    if (readBytes <= 0) {
      continue;
    }
    if (Update.write(gOtaChunk, readBytes) != static_cast<size_t>(readBytes)) {
      lastError_ = "update_write_failed";
      mbedtls_sha256_free(&ctx);
      http.end();
      return false;
    }
    mbedtls_sha256_update(&ctx, gOtaChunk, readBytes);
    written += readBytes;
  }
  http.end();

  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  char hex[65];
  for (uint8_t i = 0; i < 32; ++i) {
    snprintf(hex + (i * 2), 3, "%02x", digest[i]);
  }
  hex[64] = '\0';
  if (!info.sha256.equalsIgnoreCase(hex)) {
    lastError_ = "sha256_mismatch";
    return false;
  }
  if (!Update.end(true) || !Update.isFinished()) {
    lastError_ = "update_end_failed";
    return false;
  }
  lastPhase_ = "rebooting";
  delay(100);
  ESP.restart();
  return true;
}

String OtaManager::extractString(const String& source, const char* key) const {
  String pattern = "\"" + String(key) + "\"";
  int keyIndex = source.indexOf(pattern);
  if (keyIndex < 0) {
    return "";
  }
  int colon = source.indexOf(':', keyIndex + pattern.length());
  int start = source.indexOf('"', colon + 1);
  int end = source.indexOf('"', start + 1);
  if (colon < 0 || start < 0 || end < 0) {
    return "";
  }
  return source.substring(start + 1, end);
}

size_t OtaManager::extractSize(const String& source, const char* key) const {
  String pattern = "\"" + String(key) + "\"";
  int keyIndex = source.indexOf(pattern);
  if (keyIndex < 0) {
    return 0;
  }
  int colon = source.indexOf(':', keyIndex + pattern.length());
  int end = source.indexOf(',', colon + 1);
  if (end < 0) {
    end = source.indexOf('}', colon + 1);
  }
  if (colon < 0 || end < 0) {
    return 0;
  }
  return static_cast<size_t>(source.substring(colon + 1, end).toInt());
}

int OtaManager::compareVersion(const String& remote, const String& local) const {
  int r[3] = {0, 0, 0};
  int l[3] = {0, 0, 0};
  sscanf(remote.c_str(), "v%d.%d.%d", &r[0], &r[1], &r[2]);
  sscanf(local.c_str(), "v%d.%d.%d", &l[0], &l[1], &l[2]);
  for (uint8_t i = 0; i < 3; ++i) {
    if (r[i] > l[i]) {
      return 1;
    }
    if (r[i] < l[i]) {
      return -1;
    }
  }
  return 0;
}

}  // namespace nhos
