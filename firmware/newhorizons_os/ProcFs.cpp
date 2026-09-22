#include "ProcFs.h"

#include <WiFi.h>

#include "AirtimeArbiter.h"
#include "AppManager.h"
#include "AppRegistry.h"
#include "BootModeManager.h"
#include "Config.h"
#include "DeviceConfig.h"
#include "FaultRecorder.h"
#include "FindMeClient.h"
#include "MatrixScanner.h"
#include "PowerGovernor.h"
#include "PowerStateManager.h"
#include "Scheduler.h"
#include "Storage.h"
#include "TimeSync.h"
#include "ServiceManager.h"
#include "WifiManager.h"

namespace nhos {
namespace {

// Ordering is the listing order. crash.elf is the only binary entry.
constexpr const char* kEntries[] = {
    "version", "uptime", "tasks", "services", "apps", "packages", "mem", "scan", "net", "power", "crash",
    "kmsg", "crash.elf",
};
constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

String uptimeText() {
  const uint32_t ms = millis();
  const uint32_t totalSeconds = ms / 1000;
  const uint32_t days = totalSeconds / 86400;
  const uint32_t hours = (totalSeconds % 86400) / 3600;
  const uint32_t minutes = (totalSeconds % 3600) / 60;
  const uint32_t seconds = totalSeconds % 60;
  String out = String(days) + "d " + String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s\n";
  out += "millis=" + String(ms) + "\n";
  return out;
}

}  // namespace

void ProcFs::attach(Scheduler& scheduler, FaultRecorder& faults, MatrixScanner& scanner,
                    BootModeManager& boot, PowerStateManager& powerState, WifiManager& wifi,
                    FindMeClient& findme, DeviceConfig& deviceConfig) {
  scheduler_ = &scheduler;
  faults_ = &faults;
  scanner_ = &scanner;
  boot_ = &boot;
  powerState_ = &powerState;
  wifi_ = &wifi;
  findme_ = &findme;
  deviceConfig_ = &deviceConfig;
}

bool ProcFs::exists(const String& path) const {
  for (size_t i = 0; i < kEntryCount; ++i) {
    if (path == kEntries[i]) {
      return true;
    }
  }
  return false;
}

String ProcFs::list() const {
  String out = "[";
  for (size_t i = 0; i < kEntryCount; ++i) {
    if (i != 0) {
      out += ",";
    }
    out += "{\"path\":\"";
    out += kEntries[i];
    out += "\",\"size\":";
    out += String(static_cast<unsigned int>(size(String(kEntries[i]))));
    out += "}";
  }
  out += "]";
  return out;
}

size_t ProcFs::size(const String& path) const {
  if (path == "crash.elf") {
    return faults_ != nullptr ? faults_->coreDumpSize() : 0;
  }
  String content;
  if (!generate(path, content)) {
    return 0;
  }
  return content.length();
}

bool ProcFs::read(const String& path, size_t offset, size_t length, std::vector<uint8_t>& out) const {
  out.clear();
  if (path == "crash.elf") {
    if (faults_ == nullptr || !faults_->coreDumpAvailable()) {
      return false;
    }
    out.resize(length);
    size_t readBytes = 0;
    if (!faults_->readCoreDump(offset, out.data(), length, readBytes)) {
      out.clear();
      return false;
    }
    out.resize(readBytes);
    return true;
  }

  String content;
  if (!generate(path, content)) {
    return false;
  }
  // Each chunk re-generates against current state, so a long read can span
  // two different snapshots. Same property real /proc files have; these are
  // diagnostics, not a transactional export.
  if (offset >= content.length()) {
    return true;  // clean EOF rather than an error
  }
  const size_t available = content.length() - offset;
  const size_t toCopy = length < available ? length : available;
  out.assign(content.begin() + offset, content.begin() + offset + toCopy);
  return true;
}

bool ProcFs::generate(const String& path, String& out) const {
  if (path == "version") {
    out = String("product=") + kProductName + "\n";
    out += String("firmware=") + kFirmwareVersion + "\n";
    out += String("protocol=") + kProtocolName + "\n";
    out += String("hardware=") + kHardwareModel + "\n";
    if (boot_ != nullptr) {
      out += String("mode=") + boot_->modeName() + "\n";
      out += String("ota_rollback=") + boot_->otaRollbackStatusJson() + "\n";
    }
    return true;
  }
  if (path == "uptime") {
    out = uptimeText();
    if (faults_ != nullptr) {
      out += "boot_count=" + String(faults_->bootCount()) + "\n";
      out += String("last_reset_reason=") + FaultRecorder::resetReasonName(faults_->lastResetReason()) + "\n";
    }
    if (clock_ != nullptr) {
      out += String("clock=") + clock_->statusJson() + "\n";
    }
    return true;
  }
  if (path == "tasks") {
    if (scheduler_ == nullptr) {
      return false;
    }
    out = scheduler_->tasksText();
    return true;
  }
  if (path == "services") {
    if (services_ == nullptr) {
      return false;
    }
    out = services_->servicesText();
    return true;
  }
  if (path == "kmsg") {
    if (storage_ == nullptr) {
      return false;
    }
    out = storage_->kmsgText();
    return true;
  }
  if (path == "apps") {
    if (apps_ == nullptr) {
      return false;
    }
    out = apps_->appsText();
    return true;
  }
  if (path == "packages") {
    if (appRegistry_ == nullptr) {
      return false;
    }
    out = appRegistry_->packagesText();
    return true;
  }
  if (path == "mem") {
    out = "heap_free=" + String(ESP.getFreeHeap()) + "\n";
    out += "heap_total=" + String(ESP.getHeapSize()) + "\n";
    out += "heap_min_free=" + String(ESP.getMinFreeHeap()) + "\n";
    // The allocator can hold plenty of free bytes and still fail a modest
    // request once fragmented, which is why this is tracked separately --
    // it, not heap_free, is what trips the RamDanger indicator.
    out += "heap_max_alloc=" + String(ESP.getMaxAllocHeap()) + "\n";
    out += "sketch_size=" + String(ESP.getSketchSize()) + "\n";
    out += "flash_size=" + String(ESP.getFlashChipSize()) + "\n";
    return true;
  }
  if (path == "scan") {
    if (scanner_ == nullptr) {
      return false;
    }
    out = scanner_->healthJson();
    out += "\n";
    return true;
  }
  if (path == "net") {
    if (wifi_ == nullptr) {
      return false;
    }
    const bool espNow = deviceConfig_ != nullptr && deviceConfig_->data().transport.mode == "espnow";
    out = String("transport=") + (espNow ? "espnow" : "wifi_udp") + "\n";
    out += String("wifi_connected=") + (wifi_->isConnected() ? "true" : "false") + "\n";
    out += String("ip=") + (wifi_->isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\n";
    out += String("rssi=") + String(WiFi.RSSI()) + "\n";
    if (findme_ != nullptr) {
      out += String("gateway=") + (findme_->hasGateway() ? findme_->streamHost() : String("-")) + "\n";
      out += String("findme=") + findme_->statusJson() + "\n";
    }
    if (arbiter_ != nullptr) {
      out += "\n";
      out += arbiter_->netText();
    }
    return true;
  }
  if (path == "power") {
    if (powerState_ == nullptr) {
      return false;
    }
    out = powerState_->statusJson();
    out += "\n";
    out += "cpu_mhz=" + String(getCpuFrequencyMhz()) + "\n";
    if (governor_ != nullptr) {
      out += String("governor=") + governor_->statusJson() + "\n";
    }
    return true;
  }
  if (path == "crash") {
    if (faults_ == nullptr) {
      return false;
    }
    out = faults_->statusJson();
    out += "\n";
    out += faults_->historyJson();
    out += "\n";
    return true;
  }
  return false;
}

}  // namespace nhos
