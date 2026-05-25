#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <vector>

namespace nhos {

class Storage {
 public:
  bool begin();
  Preferences& prefs();

  String getString(const char* key, const String& fallback = "");
  void putString(const char* key, const String& value);
  uint32_t getUInt(const char* key, uint32_t fallback = 0);
  void putUInt(const char* key, uint32_t value);

  bool validUserPath(const String& path) const;
  bool writeFile(const String& scope, const String& path, const uint8_t* data, size_t len, bool append = false);
  bool readFile(const String& scope, const String& path, std::vector<uint8_t>& out, size_t offset = 0, size_t length = 1024);
  size_t fileSize(const String& scope, const String& path);
  bool deleteFile(const String& scope, const String& path);
  String listFiles(const String& scope);

  void logLine(const String& line);
  String tailLog(size_t maxLines);
  void clearLog();

 private:
  String scopedPath(const String& scope, const String& path) const;
  bool ensureDirs();

  Preferences prefs_;
};

}  // namespace nhos
