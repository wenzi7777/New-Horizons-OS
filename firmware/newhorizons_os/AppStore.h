#pragma once

#include <Arduino.h>

#include "AppContract.h"

namespace nhos {

class Storage;

// An app's own storage (the `storage` capability, v2.1.0): store.get/set and
// store.append/clear. One per flow slot, all of it fixed-size members -- the
// four slots together hold kMaxSlots * kVmStoreBufferBytes of row buffer and
// nothing is allocated while an app runs.
//
//  * Values: up to kVmStoreMaxKeys numbers, keyed by the FNV-1a hash of their
//    name, kept in one NVS blob ("as_<id>") and written from service() at
//    most every kKvSaveIntervalMs, and on close().
//  * Files: rows "ms,a,b,c,d\n" appended to <id>/<file>.csv in the appdata
//    scope (/a on flash), through a kVmStoreBufferBytes buffer written when
//    full, when another file is appended to, every kVmStoreFlushMs and on
//    close(). An app's files never pass kVmStoreQuotaBytes together: a row
//    that would is refused, and the app told so once (store_full).
//
// A flash write is milliseconds, so none happens inside a handler except the
// buffer-full one, which a handler has to cause on purpose.
class AppStore {
 public:
  static constexpr uint32_t kKvSaveIntervalMs = 30000;

  void attach(Storage* storage) { storage_ = storage; }
  // Starts serving `appId`: reads its values and adds up its files.
  void open(const char* appId);
  // Writes everything out and forgets the app.
  void close();
  bool isOpen() const { return id_[0] != '\0'; }

  float get(const char* key) const;
  void set(const char* key, float value);
  // False when the row would pass the quota. `reportFull` is set on the first
  // refusal since open() or a clear(), so the app is told once, not per row.
  bool append(const char* file, uint32_t nowMs, const float values[4], bool& reportFull);
  void clear(const char* file);

  // Housekeeping, outside any handler's time.
  void service(uint32_t nowMs);
  void flush();

  uint32_t usedBytes() const { return used_; }
  // Removes an app's values and files (uninstall).
  static void erase(Storage& storage, const char* appId);

  // A number as JavaScript's String(Number(v.toPrecision(7))) writes it, so a
  // CSV from the device matches the App Studio simulator's. NaN is "".
  static void formatNumber(float value, char* out, size_t size);

 private:
  struct Kv {
    uint32_t hash;
    float value;
  };
  void saveValues();
  void writeBuffer();
  String filePath(const char* file) const;

  Storage* storage_ = nullptr;
  char id_[16] = {0};
  Kv kv_[kVmStoreMaxKeys] = {};
  uint8_t kvCount_ = 0;
  bool kvDirty_ = false;
  uint32_t kvSavedMs_ = 0;
  char buf_[kVmStoreBufferBytes] = {0};
  uint16_t bufLen_ = 0;
  char bufFile_[16] = {0};
  uint32_t bufSinceMs_ = 0;
  uint32_t used_ = 0;  // on flash + buffered
  bool fullReported_ = false;
};

}  // namespace nhos
