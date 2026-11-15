#pragma once

#include <Arduino.h>

#include "AppPackage.h"

namespace nhos {

class Storage;
class AppManager;
class AppVm;
class AppGovernor;

// The on-device package registry: apps/index.json, and which packages are
// activated -- running, each in its own AppVm placed in the app pool.
//
// v2.1.0: no slots. Activating a package creates its app in the pool and
// adds it to AppManager's run list; deactivating removes and frees it. How
// many run is whatever the pool and the time budget admit (`activate`
// refuses with app_pool_exhausted / app_budget_exhausted), and an app's
// position in `order` decides whose output wins where they compete.
class AppRegistry {
 public:
  // Installed packages, active or not. Resident RAM (an Entry each, ~300 B),
  // not a limit on how many run; the flash holds the packages themselves.
  static constexpr uint8_t kMaxPackages = 16;
  // 8192 since v1.19.0: the sysmon panel alone is ~5.4 KB. Read transiently
  // (file buffer + one String), never held, so the cost is a short-lived
  // ~16 KB on install/activate, not resident RAM. Must match the SDK's limit.
  static constexpr size_t kMaxPackageBytes = 8192;
  static constexpr const char* kIndexPath = "apps/index.json";
  static constexpr const char* kIndexAbsPath = "/files/apps/index.json";
  static constexpr const char* kIndexTmpPath = "/files/apps/index.json.tmp";

  struct Entry {
    bool valid = false;
    AppPackageManifest manifest;
    // The running app while activated, nullptr otherwise.
    AppVm* app = nullptr;
    // Activated (persisted: re-activated at boot), and where in the run order.
    bool active = false;
    uint16_t order = 0;
    uint32_t sizeBytes = 0;
    uint32_t estimatedUs = 0;
    // What the app takes of the pool when active (AppVm::memoryFor); known
    // from install on, so the UI can say whether it would fit.
    uint32_t memoryBytes = 0;
    bool loadFailed = false;
    // Built before the app VM (v2.0.0): no image to run. Kept in the index so
    // the Desktop/Studio can offer the rebuilt package in its place.
    bool needsUpdate = false;
    // The dangerous capabilities the operator consented to at install
    // (app_install `grant`); never wider than what the package declares.
    uint32_t granted = 0;
  };

  void begin(Storage& storage, AppManager& apps, uint16_t cellCount,
             const AppGovernor* governor = nullptr);
  // Reads apps/index.json, then re-activates every active entry in order. An
  // index from before v2.1.0 names a slot instead: slot >= 0 reads as
  // active, in slot order. A package that fails to reload is kept, inactive,
  // with a reason logged: a bad graph must not stop the device booting.
  void restore();

  bool install(const String& path, const String& expectedSha256, bool replace,
               uint32_t granted, String& installedId, String& error);
  bool uninstall(const String& id, bool keepFile, String& error);
  // Creates the app in the pool, runs it, and appends it to the run order.
  bool activate(const String& id, String& error);
  bool deactivate(const String& id, String& error);
  // Moves an active app to position `to` of the run order (0 = first, whose
  // output wins). Clamped to the end.
  bool reorder(const String& id, uint16_t to, String& error);
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
  uint16_t activeCount() const;
  // The loop-free active apps' static estimates, summed. Apps with loops have
  // none and are held to their time slice instead.
  uint32_t totalEstimatedUs() const;

 private:
  bool loadIndex();
  // Writes via Storage::writeTextFileAtomic(), which on SPIFFS is remove +
  // rename and therefore NOT atomic. loadIndex() handles the torn case.
  bool saveIndex();
  bool readPackage(const String& path, String& json, String& sha, uint32_t& size, String& error);
  // Loads `json` into `app`: grant check, image, manifest. On failure the
  // app keeps what it had.
  bool load(Entry& entry, AppVm* app, const String& json, String& error);
  // Creates and starts the app for `entry` at `order`; on failure nothing of
  // it is left in the pool.
  bool bind(Entry& entry, uint16_t order, const String& json, String& error);
  void unbind(Entry& entry);
  uint16_t nextOrder() const;

  Storage* storage_ = nullptr;
  AppManager* apps_ = nullptr;
  const AppGovernor* governor_ = nullptr;
  Entry entries_[kMaxPackages];
  uint16_t cellCount_ = 0;
  uint32_t indexWrites_ = 0;
  bool recoveredFromTmp_ = false;
  bool rebuilt_ = false;
};

}  // namespace nhos
