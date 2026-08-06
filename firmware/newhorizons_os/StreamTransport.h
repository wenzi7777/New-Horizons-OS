#pragma once

// Transport-agnostic sink for outbound NHO/Arduino/1 byte buffers (matrix
// frames and heartbeats). MatrixScanner/newhorizons_os.ino depend on this
// abstraction instead of a concrete WiFiUDP + host/port pair, so the same
// PacketBuilder/MatrixScanner code works unchanged whether the active
// transport is WiFi/UDP (UdpStreamTransport) or ESP-NOW
// (EspNowStreamTransport).

#include <cstddef>
#include <cstdint>

namespace nhos {

class StreamTransport {
 public:
  virtual ~StreamTransport() = default;

  // Whether this transport currently has somewhere to send to (UDP: a
  // discovered/configured host; ESP-NOW: paired with a Hub). Callers that
  // hold data pending until a destination exists (see
  // MatrixScanner::sendQueuedPacket()) must check this before consuming a
  // queued packet -- matches the original UDP-only code's behavior of
  // leaving a queued packet in place rather than discarding it when no
  // host was configured.
  virtual bool ready() const = 0;

  // Sends a fully-built NHO/Arduino/1 byte buffer. Returns true if the
  // send was attempted successfully (matches the existing
  // streamUdp.endPacket()==1 success semantics -- not a delivery
  // guarantee; this protocol is fire-and-forget by design).
  virtual bool sendFrame(const uint8_t* data, size_t len) = 0;
};

}  // namespace nhos
