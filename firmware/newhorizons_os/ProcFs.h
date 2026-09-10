#pragma once

#include <Arduino.h>
#include <vector>

namespace nhos {

class AirtimeArbiter;
class AppManager;
class BootModeManager;
class DeviceConfig;
class FaultRecorder;
class FindMeClient;
class MatrixScanner;
class PowerGovernor;
class PowerStateManager;
class Scheduler;
class ServiceManager;
class Storage;
class TimeSync;
class WifiManager;

// A synthetic, read-only "proc" scope layered over the existing file
// commands. Nothing here is stored: every read runs a generator against live
// state, the way /proc does.
//
// It deliberately introduces no new command surface. file_list /
// file_read_begin / file_read_chunk already exist and are already wired
// through the Desktop terminal, so `cat /proc/tasks` works there the moment
// this is registered -- no protocol change, no backend change.
class ProcFs {
 public:
  static constexpr const char* kScope = "proc";
  static bool isProcScope(const String& scope) { return scope == kScope; }

  void attach(Scheduler& scheduler, FaultRecorder& faults, MatrixScanner& scanner,
              BootModeManager& boot, PowerStateManager& powerState, WifiManager& wifi,
              FindMeClient& findme, DeviceConfig& deviceConfig);
  void setArbiter(AirtimeArbiter* arbiter) { arbiter_ = arbiter; }
  void setServiceManager(ServiceManager* services) { services_ = services; }
  void setStorage(Storage* storage) { storage_ = storage; }
  void setClock(TimeSync* clock) { clock_ = clock; }
  void setPowerGovernor(PowerGovernor* governor) { governor_ = governor; }
  void setAppManager(AppManager* apps) { apps_ = apps; }

  // JSON array matching Storage::listFiles()'s shape, so the caller cannot
  // tell a proc listing from a real one.
  String list() const;
  bool exists(const String& path) const;
  size_t size(const String& path) const;
  bool read(const String& path, size_t offset, size_t length, std::vector<uint8_t>& out) const;

 private:
  // Generated text for everything except crash.elf, which streams straight
  // out of the coredump partition.
  bool generate(const String& path, String& out) const;

  Scheduler* scheduler_ = nullptr;
  FaultRecorder* faults_ = nullptr;
  MatrixScanner* scanner_ = nullptr;
  BootModeManager* boot_ = nullptr;
  PowerStateManager* powerState_ = nullptr;
  WifiManager* wifi_ = nullptr;
  FindMeClient* findme_ = nullptr;
  DeviceConfig* deviceConfig_ = nullptr;
  AirtimeArbiter* arbiter_ = nullptr;
  ServiceManager* services_ = nullptr;
  Storage* storage_ = nullptr;
  TimeSync* clock_ = nullptr;
  PowerGovernor* governor_ = nullptr;
  AppManager* apps_ = nullptr;
};

}  // namespace nhos
