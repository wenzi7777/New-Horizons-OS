#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SKETCH="${ROOT}/firmware/newhorizons_os"
# Shared with the Hub (README there); every build needs it.
CORE_LIB="${ROOT}/firmware/libraries/nhos_core"
OUT_DIR="${ROOT}/firmware/build_gcu_v22c_lts"
BUILD_PATH="${OUT_DIR}/compile"
RELEASE_DIR="${ROOT}/releases/artifacts"
MANIFEST_DIR="${MANIFEST_DIR:-$(dirname "${RELEASE_DIR}")}"
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs}"
VERSION="${VERSION:-v1.0.0}"
# stable (default) or beta; beta versions carry a suffix, e.g. v1.14.0-beta.1.
CHANNEL="${CHANNEL:-stable}"
BASE_URL="${BASE_URL:-https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/${VERSION}/releases/artifacts}"
CHANGELOG_URL="${CHANGELOG_URL:-https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/${VERSION}/releases/notes/${VERSION}.md}"

mkdir -p "${OUT_DIR}" "${BUILD_PATH}" "${RELEASE_DIR}" "${MANIFEST_DIR}"

arduino-cli compile \
  --library "${CORE_LIB}" \
  --fqbn "${FQBN}" \
  --build-path "${BUILD_PATH}" \
  --build-property "compiler.cpp.extra_flags=-DNHOS_BOARD_GCU_V22C_LTS -DNHOS_FIRMWARE_VERSION=\"${VERSION}\"" \
  "${SKETCH}" \
  --output-dir "${OUT_DIR}"

main_bin="$(find "${OUT_DIR}" -maxdepth 1 -name '*.bin' ! -name '*bootloader*' ! -name '*partitions*' ! -name '*.merged.bin' -print -quit)"
if [[ -z "${main_bin}" ]]; then
  echo "No firmware .bin emitted under ${OUT_DIR}" >&2
  exit 1
fi

target="${RELEASE_DIR}/newhorizons-os-gcu-v22c-lts-${VERSION}.bin"
cp "${main_bin}" "${target}"
# Which manifests this lands in depends on CHANNEL (stable/beta), see
# publish_track() in generate_arduino_manifest.py: stable writes
# arduino-gcu-v22c-lts-latest.json (and -beta.json unless a newer beta is there),
# beta writes only arduino-gcu-v22c-lts-beta.json. The manifest keeps the board's
# pre-rename VD-CTL/R name (NHOS_BOARD_OTA_MODEL): firmware older than the
# TIA-CTL rename only accepts an exact match.
python3 "${ROOT}/firmware/scripts/generate_arduino_manifest.py" \
  --firmware "${target}" \
  --track arduino-gcu-v22c-lts \
  --manifest-dir "${MANIFEST_DIR}" \
  --channel "${CHANNEL}" \
  --model "VD-CTL/R v2.2.C GCU LTS" \
  --version "${VERSION}" \
  --base-url "${BASE_URL}" \
  --changelog-url "${CHANGELOG_URL}"
echo "${target}"
