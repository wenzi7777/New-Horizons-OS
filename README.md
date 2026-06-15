# New Horizons OS Arduino

This repository is now the Arduino/C++ edition of New Horizons OS for the VD-CTL/R product line, including the mainline `VD-CTL/R v1.0.F 2026.4`, `VD-CTL/R v2.3.D GCU LTS`, `VD-CTL/R v2.2.C GCU LTS`, and `VD-CTL/R v2.1 GCU LTS`.

The previous MicroPython New Horizons OS tree was archived outside this repository before the reset:

```text
/Users/nickxu/Documents/vd-ctl-r-os-lts/archives/newhorizons-os-micropython-20260524-200010/
/Users/nickxu/Documents/vd-ctl-r-os-lts/archives/newhorizons-os-micropython-20260524-200010.tar.gz
```

## Targets

- `VD-CTL/R v1.0.F 2026.4`
  - MCU: ESP32-S3 Mini 1 N8
  - Flash: 8 MB
  - PSRAM: none
  - Matrix: `10 x 21`
  - Arduino FQBN: `esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB`
- `VD-CTL/R v2.3.D GCU LTS`
  - Flash: 4 MB
  - Matrix: `15 x 15`
  - Arduino FQBN: `esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs`
- `VD-CTL/R v2.2.C GCU LTS`
  - Flash: 4 MB
  - Matrix: `11 x 13`
  - Arduino FQBN: `esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs`
- `VD-CTL/R v2.1 GCU LTS`
  - Flash: 4 MB
  - Matrix: `10 x 12`
  - Arduino FQBN: `esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs`
- Runtime protocol: `NHO/Arduino/1`
- UDP stream packet version: `3`

## Layout

```text
firmware/newhorizons_os/           Arduino sketch and C++ modules
firmware/scripts/                  build, flash, and OTA manifest tools
hardware/vd-ctl-r-v1.0f/           fixed board contract and pin map notes
releases/                          OTA release metadata and published firmware artifacts
tests/                             host-side scaffold and manifest tests
```

The old MicroPython runtime layout (`device/root`, `device/os`, `device/recovery`, `.mpy`, TLV per-file manifests) is no longer part of the active repository. OTA is whole-firmware OTA through the ESP32 Arduino `Update` flow.

## Build

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB firmware/newhorizons_os
```

Or use the release helper:

```bash
VERSION=v0.5.4 firmware/scripts/build_arduino_release.sh
VERSION=v0.5.4 firmware/scripts/build_arduino_release_gcu_lts.sh
VERSION=v0.5.4 firmware/scripts/build_arduino_release_gcu_v22c_lts.sh
VERSION=v0.5.4 firmware/scripts/build_arduino_release_gcu_v21_lts.sh
```

## Flash

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
firmware/scripts/flash_arduino_firmware.sh /dev/cu.usbserial-10
```

Adjust the serial port after checking:

```bash
arduino-cli board list
```

## OTA Manifest

Generate the Arduino OTA manifest from a built firmware binary:

```bash
# VD-CTL/R v1.0.F 2026.4
firmware/scripts/generate_arduino_manifest.py \
  --firmware releases/artifacts/newhorizons-os-v0.5.4.bin \
  --output releases/arduino-v10f-latest.json \
  --version v0.5.4 \
  --model "VD-CTL/R v1.0.F 2026.4" \
  --base-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/artifacts \
  --changelog-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/notes/v0.5.4.md

# VD-CTL/R v2.3.D GCU LTS
firmware/scripts/generate_arduino_manifest.py \
  --firmware releases/artifacts/newhorizons-os-gcu-v23d-lts-v0.5.4.bin \
  --output releases/arduino-gcu-v23d-lts-latest.json \
  --version v0.5.4 \
  --model "VD-CTL/R v2.3.D GCU LTS" \
  --base-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/artifacts \
  --changelog-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/notes/v0.5.4.md

# VD-CTL/R v2.2.C GCU LTS
firmware/scripts/generate_arduino_manifest.py \
  --firmware releases/artifacts/newhorizons-os-gcu-v22c-lts-v0.5.4.bin \
  --output releases/arduino-gcu-v22c-lts-latest.json \
  --version v0.5.4 \
  --model "VD-CTL/R v2.2.C GCU LTS" \
  --base-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/artifacts \
  --changelog-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/notes/v0.5.4.md

# VD-CTL/R v2.1 GCU LTS
firmware/scripts/generate_arduino_manifest.py \
  --firmware releases/artifacts/newhorizons-os-gcu-v21-lts-v0.5.4.bin \
  --output releases/arduino-gcu-v21-lts-latest.json \
  --version v0.5.4 \
  --model "VD-CTL/R v2.1 GCU LTS" \
  --base-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/artifacts \
  --changelog-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/notes/v0.5.4.md
```

The manifest shape is JSON:

```json
{
  "product": "New Horizons OS Arduino",
  "protocol": "NHO/Arduino/1",
  "model": "VD-CTL/R v1.0.F 2026.4",
  "latest": "v0.5.4",
  "changelog_url": "https://example.com/releases/notes/v0.5.4.md",
  "firmware": {
    "url": "https://example.com/newhorizons-os-v0.5.4.bin",
    "sha256": "...",
    "size": 1129936
  }
}
```

## Verification

```bash
python3 -m unittest discover -s tests -q
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB firmware/newhorizons_os
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs --build-property "build.extra_flags=-DNHOS_BOARD_GCU_V22C_LTS" firmware/newhorizons_os
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs --build-property "build.extra_flags=-DNHOS_BOARD_GCU_V21_LTS" firmware/newhorizons_os
```

Hardware smoke tests still need to be run on the physical board: flash, serial boot, Wi-Fi provisioning, UDP discovery, 10x21 60 Hz stream, maintenance mode, and OTA rollback.
