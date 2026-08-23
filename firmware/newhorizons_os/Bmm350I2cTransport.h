#pragma once

#include <cstdint>

namespace nhos {

enum class Bmm350TransportResult : int8_t {
  Success = 0,
  CommunicationFailure = -1,
  NullPointer = -2,
};

class Bmm350I2cBus {
 public:
  virtual ~Bmm350I2cBus() = default;
  virtual bool write(uint8_t address, uint8_t reg, const uint8_t* data,
                     uint8_t length) = 0;
  virtual uint8_t read(uint8_t address, uint8_t reg, uint8_t* data,
                       uint8_t length) = 0;
};

class Bmm350I2cTransport {
 public:
  static constexpr uint8_t kChunkBytes = 24;
  static Bmm350TransportResult read(Bmm350I2cBus& bus, uint8_t address,
                                    uint8_t reg, uint8_t* data, uint32_t length);
  static Bmm350TransportResult write(Bmm350I2cBus& bus, uint8_t address,
                                     uint8_t reg, const uint8_t* data,
                                     uint32_t length);
};

}  // namespace nhos
