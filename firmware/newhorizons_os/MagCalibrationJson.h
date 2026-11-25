#pragma once

#include <Arduino.h>

#include "MagCalibration.h"

namespace nhos {

// The calibration as JSON, one shape for both the config file and the
// set_mag_calibration command:
//   {"mode":"full","offset":[x,y,z],"matrix":[9 values, row-major],
//    "field_ut":47.1,"residual_pct":1.8,"calibrated_at_ms":1764000000000}
// Parsing does not validate the numbers (magcal::validate does); it fails
// only on a shape it cannot read. A missing mode, or "none", is the cleared
// calibration.
bool parseMagCalibrationJson(const String& json, MagCalibrationData& out, String& error);
void appendMagCalibrationJson(String& out, const MagCalibrationData& cal);

}  // namespace nhos
