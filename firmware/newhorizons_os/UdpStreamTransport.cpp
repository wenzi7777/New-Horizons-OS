#include "UdpStreamTransport.h"

#include "FindMeClient.h"

namespace nhos {

void UdpStreamTransport::begin() { udp_.begin(kUdpStreamPort); }

void UdpStreamTransport::attach(FindMeClient& findme) { findme_ = &findme; }

bool UdpStreamTransport::ready() const {
  return findme_->hasGateway() && !findme_->streamHost().isEmpty();
}

bool UdpStreamTransport::sendFrame(const uint8_t* data, size_t len) {
  if (!ready()) {
    return false;
  }
  const String& host = findme_->streamHost();
  udp_.beginPacket(host.c_str(), findme_->streamPort());
  udp_.write(data, len);
  return udp_.endPacket() == 1;
}

}  // namespace nhos
