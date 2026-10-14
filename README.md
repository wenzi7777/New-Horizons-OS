# New Horizons OS Arduino

This repository is now the Arduino/C++ edition of New Horizons OS for the TIA-CTL product line, including the mainline `TIA-CTL v1.0.F 2026.4`, `TIA-CTL v2.3.D GCU LTS`, `TIA-CTL v2.2.C GCU LTS`, and `TIA-CTL v2.1 GCU LTS`.

The previous MicroPython New Horizons OS tree was archived outside this repository before the reset:

```text
/Users/nickxu/Documents/vd-ctl-r-os-lts/archives/newhorizons-os-micropython-20260524-200010/
/Users/nickxu/Documents/vd-ctl-r-os-lts/archives/newhorizons-os-micropython-20260524-200010.tar.gz
```

## Targets

- `TIA-CTL v1.0.F 2026.4`
  - MCU: ESP32-S3 Mini 1 N8
  - Flash: 8 MB
  - PSRAM: none
  - Matrix: `10 x 21`
  - Arduino FQBN: `esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB`
- `TIA-CTL v2.3.D GCU LTS`
  - Flash: 4 MB
  - Matrix: `15 x 15`
  - Arduino FQBN: `esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs`
- `TIA-CTL v2.2.C GCU LTS`
  - Flash: 4 MB
  - Matrix: `10 x 12` (IO11 and IO17 reach the connector but must not be used)
  - Arduino FQBN: `esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs`
- `TIA-CTL v2.1 GCU LTS`
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
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB --build-property "compiler.cpp.extra_flags=-DNHOS_BOARD_V10F" firmware/newhorizons_os
```

Every build must name its board (`-DNHOS_BOARD_V10F`, `-DNHOS_BOARD_V15F`,
`-DNHOS_BOARD_GCU_V21_LTS`, `-DNHOS_BOARD_GCU_V22C_LTS` or
`-DNHOS_BOARD_GCU_V23D_LTS`); a build with none fails with `#error` instead of
silently becoming v1.0.F. The per-board scripts below pass it for you.

Or use the release helper:

```bash
VERSION=v0.5.4 firmware/scripts/build_arduino_release.sh
VERSION=v0.5.4 firmware/scripts/build_arduino_release_gcu_lts.sh
VERSION=v0.5.4 firmware/scripts/build_arduino_release_gcu_v22c_lts.sh
VERSION=v0.5.4 firmware/scripts/build_arduino_release_gcu_v21_lts.sh
VERSION=v0.5.4 firmware/scripts/build_arduino_release_v15f.sh
```

## Flash

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA

# TIA-CTL v1.0.F 2026.4
firmware/scripts/flash_arduino_firmware.sh /dev/cu.usbserial-10

# TIA-CTL v2.3.D GCU LTS
firmware/scripts/flash_arduino_firmware_gcu_v23d_lts.sh /dev/cu.usbserial-10

# TIA-CTL v2.2.C GCU LTS
firmware/scripts/flash_arduino_firmware_gcu_v22c_lts.sh /dev/cu.usbserial-10

# TIA-CTL v2.1 GCU LTS
firmware/scripts/flash_arduino_firmware_gcu_v21_lts.sh /dev/cu.usbserial-10

# TIA-CTL v1.5.F 2026.7 (native USB CDC; defaults to first /dev/cu.usbmodem*)
firmware/scripts/flash_arduino_firmware_v15f.sh
```

On Windows, the same per-board scripts exist as PowerShell (compile + upload;
pass the COM port shown by `arduino-cli board list`):

```powershell
cd NewHorizonsOS-OTA
.\firmware\scripts\flash_arduino_firmware.ps1 COM5              # v1.0.F (hold Action Button)
.\firmware\scripts\flash_arduino_firmware_gcu_v23d_lts.ps1 COM5  # v2.3.D GCU LTS
.\firmware\scripts\flash_arduino_firmware_gcu_v22c_lts.ps1 COM5  # v2.2.C GCU LTS
.\firmware\scripts\flash_arduino_firmware_gcu_v21_lts.ps1 COM5   # v2.1 GCU LTS
.\firmware\scripts\flash_arduino_firmware_v15f.ps1               # v1.5.F (auto-detects native USB)
```

If script execution is blocked, run it as
`powershell -ExecutionPolicy Bypass -File .\firmware\scripts\<script>.ps1 COM5`.

Adjust the serial port after checking:

```bash
arduino-cli board list
```

## Update channels (stable / beta)

Every board follows one of two update channels, set per device
(`set_ota_config` `channel`, config key `ota.channel`, or the Desktop's
update settings). **New and reset devices are on `stable`.**

| Channel | Manifest | Gets |
|---|---|---|
| `stable` | `releases/<track>-latest.json` | public releases only |
| `beta` | `releases/<track>-beta.json` | prereleases first, plus every stable release |

- Beta versions carry a semver prerelease suffix: `v1.14.0-beta.1`. The tag
  and `releases/notes/<version>.md` use the same string.
- A prerelease sorts below its release (`v1.14.0-beta.2 < v1.14.0`), see
  `firmware/newhorizons_os/VersionCompare.h` and the shared vectors in
  `spec/fixtures/versions.json` (the Desktop, Gateway and Hub test against
  the same file).
- Leaving beta never downgrades: the device keeps its beta until a stable
  release is newer, then moves onto it by itself.
- `<track>-latest.json` is the only manifest firmware older than v1.14.0
  reads, so it must never carry a prerelease. `generate_arduino_manifest.py`
  refuses to write one there.
- `ota.manifest_url` is now an optional custom override; empty follows the
  channel. A stored URL equal to the board's old default is migrated to
  "follow the channel" on boot.

Release by channel (`CHANNEL` defaults to `stable`):

```bash
# Beta: writes <track>-<version>.json and <track>-beta.json only
CHANNEL=beta VERSION=v1.14.1-beta.1 bash firmware/scripts/build_arduino_release_v15f.sh

# Stable: writes <track>-<version>.json and <track>-latest.json, and also
# <track>-beta.json unless a newer beta is already there
VERSION=v1.14.1 bash firmware/scripts/build_arduino_release_v15f.sh
```

## OTA Manifest

The release scripts call the generator with `--track`/`--channel`, which
writes every manifest the channel needs (see above). It can also write a
single file. Generate the Arduino OTA manifest from a built firmware binary:

```bash
# TIA-CTL v1.0.F 2026.4
firmware/scripts/generate_arduino_manifest.py \
  --firmware releases/artifacts/newhorizons-os-v0.5.4.bin \
  --output releases/arduino-v10f-latest.json \
  --version v0.5.4 \
  --model "VD-CTL/R v1.0.F 2026.4" \
  --base-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/artifacts \
  --changelog-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/notes/v0.5.4.md

# TIA-CTL v2.3.D GCU LTS
firmware/scripts/generate_arduino_manifest.py \
  --firmware releases/artifacts/newhorizons-os-gcu-v23d-lts-v0.5.4.bin \
  --output releases/arduino-gcu-v23d-lts-latest.json \
  --version v0.5.4 \
  --model "VD-CTL/R v2.3.D GCU LTS" \
  --base-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/artifacts \
  --changelog-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/notes/v0.5.4.md

# TIA-CTL v2.2.C GCU LTS
firmware/scripts/generate_arduino_manifest.py \
  --firmware releases/artifacts/newhorizons-os-gcu-v22c-lts-v0.5.4.bin \
  --output releases/arduino-gcu-v22c-lts-latest.json \
  --version v0.5.4 \
  --model "VD-CTL/R v2.2.C GCU LTS" \
  --base-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/artifacts \
  --changelog-url https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.5.4/releases/notes/v0.5.4.md

# TIA-CTL v2.1 GCU LTS
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
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB --build-property "compiler.cpp.extra_flags=-DNHOS_BOARD_V10F" firmware/newhorizons_os
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs --build-property "compiler.cpp.extra_flags=-DNHOS_BOARD_GCU_V23D_LTS" firmware/newhorizons_os
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs --build-property "build.extra_flags=-DNHOS_BOARD_GCU_V22C_LTS" firmware/newhorizons_os
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs --build-property "build.extra_flags=-DNHOS_BOARD_GCU_V21_LTS" firmware/newhorizons_os
```

Hardware smoke tests still need to be run on the physical board: flash, serial boot, Wi-Fi provisioning, UDP discovery, 10x21 60 Hz stream, maintenance mode, and OTA rollback.
