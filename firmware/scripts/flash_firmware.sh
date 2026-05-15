#!/bin/zsh
set -euo pipefail

PORT="${1:-/dev/cu.usbserial-10}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

source "/Users/nickxu/.espressif/tools/activate_idf_v5.5.1.sh"
python -m esptool --chip esp32s3 -p "${PORT}" -b 460800 --before default_reset --after hard_reset write_flash \
  0x0 "${ROOT}/build/esp32s3/bootloader.bin" \
  0x8000 "${ROOT}/build/esp32s3/partition-table.bin" \
  0x10000 "${ROOT}/build/esp32s3/micropython.bin"
