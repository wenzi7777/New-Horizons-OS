# vdboard Native Module

`vdboard` is the board-native layer for the ESP32-S3 firmware. It is intended to be
compiled into a custom MicroPython firmware as a `USER_C_MODULES` external module.

## Responsibilities

- `vdboard.scan`
  - pinned FreeRTOS scan task
  - open-drain matrix scan
  - SPSC ring buffer in shared RAM
  - per-cell blocking sample for calibration
- `vdboard.prov`
  - ESP-IDF Unified Provisioning manager
  - BLE transport only in v1
- `vdboard.sys`
  - reboot and base metadata

## Frame Layout

Each frame returned by `pop_frame_mv()` / `peek_latest_mv()` is:

- header: `<IIHHHH`
  - `seq`
  - `timestamp_ms`
  - `rows`
  - `cols`
  - `point_count`
  - `payload_type`
- payload:
  - row-major `uint16_t` millivolts for v1 (`payload_type = 1`)

Python decodes the frame with `os/frame_protocol.py`.

## Build Integration

Point `USER_C_MODULES` to [native/micropython.cmake](/Users/nickxu/Documents/vd-ctl-r-os-lts/native/micropython.cmake) when building the ESP32 MicroPython port with CMake.

Example shape:

```bash
cd micropython/ports/esp32
make submodules
idf.py set-target esp32s3
make USER_C_MODULES=/absolute/path/to/vd-ctl-r-os-lts/native/micropython.cmake BOARD=GENERIC_S3
```

Adjust `BOARD`, `sdkconfig`, and partition settings for the final hardware target.
