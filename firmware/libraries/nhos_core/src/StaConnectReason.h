#pragma once

#include <cstdint>

// Why a Wi-Fi station connect attempt failed, in the words the setup page
// shows. Plain C++ (no Arduino): the input is the disconnect reason ESP-IDF
// reports (wifi_err_reason_t, carried by ARDUINO_EVENT_WIFI_STA_DISCONNECTED).

namespace nhos {

// The codes /api/status reports; the page has a translation for each.
static constexpr char kStaFailWrongPassword[] = "wrong_password";
static constexpr char kStaFailNotFound[] = "not_found";
static constexpr char kStaFailTimeout[] = "timeout";
static constexpr char kStaFailOther[] = "other";

// reason 0 means "no disconnect event was seen" and maps to timeout.
const char* staConnectFailureCode(uint8_t reason);

}  // namespace nhos
