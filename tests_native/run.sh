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

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  "$SCRIPT_DIR/test_esp_now_result_pager.cpp" \
  "$FIRMWARE_DIR/EspNowResultPager.cpp" \
  -o "$BUILD_DIR/test_esp_now_result_pager"

"$BUILD_DIR/test_esp_now_result_pager"

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
  "$SCRIPT_DIR/test_battery_gauge_sync_policy.cpp" \
  "$FIRMWARE_DIR/BatteryGaugeSyncPolicy.cpp" \
  -o "$BUILD_DIR/test_battery_gauge_sync_policy"

"$BUILD_DIR/test_battery_gauge_sync_policy"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -DNHOS_BOARD_V15F \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_v15f_board_capabilities.cpp" \
  -o "$BUILD_DIR/test_v15f_board_capabilities"

"$BUILD_DIR/test_v15f_board_capabilities"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_action_button_policy.cpp" \
  "$FIRMWARE_DIR/ActionButtonPolicy.cpp" \
  -o "$BUILD_DIR/test_action_button_policy"

"$BUILD_DIR/test_action_button_policy"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_ws2812_rmt_encoder.cpp" \
  -o "$BUILD_DIR/test_ws2812_rmt_encoder"

"$BUILD_DIR/test_ws2812_rmt_encoder"

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
  "$FIRMWARE_DIR/BatteryStatusJsonMerge.cpp" \
  -o "$BUILD_DIR/test_v5_runtime"

"$BUILD_DIR/test_v5_runtime"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_calibration_curve.cpp" \
  "$FIRMWARE_DIR/CalibrationCurve.cpp" \
  -o "$BUILD_DIR/test_calibration_curve"

"$BUILD_DIR/test_calibration_curve"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_lifetime_policy.cpp" \
  "$FIRMWARE_DIR/LifetimePolicy.cpp" \
  -o "$BUILD_DIR/test_lifetime_policy"

"$BUILD_DIR/test_lifetime_policy"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_clock_core.cpp" \
  "$FIRMWARE_DIR/ClockCore.cpp" \
  -o "$BUILD_DIR/test_clock_core"

"$BUILD_DIR/test_clock_core"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_espnow_time_wire.cpp" \
  "$FIRMWARE_DIR/EspNowTimeWire.cpp" \
  "$FIRMWARE_DIR/EspNowFrame.cpp" \
  -o "$BUILD_DIR/test_espnow_time_wire"

"$BUILD_DIR/test_espnow_time_wire"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_version_compare.cpp" \
  "$FIRMWARE_DIR/VersionCompare.cpp" \
  -o "$BUILD_DIR/test_version_compare"

"$BUILD_DIR/test_version_compare" "$SCRIPT_DIR/../spec/fixtures/versions.json"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_update_channel.cpp" \
  -o "$BUILD_DIR/test_update_channel"

"$BUILD_DIR/test_update_channel"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_scan_lateness.cpp" \
  -o "$BUILD_DIR/test_scan_lateness"

"$BUILD_DIR/test_scan_lateness"

# Wire-format fixtures the Desktop and Gateway parse in their own tests: the
# committed file must be exactly what the firmware encoder produces today.
"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/gen_packet_fixtures.cpp" \
  "$FIRMWARE_DIR/PacketV5Codec.cpp" \
  -o "$BUILD_DIR/gen_packet_fixtures"

"$BUILD_DIR/gen_packet_fixtures" > "$BUILD_DIR/packets.json"
if ! cmp -s "$BUILD_DIR/packets.json" "$SCRIPT_DIR/../spec/fixtures/packets.json"; then
  echo "spec/fixtures/packets.json is out of date with PacketV5Codec.cpp; regenerate with:" >&2
  echo "  tests_native/.build/gen_packet_fixtures > spec/fixtures/packets.json" >&2
  exit 1
fi
echo "packet fixtures: up to date"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_reply_pacing.cpp" \
  -o "$BUILD_DIR/test_reply_pacing"

"$BUILD_DIR/test_reply_pacing"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  -I"$FIRMWARE_DIR" \
  "$SCRIPT_DIR/test_heavy_pacing.cpp" \
  -o "$BUILD_DIR/test_heavy_pacing"

"$BUILD_DIR/test_heavy_pacing"
