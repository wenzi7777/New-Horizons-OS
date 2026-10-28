#pragma once

#include <cstddef>
#include <string>
#include <vector>

// The network list the SoftAP setup page shows (device and Hub). Plain C++
// (no Arduino) so tests_native can check it; each product feeds it the
// results of WiFi.scanNetworks() and serves the JSON as /api/scan.

namespace nhos {

struct WifiScanEntry {
  std::string ssid;
  int rssi = 0;
  bool open = false;
};

// Hidden networks (empty SSID) are dropped, every SSID appears once (a mesh
// or several APs broadcast the same name: the strongest wins), strongest
// first, at most maxCount.
std::vector<WifiScanEntry> tidyWifiScan(const std::vector<WifiScanEntry>& raw, size_t maxCount);

// {"scanning":false,"networks":[{"ssid":"...","rssi":-48,"open":false}]}
std::string wifiScanJson(const std::vector<WifiScanEntry>& networks, bool scanning);

// JSON string body (no surrounding quotes): escapes ", \ and control bytes.
void appendJsonEscaped(std::string& out, const std::string& value);

}  // namespace nhos
