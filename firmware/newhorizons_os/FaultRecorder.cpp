#include "FaultRecorder.h"

#include <esp_core_dump.h>
#include <esp_partition.h>
#include <esp_system.h>

#include "Config.h"
#include "JsonUtils.h"

namespace nhos {
namespace {

constexpr char kNamespace[] = "nhos_fault";
constexpr char kWriteIndexKey[] = "wr";
constexpr char kBootCountKey[] = "boots";

String ringKey(uint8_t index) {
  return String("r") + String(index);
}

void copyField(char* dest, size_t destSize, const char* src) {
  if (destSize == 0) {
    return;
  }
  if (src == nullptr) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, destSize - 1);
  dest[destSize - 1] = '\0';
}

}  // namespace

void FaultRecorder::begin() {
  prefs_.begin(kNamespace, false);
  bootId_ = prefs_.getUInt(kBootCountKey, 0) + 1;
  prefs_.putUInt(kBootCountKey, bootId_);
  loadRing();

  lastResetReason_ = static_cast<uint8_t>(esp_reset_reason());
  lastBootCrashed_ = isCrashReason(lastResetReason_);

  // The dump is deliberately NOT erased here: it stays in flash so it can be
  // pulled off the device afterwards. A fresh panic overwrites it anyway, and
  // a stale one is never read because the summary is only consulted on a boot
  // that actually followed a crash.
  size_t addr = 0;
  size_t size = 0;
  if (esp_core_dump_image_get(&addr, &size) == ESP_OK && size > 0) {
    coreDumpAddr_ = addr;
    coreDumpBytes_ = size;
  }

  if (!lastBootCrashed_) {
    return;
  }

  FaultRecord record;
  record.valid = true;
  record.bootId = bootId_ - 1;  // the run that died, not this one
  record.reason = lastResetReason_;
  record.coreDumpBytes = static_cast<uint32_t>(coreDumpBytes_);
  copyField(record.version, sizeof(record.version), kFirmwareVersion);
  captureCoreDumpSummary(record);
  pushRecord(record);
}

bool FaultRecorder::isCrashReason(uint8_t reason) {
  switch (static_cast<esp_reset_reason_t>(reason)) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
      return true;
    default:
      // POWERON / SW (our own ESP.restart()) / DEEPSLEEP / EXT / USB / JTAG
      // are all intentional ways to end a run.
      return false;
  }
}

const char* FaultRecorder::resetReasonName(uint8_t reason) {
  switch (static_cast<esp_reset_reason_t>(reason)) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_UNKNOWN:
    default: return "unknown";
  }
}

bool FaultRecorder::captureCoreDumpSummary(FaultRecord& record) const {
  if (coreDumpBytes_ == 0 || esp_core_dump_image_check() != ESP_OK) {
    return false;
  }
  // ~200 bytes; only ever built on a boot that followed a crash, so the
  // stack cost is not part of the steady-state budget.
  esp_core_dump_summary_t summary = {};
  if (esp_core_dump_get_summary(&summary) != ESP_OK) {
    return false;
  }
  record.pc = summary.exc_pc;
  copyField(record.task, sizeof(record.task), summary.exc_task);
  return true;
}

void FaultRecorder::loadRing() {
  writeIndex_ = prefs_.getUChar(kWriteIndexKey, 0) % kRingSize;
  for (uint8_t i = 0; i < kRingSize; ++i) {
    FaultRecord record;
    const size_t read = prefs_.getBytes(ringKey(i).c_str(), &record, sizeof(record));
    if (read == sizeof(record)) {
      ring_[i] = record;
    }
  }
}

void FaultRecorder::pushRecord(const FaultRecord& record) {
  ring_[writeIndex_] = record;
  prefs_.putBytes(ringKey(writeIndex_).c_str(), &ring_[writeIndex_], sizeof(FaultRecord));
  writeIndex_ = static_cast<uint8_t>((writeIndex_ + 1) % kRingSize);
  prefs_.putUChar(kWriteIndexKey, writeIndex_);
}

bool FaultRecorder::readCoreDump(size_t offset, uint8_t* out, size_t length, size_t& outRead) const {
  outRead = 0;
  if (out == nullptr || coreDumpBytes_ == 0 || offset >= coreDumpBytes_) {
    return false;
  }
  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (partition == nullptr) {
    return false;
  }
  const size_t available = coreDumpBytes_ - offset;
  const size_t toRead = length < available ? length : available;
  // esp_core_dump_image_get() reports a flash address; the partition read
  // wants an offset within that same partition.
  const size_t partitionOffset = (coreDumpAddr_ - partition->address) + offset;
  if (esp_partition_read(partition, partitionOffset, out, toRead) != ESP_OK) {
    return false;
  }
  outRead = toRead;
  return true;
}

void FaultRecorder::appendRecordJson(String& out, const FaultRecord& record) {
  out += "{\"boot_id\":";
  out += String(record.bootId);
  out += ",\"reason\":\"";
  out += resetReasonName(record.reason);
  out += "\",\"pc\":\"0x";
  out += String(record.pc, HEX);
  out += "\",\"task\":\"";
  out += jsonEscape(String(record.task));
  out += "\",\"firmware_version\":\"";
  out += jsonEscape(String(record.version));
  out += "\",\"core_dump_bytes\":";
  out += String(record.coreDumpBytes);
  out += "}";
}

String FaultRecorder::statusJson() const {
  String json = "{\"boot_count\":";
  json += String(bootId_);
  json += ",\"last_reset_reason\":\"";
  json += resetReasonName(lastResetReason_);
  json += "\",\"last_boot_crashed\":";
  json += lastBootCrashed_ ? "true" : "false";
  json += ",\"core_dump_bytes\":";
  json += String(static_cast<uint32_t>(coreDumpBytes_));
  json += ",\"recorded_faults\":";
  uint8_t count = 0;
  for (uint8_t i = 0; i < kRingSize; ++i) {
    if (ring_[i].valid) {
      ++count;
    }
  }
  json += String(count);
  json += "}";
  return json;
}

String FaultRecorder::historyJson() const {
  String json = "[";
  bool first = true;
  // Newest first: walk backwards from the slot the next write would land in.
  for (uint8_t i = 0; i < kRingSize; ++i) {
    const uint8_t index = static_cast<uint8_t>((writeIndex_ + kRingSize - 1 - i) % kRingSize);
    if (!ring_[index].valid) {
      continue;
    }
    if (!first) {
      json += ",";
    }
    appendRecordJson(json, ring_[index]);
    first = false;
  }
  json += "]";
  return json;
}

bool FaultRecorder::clear() {
  for (uint8_t i = 0; i < kRingSize; ++i) {
    ring_[i] = FaultRecord();
    prefs_.remove(ringKey(i).c_str());
  }
  writeIndex_ = 0;
  prefs_.putUChar(kWriteIndexKey, 0);
  lastBootCrashed_ = false;
  const bool erased = esp_core_dump_image_erase() == ESP_OK;
  coreDumpBytes_ = 0;
  coreDumpAddr_ = 0;
  return erased;
}

}  // namespace nhos
