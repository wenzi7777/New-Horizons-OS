#pragma once

#include <WiFiClientSecure.h>

// Verify the OTA servers' certificates instead of trusting anyone
// (setInsecure). Shared byte-for-byte with the Hub.
//
// The trust store is the Mozilla root bundle the ESP-IDF libraries in this
// Arduino core already embed (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL;
// NetworkClientSecure references esp_crt_bundle_attach, so it is linked in
// whether we use it or not). Expiry dates are not checked
// (CONFIG_MBEDTLS_HAVE_TIME_DATE is off), so a device whose clock has not
// synced yet still updates; what is checked is that the chain ends at a
// public root and names the host -- enough that whoever can touch the
// network can no longer hand a device its firmware.
//
// A custom manifest on a self-signed server will now fail the handshake.

extern const uint8_t nhos_x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t nhos_x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace nhos {

inline void trustPublicCertificateAuthorities(WiFiClientSecure& client) {
  client.setCACertBundle(nhos_x509_crt_bundle_start,
                         static_cast<size_t>(nhos_x509_crt_bundle_end - nhos_x509_crt_bundle_start));
}

}  // namespace nhos
