#include "FindMeClient.h"

#include <ctype.h>
#include <WiFi.h>

namespace nhos {
namespace {
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
  gatewayId_ = storage.getString("findme_gw_id", "");
  gatewayName_ = storage.getString("findme_gw_name", "");
  // A persisted endpoint is only a unicast discovery hint. It is not proof
  // that the Gateway is reachable in this boot: treating it as attached sent
  // matrix frames before the first fresh offer, then those transient sends
  // queued orange warning flashes over the required blue search indication.
  attachedThisBoot_ = false;
  state_ = "idle";
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
    // A reconnect is a fresh attachment for the stream even when the same
    // Gateway answers: its address has to resolve again.
    ++attachGeneration_;
    discoverNow();
  }
  wasWifiConnected_ = connected;
  if (!connected) {
    return;
  }
  if (!ensureUdp()) {
    return;
  }
  uint32_t now = millis();
  if (transferActive() && transferRemainingMs(now) == 0) {
    preferredGatewayId_ = "";
    claimId_ = "";
    transferDeadlineMs_ = 0;
    transferSourceIp_ = IPAddress();
    lastError_ = "findme_transfer_timeout";
    // Still attached to the relay it was leaving: back to renewing that lease
    // (lost after 3 unanswered renewals, as always).
    offerPolicy_.restartLease();
    searchNow();
  }
  readOffers();
  if (now < cooldownUntilMs_) {
    return;
  }
  if (nextDiscoverMs_ && static_cast<int32_t>(now - nextDiscoverMs_) >= 0) {
    sendDiscover();
  }
}

void FindMeClient::setModeName(const String& mode) {
  if (!mode.isEmpty()) {
    mode_ = mode;
  }
}

void FindMeClient::discoverNow() {
  attachedThisBoot_ = false;
  nextDiscoverMs_ = 1;
  state_ = "discovering";
}

void FindMeClient::searchNow() {
  nextDiscoverMs_ = 1;
}

void FindMeClient::switchGateway(const String& preferredGatewayId, const String& claimId, uint32_t ttlMs,
                                 const IPAddress& fromIp) {
  const uint32_t effectiveTtlMs = ttlMs ? ttlMs : 30000;
  const bool alreadyAttached = hasGateway() &&
                               gatewayId_ == preferredGatewayId &&
                               claimId_ == claimId;
  if (alreadyAttached) {
    return;
  }
  const bool sameTransfer = transferActive() &&
                            preferredGatewayId_ == preferredGatewayId &&
                            claimId_ == claimId;
  preferredGatewayId_ = preferredGatewayId;
  claimId_ = claimId;
  if (static_cast<uint32_t>(fromIp) != 0) {
    transferSourceIp_ = fromIp;
  } else if (!sameTransfer) {
    transferSourceIp_ = IPAddress();
  }
  if (!sameTransfer) {
    transferDeadlineMs_ = millis() + effectiveTtlMs;
    if (transferDeadlineMs_ == 0) {
      transferDeadlineMs_ = 1;
    }
  }
  cooldownUntilMs_ = 0;
  // The attachment is kept: the stream goes on to the current relay until the
  // one asked for offers (or the transfer times out and the lease resumes).
  state_ = "discovering";
  searchNow();
}

bool FindMeClient::hasGateway() const {
  return attachedThisBoot_ && !streamHost_.isEmpty() && streamPort_ > 0;
}

bool FindMeClient::relayLost() const {
  return hasGateway() && offerPolicy_.currentGatewayLost();
}

bool FindMeClient::namesCurrentGateway() const {
  return !gatewayId_.isEmpty() && offerPolicy_.namesCurrentGateway(hasGateway(), transferActive());
}

const String& FindMeClient::streamHost() const {
  return streamHost_;
}

uint16_t FindMeClient::streamPort() const {
  return streamPort_ ? streamPort_ : kUdpStreamPort;
}

String FindMeClient::statusJson() const {
  const uint32_t now = millis();
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
  out += "\",\"preferred_gateway_id\":\"";
  out += jsonEscape(preferredGatewayId_);
  out += "\",\"transfer_active\":";
  out += transferActive() ? "true" : "false";
  out += ",\"transfer_remaining_ms\":";
  out += String(transferRemainingMs(now));
  out += ",\"last_success_ms\":";
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
  // FindMeOfferPolicy.h: searching (not attached, relay lost, or a transfer
  // running) broadcasts without naming a relay, so every relay may offer.
  // Attached, the lease is renewed unicast with the relay, naming it and the
  // priority it was taken at (its owner then knows whether another relay took
  // it over or only picked it up while the owner was away); every Nth renewal
  // is also broadcast as a beacon, so the owner hears it on any relay.
  const bool attached = hasGateway();
  const bool transferring = transferActive();
  const FindMeRoundPlan plan = offerPolicy_.startRound(attached, transferring);
  const bool searching = plan.kind == FindMeRoundKind::Search;
  hasCandidate_ = false;
  if (searching && attached && !transferring &&
      offerPolicy_.missedRounds() == kFindMeStickyMissedRounds) {
    Serial.print(F("findme_relay_lost gateway="));
    Serial.println(gatewayId_);
  }
  const String payload = encodeDiscoverJson(!searching);
  if (payload.isEmpty()) {
    lastError_ = "findme_encode_failed";
    nextDiscoverMs_ = millis() + plan.nextInMs;
    return;
  }
  bool sent = false;
  if (plan.kind != FindMeRoundKind::Renew) {
    sent = sendDiscoverTo(IPAddress(255, 255, 255, 255), payload) || sent;
    sent = sendDiscoverTo(directedBroadcast(), payload) || sent;
  }
  IPAddress knownGateway;
  const bool haveKnown = knownGateway.fromString(streamHost_);
  if (haveKnown) {
    sent = sendDiscoverTo(knownGateway, payload) || sent;
  }
  // The relay that asked for the transfer, in case broadcast does not reach
  // it (some networks drop it).
  const uint32_t source = static_cast<uint32_t>(transferSourceIp_);
  if (searching && transferring && source != 0 &&
      (!haveKnown || source != static_cast<uint32_t>(knownGateway))) {
    sent = sendDiscoverTo(transferSourceIp_, payload) || sent;
  }
  if (!sent) {
    lastError_ = "findme_discover_send_failed";
  }
  lastDiscoverMs_ = millis();
  nextDiscoverMs_ = lastDiscoverMs_ + plan.nextInMs;
  if (nextDiscoverMs_ == 0) {
    nextDiscoverMs_ = 1;
  }
  if (searching) {
    // While attached the state stays "attached": relays read it in status.
    state_ = "discovering";
    Serial.println(F("findme_discover_sent"));
  }
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

void FindMeClient::respondToProbe(const IPAddress& host) {
  // Throttled per asking address: one relay probing often must not keep
  // another (or the Desktop) from hearing the device.
  const uint32_t now = millis();
  const uint32_t ip = static_cast<uint32_t>(host);
  ProbeSlot* slot = nullptr;
  for (uint8_t i = 0; i < kProbeSlots; ++i) {
    if (probeSlots_[i].ip == ip && ip != 0) {
      slot = &probeSlots_[i];
      break;
    }
  }
  if (slot) {
    if (now - slot->lastMs < 5000) {
      return;
    }
  } else {
    // An empty slot, else the one answered longest ago.
    slot = &probeSlots_[0];
    for (uint8_t i = 0; i < kProbeSlots; ++i) {
      if (probeSlots_[i].ip == 0) {
        slot = &probeSlots_[i];
        break;
      }
      if (now - probeSlots_[i].lastMs > now - slot->lastMs) {
        slot = &probeSlots_[i];
      }
    }
    slot->ip = ip;
  }
  slot->lastMs = now;
  const String payload = encodeDiscoverJson(namesCurrentGateway());
  if (payload.isEmpty()) {
    return;
  }
  if (udp_.beginPacket(host, kDiscoveryPort)) {
    udp_.write(reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length());
    udp_.endPacket();
  }
}

void FindMeClient::readOffers() {
  uint8_t packet[512];
  while (true) {
    int packetLen = udp_.parsePacket();
    if (packetLen <= 0) {
      break;
    }
    size_t len = udp_.read(packet, min(packetLen, static_cast<int>(sizeof(packet))));

    // Handle findme_probe: gateway asks devices to announce themselves.
    // Respond without changing attach state.
    if (len > 2) {
      String raw = bytesToString(packet, len);
      raw.trim();
      if (raw.startsWith("{") && extractJsonString(raw, "type") == "findme_probe") {
        respondToProbe(udp_.remoteIP());
        continue;
      }
    }

    Offer offer = decodeOffer(packet, len);
    if (!offer.valid) {
      continue;
    }
    if (offer.version < kFindMeMinOfferVersion) {
      // The retired Python NH Gateway: never attach to it.
      continue;
    }
    const bool transferring = transferActive();
    if (transferring && offer.gatewayId != preferredGatewayId_) {
      continue;
    }
    if (!offer.accept) {
      lastError_ = offer.reason.isEmpty() ? "findme_offer_rejected" : offer.reason;
      if (!hasGateway()) {
        state_ = "rejected";
      }
      continue;
    }
    if (transferring && offer.claimId != claimId_) {
      lastError_ = "findme_transfer_offer_mismatch";
      continue;
    }
    const bool attached = hasGateway();
    const bool sameGateway = attached && offer.gatewayId == gatewayId_;
    // The Gateway a transfer asked for, or the current one refreshing itself:
    // taken at once (FindMeOfferPolicy.h).
    if (transferring || sameGateway) {
      acceptOffer(offer, udp_.remoteIP());
      offerPolicy_.onAccepted();
      continue;
    }
    // Choosing a relay (not attached, or the current one is gone): collect
    // the round's offers and take the best when the read window closes.
    if (!offerPolicy_.shouldAccept(attached, false, false, true)) {
      continue;
    }
    FindMeOfferRank rank;
    rank.priority = offer.priority;
    rank.upstreamOnline = offer.upstreamOnline;
    rank.rttMs = findMeOfferRtt(lastDiscoverMs_, millis(), offer.heldMs);
    rank.servedDevices = offer.servedDevices;
    if (!hasCandidate_ || findMeOfferBetter(rank, candidateRank_)) {
      hasCandidate_ = true;
      candidate_ = offer;
      candidateHost_ = udp_.remoteIP();
      candidateRank_ = rank;
    }
  }
  finishChoosing();
}

void FindMeClient::finishChoosing() {
  if (!hasCandidate_ || (millis() - lastDiscoverMs_) <= kOfferReadWindowMs) {
    return;
  }
  hasCandidate_ = false;
  // Attached meanwhile (a transfer, or the old relay came back): keep that.
  if (hasGateway() && !offerPolicy_.currentGatewayLost()) {
    return;
  }
  acceptOffer(candidate_, candidateHost_);
  offerPolicy_.onAccepted();
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
  attachedThisBoot_ = true;
  state_ = "attached";
  lastError_ = "";
  lastSuccessMs_ = millis();
  // The lease: renewed with this relay one interval from now.
  nextDiscoverMs_ = lastSuccessMs_ + offerPolicy_.leaseIntervalMs();
  if (nextDiscoverMs_ == 0) {
    nextDiscoverMs_ = 1;
  }
  cooldownUntilMs_ = 0;
  preferredGatewayId_ = "";
  transferDeadlineMs_ = 0;
  transferSourceIp_ = IPAddress();
  if (storage_) {
    storage_->putString("findme_host", streamHost_);
    storage_->putUInt("findme_udp_port", streamPort_);
    storage_->putString("findme_gw_id", gatewayId_);
    storage_->putString("findme_gw_name", gatewayName_);
  }
  if (!isSameGateway) {
    ++attachGeneration_;
    Serial.print(F("findme_offer_accepted gateway="));
    Serial.print(gatewayId_);
    Serial.print(F(" host="));
    Serial.print(streamHost_);
    Serial.print(F(" udp_port="));
    Serial.println(streamPort_);
  }
}

bool FindMeClient::transferActive() const {
  return !preferredGatewayId_.isEmpty() && transferDeadlineMs_ != 0;
}

uint32_t FindMeClient::transferRemainingMs(uint32_t now) const {
  if (!transferActive() || static_cast<int32_t>(now - transferDeadlineMs_) >= 0) {
    return 0;
  }
  return transferDeadlineMs_ - now;
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
  offer.version = extractJsonInt(json, "version", 0);
  offer.upstreamOnline = extractJsonString(json, "upstream_status") == "online";
  const int32_t served = extractJsonInt(json, "served_devices", 0);
  offer.servedDevices = static_cast<uint32_t>(served > 0 ? served : 0);
  const int32_t held = extractJsonInt(json, "held_ms", 0);
  offer.heldMs = static_cast<uint32_t>(held > 0 ? held : 0);
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

String FindMeClient::encodeDiscoverJson(bool withCurrentGateway) const {
  const String uid = deviceUidString();
  String out = "{";
  out += "\"type\":\"findme_discover\"";
  out += ",\"device_uid\":\"";
  out += uid;
  out += "\",\"device_name\":\"";
  out += jsonEscape(String("NHOS-") + uid);
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
  if (withCurrentGateway && !gatewayId_.isEmpty()) {
    out += ",\"current_gateway_id\":\"";
    out += jsonEscape(gatewayId_);
    out += "\",\"current_priority\":";
    out += String(priority_);
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
