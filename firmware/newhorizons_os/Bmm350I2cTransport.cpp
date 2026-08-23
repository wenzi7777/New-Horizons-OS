#include "Bmm350I2cTransport.h"

namespace nhos {
namespace {
uint8_t chunkLength(uint32_t remaining) {
  return static_cast<uint8_t>(
      remaining > Bmm350I2cTransport::kChunkBytes
          ? Bmm350I2cTransport::kChunkBytes
          : remaining);
}
}  // namespace

Bmm350TransportResult Bmm350I2cTransport::read(Bmm350I2cBus& bus, uint8_t address,
                                                uint8_t reg, uint8_t* data,
                                                uint32_t length) {
  if (!data) return Bmm350TransportResult::NullPointer;
  for (uint32_t offset = 0; offset < length;) {
    const uint8_t count = chunkLength(length - offset);
    if (bus.read(address, static_cast<uint8_t>(reg + offset), data + offset, count) != count) {
      return Bmm350TransportResult::CommunicationFailure;
    }
    offset += count;
  }
  return Bmm350TransportResult::Success;
}

Bmm350TransportResult Bmm350I2cTransport::write(Bmm350I2cBus& bus, uint8_t address,
                                                 uint8_t reg, const uint8_t* data,
                                                 uint32_t length) {
  if (!data) return Bmm350TransportResult::NullPointer;
  for (uint32_t offset = 0; offset < length;) {
    const uint8_t count = chunkLength(length - offset);
    if (!bus.write(address, static_cast<uint8_t>(reg + offset), data + offset, count)) {
      return Bmm350TransportResult::CommunicationFailure;
    }
    offset += count;
  }
  return Bmm350TransportResult::Success;
}

}  // namespace nhos
