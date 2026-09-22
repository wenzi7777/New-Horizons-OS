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
enum AppPackageKind : uint8_t { kAppPackageFlow = 0, kAppPackageReadout = 1 };

class AppPackage {
 public:
  // Validates the manifest block only. Does NOT parse the graph -- that stays
  // in FlowApp, which is the only thing that knows what a graph costs.
  static bool parseManifest(const String& json, AppPackageManifest& out, String& error);

  // Registry-side id rules, enforced here so install and reindex agree.
  static bool validId(const String& id);
  static String packagePath(const String& id);  // "apps/<id>.nha", user scope

  // Lexicographic-by-component compare of `minOs` against kFirmwareVersion.
  // A leading 'v' is tolerated on either side.
  static bool osVersionSatisfies(const char* minOs);

  static String sha256Hex(const uint8_t* data, size_t length);

  static uint16_t capabilityFromName(const String& name);
};

}  // namespace nhos
