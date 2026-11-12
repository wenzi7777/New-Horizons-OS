#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>

#include "Config.h"
#include "FindMeOfferPolicy.h"
#include "FindMeOfferSelect.h"
#include "Storage.h"
#include "WifiManager.h"

namespace nhos {

class FindMeClient {
 public:
  void begin(Storage& storage, WifiManager& wifi, const uint8_t uid[6]);
  void service();
  void setModeName(const String& mode);
  void discoverNow();
  void switchGateway(const String& preferredGatewayId, const String& claimId, uint32_t ttlMs);

  bool hasGateway() const;
  // Bumped every time the stream moves to a different Gateway (never for the
  // same one re-offering), so a caller can tell a fresh attachment -- whose
  // first sends may fail while the new host's address resolves -- from a
  // stream that has been up for a while.
  uint32_t attachGeneration() const { return attachGeneration_; }
  const String& streamHost() const;
  uint16_t streamPort() const;
  String statusJson() const;
  void recordHeartbeat(uint32_t sentMs, const String& error);

 private:
  struct Offer {
    bool valid = false;
    bool accept = false;
    String gatewayId;
    String gatewayName;
    String claimId;
    String reason;
    uint16_t udpPort = kUdpStreamPort;
    int priority = 0;
    uint32_t ttlMs = 0;
    int32_t version = 0;
    bool upstreamOnline = false;
    uint32_t servedDevices = 0;
    // How long the relay held this answer back on purpose (not network delay).
    uint32_t heldMs = 0;
  };

  bool ensureUdp();
  void sendDiscover();
  bool sendDiscoverTo(const IPAddress& host, const String& payload);
  IPAddress directedBroadcast() const;
  void readOffers();
  void acceptOffer(const Offer& offer, const IPAddress& host);
  bool transferActive() const;
  uint32_t transferRemainingMs(uint32_t now) const;
  Offer decodeOffer(const uint8_t* data, size_t len) const;
  String deviceUidString() const;
  String jsonEscape(const String& value) const;
  String encodeDiscoverJson(bool withCurrentGateway) const;
  void finishChoosing();
  String extractJsonString(const String& json, const char* key) const;
  int32_t extractJsonInt(const String& json, const char* key, int32_t fallback) const;
  bool extractJsonBool(const String& json, const char* key, bool fallback) const;

  void respondToProbe(const IPAddress& host);
  String encodeDiscoverJsonWithCurrentGateway() const;

  Storage* storage_ = nullptr;
  WifiManager* wifi_ = nullptr;
  WiFiUDP udp_;
  bool udpStarted_ = false;
  bool wasWifiConnected_ = false;
  bool attachedThisBoot_ = false;
  uint8_t uid_[6] = {0};
  String mode_ = "normal";
  String state_ = "idle";
  String gatewayId_;
  String gatewayName_;
  String streamHost_;
  String claimId_;
  String preferredGatewayId_;
  uint32_t transferDeadlineMs_ = 0;
  String lastError_;
  uint16_t streamPort_ = kUdpStreamPort;
  int priority_ = 0;
  uint16_t seq_ = 1;
  uint32_t nextDiscoverMs_ = 0;
  uint32_t lastDiscoverMs_ = 0;
  // The best offer of the current round while choosing a relay
  // (FindMeOfferSelect.h); taken once the read window closes.
  bool hasCandidate_ = false;
  Offer candidate_;
  IPAddress candidateHost_;
  FindMeOfferRank candidateRank_;
  // Sticks an attached device to its Gateway (see FindMeOfferPolicy.h).
  FindMeOfferPolicy offerPolicy_;
  uint32_t attachGeneration_ = 0;
  uint32_t lastSuccessMs_ = 0;
  uint32_t lastHeartbeatMs_ = 0;
  uint32_t cooldownUntilMs_ = 0;
  uint32_t lastProbeResponseMs_ = 0;
  String heartbeatLastError_;
};

}  // namespace nhos
