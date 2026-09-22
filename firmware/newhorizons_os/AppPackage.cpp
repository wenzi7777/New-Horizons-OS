#include "AppPackage.h"

#include <mbedtls/sha256.h>

#include <vector>

#include "Config.h"
#include "JsonUtils.h"

namespace nhos {
namespace {

// Manifest fields that would shadow a top-level package key. The graph body is
// read with a flat key scan, so a manifest entry called "nodes" could be found
// before the real one.
bool shadowsPackageKey(const String& manifest) {
  String unused;
  return jsonExtractArray(manifest, "nodes", unused) ||
         jsonExtractObject(manifest, "manifest", unused);
}

void splitVersion(const char* text, long out[3]) {
  out[0] = out[1] = out[2] = 0;
  if (text == nullptr) {
    return;
  }
  String value(text);
  value.trim();
  if (value.startsWith("v") || value.startsWith("V")) {
    value = value.substring(1);
  }
  for (uint8_t part = 0; part < 3 && value.length() > 0; ++part) {
    const int dot = value.indexOf('.');
    const String piece = dot >= 0 ? value.substring(0, dot) : value;
    out[part] = piece.toInt();
    value = dot >= 0 ? value.substring(dot + 1) : "";
  }
}

void copyField(char* dest, size_t size, const String& value) {
  strncpy(dest, value.c_str(), size - 1);
  dest[size - 1] = '\0';
}

}  // namespace

bool AppPackage::validId(const String& id) {
  // "/files/" + "apps/" + id + ".nha" must fit SPIFFS' 31-character path limit,
  // which leaves exactly 15 characters.
  if (id.length() == 0 || id.length() > AppPackageManifest::kIdLen - 1) {
    return false;
  }
  if (id == "index") {
    return false;
  }
  const char first = id[0];
  if (first < 'a' || first > 'z') {
    return false;
  }
  for (unsigned int i = 0; i < id.length(); ++i) {
    const char c = id[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

String AppPackage::packagePath(const String& id) { return String("apps/") + id + ".nha"; }

bool AppPackage::osVersionSatisfies(const char* minOs) {
  long required[3];
  long current[3];
  splitVersion(minOs, required);
  splitVersion(kFirmwareVersion, current);
  for (uint8_t i = 0; i < 3; ++i) {
    if (current[i] != required[i]) {
      return current[i] > required[i];
    }
  }
  return true;
}

uint16_t AppPackage::capabilityFromName(const String& name) {
  if (name == "read_matrix") return kAppCapReadMatrix;
  if (name == "read_imu") return kAppCapReadImu;
  if (name == "emit_event") return kAppCapEmitEvent;
  if (name == "drive_led") return kAppCapDriveLed;
  if (name == "write_file") return kAppCapWriteFile;
  if (name == "tick") return kAppCapTick;
  if (name == "button") return kAppCapButton;
  if (name == "power") return kAppCapPower;
  if (name == "link") return kAppCapLink;
  if (name == "budget") return kAppCapBudget;
  return 0;
}

bool AppPackage::parseManifest(const String& json, AppPackageManifest& out, String& error) {
  long version = 0;
  if (!jsonExtractInt(json, "nhapp", version)) {
    error = "not_a_package";
    return false;
  }
  if (version != 1) {
    error = String("unsupported_package_version:") + String(version);
    return false;
  }
  const String kind = jsonExtractString(json, "kind", "flow");
  if (kind != "flow") {
    // Reserved for a future runtime. An older firmware must refuse a kind it
    // does not understand rather than misread it as a flow graph.
    error = String("unsupported_kind:") + kind;
    return false;
  }

  String manifest;
  if (!jsonExtractObject(json, "manifest", manifest)) {
    error = "missing_manifest";
    return false;
  }
  if (shadowsPackageKey(manifest)) {
    error = "manifest_key_shadows_package_key";
    return false;
  }

  out = AppPackageManifest();
  out.kind = kAppPackageFlow;

  const String id = jsonExtractString(manifest, "id", "");
  if (id.length() == 0) {
    error = "missing_id";
    return false;
  }
  if (!validId(id)) {
    error = String("invalid_id:") + id;
    return false;
  }
  copyField(out.id, sizeof(out.id), id);

  const String packageVersion = jsonExtractString(manifest, "version", "");
  if (packageVersion.length() == 0) {
    error = "missing_version";
    return false;
  }
  copyField(out.version, sizeof(out.version), packageVersion);
  copyField(out.name, sizeof(out.name), jsonExtractString(manifest, "name", id));
  copyField(out.author, sizeof(out.author), jsonExtractString(manifest, "author", ""));
  copyField(out.summary, sizeof(out.summary), jsonExtractString(manifest, "summary", ""));
  copyField(out.category, sizeof(out.category), jsonExtractString(manifest, "category", "other"));
  copyField(out.minOs, sizeof(out.minOs), jsonExtractString(manifest, "min_os", "v1.0.0"));

  if (!osVersionSatisfies(out.minOs)) {
    error = String("os_too_old:") + out.minOs;
    return false;
  }

  long budget = 0;
  if (jsonExtractInt(manifest, "budget_us", budget) && budget > 0) {
    out.declaredBudgetUs = static_cast<uint32_t>(budget);
  }

  // Capabilities arrive as a list of names. A bare integer bitmask in a
  // user-authored file would be hostile to read and easy to get wrong.
  String capabilityArray;
  if (jsonExtractArray(manifest, "capabilities", capabilityArray)) {
    int cursor = 0;
    while (cursor < static_cast<int>(capabilityArray.length())) {
      const int open = capabilityArray.indexOf('"', cursor);
      if (open < 0) break;
      const int close = capabilityArray.indexOf('"', open + 1);
      if (close < 0) break;
      const String name = capabilityArray.substring(open + 1, close);
      const uint16_t bit = capabilityFromName(name);
      if (bit == 0) {
        error = String("unknown_capability:") + name;
        return false;
      }
      out.capabilities |= bit;
      cursor = close + 1;
    }
  }
  if (out.capabilities == kAppCapNone) {
    out.capabilities = kAppCapReadMatrix | kAppCapEmitEvent;
  }
  // A flow package has no business writing files, whatever it declares.
  if ((out.capabilities & kAppCapWriteFile) != 0) {
    error = "capability_not_permitted:write_file";
    return false;
  }
  return true;
}

String AppPackage::sha256Hex(const uint8_t* data, size_t length) {
  uint8_t digest[32];
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts(&context, 0);
  mbedtls_sha256_update(&context, data, length);
  mbedtls_sha256_finish(&context, digest);
  mbedtls_sha256_free(&context);

  String hex;
  hex.reserve(64);
  for (uint8_t byte : digest) {
    if (byte < 16) hex += '0';
    hex += String(byte, HEX);
  }
  return hex;
}

}  // namespace nhos
