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
  if (!SPIFFS.begin(false)) {
    if (!SPIFFS.begin(true)) {
      return false;
    }
  }
  prefs_.begin("nhos", false);
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

bool Storage::writeFile(const String& scope, const String& path, const uint8_t* data, size_t len, bool append) {
  const String full = scopedPath(scope, path);
  if (full.isEmpty()) {
    return false;
  }
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
  const String full = scopedPath(scope, path);
  return !full.isEmpty() && SPIFFS.remove(full);
}

String Storage::listFiles(const String& scope) {
  const String dirPath = scopedPath(scope, "");
  File root = SPIFFS.open(dirPath);
  String out = "[";
  bool first = true;
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += "{\"path\":\"";
      out += String(file.name());
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
  const size_t userBytes = directorySize("/files");
  const size_t logBytes = directorySize("/logs");
  const size_t calibrationBytes = directorySize("/calibration");
  const size_t offlineBytes = directorySize("/offline");
  const size_t configBytes = directorySize("/config");
  const size_t known = userBytes + logBytes + calibrationBytes + offlineBytes + configBytes;
  const size_t otherBytes = used > known ? used - known : 0;

  String out = "{\"total_bytes\":";
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
  logLevel_ = parseLogLevel(level);
  logLevelName_ = level.isEmpty() ? String("info") : level;
  rotateLogIfNeeded(0);
}

String Storage::logStatusJson() const {
  File file = SPIFFS.open(kLogPath, FILE_READ);
  String out = "{\"enabled\":";
  out += logEnabled_ ? "true" : "false";
  out += ",\"level\":\"";
  out += jsonEscape(logLevelName_);
  out += "\",\"mode\":\"";
  out += logMaxBytes_ > kDefaultLogMaxBytes ? "extended" : "standard";
  out += "\",\"max_bytes\":";
  out += String(static_cast<unsigned int>(logMaxBytes_));
  out += ",\"bytes\":";
  out += String(static_cast<unsigned int>(file ? file.size() : 0));
  out += ",\"path\":\"";
  out += kLogPath;
  out += "\"}";
  return out;
}

void Storage::logLine(const String& line, LogLevel level) {
  if (!logEnabled_ || static_cast<uint8_t>(level) > static_cast<uint8_t>(logLevel_)) {
    return;
  }
  rotateLogIfNeeded(line.length() + 1);
  File current = SPIFFS.open(kLogPath, FILE_APPEND);
  if (!current) {
    return;
  }
  current.println(line);
  current.close();
  rotateLogIfNeeded(0);
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
    out += lines[i];
    out += "\"";
  }
  out += "]";
  return out;
}

void Storage::clearLog() {
  SPIFFS.remove(kLogPath);
  SPIFFS.remove("/logs/device.log.1");
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
    root = "/calibration";
  } else if (scope == "offline") {
    root = "/offline";
  }
  if (path.isEmpty()) {
    return root;
  }
  return root + "/" + path;
}

size_t Storage::directorySize(const char* path) const {
  File root = SPIFFS.open(path);
  if (!root || !root.isDirectory()) {
    File file = SPIFFS.open(path, FILE_READ);
    return file ? file.size() : 0;
  }
  size_t total = 0;
  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      total += directorySize(file.path());
    } else {
      total += file.size();
    }
    file = root.openNextFile();
  }
  return total;
}

bool Storage::ensureDirs() {
  SPIFFS.mkdir("/files");
  SPIFFS.mkdir("/logs");
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
  if (currentSize + incomingBytes <= logMaxBytes_) {
    return;
  }
  SPIFFS.remove("/logs/device.log.1");
  SPIFFS.rename(kLogPath, "/logs/device.log.1");
}

}  // namespace nhos
