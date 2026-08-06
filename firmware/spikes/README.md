# Phase 0 spikes — New Horizons Direct

These are throwaway validation sketches for the two highest-leverage unknowns
in the "New Horizons Direct" plan (`~/.claude/plans/gcu-v2-3-d-new-horizons-hub-ble-hub-hub-lazy-marshmallow.md`,
section 6 — not part of this repo, so no relative link here). **Nothing past Phase 1 (the `EspNowFrame` protocol layer,
already implemented in `firmware/newhorizons_os/EspNowFrame.{h,cpp}`) should
be built out until these two spikes produce real numbers** — they gate
whether "60fps / 2–4 concurrent devices" is achievable at all, and whether a
no-PSRAM GCU V2.3.D board can run WiFi + TLS WebSocket + ESP-NOW at once.

Both spikes require real ESP32-S3 hardware and a Serial monitor — I (the
agent) cannot run these myself; a plain ESP32-S3 dev board is fine for a
first pass, doesn't need to be the actual GCU V2.3.D board.

## 6.1 — `espnow_throughput_sender` / `espnow_throughput_receiver`

What it measures: real ESP-NOW throughput and fragment/frame loss when
streaming ~1884-byte synthetic frames (the current worst-case
`NHO/Arduino/1` frame size for a GCU V2.3.D board) at 60fps, scaled up to 4
concurrent senders against one receiver.

1. Flash `espnow_throughput_receiver` to one board. Open its Serial monitor
   (115200 baud) and copy the printed MAC address.
2. Paste that MAC into `kReceiverMac` in `espnow_throughput_sender.ino`.
3. Flash the sender to 1–4 boards. Watch the receiver's Serial output — it
   prints per-peer stats every 5s: `approx_fps`, `approx_kbps`, `loss=%`.

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB espnow_throughput_receiver
arduino-cli upload -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB espnow_throughput_receiver
# after editing kReceiverMac:
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB espnow_throughput_sender
arduino-cli upload -p /dev/cu.usbserial-YYYY --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB espnow_throughput_sender
```

**Exit criteria** (from the plan): near-zero `loss=%` with `approx_fps`
close to 60 at 4 concurrent senders. If loss climbs or fps can't keep up as
senders are added, that's the signal to revisit the fps/concurrency targets
with the user before writing any Hub or device product firmware.

### Result (2026-08-05, single link: VD-CTL/R v1.0.F sender ->
### VD-CTL/R v2.3.D GCU LTS receiver, real hardware)

**Does not meet the exit criteria at the default 60fps / 1884B-frame
setting.** Measured on one link (not yet the full 4-concurrent-device
scenario):

| target fps | frags/s | result |
|---|---|---|
| 20 | 160 | clean: `loss=0.0%`, `frames_ok=100/100` per 5s window, ~301 kbit/s sustained |
| 40 | 320 | collapses: 76–91% loss |
| 60 (default) | 480 | collapses: ~59% of `esp_now_send()` calls fail with `ESP_ERR_ESPNOW_NO_MEM` even with per-fragment pacing + retry; **0 complete frames** ever reassembled |

Two mitigations were tried on the sender before concluding this is a real
throughput ceiling, not a fixable bug: (1) retrying individual
`esp_now_send()` calls on `ESP_ERR_ESPNOW_NO_MEM` with a short backoff, (2)
replacing the "fire all 8 fragments in one burst" loop with even pacing
across the frame interval (both changes are kept in
`espnow_throughput_sender.ino`). Neither meaningfully moved the 40/60fps
failure rate, which points to a genuine sustained-throughput limit on this
exact hardware/link (~160-200 fragments/s, i.e. roughly 20-25fps at the
current 8-fragment/1884B frame size) rather than a queue-depth tuning
problem.

**This directly affects the plan's confirmed requirement #2** ("keep
near-current ~60fps data rate, which is why ESP-NOW was chosen over BLE").
At the current worst-case frame size, ESP-NOW does not clear that bar
either on a single link.

### "Direct Stability Mode" candidate (2026-08-05, same hardware)

Per user direction, rather than accepting 60fps as fixed, New Horizons
Direct gets a dedicated low-resource profile ("Direct Stability Mode")
activated when a device is running over ESP-NOW instead of WiFi/UDP.
First candidate tested: drop the raw-ADC block from the frame (matrix +
IMU + mag + battery + HMAC only, 984B instead of 1884B -> 4 ESP-NOW
fragments instead of 8) at a 24fps target, keeping the pacing + retry
logic from the 60fps runs above.

Result: **clean after a brief warm-up window** -- first 5s window showed
6.4% loss (startup transient), then four consecutive 5s windows at
`loss=0.0%`, `approx_fps=24.4`, ~192 kbit/s sustained. This is a real,
working parameter set for a single link.

### 2 concurrent devices, uncoordinated (2026-08-05)

With 2 real sender boards (1 on USB, 1 on battery) both running the 24fps/984B
profile with no coordination between them, one link stayed clean but the
other showed **persistent 2-20% loss** across five 5s windows (9.1%, 10.7%,
17.6%, 21.3%, 2.3%) -- i.e. "clean at 1 device" did not hold at 2 devices.
This matches the plan's concern that airtime on the Hub's one radio is a
shared resource: two independently-timed senders occasionally transmit at
the same moment and collide.

### 2 concurrent devices, time-slot coordinated (2026-08-05)

Per user direction, added simple TDMA-style slotting to
`espnow_throughput_sender.ino`: the frame period is split into `kSlotCount`
slots, and each device confines its fragments to its own `kSlotIndex`-th
slice instead of spreading them across the whole frame period (this is
what the real `EspNowPairing`/Hub would assign dynamically at pairing
time -- see plan section 6a). Re-ran the same 2-device test with
`kSlotCount=2`, one board `kSlotIndex=0` and the other `kSlotIndex=1`.

Result: **clean.** One 5s warm-up window showed 5.6%/6.3% loss (the same
startup transient seen after every fresh boot in earlier runs), then four
consecutive windows at `loss=0.0%` on *both* peers simultaneously, each
~24.2-24.6fps, ~190-193 kbit/s. This directly fixes the uncoordinated-2-device
result above and validates the plan's "Hub-local time-slot coordination"
design (section 6a) as the right lever for intra-Hub collision avoidance.

### 4 concurrent devices, static per-device slot offset (2026-08-05)

Got 4 real boards. With `kSlotCount=4` and each device's slot-window offset
computed relative to its **own free-running clock** (i.e. its own
boot/reset time), results were bad and inconsistent across peers: one peer
trended toward 0% loss over several windows, the other three stayed at a
persistent 63-92% loss. Root cause: 4 boards flashed and reset at
different real-world moments have no common time reference at all, so a
per-device offset computed from "time since MY OWN reset" doesn't actually
land in a consistent real-world time slice relative to the other devices'
offsets -- the slots drift into each other even though each device
individually "obeys" its own assigned slot. (The earlier 2-device test
that looked clean was plausibly luck: 2 boards flashed back-to-back
happened to reset closely enough in time that their clocks stayed loosely
aligned for the duration of that short test.)

### 4 concurrent devices, Hub-broadcast sync beacon (2026-08-05)

Added a 1-byte beacon the Hub broadcasts once per frame period; senders
re-anchor their slot-window countdown to the moment they *receive* it
instead of their own clock. Partial improvement: one peer became mostly
clean, but the other three still showed 36-90% loss. Suspected cause:
802.11 broadcast frames get no MAC-layer ACK/retry (unlike unicast), so
devices can silently miss beacons with no way to notice or recover --
different devices likely have different beacon-miss rates depending on
what they happen to be doing (mid-transmission, etc.) at the moment a
beacon goes out.

### 4 concurrent devices, Hub-driven unicast polling (2026-08-05)

Replaced the beacon entirely with the Hub explicitly polling one
registered device at a time over **unicast** (which does get MAC-layer
ACK+retry): each device sends a one-time `HELLO` to register with the Hub
(now re-sent periodically rather than once, after discovering that
re-flashing/resetting the Hub mid-test -- an artifact of this spike's
read-serial tooling resetting the board via DTR on every read -- wiped its
peer roster and devices never re-announced themselves). Devices are
otherwise fully passive: no local timer at all, they only fragment+send
when a `POLL` addressed to them arrives. This also simplified the sender
firmware to be identical across all boards -- no more per-board slot
config.

Result: **still not clean.** All 4 peers registered correctly this time,
but loss stayed in the 19-81% range across peers and, notably, appeared to
*worsen* over successive 5s windows rather than converge. Suspected root
cause: the Hub advances to the next device on a **fixed wall-clock
schedule** (`kPollBudgetUs` per device) regardless of whether the
currently-polled device has actually finished responding -- if a poll's
own delivery (or the device's response) takes longer than expected for any
reason (retry backoff, radio contention), the Hub moves on to the next
device's poll before the current one is done, and the two devices'
fragments collide. Fixing this properly needs the Hub to advance on
**completion** (a full frame received from the current device, or an
explicit timeout as a fallback) rather than a fixed timer -- a real
protocol design task, not a quick spike tweak.

**Conclusion: reliably coordinating 4 concurrent devices was not achieved
at the spike level after three different approaches** (static per-device
offset, broadcast beacon, fixed-schedule unicast polling). This is real,
useful signal -- "guaranteed stable 24fps at 2-4 concurrent devices" needs
dedicated protocol design (most likely: Hub advances polling on
frame-completion-or-timeout, not a fixed clock) during Phase 2/3's real
implementation, not something to keep hacking at inside throwaway spike
code. The 2-device-with-static-slots result earlier in this doc should be
treated as inconclusive/possibly lucky, not a validated data point.

### PHY rate config (2026-08-06)

All results above were measured at ESP-NOW's **default** PHY rate
(`WIFI_PHY_RATE_1M_L`, 1Mbps 802.11b long preamble) -- Espressif's
maximally-conservative default, not a speed-optimized choice. The
diagnosed bottleneck in the 40/60fps collapse above was airtime/TX-queue
saturation, so a faster PHY rate should directly raise the ceiling. This
was untested until this session: neither `esp_now_set_peer_rate_config()`
nor any related rate-config API had ever been called anywhere in this
repo.

Real hardware (VD-CTL/R v1.0.F sender -> VD-CTL/R v2.3.D GCU LTS receiver,
single link, `espnow_throughput_sender.ino`/`espnow_throughput_receiver.ino`
with `esp_now_set_peer_rate_config()` wired in after `esp_now_add_peer()`,
`esp_wifi_set_protocol()` called defensively beforehand):

| PHY rate | target fps | loss | achieved fps | throughput |
|---|---|---|---|---|
| MCS3 LGI (~26Mbps) | 24 | `loss=0.0%` (6 windows) | 83-87 | ~660-686 kbit/s |
| MCS3 LGI (~26Mbps) | 40 | `loss=0.0%` (6 windows) | 132-137 | ~1039-1083 kbit/s |
| MCS3 LGI (~26Mbps) | 60 | 1.2-4.8% | 180-191 | ~1421-1505 kbit/s |
| MCS5 SGI (~57.8Mbps) | 60 | 0.2-2.8% | 171-204 | ~1347-1602 kbit/s |
| MCS3 LGI (~26Mbps) | 24, ~6m through a wall, sender on battery | 0.4-1.9% | 72-94 | ~570-737 kbit/s |

Note "achieved fps" exceeds "target fps" at every row -- the sender has no
local frame timer at all (see `EspNowPairing`/`espnow_throughput_sender.ino`:
it only fragments+sends on receiving a `POLL`), so with a single registered
peer the Hub's polling loop cycles as fast as reassembly completes, not at
the nominal per-device budget derived from `kTargetFps`. `kTargetFps` still
matters here because it sets `kPollBudgetUs` (the fragment-pacing spread on
both sides) -- a smaller budget means tighter fragment pacing, which is why
achieved fps kept climbing as the target rose.

**Environmental confound caught before trusting any of the above**: the
first attempt showed 2 extra "ghost" peers registering on the receiver with
58-90% loss each, degrading the real sender's numbers too -- traced to
other New Horizons device boards still powered on nearby, still in
`espnow` transport mode from earlier sessions, coincidentally matching this
spike's naive single-byte `HELLO` magic (no device-identity check). Not a
rate-config bug. Resolved by powering off the other boards; all numbers
above are from the clean, single-real-peer environment.

**Conclusion**: MCS3 LGI clears both the real target (24fps -- clean, both
at desk range and at ~6m through a wall on battery) and massively
outperforms the old default-rate ceiling (previously: 40fps collapsed to
76-91% loss, 60fps got ~0 complete frames) at every fps step tested,
including well past the real target. The residual 1-5% loss only shows up
at the extreme ~180-200fps end (target=60, both MCS3 LGI and MCS5 SGI) --
going to a higher MCS didn't clear it, suggesting that residual is a
different (TX-queue-pacing) limit, not further alleviated by raw PHY rate,
and out of scope for this round (see `kSendWindowUs` note below). MCS3 LGI
was chosen over MCS5 SGI for production: no meaningful throughput
advantage at the only regime where MCS3 LGI wasn't already fully clean,
and LGI (long guard interval) trades a small rate ceiling for more margin
against a noisy/marginal link than SGI.

**Wired into production code** (`EspNowPairing.cpp` device side,
`EspNowHubManager.cpp` Hub side, both repos) after this validation,
real-hardware end-to-end tested with the actual HELLO/PAIRED/POLL pairing
flow (not just this spike's point-to-point harness) -- device and Hub paired
cleanly and forwarded frames to a local Desktop Backend without incident.

**Deliberately not touched this round**: `kSendWindowUs`
(`EspNowStreamTransport.cpp`), `kEspNowResponseSendWindowUs`
(`EspNowPairing.cpp`), `kHubPollTimeoutUs` (`EspNowHubManager.h`) -- these
are TX-queue/driver-level pacing constants, not a direct function of PHY
rate, and changing them in the same round as the rate config would make it
impossible to attribute results to either change. Only worth revisiting if
`ESP_ERR_ESPNOW_NO_MEM` reappears at fps levels the product actually
targets (it didn't, at any fps at or below the real 24fps "Direct
Stability Mode" target).

## 6.2 — `hub_memory_budget`

What it measures: `ESP.getFreeHeap()`/`getMaxAllocHeap()` at each stage of
bringing up WiFi STA + a TLS WebSocket client + ESP-NOW with 4 registered
peers concurrently — the combination the real Hub firmware needs, on
hardware with **no PSRAM**.

1. Edit `kWifiSsid`/`kWifiPassword` and `kWsHost`/`kWsPort`/`kWsPath`/`kUseTls`
   at the top of `hub_memory_budget.ino` (defaults point at the production
   `wss://` endpoint from `config_store.py`; point at a local
   `./scripts/start_local.sh --build` backend with `kUseTls=false` for a
   TLS-free baseline first).
2. Requires the `WebSockets` library (Markus Sattler /
   Links2004/arduinoWebSockets): `arduino-cli lib install WebSockets`.
3. Flash and watch Serial — it logs a `[heap] stage=... free=... max_alloc=...`
   line at `boot`, `wifi_connected`, `espnow_ready_4peers`, `ws_begin_called`,
   `ws_connected`, and once per second in steady state.

**Exit criteria**: compare the steady-state `free`/`max_alloc` numbers
against `newhorizons_os.ino`'s own `ESP.getFreeHeap() < 30000` danger
threshold. If steady-state free heap after `ws_connected` is anywhere near
that line (or `max_alloc` is much smaller than `free`, indicating
fragmentation), the Hub firmware plan needs to revisit its library choice
(`esp_websocket_client` vs `WebSocketsClient` — the plan explicitly leaves
this open pending this spike) or its buffering assumptions before Phase 2
starts.

### Result (2026-08-05, real GCU V2.3.D hardware, production wss:// endpoint)

**Clean -- comfortable headroom, no library change needed.** Using the
`WebSockets` (Links2004/arduinoWebSockets) library against the real
production `wss://isensing-s1.u-aizu.ac.jp/newhorizons/gateway/ws`
endpoint on CNLab-wifi:

| stage | free heap | max_alloc |
|---|---|---|
| boot | 273,904 | 249,844 |
| wifi_connected | 213,132 | 200,692 |
| espnow_ready_4peers | 212,760 | 200,692 (adding 4 ESP-NOW peers cost ~370B -- negligible) |
| ws_begin_called | 212,624 | 200,692 |
| **ws_connected (steady state)** | **163,968** | **147,444** |

The TLS handshake itself cost ~51KB of heap (212,624 -> 161,356 during
connect), which is the single biggest chunk in the whole boot sequence --
but steady-state free heap (163,968) is still **>5x** the `ESP.getFreeHeap()
< 30000` danger threshold from `newhorizons_os.ino`, and `max_alloc` tracks
`free` closely (~90%), indicating no significant fragmentation. The
connection received 2 text frames from the backend and stayed up
throughout the capture window (no disconnect/reconnect observed) even
without a pre-registered gateway_id/auth token.

**Conclusion: the plan's biggest flagged memory risk (WiFi + TLS
WebSocket + ESP-NOW-with-4-peers concurrently, no PSRAM) is not actually a
problem on real GCU V2.3.D hardware.** No need to fall back to
`esp_websocket_client` for memory reasons; `WebSocketsClient` has enough
headroom.

## Notes

- `EspNowFrame.h`/`EspNowFrame.cpp` in each spike folder are copies of
  `firmware/newhorizons_os/EspNowFrame.{h,cpp}` — duplicated so each spike
  builds as a standalone Arduino sketch. If the shared module changes,
  re-copy before re-running a spike.
- Both spikes hardcode ESP-NOW channel 6; keep sender/receiver in sync if
  you change it.
- Neither spike touches `firmware/newhorizons_os/` — the existing
  WiFi/UDP + Gateway path used by lab devices is completely unaffected.
