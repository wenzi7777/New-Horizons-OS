# New Horizons OS Arduino

This repository is now the Arduino/C++ edition of New Horizons OS for the VD-CTL/R v1.0.F 2026.4 board.

The previous MicroPython New Horizons OS tree was archived outside this repository before the reset:

```text
/Users/nickxu/Documents/vd-ctl-r-os-lts/archives/newhorizons-os-micropython-20260524-200010/
/Users/nickxu/Documents/vd-ctl-r-os-lts/archives/newhorizons-os-micropython-20260524-200010.tar.gz
```

## Target

- MCU: ESP32-S3 Mini 1 N8
- Flash: 8 MB
- PSRAM: none
- Hardware model: `VD-CTL/R v1.0.F 2026.4`
- Arduino FQBN: `esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB`
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
VERSION=v0.5.3 firmware/scripts/build_arduino_release.sh
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
firmware/scripts/generate_arduino_manifest.py \
  --firmware releases/artifacts/newhorizons-os-v0.5.3.bin \
  --output releases/arduino-latest.json \
  --version v0.5.3 \
  --base-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.3/releases/artifacts
```

The manifest shape is JSON:

```json
{
  "product": "New Horizons OS Arduino",
  "protocol": "NHO/Arduino/1",
  "model": "VD-CTL/R v1.0.F 2026.4",
  "latest": "v0.5.3",
  "firmware": {
    "url": "https://example.com/newhorizons-os-v0.5.3.bin",
    "sha256": "...",
    "size": 1129936
  }
}
```

## Verification

```bash
python3 -m pytest tests -q
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB firmware/newhorizons_os
```

Hardware smoke tests still need to be run on the physical board: flash, serial boot, Wi-Fi provisioning, UDP discovery, 10x21 60 Hz stream, maintenance mode, and OTA rollback.
