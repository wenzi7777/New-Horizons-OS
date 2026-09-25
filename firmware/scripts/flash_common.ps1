# Shared body of the Windows flash_arduino_firmware*.ps1 scripts -- not meant
# to be run directly. Each per-board script dot-sources this file and calls
# Invoke-NhosFlash with that board's FQBN, build directory and board define,
# mirroring its .sh counterpart: compile, then upload.

$ErrorActionPreference = 'Stop'

function Write-NhosFailure([string]$Message) {
  Write-Host $Message -ForegroundColor Red
  exit 1
}

# v1.5.F enumerates as the ESP32-S3's own USB CDC (Espressif VID 0x303A)
# rather than through a USB-serial chip, so it can be picked out by VID.
function Find-NhosNativeUsbPort {
  $json = & arduino-cli board list --format json | Out-String
  if ($LASTEXITCODE -ne 0) { return $null }
  $data = $json | ConvertFrom-Json
  # arduino-cli >= 0.36 wraps the list in "detected_ports"; older ones
  # returned the bare array.
  $entries = if ($null -ne $data.detected_ports) { $data.detected_ports } else { $data }
  foreach ($entry in $entries) {
    $p = $entry.port
    if ($null -ne $p -and $p.protocol -eq 'serial' -and
        $null -ne $p.properties -and $p.properties.vid -eq '0x303A') {
      return $p.address
    }
  }
  return $null
}

function Invoke-NhosFlash {
  param(
    [string]$Port,
    [Parameter(Mandatory = $true)][string]$BoardName,
    [Parameter(Mandatory = $true)][string]$Fqbn,
    [Parameter(Mandatory = $true)][string]$BuildDirName,
    [Parameter(Mandatory = $true)][string]$BuildProperty,
    [string]$DownloadModeHint = '',
    [switch]$NativeUsb
  )

  if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
    Write-NhosFailure 'arduino-cli not found on PATH. Install it and the esp32 core first.'
  }
  if ($env:FQBN) { $Fqbn = $env:FQBN }

  if (-not $Port -and $NativeUsb) {
    $Port = Find-NhosNativeUsbPort
    if (-not $Port) {
      Write-NhosFailure 'No native USB CDC port (VID 0x303A) found; enter download mode, or pass the COM port as the first argument.'
    }
  }
  if (-not $Port) {
    Write-Host 'Usage: pass the board''s COM port as the first argument, e.g. COM5.' -ForegroundColor Red
    Write-Host 'Available ports:'
    & arduino-cli board list
    exit 1
  }

  $root = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($PSScriptRoot, '..', '..'))
  $sketch = [System.IO.Path]::Combine($root, 'firmware', 'newhorizons_os')
  $buildPath = [System.IO.Path]::Combine($root, 'firmware', $BuildDirName, 'compile')
  New-Item -ItemType Directory -Force -Path $buildPath | Out-Null

  Write-Host "Board: $BoardName"
  Write-Host "Port:  $Port"
  if ($DownloadModeHint) { Write-Host $DownloadModeHint -ForegroundColor Yellow }

  & arduino-cli compile `
    --fqbn $Fqbn `
    --build-path $buildPath `
    --build-property $BuildProperty `
    $sketch
  if ($LASTEXITCODE -ne 0) { Write-NhosFailure "Compile failed (exit $LASTEXITCODE)." }

  & arduino-cli upload `
    -p $Port `
    --fqbn $Fqbn `
    --input-dir $buildPath `
    $sketch
  if ($LASTEXITCODE -ne 0) { Write-NhosFailure "Upload failed (exit $LASTEXITCODE). Check the port is not held by a serial monitor and the board is in download mode." }
}
