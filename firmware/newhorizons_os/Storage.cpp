#include "Storage.h"

#include <FS.h>
#include <SPIFFS.h>

namespace nhos {
namespace {
constexpr char kLogPath[] = "/logs/device.log";

String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out;
}
}

bool Storage::begin() {
  formattedOnBoot_ = false;
  mountError_ = "";
  prefs_.begin("nhos", false);
  mounted_ = SPIFFS.begin(false);
  if (!mounted_) {
    // "spiffs_init", not the original "spiffs_initialized": NVS keys cap at
    // 15 characters, so that write always failed (KEY_TOO_LONG), the flag was
    // never set, and every mount failure was treated as a first boot --
    // formatting away calibration and apps instead of reporting mount_failed.
    const bool firstBootFormatAllowed = !prefs_.getBool("spiffs_init", false);
    if (!firstBootFormatAllowed || !SPIFFS.begin(true)) {
      mountError_ = firstBootFormatAllowed ? "mount_format_failed" : "mount_failed";
      return false;
    }
    mounted_ = true;
    formattedOnBoot_ = true;
  }
  prefs_.putBool("spiffs_init", true);
  return ensureDirs();
}

Preferences& Storage::prefs() {
  return prefs_;
}

String Storage::getString(const char* key, const String& fallback) {
  return prefs_.getString(key, fallback);
}

void Storage::putString(const char* key, const String& value) {
  prefs_.putString(key, value);
}

uint32_t Storage::getUInt(const char* key, uint32_t fallback) {
  return prefs_.getUInt(key, fallback);
}

void Storage::putUInt(const char* key, uint32_t value) {
  prefs_.putUInt(key, value);
}

bool Storage::validUserPath(const String& path) const {
  if (path.isEmpty() || path.startsWith("/") || path.indexOf("..") >= 0) {
    return false;
  }
  return path.indexOf('\\') < 0;
}

bool Storage::isPathTooLong(const String& scope, const String& path) const {
  return scopedPath(scope, path).isEmpty();
}

bool Storage::writeFile(const String& scope, const String& path, const uint8_t* data, size_t len, bool append) {
  const String full = scopedPath(scope, path);
  if (full.isEmpty()) {
    return false;
  }
  forgetLogBytes();  // the logs scope is writable
  File file = SPIFFS.open(full, append ? FILE_APPEND : FILE_WRITE);
  if (!file) {
    return false;
  }
  return file.write(data, len) == len;
}

bool Storage::readFile(const String& scope, const String& path, std::vector<uint8_t>& out, size_t offset, size_t length) {
  const String full = scopedPath(scope, path);
  if (full.isEmpty()) {
    return false;
  }
  File file = SPIFFS.open(full, FILE_READ);
  if (!file) {
    return false;
  }
  if (!file.seek(offset, SeekSet)) {
    return false;
  }
  out.assign(length, 0);
  size_t read = file.read(out.data(), length);
  out.resize(read);
  return true;
}

bool Storage::readTextFile(const String& path, String& out) const {
  if (!path.startsWith("/") || path.indexOf("..") >= 0) {
    return false;
  }
  File file = SPIFFS.open(path, FILE_READ);
  if (!file) {
    return false;
  }
  out = file.readString();
  return true;
}

bool Storage::writeTextFileAtomic(const String& path, const String& content) {
  if (!path.startsWith("/") || path.indexOf("..") >= 0) {
    return false;
  }
  const String tmpPath = path + ".tmp";
  SPIFFS.remove(tmpPath);
  File file = SPIFFS.open(tmpPath, FILE_WRITE);
  if (!file) {
    return false;
  }
  if (file.print(content) != content.length()) {
    file.close();
    SPIFFS.remove(tmpPath);
    return false;
  }
  file.close();
  SPIFFS.remove(path);
  return SPIFFS.rename(tmpPath, path);
}

size_t Storage::fileSize(const String& scope, const String& path) {
  const String full = scopedPath(scope, path);
  if (full.isEmpty()) {
    return 0;
  }
  File file = SPIFFS.open(full, FILE_READ);
  if (!file) {
    return 0;
  }
  return file.size();
}

bool Storage::deleteFile(const String& scope, const String& path) {
  forgetLogBytes();
  const String full = scopedPath(scope, path);
  return !full.isEmpty() && SPIFFS.remove(full);
}

std::vector<String> Storage::listFilePaths(const String& scope) {
  std::vector<String> out;
  const String root = scopedPath(scope, "");
  File dir = SPIFFS.open(root);
  if (!dir || !dir.isDirectory()) {
    return out;
  }
  // scopedPath(scope, "") has no trailing separator, so drop it too -- the
  // result has to be something scopedPath() will rebuild into the same
  // absolute name, and a stray leading '/' is not.
  const String prefix = root + "/";
  File file = dir.openNextFile();
  while (file) {
    String full(file.path());
    if (full.startsWith(prefix) && full.length() > prefix.length()) {
      out.push_back(full.substring(prefix.length()));
    }
    file = dir.openNextFile();
  }
  return out;
}

String Storage::listFiles(const String& scope) {
  const String dirPath = scopedPath(scope, "");
  File root = SPIFFS.open(dirPath);
  String out = "[";
  bool first = true;
  if (root && root.isDirectory()) {
    // The same scope-relative path listFilePaths() gives, not File::name():
    // SPIFFS is flat, so name() drops the "apps/" of "apps/x.nha", and a
    // file_read_* / file_delete for the bare "x.nha" missed the file.
    const String prefix = dirPath + "/";
    File file = root.openNextFile();
    while (file) {
      const String full(file.path());
      if (!full.startsWith(prefix) || full.length() <= prefix.length()) {
        file = root.openNextFile();
        continue;
      }
      if (!first) {
        out += ",";
      }
      first = false;
      out += "{\"path\":\"";
      out += jsonEscape(full.substring(prefix.length()));
      out += "\",\"size\":";
      out += String(static_cast<unsigned int>(file.size()));
      out += "}";
      file = root.openNextFile();
    }
  }
  out += "]";
  return out;
}

String Storage::storageStatusJson() {
  // Diagnostic JSON field: "categories".
  const size_t total = SPIFFS.totalBytes();
  const size_t used = SPIFFS.usedBytes();
  CategoryBytes bytes;
  File root = SPIFFS.open("/");
  if (root && root.isDirectory()) {
    accumulateCategories(root, bytes);
  }
  const size_t userBytes = bytes.user;
  const size_t logBytes = bytes.logs;
  const size_t calibrationBytes = bytes.calibration;
  const size_t offlineBytes = bytes.offline;
  const size_t configBytes = bytes.config;
  const size_t known = userBytes + logBytes + calibrationBytes + offlineBytes + configBytes;
  const size_t otherBytes = used > known ? used - known : 0;

  String out = "{\"mounted\":";
  out += mounted_ ? "true" : "false";
  out += ",\"formatted_on_boot\":";
  out += formattedOnBoot_ ? "true" : "false";
  out += ",\"mount_error\":\"";
  out += jsonEscape(mountError_);
  out += "\",\"total_bytes\":";
  out += String(static_cast<unsigned int>(total));
  out += ",\"used_bytes\":";
  out += String(static_cast<unsigned int>(used));
  out += ",\"free_bytes\":";
  out += String(static_cast<unsigned int>(total > used ? total - used : 0));
  out += ",\"categories\":[";
  out += "{\"scope\":\"user\",\"bytes\":";
  out += String(static_cast<unsigned int>(userBytes));
  out += "},{\"scope\":\"logs\",\"bytes\":";
  out += String(static_cast<unsigned int>(logBytes));
  out += "},{\"scope\":\"calibration\",\"bytes\":";
  out += String(static_cast<unsigned int>(calibrationBytes));
  out += "},{\"scope\":\"offline\",\"bytes\":";
  out += String(static_cast<unsigned int>(offlineBytes));
  out += "},{\"scope\":\"config\",\"bytes\":";
  out += String(static_cast<unsigned int>(configBytes));
  out += "},{\"scope\":\"other\",\"bytes\":";
  out += String(static_cast<unsigned int>(otherBytes));
  out += "}]}";
  return out;
}

void Storage::configureLog(bool enabled, size_t maxBytes, const String& level) {
  logEnabled_ = enabled;
  logMaxBytes_ = maxBytes > 0 ? maxBytes : kDefaultLogMaxBytes;
  if (logMaxBytes_ > kExtendedLogMaxBytes) {
    logMaxBytes_ = kExtendedLogMaxBytes;
  }
  // The name is derived from the level that actually applies. Before, an
  // empty level reported "error" while filtering at info (parseLogLevel's
  // fallback), and an unknown one such as "verbose" reported itself verbatim.
  logLevel_ = parseLogLevel(level.isEmpty() ? String("error") : level);
  logLevelName_ = logLevelName(logLevel_);
  rotateLogIfNeeded(0);
}

size_t Storage::currentLogBytes() const {
  if (!logBytesKnown_) {
    File file = SPIFFS.open(kLogPath, FILE_READ);
    noteLogWritten(file ? file.size() : 0);
  }
  return logBytes_;
}

String Storage::logStatusJson() const {
  String out = "{\"enabled\":";
  out += logEnabled_ ? "true" : "false";
  out += ",\"level\":\"";
  out += jsonEscape(logLevelName_);
  out += "\",\"mode\":\"";
  out += logMaxBytes_ > kDefaultLogMaxBytes ? "extended" : "standard";
  out += "\",\"max_bytes\":";
  out += String(static_cast<unsigned int>(logMaxBytes_));
  out += ",\"effective_total_bytes\":";
  out += String(static_cast<unsigned int>(logMaxBytes_ * 2));
  out += ",\"bytes\":";
  out += String(static_cast<unsigned int>(currentLogBytes()));
  out += ",\"path\":\"";
  out += kLogPath;
  out += "\",\"tag_levels\":";
  out += tagLevelsJson();
  out += "}";
  return out;
}

void Storage::logLine(const String& line, LogLevel level) {
  logTagged("sys", line, level);
}

void Storage::logTagged(const char* tag, const String& line, LogLevel level) {
  if (tag == nullptr) {
    tag = "sys";
  }
  // The RAM ring is filled even when logging to flash is switched off, or
  // turned down: a live tail is exactly what you want when you have
  // deliberately stopped writing to flash, and the ring costs no flash at
  // all. So the GLOBAL level -- the flash-wear lever, "error" by default --
  // does not apply to it; only a per-tag override does, which is the explicit
  // way to quiet one chatty subsystem. Filtering the ring by the global level
  // too left dmesg holding nothing but the one line logged before the config
  // loaded, on every device still at the default.
  if (ringAllows(tag, level)) {
    pushRing(tag, line, level);
  }
  if (!tagAllows(tag, level) || !logEnabled_) {
    return;
  }
  rotateLogIfNeeded(line.length() + 1);
  File current = SPIFFS.open(kLogPath, FILE_APPEND);
  if (!current) {
    forgetLogBytes();
    return;
  }
  current.println(line);
  noteLogWritten(current.size());
  current.close();
  rotateLogIfNeeded(0);
}

const Storage::TagLevel* Storage::tagOverride(const char* tag) const {
  for (uint8_t i = 0; i < tagLevelCount_; ++i) {
    if (strncmp(tagLevels_[i].tag, tag, kLogTagLen - 1) == 0) {
      return &tagLevels_[i];
    }
  }
  return nullptr;
}

bool Storage::tagAllows(const char* tag, LogLevel level) const {
  const TagLevel* explicitLevel = tagOverride(tag);
  const LogLevel limit = explicitLevel != nullptr ? explicitLevel->level : logLevel_;
  return static_cast<uint8_t>(level) <= static_cast<uint8_t>(limit);
}

bool Storage::ringAllows(const char* tag, LogLevel level) const {
  const TagLevel* explicitLevel = tagOverride(tag);
  const LogLevel limit = explicitLevel != nullptr ? explicitLevel->level : kRingDefaultLevel;
  return static_cast<uint8_t>(level) <= static_cast<uint8_t>(limit);
}

bool Storage::setTagLevel(const char* tag, LogLevel level) {
  if (tag == nullptr || *tag == '\0') {
    return false;
  }
  for (uint8_t i = 0; i < tagLevelCount_; ++i) {
    if (strncmp(tagLevels_[i].tag, tag, kLogTagLen - 1) == 0) {
      tagLevels_[i].level = level;
      return true;
    }
  }
  if (tagLevelCount_ >= kMaxTagLevels) {
    return false;
  }
  TagLevel& entry = tagLevels_[tagLevelCount_++];
  strncpy(entry.tag, tag, kLogTagLen - 1);
  entry.tag[kLogTagLen - 1] = '\0';
  entry.level = level;
  return true;
}

void Storage::pushRing(const char* tag, const String& line, LogLevel level) {
  LogRingEntry& entry = ring_[ringWrite_];
  entry.ms = millis();
  entry.level = static_cast<uint8_t>(level);
  entry.valid = true;
  strncpy(entry.tag, tag, kLogTagLen - 1);
  entry.tag[kLogTagLen - 1] = '\0';
  strncpy(entry.text, line.c_str(), kLogRingTextLen - 1);
  entry.text[kLogRingTextLen - 1] = '\0';
  ringWrite_ = static_cast<uint8_t>((ringWrite_ + 1) % kLogRingEntries);
}

const char* Storage::logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Error: return "error";
    case LogLevel::Warn: return "warn";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info:
    default: return "info";
  }
}

String Storage::tagLevelsJson() const {
  String json = "{\"global\":\"";
  json += logLevelName_;
  json += "\",\"tags\":[";
  for (uint8_t i = 0; i < tagLevelCount_; ++i) {
    if (i != 0) {
      json += ",";
    }
    json += "{\"tag\":\"";
    json += jsonEscape(String(tagLevels_[i].tag));
    json += "\",\"level\":\"";
    json += logLevelName(tagLevels_[i].level);
    json += "\"}";
  }
  json += "]}";
  return json;
}

String Storage::kmsgJson() const {
  String json = "[";
  bool first = true;
  for (uint8_t i = 0; i < kLogRingEntries; ++i) {
    const LogRingEntry& entry = ring_[(ringWrite_ + i) % kLogRingEntries];
    if (!entry.valid) {
      continue;
    }
    if (!first) {
      json += ",";
    }
    first = false;
    String line = "[";
    line += String(entry.ms);
    line += "] ";
    line += logLevelName(static_cast<LogLevel>(entry.level));
    line += " ";
    line += entry.tag;
    line += ": ";
    line += entry.text;
    json += "\"";
    // Boot lines embed status JSON, so escaping is not optional here.
    json += jsonEscape(line);
    json += "\"";
  }
  json += "]";
  return json;
}

String Storage::kmsgText() const {
  String out;
  out.reserve(kLogRingEntries * (kLogRingTextLen / 2));
  // Oldest first, so a tail reads like a console scrollback.
  for (uint8_t i = 0; i < kLogRingEntries; ++i) {
    const LogRingEntry& entry = ring_[(ringWrite_ + i) % kLogRingEntries];
    if (!entry.valid) {
      continue;
    }
    out += "[";
    out += String(entry.ms);
    out += "] ";
    out += logLevelName(static_cast<LogLevel>(entry.level));
    out += " ";
    out += entry.tag;
    out += ": ";
    out += entry.text;
    out += "\n";
  }
  return out;
}

String Storage::tailLog(size_t maxLines) {
  File file = SPIFFS.open(kLogPath, FILE_READ);
  if (!file) {
    return "[]";
  }
  std::vector<String> lines;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length()) {
      lines.push_back(line);
      if (lines.size() > maxLines) {
        lines.erase(lines.begin());
      }
    }
  }
  String out = "[";
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) {
      out += ",";
    }
    out += "\"";
    // Pre-existing: boot lines carry embedded status JSON, whose quotes made
    // log_tail's response unparseable.
    out += jsonEscape(lines[i]);
    out += "\"";
  }
  out += "]";
  return out;
}

void Storage::clearLog() {
  SPIFFS.remove(kLogPath);
  SPIFFS.remove("/logs/device.log.1");
  noteLogWritten(0);
}

LogLevel Storage::parseLogLevel(const String& level) {
  if (level == "error") {
    return LogLevel::Error;
  }
  if (level == "warn") {
    return LogLevel::Warn;
  }
  if (level == "debug") {
    return LogLevel::Debug;
  }
  return LogLevel::Info;
}

String Storage::scopedPath(const String& scope, const String& path) const {
  if (!path.isEmpty() && !validUserPath(path)) {
    return "";
  }
  String root = "/files";
  if (scope == "logs") {
    root = "/logs";
  } else if (scope == "calibration") {
    root = "/cal";
  } else if (scope == "offline") {
    root = "/offline";
  }
  if (path.isEmpty()) {
    return root;
  }
  String full = root + "/" + path;
  // SPIFFS_OBJ_NAME_LEN = 32 (includes null terminator), so max usable = 31 chars.
  if (full.length() > 31) {
    return "";
  }
  return full;
}

void Storage::accumulateCategories(File& dir, CategoryBytes& out) const {
  File file = dir.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      accumulateCategories(file, out);
    } else {
      const String path = file.path();
      const size_t size = file.size();
      if (path.startsWith("/files/")) {
        out.user += size;
      } else if (path.startsWith("/logs/")) {
        out.logs += size;
      } else if (path.startsWith("/cal/") || path.startsWith("/calibration/")) {
        out.calibration += size;
      } else if (path.startsWith("/offline/")) {
        out.offline += size;
      } else if (path.startsWith("/config/")) {
        out.config += size;
      }
    }
    file = dir.openNextFile();
  }
}

bool Storage::ensureDirs() {
  SPIFFS.mkdir("/files");
  SPIFFS.mkdir("/logs");
  SPIFFS.mkdir("/cal");
  SPIFFS.mkdir("/calibration");
  SPIFFS.mkdir("/offline");
  SPIFFS.mkdir("/config");
  return true;
}

void Storage::rotateLogIfNeeded(size_t incomingBytes) {
  File check = SPIFFS.open(kLogPath, FILE_READ);
  if (!check) {
    return;
  }
  const size_t currentSize = check.size();
  check.close();
  noteLogWritten(currentSize);
  if (currentSize + incomingBytes <= logMaxBytes_) {
    return;
  }
  SPIFFS.remove("/logs/device.log.1");
  SPIFFS.rename(kLogPath, "/logs/device.log.1");
  noteLogWritten(0);
}

}  // namespace nhos
