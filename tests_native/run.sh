#!/usr/bin/env bash
# Builds and runs the host-native (non-Arduino) firmware unit tests.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$SCRIPT_DIR/../firmware/newhorizons_os"
BUILD_DIR="$SCRIPT_DIR/.build"
mkdir -p "$BUILD_DIR"

CXX="${CXX:-c++}"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  "$SCRIPT_DIR/test_esp_now_frame.cpp" \
  "$FIRMWARE_DIR/EspNowFrame.cpp" \
  -o "$BUILD_DIR/test_esp_now_frame"

"$BUILD_DIR/test_esp_now_frame"
