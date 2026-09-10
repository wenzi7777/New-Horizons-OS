#pragma once

#include <Arduino.h>
#include <Preferences.h>

namespace nhos {

// One post-mortem entry, reconstructed at boot from the *previous* run.
//
// Nothing is written at crash time -- a panic handler is the worst place to
// start touching NVS. Everything here is recovered afterwards: the reset
// reason from the RTC watchdog/panic registers, and (when the panic handler
// got far enough to write one) the task name and faulting PC out of the core
// dump the ROM parked in the coredump partition.
struct FaultRecord {
  bool valid = false;
  uint32_t bootId = 0;
  uint8_t reason = 0;       // esp_reset_reason_t of the crashed run
  uint32_t pc = 0;          // faulting PC; 0 when no core dump was written
  uint32_t coreDumpBytes = 0;
  // Firmware string read on the boot *after* the crash. Same image as the
  // one that crashed unless an OTA rollback happened in between -- in which
  // case BootModeManager::rolledBackFrom() names what actually died.
  char version[24] = {0};
  char task[16] = {0};      // faulting task name; empty when no core dump
};

class FaultRecorder {
 public:
  void begin();

  // True when the previous run ended in a panic/watchdog/brownout rather
  // than a clean ESP.restart() or power cycle.
  bool lastBootCrashed() const { return lastBootCrashed_; }
  uint32_t bootCount() const { return bootId_; }
  uint8_t lastResetReason() const { return lastResetReason_; }
  static const char* resetReasonName(uint8_t reason);
  // Reset reasons that mean "this run did not end on purpose".
  static bool isCrashReason(uint8_t reason);

  // Raw core dump, kept in flash for offline `esp-coredump` analysis. Served
  // through the existing file_read_chunk path rather than a new command.
  bool coreDumpAvailable() const { return coreDumpBytes_ > 0; }
  size_t coreDumpSize() const { return coreDumpBytes_; }
  bool readCoreDump(size_t offset, uint8_t* out, size_t length, size_t& outRead) const;

  String statusJson() const;   // compact summary, folded into `status`
  String historyJson() const;  // full ring, for crash_log / /proc/crash
  bool clear();                // wipes the ring and erases the core dump

 private:
  static constexpr uint8_t kRingSize = 3;

  void loadRing();
  void pushRecord(const FaultRecord& record);
  bool captureCoreDumpSummary(FaultRecord& record) const;
  static void appendRecordJson(String& out, const FaultRecord& record);

  Preferences prefs_;
  FaultRecord ring_[kRingSize];
  uint8_t writeIndex_ = 0;
  uint32_t bootId_ = 0;
  uint8_t lastResetReason_ = 0;
  bool lastBootCrashed_ = false;
  size_t coreDumpAddr_ = 0;
  size_t coreDumpBytes_ = 0;
};

}  // namespace nhos
