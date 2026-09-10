#pragma once

#include <Arduino.h>

namespace nhos {

class DeviceConfig;
class PowerGovernor;
class Storage;

enum class ConfigType : uint8_t { Bool, Int, Float, Text, Enum };

// One declaratively described knob.
//
// `command` / `argument` are how a write is performed: rather than
// reimplementing validation, config_set synthesises the request the existing
// set_* handler already accepts and dispatches to it. That handler stays the
// single owner of range-checking, persistence and the apply step, so a
// generic setter cannot drift away from the specific one.
struct ConfigEntry {
  const char* path;
  ConfigType type;
  const char* command;   // existing set_* command that applies this value
  const char* argument;  // its argument name
  float min;             // Int/Float only; 0 otherwise
  float max;
  const char* allowed;   // Enum only: comma-separated legal values
  const char* summary;
};

// The device's introspectable configuration surface -- the sysctl table.
//
// Before this, every knob was a bespoke set_* command in a 1500-line
// if-chain, and each one had to be hardcoded again in the backend allowlist,
// the terminal help, the settings page and three i18n files. The schema is
// now something the device reports about itself, so those can be generated
// instead of hand-maintained.
//
// Deliberately a description layer, not a replacement: the set_* commands
// keep working untouched and remain the implementation.
class ConfigRegistry {
 public:
  static const ConfigEntry* entries();
  static size_t entryCount();
  static const ConfigEntry* find(const String& path);
  static const char* typeName(ConfigType type);

  // Schema, as reported by config_schema. Stable enough to hash and cache.
  static String schemaJson();
  // Current values for every entry, as reported by config_get.
  static String valuesJson(const DeviceConfig& config, const PowerGovernor* governor,
                           Storage* storage);
  static bool readValue(const String& path, const DeviceConfig& config,
                        const PowerGovernor* governor, Storage* storage, String& out);

  // Cheap stable fingerprint of the schema, so a client can tell whether its
  // cached copy is still valid without refetching it.
  static uint32_t schemaHash();

  // Renders `value` as the JSON scalar the owning handler's extractor
  // expects, and range/enum-checks it on the way. Numbers and bools MUST go
  // over unquoted: the extractors take the raw text up to the next delimiter
  // and strtol() it, so a quoted "30" fails to parse and silently falls back
  // to the argument's compile-time default. Returns false (with `error` set)
  // rather than emitting something the handler would misread.
  static bool encodeValue(const ConfigEntry& entry, const String& value, String& out,
                          String& error);
};

}  // namespace nhos
