# v2.0.F 電導輸出與自動量程改版計劃

狀態：已實作（2026-11-16，含第四階段電導校正），未 commit、未發布、未實機驗證。發布順序：韌體先（wire.json 新增 range TLV）。

## 目標

1. 只焊部分 Rfb 時，自動量程與換算都要正確。
2. v2.0.F 新增**電導輸出（µS）**，取代以「330 mV / 20k」為參考的 virtual 換算。
3. 自動量程（UI 上的「智慧 Vref」）改為雙向：飽和時降 Vref，訊號小時升 Vref 換取解析度。

## 不可破壞的前提

- **原始 ADC mV 模式完全保留**，所有板子的預設都是它：數值路徑、校正曲線、tare、CSV 格式都不變。
- **舊板子（v1.0.F / v1.5.F / GCU 系列）不提供電導模式。** 它們量不到 Rfb，要靠手動填寫，絕對值不可信，所以不做。
- **v2.0.F 開啟自動量程時，必須使用電導輸出。** Vref 會變動，只有換算後的數值才有意義。
- mV 上擬合的校正曲線**只在 mV 模式套用**，不能靜默套到 µS 上。

## 模式總覽

| 板子 | 輸出 | Vref |
|---|---|---|
| v1.0.F / v1.5.F / GCU | 原始 ADC mV（唯一） | 固定 |
| v2.0.F | 原始 ADC mV（預設） | 手動（`set_vref`） |
| v2.0.F | 電導 µS | 手動 |
| v2.0.F | 電導 µS | 自動量程 |

自動量程搭配 mV 輸出的組合不存在，韌體直接拒絕（`range_conductance_required`）。

---

## 第一階段：只焊部分 Rfb 的修復

**問題**

- 沒焊 Rfb 的列，op-amp 沒有回授，輸出貼在電源軌。
- `RangeCompensator::process()` 先標記飽和、再看該列有沒有 Rfb，所以這些列每一幀都被算成飽和。
- 結果：只要開 auto，Vref 每 400 ms 乘 0.8，幾秒內降到 100 mV 並停在那裡。已焊的列全部在最差的解析度下工作。
- 換算模式下，沒有 Rfb 的列直接輸出原始值，數值會隨 Vref 跳動。

**改動（韌體）**

- `process()`：只有 `rfb_[row] > 0` 的列才計入 `frameSatMask_`。
- 換算或電導模式下，沒有 Rfb 的列輸出 0（見「已決定 2」）。
- `rfb_measure` 量到的每列狀態（`ok` / `no_feedback` / `saturated` / `open` / `adc_read_failed`）存進 NVS，並放進 `status.range.rows`，不再只出現在量測當下的回覆裡。

**改動（三端 UI：Desktop `VrefPanel.tsx`、Studio `VrefCards.tsx`、Pocket `DeviceScreen.tsx`）**

- 顯示每一列的 Rfb 狀態，讓使用者對照實際焊接情況。
- 有焊卻量不到（`saturated`，表示 Rfb 太大）時給出明確提示，不要和「沒焊」（`no_feedback`）混在一起。

**測試**

- `tests_native/test_range_math.cpp`：部分列 `rfb = 0` 時，飽和遮罩與 auto 步進都只看已量到的列。

這個階段不改數值單位，可以單獨發布。

---

## 第二階段：電導輸出

**公式**（套在漏電補償之後）

```
G [µS] = (Vout − Vref − offset) / (Vref × Rfb) × 1e6
```

- `signal ≤ 0` 時輸出 0。
- 讀值 ≥ 3000 mV（飽和）時仍輸出算出來的值，但該幀記為飽和（見第三階段的每幀遮罩）。這個值只代表下限。
- Vref 和 Rfb 完全從結果中消去。Rfb 量錯只會造成等比例誤差，不會在 Vref 切換時產生台階；offset 的誤差才會。

**韌體**

- `RangeMath`：新增 `conductanceUs()`，純函式，加 native 測試。
- `set_range`：改為 `output: "mv" | "conductance"` 加 `auto: bool`。舊的 `mode` 參數處理見「已決定 1」。
- `Calibration::apply()`：輸出為 µS 時略過現有的 mV 曲線與 tare，`status.calibration` 顯示「未套用：單位不符」。電導專用的校正不在這次範圍，見第四階段。
- `kPressureActiveThreshold` / `kPressureFullScale`（LED 壓力表、active cell 計數）是 mV 門檻，µS 模式要另一組值。
- App VM：檢查會讀感測值的 app（例如 sysmon、壓力相關 app），是否假設單位是 mV。`opset.mjs` 或 capabilities 要能讓 app 知道目前的單位。

**協定與錄製**

- `status.range` 加 `output_unit`。
- 封包的 flags byte 8 個 bit 都已經用掉，單位資訊放進 `extensions` TLV（`wire.json` 新增欄位 → `gen_wire.py` → 依慣例先推韌體）。
- `.meta.json`：`range` 加 `output_unit`，並列入 `TRACKED`，錄製中途切換會被記錄。
- CSV：主欄位的值照實寫，單位由 meta 說明。可以考慮在欄位名稱或檔頭加註單位。
- Desktop `recording_meta.py`、TS `engine` 的錄製輸出要保持 byte-identical（`packages/engine` 測試）。

**UI（三端同步，選單加套用，不用切換即生效的開關）**

- 原本的「換算」開關改為「輸出：ADC mV / 電導 µS」選單。
- 「智慧 Vref」只有在輸出為電導時才能選；選了智慧 Vref 會連帶把輸出設為電導，套用前清楚提示。
- 即時圖、熱圖、分析工具的軸標與色階要依 `output_unit` 切換。
- i18n 字串：en / ja / zh-CN（Desktop、Studio、Pocket）。

---

## 第三階段：自動量程雙向調整

**決策**

- **下降**：照現狀，有已量到 Rfb 的列飽和就 ×0.8，下限 100 mV。
- **上升**：預測式，不再每步 ×1.1。無按壓時 `Vout − Vref` ∝ Vref，所以
  `V' = V × 目標峰值 / 近期峰值`，目標峰值約為 3000 mV 的 60–70%。
- 快降慢升：上升前要觀察一段時間（例如 5–10 s）的峰值，並保留遲滯區，避免在飽和邊緣振盪。
- **上限 = min(使用者 limit, 750 mV)。** 使用者把 limit 調到 800 以上，只代表手動 `set_vref` 時自負 GPIO45 strap 的風險，不代表同意 auto 長時間停在危險區。
- 開機、重啟、關機前仍一律回到 330 mV，EEPROM 不動（`VrefPolicy` 不變）。

**每幀資料**（`extensions` TLV）

- 當下的 Vref（mV，u16）。
- 飽和列遮罩（u32）。
- 有了這兩項，分析時能精確知道每一幀的量測條件，`.meta.json` 的 Vref 變化紀錄就只是輔助。

**可選：記錄真正的 ADC 原始值**

- 目前 `raw_adc` 欄位其實是換算並濾波後的值，名稱有誤導性。
- 建議讓 `stream_raw_adc` 在 µS 模式下送換算前的 ADC mV。Rfb 或 offset 量錯時，可以事後重新計算。

**風險**

- 600–750 mV 長時間運作的行為，目前只有 `rfb_measure` 的擬合點驗證過線性，需要實機 soak。
- Vref 調高時，突然的重壓可能在迴圈反應前就飽和（每步最快 400 ms），那幾幀資料會遺失。

---

## 第四階段（2026-11-16 已實作）：電導模式的校正

取代第二階段「µS 時略過校正」的做法。模型不變（論文的 p = a + b·√x + c·x），電導模式下 x = G − G₀。

- **校正檔標註單位**：`/cal/meta` 加 `unit=mV|uS`；沒有這行的舊檔一律是 mV，所以別人的實驗不受影響。仍然只有一份校正檔。
- **空的校正檔不算單位不符**（沒有 tare 也沒有 levels 時 `unit_matches` 為 true）；2026-11-16 實機測試後補上。
- **單位不符時只暫停**：`output_mode` 回報 `raw`、不套用，也不刪檔；切回原單位就恢復。`calibration_enable` 回 `calibration_unit_mismatch`。
- **session 以當下單位開始**：已存校正檔是另一種單位時，草稿從空白開始，存檔會取代舊檔。session 期間 `set_range` 的 `mode` / `clear_rfb` 回 `calibration_session_active`；單位若仍改變，擷取與存檔回 `calibration_unit_changed`。
- **單獨歸零**：已存的 levels 是另一種單位時回 `calibration_unit_mismatch`。
- **讀出曲線**：µS 固定用恆等式（c1/c2 只用於 mV）。
- **飽和**：擷取時，有 Rfb 的列 ADC ≥ 3000 mV 的樣本記為 `kSaturatedCaptureUs`（1e9）。µS 的 clip 是 `kConductanceClipUs`（1e6），多數區塊見過飽和就是飽和點（`s`）。擷取是阻塞的，期間 Vref 不會動。
- **沒焊 Rfb 的列**：fit 結果 `no_rfb`、點狀態 `r`、`cells_no_rfb` 計數；不算失敗，`complete` 不計它們。
- **status 欄位**：`unit`、`active_unit`、`unit_matches`、`draft_unit`。錄製 meta 的 `calibration.unit` 也會隨錄製追蹤。
- **UI**：Desktop / Studio 的校正頁顯示單位不符或電導校正提示。Pocket 沒有校正頁。

---

## 已決定（2026-11-16）

1. **舊的 `virtual` / `auto` 直接重設。** 目前只有開發者自己在用，v2.0.F 還沒廣泛使用，不需要遷移提示。
   新韌體開機時讀到舊的 `rng_mode`（virtual 或 auto），一律重設為 raw mV，Vref 回 330 mV，不加 `range_migrated` 之類的提示。`virtual` 模式移除；`set_range mode=virtual` 回 `range_mode_removed`。
2. **電導模式下，沒有 Rfb 的列輸出 0。** 有效列由 `status.range.rows` 和 `.meta.json` 的 `range.rfb_ohm`（已有記錄）判斷。Rfb 表只在 `rfb_measure` 時改變，不需要每幀傳遮罩。
   不用 NaN：Python 的 `json` 會輸出不合法的 `NaN`，瀏覽器的 `JSON.parse` 無法解析。

## 發布順序

依照 `wire.json` 的慣例：韌體先發布，接著 Desktop、Studio / Pocket / headless。每個階段都需要在 v2.0.F 實機驗證後再推到 stable。
