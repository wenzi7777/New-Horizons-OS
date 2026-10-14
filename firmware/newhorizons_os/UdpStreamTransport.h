#pragma once

// The WiFi/UDP streaming send path behind the StreamTransport interface.
// Frames go to the Gateway FindMeClient discovered, and nowhere else.
//
// There used to be a fallback to a manually configured host/port kept by
// ControlServer (NVS stream_host / stream_port). Nothing could set it any
// more -- its set_transport branch sat behind the newer set_transport that
// switches WiFi/ESP-NOW, so it never ran -- yet a value left in NVS from
// before kept being used whenever no Gateway answered. Removed in v1.14.0;
// the stale keys are simply no longer read.

#include <Arduino.h>
#include <WiFiUdp.h>

#include "Config.h"
#include "StreamTransport.h"

namespace nhos {

class FindMeClient;

class UdpStreamTransport : public StreamTransport {
 public:
  // Sole bind of kUdpStreamPort for the wifi_udp path. Outbound stream,
  // heartbeat, and inbound control commands all share this one WiFiUDP
  // instance — a second begin(kUdpStreamPort) on another WiFiUDP steals
  // RX from serviceUdpCommand while TX still looks healthy.
  void begin();

  void attach(FindMeClient& findme);

  bool ready() const override;
  bool sendFrame(const uint8_t* data, size_t len) override;

  // Exposed for ControlServer::serviceUdpCommand() (inbound command
  // parsing on the same bound socket). Matrix/heartbeat go through
  // sendFrame() above.
  WiFiUDP& udp() { return udp_; }

 private:
  WiFiUDP udp_;
  FindMeClient* findme_ = nullptr;
};

}  // namespace nhos
