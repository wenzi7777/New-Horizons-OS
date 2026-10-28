#include "WifiScanList.h"

#include <algorithm>
#include <cstdio>

namespace nhos {

std::vector<WifiScanEntry> tidyWifiScan(const std::vector<WifiScanEntry>& raw, size_t maxCount) {
  std::vector<WifiScanEntry> out;
  for (const WifiScanEntry& entry : raw) {
    if (entry.ssid.empty()) {
      continue;
    }
    auto same = std::find_if(out.begin(), out.end(),
                             [&](const WifiScanEntry& e) { return e.ssid == entry.ssid; });
    if (same == out.end()) {
      out.push_back(entry);
    } else if (entry.rssi > same->rssi) {
      *same = entry;
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const WifiScanEntry& a, const WifiScanEntry& b) { return a.rssi > b.rssi; });
  if (out.size() > maxCount) {
    out.resize(maxCount);
  }
  return out;
}

void appendJsonEscaped(std::string& out, const std::string& value) {
  for (char c : value) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (u < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", u);
      out += buf;
    } else {
      out += c;
    }
  }
}

std::string wifiScanJson(const std::vector<WifiScanEntry>& networks, bool scanning) {
  std::string out = "{\"scanning\":";
  out += scanning ? "true" : "false";
  out += ",\"networks\":[";
  for (size_t i = 0; i < networks.size(); ++i) {
    if (i > 0) {
      out += ',';
    }
    out += "{\"ssid\":\"";
    appendJsonEscaped(out, networks[i].ssid);
    out += "\",\"rssi\":";
    out += std::to_string(networks[i].rssi);
    out += ",\"open\":";
    out += networks[i].open ? "true" : "false";
    out += '}';
  }
  out += "]}";
  return out;
}

}  // namespace nhos
