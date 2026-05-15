#!/bin/zsh
set -euo pipefail

if [[ -z "${MICROPY_SRC:-}" ]]; then
  echo "Set MICROPY_SRC to a MicroPython checkout at commit 1c63211817d9c5164542b94771634cf80b300fdf"
  exit 1
fi

source "/Users/nickxu/.espressif/tools/activate_idf_v5.5.1.sh"
export MICROPY_MPYCROSS="${MICROPY_SRC}/mpy-cross/build/mpy-cross"
export IDFPY="$IDF_PYTHON_ENV_PATH/bin/python $IDF_PATH/tools/idf.py"

cd "${MICROPY_SRC}/ports/esp32"
make BOARD=ESP32_GENERIC_S3 BUILD=build-ESP32_GENERIC_S3-vdboard-mpyfix \
  USER_C_MODULES="$(cd "$(dirname "$0")/../native/.." && pwd)/native/micropython.cmake" \
  EXTRA_CMAKE_ARGS="-DMICROPY_PY_BTREE=OFF -DMICROPY_MPYCROSS=${MICROPY_MPYCROSS}"
