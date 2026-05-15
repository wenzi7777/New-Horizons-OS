# New Horizons OS

這個倉庫是 `New Horizons OS` 的 OTA 測試倉，內容分成兩層：

- `firmware/`：Base firmware、`vdboard` native overlay、build/flash/manifest 腳本。
- `device/`：不可熱修的 immutable launcher，以及 `minimal` / `full` 兩個 OTA channel。

## 目錄

- `device/immutable/`
  - 首次上板必須一起放進裝置 root 的 immutable launcher。
- `device/channels/minimal/files/`
  - 最小可測試 OTA channel，主入口是 `app_minimal.py`。
- `device/channels/full/files/`
  - 完整主系統 channel，主入口是 `app.py`。

## 首次測試

1. 用 `firmware/scripts/flash_firmware.sh` 刷 base firmware。
2. 將 `device/immutable/*` 複製到裝置 root。
3. 將 `device/channels/minimal/files/*` 複製到裝置 root。
4. 開機後在 3 秒 boot window 內按 action button，進入 BLE provisioning。
5. 配網成功後，裝置會以 `minimal` channel 啟動。
6. 如要切到 `full` channel，可透過 `set_channel` UDP command，或直接修改 `.device/runtime_config.json`。

## manifest 與 tag

- `minimal`:
  - `https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.1.0/device/channels/minimal/manifest.json`
- `full`:
  - `https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.1.0/device/channels/full/manifest.json`

更新 manifest：

```bash
python firmware/scripts/generate_manifest.py --repo-root . --channel minimal --version v0.1.0
python firmware/scripts/generate_manifest.py --repo-root . --channel full --version v0.1.0
```

## Base firmware build

`firmware/micropython/UPSTREAM.lock` 記錄了目前測試使用的 MicroPython commit 與 build 參數。

重建時：

```bash
MICROPY_SRC=/path/to/micropython firmware/scripts/build_firmware.sh
```
