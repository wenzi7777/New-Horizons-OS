# Windows counterpart of flash_arduino_firmware_v20f.sh: compile and flash New Horizons OS
# to a TIA-CTL v2.0.F 2026.10 board.
#
#   .\firmware\scripts\flash_arduino_firmware_v20f.ps1  # or pass the COM port
param([string]$Port = '')

. (Join-Path $PSScriptRoot 'flash_common.ps1')

Invoke-NhosFlash -Port $Port `
  -BoardName 'TIA-CTL v2.0.F 2026.10' `
  -Fqbn 'esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,USBMode=hwcdc,CDCOnBoot=cdc' `
  -BuildDirName 'build_v20f' `
  -BuildProperty 'compiler.cpp.extra_flags=-DNHOS_BOARD_V20F' `
  -DownloadModeHint 'Enter download mode first: hold BOOT, tap RST, release BOOT.' `
  -NativeUsb
