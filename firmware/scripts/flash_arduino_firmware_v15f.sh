#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-}"
if [[ -z "${PORT}" ]]; then
  PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -n 1 || true)"
fi
if [[ -z "${PORT}" ]]; then
  echo "No native USB CDC port found; pass /dev/cu.usbmodem* as the first argument." >&2
  exit 1
fi

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SKETCH="${ROOT}/firmware/newhorizons_os"
BUILD_PATH="${ROOT}/firmware/build_v15f/compile"
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,USBMode=hwcdc,CDCOnBoot=cdc}"

mkdir -p "${BUILD_PATH}"

arduino-cli compile \
  --fqbn "${FQBN}" \
  --build-path "${BUILD_PATH}" \
  --build-property "compiler.cpp.extra_flags=-DNHOS_BOARD_V15F" \
  "${SKETCH}"

arduino-cli upload \
  -p "${PORT}" \
  --fqbn "${FQBN}" \
  --input-dir "${BUILD_PATH}" \
  "${SKETCH}"
