#include "UdpStreamTransport.h"

#include "ControlServer.h"
#include "FindMeClient.h"

namespace nhos {

void UdpStreamTransport::begin() { udp_.begin(kUdpStreamPort); }

void UdpStreamTransport::attach(ControlServer& control, FindMeClient& findme) {
  control_ = &control;
  findme_ = &findme;
}

bool UdpStreamTransport::ready() const {
  if (findme_->hasGateway()) {
    return !findme_->streamHost().isEmpty();
  }
  return !control_->streamHost().isEmpty();
}

bool UdpStreamTransport::sendFrame(const uint8_t* data, size_t len) {
  const String* host = &control_->streamHost();
  uint16_t port = control_->streamPort();
  if (findme_->hasGateway()) {
    host = &findme_->streamHost();
    port = findme_->streamPort();
  }
  if (host->isEmpty()) {
    return false;
  }
  udp_.beginPacket(host->c_str(), port);
  udp_.write(data, len);
  return udp_.endPacket() == 1;
}

}  // namespace nhos
