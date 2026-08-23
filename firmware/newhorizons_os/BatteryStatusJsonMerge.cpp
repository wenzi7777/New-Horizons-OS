#include "BatteryStatusJsonMerge.h"

namespace nhos {
namespace {

bool isJsonObject(const std::string& value) {
  return value.size() >= 2 && value.front() == '{' && value.back() == '}';
}

}  // namespace

std::string mergeBatteryStatusJson(const std::string& chargerJson,
                                   const std::string& gaugeJson) {
  const bool chargerIsObject = isJsonObject(chargerJson);
  const bool gaugeIsObject = isJsonObject(gaugeJson);
  if (!chargerIsObject) {
    return gaugeIsObject ? gaugeJson : chargerJson;
  }
  if (!gaugeIsObject) {
    return chargerJson;
  }

  std::string merged = chargerJson.substr(0, chargerJson.size() - 1);
  if (gaugeJson.size() > 2) {
    if (merged.size() > 1) merged += ',';
    merged += gaugeJson.substr(1);
  } else {
    merged += '}';
  }
  return merged;
}

}  // namespace nhos
