# New Horizons OS 裝置檔案架構

這份文件定義 ESP32-S3 裝置上的目標 filesystem layout。新的設計以 Recovery / OS / Maintenance 分工取代舊的 `minimal/full channel` runtime overlay。

## Runtime Layout

```text
/
  boot.py
  main.py
  launcher.py
  recovery.py
  device_state/
    runtime_config.tlv
    network_config.tlv
    os_state.tlv
    update_state.tlv
  recovery/
    recovery_app.py
    wifi_portal.py
    udp_control.py
    udp_stream.py
    os_writer.py
    storage.py
    device_identity.py
    device_logging.py
  nhos/
    app.py
    config.py
    runtime_config.py
    udp_control.py
    calibration_store.py
    ...
  ota_stage/
    nhos/
      ...
  data/
    files/
    logs/
      device.log
      device.log.1
      maintenance.log
      writer.log
    tmp/
```

## Ownership Rules

- root 啟動檔加上 `/recovery` 組成 Recovery environment；Normal OS writer 不得寫入這兩個區域。
- `/nhos` 是已安裝的 New Horizons OS；Recovery `write_os` 只安裝或更新這棵樹。設備端不能使用 `/os`，因為會遮蔽 MicroPython 內建 `os` 模組並造成 `mpremote` 的 `os.stat` 失敗。
- `/ota_stage/nhos` 是 OS writer 的暫存下載區；只有 hash 驗證成功後才會 promote 到 `/nhos`。
- `/data/files` 是 Maintenance file commands 唯一可管理的使用者檔案區。
- `/data/logs` 是 Maintenance log commands 唯一可暴露的 log 區。`device.log` 預設啟用，總容量 default 16KB / extended 64KB，使用 active + `.1` 兩段輪替。
- `/data/tmp` 是 upload/download 暫存區。
- `/device_state` 存放小型設定與 boot/update state，只能透過明確定義的 config/state API 更新。

## Mode Responsibilities

- Recovery Mode 從 `/recovery` 執行，只能執行 `check_os_release`、`write_os`、`reboot_to_os`。
- Normal Mode 從 `/nhos` 執行，負責矩陣掃描與資料回報。
- Maintenance Mode 是 `/nhos` 內的主系統模式，會停止正常 streaming，並暴露校準、檔案與 log 命令。

## Repository Source Layout

```text
device/
  root/        # boot.py, main.py, launcher.py, recovery.py，上傳到 /
  recovery/    # Wi-Fi/TCP/WebUI command receiver 與 OS writer，上傳到 /recovery
  os/          # normal OS source tree 與 OS manifest，由 Recovery write_os 寫入 /nhos
```

`device/channels/*` 與 `device/immutable/*` 已不再是 runtime source-of-truth。新的文件與日常操作只使用 `--target recovery` 與 `--target os`。

## Safety Rules

- OS manifest path 一律相對於 `/nhos`。
- OS writer 會對現有 `/nhos/{path}` 做 streaming SHA256；hash 相同則 skip。
- OS manifest 的 `delete` entries 只能移除 `/nhos` 底下的檔案。
- Maintenance file commands 會拒絕 absolute path、空 path、以及包含 `.` 或 `..` 的 path。
- Maintenance file commands 必須 chunk-based，不能把整個檔案讀進 RAM。

## Local Development Upload Targets

- `recovery`：上傳 `device/root` 到 `/`，並上傳 `device/recovery` 到 `/recovery`。
- `os`：只上傳 `device/os` 到 `/nhos`，用於本地開發測試。
- `all`：上傳 root、recovery、OS。

正式 OS 更新應使用 Recovery `write_os`。它固定從 GitHub `releases/latest.tlv` 取得 OS manifest，並且只寫入 hash 不同的 `/nhos` 檔案。本地開發測試不要切換 release URL，直接使用 `upload_filesystem.py --target os`。

## Firmware / Filesystem Commands

空白板刷入 base firmware：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
zsh firmware/scripts/flash_firmware.sh /dev/cu.usbserial-210
```

`flash_firmware.sh` 只寫入 bootloader、partition table 與 MicroPython firmware，不會先整片 erase flash。

刷完 firmware 後寫入 Recovery：

```bash
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-210 --target recovery
```

本地開發直接寫入 OS：

```bash
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-210 --target os
```

重生 OS manifest：

```bash
python3 firmware/scripts/generate_manifest.py --repo-root . --target os --version v0.2.4
```
