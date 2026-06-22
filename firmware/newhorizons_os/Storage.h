#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <vector>

#include "Config.h"

namespace nhos {

enum class LogLevel : uint8_t {
  Error = 0,
  Warn = 1,
  Info = 2,
  Debug = 3,
};

class Storage {
 public:
  bool begin();
  Preferences& prefs();

  String getString(const char* key, const String& fallback = "");
  void putString(const char* key, const String& value);
  uint32_t getUInt(const char* key, uint32_t fallback = 0);
  void putUInt(const char* key, uint32_t value);

  bool validUserPath(const String& path) const;
  bool isPathTooLong(const String& scope, const String& path) const;
  bool writeFile(const String& scope, const String& path, const uint8_t* data, size_t len, bool append = false);
  bool readFile(const String& scope, const String& path, std::vector<uint8_t>& out, size_t offset = 0, size_t length = 1024);
  bool readTextFile(const String& path, String& out) const;
  bool writeTextFileAtomic(const String& path, const String& content);
  size_t fileSize(const String& scope, const String& path);
  bool deleteFile(const String& scope, const String& path);
  String listFiles(const String& scope);
  String storageStatusJson();

  void configureLog(bool enabled, size_t maxBytes, const String& level);
  String logStatusJson() const;
  void logLine(const String& line, LogLevel level = LogLevel::Info);
  String tailLog(size_t maxLines);
  void clearLog();
  static LogLevel parseLogLevel(const String& level);

 private:
  String scopedPath(const String& scope, const String& path) const;
  size_t directorySize(const char* path) const;
  bool ensureDirs();
  void rotateLogIfNeeded(size_t incomingBytes = 0);

  Preferences prefs_;
  bool mounted_ = false;
  bool formattedOnBoot_ = false;
  String mountError_;
  bool logEnabled_ = true;
  size_t logMaxBytes_ = kDefaultLogMaxBytes;
  LogLevel logLevel_ = LogLevel::Info;
  String logLevelName_ = "info";
};

}  // namespace nhos
