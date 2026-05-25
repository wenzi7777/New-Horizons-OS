#include "Config.h"

namespace nhos {

static_assert(kMaxSensors == 210, "New Horizons v1.0.F matrix must stay 10x21");
static_assert(kPacketHeaderLen == 20, "packet header is part of the UDP wire contract");

}  // namespace nhos
