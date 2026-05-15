# New Horizons OS

這個倉庫是 `New Horizons OS` 的 OTA 測試倉，分成兩層：

- `firmware/`：Base firmware、`vdboard` native overlay、build/flash/manifest 腳本。
- `device/`：不可熱修的 immutable launcher，以及 `minimal` / `full` 兩個 OTA channel。

v1 目前採用：

- Base firmware 只負責 MicroPython + native module。
- 首次配網改成 `SoftAP + Web UI`，不再使用 BLE provisioning。
- 裝置識別、AP SSID 與封包 `device_id` 都以板子的 `MAC` 派生。

## 目錄

- `device/immutable/`
  - 首次上板必須一起放進裝置 root 的 immutable launcher 與共用工具。
- `device/channels/minimal/files/`
  - 最小可測 OTA channel，主入口是 `app_minimal.py`。
  - 主要用途：首次配網、OTA 驗證、切換到 `full`。
- `device/channels/full/files/`
  - 完整主系統 channel，主入口是 `app.py`。
- `host_ui.py`
  - 本機單一程式 Web UI。
  - 可做多裝置切換、UDP 控制、OTA 升級、感測矩陣可視化、校準操作。

## 空白板首次流程

1. 刷 base firmware：

```bash
zsh firmware/scripts/flash_firmware.sh /dev/cu.usbserial-10
```

2. 上傳 immutable + `minimal`：

```bash
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-10 --channel minimal
```

3. 重開機後，在 3 秒 boot window 內按一下 action button。

4. 裝置會啟動 SoftAP，SSID 格式為：

```text
NewHorizonsOS-<MAC後6碼>
```

5. 連上這個 AP，打開：

```text
http://192.168.4.1
```

6. 在 Web UI 輸入正式 Wi-Fi 帳密。成功連網後，裝置會以 `minimal` channel 啟動。

## 切換到 Full

### 方式 1：直接覆蓋上傳

```bash
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-10 --channel full --channel-only
```

### 方式 2：透過本機 Web UI / UDP 控制

先啟動本機控制台：

```bash
python3 host_ui.py --http-port 8787 --udp-port 5005 --control-local-port 22345
```

打開：

```text
http://127.0.0.1:8787
```

然後在 UI 中：

1. 加入裝置 IP。
2. 先送 `set_servers`，把 `master_server` / `data_server` 指到你的電腦。
3. 送 `upgrade_to_full`，裝置就會切到 `full` manifest、下載更新並於需要時重啟。

## 本機 Web UI

`host_ui.py` 是單一程式版本，內含：

- UDP data receiver
- UDP control client
- HTTP Web UI

支援功能：

- 多裝置列表與切換
- 即時矩陣熱圖
- `status` / `check_update` / `apply_update` / `upgrade_to_full`
- `set_servers`
- `enter_calibration_mode` / `start_calibration` / `calibrate_all` / `dump_calibration`

預設通訊規則：

- 裝置資料上送：`UDP 5005`
- 控制命令來源：本機 `UDP 22345`

## Manifest 與 Tag

- `minimal`
  - `https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.1.0/device/channels/minimal/manifest.json`
- `full`
  - `https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.1.0/device/channels/full/manifest.json`

更新 manifest：

```bash
python3 firmware/scripts/generate_manifest.py --repo-root . --channel minimal --version v0.1.0
python3 firmware/scripts/generate_manifest.py --repo-root . --channel full --version v0.1.0
```

## Base Firmware Build

`firmware/micropython/UPSTREAM.lock` 記錄了目前測試使用的 MicroPython commit 與 build 參數。

重建時：

```bash
MICROPY_SRC=/path/to/micropython firmware/scripts/build_firmware.sh
```

## 直接上傳到板子

建議用官方 `mpremote`：

```bash
python3 -m pip install --user mpremote
```

首次上板，上傳 immutable + `minimal`：

```bash
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-10 --channel minimal
```

如果之後只想覆蓋 channel，不想再碰 immutable：

```bash
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-10 --channel full --channel-only
```

預設上傳完會自動 `soft-reset`。如果你要保留目前執行狀態：

```bash
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-10 --channel minimal --no-reset
```
