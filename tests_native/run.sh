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

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  "$SCRIPT_DIR/test_ota_chunk_codec.cpp" \
  "$FIRMWARE_DIR/OtaChunkCodec.cpp" \
  -o "$BUILD_DIR/test_ota_chunk_codec"

"$BUILD_DIR/test_ota_chunk_codec"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_v15f_foundation.cpp" \
  "$FIRMWARE_DIR/BatteryManualProfile.cpp" \
  "$FIRMWARE_DIR/BatteryProfile.cpp" \
  -o "$BUILD_DIR/test_v15f_foundation"

"$BUILD_DIR/test_v15f_foundation"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_v15f_regressions.cpp" \
  "$FIRMWARE_DIR/Bmm350BridgePolicy.cpp" \
  "$FIRMWARE_DIR/Bmm350I2cTransport.cpp" \
  "$FIRMWARE_DIR/BatteryChargeSafety.cpp" \
  "$FIRMWARE_DIR/MagnetometerSampleCache.cpp" \
  "$FIRMWARE_DIR/MagnetometerSamplePolicy.cpp" \
  "$FIRMWARE_DIR/PacketSensorBlocks.cpp" \
  "$FIRMWARE_DIR/PowerStatusJson.cpp" \
  -o "$BUILD_DIR/test_v15f_regressions"

"$BUILD_DIR/test_v15f_regressions"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_v5_runtime.cpp" \
  "$FIRMWARE_DIR/PacketV5Codec.cpp" \
  "$FIRMWARE_DIR/BatteryManualProfile.cpp" \
  "$FIRMWARE_DIR/BatteryLedPolicy.cpp" \
  -o "$BUILD_DIR/test_v5_runtime"

"$BUILD_DIR/test_v5_runtime"
