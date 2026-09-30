#include "LifetimeStats.h"

#include <esp_system.h>
#include <math.h>

#include "BootModeManager.h"
#include "Config.h"
#include "FaultRecorder.h"
#include "JsonUtils.h"
#include "TimeSync.h"

namespace nhos {
namespace {

constexpr char kNamespace[] = "nhos_life";
constexpr char kStatsKey[] = "stats";
// Temperature and heap move slowly; sampling them on every 1 Hz tick would
// only cost time.
constexpr uint32_t kSampleIntervalMs = 60000;

LifetimeStats* shutdownInstance = nullptr;

String tempJson(int16_t c10) {
  if (c10 == kLifetimeTempUnset) {
    return "null";
  }
  return String(c10 / 10.0f, 1);
}

}  // namespace

void LifetimeStats::begin(const FaultRecorder& faults, const BootModeManager& boot) {
  faults_ = &faults;
  boot_ = &boot;
  prefs_.begin(kNamespace, false);

  // An older (shorter) schema is read as a prefix; fields it lacks keep
  // their defaults. A blob larger than ours comes from newer firmware after
  // a downgrade -- getBytes() refuses it, and we start over rather than
  // misread it.
  const size_t stored = prefs_.getBytesLength(kStatsKey);
  bool fresh = true;
  if (stored > 0 && stored <= sizeof(counters_) &&
      prefs_.getBytes(kStatsKey, &counters_, sizeof(counters_)) == stored) {
    fresh = false;
  } else {
    counters_ = LifetimeCounters();
  }
  counters_.schema = kLifetimeSchema;

  lifetimeNoteBoot(counters_, faults.lastResetReason(), faults.bootCount(), kFirmwareVersion, fresh);
  if (boot.rolledBackThisBoot()) {
    ++counters_.otaRollback;
  }
  ready_ = true;
  lastTickMs_ = millis();
  flush();

  shutdownInstance = this;
  esp_register_shutdown_handler(&LifetimeStats::onShutdown);
}

void LifetimeStats::onShutdown() {
  if (shutdownInstance != nullptr) {
    shutdownInstance->flush();
  }
}

void LifetimeStats::service(uint32_t nowMs, bool awake, bool scanning) {
  if (!ready_) {
    return;
  }
  lifetimeAccumulate(counters_, remainder_, nowMs - lastTickMs_, awake, scanning);
  lastTickMs_ = nowMs;

  if (!sampled_ || nowMs - lastSampleMs_ >= kSampleIntervalMs) {
    sampled_ = true;
    lastSampleMs_ = nowMs;
    const float celsius = temperatureRead();
    if (!isnan(celsius)) {
      currentTempC10_ = static_cast<int16_t>(lroundf(celsius * 10.0f));
    }
    lifetimeNoteTemperature(counters_, celsius);
    lifetimeNoteMinHeap(counters_, ESP.getMinFreeHeap());
    if (counters_.firstSeenUnix == 0 && clock_ != nullptr && clock_->hasSynced()) {
      counters_.firstSeenUnix = static_cast<uint32_t>(clock_->nowEpochMs() / 1000ULL);
    }
  }

  // Soft-off is where a device is most likely to be unplugged next.
  const bool enteredSoftOff = wasAwake_ && !awake;
  wasAwake_ = awake;
  if (enteredSoftOff || lifetimeFlushDue(nowMs, lastFlushMs_)) {
    flush();
  }
}

void LifetimeStats::noteOtaConfirmed() {
  ++counters_.otaSuccess;
  flush();
}

void LifetimeStats::noteWifiDisconnect() {
  ++counters_.wifiDisconnects;
}

void LifetimeStats::noteButtonPress() {
  ++counters_.buttonPresses;
}

bool LifetimeStats::flush() {
  if (!ready_) {
    return false;
  }
  lifetimeNoteSession(counters_, sessionS());
  lastFlushMs_ = millis();
  return prefs_.putBytes(kStatsKey, &counters_, sizeof(counters_)) == sizeof(counters_);
}

uint32_t LifetimeStats::crashTotal() const {
  uint32_t total = 0;
  for (uint8_t i = 0; i < kLifetimeResetSlots; ++i) {
    if (FaultRecorder::isCrashReason(i)) {
      total += counters_.resetCounts[i];
    }
  }
  return total;
}

uint8_t LifetimeStats::healthReasons() const {
  LifetimeHealthInputs inputs;
  if (boot_ != nullptr) {
    inputs.safeMode = boot_->mode() == RunMode::SafeMaintenance;
    inputs.rolledBack = boot_->rolledBackFrom().length() > 0;
  }
  if (faults_ != nullptr) {
    inputs.bootCount = faults_->bootCount();
    inputs.lastCrashBootId = faults_->lastCrashBootId();
  }
  inputs.currentTempC10 = currentTempC10_;
  inputs.minFreeHeap = counters_.minFreeHeap;
  return lifetimeHealthReasons(inputs);
}

String LifetimeStats::healthJson() const {
  const uint8_t reasons = healthReasons();
  String json;
  json.reserve(900);
  json += "{\"schema\":";
  json += String(counters_.schema);
  json += ",\"health\":{\"verdict\":\"";
  json += lifetimeVerdictName(lifetimeVerdictFor(reasons));
  json += "\",\"reasons\":[";
  bool first = true;
  for (uint8_t i = 0; i < kLifetimeReasonCount; ++i) {
    if (reasons & (1 << i)) {
      json += first ? "\"" : ",\"";
      json += lifetimeReasonName(i);
      json += "\"";
      first = false;
    }
  }
  json += "]}";

  json += ",\"time\":{";
  first = true;
  jsonUnsignedField(json, "power_on_s", lifetimePowerOnS(counters_), first);
  jsonUnsignedField(json, "awake_s", counters_.awakeS, first);
  jsonUnsignedField(json, "soft_off_s", counters_.softOffS, first);
  jsonUnsignedField(json, "scan_s", counters_.scanS, first);
  jsonUnsignedField(json, "session_s", sessionS(), first);
  jsonUnsignedField(json, "longest_session_s", max(counters_.longestSessionS, sessionS()), first);
  jsonUnsignedField(json, "first_seen_unix", counters_.firstSeenUnix, first);
  jsonUnsignedField(json, "flush_interval_s", kLifetimeFlushIntervalMs / 1000, first);
  json += "}";

  json += ",\"boot\":{";
  first = true;
  jsonUnsignedField(json, "boot_count", faults_ != nullptr ? faults_->bootCount() : 0, first);
  jsonUnsignedField(json, "tracked_since_boot", counters_.trackedSinceBoot, first);
  jsonUnsignedField(json, "crash_total", crashTotal(), first);
  jsonStringField(json, "last_reset_reason",
                  faults_ != nullptr ? FaultRecorder::resetReasonName(faults_->lastResetReason()) : "",
                  first);
  json += ",\"reset_counts\":{";
  bool firstReason = true;
  for (uint8_t i = 0; i < kLifetimeResetSlots; ++i) {
    if (counters_.resetCounts[i] != 0) {
      jsonUnsignedField(json, FaultRecorder::resetReasonName(i), counters_.resetCounts[i], firstReason);
    }
  }
  json += "}}";

  json += ",\"firmware\":{";
  first = true;
  jsonStringField(json, "current", kFirmwareVersion, first);
  jsonStringField(json, "first", String(counters_.firstFirmware), first);
  jsonUnsignedField(json, "changes", counters_.firmwareChanges, first);
  jsonUnsignedField(json, "ota_success", counters_.otaSuccess, first);
  jsonUnsignedField(json, "ota_rollback", counters_.otaRollback, first);
  jsonStringField(json, "rolled_back_from", boot_ != nullptr ? boot_->rolledBackFrom() : String(""), first);
  json += "}";

  json += ",\"environment\":{";
  first = true;
  jsonRawField(json, "chip_temp_c", tempJson(currentTempC10_), first);
  jsonRawField(json, "chip_temp_max_c", tempJson(counters_.tempMaxC10), first);
  jsonRawField(json, "chip_temp_min_c", tempJson(counters_.tempMinC10), first);
  jsonUnsignedField(json, "min_free_heap", counters_.minFreeHeap, first);
  json += "}";

  json += ",\"connectivity\":{";
  first = true;
  jsonUnsignedField(json, "wifi_disconnects", counters_.wifiDisconnects, first);
  json += "}";

#if NHOS_BOARD_HAS_BUTTON
  json += ",\"input\":{";
  first = true;
  jsonUnsignedField(json, "button_presses", counters_.buttonPresses, first);
  json += "}";
#endif
  json += "}";
  return json;
}

String LifetimeStats::procText() const {
  const uint8_t reasons = healthReasons();
  String out;
  out += String("verdict=") + lifetimeVerdictName(lifetimeVerdictFor(reasons)) + "\n";
  out += "reasons=";
  bool first = true;
  for (uint8_t i = 0; i < kLifetimeReasonCount; ++i) {
    if (reasons & (1 << i)) {
      out += first ? "" : ",";
      out += lifetimeReasonName(i);
      first = false;
    }
  }
  out += "\n";
  out += "power_on_s=" + String(lifetimePowerOnS(counters_)) + "\n";
  out += "awake_s=" + String(counters_.awakeS) + "\n";
  out += "soft_off_s=" + String(counters_.softOffS) + "\n";
  out += "scan_s=" + String(counters_.scanS) + "\n";
  out += "longest_session_s=" + String(max(counters_.longestSessionS, sessionS())) + "\n";
  out += "boot_count=" + String(faults_ != nullptr ? faults_->bootCount() : 0) + "\n";
  out += "tracked_since_boot=" + String(counters_.trackedSinceBoot) + "\n";
  out += "crash_total=" + String(crashTotal()) + "\n";
  out += "reset_counts=";
  first = true;
  for (uint8_t i = 0; i < kLifetimeResetSlots; ++i) {
    if (counters_.resetCounts[i] != 0) {
      out += first ? "" : ",";
      out += String(FaultRecorder::resetReasonName(i)) + ":" + String(counters_.resetCounts[i]);
      first = false;
    }
  }
  out += "\n";
  out += String("firmware_first=") + counters_.firstFirmware + "\n";
  out += "firmware_changes=" + String(counters_.firmwareChanges) + "\n";
  out += "ota_success=" + String(counters_.otaSuccess) + "\n";
  out += "ota_rollback=" + String(counters_.otaRollback) + "\n";
  out += "chip_temp_c=" + tempJson(currentTempC10_) + "\n";
  out += "chip_temp_max_c=" + tempJson(counters_.tempMaxC10) + "\n";
  out += "chip_temp_min_c=" + tempJson(counters_.tempMinC10) + "\n";
  out += "min_free_heap=" + String(counters_.minFreeHeap) + "\n";
  out += "wifi_disconnects=" + String(counters_.wifiDisconnects) + "\n";
#if NHOS_BOARD_HAS_BUTTON
  out += "button_presses=" + String(counters_.buttonPresses) + "\n";
#endif
  return out;
}

}  // namespace nhos
