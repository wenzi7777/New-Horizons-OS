#!/bin/zsh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEFAULT_MICROPY_SRC="$(cd "${ROOT}/../third_party/micropython" 2>/dev/null && pwd || true)"

if [[ -z "${MICROPY_SRC:-}" && -n "${DEFAULT_MICROPY_SRC}" && -d "${DEFAULT_MICROPY_SRC}/ports/esp32" ]]; then
  MICROPY_SRC="${DEFAULT_MICROPY_SRC}"
fi

if [[ -z "${MICROPY_SRC:-}" || ! -d "${MICROPY_SRC}/ports/esp32" ]]; then
  echo "Set MICROPY_SRC to a MicroPython checkout at commit 1c63211817d9c5164542b94771634cf80b300fdf"
  exit 1
fi

BOARD_NAME="${MICROPY_BOARD:-NEWHORIZONS_ESP32S3_N8}"
BOARD_DIR="${MICROPY_BOARD_DIR:-${ROOT}/firmware/micropython/boards/${BOARD_NAME}}"
BUILD="${MICROPY_BUILD:-build-${BOARD_NAME}-vdboard-8m}"
ARTIFACT_DIR="${ROOT}/firmware/build/esp32s3"
PARTITION_NAME="partitions-newhorizons-8MiBplus.csv"

set +u
source "/Users/nickxu/.espressif/tools/activate_idf_v5.5.1.sh"
set -u
export MICROPY_MPYCROSS="${MICROPY_SRC}/mpy-cross/build/mpy-cross"
export IDFPY="$IDF_PYTHON_ENV_PATH/bin/python $IDF_PATH/tools/idf.py"

cd "${MICROPY_SRC}/ports/esp32"
cp "${BOARD_DIR}/partitions-8MiBplus.csv" "${PARTITION_NAME}"
rm -f "${BUILD}/sdkconfig" "${BUILD}/sdkconfig.old"

make BOARD="${BOARD_NAME}" BOARD_DIR="${BOARD_DIR}" BUILD="${BUILD}" \
  USER_C_MODULES="${ROOT}/firmware/native/micropython.cmake" \
  EXTRA_CMAKE_ARGS="-DMICROPY_PY_BTREE=OFF -DMICROPY_MPYCROSS=${MICROPY_MPYCROSS}"

mkdir -p "${ARTIFACT_DIR}"
cp "${BUILD}/bootloader/bootloader.bin" "${ARTIFACT_DIR}/bootloader.bin"
cp "${BUILD}/partition_table/partition-table.bin" "${ARTIFACT_DIR}/partition-table.bin"
cp "${BUILD}/micropython.bin" "${ARTIFACT_DIR}/micropython.bin"

echo "Firmware artifacts written to ${ARTIFACT_DIR}"
