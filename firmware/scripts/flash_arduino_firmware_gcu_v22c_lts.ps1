# Windows counterpart of flash_arduino_firmware_gcu_v22c_lts.sh: compile and flash New Horizons OS
# to a VD-CTL/R v2.2.C GCU LTS board.
#
#   .\firmware\scripts\flash_arduino_firmware_gcu_v22c_lts.ps1 COM5
param([string]$Port = '')

. (Join-Path $PSScriptRoot 'flash_common.ps1')

Invoke-NhosFlash -Port $Port `
  -BoardName 'VD-CTL/R v2.2.C GCU LTS' `
  -Fqbn 'esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs' `
  -BuildDirName 'build_gcu_v22c_lts' `
  -BuildProperty 'build.extra_flags=-DNHOS_BOARD_GCU_V22C_LTS'
