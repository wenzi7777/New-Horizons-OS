// WifiScanList / StaConnectReason: what the SoftAP setup page lists and how
// it explains a failed connect.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "StaConnectReason.h"
#include "WifiScanList.h"

namespace {
int failures = 0;
void expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}
}  // namespace

int main() {
  using nhos::WifiScanEntry;
  const std::vector<WifiScanEntry> raw = {
      {"Lab", -70, false}, {"", -30, true},     {"Guest", -60, true},
      {"Lab", -45, false}, {"Far", -90, false}, {"Lab", -80, false},
  };
  const auto tidy = nhos::tidyWifiScan(raw, 16);
  expect(tidy.size() == 3, "hidden SSIDs dropped and duplicates merged");
  expect(tidy[0].ssid == "Lab" && tidy[0].rssi == -45, "a duplicate keeps its strongest reading");
  expect(tidy[1].ssid == "Guest" && tidy[2].ssid == "Far", "strongest first");
  expect(nhos::tidyWifiScan(raw, 2).size() == 2, "capped at maxCount");

  const std::string json = nhos::wifiScanJson({{"a\"b\\c", -50, true}}, false);
  expect(json == "{\"scanning\":false,\"networks\":[{\"ssid\":\"a\\\"b\\\\c\",\"rssi\":-50,\"open\":true}]}",
         "SSID is JSON-escaped");
  expect(nhos::wifiScanJson({}, true) == "{\"scanning\":true,\"networks\":[]}", "empty list while scanning");
  std::string ctl;
  nhos::appendJsonEscaped(ctl, std::string("x\ny"));
  expect(ctl == "x\\u000ay", "control bytes escaped");

  expect(std::strcmp(nhos::staConnectFailureCode(0), "timeout") == 0, "no event is a timeout");
  expect(std::strcmp(nhos::staConnectFailureCode(15), "wrong_password") == 0, "4-way handshake timeout");
  expect(std::strcmp(nhos::staConnectFailureCode(204), "wrong_password") == 0, "handshake timeout");
  expect(std::strcmp(nhos::staConnectFailureCode(201), "not_found") == 0, "no AP found");
  expect(std::strcmp(nhos::staConnectFailureCode(8), "other") == 0, "anything else");

  if (failures == 0) {
    std::printf("setup portal logic: all passed\n");
  }
  return failures == 0 ? 0 : 1;
}
