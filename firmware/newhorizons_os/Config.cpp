#include "Config.h"

namespace nhos {

#if defined(NHOS_BOARD_GCU_V23D_LTS)
static_assert(kMaxSensors == 225, "GCU LTS matrix must stay 15x15");
#else
static_assert(kMaxSensors == 210, "New Horizons v1.0.F matrix must stay 10x21");
#endif
static_assert(kPacketHeaderLen == 20, "packet header is part of the UDP wire contract");

}  // namespace nhos
