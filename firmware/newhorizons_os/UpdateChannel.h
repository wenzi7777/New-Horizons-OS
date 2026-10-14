#pragma once

#include <cstring>

// Update channels: which OTA manifest a device follows. Plain C++ (no
// Arduino), shared byte-for-byte with the Hub.
//
// Each board publishes two manifests next to each other:
//
//   <base>-latest.json   stable -- the only one firmware older than the
//                        channels ever reads, so it must never point at a
//                        prerelease (generate_arduino_manifest.py refuses)
//   <base>-beta.json     beta   -- the newest prerelease, or the newest
//                        stable when that is newer, so beta devices get
//                        every release too
//
// Leaving beta never downgrades: a device on v1.14.0-beta.2 that switches
// back to stable simply sees v1.13.x as not newer (compareVersions) and
// stays put until a stable release overtakes it.

namespace nhos {

static constexpr char kUpdateChannelStable[] = "stable";
static constexpr char kUpdateChannelBeta[] = "beta";
// Comma-separated, in the shape ConfigRegistry's Enum entries use.
static constexpr char kUpdateChannelNames[] = "stable,beta";

inline bool isValidUpdateChannel(const char* channel) {
  return channel != nullptr &&
         (std::strcmp(channel, kUpdateChannelStable) == 0 ||
          std::strcmp(channel, kUpdateChannelBeta) == 0);
}

// Manifest file suffix appended to a board's manifest base URL. Anything
// that is not a known channel falls back to stable: a corrupt config must
// never move a device onto prereleases.
inline const char* updateChannelManifestSuffix(const char* channel) {
  if (channel != nullptr && std::strcmp(channel, kUpdateChannelBeta) == 0) {
    return "-beta.json";
  }
  return "-latest.json";
}

}  // namespace nhos
