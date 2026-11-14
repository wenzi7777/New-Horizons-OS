#include "AppStore.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Storage.h"
#include "VmCore.h"

namespace nhos {

namespace {

constexpr uint8_t kKvMagic = 0xA5;
constexpr size_t kKvHeaderBytes = 2 + 16;  // magic, count, id

String kvKey(const char* appId) {
  // NVS keys stop at 15 characters.
  String key = "as_";
  key += String(appId).substring(0, 12);
  return key;
}

// "1.5e-07" -> "1.5e-7", "1e+21" -> "1e+21": JavaScript's exponent form.
void jsExponent(char* text) {
  char* e = strchr(text, 'e');
  if (e == nullptr) return;
  char* digits = e + 2;  // past the sign
  char* first = digits;
  while (*first == '0' && first[1] != '\0') ++first;
  if (first != digits) memmove(digits, first, strlen(first) + 1);
}

// Fixed notation with `decimals` places, trailing zeros (and a bare point)
// dropped.
void fixedTrimmed(double value, int decimals, char* out, size_t size) {
  snprintf(out, size, "%.*f", decimals, value);
  if (strchr(out, '.') == nullptr) return;
  size_t length = strlen(out);
  while (length > 0 && out[length - 1] == '0') out[--length] = '\0';
  if (length > 0 && out[length - 1] == '.') out[--length] = '\0';
}

}  // namespace

void AppStore::formatNumber(float value, char* out, size_t size) {
  if (size == 0) return;
  out[0] = '\0';
  if (isnan(value)) return;
  if (isinf(value)) {
    snprintf(out, size, value > 0 ? "Infinity" : "-Infinity");
    return;
  }
  if (value == 0.0f) {
    snprintf(out, size, "0");
    return;
  }
  // Seven significant digits, then written the shortest way JavaScript would.
  char rounded[32];
  snprintf(rounded, sizeof(rounded), "%.6e", static_cast<double>(value));
  const double v = strtod(rounded, nullptr);
  const double magnitude = fabs(v);
  if (magnitude >= 1e21 || magnitude < 1e-6) {
    snprintf(out, size, "%.7g", v);
    jsExponent(out);
    return;
  }
  // Places after the point that seven significant digits can need.
  const int exponent = static_cast<int>(floor(log10(magnitude)));
  const int decimals = exponent >= 6 ? 0 : 6 - exponent;
  fixedTrimmed(v, decimals, out, size);
}

void AppStore::open(const char* appId) {
  close();
  if (appId == nullptr || appId[0] == '\0') return;
  strncpy(id_, appId, sizeof(id_) - 1);
  id_[sizeof(id_) - 1] = '\0';
  kvCount_ = 0;
  kvDirty_ = false;
  fullReported_ = false;
  used_ = 0;
  if (storage_ == nullptr) return;

  uint8_t blob[kKvHeaderBytes + kVmStoreMaxKeys * sizeof(Kv)];
  const String key = kvKey(id_);
  const size_t size = storage_->prefs().getBytesLength(key.c_str());
  if (size >= kKvHeaderBytes && size <= sizeof(blob) &&
      storage_->prefs().getBytes(key.c_str(), blob, size) == size && blob[0] == kKvMagic &&
      strncmp(reinterpret_cast<const char*>(blob + 2), id_, 16) == 0) {
    const uint8_t count = blob[1];
    if (count <= kVmStoreMaxKeys && kKvHeaderBytes + count * sizeof(Kv) <= size) {
      memcpy(kv_, blob + kKvHeaderBytes, count * sizeof(Kv));
      kvCount_ = count;
    }
  }

  const String prefix = String(id_) + "/";
  for (const String& path : storage_->listFilePaths("appdata")) {
    if (path.startsWith(prefix)) used_ += storage_->fileSize("appdata", path);
  }
}

void AppStore::close() {
  if (!isOpen()) return;
  flush();
  saveValues();
  id_[0] = '\0';
  kvCount_ = 0;
  bufLen_ = 0;
  bufFile_[0] = '\0';
  used_ = 0;
}

float AppStore::get(const char* key) const {
  const uint32_t hash = vmHash(key);
  for (uint8_t i = 0; i < kvCount_; ++i) {
    if (kv_[i].hash == hash) return kv_[i].value;
  }
  return 0.0f;
}

void AppStore::set(const char* key, float value) {
  if (!isOpen()) return;
  const uint32_t hash = vmHash(key);
  for (uint8_t i = 0; i < kvCount_; ++i) {
    if (kv_[i].hash == hash) {
      if (memcmp(&kv_[i].value, &value, sizeof(float)) != 0) {
        kv_[i].value = value;
        kvDirty_ = true;
      }
      return;
    }
  }
  // A new name past kVmStoreMaxKeys is dropped, as in the simulator.
  if (kvCount_ >= kVmStoreMaxKeys) return;
  kv_[kvCount_].hash = hash;
  kv_[kvCount_].value = value;
  ++kvCount_;
  kvDirty_ = true;
}

bool AppStore::append(const char* file, uint32_t nowMs, const float values[4], bool& reportFull) {
  reportFull = false;
  if (!isOpen() || storage_ == nullptr) return false;
  char row[96];
  int length = snprintf(row, sizeof(row), "%lu", static_cast<unsigned long>(nowMs));
  for (uint8_t i = 0; i < 4; ++i) {
    char cell[24];
    formatNumber(values[i], cell, sizeof(cell));
    length += snprintf(row + length, sizeof(row) - length, ",%s", cell);
  }
  length += snprintf(row + length, sizeof(row) - length, "\n");
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(row)) return false;

  if (used_ + static_cast<uint32_t>(length) > kVmStoreQuotaBytes) {
    if (!fullReported_) {
      fullReported_ = true;
      reportFull = true;
    }
    return false;
  }
  if (bufLen_ > 0 && (strcmp(bufFile_, file) != 0 || bufLen_ + length > kVmStoreBufferBytes)) {
    writeBuffer();
  }
  if (bufLen_ == 0) {
    strncpy(bufFile_, file, sizeof(bufFile_) - 1);
    bufFile_[sizeof(bufFile_) - 1] = '\0';
    bufSinceMs_ = millis();
  }
  memcpy(buf_ + bufLen_, row, length);
  bufLen_ += length;
  used_ += length;
  return true;
}

void AppStore::clear(const char* file) {
  if (!isOpen() || storage_ == nullptr) return;
  if (bufLen_ > 0 && strcmp(bufFile_, file) == 0) {
    used_ -= bufLen_ <= used_ ? bufLen_ : used_;
    bufLen_ = 0;
    bufFile_[0] = '\0';
  }
  const String path = filePath(file);
  const size_t size = storage_->fileSize("appdata", path);
  if (size > 0 && storage_->deleteFile("appdata", path)) {
    used_ -= size <= used_ ? size : used_;
  }
  fullReported_ = false;
}

void AppStore::service(uint32_t nowMs) {
  if (!isOpen()) return;
  if (bufLen_ > 0 && millis() - bufSinceMs_ >= kVmStoreFlushMs) writeBuffer();
  if (kvDirty_ && nowMs - kvSavedMs_ >= kKvSaveIntervalMs) {
    kvSavedMs_ = nowMs;
    saveValues();
  }
}

void AppStore::flush() {
  if (bufLen_ > 0) writeBuffer();
}

String AppStore::filePath(const char* file) const {
  String path = id_;
  path += "/";
  path += file;
  path += ".csv";
  return path;
}

void AppStore::writeBuffer() {
  if (storage_ != nullptr && bufLen_ > 0 &&
      !storage_->writeFile("appdata", filePath(bufFile_), reinterpret_cast<const uint8_t*>(buf_),
                           bufLen_, true)) {
    // The rows are lost; their bytes no longer count against the quota.
    used_ -= bufLen_ <= used_ ? bufLen_ : used_;
    storage_->logTagged("apps", String("app_store_write_failed app=") + id_ + " file=" + bufFile_,
                        LogLevel::Warn);
  }
  bufLen_ = 0;
  bufFile_[0] = '\0';
}

void AppStore::saveValues() {
  if (!kvDirty_ || storage_ == nullptr || !isOpen()) return;
  uint8_t blob[kKvHeaderBytes + kVmStoreMaxKeys * sizeof(Kv)] = {0};
  blob[0] = kKvMagic;
  blob[1] = kvCount_;
  strncpy(reinterpret_cast<char*>(blob + 2), id_, 16);
  memcpy(blob + kKvHeaderBytes, kv_, kvCount_ * sizeof(Kv));
  storage_->prefs().putBytes(kvKey(id_).c_str(), blob, kKvHeaderBytes + kvCount_ * sizeof(Kv));
  kvDirty_ = false;
}

void AppStore::erase(Storage& storage, const char* appId) {
  if (appId == nullptr || appId[0] == '\0') return;
  const String key = kvKey(appId);
  if (storage.prefs().isKey(key.c_str())) storage.prefs().remove(key.c_str());
  const String prefix = String(appId) + "/";
  for (const String& path : storage.listFilePaths("appdata")) {
    if (path.startsWith(prefix)) storage.deleteFile("appdata", path);
  }
}

}  // namespace nhos
