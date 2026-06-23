#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SKETCH="${ROOT}/firmware/newhorizons_os"
OUT_DIR="${ROOT}/firmware/build_gcu_v21_lts"
BUILD_PATH="${OUT_DIR}/compile"
RELEASE_DIR="${ROOT}/releases/artifacts"
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs}"
VERSION="${VERSION:-v0.10.0}"

mkdir -p "${OUT_DIR}" "${BUILD_PATH}" "${RELEASE_DIR}"

arduino-cli compile \
  --fqbn "${FQBN}" \
  --build-path "${BUILD_PATH}" \
  --build-property "build.extra_flags=-DNHOS_BOARD_GCU_V21_LTS" \
  "${SKETCH}" \
  --output-dir "${OUT_DIR}"

main_bin="$(find "${OUT_DIR}" -maxdepth 1 -name '*.bin' ! -name '*bootloader*' ! -name '*partitions*' ! -name '*.merged.bin' -print -quit)"
if [[ -z "${main_bin}" ]]; then
  echo "No firmware .bin emitted under ${OUT_DIR}" >&2
  exit 1
fi

target="${RELEASE_DIR}/newhorizons-os-gcu-v21-lts-${VERSION}.bin"
cp "${main_bin}" "${target}"
echo "${target}"
