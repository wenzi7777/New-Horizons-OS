#include "FindMeClient.h"

#include <ctype.h>
#include <WiFi.h>

namespace nhos {
namespace {
constexpr uint32_t kDiscoverIntervalMs = 3000;
constexpr uint32_t kOfferReadWindowMs = 600;

String bytesToString(const uint8_t* data, size_t len) {
  String out;
  out.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    out += static_cast<char>(data[i]);
  }
  return out;
}
}

void FindMeClient::begin(Storage& storage, WifiManager& wifi, const uint8_t uid[6]) {
  storage_ = &storage;
  wifi_ = &wifi;
  memcpy(uid_, uid, sizeof(uid_));
  streamHost_ = storage.getString("findme_host", "");
  streamPort_ = static_cast<uint16_t>(storage.getUInt("findme_udp_port", kUdpStreamPort));
  gatewayId_ = storage.getString("findme_gateway_id", "");
  gatewayName_ = storage.getString("findme_gateway_name", "");
  state_ = streamHost_.isEmpty() ? "idle" : "attached";
  wasWifiConnected_ = wifi_->isConnected();
  if (wasWifiConnected_) {
    discoverNow();
  }
}

void FindMeClient::service() {
  if (!wifi_) {
    return;
  }
  const bool connected = wifi_->isConnected();
  if (connected && !wasWifiConnected_) {
    discoverNow();
  }
  wasWifiConnected_ = connected;
  if (!connected) {
    return;
  }
  if (!ensureUdp()) {
    return;
  }
  readOffers();
  uint32_t now = millis();
  if (now < cooldownUntilMs_) {
    return;
  }
  if (nextDiscoverMs_ && now >= nextDiscoverMs_) {
    sendDiscover();
  }
}

void FindMeClient::setModeName(const String& mode) {
  if (!mode.isEmpty()) {
    mode_ = mode;
  }
}

void FindMeClient::discoverNow() {
  nextDiscoverMs_ = 1;
  state_ = "discovering";
}

void FindMeClient::switchGateway(const String& preferredGatewayId, const String& claimId, uint32_t ttlMs) {
  (void)ttlMs;
  preferredGatewayId_ = preferredGatewayId;
  claimId_ = claimId;
  cooldownUntilMs_ = 0;
  discoverNow();
}

bool FindMeClient::hasGateway() const {
  return !streamHost_.isEmpty() && streamPort_ > 0;
}

String FindMeClient::streamHost() const {
  return streamHost_;
}

uint16_t FindMeClient::streamPort() const {
  return streamPort_ ? streamPort_ : kUdpStreamPort;
}

String FindMeClient::statusJson() const {
  String out = "{";
  out += "\"state\":\"";
  out += jsonEscape(state_);
  out += "\",\"gateway_id\":\"";
  out += jsonEscape(gatewayId_);
  out += "\",\"gateway_name\":\"";
  out += jsonEscape(gatewayName_);
  out += "\",\"host\":\"";
  out += jsonEscape(streamHost_);
  out += "\",\"udp_port\":";
  out += String(streamPort());
  out += ",\"priority\":";
  out += String(priority_);
  out += ",\"claim_id\":\"";
  out += jsonEscape(claimId_);
  out += "\",\"last_success_ms\":";
  out += String(lastSuccessMs_);
  out += ",\"last_error\":\"";
  out += jsonEscape(lastError_);
  out += "\",\"heartbeat_interval_ms\":";
  out += String(kHeartbeatIntervalMs);
  out += ",\"last_heartbeat_ms\":";
  out += String(lastHeartbeatMs_);
  out += ",\"heartbeat_last_error\":\"";
  out += jsonEscape(heartbeatLastError_);
  out += "\"}";
  return out;
}

void FindMeClient::recordHeartbeat(uint32_t sentMs, const String& error) {
  lastHeartbeatMs_ = sentMs;
  heartbeatLastError_ = error;
}

bool FindMeClient::ensureUdp() {
  if (udpStarted_) {
    return true;
  }
  udpStarted_ = udp_.begin(kDiscoveryPort);
  if (!udpStarted_) {
    lastError_ = "findme_udp_begin_failed";
  }
  return udpStarted_;
}

void FindMeClient::sendDiscover() {
  String payload = encodeDiscoverJson();
  if (payload.isEmpty()) {
    lastError_ = "findme_encode_failed";
    nextDiscoverMs_ = millis() + kDiscoverIntervalMs;
    return;
  }
  bool sent = sendDiscoverTo(IPAddress(255, 255, 255, 255), payload);
  sent = sendDiscoverTo(directedBroadcast(), payload) || sent;
  IPAddress knownGateway;
  if (knownGateway.fromString(streamHost_)) {
    sent = sendDiscoverTo(knownGateway, payload) || sent;
  }
  if (!sent) {
    lastError_ = "findme_discover_send_failed";
  }
  lastDiscoverMs_ = millis();
  nextDiscoverMs_ = lastDiscoverMs_ + kDiscoverIntervalMs;
  state_ = "discovering";
  Serial.println(F("findme_discover_sent"));
}

bool FindMeClient::sendDiscoverTo(const IPAddress& host, const String& payload) {
  if (!payload.length()) {
    return false;
  }
  if (!udp_.beginPacket(host, kDiscoveryPort)) {
    return false;
  }
  udp_.write(reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length());
  return udp_.endPacket() == 1;
}

IPAddress FindMeClient::directedBroadcast() const {
  const IPAddress local = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  return IPAddress(
      static_cast<uint8_t>(local[0] | (0xff ^ mask[0])),
      static_cast<uint8_t>(local[1] | (0xff ^ mask[1])),
      static_cast<uint8_t>(local[2] | (0xff ^ mask[2])),
      static_cast<uint8_t>(local[3] | (0xff ^ mask[3])));
}

void FindMeClient::readOffers() {
  uint8_t packet[512];
  while (true) {
    int packetLen = udp_.parsePacket();
    if (packetLen <= 0) {
      break;
    }
    size_t len = udp_.read(packet, min(packetLen, static_cast<int>(sizeof(packet))));
    Offer offer = decodeOffer(packet, len);
    if (!offer.valid) {
      continue;
    }
    if (!offer.accept) {
      lastError_ = offer.reason.isEmpty() ? "findme_offer_rejected" : offer.reason;
      state_ = "rejected";
      continue;
    }
    if (!hasGateway() || offer.priority >= priority_ || (millis() - lastDiscoverMs_) <= kOfferReadWindowMs) {
      acceptOffer(offer, udp_.remoteIP());
    }
  }
}

void FindMeClient::acceptOffer(const Offer& offer, const IPAddress& host) {
  const String nextHost = host.toString();
  const uint16_t nextPort = offer.udpPort ? offer.udpPort : kUdpStreamPort;
  const bool isSameGateway = hasGateway() &&
                             streamHost_ == nextHost &&
                             streamPort_ == nextPort &&
                             gatewayId_ == offer.gatewayId;
  gatewayId_ = offer.gatewayId;
  gatewayName_ = offer.gatewayName;
  claimId_ = offer.claimId;
  streamHost_ = nextHost;
  streamPort_ = nextPort;
  priority_ = offer.priority;
  state_ = "attached";
  lastError_ = "";
  lastSuccessMs_ = millis();
  nextDiscoverMs_ = 0;
  cooldownUntilMs_ = 0;
  preferredGatewayId_ = "";
  if (storage_) {
    storage_->putString("findme_host", streamHost_);
    storage_->putUInt("findme_udp_port", streamPort_);
    storage_->putString("findme_gateway_id", gatewayId_);
    storage_->putString("findme_gateway_name", gatewayName_);
  }
  if (!isSameGateway) {
    Serial.print(F("findme_offer_accepted gateway="));
    Serial.print(gatewayId_);
    Serial.print(F(" host="));
    Serial.print(streamHost_);
    Serial.print(F(" udp_port="));
    Serial.println(streamPort_);
  }
}

FindMeClient::Offer FindMeClient::decodeOffer(const uint8_t* data, size_t len) const {
  Offer offer;
  if (!data || !len) {
    return offer;
  }
  String json = bytesToString(data, len);
  json.trim();
  if (!json.startsWith("{")) {
    return offer;
  }
  if (extractJsonString(json, "type") != "findme_offer") {
    return offer;
  }
  const String offeredUid = extractJsonString(json, "device_uid");
  if (!offeredUid.isEmpty() && offeredUid != deviceUidString()) {
    return offer;
  }
  offer.valid = true;
  offer.accept = extractJsonBool(json, "accept", false);
  offer.gatewayId = extractJsonString(json, "gateway_id");
  offer.gatewayName = extractJsonString(json, "gateway_name");
  offer.claimId = extractJsonString(json, "claim_id");
  offer.reason = extractJsonString(json, "reason");
  offer.udpPort = static_cast<uint16_t>(extractJsonInt(json, "udp_port", kUdpStreamPort));
  offer.priority = extractJsonInt(json, "priority", 0);
  int32_t ttl = extractJsonInt(json, "ttl_ms", 0);
  offer.ttlMs = static_cast<uint32_t>(ttl > 0 ? ttl : 0);
  return offer;
}

String FindMeClient::deviceUidString() const {
  char out[13];
  snprintf(out, sizeof(out), "%02X%02X%02X%02X%02X%02X", uid_[0], uid_[1], uid_[2], uid_[3], uid_[4], uid_[5]);
  return String(out);
}

String FindMeClient::jsonEscape(const String& value) const {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value.charAt(i);
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

String FindMeClient::encodeDiscoverJson() const {
  const String uid = deviceUidString();
  String out = "{";
  out += "\"type\":\"findme_discover\"";
  out += ",\"device_uid\":\"";
  out += uid;
  out += "\",\"device_name\":\"";
  out += jsonEscape(String("New Horizons OS-") + uid);
  out += "\",\"mode\":\"";
  out += jsonEscape(mode_);
  out += "\",\"firmware_version\":\"";
  out += jsonEscape(kFirmwareVersion);
  out += "\",\"hardware_model\":\"";
  out += jsonEscape(kHardwareModel);
  out += "\",\"wifi_rssi\":";
  out += String(WiFi.RSSI());
  out += ",\"protocol\":\"";
  out += jsonEscape(kProtocolName);
  out += "\"";
  if (!preferredGatewayId_.isEmpty()) {
    out += ",\"preferred_gateway_id\":\"";
    out += jsonEscape(preferredGatewayId_);
    out += "\",\"claim_id\":\"";
    out += jsonEscape(claimId_);
    out += "\"";
  }
  out += "}";
  return out;
}

String FindMeClient::extractJsonString(const String& json, const char* key) const {
  const String marker = String("\"") + key + "\":";
  int index = json.indexOf(marker);
  if (index < 0) {
    return "";
  }
  index += marker.length();
  while (index < static_cast<int>(json.length()) && isspace(static_cast<unsigned char>(json.charAt(index)))) {
    ++index;
  }
  if (index >= static_cast<int>(json.length()) || json.charAt(index) != '"') {
    return "";
  }
  ++index;
  String out;
  bool escaped = false;
  for (; index < static_cast<int>(json.length()); ++index) {
    char c = json.charAt(index);
    if (escaped) {
      out += c;
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      break;
    }
    out += c;
  }
  return out;
}

int32_t FindMeClient::extractJsonInt(const String& json, const char* key, int32_t fallback) const {
  const String marker = String("\"") + key + "\":";
  int index = json.indexOf(marker);
  if (index < 0) {
    return fallback;
  }
  index += marker.length();
  while (index < static_cast<int>(json.length()) && isspace(static_cast<unsigned char>(json.charAt(index)))) {
    ++index;
  }
  int end = index;
  if (end < static_cast<int>(json.length()) && (json.charAt(end) == '-' || json.charAt(end) == '+')) {
    ++end;
  }
  while (end < static_cast<int>(json.length()) && isdigit(static_cast<unsigned char>(json.charAt(end)))) {
    ++end;
  }
  if (end == index) {
    return fallback;
  }
  return static_cast<int32_t>(json.substring(index, end).toInt());
}

bool FindMeClient::extractJsonBool(const String& json, const char* key, bool fallback) const {
  const String marker = String("\"") + key + "\":";
  int index = json.indexOf(marker);
  if (index < 0) {
    return fallback;
  }
  index += marker.length();
  while (index < static_cast<int>(json.length()) && isspace(static_cast<unsigned char>(json.charAt(index)))) {
    ++index;
  }
  if (json.substring(index).startsWith("true")) {
    return true;
  }
  if (json.substring(index).startsWith("false")) {
    return false;
  }
  int32_t numeric = extractJsonInt(json, key, fallback ? 1 : 0);
  return numeric != 0;
}

}  // namespace nhos
