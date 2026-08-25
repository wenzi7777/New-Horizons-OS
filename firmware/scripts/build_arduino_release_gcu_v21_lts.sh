#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SKETCH="${ROOT}/firmware/newhorizons_os"
OUT_DIR="${ROOT}/firmware/build_gcu_v21_lts"
BUILD_PATH="${OUT_DIR}/compile"
RELEASE_DIR="${ROOT}/releases/artifacts"
MANIFEST_DIR="${MANIFEST_DIR:-$(dirname "${RELEASE_DIR}")}"
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs}"
VERSION="${VERSION:-v0.17.7}"
BASE_URL="${BASE_URL:-https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/${VERSION}/releases/artifacts}"
CHANGELOG_URL="${CHANGELOG_URL:-https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/${VERSION}/releases/notes/${VERSION}.md}"

mkdir -p "${OUT_DIR}" "${BUILD_PATH}" "${RELEASE_DIR}" "${MANIFEST_DIR}"

arduino-cli compile \
  --fqbn "${FQBN}" \
  --build-path "${BUILD_PATH}" \
  --build-property "build.extra_flags=-DNHOS_BOARD_GCU_V21_LTS" \
  --build-property "compiler.cpp.extra_flags=-DNHOS_FIRMWARE_VERSION=\"${VERSION}\"" \
  "${SKETCH}" \
  --output-dir "${OUT_DIR}"

main_bin="$(find "${OUT_DIR}" -maxdepth 1 -name '*.bin' ! -name '*bootloader*' ! -name '*partitions*' ! -name '*.merged.bin' -print -quit)"
if [[ -z "${main_bin}" ]]; then
  echo "No firmware .bin emitted under ${OUT_DIR}" >&2
  exit 1
fi

target="${RELEASE_DIR}/newhorizons-os-gcu-v21-lts-${VERSION}.bin"
cp "${main_bin}" "${target}"
for manifest in "${MANIFEST_DIR}/arduino-gcu-v21-lts-latest.json" "${MANIFEST_DIR}/arduino-gcu-v21-lts-${VERSION}.json"; do
  python3 "${ROOT}/firmware/scripts/generate_arduino_manifest.py" \
    --firmware "${target}" \
    --output "${manifest}" \
    --model "VD-CTL/R v2.1 GCU LTS" \
    --version "${VERSION}" \
    --base-url "${BASE_URL}" \
    --changelog-url "${CHANGELOG_URL}"
done
echo "${target}"
