#pragma once

#include <string>

namespace nhos {

// Merge the two already-serialized battery status objects without re-parsing
// their fields. A malformed side is discarded in favour of the valid object.
std::string mergeBatteryStatusJson(const std::string& chargerJson,
                                   const std::string& gaugeJson);

}  // namespace nhos
