# New Horizons OS

這個倉庫是 ESP32-S3 Mini 1（8M flash、無 PSRAM）用的 New Horizons OS 韌體與裝置端 Python 檔案系統。現在的設計不再使用 `minimal/full channel` 作為主要架構，而是拆成 Recovery、OS、Maintenance 三個責任層。

## 架構

- `firmware/`：MicroPython base firmware、`vdboard` native module、build/flash/upload/manifest 腳本。
- `device/root/`：上傳到裝置 `/` 的啟動檔，包含 `boot.py`、`main.py`、`launcher.py`、`recovery.py`。
- `device/recovery/`：Recovery Mode，保留 Wi-Fi、TCP/WebUI command、OS writer。只有這裡可以執行 `write_os`。
- `device/os/`：Normal OS source tree，會被 Recovery `write_os` 寫入裝置 `/nhos`。設備端不使用 `/os`，避免遮蔽 MicroPython 內建 `os` 模組。
- `docs/os_file_architecture.md`：裝置 runtime filesystem 與安全規則。

## 空白板刷入 Firmware

先確認 `firmware/build/esp32s3/` 已有：

- `bootloader.bin`
- `partition-table.bin`
- `micropython.bin`

刷入 base firmware：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
zsh firmware/scripts/flash_firmware.sh /dev/cu.usbserial-210
```

這個腳本只寫入 bootloader、partition table 與 MicroPython firmware，不會先整片 erase flash。

刷完 firmware 後，上傳 Recovery 檔案系統：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-210 --target recovery
```

這會上傳：

- `device/root/*` 到裝置 `/`
- `device/recovery/*` 到裝置 `/recovery`

上傳會建立需要的目錄，檔案同名時直接覆蓋，不會刪 `device_state/network_config.tlv` 或其他狀態檔。若只想覆蓋 Recovery 程式、跳過 root launcher：

```bash
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-210 --target recovery --target-only
```

## 本地開發測試上傳

正式 OS 更新應該走 Recovery WebUI/UDP 的 `write_os`，由 TLV manifest 決定部分寫入。若只是本地開發，需要直接把目前 OS source 塞進板子，可用：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-210 --target os
```

一次上傳 root、recovery、os：

```bash
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-210 --target all
```

上傳完成預設會 `soft-reset`。若要保留目前執行狀態：

```bash
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-210 --target os --no-reset
```

## OS Manifest

修改 `device/os/` 後，重生 OS manifest：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
python3 firmware/scripts/build_mpy_release.py --repo-root . --source device/os --output device/os_mpy
python3 firmware/scripts/generate_manifest.py --repo-root . --target os --version vX.Y.Z \
  --source-root device/os_mpy --base-url-path device/os_mpy \
  --delete-source-root device/os --delete-suffix .py \
  --delete-path main.py \
  --delete-path mqtt_transport.py \
  --delete-path tcp_control.py \
  --delete-path tcp_control.mpy \
  --delete-path umqtt/simple.py \
  --delete-path umqtt/robust.py \
  --delete-path umqtt/__init__.py
```

產物是：

```text
device/os/manifest.tlv
device/os_mpy/
```

OS manifest 會：

- 使用 `type: "os"` 與 `target_root: "/nhos"`。
- 發布時 OS Python 模組會先編譯成 `.mpy`，manifest 下載 `device/os_mpy/` 內的產物。
- OS canonical entrypoint 是 `/nhos/app.mpy`；launcher 與 recovery status 只用這個檔案判定 OS installed。
- manifest 會把舊 `/nhos/*.py` 與歷史 `/nhos/main.py` 加入 `delete`，避免舊 source module 留在裝置上干擾 `.mpy` import。
- 以 streaming SHA256 記錄每個檔案 hash。
- 排除 `.device/`、`device_state/`、`__pycache__/`、`.pyc` 與 manifest artifact。
- 不再包含舊 `channel` 欄位。

Recovery `write_os` 會先對 `/nhos/{path}` 算 hash；hash 相同就 skip，不同或不存在才下載到 `/ota_stage/nhos/...`，驗證成功後再套用到 `/nhos`。

## Release Metadata

Recovery 的 `check_os_release` / `write_os` 使用 GitHub 上的 release URL，不再提供 WebUI 本地 release URL 切換。預設 URL：

```text
https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/latest.tlv
```

release TLV 解碼後格式：

```text
{
  "product": "New Horizons OS",
  "latest": "v0.4.26",
  "manifest_url": "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.4.26/device/os/manifest.tlv"
}
```

Wi-Fi setup WebUI 只保留 Wi-Fi 欄位；設備連上 LAN 後會透過 UDP `22346` 自動發現 New Horizons Gateway，並使用 Gateway 回覆的 UDP control/data port。OS release 固定走 GitHub。若要做本地 OS 開發測試，請使用 `upload_filesystem.py --target os` 直接上傳，不走 release URL。

## WebUI / UDP

正式與本機 WebUI 都放在：

```text
/Users/nickxu/Documents/vd-ctl-r-os-lts/apps/newhorizons
```

New Horizons WebUI/backend 已經從 `mqtt_test` 拆出。本地請用 `apps/newhorizons` 的 Docker 啟動；之後要部署到實驗室伺服器時，再把獨立 app mount 到 `/newhorizons`。

## 命令邊界

Recovery-only：

- `check_os_release`
- `write_os`
- `reboot_to_os`

Normal/Maintenance：

- `enter_maintenance`
- `exit_maintenance`
- `maintenance_status`
- `reboot_to_recovery`
- `calibration_sample_cell`
- `calibration_sample_all`
- `calibration_save`
- `file_list`
- `file_upload_begin`
- `file_upload_chunk`
- `file_upload_finish`
- `file_download_begin`
- `file_download_chunk`
- `file_delete`
- `set_logging`
- `log_tail`
- `log_clear`

Normal/Maintenance 收到 `write_os` 會回：

```json
{
  "status": "error",
  "message": "requires_recovery",
  "next_command": "reboot_to_recovery"
}
```

## Logging / Serial Status

File log 預設啟用，寫入 `/data/logs/device.log`，並使用兩段輪替：

- Default：總容量 16KB。
- Extended：總容量 64KB。
- active 檔與 `.1` backup 各使用約一半容量。
- `log_tail` 串流讀取 `.1` 與 active，只保留最後 N 行在 RAM。
- `log_clear` 會清除 active 與 `.1`。

設定 logging：

```json
{
  "command": "set_logging",
  "enabled": true,
  "capacity": "default"
}
```

`capacity` 只接受 `default` 或 `extended`。關閉 file log 時，serial 的狀態摘要仍會輸出；預設不記錄 raw matrix frame、每次 packet 成功送出、或每秒等待訊息。

## 重新 Build Firmware

只有改到 MicroPython base、native C module、partition layout 時才需要重新 build firmware。

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
export MICROPY_SRC=/path/to/micropython
zsh firmware/scripts/build_firmware.sh
zsh firmware/scripts/flash_firmware.sh /dev/cu.usbserial-210
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-210 --target recovery
```

一般 Python OS 變更不需要重刷 firmware；重生 manifest 後走 Recovery `write_os` 即可。
