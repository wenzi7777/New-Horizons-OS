# Windows counterpart of flash_arduino_firmware_gcu_v21_lts.sh: compile and flash New Horizons OS
# to a VD-CTL/R v2.1 GCU LTS board.
#
#   .\firmware\scripts\flash_arduino_firmware_gcu_v21_lts.ps1 COM5
param([string]$Port = '')

. (Join-Path $PSScriptRoot 'flash_common.ps1')

Invoke-NhosFlash -Port $Port `
  -BoardName 'VD-CTL/R v2.1 GCU LTS' `
  -Fqbn 'esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs' `
  -BuildDirName 'build_gcu_v21_lts' `
  -BuildProperty 'build.extra_flags=-DNHOS_BOARD_GCU_V21_LTS'
