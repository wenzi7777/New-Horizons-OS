#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/cu.usbserial-10}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SKETCH="${ROOT}/firmware/newhorizons_os"
# Shared with the Hub (README there); every build needs it.
CORE_LIB="${ROOT}/firmware/libraries/nhos_core"
BUILD_PATH="${ROOT}/firmware/build_gcu_v22c_lts/compile"
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs}"

mkdir -p "${BUILD_PATH}"

arduino-cli compile \
  --library "${CORE_LIB}" \
  --fqbn "${FQBN}" \
  --build-path "${BUILD_PATH}" \
  --build-property "compiler.cpp.extra_flags=-DNHOS_BOARD_GCU_V22C_LTS" \
  "${SKETCH}"

arduino-cli upload \
  -p "${PORT}" \
  --fqbn "${FQBN}" \
  --input-dir "${BUILD_PATH}" \
  "${SKETCH}"
