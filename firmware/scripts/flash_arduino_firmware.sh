#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/cu.usbserial-10}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SKETCH="${ROOT}/firmware/newhorizons_os"
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB}"

arduino-cli upload \
  -p "${PORT}" \
  --fqbn "${FQBN}" \
  "${SKETCH}"
