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
  // Mirrors the existing `streamUdp.begin(nhos::kUdpStreamPort)` call in
  // newhorizons_os.ino's setup().
  void begin();

  void attach(ControlServer& control, FindMeClient& findme);

  bool ready() const override;
  bool sendFrame(const uint8_t* data, size_t len) override;

  // Exposed for the two remaining UDP-specific call sites that need the
  // raw socket directly: ControlServer::serviceUdpCommand() (inbound
  // command parsing) and MatrixScanner's queued-packet path goes through
  // sendFrame() above instead, not this.
  WiFiUDP& udp() { return udp_; }

 private:
  WiFiUDP udp_;
  ControlServer* control_ = nullptr;
  FindMeClient* findme_ = nullptr;
};

}  // namespace nhos
