#pragma once

#include <Arduino.h>

#include "AppPackage.h"

namespace nhos {

class Storage;
class AppManager;
class FlowApp;

// The on-device package registry: apps/index.json plus the binding of packages
// to the fixed flow slots.
//
// Deliberately a second layer rather than a change to AppManager. AppManager's
// install list is compiled in and only ever grows -- it holds host objects that
// exist for the life of the firmware. A package, by contrast, is content: it
// arrives, binds to a host, and can be removed. Conflating the two would mean
// pretending slots can be destroyed, which they cannot.
class AppRegistry {
 public:
  static constexpr uint8_t kMaxPackages = 8;
  static constexpr uint8_t kMaxSlots = 4;
  static constexpr size_t kMaxPackageBytes = 4096;
  static constexpr const char* kIndexPath = "apps/index.json";
  static constexpr const char* kIndexAbsPath = "/files/apps/index.json";
  static constexpr const char* kIndexTmpPath = "/files/apps/index.json.tmp";

  struct Entry {
    bool valid = false;
    AppPackageManifest manifest;
    int8_t slot = -1;  // -1 = installed but not activated
    uint32_t sizeBytes = 0;
    uint32_t estimatedUs = 0;
    bool loadFailed = false;
  };

  void begin(Storage& storage, AppManager& apps, FlowApp* const slots[kMaxSlots],
             uint16_t cellCount);
  // Reads apps/index.json, then re-activates every entry with slot >= 0. A
  // package that fails to reload is kept in the index with slot = -1 and a
  // reason logged: a bad graph must not stop the device booting.
  void restore();

  bool install(const String& path, const String& expectedSha256, bool replace,
               String& installedId, String& error);
  bool uninstall(const String& id, bool keepFile, String& error);
  bool activate(const String& id, int8_t slot, String& error);
  bool deactivate(const String& id, int8_t slot, String& error);
  // Rebuilds the index from the .nha files actually present: the recovery path
  // for a torn write, and the reason a lost index is survivable at all.
  // `persistWhenEmpty` is false on the boot path: a rebuild that finds
  // nothing must not write an empty index, because that index would then
  // load cleanly forever and stop the device ever trying again.
  bool reindex(uint16_t& recovered, uint16_t& dropped, String& error,
               bool persistWhenEmpty = true);
  bool verify(const String& id, String& computedSha, bool& match, String& error);

  String statusJson() const;
  String packagesText() const;
  int8_t indexOfId(const String& id) const;
  int8_t freeSlot() const;
  uint32_t totalEstimatedUs() const;

 private:
  bool loadIndex();
  // Writes via Storage::writeTextFileAtomic(), which on SPIFFS is remove +
  // rename and therefore NOT atomic. loadIndex() handles the torn case.
  bool saveIndex();
  bool readPackage(const String& path, String& json, String& sha, uint32_t& size, String& error);
  bool bind(Entry& entry, int8_t slot, const String& json, String& error);
  void unbind(int8_t slot);

  Storage* storage_ = nullptr;
  AppManager* apps_ = nullptr;
  FlowApp* slots_[kMaxSlots] = {nullptr, nullptr, nullptr, nullptr};
  int8_t slotOwner_[kMaxSlots] = {-1, -1, -1, -1};
  Entry entries_[kMaxPackages];
  uint16_t cellCount_ = 0;
  uint32_t indexWrites_ = 0;
  bool recoveredFromTmp_ = false;
  bool rebuilt_ = false;
};

}  // namespace nhos
