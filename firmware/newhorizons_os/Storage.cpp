#include "Storage.h"

#include <FS.h>
#include <SPIFFS.h>

namespace nhos {
namespace {
constexpr char kLogPath[] = "/logs/device.log";
constexpr size_t kMaxLogBytes = 32 * 1024;
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

void Storage::logLine(const String& line) {
  File current = SPIFFS.open(kLogPath, FILE_APPEND);
  if (!current) {
    return;
  }
  current.println(line);
  current.close();
  File check = SPIFFS.open(kLogPath, FILE_READ);
  if (check && check.size() > kMaxLogBytes) {
    check.close();
    SPIFFS.remove("/logs/device.log.1");
    SPIFFS.rename(kLogPath, "/logs/device.log.1");
  }
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

bool Storage::ensureDirs() {
  SPIFFS.mkdir("/files");
  SPIFFS.mkdir("/logs");
  SPIFFS.mkdir("/calibration");
  SPIFFS.mkdir("/offline");
  return true;
}

}  // namespace nhos
