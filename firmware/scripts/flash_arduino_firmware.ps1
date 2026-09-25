# Windows counterpart of flash_arduino_firmware.sh: compile and flash New Horizons OS
# to a VD-CTL/R v1.0.F 2026.4 board.
#
#   .\firmware\scripts\flash_arduino_firmware.ps1 COM5
param([string]$Port = '')

. (Join-Path $PSScriptRoot 'flash_common.ps1')

Invoke-NhosFlash -Port $Port `
  -BoardName 'VD-CTL/R v1.0.F 2026.4' `
  -Fqbn 'esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB' `
  -BuildDirName 'build_v10f' `
  -BuildProperty 'compiler.cpp.extra_flags=-DNHOS_BOARD_V10F' `
  -DownloadModeHint 'Hold the Action Button down for the WHOLE flash (it sits on IO46, a strapping pin).'
