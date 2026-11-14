#pragma once

#include <Arduino.h>

namespace nhos {

// The external I/O an app may reach with io_i2c / io_uart (v2.1.0), on the
// boards that expose it (NHOS_BOARD_HAS_APP_I2C / _UART: v1.5.F and v2.0.F).
// Everywhere else every call refuses, and the registry has already refused
// to install a package asking for it.
//
// I2C is the board's own bus (Wire), so VmCore has already refused the
// board's chips (kVmI2cDenied); a transfer is held to kTimeoutMs and its time
// counts against the calling app's slice like any other work.
//
// The UART is one port with one owner: the first app to open it keeps it
// until it closes it or stops, when the driver is removed and the pins go
// back to inputs. Its driver and 256-byte receive buffer exist only while
// open.
class AppIo {
 public:
  static constexpr uint16_t kTimeoutMs = 5;
  static constexpr uint16_t kUartRxBytes = 256;

  static bool i2cRead(uint8_t addr, uint8_t reg, uint8_t* out, uint8_t n);
  // bytes[0] is the register.
  static bool i2cWrite(uint8_t addr, const uint8_t* bytes, uint8_t n);

  static bool uartOpen(const void* owner, uint32_t baud);
  static void uartClose(const void* owner);
  static int32_t uartWrite(const void* owner, const uint8_t* bytes, uint8_t n);
  static int32_t uartAvailable(const void* owner);
  static int32_t uartRead(const void* owner);
  static bool uartOwnedBy(const void* owner) { return owner != nullptr && uartOwner_ == owner; }

 private:
  static const void* uartOwner_;
};

}  // namespace nhos
