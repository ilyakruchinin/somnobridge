# SomnoBridge → SomnoTrace transfer speed: analysis and recommendations

Status: analysis only (2026-10-06). Target: **≥100 KB/s effective** for
SomnoTrace (ESP32-S3, central) pulling AirSense DATALOG files from the
EZShare card through SomnoBridge (ESP32-S3-GEEK). Constraints stay as they
are: SomnoBridge's 5 s card poll is not disturbed, the card is read-only,
no reboots, and every file is SHA-256 / byte-count / source-tuple verified.
**SomnoTrace never leaves its home WiFi.** Any channel accommodation for
an ESP-NOW data plane is made on the SomnoBridge/EZShare side (P3).

## 1. Where we are (measured)

Test rig: SomnoBridge ↔ Linux host (Intel AX200, BlueZ, `ble_query.py`).
This is **not** the production central. See §2.6 for why that matters.

| Configuration | 1.24 MB BRP file | Notes |
|---|---|---|
| Baseline (default PHY/interval, MTU 247) | 19.3 KiB/s | month sweep: 95 files / 8.0 MB in ~35 min wall |
| + 2M PHY, DLE request, 7.5–30 ms (central picked 30 ms) | **46.3 KiB/s** | `phy=2/2 itvl=24` |
| + interval capped at 15 ms | **51.6 KiB/s** | `itvl=12` |
| Same settings, two later runs | 19.9 / 20.1 KiB/s | `retry_ms` 51.6 s / 47.3 s of ~60 s; `stall_ms=0` |

What the device-side counters show (`stream done ... stall_ms retry_ms`):

- `stall_ms=0`: the device never waited for host ACK credit. **Our
  application flow control is not the bottleneck.**
- `retry_ms` ≈ 80–85 % of stream time in the slow runs: the NimBLE notify
  pool was full and the sender waited for the controller to put frames on
  air. **Air time is the bottleneck.** The NOTIFY_TX wakeup did not help
  (20.1 vs 19.9 KiB/s). The sender is not slow to react. The controller
  just is not draining.
- The 2.5× variance at identical negotiated parameters is **not explained
  yet**. Leading suspects: (a) WiFi/BLE coexistence on SomnoBridge (the
  same task reads HTTP from EZShare over WiFi while it notifies over BLE,
  and both share one 2.4 GHz radio), and (b) the host AX200 is itself a
  WiFi+BT combo chip carrying the host's own WiFi traffic. Fewer packets
  per connection event (central-controlled) would produce exactly this
  signature.

Reference ceiling: Espressif's IDF 5.5.1 `throughput_app` example (two
ESP32s, MTU 512, 7.5 ms interval, 7.5 ms CE length) reports **NOTIFY
~340 kbps, WRITE ~500 kbps (~42–62 KB/s)**. On 2M PHY with proper CE
length, ESP-to-ESP should do better, but **raw BLE GATT on ESP32 is not
a reliable path to 100 KB/s by itself.** Reaching the target needs
compression on top of BLE, or an ESP-NOW data plane (§3, P3).

Per-file fixed overhead (independent of link speed):

| Cost | Where | Size |
|---|---|---|
| Fixed 2.5 s settle after every download | `ble_ops.c` post-check: `vTaskDelay(2500)` before re-checking the source tuple | 95 files × 2.5 s ≈ **4 min** of the 35 min month sweep |
| `STALE` on first file of each day | `op_get_file`: listing >90 s old → rescan + refuse; client sleeps 12 s | 6 days × ~12 s |
| New HTTP connection per file | `ezs_download` → `open_get` / `esp_http_client_cleanup` per file | TCP+HTTP setup each time (one or two RTTs to EZShare) |
| New BLE client session per file (test rig only) | `ble_query.py` process per `--get` | D-Bus discovery, notify subscribe |

An 832-byte file takes ~3.0 s end to end, almost all of it the 2.5 s
settle.

## 2. Analysis by layer

### 2.1 Physical / link layer (BLE)

| Lever | State | Assessment |
|---|---|---|
| PHY | **2M negotiated** (experimental change, see §5) | Biggest single gain: 19→46 KiB/s. Keep. |
| Data Length Extension (251 B LL payload) | Requested via `ble_gap_set_data_len(251, 2120)`; **outcome not logged** | Must confirm. Without DLE, each 247-byte ATT PDU splits into ~10 LL fragments. |
| ATT MTU | 247 | Correct choice: 247 + 4 B L2CAP = 251 B, exactly one DLE PDU. Going to 517 adds LL fragmentation and **no** air-time gain. Only per-PDU host overhead drops slightly. |
| Connection interval | Central-chosen; 15 ms when we ask for 7.5–15 ms | What matters is **packets per event**, set by the central's CE length. SomnoTrace, as central, controls `min_ce_len`/`max_ce_len` in `ble_gap_connect`. |
| CE length / more-data | `CONFIG_BT_CTRL_CE_LENGTH_TYPE_ORIG` on the bridge | At 2M PHY a 251 B PDU + ACK takes ~1.4 ms, so a 15 ms event could carry ~10 PDUs. Observed best case is ~3.2 PDUs/event (51.6 KiB/s ÷ 244 B ÷ 66.7 events/s). The event is being cut short: central CE limit, coex, or controller buffers. |
| Notify vs indicate | Data channel = notify (good). Control = indicate (fine, low volume). | No change. |
| TX power | +9 dBm default | Fine. |
| Coexistence | SW coex, balanced; WiFi download runs **concurrently** with BLE notify | Most likely cause of the 20 vs 50 KiB/s variance. See §2.4. |
| Bridge buffers | MSYS_1 32×256 B (~16 in-flight notifies), internal-RAM only | Enough for ~1.5 full 2M events. Tight, but internal RAM was the binding constraint (the NimBLE host task failure). Revisit after §3.3 frees RAM. |
| CPU | Bridge 160 MHz; SomnoTrace 240 MHz | Raise the bridge to 240 MHz once compression lands (§2.3). |

Production central (SomnoTrace) specifics:

- It already holds up to **two other BLE links** (AS11, oximeter;
  `NIMBLE_MAX_CONNECTIONS=2` today, so it would need 3). Every link's
  connection events take radio time from the bridge link.
- It runs a WiFi STA (home network, SMB/SleepHQ uploads) under SW coex.
- So the production link will be **no better** than a clean ESP-to-ESP
  benchmark unless SomnoTrace schedules a transfer window: oximeter/AS11
  idle, WiFi uploads paused.

### 2.2 Source layer (EZShare HTTP)

- **Raw EZShare download throughput has never been measured** on the
  bridge. Once the radio link gets fast, this becomes the ceiling for any
  uncached transfer. EZShare cards are slow (SD behind a tiny HTTP
  server), and their real rate could be well under the WiFi ceiling.
  **Measure first (M1).**
- `ezs_download` reads 4 KiB chunks into a stack buffer and calls the
  BLE sink synchronously. A slow BLE sink therefore stalls TCP reads, and
  the TCP receive window (lwIP `TCP_WND` 5760 B) throttles the card. The
  WiFi download is paced by BLE and holds the radio for the whole
  transfer.
- Each file opens a new HTTP connection. `keep_alive_enable=true` is set,
  but the handle is cleaned up per file, so reuse never happens.
- HTTP Range support on EZShare is unknown. It decides whether resume
  (§3.7) can go to the card or must be served from the cache.

### 2.3 File / content layer (compression)

Measured on the actual month set (95 files, 8,011,138 B). Ratio is
raw ÷ compressed:

| Method | BRP (87 % of bytes) | PLD | SA2 | CSL/EVE | **All** |
|---|---|---|---|---|---|
| deflate -1 | 1.79× | 4.53× | 43× | ~3.9× | **1.97×** |
| deflate -6 | 1.85× | 5.49× | 55× | ~4.1× | 2.05× |
| deflate -9 | 1.85× | 5.51× | 58× | ~4.2× | 2.05× |
| deflate -9, 4 KiB window | 1.72× | 5.38× | 60× | ~4.3× | 1.90× |
| **16-bit delta + deflate -9** | **2.42×** | 6.23× | 57× | ~3.9× | **2.66×** |
| 16-bit delta + deflate, 4 KiB window | 2.28× | 6.09× | 58× | ~4.0× | 2.51× |
| delta + byte-plane split + deflate | 2.45× | 5.47× | 58× | ~3.8× | 2.68× |
| xz -9e (reference only, too heavy for MCU) | 2.58× | 7.43× | 57× | ~3.4× | 2.85× |

Findings:

- **Plain deflate gives 2×. A sample-delta pre-filter gives 2.66×.**
  EDF data records are 16-bit little-endian samples, and smooth waveforms
  (flow, pressure) delta-code well. The filter is a few lines of C,
  costs O(n), and is lossless.
- Compression level barely matters above -1 (1.97× vs 2.05×). The delta
  filter matters much more than the level. Use a **low level plus
  delta**, which is cheap on the MCU.
- A 4 KiB window loses only ~6 %, so a small-RAM compressor is viable if
  the 32 KiB ROM deflate's memory is a problem.
- A per-signal delta (EDF records hold contiguous per-signal blocks) will
  likely beat the naive whole-body delta, which runs across signal
  boundaries. Worth a short experiment.
- Availability: **ESP32-S3 ROM has both `tdefl_*` (deflate) and
  `tinfl_*` (inflate)** (`esp32s3.rom.ld`). No new dependency and no
  flash cost.
  - ROM `tdefl` (built with `TDEFL_LESS_MEMORY=1`) needs ~165 KB of
    state: 32 KiB dict, 64 KiB `m_next`, 24 KiB code buffer, 31 KiB out
    buffer, plus tables. Put it in PSRAM.
  - `tinfl` on SomnoTrace needs a 32 KiB dictionary window. SomnoTrace
    has octal PSRAM.
- On-device speed is unknown. Host deflate -1 ran at 64 MB/s, so an S3
  at 240 MHz from PSRAM should still be in the MB/s range at level 1,
  far above link speed. **Benchmark (M3).**
- Integrity: keep SHA-256 over the **raw** bytes. SomnoTrace inflates,
  hashes, and compares, so end-to-end verification semantics do not
  change.
- At a 50 KiB/s link, **2.66× → ~130 KiB/s effective**.

### 2.4 Caching / buffering (bridge)

- The bridge has **2 MB PSRAM** (S3R2). The catalog budget is 768 KiB
  (`CAT_PS_BUDGET`), leaving roughly 1 MB usable for a file cache after
  heap needs.
  - The largest observed file is 1.53 MB raw. A long uninterrupted night
    could produce a 2–3 MB BRP.
  - Raw caching in PSRAM is therefore not enough for whole files.
    **Compressed** caching (~2.4× on BRP) covers ~2.4 MB raw.
- **Flash has room**: 16 MB total; app 4 MB + NVS 3 MB + small partitions
  ≈ 7.1 MB used, so ~8.9 MB is free. A dedicated `cache` partition of
  4–8 MB holds a compressed month (~3 MB) with margin.
  - Write speed (~100–300 KB/s) and wear are fine for a nightly workload.
  - Caveat: on S3R2, flash and PSRAM share the SPI bus. Flash
    erase/write stalls PSRAM and cache. Schedule cache writes at low
    priority, never during timing-critical capture.
- **Cache-then-send removes WiFi/BLE contention during the BLE burst.**
  Download from EZShare at full WiFi speed into PSRAM/flash, then stream
  over BLE with the radio otherwise idle. Only the scheduler's 5 s
  listing poll (~200–250 ms per poll, ~5 % duty) still touches WiFi.
  This is the most direct test of the coex hypothesis (M4).
- **Proactive prefetch**: when therapy goes IN→OUT, the bridge already
  rescans the day. It can then pull the night's new files into the
  compressed cache in the background, as low-priority scheduler jobs that
  respect the poll cadence. When SomnoTrace asks, the transfer is
  cache→radio only, and EZShare speed drops off the critical path.
  - This reads each file once more than today. It is still read-only
    and still one-shot per file.
  - Re-validate the source tuple (size + stamp) at serve time so the
    cache cannot serve a stale file.

### 2.5 Protocol / latency layer

- The credit window is not limiting (`stall_ms=0`). The host window was
  raised to 128 frames experimentally, which is harmless.
- Our 16-byte frame header per 244-byte ATT payload is ~6.6 % overhead.
  ATT notify adds 3 B more. Over a large SDU this amortizes to ~0 %.
- **L2CAP CoC** (`CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM=0` today; NimBLE
  and IDF example `ble_l2cap_coc` support it):
  - Credit-based flow control is built in, replacing our app-level ACKs.
  - It removes the ATT and per-PDU app headers.
  - SDUs can be 4–64 KiB, with one header per SDU.
  - Expected gain is ~7–10 % plus simpler code. It does **not** fix
    air-time starvation. Medium priority.
- **Per-file round trips** dominate small files. A batch operation
  (`GET_FILES` with a manifest, or a pipelined queue of `GET_FILE`
  requests) lets the bridge stream file N+1 right after N without a
  request round trip.
- **Post-download verification** waits a fixed 2.5 s, then reads the
  catalog. Replace the fixed sleep with an event (wake when the
  submitted `JOB_DAY` completes), or verify a whole batch with one day
  rescan at the end. The day listing is one HTTP GET (~250 ms) either
  way.
- **`STALE` refusal** costs ~12 s per day boundary. Options: block the
  request until the queued rescan lands (bounded ~10 s, no client retry
  dance), or pre-refresh every day in a batch manifest up front with one
  scheduler pass.

### 2.6 Measurement rig

- The Linux/AX200 host shares its radio with host WiFi, has opaque CE
  scheduling, and cost us one hard adapter wedge. **Numbers from it are
  noisy lower bounds.**
- Production is ESP-to-ESP. Every decision in §3 should be validated on
  a second ESP32-S3 acting as central:
  - start from IDF `throughput_app/blecent_throughput`;
  - set 2M PHY, 7.5 ms interval, and CE length = interval;
  - run with and without SomnoTrace-like background load (2 extra BLE
    links + WiFi STA traffic).

## 3. Recommendations (prioritized)

### P0: Measure before building (each is small; they decide the architecture)

| ID | Measurement | Why |
|---|---|---|
| M1 | Bridge-side EZShare raw throughput: `ezs_download` into a null sink for 1.2 MB, BLE connected but idle. Also repeat with BLE streaming. | Ceiling for uncached transfers. If EZShare does <150 KB/s, prefetch/caching (§2.4) is mandatory for any data plane. |
| M2 | ESP-to-ESP BLE ceiling: two S3s, `throughput_app`, 2M PHY, 7.5 ms, CE = interval, MTU 247 vs 517, notify. | Tells us if BLE alone can approach 100 KB/s raw (Espressif's 1M example: ~42 KB/s). |
| M3 | ROM `tdefl` speed on S3 at 160/240 MHz, levels 1/3/6, PSRAM state, plus `tinfl` speed. | Confirms compression is not CPU-bound. |
| M4 | Coex attribution: stream the same file from a PSRAM copy (WiFi idle) vs live HTTP. Log `retry_ms` and PDUs/event. | Confirms or kills the coex hypothesis behind the 20↔50 KiB/s variance. |
| M5 | ESP-NOW v2 throughput between two S3s on one channel: 1470 B frames, unicast, rates 1M / 11M / 24M / MCS3 / MCS7 via `esp_now_set_peer_rate_config`, with and without a concurrent STA connection. | Sizes the ESP-NOW data plane (P3). |
| M6 | Log negotiated DLE (`BLE_GAP_EVENT_DATA_LEN_CHG` or controller read) next to PHY/interval. | Rules out a silent DLE failure. |
| M7 | **EZShare channel-change bench test on a spare card in a USB reader** (same firmware as the patient card if possible): capture `/config?vtype=0` HTML and the form submit; change the channel over HTTP; diff the SD contents before and after (`ezshare.cfg`, FAT); check whether the AP restarts immediately or needs a power cycle; measure time to apply; confirm the card reads back the new channel; repeat 20× for robustness. | Go/no-go for Option A1 and its card-write risk. |
| M8 | Bridge hop cost: disconnect → `esp_wifi_set_channel(H)` → ESP-NOW burst → back to E → reassociate with EZShare, with DHCP vs static IP and a directed BSSID/channel connect. Measure the poll gap. | Sizes Option A2 window length and poll-gap impact. |

### P1: Remove fixed overhead (cheap, link-independent, certain gains)

1. **Drop the 2.5 s post-download settle.** Make post-verification
   event-driven, or do it once per batch. Saves ~2.5 s/file (~4 min per
   month).
2. **Batch / pipelined transfers.** One request lists N files, the
   bridge streams back to back, and each file keeps its own STREAM_END
   verdict and SHA. Pre-refresh all involved days up front, which also
   eliminates `STALE` round trips.
3. **Reuse the HTTP connection** across files in a batch (keep the
   `esp_http_client` handle; EZShare keep-alive behavior to be verified).
4. **Keep 2M PHY + DLE + short interval requests** (already in the
   working tree, §5). On SomnoTrace as central, set `itvl` 7.5–15 ms,
   `min_ce_len`/`max_ce_len` ≈ interval, 2M preferred PHY, and DLE 251.
5. SomnoTrace: open a **transfer window**. Pause oximeter/AS11 polling
   and WiFi uploads while pulling from the bridge, so the bridge link
   gets the radio.

### P1: Decouple WiFi and BLE on the bridge

6. **Cache-then-send.** Fetch the whole file (compressed, §P2) into PSRAM
   first, then stream. Spill to a flash `cache` partition for files
   above the PSRAM budget. Expected effect: the BLE burst gets the radio
   alone, which addresses the 2.5× variance (pending M4).
7. Raise bridge CPU to 240 MHz once compression is on.

### P2: Compression (≈2–2.7× effective)

8. **16-bit delta pre-filter + raw deflate (ROM `tdefl`, level 1–3)**,
   applied as the file enters the cache:
   - Negotiated per transfer (`"encoding":"delta16+deflate"` in
     STREAM_BEGIN), with plain fallback.
   - The SHA-256 covers the raw bytes. STREAM_END reports raw and
     encoded byte counts.
   - Skip compression when it does not pay (tiny CSL/EVE files go out
     raw or batched).
   - Expected month set: 8.0 MB → ~3.0 MB on the air.
9. Try a per-signal delta (EDF record layout aware) for a few more
   percent on BRP/PLD.

### P2: Do less work

10. **Incremental sync is the real long-term win.**
    - Completed-session EDF files are immutable. SomnoTrace already
      keeps download history, so skip files whose (name, size, stamp)
      are known.
    - A month backfill happens once. The nightly delta is one night
      (~1.3–2.5 MB on this data), so seconds at the target speed.
11. **Resume**: add an `offset` to GET_FILE so an interrupted large file
    resumes. Serve it from cache, or via HTTP Range if EZShare supports
    it (test).
12. Decide which signal files SomnoTrace actually needs. If, for
    example, SA2/PLD are not required for its EDF generation, skipping
    them saves ~12 % of bytes. (Product decision.)
13. **Proactive prefetch on IN→OUT** into the compressed flash cache
    (§2.4). It takes EZShare speed off the critical path entirely.

### P3: Protocol refinements (BLE)

14. L2CAP CoC data channel (large SDUs, built-in credits, no ATT or
    per-PDU app header). ~7–10 %, plus simpler flow control.
15. Revisit the MSYS_1 pool (32 → 48/64) after caching and AP-mode
    changes free internal RAM. Watch `lrg_i` (the nimble_host stack + SM
    crypto failure mode documented in AGENTS.md).

### P3 (strategic): Replace BLE as the bulk data plane

Both ends are ESP32-S3s with WiFi, so the cleanest path to **far above
100 KB/s** is: **BLE as control plane** (pairing, auth, catalog, timing
metadata, key exchange) and **WiFi as data plane**.

**Design rule (decided): SomnoTrace never leaves the home network.**
Any channel accommodation happens on the SomnoBridge/EZShare side.

#### The channel constraint

ESP-NOW only works between radios on the same WiFi channel. A connected
STA can send and receive ESP-NOW only on its AP's current channel.

- SomnoTrace's STA sits on the home AP's channel. Call it **H**.
- The bridge's STA sits on the EZShare card's AP channel. Call it **E**.
  The card's default is CH11.

When H ≠ E, there are two ways to get the bridge onto H without moving
SomnoTrace.

#### Option A1 (preferred): align EZShare's channel to SomnoTrace's channel

**Feasibility: yes, the card's channel is configurable**, and the bridge
can drive it over HTTP. Sources: official ez Share manuals, FCC user
guides, hacker notes.

- Admin page: `http://ezshare.card/config?vtype=0` (also reachable at
  192.168.4.1). Default admin password `admin`.
- Settable fields: SSID, WiFi password, admin password, **WiFi channel
  1–13** (default CH11).
- Admin flow **captured live** (help.js + config.js from the card):
  1. `GET /publicdir/sendurl?client=1&password=<admin>` — marks the
     client IP as admin; `<clientLogin>1</clientLogin>` ok, `-2` means
     another admin session is online.
  2. `GET /config?password=<admin>&back_href=&vtype=1` — the browser's
     login-form navigation (method=get drops the action query and sends
     the form fields); arms the write session and returns the config
     page (hidden inputs `oldAuth`/`oldNick`/`oldPass`/`oldChannel`
     echo the current values — send them back unchanged).
  3. `GET /config?auth=<admin>&ssid=<ssid>&pass=<wifi>&channel=<n>`
     — stages the config; replies `config_success` **but applies
     nothing** by itself.
  4. `GET /config?reset=1` — commits; the card reboots its AP (clients
     drop; the AP is back on air in ~10–15 s).
  Implemented as `ezs_admin_change_wifi_pass()` → BLE op
  `EZSHARE_ADMIN` (verified end-to-end against the live card).

How it would work, without the user doing anything:

1. Over the bonded BLE link, SomnoTrace reports its current STA channel
   H and its WiFi MAC. The bridge stores H.
2. If H ≠ E, therapy is OUT, and transfers are allowed, the bridge logs
   in to the card admin form and sets channel = H.
3. The card's AP comes back on H. The bridge STA reconnects (netw does
   not pin a channel, so the normal scan finds it) and the scheduler
   resumes.
4. From then on, the bridge stays connected to EZShare **and** can do
   ESP-NOW with SomnoTrace at the same time:
   - no disconnects;
   - the 5 s poll continues during transfers;
   - no per-transfer cost.
5. If the home AP later moves channel (router auto-channel), SomnoTrace
   reports the new H and the bridge re-aligns. Rate-limit this
   (e.g. ≤1 per day, therapy OUT only). Use Option A2 for transfers that
   cannot wait.

Risks and open questions (M7 must answer them on a **spare card in a
USB reader**, never on the patient's card first):

| Risk | Detail | Mitigation |
|---|---|---|
| **The card writes to the SD card** | EZShare persists its settings to `ezshare.cfg` in the SD root. Deleting that file is the documented factory reset. A channel change is therefore a write by the card controller to a FAT volume the **AirSense owns and caches concurrently**. This breaks SomnoBridge's read-only principle and carries a small FAT-corruption risk. | Explicit user opt-in. Write only with therapy OUT and the AirSense idle. Rare by design: once at setup, then only when H changes. Read back the config to verify. Never retry in a loop. Check on the spare card exactly what is written and when (immediately vs at power-up). |
| The new channel may only apply after a card power cycle | Some firmware applies settings at boot. The bridge cannot power-cycle the AirSense's card slot. | M7 determines the behavior. If a power cycle is needed, the change lands at the next AirSense power-up, and Option A2 covers the gap. |
| The card AP restarts | A gap in polls and timing evidence while the AP comes back and the bridge reassociates (expected: seconds) | Do it only with therapy OUT. Journal the gap like other reacquisitions. |
| Non-default admin password | If the user changed it, the bridge needs it. | Add an optional "EZShare admin password" field to the setup portal, stored in encrypted NVS with the other card credentials. |
| Unknown card / firmware variants | Form layout may differ between firmware versions (e.g. 4.4.0 2014 vs newer) | Feature-detect. Abort cleanly and fall back to A2 if the form is not recognized. |
| Regulatory channels 12–13 | The ESP32 country setting must allow H | Set the WiFi country to match. Treat H ∉ allowed set as "use A2". |
| Shared airtime | EZShare AP, home AP and ESP-NOW all on H | Normal same-channel sharing. ESP-NOW bursts are short. |

#### Option A2 (fallback, no card writes): the bridge hops to SomnoTrace's channel

This is the user-suggested "bridge temporarily disconnects from EZShare"
mode:

1. **Prefetch first.** While on EZShare, the bridge pulls the requested
   (or the night's new) files into the compressed cache (§2.4). This
   makes cache-then-send (P1-6) mandatory for A2.
2. Transfer window, therapy OUT only:
   - pause the scheduler;
   - `esp_wifi_disconnect`, then `esp_wifi_set_channel(H)`;
   - stream from the cache over ESP-NOW;
   - restore channel E and reconnect to EZShare;
   - run an immediate day rescan, then resume the scheduler.
3. Bound each window, e.g. ≤20–30 s. Between windows, return to the card
   for at least one poll cycle so therapy-start detection is never late
   by more than one window.
4. Speed up reassociation: use a static IP for the bridge on the card's
   /24 (no DHCP) and remember the BSSID and channel E for a directed,
   single-channel reconnect.

Cost: a poll gap of window length + reconnect (expected ~1–3 s; M8).

- Nightly delta: ~2 MB raw → ~0.8 MB compressed → a few seconds of
  airtime at a few hundred KB/s, so one short window.
- Month backfill: ~3 MB compressed, so a few windows.
- No writes to the card, no dependence on EZShare firmware details.

#### Common to A1 and A2

- **BLE stays the control plane:**
  - pairing and authorization;
  - catalog and timing metadata;
  - transfer requests, verdicts, and SHA-256;
  - exchange of the ESP-NOW peer MAC, channel H, and **PMK/LMK keys**.
  ESP-NOW unicast is then CCMP-encrypted for an authenticated peer only.
- **ESP-NOW v2** (IDF 5.5.1: `ESP_NOW_MAX_DATA_LEN_V2` = 1470 B):
  - Raise the PHY rate with `esp_now_set_peer_rate_config`. The default
    1 Mbps rate is no faster than BLE 2M.
  - Unicast has MAC-level ACK and retry. Ordering, windowing, and resume
    reuse our existing offset/credit/SHA framing over 1470 B frames.
- With BLE idle during the ESP-NOW burst, there is no BT/WiFi coex
  contention on either board.
- SomnoTrace stays associated to the home AP throughout. Its own WiFi
  traffic (uploads) shares airtime on H; pausing uploads during a
  transfer is optional.

#### Options considered and rejected

| Option | Why rejected |
|---|---|
| SomnoTrace hops to EZShare's channel / joins a bridge SoftAP / joins EZShare directly | Violates the "SomnoTrace never leaves home WiFi" rule. Direct EZShare access also puts a second HTTP client on the card and threatens the bridge's 5 s polls. |
| Bridge SoftAP + TCP on channel H | The bridge's single radio would need its STA (E) and AP (H) on different channels, which the ESP32 cannot do. It also needs scarce internal RAM for AP clients. |
| Wired UART/USB | Only viable if the boards are co-located. Changes the product. |

**Recommendation:** ESP-NOW data plane plus BLE control plane, served
from the bridge's compressed cache:

- **A1 (EZShare channel alignment)** as the steady state, once M7 shows
  the card-side config change is safe and you accept the one-off
  `ezshare.cfg` write.
- **A2 (bridge hop)** as the always-available fallback, and as the
  default until A1 is validated.
- M5 sizes the gain. It is likely to clear 100 KB/s by a wide margin.

## 4. Expected outcome (month set: 95 files, 8.0 MB)

| Stage | Effective rate | Month sweep | Nightly (~2 MB) |
|---|---|---|---|
| Today (baseline) | ~19 KiB/s large files; ~3.8 KiB/s whole-sweep | ~35 min | ~3–4 min (measured: 15 files / 2.19 MB in 194 s) |
| P1 overhead fixes only | ~20 KiB/s | ~7 min | ~1.7 min |
| + 2M PHY/DLE/short interval (P1) | ~45–50 KiB/s | ~2.7 min | ~40 s |
| + delta+deflate (P2), 2.66× | **~120–135 KiB/s** | **~1 min** | **~15 s** |
| ESP-NOW data plane (P3-A1, or A2 incl. hop cost), cached + compressed | link-bound, likely ≫100 KB/s (M5) | ~10–30 s (A2: + a few reconnects) | a few s (A2: + one ~1–3 s poll gap) |
| + incremental sync (P2-10) | only new data moves | one-time backfill | as above |

The BLE-only path reaches the 100 KB/s target **only with compression**,
and only if the production central sustains ~45–50 KiB/s raw while it
also holds AS11/oximeter links and WiFi (M2 under load). The ESP-NOW
data plane is the path with real headroom. With A1 (EZShare aligned to
SomnoTrace's channel), it costs neither device its network connection.

## 5. Experimental changes currently in the working tree (uncommitted)

These came from the speed-tuning attempt before this analysis. They are
flashed and verified to work (SHA OK, no reboots), but not yet committed
or documented elsewhere:

- `ble_svc.c`, on connect:
  - `ble_gap_set_data_len(251, 2120)`;
  - preferred 2M PHY;
  - conn-param update request 7.5–15 ms, latency 0, 4 s supervision.
- `ble_svc.c`: CONN_UPDATE / PHY_UPDATE logging. NOTIFY_TX wakes
  `ble_stream` (measured no gain; harmless, keeps the 15 ms bound).
- `ble_svc_diag_state()` extended with interval/PHY. The `diag` line now
  prints `itvl=… phy=tx/rx`.
- `ble_ops.c`:
  - `stall_ms` / `retry_ms` counters in `stream done`;
  - the send-retry sleep is now a notify-wait (≤15 ms).
- `ble_query.py`: ACK window 32 → 128 frames.

Keep the PHY/DLE/interval requests and the counters. The NOTIFY_TX wake
can stay or go.

---

## Appendix A — independent validation, BLE-first plan, and seamless-operation limits

This appendix reviews the preceding analysis against the current source,
ESP-IDF **v5.5.1**, and the NimBLE revision pinned by that release
(`b45dcedcafb7888174c3567002c36b342ec0b723`). Section A.9 additionally
assesses released v5.5.5, v6.0.3, and v6.1 upgrade opportunities. It
qualifies earlier claims without changing the main document. No firmware
changes, flashing, card configuration,
or new radio benchmarks were performed for this review. Additional host
compression tests were read-only; only aggregate results are retained here.

### A.1 Verdict

**Agree with the objective and most of the proposed building blocks, but
not with the conclusion that BLE must be replaced.** We have not measured
the ceiling of the actual two-ESP32-S3 link, nor isolated the present
software/queue bottlenecks. Start with optimized, compressed BLE GATT;
retain ESP-NOW as an optional accelerator, not the default architecture.

- **100 KiB/s raw BLE application throughput is physically plausible**
  with 2M PHY, DLE, sufficiently long events, and continuously supplied
  buffers. Whether these particular boards can sustain it while serving
  all their other work is unproven.
- **100 KiB/s effective original-file throughput is a more accessible
  target.** The new compression measurements in A.3 require approximately
  39–44 KiB/s encoded throughput before setup/source-validation costs.
  The earlier 46–52 KiB/s tests make that credible, but not demonstrated
  end to end on SomnoTrace.
- **BLE-only is the simplest seamless option:** no network credentials
  shared, no channel coordination, no EZShare settings changed, and no
  intentional loss of card connectivity.
- **No single radio operates on two WiFi channels simultaneously.**
  Long disconnect/hop windows interrupt card observation. Newer IDF
  off-channel APIs may instead preserve association and fit short bursts
  between required polls (A.9); that possibility needs measurement, not
  an assumption of uninterrupted RF access. BLE remains the simplest
  universal option; validated same-channel ESP-NOW is another.

### A.2 Corrections and qualifications to the preceding sections

1. **The IDF throughput README is a sample, not a ceiling.** [R1] gives
   approximately 340/500 kbps results but does not specify an ESP32-S3
   2M-PHY test configuration, RF environment, encryption, or background
   load. It cannot establish that 100 KB/s raw is impossible on our
   hardware. Conversely, Bluetooth's theoretical 2M-PHY ceiling is not a
   guarantee for an S3 running simultaneous WiFi and multiple BLE links.

2. **`retry_ms` establishes backpressure, not its root cause.**
   `ble_svc_send_bytes()` returns the same `false` for authorization loss,
   missing subscription, mbuf allocation failure, and any NimBLE notify
   error. Queue pressure can originate in controller credits, host
   scheduling, CPU/logging overhead, or radio scheduling. `stall_ms=0`
   rules out explicit zero-credit waits in the measured sender, not all
   costs of ACK writes, receive dispatch, or the current credit algorithm.
   Preserve error codes and count each failure class before attributing
   the remaining limit entirely to airtime.

3. **The NOTIFY_TX wakeup is not a controller-TX-completion mechanism.**
   In the pinned `ble_gatts_notify_custom()`, NimBLE calls
   `ble_gap_notify_tx_event()` immediately after attempting
   `ble_att_clt_tx_notify()`, before returning. A successful event means
   successful submission, not that the radio sent the PDU or released a
   controller slot. Indication confirmation is a separate status. [R2]
   Therefore the current callback can wake the sender on its own queued
   submissions, or on unrelated control indications. The experiment did
   not test genuine buffer-release-driven pacing. Do not treat it as a
   proven harmless TX-completion implementation. Use a supported stack
   congestion/credit signal if available; otherwise use bounded adaptive
   retries and measure HCI Number Of Completed Packets without replacing
   NimBLE's internal event handler. [R3]

   The current 200-attempt retry loop and `miss_ms += 5` authorization
   grace are also iteration-based. A wait that now ranges from immediate
   to 15 ms does not maintain the original approximately 2 s send deadline
   or 500 ms authorization deadline. A future implementation should use
   monotonic-time deadlines and separate stream-start/work notifications
   from congestion wakeups.

4. **Existing credit handling needs correctness work before higher speed.**
   The ACK handler assigns `acked = offset` and `window = advertised_window`;
   the sender independently decrements that same window. A delayed ACK
   ignores bytes already sent beyond its offset, and the shared updates
   are not a synchronized credit transaction. The host additionally uses
   the highest received end-offset, filling holes with zeros, rather than
   explicitly proving a contiguous acknowledged prefix. SHA checks catch
   corruption at the end, but these are not robust flow-control semantics.
   Define a receive right-edge, for example `contiguous_ack + capacity`,
   and send only below that edge. Validate monotonic offsets, clamp grants
   to negotiated bounds, reject stale/out-of-range ACKs, and serialize
   state changes. A 128-frame window is not automatically harmless just
   because a test completed. Do not directly reuse this algorithm as an
   ESP-NOW loss/reordering/retransmission protocol.

5. **Airtime and MTU arithmetic need refinement.** At MTU 247, ATT leaves
   244 bytes, and our 16-byte header leaves **228 file bytes**, not 244.
   The best reported 51.6 KiB/s corresponds to roughly 3.5 full file
   frames per 15 ms event on average, rather than 3.2, and includes
   non-stream costs, so it does not directly measure event occupancy.
   MTU 247 is an excellent baseline, but larger MTUs are not categorically
   useless: they amortize ATT/application headers and API dispatch over
   multiple LL packets. An ideal two-PDU packing experiment is MTU 498
   (`498 + 4` L2CAP bytes = `2 × 251`); it yields 479 file bytes versus
   456 in two present notifications. Actual host/controller fragmentation
   must be checked. Blindly choosing 517 can produce a short third
   fragment. **Do not increase MTU by configuration alone:** the present
   `frame[260]` buffer and all framing/reassembly limits must first be
   made consistent with the new size. Expect an incremental improvement,
   not a doubling.

6. **Connection-event hints are not guaranteed airtime reservations.**
   NimBLE connection intervals use 1.25 ms units; `min_ce_len` and
   `max_ce_len` use **0.625 ms units**. [R4] A 15 ms interval is 12
   interval units but 24 event-length units. The S3 controller offers
   ORIGINAL, HCI-CE-parameter, and Espressif-defined event-length modes;
   the current build uses ORIGINAL. [R5] Test the HCI-CE-parameter mode
   on the actual central rather than assuming `max_ce_len` alone changes
   behavior. Never reserve the entire interval at the expense of other
   active links. Sweep 7.5, 15, and 30 ms: shorter is not always faster
   under coexistence and multiple connections.

7. **Caching reduces traffic contention but does not eliminate coex.**
   Espressif describes time slices based on WiFi state, including a
   WiFi-CONNECTED/BLE-CONNECTED scheme with 50% shares. [R6] An associated
   but idle STA still has beacons and coexistence scheduling; keeping BLE
   connections alive also entails connection events without application
   traffic. Other SomnoTrace BLE links remain relevant during ESP-NOW.
   Likewise, 250 ms HTTP service time is not 250 ms of occupied RF time:
   the stated 5% poll duty is not a measured radio duty cycle. Do not
   disable coex, misuse BLE-Mesh status APIs, or assume the deprecated
   coexistence preference API is a generic throughput switch.

8. **Several forecasts are unsupported or inconsistently scoped.** The
   retained downloads total **8,011,138 bytes in 95 files**. In the serial
   log, the final 95 completions before the first speed-experiment reboot
   sum to exactly that total; first-to-last completion spans **984.363 s
   (16.4 minutes)**. This is a completion span, not a precise request-start
   to final-verdict measurement, but it does not support the stated
   approximately 35-minute clean sweep. Do not use the earlier 3.8 KiB/s
   aggregate for planning without its underlying timestamps. Also, six
   populated folders are not a full month of typical therapy; the
   95-file run establishes a multi-day reliability result, not a
   validated 30-night capacity model.

   The approximately 1-minute compressed-BLE forecast assumes a warm
   cache and omits fetch/compression/validation latency. For a cold,
   staged transfer, time includes `raw_bytes / card_rate` **plus**
   `encoded_bytes / radio_rate`, plus CPU, persistence, and validation.
   Pipelining can overlap stages, but then coexistence must be measured.

9. **EZShare channel setting is supported; transparent safe automation
   remains unvalidated.** Manuals describe user-set channels and resetting
   configuration by deleting `ezshare.cfg`. [R10] They do not establish
   the exact endpoint for this card, an immediate channel switch, nor
   which SD/FAT sectors every channel change writes. A configuration-file
   association is evidence of a possible card-write risk, not proof of a
   particular write sequence or a quantified corruption probability.
   `THERAPY_OUT` does not mean AirSense has unmounted the card. Spare-card
   characterization and explicit opt-in remain essential. An unavailable
   admin credential or required physical power cycle prevents completely
   transparent setup. Invalid regulatory channels cannot be worked around
   by choosing the hopping mode; both modes must remain legal.

10. **A2 has a real monitoring gap and is not always available.** A
    20–30 s hop plus reconnect misses multiple 5 s observations. Poll gaps
    can enlarge timing uncertainty and delay resume detection even if the
    user sees no prompt. Reconnect time and static-IP safety are unmeasured;
    a guessed address risks collisions. The current `netw_reconn` task
    automatically reconnects while STA is disconnected, so it would fight
    an intentional hop unless a separate mode suppresses recovery, scan,
    and scheduler activity. Wait for WiFi state transitions; do not race
    `esp_wifi_set_channel()` against a connecting/scanning STA. [R7]
    Bound the hop *and* recovery, expose observation gaps honestly, and
    fall back to BLE. Do not make A2 the default under the original
    uninterrupted-monitoring requirement.

11. **Compression feasibility is stronger than the earlier extrapolation,
    but ROM performance is still unknown.** Python zlib and ROM miniz do
    not have identical speed or search strategies. Host MB/s cannot be
    scaled into a trustworthy S3 result. The ROM header fixes a 32 KiB
    dictionary and compressor layout: the 4 KiB-window zlib experiment
    does **not** imply lower memory consumption from the ROM compressor.
    Use the compiled `sizeof(tdefl_compressor)` and peak heap measurements;
    a different small-window implementation is a separate dependency
    decision. [R9] Raw deflate, zlib, and gzip are different wrappers;
    negotiate the actual format rather than calling them interchangeable.

12. **The recommendations require a protocol/spec revision.** The
    authoritative `docs/implementation-spec.md` currently excludes
    content caches and EZShare configuration calls (§1/§11), requires
    per-file post-validation (§12), and prohibits concurrent card requests
    during a body (§6/§11). The runtime's independent monitoring/download
    paths and the newer goals need reconciliation. Shared batch validation,
    persistent caching, compression, and A1/A2 are proposals, not already
    approved v1 behavior. Separately, §5's “uncommitted” note is historical:
    the tuning code is now in HEAD `f32ed17`.

### A.3 Additional compression validation and a realistic BLE-only target

Read-only host tests rechecked all 95 downloaded files. The transformation
preserved EDF headers and delta-coded body words modulo 65536; inverse
transformation plus decompression reproduced **every original byte**.
No patient fields, filenames, or waveform excerpts are recorded here.

| Encoding (Python zlib, not yet ROM miniz) | Aggregate ratio | BRP ratio |
|---|---:|---:|
| Plain, level 1, whole file | 1.968× | 1.792× |
| Plain, level 3, whole file | 2.053× | 1.865× |
| Delta16, level 1, whole file | 2.546× | 2.337× |
| Delta16, level 3, whole file | 2.679× | 2.455× |
| Delta16, level 9, whole file | 2.657× | 2.418× |
| Delta16, level 1, independent 64 KiB blocks | 2.506× | 2.298× |
| Delta16, level 3, independent 64 KiB blocks | 2.609× | 2.388× |

The last two rows reset **both the delta predictor and deflate stream**
at each raw-block boundary and passed independent byte-exact round trips.
Their aggregate encoded totals were 3,196,909 and 3,071,118 bytes,
respectively, before any new protocol block headers. Level 3 beating
level 9 is possible because search decisions differ; choose the best
measured total transfer cost, not the largest level number.

This supports a **bounded block codec**, not a mandatory whole-file cache:

- Start with plain deflate as the simplest negotiated option, then add
  an explicitly versioned reversible delta16 filter.
- Prototype 32/64/128 KiB raw blocks; 64 KiB is a useful tested baseline.
  A malformed/unknown EDF need not be interpreted as medical signals:
  use plain compression or raw fallback instead. Define header boundaries,
  byte order, unsigned wraparound, predictor reset, and odd trailing bytes.
- Give each block raw/encoded lengths, codec, index/raw offset, and a
  checksum; bound decompression output and memory. Fall back to raw when
  compression expands data. Final SHA-256 covers the original file.
- ACK/credits operate in **encoded transport bytes**, while resume and
  downstream file accounting use defined raw-block boundaries. Persist
  only verified progress. Independent blocks allow retransmission,
  streaming decompression, and resume without replaying an entire gzip
  member or retaining its whole history.

For `C = raw/encoded`, warm-cache effective throughput is approximately
`C × encoded_radio_rate`. Using the measured independent-block ratios:

| Encoded link rate | Aggregate at 2.609× | BRP at 2.388× |
|---|---:|---:|
| 20 KiB/s | 52 KiB/s | 48 KiB/s |
| 40 KiB/s | 104 KiB/s | 96 KiB/s |
| 50 KiB/s | 130 KiB/s | 119 KiB/s |
| 75 KiB/s | 196 KiB/s | 179 KiB/s |

These are arithmetic projections, not on-device results. Approximately
**38.3 KiB/s aggregate or 41.9 KiB/s BRP encoded throughput** would reach
100 KiB/s before overhead. Aim for at least **50 KiB/s sustained encoded
throughput under production load** to leave useful margin. Compression
cannot make every incompressible file exceed its raw link rate.

### A.4 Revised priorities: exhaust BLE GATT before changing transports

| Priority | Recommendation | Validation required |
|---|---|---|
| P0 | Add a controlled SDK comparison: v5.5.1 baseline versus v5.5.5 first; evaluate v6.0.3/v6.1 after the migration audit (A.9). | Compare identical effective settings, both actual S3s, memory margins, sustained throughput, and required-service latency. An SDK release number is not a speed result. |
| P0 | Separate radio-only, card-only, compressor-only, SD-writer-only, and combined timings. Record exact send errors, DLE TX/RX octets/time, PHY, interval, credits, queue occupancy, and monotonic deadlines. | Identify whether each stall is RF, HCI/mbuf, CPU, logging, receiver storage, or source I/O. |
| P0 | Remove per-PDU NimBLE INFO output from performance runs; retain sampled diagnostics and errors. | Pinned `ble_gatts_notify_custom()` logs every notify attempt. Hundreds of packets/s mean thousands of formatted log operations. Test with console attached and absent. [R2] |
| P0 | Replace the submission-event wakeup and iteration-based deadlines; make flow-control updates synchronized and ACK only contiguous verified receipt. | Tests with delayed/duplicate ACKs, missing frames, stalled receivers, link loss, and authorization loss; no unbounded run-ahead or false success. |
| P1 | Tune the actual SomnoTrace central: negotiated 2M + 251-byte DLE, latency 0, event-length mode/hints, and a 7.5/15/30 ms sweep. | A useful raw target needs roughly seven 228-byte frames per 15 ms event (about 104 KiB/s), or equivalent event occupancy. Prove it rather than infer it from PHY. |
| P1 | Separate CPU contention before increasing pools. Current WiFi task, NimBLE host, and controller all use core 0. Espressif recommends WiFi on a different core from BT host/controller. [R6] | Measure both affinities, stream/compressor priority, scheduler latency, and 160/240 MHz operation. Keep callbacks short and worker tasks yielding. |
| P1 | Bound a PSRAM producer/consumer queue and receiver write queue; use receive-buffer watermarks rather than ACKing every frame. | Account separately for MSYS, HCI, controller static/dynamic ACL buffers, task stacks, and crypto. Increase only the saturated resource, not every pool. |
| P1 | Replace the 2.5 s sleep with an actual successful post-refresh completion signal; reuse a persistent BLE session and batch admission. | A completed post-body scan, not a still-old matching catalog tuple, must authorize final success. Bound failure/timeouts and continue required monitoring. |
| P2 | Implement negotiated block compression and benchmark ROM miniz with PSRAM state on both S3s. | Choose plain/delta and search effort by encoded bytes plus CPU time; verify byte-exact files and final source verdicts. |
| P2 | Add bounded PSRAM block staging, optional validated idle prefetch, and block resume. | Compare a staged BLE burst with live pipeline operation. Do not require a 1.5+ MiB allocation to transfer a large file. |
| P3 | Benchmark larger-MTU GATT or L2CAP CoC only after the above. | CoC removes some framing/credit overhead, not the LL or coexistence limit. Its 4–64 KiB SDUs require suitable stack pools; it is not automatically simpler or faster on a 2 MiB bridge. |

A practical low-complexity pipeline is:

`scheduled HTTP read → bounded raw block → transform/compress worker →
encoded block queue → GATT notifications → SomnoTrace receive queue →
inflate/inverse transform → SD .part → verified atomic publication`.

Use a small number of fixed PSRAM blocks and backpressure. Preserve
partial-frame residue across HTTP chunks instead of repeatedly sending
short final notifications at every 4 KiB read boundary. A staged mode
can pause source reads during BLE bursts while still respecting the
validated card scheduling policy; a pipelined mode can overlap CPU and
I/O when measurements show it wins. Full-file staging doubles the cold
critical path when nothing overlaps, so it is not universally optimal.

HTTP reuse remains worthwhile, but `keep_alive_enable` enables TCP
keepalive probes; it is not itself an HTTP persistent-connection switch.
Reuse the handle only with correctly completed responses and a server
that does not request `Connection: close`. [R11] Measure WiFi modem-sleep
latency: default STA power saving is MIN_MODEM and the current WiFi
manager does not explicitly override it. A bounded `WIFI_PS_NONE` test
may reduce receive latency; it does not bypass coexistence slices. [R7]

Avoid a flash cache in the first optimization increment. It adds patient
data at rest, write/erase stalls, wear management, invalidation, and
partition-layout/upgrade concerns. Low-priority flash writes are not a
hard guarantee against interrupt/cache stalls. If later justified, use
bounded encrypted storage, atomic cache entries, characterized erase
scheduling, and card/source invalidation. A same-size/stamp tuple is not
cryptographic proof that a cache is still current, especially for
rewrite-prone root files or an indistinguishable card replacement.

### A.5 Better optional transport choices without disconnecting SomnoTrace

ESP-NOW remains useful, but is not the only WiFi choice. The missing
alternative is **the bridge joins the existing home AP temporarily and
sends ordinary TCP to SomnoTrace**. Neither a bridge SoftAP nor a change
to EZShare's channel is required.

| Choice | SomnoTrace home WiFi | Card observation | Assessment |
|---|---|---|---|
| Optimized compressed BLE | Stays connected | No intentional disconnect | Preferred first implementation; works when E and H differ. |
| ESP-NOW when E already equals H | Stays connected | Can remain connected | Good optional accelerator with no router/card changes. Probe actual v2 support, encrypted frame size, rates, loss, and coexistence. |
| Driver-managed off-channel ESP-NOW, newer 5.5/6.x (A.9) | Stays connected on H | May remain associated on E; radio temporarily unavailable there | Promising alternative to explicit disconnection. Prove short bounded excursions, encrypted v2 delivery, automatic home-channel return, and no missed poll deadlines. |
| ESP-NOW after opt-in EZShare alignment | Stays connected | Alignment may interrupt; steady state can stay connected | Possible after spare-card characterization, explicit permission, and spec revision. Not a universal zero-touch guarantee. |
| Bridge hops to H for ESP-NOW | Stays connected | Intentional observation gap | No router credentials needed, but only an explicitly permitted idle mode. BLE is the safe fallback when gaps are unacceptable. |
| **Bridge switches STA to the home AP, TCP to SomnoTrace** | **Stays connected** | **Intentional observation gap** | Prefer to a bespoke reliable ESP-NOW file protocol when idle gaps and secure home-WiFi provisioning are acceptable. Uses TCP flow control/retransmission, at the cost of association/DHCP and router reachability. |
| SomnoTrace provides a temporary SoftAP while its STA remains on home WiFi | Stays connected (APSTA on H) | Bridge leaves EZShare to join this AP | Technically possible, unlike two simultaneous STAs. Avoids handing the bridge home credentials, but adds AP RAM/CPU/RF load and requires full SomnoTrace regression testing. |
| Bridge has a second WiFi radio | Stays connected | Can remain connected on E | Only generally applicable simultaneous two-channel WiFi solution; hardware/BOM/driver change, not a firmware setting. |

For the TCP/home-AP option, pre-stage cache blocks while on E, switch only
SomnoBridge to home WiFi, and let it connect to a BLE-advertised/pinned
SomnoTrace endpoint on that LAN. This avoids NAT, proxying EZShare, and
any second HTTP client on the card. Secure provisioning needs explicit
permission: do not silently extract or export SomnoTrace's existing
network credentials. Use pinned TLS or a reviewed authenticated-encrypted
session bound to the existing BLE pairing. A hash alone does not secure
an otherwise plaintext file channel. Guest-network client isolation or
lack of home-AP coverage may prevent LAN transfer; fall back to BLE.

For the temporary SomnoTrace SoftAP option, AP and STA share **H**, so
there is no requirement to move the home STA. The earlier rejection
only applies to a *bridge* AP on H while its STA remains on E. However,
Espressif classifies SoftAP client + BLE coexistence as supported with
unstable performance, unlike STA + BLE. [R6] Do not call it seamless
until existing sensor links, home connection, uploads, and memory margins
pass tests. TCP paths are worth comparing against ESP-NOW before adding
custom selective-repeat retransmission and reorder buffers.

ESP-NOW v2's 1470-byte limit and 1 Mbps default are verified in the
v5.5.1 documentation. [R8] Higher configured PHY rates do not directly
translate into file throughput. Its send callback confirms MAC-layer
receipt, **not application processing or SD persistence**. Follow the
SDK's callback pacing guidance, keep WiFi callbacks short, and provide
bounded application ACK/retry/deduplication and receiver credits. Negotiate
v1/v2 compatibility; benchmark **encrypted unicast**, not broadcast.
Set explicit random PMK/peer LMK material over the authenticated channel;
never use a default PMK or log keys. BLE idle does not remove coexistence
with the remaining AS11/oximeter connections.

### A.6 What “completely seamless” can and cannot mean

**It can be seamless to the user after normal pairing:** select the best
mutually supported transport automatically, use the same logical manifest
and file verdicts, preserve a common raw-block resume position, and
fallback to BLE without re-pairing or partial files being marked complete.
Cached data still needs source/monitoring validation before publication.
Missing acceleration capability should mean “slower”, not “unable to sync”.

**It cannot honestly promise zero impact on every function with the
present single radios.** All transports consume shared RF/CPU resources.
In particular, deliberately disconnecting the bridge removes observation
of EZShare; channel-setting mutations may restart the card AP. An OUT
state is not a reservation that therapy cannot resume. Never silently
pause oximetry acquisition, disconnect active sensor links, stop home
network services, or preserve an old “validated OUT” through an observation
gap just to meet a throughput number.

A safe automatic policy is:

1. BLE is always available as the baseline, with identical security and
   verification requirements on every faster transport.
2. Use same-channel ESP-NOW only when capability/channel probes succeed
   without scanning or moving the connected radios, and measured load
   leaves required services healthy. Handle router roam/channel changes
   by stopping acceleration and falling back, not immediately rewriting
   card configuration.
3. Attempt any bridge-only network switch only after explicit acceptance
   of bounded idle monitoring gaps, complete pre-staging, and a fresh
   transfer-allowed decision. Otherwise stay on BLE. Suspension/recovery
   is an explicit state machine, not an accidental WiFi failure.
4. Preserve timing anchors/evidence independently of the data plane;
   journal gaps and reacquisition, prioritizing therapy monitoring over
   bulk speed. Respect regulatory channels and retain configuration/UI
   access throughout recovery.
5. Cap resource use and let foreground sensing/control preempt bulk work.
   Defer optional uploads only when acceptable; association alone is not
   proof that home-network functionality remained responsive.

Thus, **yes to automatic transport negotiation and recovery; no to calling
20–30 s blind windows “unaffected functionality.”** For the strict no-gap,
no-card-write, no-home-disconnect contract, compressed BLE is the safest
universal choice on the existing hardware.

### A.7 Recommended decision and acceptance gates

1. **Adopt the BLE-first plan in A.4**, not automatic EZShare rewriting or
   default bridge hopping. Revise the behavioral spec before implementing
   cache/compression/batch changes; this appendix is analysis, not that
   authorization or a compatibility change.
2. Define the target explicitly: KB = 1000 bytes, KiB = 1024 bytes.
   Report (a) encoded payload rate, (b) restored original-byte rate,
   (c) cold-sync and warm-sync wall-clock rates including validation, and
   (d) latency for small files. Compression does not increase physical
   link speed, and warm-cache speed is not cold-download speed.
3. Measure two actual S3s with all production encryption and sensor/home
   network load. Seek at least 50 KiB/s encoded bulk rate and 100 KiB/s
   restored rate on representative compressible files; treat 100 KiB/s
   uncompressed as a separate stretch goal, not an unsupported promise.
   Record median and lower-percentile performance, not just the fastest
   run; include RF range/congestion and a genuine 30-night data volume.
4. Verify byte counts, final original SHA-256, decompression bounds,
   contiguous offsets, source freshness, and `completion_allowed` for
   every file. Replace fixed sleeps without removing their safety purpose.
5. Reject optimizations that introduce reboots, heap fragmentation,
   missed required observations, degraded timing-state truthfulness,
   missing oximeter samples, sensor link loss, or home-network disruption.
   Measure scheduler issue/completion gaps rather than merely counting
   successful log lines. Include retries, packet loss, receiver SD stalls,
   reboot/resume, and absence of the serial reader in the soak tests.
6. If compressed BLE does not meet the production-load target, first
   trial same-channel ESP-NOW with BLE fallback. When E differs from H,
   test the newer driver-managed off-channel option in A.9 before assuming
   explicit WiFi disconnection is necessary. Accept it only if required
   poll deadlines and other services remain healthy. Otherwise, decide
   explicitly whether idle monitoring gaps are acceptable before comparing
   bridge-to-home-AP TCP with ESP-NOW hopping. If neither short excursions
   nor gaps are acceptable, a second radio or validated opt-in channel
   alignment is needed for a WiFi data plane.

**Bottom line:** staying BLE-only can credibly meet a good effective
throughput with substantially less complexity. Prove sustained queue and
central performance, remove fixed latency safely, and add bounded lossless
compression before committing to channel management or persistent caches.

### A.8 References and validation scope

- **[R1]** ESP-IDF v5.5.1 NimBLE throughput sample README (sample results,
  not an S3 ceiling):
  https://raw.githubusercontent.com/espressif/esp-idf/v5.5.1/examples/bluetooth/nimble/throughput_app/README.md
- **[R2]** Pinned NimBLE `ble_gattc.c`, especially
  `ble_gatts_notify_custom()` and `ble_gattc_log_notify()`:
  https://raw.githubusercontent.com/espressif/esp-nimble/b45dcedcafb7888174c3567002c36b342ec0b723/nimble/host/src/ble_gattc.c
- **[R3]** Pinned NimBLE controller-credit/data-length event processing,
  `ble_hs_hci_evt_num_completed_pkts()` and data-length-change dispatch:
  https://raw.githubusercontent.com/espressif/esp-nimble/b45dcedcafb7888174c3567002c36b342ec0b723/nimble/host/src/ble_hs_hci_evt.c
- **[R4]** Pinned NimBLE GAP header, connection/event units and
  `BLE_GAP_EVENT_DATA_LEN_CHG`:
  https://raw.githubusercontent.com/espressif/esp-nimble/b45dcedcafb7888174c3567002c36b342ec0b723/nimble/host/include/host/ble_gap.h
- **[R5]** ESP-IDF v5.5.1 controller configuration; S3 includes this
  C3-family Kconfig (event-length modes, static ACL buffers):
  https://raw.githubusercontent.com/espressif/esp-idf/v5.5.1/components/bt/controller/esp32c3/Kconfig.in
- **[R6]** ESP32-S3 coexistence policy, supported scenarios, core affinity:
  https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-guides/coexist.html
- **[R7]** ESP32-S3 WiFi driver/APSTA home-channel and power-saving rules,
  plus channel API restrictions:
  https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-guides/wifi.html
  https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/network/esp_wifi.html
- **[R8]** ESP-NOW v5.5.1 frame sizes, keys, callback semantics/pacing:
  https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/network/esp_now.html
- **[R9]** ROM miniz header/layout and fixed dictionary:
  https://raw.githubusercontent.com/espressif/esp-idf/v5.5.1/components/esp_rom/include/miniz.h
- **[R10]** EZShare user-guide excerpts describe configurable WiFi
  channels and deleting `ezshare.cfg` to reset configuration; they do not
  establish the current card's mutation sequence or safe online writes:
  https://fcc.report/FCC-ID/A7JEZS128/1903606.pdf
  https://fcc.report/FCC-ID/A7JEZS126/1633135.pdf
  The FCC PDFs were not directly fetchable during this review; indexed
  excerpts corroborate the features, not their implementation details.
- **[R11]** ESP HTTP Client persistence/cleanup and TCP keepalive:
  https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/protocols/esp_http_client.html

Local checks: `components/ble_api/ble_ops.c`, `ble_svc.c`,
`components/netw/wifi_mgr.c`, `components/ezshare/ezshare.c`,
`scripts/ble_query.py`, effective `sdkconfig`, `partitions.csv`, and the
behavioral spec. Aggregate compression round trips and serial-log totals
were validated; no new physical-link, EZShare mutation, or ROM-compressor
throughput claim is made by this appendix.

### A.9 ESP-IDF upgrade opportunities: latest 5.5, 6.0, and 6.1

Release status checked **2026-10-06** against Espressif's GitHub releases
and the official release-notes database. These are released versions,
not guesses based on a moving `stable`/`latest` documentation alias. [U1]
No SDK pin, configuration, source code, or installed firmware was changed.

#### A.9.1 Candidate versions and recommendation

| Candidate | Published | Assessment for this project |
|---|---|---|
| **v5.5.5** — newest released 5.5 patch found | 2026-07-17 | **First upgrade/A-B candidate.** Relevant controller fixes, NimBLE memory options, off-channel ESP-NOW wrappers/fixes, and better benchmark examples without requiring a 6.x crypto migration. |
| **v6.0.3** — newest released 6.0 patch found | 2026-09-02 | Useful second candidate. Includes newer throughput-example work and relevant WiFi fixes, but requires the 5.5→6.0 migration audit. No new S3 radio modulation is established. |
| **v6.1** — released, not beta/RC | 2026-08-27 | Worth benchmarking once the 6.x port exists. Newer NimBLE/controller/toolchain changes may improve stability or scheduling, but a higher minor number does not mean it contains every patch present in the later v6.0.3/v5.5.5 release. |

**Revised recommendation:** do not freeze the project on v5.5.1. Test
v5.5.5 early alongside the BLE-first optimization plan. Keep v6.0.3 and
v6.1 eligible if production-load measurements justify them. There is no
verified “upgrade alone gives 100 KiB/s” result; select the fastest stable
combination, not the newest label.

The inspected NimBLE submodule revisions also differ:

- v5.5.5: `685675c0128deafdd201c9eb82e61d227364646c`.
- v6.0.3: `6330413d856dd5427c14a3570e09b2e114d38c53`.
- v6.1: `139cada0ae932957fa06ba37d17e3c9c2c95c773`.

These identify the reviewed host sources, not a benchmark or an assertion
that the three branches form a simple chronological superset chain.

#### A.9.2 Hardware/protocol limits versus SDK features

**ESP-IDF 6.x is not Bluetooth Core 6.x.** Nor does S3 become S31 when
IDF adds preview support for ESP32-S31. Software can fix queueing,
controller scheduling, interoperability, and memory use; it cannot add
an RF peripheral/modulation absent from the existing silicon.

The target-specific **v6.1 ESP32-S3** support table [U2] confirms:

- LE 2M PHY and 251-byte DLE are supported; both were already available
  in v5.5.1. No faster-than-2M LE PHY is documented for this target.
- Bluetooth **Frame Space Update**, Channel Sounding, ISO channels, and
  the standardized newer **Shorter Connection Intervals** feature are
  marked unsupported on the S3 controller. A generic host release note
  adding Frame Space Update does not establish support on these boards.
  The S3 transfer plan should therefore not assume a shorter negotiated
  inter-frame spacing or a new high-rate PHY.
- EATT is shown as experimental for NimBLE. It is a host concurrency
  feature, not new modulation; it does not multiply radio bandwidth for
  an already-pipelined notification stream. EATT configuration already
  existed in v5.5.1, as did LE L2CAP CoC.
- The S3 WiFi feature list remains **802.11b/g/n**. [U3] Upgrading does
  not add WiFi 6/802.11ax, 5 GHz, or simultaneous independent WiFi
  channels. New-chip WiFi/PSRAM features in release notes must not be
  attributed to ESP32-S3R2.

There is nevertheless a **vendor-specific interval exception** worth
recording separately from the standardized feature table. Newer release
notes advertise a **3.75 ms minimum interval on ESP32-C3/S3**. The actual
controller Kconfig [U4] says:

- `BT_CTRL_BLE_MIN_CONN_INTERVAL_ENABLE` permits intervals below the
  Bluetooth-spec minimum, down to `3 × 1.25 ms`.
- It warns against large ATT/DLE payloads at that short interval.
- Its documented coordinated host integration is **Bluedroid**, with
  equivalent NimBLE support stated as planned, not available through a
  supported selectable NimBLE setting. The inspected NimBLE validation
  still defaults to the standard 7.5 ms floor.

Thus **3.75 ms is not our recommended bulk-speed lever**. It is a
non-standard low-latency experiment that may fit fewer full PDUs per
event and would entail a host-stack/compliance/interoperability decision.
Do not switch to heavier Bluedroid or bypass NimBLE validation merely to
obtain more events per second. Compare compliant 7.5/15/30 ms intervals
and longer usable event occupancy first.

#### A.9.3 Concrete BLE-only upgrade opportunities

1. **Memory headroom and more reliable connection scheduling.** Newer
   5.5 patches introduce `BT_NIMBLE_STATIC_TO_DYNAMIC` and
   `BT_NIMBLE_MEMPOOL_RUNTIME_ALLOC`, with optional block reuse. These
   options are present in v5.5.5 and 6.x but absent in the inspected
   v5.5.1 Kconfig. [U5] Dynamic allocation of formerly static structures
   may free baseline RAM; on-demand mbuf allocation can reduce startup
   preallocation pressure. This matters because our internal-heap
   shortage prevented host-task creation and pairing.

   **Not automatically faster:** the runtime-pool help explicitly warns
   of a performance cost. Per-packet allocation/free can increase jitter
   and fragmentation, and full streaming still needs peak memory. Test
   fixed pools versus runtime pools/block reuse with the same bounded
   in-flight capacity. Keep hot-path buffers internal where required;
   do not indiscriminately move controller/DMA/flash-sensitive objects
   to PSRAM. Allocate compression/staging in PSRAM separately.

2. **S3-specific stability fixes useful to the real central.** The newer
   controller notes include fixes for `0x08` disconnection during
   multi-connection operation and connection-parameter updates on C3/S3.
   [U4] These could let SomnoTrace sustain tuned events while retaining
   sensor links, even when they do not improve peak bandwidth. The v5.5.4
   release also explicitly fixes a NimBLE connection-loss regression
   introduced in v5.5.3. Use the latest patch, not v5.5.3 as the target.
   Preserve bonds and test every connected sensor; do not assume a
   controller update repairs application credit/authentication logic.

3. **Much better benchmark guidance, including CoC.** The v5.5.5 and
   v6.0.3 throughput trees include dedicated L2CAP CoC examples. Their
   README reports approximately **741 kbps at 1M and 1310 kbps at 2M**,
   but explicitly for **ESP32-C6**, MTU 2048, DLE 251, and traffic
   **central→peripheral**. [U6] That is evidence that pipelined BLE can
   exceed 100 KiB/s on a supported controller, not an S3 result. Our
   production direction is peripheral→central, so remeasure it on S3s.

   v6.0.3/v6.1 also contain an “Optimize NimBLE throughput example”
   change. [U7] Its implementation uses LL-aligned 495-byte notify/write
   payloads (502 bytes including ATT/L2CAP, exactly two 251-byte PDUs),
   stronger pipelining, and distinct interval profiles: longer events
   for write/notify, short round trips for reads. This reinforces A.4's
   interval sweep and A.2's packing experiment instead of insisting on
   the smallest interval and largest MTU. It is **example code**, not an
   automatic speed-up in our existing firmware. Adopt principles through
   a clean-room implementation; do not copy sample sources into the repo.

4. **Logging/host fixes help, but retain the diagnosed corrections.**
   v5.5.5 includes guards against non-DEBUG mbuf/flat-buffer dump work;
   v6.1 notes HCI logging optimization. Neither replaces disabling
   per-notification INFO output in the production hot path. Inspection
   of the v5.5.5 and v6.1 NimBLE sources confirms that
   `ble_gatts_notify_custom()` still emits NOTIFY_TX at submission time.
   Upgrading does **not** turn it into the missing controller-buffer-
   release callback, fix our shared-credit race, or make the current
   retry counter a real deadline. A.2/A.4 remain necessary.

5. **New crypto/compiler/backend changes are measurement opportunities,
   not guaranteed compression acceleration.** The ROM deflate/inflate
   machine code belongs to the chip and is not replaced by IDF. A newer
   compiler can affect our delta filter, copies, hashing, and scheduling;
   compare CPU cycles, PSRAM bandwidth, and raw/encoded throughput.
   Hardware-accelerated SHA/crypto must preserve the original-file hash
   and security semantics. No compressor speed gain from an SDK upgrade
   has been measured here.

**Priority:** fixed latency, correct pacing/credits, core affinity, and
compression stay ahead of a wholesale GATT→EATT or NimBLE→Bluedroid
rewrite. An SDK upgrade should be one independent experimental factor.

#### A.9.4 New transport opportunity: short off-channel ESP-NOW without explicit disconnects

This is the most important upgrade-driven addition to the architecture.
The newer 5.5 line adds the public wrappers:

- `esp_now_switch_channel_tx()` — visit a specified channel for a
  duration and send one ESP-NOW payload.
- `esp_now_remain_on_channel()` — request a bounded remain-on-channel
  operation.

They are absent from the inspected v5.5.1 `esp_now.h`, present in v5.5.2,
and retained in v5.5.5/v6.1. [U8] Generic WiFi action-frame/ROC APIs
existed earlier; the opportunity is a supported ESP-NOW-facing path and
newer driver fixes, not invention of a second radio. Crucially, v5.5.5,
v6.0.3, and v6.1 fix off-channel ESP-NOW failure when
`SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is enabled—the bridge uses that option.
[U9]

**Candidate A3:** keep SomnoTrace associated to home WiFi on H, keep the
bridge associated to EZShare on E, and let the bridge make small
**driver-managed** excursions to H between mandatory card observations.
Use BLE to coordinate transfer identity/capabilities, not to move either
AP. No EZShare admin credential, settings write, home-WiFi credential
sharing, or deliberate reassociation is inherently required by this
candidate.

This qualifies the earlier “only alignment or disconnect/hop” discussion.
**Same-channel contact is still necessary at the instant of ESP-NOW
transmission, but matching permanent AP channels or explicitly dropping
STA association may not be.** It is not simultaneous dual-channel RF:
while the bridge is physically on H, it cannot receive card traffic on E.
A3 can count as seamless only if those short absences fit validated
scheduler/beacon margins and **do not miss required 5 s polls**, degrade
sensor collection, or break either association.

Before recommending it, characterize the actual S3 driver:

- Probe short excursions, initially tens of milliseconds rather than
  the earlier 20–30 s blind windows. Measure issue/completion/home-channel
  return, cancellation, queue contention, and long-run beacon/association
  behavior; never assume a requested dwell time is a hard deadline.
- Determine whether multiple encrypted v2 packets can be sent efficiently
  within one dwell, or whether the wrapper visits H for every single
  packet. Its public contract says “send one ESP-NOW data”; do not assume
  a high-throughput burst queue or that ordinary `esp_now_send()` adopts
  the off-channel operation's settings.
- Verify LMK/CCMP encryption, 1470-byte support, per-peer rates, send-
  completion semantics, and ACK reception during the dwell. Never use
  plaintext medical payloads to work around an unsupported encrypted
  path. Preserve application credits, ordering, retries, and final SHA.
- Serialize operations with scans, HTTP activity, reconnection, and the
  existing scheduler. Suspend *optional* source prefetch during a burst,
  retain reserved card-poll slots, and cancel/defer acceleration as soon
  as monitoring or other foreground work needs the radio. Test cold and
  warm block staging; API presence does not prove that concurrent card
  downloads and ROC are safe or efficient.
- Keep SomnoTrace on H for reception. Include its existing BLE sensors
  and WiFi traffic in tests; association must be accompanied by healthy
  service latency. Use automatic BLE fallback for unsupported driver
  behavior, excessive retries, or insufficient poll/beacon slack.

Throughput depends on usable dwell fraction, PHY rate, switching overhead,
and coexistence—not just the configured ESP-NOW bitrate. Example only:
200 KiB/s during usable bursts at 25% duty is 50 KiB/s encoded, before
other overhead; a 2.6× codec could make that useful. **These are target
arithmetic, not measured A3 results.** A3 could remove repeated association
cost and card-setting writes, but cannot promise unlimited away-channel
windows without a functional trade-off.

Place A3 **after the compressed-BLE benchmark, but before persistent card
channel rewriting or long explicit disconnect windows**. It is already a
reason to evaluate v5.5.5; jumping directly to 6.x is not required for the
public wrappers and the cited PSRAM/off-channel fix.

Other new-looking features such as WiFi Aware/NAN discovery/security,
mesh, or host EATT should not be assumed to solve this use case. They
still require target support, channel/radio scheduling, and application
protocol work. No documented new S3 modulation or proven drop-in
simultaneous dual-channel bulk transport was found in this review.

#### A.9.5 Migration and fair upgrade-testing plan

**v6.x is feasible, but not a tag-only production change.** The official
5.5→6.0 guide [U10] documents Mbed TLS 4.0 / PSA Crypto migration, removed
legacy interfaces, Kconfig/build changes, and altered WiFi types/APIs.
This project directly uses `mbedtls_sha256_context` and the legacy
`mbedtls_sha256_*` streaming API. Audit/port that boundary and validate
crypto startup, memory, pairing, NVS protection, and all existing hashes.
SomnoTrace additionally needs its TLS/upload/sensor stack regression suite.
Preserve security controls; do not disable checks to make a newer build
fit or compile.

Recommended controlled sequence:

1. Keep the v5.5.1 image, effective non-secret settings, resource counters,
   and reproducible test inputs as the control. In separate clean build
   environments, compare **5.5.1 → 5.5.5 → 6.0.3 → 6.1**. The repository
   wrapper supports a pinned `IDF_TAG` override; no wrapper edit is needed
   for evaluation. Do not reuse one generated sdkconfig/build cache across
   major versions, and do not erase existing bonds/settings for speed tests.
2. Run radio-only S3→S3 tests before full downloads. Test each SDK on both
   ends, plus mixed-version reconnect/backward-compatibility. Keep 2M/DLE,
   payloads, intervals, log verbosity, CPU frequency, memory caps, and
   background loads equivalent; inspect effective defaults rather than
   assuming identical symbol names imply identical allocations.
3. Compare fixed/runtime/reused pools, interval/event profiles, GATT
   packing, and CoC one change at a time. Then measure block compression
   and full cold/warm transfers with both boards' normal functions active.
   Do not attribute an example's changed payload/configuration to an SDK-
   runtime gain unless an identical application comparison confirms it.
4. Run A3 separately as a capability/performance test: encrypted v2,
   differing E/H channels, no explicit disconnect, reserved polls, and
   automatic fallback. A header declaration is not a pass result.
5. Accept an upgrade for a repeatable improvement in encoded throughput,
   low-percentile throughput, CPU/heap margin, or recovery reliability,
   while preserving file verification, sensor samples, home services,
   pairing, portal access, and monitoring deadlines. Check downgrade/bond
   persistence and failure recovery before adopting a new production pin.

**Decision:** upgrading is a real optimization opportunity—particularly
v5.5.5 for memory/stability and off-channel ESP-NOW—not an obstacle we
should impose on ourselves. The existing S3 radio ceiling is unchanged;
software efficiency and bounded channel scheduling are where the gains
can come from. Keep the BLE-first approach, but include SDK A-B tests in
P0 and the new A3 candidate in the fallback decision tree.

#### A.9.6 Upgrade references

- **[U1]** Official released-version metadata and notes (checked date
  above):
  https://github.com/espressif/esp-idf/releases/tag/v5.5.5
  https://github.com/espressif/esp-idf/releases/tag/v6.0.3
  https://github.com/espressif/esp-idf/releases/tag/v6.1
  https://github.com/espressif/esp-idf/releases/tag/v5.5.4
  Full notes were read through the site's paginated public API, not just
  the release-summary bullets:
  https://release-notes.espressif.tools/release/5.5.5
  https://release-notes.espressif.tools/release/6.0.3
  https://release-notes.espressif.tools/release/6.1
- **[U2]** Target-specific S3 BLE feature matrix, including unsupported
  Frame Space Update and standardized Shorter Connection Intervals:
  https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/ble/ble-feature-support-status.html
- **[U3]** S3 WiFi feature list and single-radio coexistence policy:
  https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/wifi-driver/overview.html
  https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/coexist.html
- **[U4]** C3/S3 controller update, interval option/help, and connection
  fixes; distinguish the vendor mode from standard Bluetooth features:
  https://github.com/espressif/esp-idf/commit/d907a9497c9fcdb47c6b501462ce866f5f3cf8dd
  https://raw.githubusercontent.com/espressif/esp-idf/v6.1/components/bt/controller/esp32c3/Kconfig.in
- **[U5]** NimBLE memory/pool configurations, compared with v5.5.1:
  https://github.com/espressif/esp-idf/blob/v5.5.5/components/bt/host/nimble/Kconfig.in
  https://github.com/espressif/esp-idf/blob/v6.1/components/bt/host/nimble/Kconfig.in
- **[U6]** Updated throughput/CoC examples; reported high-rate CoC result
  is C6, not S3:
  https://raw.githubusercontent.com/espressif/esp-idf/v5.5.5/examples/bluetooth/nimble/throughput_app/README.md
  https://raw.githubusercontent.com/espressif/esp-idf/v6.0.3/examples/bluetooth/nimble/throughput_app/README.md
- **[U7]** Throughput-example optimization (LL-aligned payloads and
  operation-specific interval/pipelining changes):
  https://github.com/espressif/esp-idf/commit/1189d24f1f852fe319e552a24c2540120c3ea6cc
- **[U8]** ESP-NOW-facing off-channel API introduction and current docs:
  https://github.com/espressif/esp-idf/commit/d14ce1f4ce7a9cb23ff3c93ac40f6f210ac6c510
  https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/network/esp_now.html
- **[U9]** Off-channel transmission fix with WiFi/lwIP PSRAM allocation:
  https://github.com/espressif/esp-idf/commit/897cf407aa70586c15cf8c6b3fe9458066cf7fce
- **[U10]** 5.5→6.0 migration and crypto/WiFi requirements:
  https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32s3/migration-guides/release-6.x/6.0/index.html
  https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32s3/migration-guides/release-6.x/6.0/security.html
  https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32s3/migration-guides/release-6.x/6.0/wifi.html

---

## Appendix B — A3 feasibility under strict constraints, BLE-only plan, and cross-board version requirements

This appendix answers two direct questions: (1) do the BLE-only and
off-channel ESP-NOW options require upgrading **both** SomnoBridge and
SomnoTrace to v5.5.5; (2) whether A3 can satisfy "no disconnects" and
"provisioned seamlessly over the existing BLE channel with no new
encryption/pairing". It also records additional ideas not yet present in
the document. Facts below were re-verified against the pinned sources;
nothing earlier in this document was modified.

### B.1 Verdict on the document

**Agree with the analysis and its ordering.** The BLE-first plan, the
honest accounting of the measured 20↔50 KiB/s variance, the compression
ratios, and the transport comparison all survive re-review. The two
claims worth keeping in view: the ~120–130 KiB/s effective projection is
still **host arithmetic, not an on-device result**, and several proposals
(cache, compression, batch validation, A1/A2/A3) require the
authoritative spec revision before implementation. Nothing in the
document contradicts the verified SDK facts in A.9.

### B.2 Answer: version requirements per option

Both repositories are pinned to ESP-IDF **v5.5.1** today: SomnoBridge in
`scripts/idf.sh` and SomnoTrace in its own `scripts/idf.sh`
(`IDF_TAG:-v5.5.1`). [B1][B2]

| Option | SomnoBridge | SomnoTrace | Notes |
|---|---|---|---|
| **BLE-only squeeze** (A.4: compression, credits, logging, core affinity, intervals, MTU packing, event modes) | **No upgrade required** — v5.5.1 is sufficient | **No upgrade required** | All changes are application-level. The SDK A/B tests in A.9.5 are *optional experiments*, not prerequisites. |
| **Off-channel ESP-NOW (A3)** | **Upgrade required**: ≥ v5.5.2; **v5.5.5 recommended** | **No upgrade required initially** — v5.5.1 receiver is sufficient | `esp_now_switch_channel_tx` / `esp_now_remain_on_channel` are absent from the v5.5.1 header, added in v5.5.2, documented for ESP32-S3 in v5.5.5. [B3][B4][U8] v5.5.5 also carries the off-channel fix under the bridge's `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` setting and the C3/S3 connection fixes. [U9][U4] |
| A1 (card channel rewrite), A2 (explicit hop), home-AP TCP | v5.5.1 sufficient | v5.5.1 sufficient | None of these depend on the new APIs. |

**Why the receiver can stay on v5.5.1.** A3's receiver (SomnoTrace) never
leaves its home channel and needs only long-existing ESP-NOW receiver
facilities, all verified present in the v5.5.1 header: peer records with
per-peer LMK + CCMP encryption, receive callbacks, and v2.0 frames up to
1470 bytes (`ESP_NOW_MAX_DATA_LEN_V2`). [B3][R8] Only the *sender* that
visits the foreign channel needs the new APIs. SomnoTrace currently has
no ESP-NOW code at all (verified by search), so its integration is a
clean addition rather than a migration. [B2]

Caveats on mixed versions:

- Run the BLE **and** ESP-NOW interop tests as a 5.5.1 ↔ 5.5.5 matrix; BLE
  GATT is standardized, but do not assume — bonds, reconnection, and the
  new provisioning op must pass with the bridge on 5.5.5 and SomnoTrace
  on 5.5.1.
- Keeping both projects on the same IDF version is a support/reproducibility
  preference, not a functional requirement of A3. SomnoTrace can upgrade on
  its own maintenance schedule.
- If SomnoTrace ever needs to *initiate* off-channel operations (not needed
  in A3 — it is the stationary receiver on H), it would need ≥ 5.5.2 too.

### B.3 A3 under the constraint "no disconnects"

**What the API actually is.** `esp_now_switch_channel_tx` /
`esp_now_remain_on_channel` are driver-managed temporary channel visits
(wrappers over the WiFi action-frame/remain-on-channel machinery used by
DPP). They are **not** disconnect APIs: the STA stays associated, the
driver schedules the excursion and returns to the home channel, and the
documented restriction is only that they not race scanning/connecting
states. [B5][U8] This is the essential difference from A2's explicit
`esp_wifi_disconnect` + `esp_wifi_set_channel` sequence.

**What can still violate the constraint — and the design rules that
prevent it:**

| Risk | Mechanism | Design rule |
|---|---|---|
| Beacon-timeout disconnect on the bridge | The STA disconnects if it misses the connected AP's beacons for the configurable inactive time. [B6] | Bound each dwell to tens of ms, well inside the inactive-time budget even with consecutive excursions; monitor `WIFI_EVENT_STA_BEACON_TIMEOUT` and any disconnect as immediate abort conditions for the accelerator. |
| Excursion overlapping card HTTP | The bridge cannot receive card traffic while physically on H; TCP retransmits, but poll latency and timing evidence degrade. | Schedule excursions only in the idle gap between 5 s polls; suspend *optional* prefetch during bursts; keep required polls reserved (A.9.4 rules). |
| Racy states | The APIs must not run while the STA is scanning or connecting. [B5] | Serialize with the existing scheduler and reconnection paths; never fire an excursion during netw reconnection or AP-mode transitions. |
| Driver defects | v6.1 fixed a crash "when using ESPNOW off channel tx". [B7] | Prefer the newest 5.5 patch for the bridge; treat any off-channel crash/assert as a stop condition, not a retry. |
| Coex interactions | ESP-NOW TX with BLE connected is "stable" in the coex table; ESP-NOW RX is stable in STA mode. | Keep BLE connected throughout (it is the control plane); still verify with real sensor load on SomnoTrace. [R6][U3] |

Honest bottom line: **no-disconnect is the design intent and a testable
property, not a guarantee from the header.** The first sustained soak
(wide session, normal poll cadence, no beacon-timeout, no disconnect
events, zero missed required polls) is the acceptance evidence. If the
driver ever violates it on this exact S3 blob, fall back to BLE.

### B.4 A3 under the constraint "no new pairing / encryption UX"

Strictly, A3 introduces **transport-layer key material**, because ESP-NOW
unicast encryption is CCMP with a per-peer 16-byte LMK (and a PMK that
encrypts LMKs). [R8] Sending medical data as plaintext ESP-NOW to avoid
key handling would violate the project's security posture and is
rejected. The constraint is therefore satisfied by making the key
lifecycle **invisible to the user**, not by removing encryption:

1. One new BLE control operation, e.g. `ESPNOW_PROVISION`, executed
   **only over the already-encrypted, authenticated BLE bond** after
   pairing is complete.
2. The bridge generates fresh random 128-bit PMK and LMK values, adds its
   ESP-NOW peer record, and sends `{LMK, peer MAC, target channel H,
   capability/version}` to SomnoTrace over that encrypted link.
3. SomnoTrace installs its peer record; both sides persist the keys in
   their existing encrypted NVS. No passkey, no button, no portal step,
   no new user-visible pairing — the BLE bond remains the sole trust
   root.
4. Rotate on `FORGET_PEER` (both sides drop peer records and keys
   together). Cap the operation to the bonded peer; strangers get the
   existing refusal paths.
5. Deriving keys from the BLE bond material instead is possible in
   principle but not recommended: it reaches into the NimBLE bond store
   internals, couples stacks, and buys nothing over a random key sent
   across an encrypted authenticated channel.

This is a **spec revision item** (new op, new stored secret, new
transport), not merely firmware detail — but it meets the stated
"provisioned over the existing BLE channel with no new pairing" bar.

### B.5 Additional ideas not yet in the document

1. **Connection-event-synchronized pacing (BLE, app-level, v5.5.1).**
   Replace retry-on-`ENOMEM` churn with an adaptive token-bucket limiter
   whose refill rate tracks the *measured* drain (frames per connection
   interval). This avoids ENOMEM storms and the logging bursts they
   cause, without any new stack signal. Complements A.2.3/A.4.
2. **Poll-priority gating (BLE, app-level).** Back the BLE stream off for
   the ~hundreds of ms around each 5 s card poll so the poll's HTTP
   round completes uncontended. Costs a small, measurable throughput
   fraction; protects poll latency and the timing-evidence quality the
   whole project exists for.
3. **ESP-NOW chunk alignment (A3).** Size encoded blocks/frames to the
   v2 payload (1470 B minus a small block header) so one dwell carries
   whole frames; reuse the A.3 block codec boundaries for resume.
4. **Application ACKs over ESP-NOW during the dwell.** Credits/retry
   decisions travel bridge↔SomnoTrace on H in the same excursion window,
   rather than round-tripping over BLE mid-burst. BLE remains for setup,
   verdicts, and fallback only.
5. **Channel-match fast path.** When H happens to equal E, skip
   excursions entirely (zero cost, zero risk); SomnoTrace reports H
   over BLE and the bridge adapts automatically — including after a
   router roam. Never a card write.
6. **Policy split by payload.** Nightly deltas (~0.8 MB encoded) fit
   compressed BLE in seconds and need no excursion risk; reserve A3 for
   month-scale backfill/cold sync where burst throughput pays.
7. **Interop matrix as a tracked artifact.** 5.5.1↔5.5.5 BLE
   (bonds/reconnect), ESP-NOW v2 encrypted unicast both directions, and
   bond persistence across a bridge reflash — recorded once, rerun on
   every SDK bump.

Options considered and **not** better: WiFi Aware/NAN (adds cluster and
discovery complexity on the same single radio and channel constraint),
BLE connection subrating (unsupported on S3), broadcast ESP-NOW (no
CCMP), plaintext ESP-NOW (rejected above), a second BLE connection (no
new bandwidth), and switching to Bluedroid for the non-standard 3.75 ms
interval (heavier stack, worse fit for large payloads — A.9.2).

### B.6 Revised decision flow with A3

1. BLE-only optimization on **v5.5.1, both boards** (no upgrade): remove
   fixed latency, fix credits/pacing, add compression, benchmark. This
   remains the default path.
2. If production-load throughput still falls short: bridge-only upgrade
   to v5.5.5 and trial **A3** under B.3/B.4 rules, SomnoTrace remaining
   on v5.5.1 as receiver. Provisioning is one BLE op over the existing
   bond (B.4); disconnect-freedom is proven by the soak in B.3.
3. Same-channel ESP-NOW (H = E) remains a free subset when channels
   match. A1 (card rewrite) and A2 (explicit disconnect/hop) stay behind
   explicit opt-in. Home-AP TCP and a second radio remain last resorts.

Acceptance gates specific to A3: zero beacon-timeout/disconnect events
over a wide soak; provisioning completes with no user interaction and no
new passkey; mixed-version matrix green; first off-channel crash or
association loss permanently disables the accelerator with automatic
BLE fallback and no user-visible failure.

### B.7 References

- **[B1]** SomnoBridge `scripts/idf.sh` pins `espressif/idf:v5.5.1`
  (local file, verified this review).
- **[B2]** SomnoTrace `scripts/idf.sh` pins `IDF_TAG:-v5.5.1`; no
  `esp_now` usage exists anywhere in its sources (local verification).
- **[B3]** v5.5.1 `esp_now.h` — off-channel APIs absent; v2 1470-byte
  frames, per-peer LMK/CCMP, PMK present:
  https://raw.githubusercontent.com/espressif/esp-idf/v5.5.1/components/esp_wifi/include/esp_now.h
- **[B4]** API introduction commit (merged to release/v5.5, hence
  v5.5.2+):
  https://github.com/espressif/esp-idf/commit/d14ce1f4ce7a9cb23ff3c93ac40f6f210ac6c510
- **[B5]** ESP-NOW/WiFi off-channel API semantics and state restrictions
  (v5.5.5 S3 documentation):
  https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/network/esp_now.html
  https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/network/esp_wifi.html
- **[B6]** Beacon-timeout mechanism and `esp_wifi_set_inactive_time`:
  https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/wifi-driver/overview.html
  (Wi-Fi Beacon Timeout; WIFI_EVENT_STA_BEACON_TIMEOUT).
- **[B7]** v6.1 fix for the ESP-NOW off-channel TX crash (release body,
  commit 3154f66e):
  https://github.com/espressif/esp-idf/releases/tag/v6.1

Cross-references: A.2 (corrections), A.4 (priorities), A.9.4 (A3
candidate), A.9.5 (fair SDK testing), [R6]/[U3] (coexistence),
[R8]/[U8]/[U9] (ESP-NOW, off-channel APIs and PSRAM fix).

---

## Appendix C — recommendation: exhaust compressed BLE, then a constrained A3 trial

This appendix is a fresh review of the whole document against the two
preferred options (squeeze the existing BLE link; try off-channel
ESP-NOW only if it neither disconnects nor adds a pairing ceremony).
It does not repeat B.2's version matrix except where the recommendation
depends on it. Nothing earlier was modified.

### C.1 Agreement

**Agree with the document, and with Appendix A's BLE-first correction
of the main text.** The measurements, layer analysis, and honesty about
what is still arithmetic rather than on-device are sound. Specifically:

- 2M PHY + DLE + a shorter interval is the largest *measured* BLE gain
  (19 → 46–52 KiB/s on the best runs). That work stays.
- Airtime/backpressure, not our credit window, dominated the slow runs.
  `stall_ms=0` is real. Attributing every slow run to "the radio" is
  not: A.2.2 is right that `retry_ms` collapses several failure classes.
- Lossless delta16+deflate on the real files is the credible way for
  BLE to clear 100 KiB/s **effective** (restored bytes), if encoded
  throughput stays near the best measured link. It is not a raw-link
  claim.
- A1 (card rewrite) and A2 (explicit disconnect/hop) are correctly
  behind opt-in. They are not the path that satisfies "no disconnects".
- Both boards staying on v5.5.1 is enough for the BLE squeeze. Only
  the off-channel *sender* needs ≥ v5.5.2; v5.5.5 on SomnoBridge is the
  right pin if A3 is tried. SomnoTrace does not have to move with it
  (B.2).

**Do not treat the main text's "ESP-NOW is the recommendation" as the
current recommendation.** Appendix A already superseded it. This
appendix agrees with A, not with that earlier closing sentence.

### C.2 The two constraints, stated tightly

**No new pairing ceremony: yes, with encryption kept.**

"No new encryption" cannot mean plaintext medical payloads. It can mean
no second user-facing pairing. B.4 is the right shape: one control op
on the existing encrypted BLE bond, random PMK/LMK, encrypted NVS,
rotate on `FORGET_PEER`. The user never sees a passkey, button prompt,
or portal step for ESP-NOW. That is seamless *provisioning*. It is still
a spec change (new op, new secret, new transport).

**No disconnects: possible only as a measured property of short dwells,
not as a property of the API name.**

`esp_now_switch_channel_tx()` sends **one** ESP-NOW frame after a
channel visit. `esp_now_remain_on_channel()` dwells. Neither is a
disconnect call, and neither promises that the EZShare association
survives a long or frequent absence. While the bridge radio is on H it
cannot hear the card on E. Beacon timeout is a real disconnect path
(B.3). A v6.1 note also records an off-channel TX crash that 5.5.5 may
not contain.

So A3 is allowed only if a soak shows:

- zero `WIFI_EVENT_STA_DISCONNECTED` and zero beacon-timeout events;
- every required 5 s poll issued and completed inside its deadline;
- automatic return to E even if the excursion is cancelled or fails;
- BLE bond and SomnoTrace's other links undisturbed.

The first failed soak disables the accelerator permanently for that
firmware build and falls back to BLE. Do not retry-loop a path that
drops the card association.

### C.3 What I would actually do

**Ship order, not a menu.**

1. **BLE squeeze on v5.5.1, both boards. Do this first and finish it.**
   No SDK move. In order: monotonic send/auth deadlines; synchronized
   contiguous credits; silence per-PDU notify logs on performance runs;
   core split (WiFi off the BT core); 2M + DLE 251 + interval sweep on
   the real SomnoTrace central; negotiated delta16+deflate blocks
   (64 KiB baseline) with SHA-256 over raw bytes. Avoid a flash cache
   in this increment (A.4).
2. **Measure the production central before believing the host numbers.**
   The AX200 rig is a noisy lower bound and has wedged under notify
   load. Accept or reject the BLE plan on two S3s with sensors and home
   WiFi live. Target: ≥50 KiB/s encoded and ≥100 KiB/s restored on
   representative BRP, median and lower percentile, not the best run.
3. **Only if (2) misses the target:** upgrade **SomnoBridge alone** to
   v5.5.5 and run a bounded A3 experiment. SomnoTrace stays on v5.5.1
   unless its own maintenance wants otherwise. Provision exactly as
   B.4. Dwells start in the tens of milliseconds, only in the idle gap
   between reserved polls, encrypted v2 unicast only. Same-channel
   (H = E) is the free fast path and needs no excursion.
4. **Do not implement A1 or A2** unless a later product decision
   explicitly accepts card writes or monitoring gaps. Home-AP TCP and
   a second radio stay last resorts, as in A.5 and B.6.

This is the smallest path that can hit the speed goal without betting
the 5 s poll on an unproven driver feature.

### C.4 Options not worth adding

Nothing in B.5 is wrong. Two of them should not be treated as peer
recommendations:

- **Application ACKs over ESP-NOW during the dwell** fights the
  one-frame `switch_channel_tx` contract and the "stay off E as little
  as possible" rule. Keep setup, credits, and verdicts on the BLE
  control plane until a ROC burst is proven able to carry a whole
  window *and* its ACK without missing a poll. Do not design a second
  reliable protocol before that measurement.
- **Policy split (nightly on BLE, backfill on A3)** is a reasonable
  later optimization, not a reason to build A3 before (1) and (2).
  If compressed BLE already clears a night in ~15 s of airtime, A3's
  complexity is only justified by cold/month backfill *and* a failed
  BLE benchmark.

Not better than the plan above, and not newly available: WiFi NAN,
plaintext or broadcast ESP-NOW, a second BLE connection, Bluedroid for
a non-standard 3.75 ms interval, or an IDF upgrade sold as a new S3
modulation. v6.x remains a migration, not a PHY upgrade (A.9.2).

### C.5 Recommendation in one paragraph

Squeeze BLE on the SDK both boards already run, with block compression,
until two ESP32-S3s under production load either clear ~100 KiB/s
restored or prove they cannot. That path needs no upgrade and no new
pairing. If it falls short, try off-channel ESP-NOW on a v5.5.5
SomnoBridge only, provisioned over the existing bond, and keep it only
if a soak shows zero association loss and zero missed polls. Until that
soak passes, A3 is an experiment, not a seamless transport. A1 and A2
do not meet the no-disconnect constraint and should stay opt-in.

### C.6 References

No new external sources. This appendix relies on A.2–A.9, B.2–B.6, and
the v5.5.5/v6.1 ESP-NOW contracts already cited as [U8] and [B5]:
`esp_now_switch_channel_tx()` sends one frame per visit;
`esp_now_remain_on_channel()` is a bounded dwell, not a second radio.

---

## Appendix D — final consolidated recommendation (BLE-only plan and aggressive option)

This appendix re-validates the main document and Appendices A–C as a
whole, corrects what no longer holds, and gives the final recommendation
for **maximum download speed that stays seamless and does not degrade any
other BLE or WiFi connection** on either board. It gives two concrete
options: a pure-BLE plan with exact adjustments, and an aggressive
off-channel ESP-NOW option with a rough benefit estimate. Nothing earlier
in the document was modified. No firmware, configuration, or SDK change
was made for this review.

Validation basis: current SomnoBridge source and effective `sdkconfig`
(HEAD `f32ed17`), SomnoTrace source and `sdkconfig.defaults`, ESP-IDF
v5.5.1/v5.5.5/v6.1 headers and documentation, and GitHub commit-ancestry
checks for the ESP-NOW fixes (D.8).

### D.1 Validation ledger

**Holds (verified or consistent with evidence):**

| Claim | Where | Status |
|---|---|---|
| 2M PHY + DLE + shorter interval is the largest measured gain (19 → 46.3 → 51.6 KiB/s) | §1, §5 | Measured. Code requests these on connect (`ble_svc.c`). |
| The 20 ↔ 50 KiB/s variance at identical link parameters is unexplained | §1, A.2.2 | Still true. Coexistence remains the leading suspect (D.2). |
| Delta16 + deflate gives ≈2.5–2.7× byte-exact on the real month set; 64 KiB independent blocks ≈2.5–2.6× | §2.3, A.3 | Verified on 95 files (A.3). |
| Peripheral→central notify is the right GATT method; indications are not used for data | §2.1 | Verified in code. |
| `NOTIFY_TX` is a submission event, not controller buffer release | A.2.3 | Verified in pinned NimBLE (A.8 [R2]); also unchanged in v5.5.5/v6.1 (A.9.3). |
| Credit logic needs contiguous-ACK semantics before going faster | A.2.4 | Verified in `ble_ops.c` (`window` shared, no right edge). |
| Fixed per-file costs: 2.5 s settle; `STALE` ≈12 s per day boundary | §1, §2.5 | Verified: `vTaskDelay(pdMS_TO_TICKS(2500))` in `ble_ops.c`. |
| Month sweep was ≈16.4 min completion span, not 35 min | A.2.8 | Accepted. |
| No faster-than-2M BLE PHY on S3; Frame Space Update, Shorter Connection Intervals, Connection Subrating are unsupported on S3 | A.9.2, B.5 | Verified in the v6.1 S3 feature table. |
| ESP-NOW TX coexists stably with BLE connected; ESP-NOW RX is stable in STA mode | B.3 | Verified in the v5.5.5 S3 coexistence table. |
| STA beacon timeout default is 6 s, then 5 probes, then disconnect | B.3 | Verified in the WiFi API/driver docs. |
| SomnoTrace already runs `WIFI_PS_NONE` | (new) | Verified (`net_provision.c`). Good for ESP-NOW reception. |
| Off-channel APIs are absent in v5.5.1, present from v5.5.2 | A.9.4, B.2 | Verified (API commit `d14ce1f4` is an ancestor of v5.5.2 and v5.5.5, not v5.5.1). |

**Corrections to earlier sections (including my own Appendices B and C):**

1. **C.2 is wrong about the off-channel crash fix.** "fix the crash issue
   when using espnow off channel tx" was merged into `release/v5.5` as
   `3154f66e` (2025-10-27). It is contained in **v5.5.2 and v5.5.5**,
   not only v6.1. B.7's [B7] should cite this v5.5 merge as well.
2. **B.2's "≥ v5.5.2" minimum is too low for this bridge.** The fix
   "offchan tx fail when `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` enabled"
   (`897cf407`) is **absent from v5.5.2, v5.5.3 and v5.5.4** and present
   in **v5.5.5**. The bridge has `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`,
   and turning it off would push WiFi/lwIP buffers back into the internal
   RAM that already starved NimBLE. **For off-channel ESP-NOW, v5.5.5 is
   the minimum 5.5.x for SomnoBridge, not merely recommended.**
3. **The bridge cannot set connection-event length.** As peripheral it
   can only send a connection-parameter request, and neither the L2CAP
   nor the LL parameter-request procedure carries CE length. The
   `max_ce_len = 0xFFFF` in `ble_svc.c` has no effect. CE length is
   decided **only by SomnoTrace as central** (`ble_gap_connect` /
   `ble_gap_update_params`, plus its controller's
   `BT_CTRL_CE_LENGTH_TYPE`).
4. **SomnoTrace has no SomnoBridge client yet** (no references in its
   source). Every central-side tuning item below is new code, not a
   change to existing code. It currently allows only two BLE connections
   (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`, `BT_CTRL_BLE_MAX_ACT=4`), both
   used by AS11 and the oximeter.
5. **The main document's P3 recommendation ("ESP-NOW data plane, A1 as
   steady state") is superseded.** A1 writes to the patient's card and
   A2 creates monitoring gaps; neither meets the "no impact on other
   connections" goal. Appendices A–C already moved away from it; this
   appendix confirms that.
6. **§5 "uncommitted" is historical.** The tuning code is in HEAD
   `f32ed17` (A.2.12 already noted this).

### D.2 Hard limits that bound every option

These apply whatever we implement and set the realistic ceiling:

- **Both boards keep WiFi associated, so BLE is time-sliced on both
  ends.** With WiFi CONNECTED and BLE CONNECTED, the S3 coexistence
  scheme gives WiFi and BLE about 50 % each of a ≥100 ms period aligned
  to the AP's beacon (TBTT). BLE can still use the radio in the WiFi
  slice when WiFi is idle, but only at lower priority. The bridge's
  slices follow EZShare's beacons; SomnoTrace's follow the home AP's.
  The two schedules are unrelated, so a connection event can be cut
  short by **either** side. This is the most plausible explanation for
  the 20 ↔ 50 KiB/s swings. It also means that Espressif's
  WiFi-off throughput figures (e.g. C6 CoC ≈1310 kbps at 2M) are
  **not** reachable here. Disabling coexistence, or disconnecting either
  WiFi link, is excluded by the goal.
- **ESP32-S3 BLE tops out at 2M PHY.** No SDK version changes that
  (A.9.2).
- **The EZShare card's HTTP rate has never been measured (M1).** In a
  cold transfer (not staged in advance), every restored byte must first
  come off the card. If the card sustains less than the target restored
  rate, **both** options are card-bound.
- **SomnoTrace's sensor links share its radio.** AS11 and oximeter
  connection events take priority over bulk transfer. Transfers must
  never cause their supervision timeouts or missing samples.
- **Bridge internal RAM is the scarcest resource.** Anything that adds
  internal-RAM pools risks the NimBLE host-task and pairing failures
  already fixed once. Buffers for compression and staging go to PSRAM.

### D.3 Option 1 — pure BLE, maximised (recommended first)

No SDK upgrade on either board (both stay on v5.5.1). No new radio,
keys, or pairing. Connections preserved by design. Grouped by layer.
Values are starting points to verify with the counters in step D.3.6.

#### D.3.1 SomnoBridge configuration (`sdkconfig.defaults`)

| Setting | Now | Change to | Why |
|---|---|---|---|
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` | 160 | **240** | Headroom for delta + deflate, SHA, and the NimBLE host on the same chip. |
| `CONFIG_BT_NIMBLE_LOG_LEVEL` | INFO (1) | **WARNING** | NimBLE logs every notification at INFO ("GATT procedure initiated: notify"). At hundreds of notifications per second that is pure overhead in the hot path. |
| WiFi task core (`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_*`) | Core 0, same as NimBLE host and controller | **Core 1** (pin the stream/compress worker to core 1 too) | Espressif recommends keeping WiFi off the BT core. No extra RAM. |
| `CONFIG_LWIP_TCP_WND_DEFAULT` / `SND_BUF` | 5760 | **≈16384**, allocated in PSRAM (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is already on) | Today a 4-MSS window throttles the card on cold transfers. Validate with M1. |
| `CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT` | 32 | **Keep 32** until pacing is fixed; then raise to 48 only if `ENOMEM` send failures remain **and** `lrg_i` stays healthy | Pools are internal RAM. More buffers do not help while the controller cannot drain them. |
| `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` | 247 | **247 first**; later try **498** (two full 251-byte LL PDUs) after `frame[256]` / `frame[260]` and all reassembly limits are resized | 498 amortises ATT and frame headers over two LL PDUs. Expect a few percent, not a doubling (A.2.5). |

Keep the existing connect-time requests (2M PHY, DLE 251/2120, 7.5–15 ms
interval, latency 0, 4 s supervision timeout). Log
`BLE_GAP_EVENT_DATA_LEN_CHG` to confirm DLE actually landed (M6).

#### D.3.2 SomnoBridge stream path (`ble_ops.c`)

1. **Always send full notifications.** Carry the remainder of each 4 KiB
   HTTP read into the next frame instead of sending a short final
   notification at every chunk boundary.
2. **Shrink the per-frame header.** 16 of every 244 bytes (6.6 %) are
   header. The offset is implied by in-order delivery, so a 2–4 byte
   data-frame header with a sequence counter (full offset carried in
   periodic or control frames) gives ≈ +5 % at no risk.
3. **Pace by deadline, not by a submission event.** On `ENOMEM`, yield
   about 1 ms with a monotonic-time deadline (keep the ≈2 s send and
   500 ms authorisation bounds as real times). Drop the `NOTIFY_TX` wake
   (A.2.3). Aim to keep about two connection events' worth of
   notifications (≈16–20) queued at all times.
4. **Fix credits:** contiguous-ACK right edge, synchronised updates, ACK
   every ~¼ window, window counted in encoded bytes (A.2.4).
5. **Remove fixed latency:** replace the 2.5 s settle with an
   event-driven post-check; add a batch request (`GET_FILES` manifest)
   that pre-refreshes all involved days, which removes `STALE` retries;
   reuse the HTTP connection where the card allows it.
6. **Compress on the fly:** delta16 + ROM `tdefl` level 1–3, 64 KiB raw
   blocks, state in PSRAM, per-block header (codec, raw/encoded length,
   CRC), raw fallback when a block does not shrink, SHA-256 over the raw
   file unchanged (A.3). This needs a spec revision (A.2.12).
7. **Optional PSRAM staging:** prefetch the next file or block while the
   current one is on air. This overlaps card reads with radio time
   without a flash cache or data at rest (A.4).

#### D.3.3 SomnoTrace central (new client code)

| Item | Value | Why |
|---|---|---|
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` | 2 → **3** | AS11 + oximeter + SomnoBridge. |
| `CONFIG_BT_CTRL_BLE_MAX_ACT` | 4 → **≥5** | 3 connections + scanning + a spare. |
| `CONFIG_BT_CTRL_CE_LENGTH_TYPE` | ORIG → test **CE** ("Use CE parameter for HCI command") | Makes the controller honour `min_ce_len`/`max_ce_len`. A/B against ORIG. |
| Connect parameters for the bridge | `itvl_min = itvl_max = 12` (15 ms) as the default; sweep 7.5 / 15 / 30 ms; latency 0; `supervision_timeout = 400` | 15 ms leaves room for the other links; 7.5 ms only if the sweep shows a gain with sensors connected. |
| CE length (units of 0.625 ms) | `max_ce_len ≈ 20` (12.5 ms of a 15 ms interval) when no sensor link is active; **≈12** (7.5 ms) while AS11/oximeter are connected; update with `ble_gap_update_params` when that changes | Long events for bulk throughput, without starving sensor events. |
| After connect | `ble_gap_set_prefered_le_phy(2M)`, `ble_gap_set_data_len(251, 2120)`, MTU exchange 247 (498 later) | The central should request these too, not rely on the bridge. |
| Receive path | Notify callback copies into a PSRAM ring and returns; a worker does inflate → SHA-256 → SD `.part`; grant credits from free ring space | The NimBLE host task (core 1 on SomnoTrace) must never block on SD or inflate. |
| Scheduling | Run the bridge pull inside the post-therapy flow, **before** SMB/SleepHQ uploads; never pause sensor acquisition | Avoids competing with SomnoTrace's own WiFi uploads, and needs no user-visible "transfer window". |

SomnoTrace already has the right core split (NimBLE host on core 1,
WiFi on core 0), 240 MHz, and `WIFI_PS_NONE`.

#### D.3.4 What the BLE plan does not do

No L2CAP CoC in the first increment: it removes only ATT and frame
headers (≈7–10 %), costs pools on the RAM-starved bridge, and is
superseded by the compact header in D.3.2. No 3.75 ms vendor interval,
no Bluedroid, no flash cache, no coexistence disabling, no pausing of
oximetry. An SDK A/B (v5.5.5) is allowed as a later experiment (A.9.5),
but nothing in D.3 depends on it.

#### D.3.5 Expected BLE results

Assumptions: aggregate compression 2.5× (block-wise level 1–3, A.3);
per-file overhead after batching ≈0.3 s; month set 8,011,138 B
(≈7,823 KiB, 95 files); a night ≈2 MB (≈1,953 KiB, ≈15 files). Encoded
link range 45 / 55 / 75 KiB/s (conservative / nominal / optimistic)
under two-sided coexistence. Cold rates assume the card sustains at
least the restored rate (M1).

| Step (cumulative) | Encoded link | Restored rate | Month set | One night |
|---|---:|---:|---:|---:|
| Measured today | 20–52 KiB/s raw | same | ≈16.4 min (completion span) | 194 s for 2.19 MB (measured) |
| + fixed-overhead removal (D.3.2 #5) | 20–52 | same | ≈3–7 min | ≈45–100 s |
| + pacing, logging, core split, 240 MHz, full frames, compact header | 45–75 | same | ≈2–3 min | ≈30–45 s |
| + delta16 + deflate | 45–75 | **≈110–190 KiB/s** | **≈1.2–1.7 min** | **≈15–22 s** |
| + incremental sync (only new files) | — | — | one-time backfill | ≈15–22 s per night |

Net effect: **about 10× faster wall-clock than today** for a month or a
night, with no new radio, keys, or SDK. Raw (uncompressed) link speed
of 100 KiB/s is **not** a realistic target with both WiFi links
associated. The 100 KiB/s goal is met on restored bytes.

#### D.3.6 BLE acceptance gates

On two real S3s, with both WiFi links associated, AS11 and oximeter
connected, and no serial reader attached:

- Encoded link ≥ 45 KiB/s median, ≥ 35 KiB/s at the 10th percentile.
  Restored rate ≥ 100 KiB/s on representative BRP files.
- Zero sensor-link supervision timeouts, zero missed oximeter samples,
  every 5 s card poll issued and completed on time, zero reboots, and
  `lrg_i` healthy throughout.
- Every file passes byte count, raw SHA-256, source tuple and
  `completion_allowed`.
- Log per run: PHY, DLE, interval, CE, `ENOMEM` count, credit stalls,
  queue depth, and card read rate.

### D.4 Option 2 — aggressive: off-channel ESP-NOW data plane ("A3")

#### D.4.1 Shape

- **SomnoTrace never moves.** It stays associated to its home AP on
  channel H and receives encrypted ESP-NOW unicast there (supported in
  v5.5.1; RX is "stable in STA mode" per the coexistence table;
  `WIFI_PS_NONE` already set, so frames are not missed in modem sleep).
- **SomnoBridge stays associated to EZShare on channel E.** It makes
  short, driver-managed visits to H (`esp_now_remain_on_channel` plus
  `esp_now_switch_channel_tx`), never calling `esp_wifi_disconnect`.
- **BLE stays up throughout as the control plane:** pairing,
  manifest, per-block ACK/NACK, final verdicts, fallback. While an
  ESP-NOW burst runs, the bridge can request a longer BLE interval
  (e.g. 50 ms) to free airtime, then return to 15 ms.
- **Provisioning is seamless (B.4):** one new control op over the
  existing encrypted BLE bond sets random PMK/LMK on both ends. No new
  passkey, button, or portal step. The data stays CCMP-encrypted.
  Plaintext ESP-NOW is never used.
- **Data is pre-compressed (D.3.2 #6) and preferably staged in PSRAM**,
  so each visit to H spends its time transmitting, not waiting on the
  card. Roughly 1 MB of free PSRAM holds about one compressed night
  (≈0.8 MB). Measure the actual free SPIRAM before relying on this; if
  it is short, stage per file instead.

#### D.4.2 Why it can stay connection-safe

- **Association:** the default STA inactive time is 6 s before the
  probe/disconnect sequence starts. Visits of 20–40 ms, returning to E
  every ~50–100 ms, never come close. The real constraint is card
  HTTP and timing, not association.
- **Card polls:** no visits during a reserved window around each 5 s
  poll (e.g. 500 ms). Optional prefetch is suspended during bursts.
- **BLE links:** ESP-NOW TX with BLE connected is a supported, stable
  combination. SomnoTrace's sensor links keep coexistence priority.
- **Failure policy:** any `WIFI_EVENT_STA_BEACON_TIMEOUT`, STA
  disconnect, late poll, off-channel API error, or sensor-link timeout
  disables the accelerator for that firmware build and falls back to
  BLE. No retry loops.

#### D.4.3 Prerequisites

| Board | SDK | New code |
|---|---|---|
| SomnoBridge | **v5.5.5 required** (D.1 correction 2) | ESP-NOW sender, visit scheduler, PSRAM staging, provisioning op, fallback state machine. |
| SomnoTrace | **v5.5.1 is sufficient** | ESP-NOW receiver (encrypted peer on its current channel), reassembly into the same block pipeline as BLE, provisioning op. |

Both sides also need the spec revision for compression and batching
(A.2.12), plus the new transport and stored key.

#### D.4.4 The unknown that decides the benefit

The public contract of `esp_now_switch_channel_tx()` is "switch channel,
send **one** ESP-NOW frame, wait". Two things are not documented and
must be measured before estimating anything with confidence:

1. **Frames per visit.** Can many frames go out during one
   `remain_on_channel` dwell without paying a channel switch per frame?
2. **PHY rate off-channel.** Do off-channel frames honour
   `esp_now_set_peer_rate_config()` (e.g. 24–54 Mbps OFDM / HT MCS), or
   go out at the 1 Mbps default? At 1 Mbps a 1470-byte frame takes
   ≈12 ms of airtime. That alone would cap A3 at about BLE speed.

Reference points for on-channel ESP-NOW (no channel switching): the
ESP FAQ reports ≈214 kbps (≈26 KiB/s) at the default 1 Mbps in an open
environment. A third-party ESP32 test with 250-byte frames measured
≈760 KB/s unicast at 54 Mbps / MCS4. 1470-byte v2 frames amortise
per-frame overhead better, but encryption, coexistence on the receiver,
and channel switching cut in the other direction.

#### D.4.5 Rough benefit estimate

Model: encoded rate = G × d, where G is the encoded goodput while the
bridge is on H (after switch overhead, retries and CCMP) and d is the
fraction of time the bridge spends on H. Restored rate = 2.5 × encoded.
For **cold** transfers the same bridge radio must also read the card on
E, so the restored rate is 1 / (1/card + 1/restored). Card rates of 150
and 300 KiB/s are **placeholders** until M1 measures the real card. The
BLE row treats card reads and BLE as overlapping, which is optimistic
for BLE too: both share the radio through coexistence.

| Case | G | d | Encoded | Restored, staged (warm) | Restored, cold, card 300 KiB/s | Restored, cold, card 150 KiB/s |
|---|---:|---:|---:|---:|---:|---:|
| Conservative (1 Mbps off-channel, or one switch per frame) | 100 KiB/s | 0.3 | 30 KiB/s | 75 KiB/s | 60 KiB/s | 50 KiB/s |
| Nominal | 400 KiB/s | 0.5 | 200 KiB/s | 500 KiB/s | ≈190 KiB/s | ≈115 KiB/s |
| Optimistic | 900 KiB/s | 0.7 | 630 KiB/s | ≈1,575 KiB/s | ≈250 KiB/s | ≈135 KiB/s |
| **Option 1 (BLE), for comparison** | — | — | 45–75 KiB/s | ≈110–190 KiB/s | ≈110–190 KiB/s (card-bound if slower) | ≤150 KiB/s |

What that means in time, nominal case:

| Workload | Option 1 (BLE) | Option 2 (A3) | Speed-up | Time saved |
|---|---:|---:|---:|---:|
| One night, staged in PSRAM | ≈15–22 s | ≈6–9 s | **≈2–3×** | ≈8–13 s per night |
| Month backfill, cold (card 300 KiB/s) | ≈1.2–1.7 min | ≈1.0–1.2 min | **≈1–1.5×** | ≈0–30 s, once |
| Month backfill, cold (card 150 KiB/s) | ≈1.3–1.7 min | ≈1.5–1.6 min | ≈1× | ≈0 |
| Conservative case, any workload | — | slower than BLE | <1× | none; reject A3 |

Times include the same ≈0.3 s per-file overhead as D.3.5. The
optimistic row would bring a staged night to ≈6 s and a cold month
(card 300 KiB/s) to ≈1 min.

**Bottom line for A3:** a plausible **2–3× over optimised BLE for
staged nightly data** (more only if per-file overhead also shrinks), but
only ≈1–1.5× for cold month backfills, where the card becomes the
limit. In absolute terms, once BLE is optimised and
incremental sync is in place, it saves **seconds per night**. If the
off-channel API turns out to send one frame per switch or at 1 Mbps, it
brings **no benefit at all**.

#### D.4.6 A3 go/no-go probe (before any product work)

A short bench spike on two S3s, bridge on v5.5.5, H ≠ E, both WiFi
associated, BLE connected:

1. Measure frames per `remain_on_channel` dwell, per-visit overhead, and
   whether the per-peer rate applies off-channel. Encrypted v2 unicast
   only.
2. Proceed only if G ≥ 300 KiB/s encoded during visits **and**
   G × d ≥ 2 × the measured Option 1 encoded rate.
3. Then run an 8 h soak with continuous bursts: zero beacon timeouts,
   zero STA disconnects, every poll on time, zero sensor-link timeouts
   on SomnoTrace, zero reboots.
4. If any gate fails, stop. Option 1 stands alone.

### D.5 Other options, final disposition

| Option | Verdict | Reason |
|---|---|---|
| **A0 — same channel by user choice (H = E)** | **Worth supporting as a free fast path** | If the user's router is (or is set to) the card's channel, plain same-channel ESP-NOW works with no visits, on **v5.5.1 on both boards**. No card writes and no disconnects, d ≈ 0.8–0.9. The firmware can detect H = E automatically from the channel SomnoTrace reports over BLE. It still needs the A3 receiver, provisioning, and fallback code. |
| A1 — rewrite EZShare channel | Rejected as default | Card writes to the patient's SD; possible AP restart; needs explicit opt-in (A.2.9). |
| A2 — bridge disconnects and hops | Rejected | Deliberate monitoring gaps; violates the goal (A.2.10). |
| Bridge joins home WiFi, TCP | Rejected as default | Monitoring gap, needs home credentials on the bridge (A.5). |
| SomnoTrace temporary SoftAP | Rejected | SoftAP + BLE coexistence is "unstable" (C1) per Espressif; bridge must still leave EZShare. |
| L2CAP CoC | Deferred | ≈7–10 % at a RAM cost; compact header gets most of it (D.3.4). |
| 3.75 ms interval / Bluedroid | Rejected | Non-standard, poor fit for large payloads (A.9.2). |
| v6.0.3 / v6.1 | Deferred | Migration cost, no new S3 PHY; benchmark only if v5.5.5 shows SDK-sensitive gains. |
| Flash content cache | Deferred | Data at rest, wear, invalidation; PSRAM staging covers the need (A.4). |
| Second WiFi radio on the bridge | Last resort | Only true simultaneous dual-channel option; hardware change. |

### D.6 Final recommendation

1. **Implement Option 1 now, on v5.5.1 on both boards.** In order:
   - D.3.1 bridge configuration (240 MHz, NimBLE log WARNING, WiFi on
     core 1);
   - D.3.2 #3–#5 (deadline pacing, contiguous credits, no fixed
     sleeps, batch with pre-refresh);
   - D.3.3 SomnoTrace client (3 connections, CE mode, 15 ms with
     adaptive CE, post-therapy scheduling);
   - D.3.2 #1–#2 (full frames, compact header);
   - D.3.2 #6 compression, after the spec revision.

   Expected: ≈110–190 KiB/s restored, about 10× faster than today, with
   no new keys, pairing, SDK, or effect on sensor or WiFi links.
2. **Measure M1 (card rate) and the D.3.6 gates on two S3s** before
   deciding anything further. The card rate determines whether any
   faster transport can help cold transfers at all.
3. **Only if a faster nightly sync is still worth it:** run the D.4.6
   probe with SomnoBridge alone on v5.5.5. Adopt A3 only if it passes
   every gate, and use it for staged nightly data and large backfills,
   with BLE as the always-available fallback. Support A0 whenever the
   channels already match.
4. **Do not implement A1, A2, home-AP TCP, or SoftAP variants** under
   the current goal.

### D.7 What remains unverified

- The real EZShare HTTP rate (M1); the cold-transfer numbers in D.3.5
  and D.4.5 depend on it.
- Two-board BLE throughput with both WiFi links associated and sensor
  links connected; D.3.5's 45–75 KiB/s encoded range is an estimate
  bracketed by our 20–52 KiB/s host-rig results.
- On-device ROM `tdefl` speed and memory (M3).
- Off-channel ESP-NOW frames per visit and PHY rate (D.4.4); every A3
  number is arithmetic until then.
- Free PSRAM on the bridge during normal operation, which determines
  how much can be staged.

### D.8 References

- **[D1]** ESP32-S3 coexistence (v5.5.5): supported-scenario table
  (ESP-NOW TX "Y", RX "S"), 50/50 slices under WiFi CONNECTED + BLE
  CONNECTED, periods aligned to TBTT:
  https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-guides/coexist.html
- **[D2]** `esp_now.h` v5.5.5: one frame per `esp_now_switch_channel_tx`,
  `esp_now_remain_on_channel`, per-peer rate config, peer channel must
  equal the current channel for `esp_now_send`:
  https://raw.githubusercontent.com/espressif/esp-idf/v5.5.5/components/esp_wifi/include/esp_now.h
- **[D3]** ESP-NOW guide v5.5.5 (send-callback pacing, CCMP, 1 Mbps
  default, v2 1470 bytes):
  https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/network/esp_now.html
- **[D4]** STA inactive time default 6 s and beacon-timeout sequence:
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_wifi.html
- **[D5]** Off-channel TX crash fix merged to `release/v5.5` (contained
  in v5.5.2 and v5.5.5, not v5.5.1; verified with the GitHub compare API):
  https://github.com/espressif/esp-idf/commit/3154f66e8934a71336c41349b4ff837023c2987f
- **[D6]** Off-channel TX fix with `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`
  (contained in v5.5.5; absent from v5.5.1–v5.5.4; verified with the
  GitHub compare API):
  https://github.com/espressif/esp-idf/commit/897cf407aa70586c15cf8c6b3fe9458066cf7fce
- **[D7]** S3 BLE feature status (2M/DLE supported; Subrating, Frame
  Space Update, Shorter Connection Intervals unsupported):
  https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/ble/ble-feature-support-status.html
- **[D8]** S3 controller CE-length modes (ORIG / CE / SD):
  https://raw.githubusercontent.com/espressif/esp-idf/v5.5.1/components/bt/controller/esp32c3/Kconfig.in
- **[D9]** NimBLE throughput examples v5.5.5 (GATT notify pipelining,
  MSYS 50, MTU 512; CoC 2M ≈1310 kbps on ESP32-C6 with WiFi off):
  https://raw.githubusercontent.com/espressif/esp-idf/v5.5.5/examples/bluetooth/nimble/throughput_app/README.md
- **[D10]** ESP-NOW throughput reference points: ESP FAQ (≈214 kbps at
  1 Mbps, open environment) and a third-party ESP32 rate sweep
  (250-byte frames):
  https://github.com/espressif/esp-faq/blob/df711af/docs/en/application-solution/esp-now.rst
  https://blogs.ntu.edu.sg/ps9888-2020-g16/2020/07/11/esp-now-data-transfer/
- Local: `components/ble_api/ble_svc.c`, `ble_ops.c`, effective
  `sdkconfig`; SomnoTrace `sdkconfig.defaults`, `main/net_provision.c`,
  `main/as11_ble.c`, `main/oximeter_*.c`, `main/post_therapy.c`.

