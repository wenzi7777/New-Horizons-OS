#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SKETCH="${ROOT}/firmware/newhorizons_os"
OUT_DIR="${ROOT}/firmware/build"
RELEASE_DIR="${ROOT}/releases/artifacts"
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB}"
VERSION="${VERSION:-v0.6.2}"

mkdir -p "${OUT_DIR}" "${RELEASE_DIR}"

arduino-cli compile \
  --fqbn "${FQBN}" \
  "${SKETCH}" \
  --output-dir "${OUT_DIR}"

main_bin="$(find "${OUT_DIR}" -maxdepth 1 -name '*.bin' ! -name '*bootloader*' ! -name '*partitions*' ! -name '*.merged.bin' -print -quit)"
if [[ -z "${main_bin}" ]]; then
  echo "No firmware .bin emitted under ${OUT_DIR}" >&2
  exit 1
fi

target="${RELEASE_DIR}/newhorizons-os-${VERSION}.bin"
cp "${main_bin}" "${target}"
echo "${target}"
