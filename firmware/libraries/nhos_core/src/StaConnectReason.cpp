#include "StaConnectReason.h"

namespace nhos {

const char* staConnectFailureCode(uint8_t reason) {
  switch (reason) {
    case 0:
      return kStaFailTimeout;
    // A wrong WPA2 key shows up as a handshake that never completes, not as
    // an explicit auth failure, so all of these mean "check the password".
    case 14:   // MIC_FAILURE
    case 15:   // 4WAY_HANDSHAKE_TIMEOUT
    case 202:  // AUTH_FAIL
    case 204:  // HANDSHAKE_TIMEOUT
      return kStaFailWrongPassword;
    case 201:  // NO_AP_FOUND
    case 210:  // NO_AP_FOUND_W_COMPATIBLE_SECURITY
    case 211:  // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
    case 212:  // NO_AP_FOUND_IN_RSSI_THRESHOLD
      return kStaFailNotFound;
    default:
      return kStaFailOther;
  }
}

}  // namespace nhos
