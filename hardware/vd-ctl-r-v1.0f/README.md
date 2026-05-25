# VD-CTL/R v1.0.F 2026.4 Hardware Contract

This directory documents the fixed hardware target for the active New Horizons OS firmware.

## Board

- MCU: ESP32-S3 Mini 1 N8
- Flash: 8 MB
- PSRAM: none
- Arduino FQBN: `esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB`

## Pin Map

The source of truth used by the firmware is `firmware/newhorizons_os/BoardPins.*`.

- Analog rows: `1,2,3,4,5,6,7,8,9,10`
- Select columns: `13,14,15,16,17,18,19,20,21,26,47,33,34,48,35,36,37,38,39,40,41`
- Status LED: GPIO `11`
- External LED: GPIO `12`
- I2C SCL/SDA: GPIO `42` / `45`
- Action button: GPIO `46`

## Runtime Contract

- Protocol: `NHO/Arduino/1`
- UDP stream port: `13250`
- JSON control TCP port: `22345`
- FindMe discovery UDP port: `22346`
- Packet magic/version: `0xA55A` / `3`
