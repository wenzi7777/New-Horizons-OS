#include "EspNowStreamTransport.h"

#include <Arduino.h>
#include <cstring>
#include <esp_now.h>

namespace nhos {

namespace {
// Nominal window a paced fragment burst is spread across, generous
// relative to the Hub's kHubPollTimeoutUs (30ms in EspNowHubManager.h) so
// a normal send completes well within the Hub's timeout instead of racing
// it.
constexpr uint32_t kSendWindowUs = 15000;
}  // namespace

void EspNowStreamTransport::attach(EspNowPairing& pairing) { pairing_ = &pairing; }

bool EspNowStreamTransport::ready() const {
  return pairing_ != nullptr && pairing_->hasHub();
}

bool EspNowStreamTransport::sendFrame(const uint8_t* data, size_t len) {
  if (len > sizeof(frameBuffer_)) {
    return false;
  }
  memcpy(frameBuffer_, data, len);
  frameBufferLen_ = len;
  hasBufferedFrame_ = true;
  return true;
}

void EspNowStreamTransport::service() {
  if (pairing_ == nullptr) return;
  const uint32_t nowUs = micros();

  if (pendingSent_ < pendingCount_) {
    if (static_cast<int32_t>(nowUs - nextFragDueUs_) >= 0) {
      esp_now_send(pairing_->hubMac(), pendingFrags_[pendingSent_].bytes,
                    pendingFrags_[pendingSent_].len);
      ++pendingSent_;
      nextFragDueUs_ += fragIntervalUs_;
    }
    return;  // don't start a new frame while this one is still being paced out
  }

  if (!hasBufferedFrame_ || !pairing_->consumePollPending()) {
    return;
  }
  hasBufferedFrame_ = false;
  pendingCount_ = EspNowFragmenter::fragment(frameBuffer_, frameBufferLen_, frameId_++,
                                              kEspNowFragTypeData, pendingFrags_,
                                              kEspNowMaxFragCount);
  pendingSent_ = 0;
  fragIntervalUs_ = pendingCount_ > 0 ? kSendWindowUs / pendingCount_ : 0;
  nextFragDueUs_ = nowUs;
}

}  // namespace nhos
