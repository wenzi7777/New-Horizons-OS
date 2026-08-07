#pragma once

// StreamTransport implementation over ESP-NOW. Unlike UdpStreamTransport,
// sendFrame() does NOT transmit immediately -- it only buffers the latest
// frame (data is lossy by design; an unconsumed older frame is simply
// overwritten, same philosophy as MatrixScanner's own drop-oldest queue).
// Actual transmission only happens from service(), and only once
// EspNowPairing reports a POLL was received -- this device must stay
// purely reactive to the Hub's polling, never send on its own timer (see
// EspNowPairing.h / EspNowHubManager.h for why: real-hardware testing
// showed independently-timed sends collide even when each device
// individually "obeys" a pre-assigned time slot).

#include "Config.h"
#include "EspNowFrame.h"
#include "EspNowPairing.h"
#include "StreamTransport.h"

namespace nhos {

class EspNowStreamTransport : public StreamTransport {
 public:
  void attach(EspNowPairing& pairing);

  bool ready() const override;
  bool sendFrame(const uint8_t* data, size_t len) override;

  // Call every loop() iteration: paces out any in-flight send, and starts
  // sending the latest buffered frame once a POLL has been received.
  void service();

 private:
  EspNowPairing* pairing_ = nullptr;

  uint8_t frameBuffer_[kMaxPacketBytes];
  size_t frameBufferLen_ = 0;
  bool hasBufferedFrame_ = false;
  uint16_t frameId_ = 0;

  // Pacing state for the fragments of the frame currently being sent --
  // firing them all back-to-back saturates ESP-NOW's internal TX queue
  // (~60% ESP_ERR_ESPNOW_NO_MEM observed in spike testing without
  // pacing); see firmware/spikes/README.md.
  EspNowFragment pendingFrags_[kEspNowDataFragCount];
  uint8_t pendingCount_ = 0;
  uint8_t pendingSent_ = 0;
  uint32_t nextFragDueUs_ = 0;
  uint32_t fragIntervalUs_ = 0;
};

}  // namespace nhos
