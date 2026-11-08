#pragma once

#include <Arduino.h>

#include "App.h"

namespace nhos {

// Mirrors the manifest block of a .nha package. Fixed-width on purpose: the
// registry holds one of these per installed package and must not grow the heap
// per entry.
struct AppPackageManifest {
  static constexpr uint8_t kIdLen = 16;       // 15 chars + NUL, capped by SPIFFS
  static constexpr uint8_t kNameLen = 24;
  static constexpr uint8_t kVersionLen = 12;
  static constexpr uint8_t kAuthorLen = 24;
  static constexpr uint8_t kSummaryLen = 64;
  static constexpr uint8_t kCategoryLen = 16;
  static constexpr uint8_t kSha256Len = 65;   // 64 hex + NUL

  char id[kIdLen] = {0};
  char name[kNameLen] = {0};
  char version[kVersionLen] = {0};
  char author[kAuthorLen] = {0};
  char summary[kSummaryLen] = {0};
  char category[kCategoryLen] = {0};
  char minOs[kVersionLen] = {0};
  char sha256[kSha256Len] = {0};
  uint16_t capabilities = kAppCapNone;
  uint8_t kind = 0;             // 0 = flow graph
  uint32_t declaredBudgetUs = 0;  // 0 -> use the slot's allocation
};

// Package "kind" discriminator, so a kind this firmware does not understand is
// refused rather than misread as a flow graph.
//
// A readout is a declarative view of what the device measures. It is stored
// and reported like any other package -- so what a device has travels with the
// device rather than living in whichever Desktop installed it -- but it is
// never dispatched: no slot, no budget, no frame time. The firmware does not
// interpret its contents at all; the Desktop renders it.
//
// A panel (v1.19.0) is a readout that may also declare actions -- buttons the
// Desktop draws that send a fixed set of device commands, gated by the
// package's declared permissions. To the device it is exactly a readout:
// stored, reported, never dispatched. The permission gate lives where the
// commands are sent from (the Desktop and the App Library); the commands
// themselves enforce their own protection here regardless of who sends them.
enum AppPackageKind : uint8_t { kAppPackageFlow = 0, kAppPackageReadout = 1, kAppPackagePanel = 2 };

// Readouts and panels: declarative, drawn by the Desktop, never dispatched.
inline bool appPackageIsView(uint8_t kind) {
  return kind == kAppPackageReadout || kind == kAppPackagePanel;
}
inline const char* appPackageKindName(uint8_t kind) {
  return kind == kAppPackagePanel ? "panel" : kind == kAppPackageReadout ? "readout" : "flow";
}
inline AppPackageKind appPackageKindFromName(const String& name) {
  return name == "panel" ? kAppPackagePanel : name == "readout" ? kAppPackageReadout : kAppPackageFlow;
}

class AppPackage {
 public:
  // Validates the manifest block only. Does NOT parse the graph -- that stays
  // in FlowApp, which is the only thing that knows what a graph costs.
  static bool parseManifest(const String& json, AppPackageManifest& out, String& error);

  // Registry-side id rules, enforced here so install and reindex agree.
  static bool validId(const String& id);
  static String packagePath(const String& id);  // "apps/<id>.nha", user scope

  // Numeric compare of `minOs` against kFirmwareVersion, ignoring any
  // prerelease suffix (VersionCompare.h). A leading 'v' is tolerated.
  static bool osVersionSatisfies(const char* minOs);

  static String sha256Hex(const uint8_t* data, size_t length);

  static uint16_t capabilityFromName(const String& name);
};

}  // namespace nhos
