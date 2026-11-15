#include "AppRegistry.h"

#include <vector>

#include "AppGovernor.h"
#include "AppManager.h"
#include "AppVm.h"
#include "JsonUtils.h"
#include "Storage.h"

namespace nhos {
namespace {

void splitObjects(const String& arrayBody, std::vector<String>& out) {
  int depth = 0;
  int start = -1;
  bool inString = false;
  bool escaped = false;
  for (unsigned int i = 0; i < arrayBody.length(); ++i) {
    const char c = arrayBody[i];
    if (inString) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') inString = false;
      continue;
    }
    if (c == '"') inString = true;
    else if (c == '{') { if (depth == 0) start = static_cast<int>(i); ++depth; }
    else if (c == '}') {
      --depth;
      if (depth == 0 && start >= 0) { out.push_back(arrayBody.substring(start, i + 1)); start = -1; }
    }
  }
}

void copyField(char* dest, size_t size, const String& value) {
  strncpy(dest, value.c_str(), size - 1);
  dest[size - 1] = '\0';
}

}  // namespace

void AppRegistry::begin(Storage& storage, AppManager& apps, uint16_t cellCount,
                        const AppGovernor* governor) {
  storage_ = &storage;
  apps_ = &apps;
  cellCount_ = cellCount;
  governor_ = governor;
}

int8_t AppRegistry::indexOfId(const String& id) const {
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    if (entries_[i].valid && id == entries_[i].manifest.id) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

uint16_t AppRegistry::activeCount() const {
  uint16_t count = 0;
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    if (entries_[i].valid && entries_[i].app != nullptr) ++count;
  }
  return count;
}

uint16_t AppRegistry::nextOrder() const {
  uint16_t next = 0;
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    const Entry& entry = entries_[i];
    if (entry.valid && entry.app != nullptr && entry.order >= next) next = entry.order + 1;
  }
  return next;
}

uint32_t AppRegistry::totalEstimatedUs() const {
  uint32_t total = 0;
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    if (entries_[i].valid && entries_[i].app != nullptr) total += entries_[i].estimatedUs;
  }
  return total;
}

bool AppRegistry::readPackage(const String& path, String& json, String& sha, uint32_t& size,
                              String& error) {
  if (storage_ == nullptr) {
    error = "storage_unavailable";
    return false;
  }
  const size_t bytes = storage_->fileSize("user", path);
  if (bytes == 0) {
    error = "package_not_found";
    return false;
  }
  if (bytes > kMaxPackageBytes) {
    error = String("package_too_large:") + String(bytes);
    return false;
  }
  std::vector<uint8_t> buffer;
  if (!storage_->readFile("user", path, buffer, 0, bytes)) {
    error = "package_read_failed";
    return false;
  }
  // Hash the bytes we already hold, then convert once. Reading the file twice
  // would double the transient heap peak for no benefit.
  sha = AppPackage::sha256Hex(buffer.data(), buffer.size());
  size = static_cast<uint32_t>(buffer.size());
  json = "";
  json.reserve(buffer.size() + 1);
  for (uint8_t byte : buffer) {
    json += static_cast<char>(byte);
  }
  return true;
}

bool AppRegistry::install(const String& path, const String& expectedSha256, bool replace,
                          uint32_t granted, String& installedId, String& error) {
  String json;
  String sha;
  uint32_t size = 0;
  // The digest is required, not optional: without it a truncated or
  // tampered upload installs as long as it still parses. The Desktop always
  // sends the catalog's (or its own build's) digest.
  if (expectedSha256.length() != 64) {
    error = "sha256_required";
    return false;
  }
  if (!readPackage(path, json, sha, size, error)) {
    return false;
  }
  if (!expectedSha256.equalsIgnoreCase(sha)) {
    error = "checksum_mismatch";
    return false;
  }

  AppPackageManifest manifest;
  if (!AppPackage::parseManifest(json, manifest, error)) {
    return false;
  }
  // The filename is what uninstall and reindex key on, so a manifest that
  // disagrees with it would leave an entry nothing can remove.
  if (AppPackage::packagePath(String(manifest.id)) != path) {
    error = String("id_path_mismatch:") + manifest.id;
    return false;
  }

  // The operator's consent, checked here and not trusted to the Desktop: a
  // dangerous capability the install did not grant is refused.
  if (!AppPackage::checkGrant(manifest.capabilities, granted, error)) {
    return false;
  }

  const int8_t existing = indexOfId(String(manifest.id));
  if (existing >= 0 && !replace) {
    error = String("already_installed:") + manifest.id;
    return false;
  }

  // Dry-run the graph before it is indexed, so a broken package is refused at
  // install time rather than discovered at activation. A readout has no graph
  // -- the device stores and reports it without interpreting it.
  uint32_t estimatedUs = 0;
  uint32_t memoryBytes = 0;
  if (!appPackageIsView(manifest.kind)) {
    String graphError;
    if (!AppVm::dryRun(json, cellCount_,
                       manifest.declaredBudgetUs != 0 ? manifest.declaredBudgetUs
                                                      : AppVm::kDefaultBudgetUs,
                       manifest.capabilities, estimatedUs, memoryBytes, graphError)) {
      // A package built before v2.0.0 is not broken, only old: say so plainly
      // so the Desktop/Studio can offer the rebuilt one.
      error = graphError == "needs_update" ? graphError : String("graph_invalid:") + graphError;
      return false;
    }
  }

  int8_t index = existing;
  if (index < 0) {
    for (uint8_t i = 0; i < kMaxPackages; ++i) {
      if (!entries_[i].valid) { index = static_cast<int8_t>(i); break; }
    }
  }
  if (index < 0) {
    error = "registry_full";
    return false;
  }

  Entry previous = entries_[index];
  Entry& entry = entries_[index];
  entry.valid = true;
  entry.manifest = manifest;
  copyField(entry.manifest.sha256, sizeof(entry.manifest.sha256), sha);
  entry.sizeBytes = size;
  entry.estimatedUs = estimatedUs;
  entry.memoryBytes = memoryBytes;
  entry.loadFailed = false;
  entry.granted = granted & manifest.capabilities & kAppDangerousCapabilities;
  if (existing < 0) {
    entry.app = nullptr;
    entry.active = false;
    entry.order = 0;
  }

  if (!saveIndex()) {
    entries_[index] = previous;
    error = "index_write_failed";
    return false;
  }
  // A replaced package that was running is reloaded in place, so the running
  // app reflects what the index now says. Transactional: if the new image
  // cannot load, the old one keeps running and the entry says so.
  if (entry.app != nullptr) {
    String loadError;
    if (!load(entry, entry.app, json, loadError)) {
      entry.loadFailed = true;
    }
  }
  installedId = String(entry.manifest.id);
  return true;
}

bool AppRegistry::uninstall(const String& id, bool keepFile, String& error) {
  const int8_t index = indexOfId(id);
  if (index < 0) {
    error = String("unknown_package:") + id;
    return false;
  }
  Entry& entry = entries_[index];
  unbind(entry);
  // Uninstalling is the one way to start a persisted count over; deactivating
  // keeps it.
  if (storage_ != nullptr) {
    AppVm::clearPersisted(*storage_, entry.manifest.id);
    // Its store goes with it: values and every file in appdata/<id>/.
    AppStore::erase(*storage_, entry.manifest.id);
  }
  Entry previous = entry;
  entry = Entry();
  // Index first, then the file. A crash between them leaves an orphan file,
  // which reindex() can recover; the reverse leaves an index entry pointing at
  // a file that no longer exists, which nothing can.
  if (!saveIndex()) {
    entries_[index] = previous;
    error = "index_write_failed";
    return false;
  }
  if (!keepFile && storage_ != nullptr) {
    const String path = AppPackage::packagePath(id);
    if (!storage_->deleteFile("user", path)) {
      storage_->logTagged("apps", String("app_pkg_file_orphan path=") + path, LogLevel::Warn);
    }
  }
  return true;
}

bool AppRegistry::load(Entry& entry, AppVm* app, const String& json, String& error) {
  // Again at every load: an index rebuilt from the files alone has lost the
  // grants, and such a package must be installed (and consented to) again.
  if (!AppPackage::checkGrant(entry.manifest.capabilities, entry.granted, error)) {
    return false;
  }
  if (!app->loadFromJson(json, AppPackage::packagePath(String(entry.manifest.id)), cellCount_,
                         entry.manifest.capabilities, error)) {
    entry.needsUpdate = error == "needs_update";
    if (!entry.needsUpdate && error != "app_pool_exhausted") error = String("graph_invalid:") + error;
    return false;
  }
  entry.needsUpdate = false;
  if (!app->applyPackageManifest(entry.manifest.version, entry.manifest.summary, entry.manifest.id,
                                 entry.manifest.capabilities, entry.manifest.burst)) {
    error = "app_pool_exhausted";
    return false;
  }
  entry.estimatedUs = app->estimatedUs();
  entry.memoryBytes = app->memoryBytes();
  return true;
}

bool AppRegistry::bind(Entry& entry, uint16_t order, const String& json, String& error) {
  if (storage_ == nullptr || apps_ == nullptr) {
    error = "apps_unavailable";
    return false;
  }
  AppVm* app = AppVm::create(*storage_);
  if (app == nullptr) {
    error = "app_pool_exhausted";
    return false;
  }
  if (!load(entry, app, json, error)) {
    AppVm::destroy(app);
    return false;
  }
  // Time is the other resource. A loop-free app has a static cost, and the
  // running ones together must fit what the governor can ever give them; an
  // app with loops has no static cost and is held to its slice instead.
  const uint32_t ceiling = governor_ != nullptr ? governor_->ceilingUs() : 0;
  if (ceiling != 0 && entry.estimatedUs != 0 &&
      totalEstimatedUs() + entry.estimatedUs > ceiling) {
    error = String("app_budget_exhausted:") + String(totalEstimatedUs() + entry.estimatedUs) +
            "us>" + String(ceiling) + "us";
    AppVm::destroy(app);
    return false;
  }
  entry.app = app;
  entry.active = true;
  entry.order = order;
  apps_->add(app, order);
  return true;
}

void AppRegistry::unbind(Entry& entry) {
  if (entry.app == nullptr) {
    entry.active = false;
    return;
  }
  if (apps_ != nullptr) apps_->remove(entry.app);
  AppVm::destroy(entry.app);
  entry.app = nullptr;
  entry.active = false;
}

bool AppRegistry::activate(const String& id, String& error) {
  const int8_t index = indexOfId(id);
  if (index < 0) {
    error = String("unknown_package:") + id;
    return false;
  }
  Entry& entry = entries_[index];
  if (appPackageIsView(entry.manifest.kind)) {
    // Nothing to dispatch. Saying so is better than running an app that would
    // then sit idle and mislead anyone reading the roster.
    error = String("not_activatable:") + appPackageKindName(entry.manifest.kind);
    return false;
  }
  if (entry.app != nullptr) {
    error = "already_active";
    return false;
  }

  String json;
  String sha;
  uint32_t size = 0;
  if (!readPackage(AppPackage::packagePath(id), json, sha, size, error)) {
    return false;
  }
  if (!bind(entry, nextOrder(), json, error)) {
    return false;
  }
  entry.loadFailed = false;
  if (!saveIndex()) {
    unbind(entry);
    error = "index_write_failed";
    return false;
  }
  return true;
}

bool AppRegistry::deactivate(const String& id, String& error) {
  const int8_t index = indexOfId(id);
  if (index < 0) {
    error = String("unknown_package:") + id;
    return false;
  }
  Entry& entry = entries_[index];
  if (entry.app == nullptr) {
    error = "app_not_active";
    return false;
  }
  unbind(entry);
  if (!saveIndex()) {
    error = "index_write_failed";
    return false;
  }
  return true;
}

bool AppRegistry::reorder(const String& id, uint16_t to, String& error) {
  const int8_t index = indexOfId(id);
  if (index < 0) {
    error = String("unknown_package:") + id;
    return false;
  }
  if (entries_[index].app == nullptr) {
    error = "app_not_active";
    return false;
  }
  // The active entries in their current order, with `id` moved to `to`.
  int8_t ordered[kMaxPackages];
  uint8_t count = 0;
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    if (entries_[i].valid && entries_[i].app != nullptr && i != static_cast<uint8_t>(index)) {
      uint8_t at = count;
      while (at > 0 && entries_[ordered[at - 1]].order > entries_[i].order) {
        ordered[at] = ordered[at - 1];
        --at;
      }
      ordered[at] = static_cast<int8_t>(i);
      ++count;
    }
  }
  if (to > count) to = count;
  for (uint8_t i = count; i > to; --i) ordered[i] = ordered[i - 1];
  ordered[to] = index;
  ++count;
  for (uint8_t i = 0; i < count; ++i) {
    Entry& entry = entries_[ordered[i]];
    entry.order = i;
    entry.app->run.order = i;
  }
  apps_->sortByOrder();
  if (!saveIndex()) {
    error = "index_write_failed";
    return false;
  }
  return true;
}

bool AppRegistry::verify(const String& id, String& computedSha, bool& match, String& error) {
  const int8_t index = indexOfId(id);
  if (index < 0) {
    error = String("unknown_package:") + id;
    return false;
  }
  String json;
  uint32_t size = 0;
  if (!readPackage(AppPackage::packagePath(id), json, computedSha, size, error)) {
    return false;
  }
  match = computedSha.equalsIgnoreCase(entries_[index].manifest.sha256);
  return true;
}

bool AppRegistry::reindex(uint16_t& recovered, uint16_t& dropped, String& error,
                          bool persistWhenEmpty) {
  if (storage_ == nullptr) {
    error = "storage_unavailable";
    return false;
  }
  recovered = 0;
  dropped = 0;

  // The running apps carry on through a rebuild: each is handed to the new
  // entry for its file, and only one whose file is gone is stopped.
  Entry previous[kMaxPackages];
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    if (entries_[i].valid) ++dropped;
    previous[i] = entries_[i];
    entries_[i] = Entry();
  }

  // listFiles() reports basenames, which cannot be reopened -- SPIFFS is flat
  // and the directory is part of the filename. listFilePaths() keeps it.
  const std::vector<String> paths = storage_->listFilePaths("user");
  uint8_t next = 0;
  for (const String& path : paths) {
    if (next >= kMaxPackages) break;
    if (!path.startsWith("apps/") || !path.endsWith(".nha")) {
      continue;
    }
    String json;
    String sha;
    uint32_t size = 0;
    String readError;
    if (!readPackage(path, json, sha, size, readError)) {
      continue;
    }
    AppPackageManifest manifest;
    String parseError;
    if (!AppPackage::parseManifest(json, manifest, parseError)) {
      continue;
    }
    Entry& entry = entries_[next];
    entry.valid = true;
    entry.manifest = manifest;
    copyField(entry.manifest.sha256, sizeof(entry.manifest.sha256), sha);
    entry.sizeBytes = size;
    for (uint8_t i = 0; i < kMaxPackages; ++i) {
      Entry& old = previous[i];
      if (!old.valid || strcmp(old.manifest.id, manifest.id) != 0) continue;
      entry.app = old.app;
      entry.active = old.app != nullptr;
      entry.order = old.order;
      entry.estimatedUs = old.estimatedUs;
      entry.memoryBytes = old.memoryBytes;
      old.app = nullptr;
      break;
    }
    ++next;
    ++recovered;
  }
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    if (previous[i].app != nullptr) unbind(previous[i]);
  }
  if (dropped >= recovered) {
    dropped = static_cast<uint16_t>(dropped - recovered);
  } else {
    dropped = 0;
  }
  if (recovered == 0 && !persistWhenEmpty) {
    return true;
  }
  if (!saveIndex()) {
    error = "index_write_failed";
    return false;
  }
  return true;
}

bool AppRegistry::saveIndex() {
  if (storage_ == nullptr) {
    return false;
  }
  String json = "{\"v\":1,\"packages\":[";
  bool first = true;
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    const Entry& entry = entries_[i];
    if (!entry.valid) continue;
    if (!first) json += ",";
    first = false;
    json += "{\"id\":\"";
    json += jsonEscape(String(entry.manifest.id));
    json += "\",\"name\":\"";
    json += jsonEscape(String(entry.manifest.name));
    json += "\",\"version\":\"";
    json += jsonEscape(String(entry.manifest.version));
    json += "\",\"author\":\"";
    json += jsonEscape(String(entry.manifest.author));
    json += "\",\"summary\":\"";
    json += jsonEscape(String(entry.manifest.summary));
    json += "\",\"category\":\"";
    json += jsonEscape(String(entry.manifest.category));
    json += "\",\"min_os\":\"";
    json += jsonEscape(String(entry.manifest.minOs));
    json += "\",\"sha256\":\"";
    json += jsonEscape(String(entry.manifest.sha256));
    json += "\",\"kind\":\"";
    json += appPackageKindName(entry.manifest.kind);
    json += "\",\"capabilities\":";
    json += String(entry.manifest.capabilities);
    json += ",\"granted\":";
    json += String(entry.granted);
    json += ",\"budget_us\":";
    json += String(entry.manifest.declaredBudgetUs);
    json += ",\"size\":";
    json += String(entry.sizeBytes);
    json += ",\"burst\":";
    json += entry.manifest.burst ? "true" : "false";
    json += ",\"estimated_us\":";
    json += String(entry.estimatedUs);
    json += ",\"memory_bytes\":";
    json += String(entry.memoryBytes);
    json += ",\"active\":";
    json += entry.active ? "true" : "false";
    json += ",\"order\":";
    json += String(entry.order);
    json += "}";
  }
  json += "]}";
  ++indexWrites_;
  return storage_->writeTextFileAtomic(kIndexAbsPath, json);
}

bool AppRegistry::loadIndex() {
  if (storage_ == nullptr) {
    return false;
  }
  String body;
  bool fromTmp = false;
  if (!storage_->readTextFile(kIndexAbsPath, body) || body.length() == 0) {
    // writeTextFileAtomic removes the target before renaming, so a power cut
    // in that window leaves only the tmp file. That is the one piece of
    // evidence a torn write leaves behind.
    if (storage_->readTextFile(kIndexTmpPath, body) && body.length() > 0) {
      fromTmp = true;
    } else {
      return false;
    }
  }
  String arrayBody;
  if (!jsonExtractArray(body, "packages", arrayBody)) {
    return false;
  }
  std::vector<String> objects;
  splitObjects(arrayBody, objects);

  uint8_t next = 0;
  for (const String& object : objects) {
    if (next >= kMaxPackages) break;
    Entry& entry = entries_[next];
    entry = Entry();
    const String id = jsonExtractString(object, "id", "");
    if (!AppPackage::validId(id)) continue;
    entry.valid = true;
    copyField(entry.manifest.id, sizeof(entry.manifest.id), id);
    copyField(entry.manifest.name, sizeof(entry.manifest.name),
              jsonExtractString(object, "name", id));
    copyField(entry.manifest.version, sizeof(entry.manifest.version),
              jsonExtractString(object, "version", "0.0.0"));
    copyField(entry.manifest.author, sizeof(entry.manifest.author),
              jsonExtractString(object, "author", ""));
    copyField(entry.manifest.summary, sizeof(entry.manifest.summary),
              jsonExtractString(object, "summary", ""));
    copyField(entry.manifest.category, sizeof(entry.manifest.category),
              jsonExtractString(object, "category", "other"));
    copyField(entry.manifest.minOs, sizeof(entry.manifest.minOs),
              jsonExtractString(object, "min_os", "v1.0.0"));
    copyField(entry.manifest.sha256, sizeof(entry.manifest.sha256),
              jsonExtractString(object, "sha256", ""));
    entry.manifest.kind = appPackageKindFromName(jsonExtractString(object, "kind", "flow"));
    long number = 0;
    if (jsonExtractInt(object, "capabilities", number)) {
      entry.manifest.capabilities = static_cast<uint32_t>(number);
    }
    if (jsonExtractInt(object, "granted", number)) {
      entry.granted = static_cast<uint32_t>(number);
    }
    if (jsonExtractInt(object, "budget_us", number)) {
      entry.manifest.declaredBudgetUs = static_cast<uint32_t>(number);
    }
    if (jsonExtractInt(object, "size", number)) {
      entry.sizeBytes = static_cast<uint32_t>(number);
    }
    if (jsonExtractInt(object, "estimated_us", number) && number > 0) {
      entry.estimatedUs = static_cast<uint32_t>(number);
    }
    if (jsonExtractInt(object, "memory_bytes", number) && number > 0) {
      entry.memoryBytes = static_cast<uint32_t>(number);
    }
    bool flag = false;
    if (jsonExtractBool(object, "burst", flag)) entry.manifest.burst = flag;
    if (jsonExtractBool(object, "active", flag)) {
      entry.active = flag;
      if (jsonExtractInt(object, "order", number) && number >= 0) {
        entry.order = static_cast<uint16_t>(number);
      }
    } else if (jsonExtractInt(object, "slot", number) && number >= 0) {
      // An index from before v2.1.0: a slot, and the slot was the order.
      entry.active = true;
      entry.order = static_cast<uint16_t>(number);
    }
    ++next;
  }

  if (fromTmp) {
    recoveredFromTmp_ = true;
    if (storage_ != nullptr) {
      storage_->logTagged("apps", "app_index_recovered_from_tmp", LogLevel::Warn);
    }
    saveIndex();
  }
  return true;
}

void AppRegistry::restore() {
  if (!loadIndex()) {
    // No index, but the packages carry their own manifests -- which is exactly
    // why the manifest is embedded rather than living only in the index.
    uint16_t recovered = 0;
    uint16_t dropped = 0;
    String error;
    if (reindex(recovered, dropped, error, /*persistWhenEmpty=*/false) && recovered > 0) {
      rebuilt_ = true;
      if (storage_ != nullptr) {
        storage_->logTagged("apps", String("app_index_rebuilt count=") + String(recovered),
                            LogLevel::Warn);
      }
    }
    return;
  }

  // In order, so the first ones get the pool if not everything fits any more.
  bool pending[kMaxPackages] = {false};
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    Entry& entry = entries_[i];
    pending[i] = entry.valid && entry.active && !appPackageIsView(entry.manifest.kind);
    entry.active = false;
  }
  uint16_t nextOrder = 0;
  for (;;) {
    int8_t pick = -1;
    for (uint8_t i = 0; i < kMaxPackages; ++i) {
      if (pending[i] && (pick < 0 || entries_[i].order < entries_[pick].order)) {
        pick = static_cast<int8_t>(i);
      }
    }
    if (pick < 0) break;
    pending[pick] = false;
    Entry& entry = entries_[pick];
    String json;
    String sha;
    uint32_t size = 0;
    String error;
    // Renumbered 0..n-1 as they come up: an old index's slots may have gaps.
    if (!readPackage(AppPackage::packagePath(String(entry.manifest.id)), json, sha, size, error) ||
        !bind(entry, nextOrder, json, error)) {
      // Left installed but inactive, with a reason logged. One bad graph must
      // not keep the device from booting.
      entry.loadFailed = true;
      if (storage_ != nullptr) {
        storage_->logTagged("apps",
                            String("app_restore_failed id=") + entry.manifest.id + " reason=" + error,
                            LogLevel::Warn);
      }
      continue;
    }
    ++nextOrder;
  }
}

String AppRegistry::statusJson() const {
  String json = "{\"count\":";
  uint8_t count = 0;
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    if (entries_[i].valid) ++count;
  }
  json += String(count);
  json += ",\"capacity\":";
  json += String(kMaxPackages);
  json += ",\"active\":";
  json += String(activeCount());
  // The two resources an app is admitted against (v2.1.0).
  json += ",\"pool_bytes\":";
  json += String(AppVm::kPoolBytes);
  json += ",\"pool_used\":";
  json += String(AppVm::poolUsedBytes());
  json += ",\"pool_free\":";
  json += String(AppVm::poolFreeBytes());
  json += ",\"control_bytes\":";
  json += String(AppVm::controlBytes());
  json += ",\"budget_us\":";
  json += String(governor_ != nullptr ? governor_->ceilingUs() : 0);
  json += ",\"used_us\":";
  json += String(totalEstimatedUs());
  json += ",\"index_writes\":";
  json += String(indexWrites_);
  json += ",\"recovered_from_tmp\":";
  json += recoveredFromTmp_ ? "true" : "false";
  json += ",\"rebuilt\":";
  json += rebuilt_ ? "true" : "false";
  json += ",\"packages\":[";
  bool first = true;
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    const Entry& entry = entries_[i];
    if (!entry.valid) continue;
    if (!first) json += ",";
    first = false;
    json += "{\"id\":\"";
    json += jsonEscape(String(entry.manifest.id));
    json += "\",\"name\":\"";
    json += jsonEscape(String(entry.manifest.name));
    json += "\",\"version\":\"";
    json += jsonEscape(String(entry.manifest.version));
    json += "\",\"author\":\"";
    json += jsonEscape(String(entry.manifest.author));
    json += "\",\"summary\":\"";
    json += jsonEscape(String(entry.manifest.summary));
    json += "\",\"category\":\"";
    json += jsonEscape(String(entry.manifest.category));
    json += "\",\"min_os\":\"";
    json += jsonEscape(String(entry.manifest.minOs));
    json += "\",\"sha256\":\"";
    json += jsonEscape(String(entry.manifest.sha256));
    json += "\",\"kind\":\"";
    json += appPackageKindName(entry.manifest.kind);
    json += "\",\"capabilities\":";
    json += String(entry.manifest.capabilities);
    json += ",\"granted\":";
    json += String(entry.granted);
    json += ",\"size\":";
    json += String(entry.sizeBytes);
    json += ",\"estimated_us\":";
    json += String(entry.estimatedUs);
    json += ",\"memory_bytes\":";
    json += String(entry.app != nullptr ? entry.app->memoryBytes() : entry.memoryBytes);
    json += ",\"burst\":";
    json += entry.manifest.burst ? "true" : "false";
    json += ",\"active\":";
    json += entry.app != nullptr ? "true" : "false";
    if (entry.app != nullptr) {
      json += ",\"order\":";
      json += String(entry.order);
    }
    json += ",\"state\":\"";
    json += entry.needsUpdate ? "needs_update" : entry.loadFailed ? "load_failed" : (entry.app != nullptr ? "active" : "installed");
    json += "\"}";
  }
  json += "]}";
  return json;
}

String AppRegistry::packagesText() const {
  String out = "ID               VERSION   ORDER  SIZE   EST_US   MEM  STATE\n";
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    const Entry& entry = entries_[i];
    if (!entry.valid) continue;
    String id(entry.manifest.id);
    while (id.length() < 17) id += ' ';
    out += id;
    String version(entry.manifest.version);
    while (version.length() < 10) version += ' ';
    out += version;
    String order(entry.app != nullptr ? String(entry.order) : String("-"));
    while (order.length() < 6) order = " " + order;
    out += order;
    String size(entry.sizeBytes);
    while (size.length() < 7) size = " " + size;
    out += size;
    String estimated(entry.estimatedUs);
    while (estimated.length() < 9) estimated = " " + estimated;
    out += estimated;
    String memory(entry.app != nullptr ? entry.app->memoryBytes() : entry.memoryBytes);
    while (memory.length() < 6) memory = " " + memory;
    out += memory;
    out += "  ";
    out += entry.needsUpdate ? "needs_update" : entry.loadFailed ? "load_failed" : (entry.app != nullptr ? "active" : "installed");
    out += '\n';
  }
  return out;
}

}  // namespace nhos
