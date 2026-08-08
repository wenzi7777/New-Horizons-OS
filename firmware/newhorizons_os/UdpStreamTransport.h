#pragma once

// Mechanical extraction of the existing WiFi/UDP streaming send path
// (previously inline in newhorizons_os.ino's scanAndStreamIfDue() /
// sendHeartbeatIfDue()) behind the StreamTransport interface. Behavior is
// unchanged: prefer FindMeClient's discovered gateway host/port, falling
// back to ControlServer's manually-configured direct-ingest host/port.

#include <Arduino.h>
#include <WiFiUdp.h>

#include "Config.h"
#include "StreamTransport.h"

namespace nhos {

class ControlServer;
class FindMeClient;

class UdpStreamTransport : public StreamTransport {
 public:
  // Sole bind of kUdpStreamPort for the wifi_udp path. Outbound stream,
  // heartbeat, and inbound control commands all share this one WiFiUDP
  // instance — a second begin(kUdpStreamPort) on another WiFiUDP steals
  // RX from serviceUdpCommand while TX still looks healthy.
  void begin();

  void attach(ControlServer& control, FindMeClient& findme);

  bool ready() const override;
  bool sendFrame(const uint8_t* data, size_t len) override;

  // Exposed for ControlServer::serviceUdpCommand() (inbound command
  // parsing on the same bound socket). Matrix/heartbeat go through
  // sendFrame() above.
  WiFiUDP& udp() { return udp_; }

 private:
  WiFiUDP udp_;
  ControlServer* control_ = nullptr;
  FindMeClient* findme_ = nullptr;
};

}  // namespace nhos
