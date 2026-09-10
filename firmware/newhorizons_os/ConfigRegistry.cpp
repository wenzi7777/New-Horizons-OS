#include "ConfigRegistry.h"

#include "DeviceConfig.h"
#include "JsonUtils.h"
#include "PowerGovernor.h"
#include "Storage.h"

namespace nhos {
namespace {

// Ranges mirror what the owning set_* handler already enforces; they are
// advisory metadata for clients building a UI, not a second gate.
const ConfigEntry kEntries[] = {
    {"scan.target_fps", ConfigType::Int, "set_scan_timing", "target_fps", 1, kMaxTargetFps,
     nullptr, "Matrix scan rate in frames per second."},
    {"scan.settle_us", ConfigType::Int, "set_scan_timing", "settle_us", 0, 1000, nullptr,
     "Settling delay after driving a column, before sampling."},
    {"scan.send_every_n_frames", ConfigType::Int, "set_scan_timing", "send_every_n_frames", 1,
     255, nullptr, "Transmit one frame out of every N scanned."},
    {"scan.raw_adc", ConfigType::Bool, "set_raw_adc", "enabled", 0, 0, nullptr,
     "Include the unprocessed ADC block alongside calibrated levels."},
    {"stream.buffer_enabled", ConfigType::Bool, "set_stream_buffer", "enabled", 0, 0, nullptr,
     "Decouple scanning from transmission with a packet ring."},
    {"stream.buffer_mode", ConfigType::Enum, "set_stream_buffer", "mode", 0, 0,
     "standard,extended", "Ring depth preset."},
    {"filter.enabled", ConfigType::Bool, "set_filter", "enabled", 0, 0, nullptr,
     "Enable the median + low-pass filter chain."},
    {"filter.median", ConfigType::Int, "set_filter", "median", 1, kFilterMedianMax, nullptr,
     "Median window size in samples."},
    {"filter.alpha", ConfigType::Float, "set_filter", "alpha", kFilterAlphaMin, kFilterAlphaMax,
     nullptr, "Low-pass smoothing factor."},
    {"imu.enabled", ConfigType::Bool, "set_imu", "enabled", 0, 0, nullptr,
     "Sample the IMU and attach it to outgoing frames."},
    {"log.enabled", ConfigType::Bool, "set_log", "enabled", 0, 0, nullptr,
     "Write the device log to flash."},
    {"log.level", ConfigType::Enum, "set_log", "level", 0, 0, "error,warn,info,debug",
     "Global log verbosity."},
    {"log.mode", ConfigType::Enum, "set_log", "mode", 0, 0, "standard,extended",
     "Log file size budget."},
    {"ota.auto_apply_on_boot", ConfigType::Bool, "set_ota_config", "auto_apply_on_boot", 0, 0,
     nullptr, "Check for and apply a newer firmware during boot."},
    {"ota.manifest_url", ConfigType::Text, "set_ota_config", "manifest_url", 0, 0, nullptr,
     "Release manifest this device updates from."},
    {"power.profile", ConfigType::Enum, "set_power_profile", "profile", 0, 0,
     "performance,balanced,powersave", "CPU frequency policy while idle."},
    {"power.charge_profile", ConfigType::Enum, "set_charge_profile", "profile", 0, 0,
     "compatible,fast", "Battery charge current preset."},
};

constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

String boolText(bool value) { return value ? "true" : "false"; }

}  // namespace

const ConfigEntry* ConfigRegistry::entries() { return kEntries; }
size_t ConfigRegistry::entryCount() { return kEntryCount; }

const ConfigEntry* ConfigRegistry::find(const String& path) {
  for (size_t i = 0; i < kEntryCount; ++i) {
    if (path == kEntries[i].path) {
      return &kEntries[i];
    }
  }
  return nullptr;
}

const char* ConfigRegistry::typeName(ConfigType type) {
  switch (type) {
    case ConfigType::Bool: return "bool";
    case ConfigType::Int: return "int";
    case ConfigType::Float: return "float";
    case ConfigType::Enum: return "enum";
    case ConfigType::Text:
    default: return "text";
  }
}

bool ConfigRegistry::readValue(const String& path, const DeviceConfig& config,
                               const PowerGovernor* governor, Storage* storage, String& out) {
  const DeviceConfigData& data = config.data();
  if (path == "scan.target_fps") { out = String(data.scanTiming.targetFps); return true; }
  if (path == "scan.settle_us") { out = String(data.scanTiming.settleUs); return true; }
  if (path == "scan.send_every_n_frames") { out = String(data.scanTiming.sendEveryNFrames); return true; }
  if (path == "scan.raw_adc") { out = boolText(data.streamRawAdc); return true; }
  if (path == "stream.buffer_enabled") { out = boolText(data.streamBuffer.enabled); return true; }
  if (path == "stream.buffer_mode") { out = data.streamBuffer.mode; return true; }
  if (path == "filter.enabled") { out = boolText(data.filter.enabled); return true; }
  if (path == "filter.median") { out = String(data.filter.median); return true; }
  if (path == "filter.alpha") { out = String(data.filter.alpha, 3); return true; }
  if (path == "imu.enabled") { out = boolText(data.imuEnabled); return true; }
  if (path == "log.enabled") { out = boolText(data.logging.enabled); return true; }
  if (path == "log.level") { out = data.logging.level; return true; }
  if (path == "log.mode") { out = data.logging.mode; return true; }
  if (path == "ota.auto_apply_on_boot") { out = boolText(data.ota.autoApplyOnBoot); return true; }
  if (path == "ota.manifest_url") { out = data.ota.manifestUrl; return true; }
  if (path == "power.profile") {
    // The only entry not backed by DeviceConfig: the governor owns it.
    out = governor != nullptr ? PowerGovernor::profileName(governor->profile()) : String("performance");
    return true;
  }
  if (path == "power.charge_profile") {
    // Persisted by PowerManager straight into NVS, not DeviceConfigData.
    out = storage != nullptr ? storage->getString("charge_profile", "compatible") : String("compatible");
    return true;
  }
  return false;
}

String ConfigRegistry::schemaJson() {
  String json = "{\"hash\":";
  json += String(schemaHash());
  json += ",\"count\":";
  json += String(static_cast<unsigned>(kEntryCount));
  json += ",\"entries\":[";
  for (size_t i = 0; i < kEntryCount; ++i) {
    const ConfigEntry& entry = kEntries[i];
    if (i != 0) {
      json += ",";
    }
    json += "{\"path\":\"";
    json += entry.path;
    json += "\",\"type\":\"";
    json += typeName(entry.type);
    json += "\",\"command\":\"";
    json += entry.command;
    json += "\",\"argument\":\"";
    json += entry.argument;
    json += "\"";
    if (entry.type == ConfigType::Int || entry.type == ConfigType::Float) {
      json += ",\"min\":";
      json += String(entry.min, entry.type == ConfigType::Int ? 0 : 3);
      json += ",\"max\":";
      json += String(entry.max, entry.type == ConfigType::Int ? 0 : 3);
    }
    if (entry.allowed != nullptr) {
      json += ",\"allowed\":\"";
      json += entry.allowed;
      json += "\"";
    }
    json += ",\"summary\":\"";
    json += jsonEscape(String(entry.summary));
    json += "\"}";
  }
  json += "]}";
  return json;
}

String ConfigRegistry::valuesJson(const DeviceConfig& config, const PowerGovernor* governor,
                                  Storage* storage) {
  String json = "{";
  for (size_t i = 0; i < kEntryCount; ++i) {
    String value;
    if (!readValue(String(kEntries[i].path), config, governor, storage, value)) {
      continue;
    }
    if (json.length() > 1) {
      json += ",";
    }
    json += "\"";
    json += kEntries[i].path;
    // Reported as strings uniformly: the schema carries the type, so a
    // client parses per-entry rather than guessing per-value.
    json += "\":\"";
    json += jsonEscape(value);
    json += "\"";
  }
  json += "}";
  return json;
}

bool ConfigRegistry::encodeValue(const ConfigEntry& entry, const String& value, String& out,
                                 String& error) {
  switch (entry.type) {
    case ConfigType::Bool: {
      if (value == "true" || value == "1") { out = "true"; return true; }
      if (value == "false" || value == "0") { out = "false"; return true; }
      error = "expected_bool";
      return false;
    }
    case ConfigType::Int: {
      char* end = nullptr;
      const long parsed = strtol(value.c_str(), &end, 10);
      if (end == nullptr || *end != '\0' || value.length() == 0) {
        error = "expected_int";
        return false;
      }
      if (parsed < static_cast<long>(entry.min) || parsed > static_cast<long>(entry.max)) {
        error = String("out_of_range:") + String(entry.min, 0) + ".." + String(entry.max, 0);
        return false;
      }
      out = String(parsed);
      return true;
    }
    case ConfigType::Float: {
      char* end = nullptr;
      const double parsed = strtod(value.c_str(), &end);
      if (end == nullptr || *end != '\0' || value.length() == 0) {
        error = "expected_float";
        return false;
      }
      if (parsed < entry.min || parsed > entry.max) {
        error = String("out_of_range:") + String(entry.min, 3) + ".." + String(entry.max, 3);
        return false;
      }
      out = String(static_cast<float>(parsed), 4);
      return true;
    }
    case ConfigType::Enum: {
      const String allowed = String(entry.allowed != nullptr ? entry.allowed : "");
      // Match on comma-delimited whole tokens so "fast" cannot match inside
      // some longer legal value.
      const String needle = String(",") + value + ",";
      if ((String(",") + allowed + ",").indexOf(needle) < 0) {
        error = String("expected_one_of:") + allowed;
        return false;
      }
      out = String("\"") + jsonEscape(value) + "\"";
      return true;
    }
    case ConfigType::Text:
    default:
      out = String("\"") + jsonEscape(value) + "\"";
      return true;
  }
}

uint32_t ConfigRegistry::schemaHash() {
  // FNV-1a over the path/type/argument triples. Changes whenever the shape
  // of the surface changes, not when a value does.
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < kEntryCount; ++i) {
    const char* parts[3] = {kEntries[i].path, typeName(kEntries[i].type), kEntries[i].argument};
    for (uint8_t p = 0; p < 3; ++p) {
      for (const char* c = parts[p]; c != nullptr && *c != '\0'; ++c) {
        hash ^= static_cast<uint8_t>(*c);
        hash *= 16777619u;
      }
    }
  }
  return hash;
}

}  // namespace nhos
