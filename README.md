# New Horizons OS

這個倉庫是 `New Horizons OS` 的 OTA 測試倉，分成兩層：

- `firmware/`：Base firmware、`vdboard` native overlay、build/flash/manifest 腳本。
- `device/`：不可熱修的 immutable launcher，以及 `minimal` / `full` 兩個 OTA channel。

v1 目前採用：

- Base firmware 只負責 MicroPython + native module。
- 首次配網改成 `SoftAP + Web UI`，不再使用 BLE provisioning。
- 裝置識別、AP SSID 與封包 `device_id` 都以板子的 `MAC` 派生。

## 先理解兩層更新

- `Base firmware`
  - 包含 MicroPython 韌體本體與 `vdboard` native module。
  - 這一層的更新需要重新刷機。
- `OTA app`
  - 包含 `device/immutable/`、`device/channels/minimal/files/`、`device/channels/full/files/`。
  - 這一層的更新由 manifest 驅動，裝置依檔案 hash 自行下載更新。

換句話說：

- 你改的是 Python/config/UI/控制流程：通常只要 OTA。
- 你改的是 C module / MicroPython base / partition：需要重刷 firmware。

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

## 前置需求

### 1. Firmware 刷機環境

- macOS
- `ESP-IDF v5.5.1`
- `esptool`
- `mpremote`

安裝 `mpremote`：

```bash
python3 -m pip install --user mpremote
```

### 2. 已存在的腳本

- 建置 firmware：`firmware/scripts/build_firmware.sh`
- 刷寫 firmware：`firmware/scripts/flash_firmware.sh`
- 產生 manifest：`firmware/scripts/generate_manifest.py`
- 上傳檔案系統：`firmware/scripts/upload_filesystem.py`

## 空白板首次流程

以下是「一張空白板子」從零開始的完整流程。

1. 刷 base firmware：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
zsh firmware/scripts/flash_firmware.sh /dev/cu.usbserial-10
```

這個腳本實際刷入：

- `firmware/build/esp32s3/bootloader.bin`
- `firmware/build/esp32s3/partition-table.bin`
- `firmware/build/esp32s3/micropython.bin`

2. 上傳 immutable + `minimal` 檔案系統：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
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

## 已有板子時，什麼情況需要重刷

### 只改了 OTA app

例如你改了這些：

- `device/channels/minimal/files/*.py`
- `device/channels/full/files/*.py`
- `device/immutable/*.py`
- `manifest.json`

這種情況：

- 不需要重刷 `bootloader.bin`
- 不需要重刷 `micropython.bin`
- 只需要上傳檔案，或走 OTA 更新

### 改了 base firmware

例如你改了這些：

- `firmware/native/vdboard/*.c`
- `firmware/native/vdboard/*.h`
- MicroPython build 相關設定
- partition layout

這種情況：

- 需要重新 build firmware
- 需要重新刷機

## 切換到 Full

### 方式 1：直接覆蓋上傳

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-10 --channel full --channel-only
```

### 方式 2：透過本機 Web UI / UDP 控制

先啟動本機控制台：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
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

## Build Firmware

如果你改了 native C module，需要先重新 build。

`firmware/micropython/UPSTREAM.lock` 記錄了目前測試使用的 MicroPython commit 與 build 參數。

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
export MICROPY_SRC=/path/to/micropython
zsh firmware/scripts/build_firmware.sh
```

build 完成後，請把產物複製或同步到：

- `firmware/build/esp32s3/bootloader.bin`
- `firmware/build/esp32s3/partition-table.bin`
- `firmware/build/esp32s3/micropython.bin`

再用：

```bash
zsh firmware/scripts/flash_firmware.sh /dev/cu.usbserial-10
```

## Manifest 與 Tag 發布規則

裝置應該指向 **tag 版** manifest，而不是 `main` branch raw URL。

目前格式：

- `minimal`
  - `https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.1.0/device/channels/minimal/manifest.json`
- `full`
  - `https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.1.0/device/channels/full/manifest.json`

這代表：

- 你平常直接 `git push main`，不會讓已經鎖定 `v0.1.0` 的裝置自動升級
- 正式 OTA 發布時，必須建立新 tag

## 日常開發測試

如果只是本地開發板測試，你可以：

1. 改檔
2. 重生 manifest
3. 直接上傳 channel，或推到 GitHub 後讓測試板抓新 tag

重生 manifest：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
python3 firmware/scripts/generate_manifest.py --repo-root . --channel minimal --version v0.1.0
python3 firmware/scripts/generate_manifest.py --repo-root . --channel full --version v0.1.0
```

## 正式發布新的 OTA 版本

假設你要發布 `v0.1.1`：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA

python3 firmware/scripts/generate_manifest.py --repo-root . --channel minimal --version v0.1.1
python3 firmware/scripts/generate_manifest.py --repo-root . --channel full --version v0.1.1

git add .
git commit -m "Release v0.1.1 OTA update"
git push origin main

git tag v0.1.1
git push origin v0.1.1
```

如果這次也有新的 base firmware，建議再做 GitHub Release，並附上：

- `firmware/build/esp32s3/bootloader.bin`
- `firmware/build/esp32s3/partition-table.bin`
- `firmware/build/esp32s3/micropython.bin`

## 裝置如何拿到新 OTA

裝置能否拿到新版本，取決於它目前設定的 `manifest_url`。

### 如果裝置指向舊 tag

例如：

```text
.../v0.1.0/device/channels/full/manifest.json
```

那你就算 `push main`，它也不會升級。

### 如果要讓裝置升級到新版本

你需要把裝置改到新 manifest，例如：

```text
.../v0.1.1/device/channels/full/manifest.json
```

做法可以是：

- 重新寫入 `.device/runtime_config.json`
- 透過 `minimal` / `full` 的控制命令更新 channel/manifest
- 透過本機 `host_ui.py` 的控制功能下發

## 直接上傳到板子

如果你不想經過 GitHub / OTA，想直接把目前 repo 內容塞進板子，可用以下方式。

建議用官方 `mpremote`：

```bash
python3 -m pip install --user mpremote
```

首次上板，上傳 immutable + `minimal`：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-10 --channel minimal
```

如果之後只想覆蓋 channel，不想再碰 immutable：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-10 --channel full --channel-only
```

預設上傳完會自動 `soft-reset`。如果你要保留目前執行狀態：

```bash
cd /Users/nickxu/Documents/vd-ctl-r-os-lts/NewHorizonsOS-OTA
python3 firmware/scripts/upload_filesystem.py --port /dev/cu.usbserial-10 --channel minimal --no-reset
```
