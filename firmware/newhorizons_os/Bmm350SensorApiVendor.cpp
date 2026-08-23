#include "BoardConfig.h"

#if NHOS_BOARD_MAG_MODEL == 2
// Bosch Sensortec BMM350 SensorAPI v1.10.0, BSD-3-Clause.
// Upstream revision: 3daf377ccaf589319c0d41af192105e9275987a2.
// Keep the vendor source as C and include it once so Arduino's sketch build
// reliably compiles the third_party directory on every supported target.
extern "C" {
#include "third_party/bmm350/bmm350.c"
}
#endif  // NHOS_BOARD_MAG_MODEL == 2
