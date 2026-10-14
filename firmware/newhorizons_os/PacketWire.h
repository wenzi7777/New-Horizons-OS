#pragma once

#include <cstddef>
#include <cstdint>

#include "PacketWireConstants.h"

namespace nhos {

// NHO/Arduino/1 packet helpers. The constants are generated from
// spec/wire.json into PacketWireConstants.h. No Arduino dependency, so native
// tests can exercise the exact v5 layout.
bool packetExtensionsAreWellFormed(const uint8_t* data, size_t length);
bool packetExtensionBytesFit(size_t basePayloadBytes, size_t extensionBytes,
                             size_t packetCapacity);

}  // namespace nhos
