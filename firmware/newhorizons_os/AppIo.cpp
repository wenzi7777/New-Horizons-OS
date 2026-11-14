#include "AppIo.h"

#include <Wire.h>

#include "BoardConfig.h"

namespace nhos {

const void* AppIo::uartOwner_ = nullptr;

namespace {

// The bus's own timeout is the drivers' (50 ms by default); an app's transfer
// gets kTimeoutMs and the bus gets its own back afterwards.
struct ShortTimeout {
  uint16_t saved;
  ShortTimeout() : saved(Wire.getTimeOut()) { Wire.setTimeOut(AppIo::kTimeoutMs); }
  ~ShortTimeout() { Wire.setTimeOut(saved); }
};

}  // namespace

bool AppIo::i2cRead(uint8_t addr, uint8_t reg, uint8_t* out, uint8_t n) {
#if NHOS_BOARD_HAS_APP_I2C
  if (out == nullptr || n == 0) return false;
  ShortTimeout timeout;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<uint16_t>(addr), static_cast<size_t>(n), true) != n) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(Wire.read());
  return true;
#else
  (void)addr; (void)reg; (void)out; (void)n;
  return false;
#endif
}

bool AppIo::i2cWrite(uint8_t addr, const uint8_t* bytes, uint8_t n) {
#if NHOS_BOARD_HAS_APP_I2C
  if (bytes == nullptr || n == 0) return false;
  ShortTimeout timeout;
  Wire.beginTransmission(addr);
  Wire.write(bytes, n);
  return Wire.endTransmission(true) == 0;
#else
  (void)addr; (void)bytes; (void)n;
  return false;
#endif
}

bool AppIo::uartOpen(const void* owner, uint32_t baud) {
#if NHOS_BOARD_HAS_APP_UART
  if (owner == nullptr || (uartOwner_ != nullptr && uartOwner_ != owner)) return false;
  if (uartOwner_ == owner) Serial1.end();
  // UART1 routed to the FPC pins: UART0 stays free for the ROM's boot log.
  Serial1.setRxBufferSize(kUartRxBytes);
  Serial1.begin(baud, SERIAL_8N1, NHOS_BOARD_APP_UART_RX, NHOS_BOARD_APP_UART_TX);
  uartOwner_ = owner;
  return true;
#else
  (void)owner; (void)baud;
  return false;
#endif
}

void AppIo::uartClose(const void* owner) {
#if NHOS_BOARD_HAS_APP_UART
  if (owner == nullptr || uartOwner_ != owner) return;
  Serial1.end();
  pinMode(NHOS_BOARD_APP_UART_TX, INPUT);
  pinMode(NHOS_BOARD_APP_UART_RX, INPUT);
  uartOwner_ = nullptr;
#else
  (void)owner;
#endif
}

int32_t AppIo::uartWrite(const void* owner, const uint8_t* bytes, uint8_t n) {
#if NHOS_BOARD_HAS_APP_UART
  if (!uartOwnedBy(owner) || bytes == nullptr) return 0;
  // Never blocks a handler: what does not fit the TX FIFO now is not sent.
  const int room = Serial1.availableForWrite();
  const uint8_t count = room < 0 ? 0 : (room < n ? static_cast<uint8_t>(room) : n);
  return count == 0 ? 0 : static_cast<int32_t>(Serial1.write(bytes, count));
#else
  (void)owner; (void)bytes; (void)n;
  return 0;
#endif
}

int32_t AppIo::uartAvailable(const void* owner) {
#if NHOS_BOARD_HAS_APP_UART
  return uartOwnedBy(owner) ? Serial1.available() : 0;
#else
  (void)owner;
  return 0;
#endif
}

int32_t AppIo::uartRead(const void* owner) {
#if NHOS_BOARD_HAS_APP_UART
  return uartOwnedBy(owner) ? Serial1.read() : -1;
#else
  (void)owner;
  return -1;
#endif
}

}  // namespace nhos
