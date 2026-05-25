#pragma once

#include <Arduino.h>

#include "Storage.h"

namespace nhos {

struct UpdateInfo {
  bool available = false;
  String version;
  String url;
  String sha256;
  size_t size = 0;
  String error;
};

class OtaManager {
 public:
  void begin(Storage& storage);
  UpdateInfo checkUpdate(const String& manifestUrl = "");
  bool applyUpdate(const String& manifestUrl = "");
  bool autoApplyIfNewer(const String& manifestUrl = "");
  String lastStatusJson() const;

 private:
  bool fetchManifest(const String& url, String& payload);
  bool parseManifest(const String& payload, UpdateInfo& out);
  bool downloadAndApply(const UpdateInfo& info);
  String extractString(const String& source, const char* key) const;
  size_t extractSize(const String& source, const char* key) const;
  int compareVersion(const String& remote, const String& local) const;

  Storage* storage_ = nullptr;
  String lastPhase_ = "idle";
  String lastError_;
  bool lastAvailable_ = false;
  String lastVersion_;
  String lastUrl_;
  size_t lastSize_ = 0;
  String lastManifestUrl_;
};

}  // namespace nhos
