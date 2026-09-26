#include "AppRegistry.h"

#include <vector>

#include "AppManager.h"
#include "FlowApp.h"
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

void AppRegistry::begin(Storage& storage, AppManager& apps, FlowApp* const slots[kMaxSlots],
                        uint16_t cellCount) {
  storage_ = &storage;
  apps_ = &apps;
  for (uint8_t i = 0; i < kMaxSlots; ++i) {
    slots_[i] = slots[i];
  }
  cellCount_ = cellCount;
}

int8_t AppRegistry::indexOfId(const String& id) const {
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    if (entries_[i].valid && id == entries_[i].manifest.id) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

int8_t AppRegistry::freeSlot() const {
  for (uint8_t i = 0; i < kMaxSlots; ++i) {
    if (slotOwner_[i] < 0) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

uint32_t AppRegistry::totalEstimatedUs() const {
  uint32_t total = 0;
  for (uint8_t i = 0; i < kMaxSlots; ++i) {
    if (slotOwner_[i] >= 0) {
      total += entries_[slotOwner_[i]].estimatedUs;
    }
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
                          String& installedId, String& error) {
  String json;
  String sha;
  uint32_t size = 0;
  if (!readPackage(path, json, sha, size, error)) {
    return false;
  }
  if (expectedSha256.length() > 0 && !expectedSha256.equalsIgnoreCase(sha)) {
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

  const int8_t existing = indexOfId(String(manifest.id));
  if (existing >= 0 && !replace) {
    error = String("already_installed:") + manifest.id;
    return false;
  }

  // Dry-run the graph before it is indexed, so a broken package is refused at
  // install time rather than discovered at activation. A readout has no graph
  // -- the device stores and reports it without interpreting it.
  uint32_t estimatedUs = 0;
  if (manifest.kind != kAppPackageReadout) {
    String graphError;
    if (!FlowApp::dryRun(json, cellCount_,
                         manifest.declaredBudgetUs != 0 ? manifest.declaredBudgetUs
                                                        : FlowApp::kDefaultBudgetUs,
                         estimatedUs, graphError)) {
      error = String("graph_invalid:") + graphError;
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
  entry.loadFailed = false;
  if (existing < 0) {
    entry.slot = -1;
  }

  if (!saveIndex()) {
    entries_[index] = previous;
    error = "index_write_failed";
    return false;
  }
  // A replaced package that was live is rebound so the running slot reflects
  // what the index now says.
  if (entry.slot >= 0) {
    String bindError;
    if (!bind(entry, entry.slot, json, bindError)) {
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
  if (entry.slot >= 0) {
    unbind(entry.slot);
    entry.slot = -1;
  }
  // Uninstalling is the one way to start a persisted count over; deactivating
  // keeps it.
  if (storage_ != nullptr) {
    FlowApp::clearPersisted(*storage_, entry.manifest.id);
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

bool AppRegistry::bind(Entry& entry, int8_t slot, const String& json, String& error) {
  FlowApp* app = slots_[slot];
  if (app == nullptr) {
    error = "slot_unavailable";
    return false;
  }
  if (!app->loadFromJson(json, AppPackage::packagePath(String(entry.manifest.id)), cellCount_,
                         error)) {
    error = String("graph_invalid:") + error;
    return false;
  }
  app->applyPackageManifest(entry.manifest.version, entry.manifest.summary, entry.manifest.id,
                            entry.manifest.capabilities);
  entry.estimatedUs = app->estimatedUs();
  slotOwner_[slot] = static_cast<int8_t>(&entry - entries_);
  entry.slot = slot;
  return true;
}

void AppRegistry::unbind(int8_t slot) {
  if (slot < 0 || slot >= kMaxSlots || slots_[slot] == nullptr) {
    return;
  }
  slots_[slot]->unload();
  slotOwner_[slot] = -1;
}

bool AppRegistry::activate(const String& id, int8_t slot, String& error) {
  const int8_t index = indexOfId(id);
  if (index < 0) {
    error = String("unknown_package:") + id;
    return false;
  }
  Entry& entry = entries_[index];
  if (entry.manifest.kind == kAppPackageReadout) {
    // Nothing to dispatch. Saying so is better than binding it to a slot that
    // would then sit idle and mislead anyone reading the roster.
    error = "not_activatable:readout";
    return false;
  }
  if (entry.slot >= 0) {
    error = String("already_active:") + String(entry.slot);
    return false;
  }
  if (slot < 0) {
    slot = freeSlot();
    if (slot < 0) {
      error = "no_free_slot";
      return false;
    }
  }
  if (slot >= kMaxSlots) {
    error = String("slot_out_of_range:") + String(slot);
    return false;
  }
  if (slotOwner_[slot] >= 0) {
    error = String("slot_occupied:") + entries_[slotOwner_[slot]].manifest.id;
    return false;
  }

  String json;
  String sha;
  uint32_t size = 0;
  if (!readPackage(AppPackage::packagePath(id), json, sha, size, error)) {
    return false;
  }
  if (!bind(entry, slot, json, error)) {
    return false;
  }
  entry.loadFailed = false;
  if (!saveIndex()) {
    unbind(slot);
    entry.slot = -1;
    error = "index_write_failed";
    return false;
  }
  return true;
}

bool AppRegistry::deactivate(const String& id, int8_t slot, String& error) {
  int8_t index = -1;
  if (id.length() > 0) {
    index = indexOfId(id);
    if (index < 0) {
      error = String("unknown_package:") + id;
      return false;
    }
    slot = entries_[index].slot;
  } else {
    if (slot < 0 || slot >= kMaxSlots) {
      error = String("slot_out_of_range:") + String(slot);
      return false;
    }
    index = slotOwner_[slot];
  }
  if (index < 0 || slot < 0) {
    error = String("slot_not_active:") + String(slot);
    return false;
  }
  unbind(slot);
  entries_[index].slot = -1;
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

  // Remember the bindings so a rebuild does not silently stop everything that
  // was running.
  char boundIds[kMaxSlots][AppPackageManifest::kIdLen] = {{0}};
  for (uint8_t i = 0; i < kMaxSlots; ++i) {
    if (slotOwner_[i] >= 0) {
      strncpy(boundIds[i], entries_[slotOwner_[i]].manifest.id, AppPackageManifest::kIdLen - 1);
    }
  }
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    if (entries_[i].valid) ++dropped;
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
    entry.slot = -1;
    for (uint8_t slot = 0; slot < kMaxSlots; ++slot) {
      if (boundIds[slot][0] != '\0' && strcmp(boundIds[slot], manifest.id) == 0) {
        String bindError;
        if (bind(entry, static_cast<int8_t>(slot), json, bindError)) {
          break;
        }
        entry.loadFailed = true;
      }
    }
    ++next;
    ++recovered;
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
    json += entry.manifest.kind == kAppPackageReadout ? "readout" : "flow";
    json += "\",\"capabilities\":";
    json += String(entry.manifest.capabilities);
    json += ",\"budget_us\":";
    json += String(entry.manifest.declaredBudgetUs);
    json += ",\"size\":";
    json += String(entry.sizeBytes);
    json += ",\"slot\":";
    json += String(entry.slot);
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
    entry.manifest.kind =
        jsonExtractString(object, "kind", "flow") == "readout" ? kAppPackageReadout
                                                               : kAppPackageFlow;
    long number = 0;
    if (jsonExtractInt(object, "capabilities", number)) {
      entry.manifest.capabilities = static_cast<uint16_t>(number);
    }
    if (jsonExtractInt(object, "budget_us", number)) {
      entry.manifest.declaredBudgetUs = static_cast<uint32_t>(number);
    }
    if (jsonExtractInt(object, "size", number)) {
      entry.sizeBytes = static_cast<uint32_t>(number);
    }
    entry.slot = -1;
    if (jsonExtractInt(object, "slot", number) && number >= 0 && number < kMaxSlots) {
      entry.slot = static_cast<int8_t>(number);
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

  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    Entry& entry = entries_[i];
    if (!entry.valid || entry.slot < 0) continue;
    if (entry.manifest.kind == kAppPackageReadout) {
      entry.slot = -1;
      continue;
    }
    const int8_t wanted = entry.slot;
    entry.slot = -1;
    String json;
    String sha;
    uint32_t size = 0;
    String error;
    if (!readPackage(AppPackage::packagePath(String(entry.manifest.id)), json, sha, size, error) ||
        !bind(entry, wanted, json, error)) {
      // Left installed but inactive, with a reason logged. One bad graph must
      // not keep the device from booting.
      entry.loadFailed = true;
      if (storage_ != nullptr) {
        storage_->logTagged("apps",
                            String("app_restore_failed id=") + entry.manifest.id + " reason=" + error,
                            LogLevel::Warn);
      }
    }
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
  json += ",\"slots\":";
  json += String(kMaxSlots);
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
    json += entry.manifest.kind == kAppPackageReadout ? "readout" : "flow";
    json += "\",\"capabilities\":";
    json += String(entry.manifest.capabilities);
    json += ",\"size\":";
    json += String(entry.sizeBytes);
    json += ",\"estimated_us\":";
    json += String(entry.estimatedUs);
    json += ",\"slot\":";
    json += String(entry.slot);
    json += ",\"state\":\"";
    json += entry.loadFailed ? "load_failed" : (entry.slot >= 0 ? "active" : "installed");
    json += "\"}";
  }
  json += "]}";
  return json;
}

String AppRegistry::packagesText() const {
  String out = "ID               VERSION   SLOT  SIZE   EST_US  STATE\n";
  for (uint8_t i = 0; i < kMaxPackages; ++i) {
    const Entry& entry = entries_[i];
    if (!entry.valid) continue;
    String id(entry.manifest.id);
    while (id.length() < 17) id += ' ';
    out += id;
    String version(entry.manifest.version);
    while (version.length() < 10) version += ' ';
    out += version;
    String slot(entry.slot);
    while (slot.length() < 6) slot = " " + slot;
    out += slot;
    String size(entry.sizeBytes);
    while (size.length() < 7) size = " " + size;
    out += size;
    String estimated(entry.estimatedUs);
    while (estimated.length() < 9) estimated = " " + estimated;
    out += estimated;
    out += "  ";
    out += entry.loadFailed ? "load_failed" : (entry.slot >= 0 ? "active" : "installed");
    out += '\n';
  }
  return out;
}

}  // namespace nhos
