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
  // Full scope-relative paths ("apps/x.nha"), not the basenames
  // listFiles() reports. SPIFFS is flat, so the directory is part of
  // the filename and File::name() drops it -- which makes a listing
  // useless for anything that has to open the file again.
  std::vector<String> listFilePaths(const String& scope);
  String storageStatusJson();

  void configureLog(bool enabled, size_t maxBytes, const String& level);
  String logStatusJson() const;
  void logLine(const String& line, LogLevel level = LogLevel::Info);
  // Tagged variant. Every line still reaches the flash log exactly as
  // before; the tag adds a per-subsystem level so a chatty subsystem can be
  // turned down without silencing the rest, and it is what /proc/kmsg groups
  // by. Lowering a tag's level is also the lever for flash wear -- the
  // untagged path writes every single line to LittleFS.
  void logTagged(const char* tag, const String& line, LogLevel level = LogLevel::Info);
  // Per-tag override. An unset tag falls back to the global level.
  bool setTagLevel(const char* tag, LogLevel level);
  String tagLevelsJson() const;
  // In-RAM ring of the most recent lines, so a live tail costs no flash
  // reads at all.
  //
  // Two renderings on purpose: /proc/kmsg is a byte stream and wants the
  // plain text, while the dmesg command is embedded into a JSON response by
  // ControlServer::ok() and must therefore be a JSON value. Returning text
  // from the command produced malformed JSON on hardware.
  String kmsgText() const;
  String kmsgJson() const;
  String tailLog(size_t maxLines);
  void clearLog();
  static LogLevel parseLogLevel(const String& level);
  static const char* logLevelName(LogLevel level);

 private:
  String scopedPath(const String& scope, const String& path) const;
  size_t directorySize(const char* path) const;
  bool ensureDirs();
  void rotateLogIfNeeded(size_t incomingBytes = 0);

  static constexpr uint8_t kLogRingEntries = 24;
  static constexpr uint8_t kLogRingTextLen = 96;
  static constexpr uint8_t kLogTagLen = 12;
  static constexpr uint8_t kMaxTagLevels = 10;

  struct LogRingEntry {
    uint32_t ms = 0;
    uint8_t level = 0;
    bool valid = false;
    char tag[kLogTagLen] = {0};
    char text[kLogRingTextLen] = {0};
  };

  struct TagLevel {
    char tag[kLogTagLen] = {0};
    LogLevel level = LogLevel::Info;
  };

  void pushRing(const char* tag, const String& line, LogLevel level);
  bool tagAllows(const char* tag, LogLevel level) const;

  LogRingEntry ring_[kLogRingEntries];
  uint8_t ringWrite_ = 0;
  TagLevel tagLevels_[kMaxTagLevels];
  uint8_t tagLevelCount_ = 0;

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
