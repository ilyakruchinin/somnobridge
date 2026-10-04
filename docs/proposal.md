# Proposal — Measuring AS10 Clock Drift Through the EZShare Card

> **Status:** recommendation, not implementation. Builds on the problem statement in
> [`as10-time-drift.md`](as10-time-drift.md). Every accuracy figure below is a
> **first-principles estimate**, not a measurement — §9 lists the experiments that
> confirm or kill each assumption, in the order they should be run.

---

## 1. Recommendation in one page

**Use the AS10's own 60-second flush as a clock, and make SomnoBridge watch it with a
phase-locked poller.**

1. During therapy the AS10 appends one 60 s EDF record to each open file about once a
   minute and updates the FAT directory entry (**size** and **mtime**). Each flush is a
   physical event that (a) SomnoBridge can timestamp against NTP-derived time and
   (b) carries an AS10-RTC stamp (the mtime). Every flush is therefore one
   *(AS10 says X, truth says Y)* pair. No file downloads, no EDF parsing — a
   directory listing is enough.
2. Because the cadence is ~60 s, SomnoBridge does not need to poll continuously. After a
   short acquisition it runs an **early/late-gate poller**: one directory listing per
   cycle, placed at the *predicted* edge; whether the new mtime is already visible
   steers the next poll earlier or later (a bang-bang phase-locked loop). Steady-state
   load is **~1 request/minute** (or 1 per several minutes once rate is known) —
   the same order as the 65 s polling that live-session tools already run against
   EZShare cards.
3. The 2 s FAT quantisation is defeated statistically (upper-envelope estimator across
   files and cycles), not by polling faster.
4. SomnoBridge timestamps everything on its own monotonic clock and logs **raw
   evidence** plus BLE time-sync anchors from SomnoTrace. NTP time is assigned
   *afterwards* by interpolating between anchors. The estimator can be re-run on the
   raw log as it improves.
5. The output is per-session `offset` **and** `rate` (ppm), with an uncertainty, in
   SomnoTrace's existing sign convention (`clock_drift_ms = NTP − device`).

**Expected accuracy** (estimates; see §8 for the budget):

| Outcome | Absolute offset | Within-session drift |
|---|---|---|
| Conservative (no calibration, quantisation unresolved) | **±1 s** | ~1–3 s/night if only an offset is applied |
| Target (envelope estimator + rate fit) | **±0.3–0.5 s** | ≲0.15 s |
| Stretch (lab-calibrated flush latency, fine phase-lock) | **±0.1–0.2 s** | ≲0.1 s |

Versus today's ~20-minute error, even the conservative case is a ~1000× improvement and
comfortably inside the 1 Hz sample spacing of the O2 ring.

---

## 2. Facts, sources and assumptions

Kept separate on purpose — the design leans on the assumptions, so they are the
experiments.

### Established (with source)

| Fact | Source |
|---|---|
| BRP/PLD/SAD are EDF with **60 s data records**; EVE/CSL are EDF+ annotation files | airbreak-plus `docs/edf_signals.md` (SX567 = AS10 family) |
| EDF fixed header carries start date/time at **1 s** resolution (`dd.mm.yy`, `hh.mm.ss`) | EDF spec; airbreak-plus `docs/as11/edf_header.md` (AS11 — AS10 layout to confirm) |
| EDF+ annotation onsets are **relative** to the file start time | airbreak-plus `docs/as11/edf_annotations.md` |
| Files grow every ~60 s during therapy; live-session tools poll EZShare listings at ~65 s | hms-cpap READMEs |
| AS11 write bursts occur every ~55–60 s, each lasting roughly 1–2 s; standby writes are ≥3× rarer | SomnoTrace author's PCNT bus captures (CPAP-AutoSync `01-FINDINGS.md`) — AS11, **AS10 unverified** |
| FAT mtime has **2 s resolution** and **no timezone** | FAT specification |
| EZShare exposes `GET /dir?dir=…` (HTML `<pre>` listing with per-entry timestamp and size) and `GET /download?…` | SomnoTrace `spec/archive/files-over-http.md` |
| The AS10 UART accessory port exposes the RTC as `DAC` (`DDMMYYYY`) and `TIC` (`HHMMSS`), and `STK`, a 10 ms tick counter used as the event-timestamp timebase | airbreak-plus `docs/serial_protocol.md`, `var_reference.tsv` |
| EZShare has a hardware MUX: the WiFi side and the AS10 never share the SD bus concurrently; SD-slot power cycling (seen on other WiFi-SD cards) does not apply | Project maintainer |
| The AS10 clock is writable over UART (`DAC`/`TIC` carry the editable flag) | airbreak-plus `docs/config_variables.md` |

### Assumed — each must be verified (§9)

| # | Assumption | If false |
|---|---|---|
| A1 | The mtime in the EZShare `/dir` listing is the **AS10's FAT stamp**, passed through unchanged | Channel dead → fall back to header start + first-appearance only (~±2 s) |
| A2 | The AS10 stamps mtime at **each 60 s flush** (i.e. `f_sync`-style), not only at file close | Mid-session stamps unavailable → one pair per session (at close), lower rate resolution |
| A3 | Directory entries become visible to the card's WiFi side shortly after the flush (no long card-side caching) | Listing lags → bracket widens; must characterise staleness |
| A4 | A directory listing is cheap for the card and does not disturb the AS10's writes | Rate cap must be tightened; see §6.4 |
| A5 | Flush cadence and RTC are related in some fixed way (either RTC-locked or independent timer) | Both cases handled; which one applies decides how well FAT quantisation can be removed (§4.4) |
| A6 | The stamp-to-visibility latency `L` is a firmware-constant, ~0.1–2 s | Becomes a calibration item, not a blocker |
| A7 | The AS10 keeps plain local wall time with no timezone and no automatic DST (from general knowledge, **not** from the docs consulted) | Offsets of n·3600 s are still detected and flagged (§7.3) |

---

## 3. The measurement equation

Notation (all times as epoch seconds; the AS10's local-time stamps are interpreted in an
assumed timezone, see §7.3):

```
o    = AS10_clock − true_time        unknown offset we want (positive = AS10 fast)
T_k  = true time at which the AS10 stamps flush k
L    = latency from stamping to the directory entry being visible over WiFi  (>= 0)
s_k  = T_k + L                       when SomnoBridge could first see it (estimated by polling)
m_k  = mtime shown in the listing    = floor2( AS10_clock(T_k) )      (2 s FAT granularity)
q_k  = quantisation error            in [0, 2) s
```

Then, for every flush, the one number we can compute:

```
r_k = m_k − s_k  =  o − L − q_k
```

The unknown offset is buried under one constant (`L`) and one bounded nuisance
(`q_k`, uniform-ish over 2 s). Three consequences drive the whole design:

1. **`s_k` must be measured well** (polling/phase-lock, §6) — its error is the noise.
2. **`q_k` is removable by extremes, not averages.** Because `q_k ∈ [0,2)`, the *upper
   envelope* `max(r_k) ≈ o − L` — provided the phase `q_k` actually varies across the
   samples (§4.4). Averaging alone leaves a ~1 s bias.
3. **`L` is a calibration constant**, best measured once in the lab against the AS10's
   UART clock (§4.7), then treated as a firmware-family constant.

The AS10 timeline that ultimately gets corrected is `true = as10 − o(t)` with
`o(t) ≈ o_0 + ε·(t − t_0)`; ε is the RTC rate error. A crystal RTC at ±20–100 ppm drifts
1.7–8.6 s/day, and reaching ~20 min of error implies months without correction. That
puts the **within-night** drift at ~0.6–2.9 s over 8 h — comparable to the accuracy
target — so the correction must be *offset + rate*, not a single constant per session.

---

## 4. Side channels, ranked

Cost = load placed on the EZShare card. Everything here is read-only.

### 4.1 Channel 1 — flush-edge ↔ mtime pairs *(primary)*

- **What:** poll the listing of the active `DATALOG\<noon-day>\` directory. When a file's
  `(size, mtime)` changes, that is a flush edge; `m_k` is read from the listing, `s_k`
  from the poll timing.
- **Gives:** one `r_k` per flush per file → offset (§3), and cadence (Channel 3).
- **Cost:** one small listing (~1 KB HTML, one directory's clusters).
- **Note:** this needs only that stamps are *fresh* (A1/A2), not that the cadence is
  clean — the pairing works for any flush jitter; periodicity merely lets us *schedule*
  polls (§6).

### 4.2 Channel 2 — EDF header start time / filename *(content, 1 s)*

- **What:** `YYYYMMDD_HHMMSS_*.edf` file names and the EDF header `startdate/starttime`
  are the RTC at file creation, at **1 s** resolution — finer than the FAT stamp.
- **Gives:** an independent stamp at session start; a cross-check that FAT mtime and
  EDF headers share a clock domain (validates A1); in the RTC-locked case (§4.4) the
  *finest* quantisation available.
- **Cost:** the file name comes free from the listing; reading the 256-byte header
  needs a ranged/partial download — **avoid during therapy** unless ranged reads are
  proven cheap (§9, E3). Read headers after the session, when bridging the file anyway.

### 4.3 Channel 3 — cadence as a rate meter

- **What:** the sequence of edge times `s_k` gives the true elapsed time between
  records. `s_{k+1} − s_k` versus the stamped `m_{k+1} − m_k` compares true time with
  AS10 RTC time; `s_{k+n} − s_k` over a night versus `60·n` compares true time with
  the AS10's *record clock*.
- **Gives:** ε (ppm) for the RTC, and — separately — the rate of the timer that paces the
  60 s records. Step detection: an mtime increment ≠ 60±2 s (or an NTP-side interval
  ≠ 60 s ± jitter) flags a clock set / DST / RTC fault mid-session.
- **Precision:** rate σ ≈ σ_edge · √(12/n) / span. For σ_edge = 0.15 s: ~0.8 ppm from all
  480 cycles of a night, ~2.6 ppm from 48 sampled cycles.
- **Why it matters:** EDF record `k` starts at header start + 60·k on the AS10's *sample
  timer*, not its RTC. The edge series pins each record to true time directly and can
  exceed the accuracy of any single RTC stamp.

### 4.4 Channel 4 — de-quantising the 2 s mtime *(the key trick)*

The FAT stamp is `floor2`, so a single `r_k` is uncertain by 2 s. Ways to beat that:

1. **Upper envelope.** `max_k r_k → o − L` as the phase `q_k` sweeps `[0,2)`. Expected
   gap to the true bound after `N` phase-diverse samples is ≈ `2/(N+1)` s
   (N=10 → 0.18 s; N=50 → 0.04 s). *Needs phase diversity* — sources:
   - **Multiple files per flush.** BRP, PLD, SAD (and EVE/CSL when they flush) are
     stamped a few hundred ms apart inside one burst, so one flush yields several
     different `q`. Where two files in one burst straddle an even-second boundary
     (`m` differs by 2 s), the boundary is located to ≈ the inter-file spacing.
   - **Phase slip.** If the flush cadence comes from a timer independent of the RTC,
     `q` slips at (ppm difference × 60 s) ≈ 1 ms per cycle; a night sweeps only a
     fraction of a second, but **every night starts at a random phase**, so pooling
     nights (with the offset+rate model) fills the interval within days.
2. **RTC-locked case.** If the 60 s flush is derived from the RTC (e.g. on a minute
   tick), `q` is constant per night and **mtime seconds are always the same value**
   (e.g. `:00`). The envelope collapses to one value, but that value is then a *known
   phase* (calibrated once); the residual is `L`, not quantisation. Distinguishing
   the two cases is experiment E4.
3. **Header start time** (Channel 2, 1 s) supplies a second, finer-quantised floor of
   the same clock and roughly halves the interval in the locked case.
4. **Dwell:** stamps are cheap — the poller records *every* stamp it sees, not just
   edges, so the envelope estimator gets all the phase diversity that occurs.

### 4.5 Channel 5 — structural events *(one-off anchors, ~free)*

- New `DATALOG\<YYYYMMDD>\` folder at noon rollover (listing the parent `DATALOG`
  costs 1 request/day).
- Session close: the last flush plus `STR.edf`/summary updates at mask-off give a
  session-end pair whose *real-time* moment is a human event with tight downstream
  timing.
- Session start (first appearance of `*_BRP.edf`, etc.) — the first flush is ~60 s
  after creation, so header start + 60 s should equal the first stamp (a
  self-consistency probe).

These add redundancy and per-session anchors; none is precise enough alone.

### 4.6 Channel 6 — zero-SD-load signals *(speculative, cheap to test)*

Because every poll makes the card touch the SD through its MUX, anything that tells us
about flush timing *without* reading the card is attractive:

- **WiFi association / beacon TSF.** SomnoBridge, as STA, sees the AP appear/disappear
  and can read the AP's 64-bit µs TSF (needs promiscuous management-frame capture).
  TSF resets on card power-up → detects **card power cycles and boundaries** with no
  requests. It does *not* give flush timing.
- **Latency signature.** If the card's MCU arbitrates the MUX or services host-side
  events, a light non-SD probe (ICMP ping, ARP, or `GET /client?command=version`
  at ~5–10 Hz) may show RTT spikes or delayed responses aligned with AS10 write
  bursts. If it does, that is an SD-load-free edge detector to *steer* the phase-lock.
  Pure hypothesis — test it against Channel 1 in the lab (E5) and drop it if there is
  no correlation.
- **Response-time as a signal.** If listings *block* while the AS10 holds the bus,
  the response latency itself marks burst boundaries. Log it regardless (it is free).

### 4.7 Channel 7 — lab ground truth *(not a runtime channel)*

Needed to *calibrate and validate* everything above, once:

- **AS10 UART (airbreak-plus).** `G S #TIC`/`#DAC` read the RTC at 1 s resolution. By
  polling `TIC` and locating the second rollover (bisect on the host's NTP-disciplined
  clock, ~10 ms) the RTC offset `o` is known to tens of ms — independent of SD.
  `STK` (10 ms timer tick) read alongside gives the timer/RTC relative rate — the answer
  to "locked or independent?" (A5). Requires the accessory-port breakout described in
  airbreak-plus `docs/guide/serial_connection.md`; AirBridge offers WiFi access to the
  same port.
- **PC host rig.** A Linux machine with an SD reader mounts the EZShare card and runs
  `while sleep 60; do date >> f; sync; done`. Its time is NTP-true, so it isolates the
  *card-side* behaviour — listing latency, staleness, mtime rendering, timezone
  handling — from any AS10 unknowns (A1, A3).
- Together they let us solve for `L` and `q` and prove the estimator on a *known* `o`
  before trusting it on nights where `o` is unknown.

### Rejected / deprioritised

| Idea | Why not |
|---|---|
| Recursive or root listings | Reads many directory clusters for no extra timing information |
| Downloading file tails to read EDF+ annotation onsets | Heavy card load; onsets are relative and buffered (lag unknown); use only if Channel 1 is dead |
| Poll every second / continuous | 28,800 requests/night against a fragile card; precision achievable with 1–2 % of that |
| Using the EZShare's own clock | Assumed absent; verify cheaply (E2) then close |
| SpO2 cross-correlation with the O2 ring | Only meaningful if both sensors are on the same finger; not a general channel |
| Reprogramming the AS10 clock | Out of scope (read-only for us) — but note the UART path exists for a future *fix* rather than correction |

---

## 5. Getting NTP time into SomnoBridge

SomnoBridge sits on the EZShare AP with no internet route, so its NTP source is
**SomnoTrace over the BLE link** it already serves.

- **Two-way exchange, NTP-style.** SomnoBridge sends `t1`; SomnoTrace replies with
  `(t2 = its NTP time on receipt, t3 = its NTP time on reply)`; SomnoBridge records `t4`.
  Offset `= ((t2−t1)+(t3−t4))/2`, RTT `= (t4−t1)−(t3−t2)`. BLE connection-interval
  granularity bounds this at roughly **±10–30 ms**; keep the lowest-RTT exchanges and
  discard outliers. This is well below the HTTP-side noise, so it is not the limiter.
- **Log anchors, don't just apply them.** Each accepted exchange is an *anchor*
  `{mono_us, ntp_ms, rtt_us}`. All observations carry `mono_us` (`esp_timer`); NTP time
  is assigned **retrospectively** by interpolating between the anchors that bracket the
  observation — better than extrapolating forward, and it self-calibrates the ESP32
  crystal's ppm.
- **Holdover.** An uncalibrated XTAL (±10 ppm) drifts ≤6 ms per 10 min and ≤0.3 s over
  8 h, so anchors every ~10 min make it negligible, and even a BLE gap of hours is
  recoverable by the anchors either side.
- **If the BLE link is down** for a whole session, the observations remain valid on
  `mono_us`; they resolve when the next anchor arrives. SomnoBridge must therefore keep
  running through BLE loss and persist its log (the GEEK's TF slot suits this).

---

## 6. Polling strategy

### 6.1 Principle

Every request is a small SD access by a device that shares the bus with a medical
recorder. The budget should be spent only where it buys timing information — at the
edge — and stopped when the answer is good enough.

### 6.2 Phases

```
   AS10 flush edges:      |          |          |          |        (~60 s apart)
                          E1         E2         E3         E4
 Phase 0  IDLE-WATCH  ────────────────────────────►  slow probe of DATALOG parent
                                                     (every 2–5 min) or WiFi-link event
 Phase 1  ACQUIRE     ─ ─ ─ ─ ─ ─ ─ ─ ─│─ ─ ─ ─ ─ ─│   coarse (≈5 s) sweep of one cycle,
                                        ▼           ▼   then halve the bracket per cycle
 Phase 2  LOCK        ················ ·│· ············ one poll per cycle at the predicted edge;
                                        ↑              visible → move earlier, else later
 Phase 3  MAINTAIN    ····················│···················  1 poll per N cycles,
                                                              verify only; widen on miss
```

- **Phase 0 — idle watch.** No therapy → no need for timing. Probe the `DATALOG` parent
  slowly (or react to the AP appearing); therapy start is the first new folder/file.
  Latency here only costs coverage of the first minutes.
- **Phase 1 — acquisition (≈3–4 cycles).** Find the flush phase within ±5 s in one cycle
  (~12 requests at 5 s spacing), then halve the bracket each following cycle
  (5 s → 1 s → 0.25 s) with a handful of requests each. Total ≈25–30 requests.
- **Phase 2 — lock (early/late gate).** One listing per cycle, timed at the predicted
  edge plus an offset `φ`. The response says whether cycle `k`'s mtime is already
  visible:
  - visible → the edge was *before* the poll → move next `φ` earlier by `δ`;
  - not visible → the edge is *after* → move later by `δ`.

  That is a bang-bang phase detector: it converges to the edge and then dithers by
  ±`δ` (start `δ` at ~0.25 s, shrink toward ~50 ms as confidence grows). Each poll is a
  censored observation (`s_k < t` or `s_k ≥ t`); the estimator is a threshold fit,
  not a lookup. **One request per minute suffices** and the same response supplies
  `m_k` for the envelope estimator.
- **Phase 3 — maintain.** After the rate is known to a few ppm the edge is predictable
  to tens of ms, so polls can drop to 1 per N cycles while step detection stays alive
  (§4.3). A miss (edge not where predicted, or mtime increment ≠ 60±2 s) drops back to
  Phase 1.

### 6.3 Load budget for an 8-hour night

| Strategy | Requests / night | Timing information |
|---|---|---|
| Continuous 1 Hz | ~28,800 | high, but unacceptable load |
| Every 5 s | ~5,760 | ±2.5 s bracket per edge |
| Live-session style (65 s, unsynchronised) | ~440 | *none* — each poll lands at a random phase |
| **Locked, every cycle (Precise)** | **~500–550** | edge to ~0.1 s per cycle; rate ~1 ppm |
| **Locked, every 5th cycle (Balanced)** | **~130–150** | rate ~3–5 ppm; offset σ ~20–40 ms statistical |
| **Locked, every 15th cycle (Frugal)** | **~60–80** | rate ~8–10 ppm; step detection still active |

Recommended default: **Balanced**, escalating to Precise when the estimator's
uncertainty exceeds target or a step is suspected. Note the *Precise* profile costs about
the same as the live-session polling already run in the wild (~440/night) — the gain
comes from *when* the requests are placed, not how many there are.

### 6.4 Guardrails (load and safety)

- **Hard rate ceiling** (e.g. ≤1 request/10 s outside Phase 1) and a global request
  budget per night, enforced in SomnoBridge regardless of what the estimator wants.
- **Never poll during a bulk download**, and never run bulk transfers *during* an active
  flush cadence unless they have been proven not to disturb the AS10 — the bridge job
  and the timing job compete for the same card.
- **Back off on any anomaly**: HTTP errors, listing latency ≫ baseline, 5xx, resets.
  Exponential backoff, and a "no-timing" mode that gives up timing rather than risk the
  recorder.
- **The most sensitive requests are the ones aimed at the burst.** The edge-locked poller
  necessarily lands near the AS10's write burst (that is where the information is).
  This is the leading unknown in A4: whether the MUX arbitrates cleanly or can corrupt an
  in-flight AS10 write. It is the **first thing to test** (E0) and it decides whether
  fine locking is allowed or whether polls must trail the burst (which costs accuracy —
  one-sided bounds instead of brackets).
- **Validate AS10 output integrity** after every experimental night: EDF `Crc16`
  channels and record counts intact, no SD-error indication on the machine.

---

## 7. Estimator, evidence, hand-off

### 7.1 Raw evidence log (what SomnoBridge persists)

Log evidence, not conclusions, so improved estimators can re-run history:

```
poll     { mono_start_us, mono_end_us, path, http_status, resp_bytes,
           entries:[{name, size, mtime_raw}], plan:{phase, phi_us, cycle_id} }
anchor   { mono_us, ntp_ms, rtt_us, peer }           # BLE time exchange
link     { mono_us, event: assoc | disassoc | tsf_reset, tsf_us? }
config   { fw_version, ezshare_version, tz_assumed, poll_profile }
```

Storage: append-only, size-capped rotation on the TF card (flash as fallback),
crash-tolerant (write-then-rename segments). Exposed read-only via the metadata API.

### 7.2 Estimator (shape, not algorithm)

- Convert `(mono_start, mono_end)` to NTP via anchors → `[t_lo, t_hi]` per poll.
- Turn visible/not-visible transitions into edge intervals; fit `s_k` with a threshold
  model; model `s_{k+1} − s_k` as `P·(1+ε_timer)` plus jitter.
- Fit `r_k = m_k − s_k` with the **upper-envelope** rule per §4.4 (robust to outliers:
  use a high quantile, not the raw max) and a piecewise-linear `o(t)` with
  **change-point (step) detection**, priors carried from adjacent sessions.
- Emit `{ session, o_ms, rate_ppm, σ_o, σ_rate, flags }`. Flags: `locked_cadence`,
  `step_detected`, `tz_step_suspect`, `low_coverage`, `envelope_starved`.
- Multi-night pooling only inside the offset+rate model; never pool across a detected
  step.

### 7.3 Timezone and DST

FAT stamps and EDF headers are local wall time with no zone. `o` therefore contains any
mismatch between the AS10's local time and the ESP's timezone. Treat estimates within
~30 s of a multiple of 3600 s as **timezone/DST steps**, not drift, and report them under
a separate flag — SomnoTrace already fights this class of problem on the AS11
(`as11_time.h`). A manual clock change mid-night shows up as a step in the same way.

### 7.4 Hand-off to SomnoTrace

Sign convention matches SomnoTrace's existing `clock_drift_ms = NTP − device`. Proposed
per-session record over the metadata API:

```
{ "session_start_as10": "YYYY-MM-DDTHH:MM:SS",   // as written by the AS10 (local, no zone)
  "clock_drift_ms":  1180432,                     // NTP − AS10 at session start
  "rate_ppm":         -37.2,                      // d(NTP − AS10)/dt sign as above
  "sigma_ms":          180,
  "flags":            ["locked_cadence"],
  "evidence_ref":     "seg-0042" }
```

SomnoTrace maps this onto its existing per-session drift index
(`session_drift_entry_t`/`lookup_drift`) and applies `t_ntp = t_as10 + drift + rate·Δt`
when generating EDF header start times, filenames and STR mask events. Applying a rate
term, not just an offset, is new for the AS10 path and matters at the 1–3 s scale
(§3).

---

## 8. Expected accuracy budget

Per-flush error terms (1σ, best estimates):

| Term | Size | Handling |
|---|---|---|
| BLE time transfer | 10–30 ms | Two-way exchange, min-RTT selection, retrospective interpolation |
| ESP32 crystal holdover | <10 ms with 10-min anchors | Anchors; self-calibrated ppm |
| HTTP observation window (RTT + card processing) | 50–150 ms, jittery | Rig-A calibration of the systematic part; averaging the rest |
| Edge bracket / lock dither | 50–250 ms | Early/late gate; shrinks with confidence |
| Flush latency `L` (stamp→visible) | 0.1–2 s **systematic**, ±0.1–0.5 s random | Lab-calibrate systematic part (rig B); averaging kills random part |
| FAT quantisation `q` | ±1 s (σ 0.58 s) raw | Envelope / multi-file / header (§4.4) |
| Assumed-TZ error | 0 or n·3600 s | Detected, flagged, not "drift" |

Per-session result by scenario:

| Scenario | Dominant error | Absolute offset | Notes |
|---|---|---|---|
| A. Everything works, `L` and `q` calibrated, phase-diverse stamps | `L` residual | **~0.1–0.2 s** | Stretch goal |
| B. Envelope works, `L` uncalibrated | `L` (0.1–2 s) | **~0.3–1 s** | Calibrate `L` once, drops to A |
| C. Locked cadence, `q` constant | half of the 1 s header quantum | **~±0.5 s** | mtime adds little; header is the finer clock |
| D. Stamps only at session close (A2 false) | quantisation + latency, one pair/session | **~±1–2 s** | Rate from cadence only; offset from one pair |
| E. Listing not usable (A1 false) | first-appearance vs header only | **~±2–5 s** | Still ≫ better than 20 min |

Rate: **~1–3 ppm** (Precise/Balanced), ~8 ppm (Frugal). At 8 h, 3 ppm is 0.09 s.

Between sessions, if the rate is known to ~3 ppm, a session can be *interpolated* from
its neighbours with ~0.26 s error per 24 h — so a night where SomnoBridge lost lock or was
absent is still recoverable to well under a second.

---

## 9. Validation plan — cheapest and most decisive first

| # | Experiment | Answers | Kills / confirms |
|---|---|---|---|
| **E0** | Ramp poll rate against a live AS10 therapy night (1/min → 1/10 s), inspect EDF `Crc16`/record counts and the machine for SD errors | **A4** — is edge-timed polling safe? | Gates the whole locked-poll design |
| E1 | Compare listing mtime with EDF header start time for the same file; check parity of mtime seconds | **A1** — whose clock, any TZ shift | Channel 1 alive/dead |
| E2 | Capture raw HTTP headers (`Date:`), `/client?command=version`; look for card-generated stamps | EZShare's own clock; firmware fingerprint | Closes the card-clock question |
| E3 | Test `Range:` support and cost of a 256-byte header read mid-therapy | Whether Channel 2 can be read live | Header polling allowed/forbidden |
| **E4** | Rig B: read `TIC` (bisected rollover) + `STK` over UART while logging SD stamps; run several nights | **A5** locked vs independent; `L`; `q` behaviour; **ground-truth `o`** | Decides §4.4 branch; calibrates `L` |
| E5 | Rig B/A: 5–10 Hz ping / `version` probe vs Channel-1 edges | Channel 6 latency signature | Adopt or drop |
| E6 | Rig A: PC host writing at 60 s cadence + `sync`; SomnoBridge polling | Card listing latency & staleness (**A3**), HTTP RTT distribution, mtime rendering | Calibrates the HTTP term; independent of AS10 |
| E7 | Full system on AS10 for ≥2 weeks with UART truth captured at intervals | End-to-end accuracy vs ground truth; drift character (offset/rate/steps); DST behaviour | Final accuracy claim |

**Pass criteria for adopting the design:** E0 shows no AS10 disturbance at the chosen
rate; E1/E2 confirm A1; E4 or E6 measures `L` and stale-listing bounds; E7 shows the
estimated `o` within 0.5 s of UART truth on ≥90 % of nights.

---

## 10. Risks and open items

| Risk | Consequence | Mitigation / fallback |
|---|---|---|
| Polling near the burst disturbs a write (A4) | Data loss or SD error on a medical recorder | E0 first; guardrails §6.4; degrade to trailing polls |
| Mtime updated only at file close (A2) | One pair per session | Scenario D; cadence still gives rate |
| Card-side listing cache | Edge appears late by a constant | Rig A measures it; folded into `L` |
| Cadence has large jitter | Poor prediction → more acquisition | Pairing still works (§4.1); widen the gate |
| Firmware variants differ (AS10 SX567 revisions, card firmware) | Per-family `L`, `q` | Fingerprint via `version`/`Identification.*`; per-family constants |
| DST / manual clock set mid-night | False "drift" | Step detection + flags (§7.3) |
| SomnoBridge not powered / not locked overnight | Missing sessions | Interpolation between neighbours; flag `low_coverage` |
| Privacy | Listings contain only names/sizes/times — no therapy data | Logs contain no EDF content; keep it that way |

**Open questions for the maintainer:**

1. Can an AS10 with the UART breakout be dedicated to E4 for a few nights? It is the
   single highest-value experiment.
2. Preferred split of work: estimator on SomnoBridge (proposed; cheap arithmetic, keeps
   SomnoTrace simple) or all-on-SomnoTrace from raw evidence?
3. Should SomnoBridge expose "hold off timing" for cases when SomnoTrace wants the card for
   a bulk pull immediately after therapy ends?
4. Log retention target (nights of raw evidence kept on the GEEK's TF card).

---

## 11. Critical Evaluation and Enhanced Strategy (Architectural Review)

> **Status:** Review addendum. Evaluates the physical and mathematical assumptions
> of the proposal, identifies safety and estimation risks, and specifies an enhanced,
> lower-risk strategy that eliminates bus contention while providing tighter accuracy.

### 11.1 Executive Assessment: What Holds vs What Fails

| Proposed Element | Status | Critique & Reality Check |
|---|---|---|
| **BLE NTP time transfer** (§5) | **Agreed (Strong)** | Pushing NTP time from SomnoTrace to SomnoBridge over BLE with two-way exchanges and retrospective interpolation on `mono_us` is elegant, robust, and avoids complex dual-radio WiFi topologies. |
| **Monotonic raw evidence logging** (§7.1) | **Agreed (Strong)** | Logging raw timestamps and observables to the GEEK's TF cache before fitting preserves ground truth and allows retrofitting improved estimators without re-running hardware tests. |
| **Offset + rate model** (`clock_drift_ms`, `rate_ppm`) (§3, §7.4) | **Agreed (Strong)** | Matches SomnoTrace's established sign convention and models within-night crystal drift (~1–3 s over 8 h). |
| **Bang-bang PLL edge-hunting** (§6.2) | **Flawed (Hazardous)** | Intentionally steering polls to land directly on the 1–2 s AS10 write burst creates high bus contention on the EZShare hardware MUX, risks SD buffer overrun on the medical recorder, and suffers from card-side HTTP latency jitter (50–1500 ms) that destroys sub-second early/late gating. |
| **Upper-envelope de-quantization** (§4.4) | **Flawed (Math)** | The `2/(N+1)` envelope convergence assumes independent, uniformly distributed phase samples $q_k \sim U[0, 2)$. In reality, the AS10's sample clock and RTC drift by <20 ppm (~1.2 ms/cycle), sweeping <0.5 s across an entire 8 h night. The phase is strongly autocorrelated; the upper envelope on a single night remains biased by up to ~1.4 s. |
| **Free 1 s filenames in listings** (§4.2) | **Unverified / Unlikely** | Real EZShare firmware emits 8.3 FAT short names (`2071A5~1.EDF`), stripping the second-resolution timestamp from `YYYYMMDD_HHMMSS_*.edf`. Finer resolution must come from EDF headers or record integration. |
| **Online-only directory restriction** (§1, §4) | **Suboptimal** | Restricting observation to 1 KB directory listings during therapy ignores the fact that SomnoBridge's core purpose is to bridge the full EDF files to SomnoTrace post-session. Full EDF headers and exact record counts provide ground-truth integration that supersedes directory heuristics. |

---

### 11.2 The Safety & Jitter Problem with Edge-Locked Polling

In §6.2 and §6.4, the proposal states:
> *"The most sensitive requests are the ones aimed at the burst. The edge-locked poller necessarily lands near the AS10's write burst (that is where the information is)."*

This is the single most critical operational risk in the proposal:

1. **Hardware MUX Contention:** The EZShare card uses an internal bus switch to multiplex flash access between the AS10's SDIO/SPI pins and the card's internal WiFi MCU. If an incoming HTTP directory request hits while the AS10 is issuing multi-block write commands, one of two things happens:
   - The card's WiFi MCU stalls until the AS10 completes, causing HTTP response latency spikes (up to 1–2 seconds) or TCP timeouts on SomnoBridge.
   - The MUX arbitrates against the AS10, delaying host block acknowledgement. For a CPAP device sampling flow at 25–50 Hz and pressure at 40 Hz into RTOS ring buffers, delayed SD writes risk buffer overrun, dropped sensor packets, or triggering a visible SD card fault on the machine.
2. **HTTP Latency Jitter vs Bang-Bang Step:** A bang-bang PLL dithering with step $\delta = 50\text{--}250\text{ ms}$ requires the observation channel to have timing noise well below $\delta$. Embedded WiFi SD web servers exhibit non-deterministic service latency (50 ms idle up to >1000 ms during flash read/write or WiFi beacon retransmission). A 300 ms response latency spike reads as a false "edge not yet visible", corrupting the PLL phase detector.
3. **Card-Side Sector Buffering:** If EZShare caches directory sectors in SRAM even for 500 ms to 1 s, the apparent visibility edge shifts unpredictably based on cache eviction rather than host write timing.

#### Enhanced Alternative: The "Quiet-Window" Mid-Cycle Poller
Instead of chasing the write burst, schedule polls at **phase $\phi \approx +30\text{ s}$** (the mid-cycle quiet window):

```
AS10 Write Bursts:   |===| (1-2s)                      |===| (1-2s)
                     ▲                                 ▲
                     k                                 k+1
                    ─────────────────────────────────────────► time (s)
                                    ★
Poll Timing:                  Mid-cycle Poll
                              (φ ≈ +30 s)
                              Bus completely idle
```

- **Zero Contention:** The SD bus is completely idle. The AS10 has finished writing and flushing directory entries ~28 seconds prior.
- **Deterministic HTTP Latency:** The card's MCU accesses flash with zero bus arbitration delay, giving rock-solid, repeatable RTTs (~50–80 ms).
- **Exact Record Counting:** Every poll at $\phi \approx +30\text{ s}$ observes:
  - Total file size $S_k = \text{HeaderSize} + k \times \text{RecordSize}$.
  - Record count $k$ incrementing strictly monotonically: $k, k+1, k+2, \dots$.
  - FAT directory timestamp $m_k$.

---

### 11.3 Mathematical Flaw in Upper-Envelope De-quantization

Section 4.4 models FAT quantization error $q_k$ as sweeping uniformly across $[0, 2)$ s, citing expected error $2/(N+1) \approx 0.04\text{ s}$ for $N=50$.

**Why this math fails in practice:**
1. **Clock Correlation:** The AS10's 60.000 s record generation is paced by hardware timer interrupts tied to the main crystal oscillator (e.g., 16/25 MHz, typically $\pm 15\text{--}25\text{ ppm}$). The RTC is paced by an independent 32.768 kHz tuning-fork crystal (typically $\pm 20\text{--}40\text{ ppm}$).
2. **Slow Phase Drift:** The relative drift between sample timer and RTC crystal is $\Delta\epsilon = |\epsilon_{\text{sample}} - \epsilon_{\text{rtc}}| \lesssim 20\text{ ppm}$. Over one 60 s cycle:
   $$\Delta q = 60\,\text{s} \times 20 \times 10^{-6} = 1.2\,\text{ms/cycle}$$
3. **Nightly Range:** Over an 8-hour therapy session (480 cycles):
   $$\text{Total phase sweep} = 480 \times 1.2\,\text{ms} \approx 0.58\,\text{s}$$
   The phase $q_k$ explores less than 30% of the 2.0 s FAT bin during an entire night!
4. **Quantization Trap:** If the session happens to start at initial phase $q_0 = 0.2\,\text{s}$, the maximum phase observed all night is $q_{\max} \approx 0.78\,\text{s}$. The upper envelope $\max(r_k)$ will underestimate $o - L$ by:
   $$2.0\,\text{s} - 0.78\,\text{s} = 1.22\,\text{s}$$
   The upper-envelope estimator leaves an uncorrectable, single-night bias of up to ~1.4 s.
5. **No Intra-Burst Diversity:** Because BRP, PLD, and SAD are written in the same ~1 s task burst, their directory updates occur virtually simultaneously. In FAT32, all three files will receive the exact same 2-second timestamp in almost all cycles. They do not supply independent $q$ realizations.

---

### 11.4 Real EZShare Directory Listings: 8.3 Short Filenames

The proposal (§4.2) assumes that `YYYYMMDD_HHMMSS_*.edf` is readable directly from the `/dir` listing at 1-second resolution.

**Hardware reality:**
- As documented in SomnoTrace's `spec/archive/files-over-http.md`, EZShare's firmware uses DOS 8.3 short filename aliasing in directory HTML listings:
  ```html
  <pre>
  2021-07-19 10:24:01       12345    <a href="/download?fname=2071A5~1.EDF&fdir=..\DATALOG\20210719">2071A5~1.EDF</a>
  </pre>
  ```
- The original long filename timestamp is hashed/truncated into the short name (e.g. `2071A5~1.EDF` or `202107~1.EDF`).
- **Consequence:** The 1-second timestamp in the filename cannot be assumed to be present in `/dir`. To obtain 1-second resolution, SomnoBridge or SomnoTrace must read the **EDF fixed header** (`bytes 0..255`) where `startdate` (`dd.mm.yy`) and `starttime` (`hh.mm.ss`) are preserved intact.

---

### 11.5 The Two-Tier Paradigm: Quiet-Window Tracking + Post-Session Synthesis

SomnoBridge does not exist in a vacuum; it is an active bridge to SomnoTrace. SomnoTrace consumes data **post-session** when calculating compliance, daily summaries, SleepHQ synchronization, and EDF export.

Therefore, drift estimation should be split into two complementary tiers:

```
┌────────────────────────────────────────────────────────────────────────┐
│ TIER 1: Online / In-Session Tracking (SomnoBridge on EZShare AP)          │
│                                                                        │
│ • Polls at quiet midpoint (φ ≈ +30 s), 1 request every 60–120 s        │
│ • Observes record increments (k), FAT mtime, and response RTT          │
│ • Maintains continuous NTP synchronization via SomnoTrace BLE anchors  │
│ • Detects: Session Start, Session Stop, DST / Clock-set step anomalies │
│ • Logs raw evidence tuple: {mono_us, ntp_interpolated, k, mtime, S_k}  │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Post-session BLE File Transfer
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ TIER 2: Post-Session Rigorous Synthesis (SomnoTrace or SomnoBridge)       │
│                                                                        │
│ • Ingests full EDF fixed headers (exact 1 s starttime: HH:MM:SS)       │
│ • Ingests STR.edf session summary (mask-on / mask-off events)          │
│ • Integrates exact sample count: Total Duration = N_records × 60.000 s  │
│ • Solves joint regression:                                             │
│     o(t) = clock_drift_ms + rate_ppm × (t − t_0)                       │
│ • Pins start/end boundaries against NTP observation windows            │
│ • Emits finalized drift parameters with verified sub-0.3 s confidence  │
└────────────────────────────────────────────────────────────────────────┘
```

#### Why Two-Tier is Decisive:
1. **Sample-Clock Rate Metering to 0.01 ppm:** Over an 8-hour session (28,800 s), the total number of records $N$ is known exactly from the EDF header and file size. Comparing the elapsed NTP time between first flush and last flush ($T_{\text{last}} - T_{\text{first}}$) against $N \times 60.000\,\text{s}$ gives:
   $$\text{Rate Error} \approx \frac{\sqrt{2} \times \sigma_{\text{poll}}}{28800\,\text{s}} \approx \frac{1.414 \times 0.2\,\text{s}}{28800\,\text{s}} \approx 0.01\,\text{ppm}$$
   This completely removes rate uncertainty without single-cycle phase locking.
2. **Absolute Offset Disambiguation:**
   - The EDF fixed header gives $t_{\text{start}}$ at **1 s** resolution.
   - The FAT directory creation mtime gives an independent 2 s floor.
   - SomnoBridge's first observed file appearance window bounds $T_{\text{start}}$ from above: $T_{\text{first\_seen\_ntp}} - \text{Latency} \ge t_{\text{start}}$.
   - Combining the 1 s header timestamp with the bounding window isolates the sub-second offset to the order of the file creation latency (~0.2–0.5 s), achieving target accuracy cleanly.

---

### 11.6 Physical Boundary Anchoring: WiFi TSF and Mask Events

Beyond directory entries, three physical hardware events provide zero-bus-load time anchors:

1. **Card Association & WiFi Beacon TSF:**
   - When the AS10 powers up its SD slot (at therapy start or machine wake), the EZShare card boots.
   - SomnoBridge detects the SSID beacon. The 802.11 beacon management frame carries a 64-bit microsecond Timing Synchronization Function (TSF) timestamp.
   - The TSF resets to 0 on boot. SomnoBridge recording the monotonic arrival of `TSF = 0` provides an exact hardware boot timestamp without issuing a single HTTP or SD request.
2. **Mask-On / Mask-Off Transients:**
   - The AS10 detects breathing and mask application automatically (SmartStart / SmartStop).
   - In `STR.edf` and the EDF summary files, MaskOn and MaskOff are recorded to the minute/second.
   - SomnoTrace's concurrent Wellue O2 Ring recording detects the sharp rise in pulse rate and motion artifact when the patient wakes up and removes the mask. Cross-referencing mask-off time bounds the end-of-session boundary within a few seconds.
3. **Session Rollover at Noon:**
   - As documented in SomnoTrace's `as11_time.h`, ResMed machines roll their daily recording folder at 12:00:00 local machine time.
   - Observing the exact NTP second when the AS10 creates the new `DATALOG\<YYYYMMDD>` folder at noon provides a direct, unquantized observation of when the AS10's RTC struck 12:00:00.

---

### 11.7 Comparison: Original Proposal vs Enhanced Strategy

| Parameter | Original Proposal | Enhanced Strategy | Benefit |
|---|---|---|---|
| **Poll Scheduling** | Early/Late gate at write edge ($\phi \approx 0\text{ s}$) | Mid-cycle quiet window ($\phi \approx +30\text{ s}$) | **Zero bus contention**, no medical device interference, stable HTTP RTT |
| **Poll Complexity** | Bang-bang PLL tracking with dither | Fixed 60–120 s cadence, phase-shifted | Eliminates PLL instability, simple robust state machine |
| **Quantization Handling** | Upper envelope over $r_k$ | Joint fit of 1 s EDF header + record integrator + quiet bounds | Mathematically sound; avoids the autocorrelated $q_k$ trap |
| **Rate Determination** | Edge-to-edge diffs across cycles | Total records ($N \times 60\,\text{s}$) vs total session NTP elapsed time | Sub-0.05 ppm precision with trivial computation |
| **Filename Reliance** | Assumes long names with HHMMSS in `/dir` | Treats `/dir` names as 8.3 aliases; extracts 1 s time from EDF header bytes | Robust against real EZShare firmware limitations |
| **Timing Role** | Standalone online estimator on SomnoBridge | Two-tier: SomnoBridge logs raw evidence; SomnoTrace finalizes at ingest | Fits SomnoTrace's existing post-therapy pipeline; maximum algorithmic flexibility |

---

### 11.8 Revised Validation Path

To reflect these insights, the experiment plan (§9) should be prioritized as follows:

1. **E0 (Card Profile & Filename Verification):**
   - Connect to EZShare AP with a laptop or ESP32; run `GET /dir?dir=A:\DATALOG\...` during an existing card dump.
   - Verify whether filenames are 8.3 (`2071A5~1.EDF`) or long names (`YYYYMMDD_...`).
   - Check HTTP response headers for `Date:` or server signatures.
2. **E1 (Quiet Window vs Edge Contention):**
   - Poll at $\phi = +30\text{ s}$ vs $\phi = 0\text{ s}$ against a test host writing at 60 s intervals.
   - Measure HTTP latency distribution in both windows to confirm quiet-window stability.
3. **E2 (Header Fetch Cost):**
   - Measure the latency and behavior of reading the first 512 bytes of an EDF file over EZShare HTTP (closing socket immediately after header).
4. **E3 (Ground Truth Verification via UART / Rig B):**
   - Run the two-tier estimator against the AS10 with UART `TIC`/`STK` bisection to measure the true residual latency $L$ and confirm single-night error $< 0.3\,\text{s}$.

---

## Appendix A — Author's Response to §11, and Revised Strategy

> **Scope:** a point-by-point assessment of the §11 review, with derivations where I
> disagree, followed by a synthesis that is better than both the original proposal and
> §11's replacement. Where this appendix conflicts with §1–§10 or §11, **this appendix
> governs** — including the accuracy figures in §1 and §8.

### A.0 Verdict

**Partially agree.** §11's *safety posture* (default away from the write burst) and its
*two-tier split* are right and should be adopted. Its *criticism of the upper-envelope
estimator* is directionally right and sharpens §4.4, though it misses the mitigations
already in the design (cross-night pooling, the RTC-locked case). Its *replacement
strategy*, however, does not work as written: a mid-cycle poll cannot localize the flush
event, so §11's offset and rate claims do not follow from its own design, and one of its
headline numbers is off by ~1000×. §A.4 supplies what §11 is missing — a way to get the
true-time comb without edge-hunting — and the result keeps §11's safety property while
retaining most of the original precision.

### A.1 Accepted, no reservations

1. **Quiet windows as the default.** §6.4 treated burst proximity as a gated risk
   (test in E0, then decide). §11 is right that the *default* must be away from the
   burst, with near-edge observation strictly opt-in. Adopted (§A.5).
2. **Two-tier split.** SomnoBridge logs raw evidence and tracks online; SomnoTrace (or
   SomnoBridge offline) finalizes at ingest with full EDF headers. This was already the
   intent in §7.1/§7.4 — §4.2's "avoid during therapy" applied to *live* header reads,
   not to post-session ingestion. §11 states it better; adopted.
3. **8.3 filename warning — as a risk, not a fact.** The archived spec
   (`files-over-http.md`) does show 8.3 aliases in a real listing, so relying on
   `HHMMSS` from names is unsafe. Refinements §11 misses: (a) a VFAT alias is a
   *truncation* of the long name (`202602~1.EDF` keeps the date, loses the time) —
   not a hash, as §11 says; (b) the spec's `2071A5~1.EDF` example is likely not an
   AS10 card at all; (c) even with 8.3 aliases, the listing still carries **size and
   mtime**, so Channel 1 is unaffected — only the "free filename timestamp" part of
   Channel 2 dies. Verify per card (merged E0).
4. **Envelope sharpening.** §11's 1.2 ms/cycle slip figure and ~1.4 s worst-case
   single-night bias are consistent with §4.4's own note ("a night sweeps only a
   fraction of a second"). What §4.4 failed to make loud enough is the **cold-start
   cost**: pooling fixes the envelope over nights, but the *first* night carries the
   bias. The headline accuracy table in §1 was therefore optimistic for night one;
   §A.6 replaces it.

### A.2 Rejected or corrected, with derivations

1. **"Mid-cycle polls give a sub-second offset" — no.** A poll at φ = +30 s observes
   *state*, not an event: it proves `T_k < t_poll`, never where within the cycle the
   flush happened. The flush is bracketed by the previous poll, i.e. a ~60–120 s
   window in §11's own cadence. §11.5's chain — "first observed appearance bounds
   T_start from above ... isolates the sub-second offset to the order of file creation
   latency" — provides only an *upper* bound whose width is the poll cadence
   (tens of seconds), not sub-second. The sub-second part must come from the
   quantisation machinery §11 criticizes; §11 removes the mechanism and does not
   replace it. (The replacement exists — §A.4 — but it is not in §11.)
2. **The 0.01 ppm claim is arithmetically wrong by ~1000×.** §11.5:
   `√2 × 0.2 s / 28,800 s ≈ 0.01 ppm`. Compute it: `0.2828 / 28,800 = 9.8×10⁻⁶`,
   i.e. **9.8 ppm**. Additionally, σ_poll = 0.2 s presumes edge-localized flush times
   — exactly what a mid-cycle scheme does not produce. With mid-cycle polls alone the
   same formula yields ~10³ ppm. The correct quiet path to rate precision is the
   count-based comb fit (§A.4): **~1–5 ppm per night** at one poll/cycle — not
   0.01 ppm, but comfortably enough.
3. **"No intra-burst diversity" — plausible, unverified, and no longer load-bearing.**
   Whether BRP/PLD/SAD share a 2 s stamp is measurable (merged E1). Either way, the
   revised strategy does not rely on it.
4. **"Unquantized noon observation" — no.** The new `DATALOG\<date>\` folder's
   creation mtime is still a 2 s FAT stamp plus visibility latency. It is a useful
   1/day anchor (as §4.5 said), not an unquantized one.
5. **Buffer-overrun risk — overstated.** The AS10 buffers ≥60 s of data by
   construction (that is why it writes once a minute); a few hundred ms of MUX
   arbitration delay cannot plausibly overrun a buffer sized for a full minute. The
   realistic residual risks are arbitration glitches and visible SD faults — still
   E0's job. Context §11 omits: EZShare cards are *used* in exactly this mode by the
   CPAP community (hms-cpap polls during therapy at 65 s), and the FYSETC-derived
   power/reboot hazard language in §11.2 describes a different device (an ESP32 SD
   card with known slot-power problems), not EZShare with its hardware MUX. Treat
   contention as *to be measured*, not as established danger.
6. **TSF reading is harder than stated.** Capturing the AP's beacon TSF needs
   monitor/promiscuous mode, which conflicts with the STA association on ESP32 — you
   cannot read TSF while connected with normal APIs. And with the EZShare MUX (no
   slot power cycling), TSF resets are rare. The boundary information §11 wants is
   available for free from association/disassociation events, no TSF needed.

### A.3 What the review missed

1. **The record count is a stronger observable than §11 uses it for.** §11.2 notes
   that each quiet poll sees `S_k = HeaderSize + k × RecordSize`. That integer `k` is
   the key to the comb fit below — it converts quiet, random-phase polls into
   sub-second true-time information without going near the burst.
2. **Cross-night pooling** (already §4.4) — §11.3 does not address it.
3. **The RTC-locked cadence case** — if the flush is derived from the RTC, the phase
   is *known* (calibratable), not random, and the quantisation trap disappears. §11
   assumes the independent-timer case throughout; the locked case could be the good
   case rather than a fallback.
4. **Odd seconds in the archived listing sample.** The spec's example shows
   `10:24:01` — an odd second, impossible in a raw 2 s FAT mtime field. Either the
   sample is idealized or EZShare renders time its own way. This sharpens E1: check
   second parity in real listings before trusting A1 at all.

### A.4 The synthesis: quiet-phase comb fitting

This replaces both the bang-bang PLL (§6.2) and §11's claim that quiet polls cannot
yield precision.

**Method.** Poll once per cycle at a *dithering* phase in the quiet window (uniform in
[+15 s, +45 s] relative to the predicted burst). Each poll yields `(t_j, S_j, m_j)`.
From `S_j` compute the exact integer record count `k_j` (record sizes are fixed per
file type and derivable from the published signal tables, e.g. airbreak-plus
`edf_signals.md`). Each poll then asserts:

```
T_0 + k_j·P  <  t_j  ≤  T_0 + (k_j+1)·P
```

where `T_0` is the true time of flush 0 (creation + flush latency λ) and `P` the true
flush period. This is an interval constraint on the comb `(T_0, P)`; intersecting all
polls' constraints is a two-parameter fit with no edge proximity required.

**Precision (n = 480 polls, P ≈ 60 s, 8 h night):**

| Quantity | Bound | Value |
|---|---|---|
| `T_0` | closest poll-to-edge approach ≈ `P/2n` | **~0.06 s** (no guard band) |
| `P` | `≈ (T_0 width)/(k_max·P)` | **~3.5 ppm** per night |
| `T_0`, `P` with guard band `G` | the guard *is* the floor: `T_0 ≈ G`, `P ≈ G/(k_max·P)` | G=3 s → T_0 ~3 s, P ~100 ppm/night |

The guard-band row is the honest cost of §11's safety posture, and it is where the
two positions genuinely trade off: **a strict guard band costs rate precision**
(usable only pooled across nights), while the *offset* is unaffected (see below). For
reference, edge-localized regression (the original PLL) gives ~0.8 ppm at
σ_edge = 0.15 s — so the quiet comb fit costs roughly 4× in rate precision and
nothing in safety. That is a good trade.

**Offset.** Once `(T_0, P)` are known, every mtime is an offset sample:
`o ∈ [m_k − T_k, m_k − T_k + 2)`. The 2 s bin is attacked by:

1. **EDF header pairing (post-session):** header `starttime` is `floor1` of the same
   RTC instant as the first-flush mtime's `floor2`; together they narrow the bin from
   2 s to 1 s (the `floor1` value resolves the odd/even bit). Adds the creation→first-
   flush constant λ₁ to the calibration set (joins λ and L — see §A.6 note).
2. **Cross-night envelope:** each night starts at a random phase (therapy start is
   random relative to even seconds) → after K nights the expected envelope gap is
   ≈ 2/(K+1) s within whichever bin applies. A week of nights → **~0.25 s**;
   two weeks → ~0.13 s.
3. **Locked-cadence gift (if E4 says so):** phase known → skip 1 and 2.

**What this means:** the offset accuracy of the original design never actually came
from edge-hunting (it was envelope-limited all along — §11's critique applies to the
original equally). What edge-hunting bought was rate precision, and the comb fit
recovers that quietly. The two critiques and the synthesis therefore converge:
**nobody needs to poll the burst.**

### A.5 Revised polling design (replaces §6.2/§6.3 phases)

- **Baseline (all profiles):** one listing per cycle at a dithered quiet-window phase;
  log `(t_j, sizes, mtimes)`; run the comb fit incrementally; guard band `G` around
  the predicted burst, default **G = 3 s**, tunable by E0.
- **Phase 0/1** of §6.2 are replaced by the comb fit itself: after ~10–15 quiet polls
  the phase is known well enough to schedule safely (to a few seconds), and precision
  accumulates over the night; there is no 5 s sweep and no edge approach at all.
- **Optional stretch — sparse edge ladders.** Only if E0 shows the card tolerates
  near-edge requests: on 2–4 cycles per night, take 3–5 polls at ~100–300 ms spacing
  around the predicted edge (~15 requests/night, ≤1 s of near-edge time). This is the
  path to ±0.1–0.2 s single-night offset and ~1 ppm rate. It is a luxury, not a
  dependency.
- **Guard-band trade, stated plainly:** `G` bounds `T_0` precision but not offset
  accuracy; tightening `G` only pays off for rate and record placement. Default to
  safety; revisit only with E0 data.
- **Load:** unchanged from §6.3 (Balanced ≈ 130–150 requests/night); the dither is
  free.

### A.6 Revised accuracy table (supersedes §1 and §8)

All figures are estimates pending E4/E6/E7.

| Regime | Absolute offset | RTC rate | Sample-timer rate |
|---|---|---|---|
| First night, mtime only, no calibration | **±1–1.4 s** (2 s bin, cold envelope) | ~70 ppm/night (mtime pattern) | ~1–5 ppm (comb fit) |
| First night + post-session header pairing | **±0.4–0.5 s** (1 s bin) | pooled only | ~1–5 ppm |
| After ~1 week of pooled nights | **±0.2–0.3 s** | **±1–2 ppm** | ~1–5 ppm/night, pooling further |
| Any night after UART calibration (E4) | minus the calibrated λ-class constant | as above | as above |
| Stretch: + sparse edge ladders (post-E0) | **±0.1–0.2 s** | ~1 ppm | ~0.8 ppm |

Calibration note: three latency constants exist — λ (creation→flush 0), λ₁ (as used
in header pairing), L (stamp→visibility). They fold into **one** composite constant
per firmware family; only the UART rig (E4) can separate them. This is why E4 remains
the keystone for absolute accuracy, exactly as §4.7 said.

### A.7 Merged experiment list

| ID | Experiment | Source |
|---|---|---|
| **E0** | Card profile: real listing (8.3 vs long names, second parity, creation vs modification time), `Date:` header, firmware version — during a normal card dump | §11.8 + §9 E1/E2 + A.3.4 |
| **E0b** | Safety ramp: latency distribution and AS10 integrity at quiet vs near-edge poll phases; defines the guard band `G` and whether ladders are ever allowed | §9 E0 + §11.8 E1 |
| **E1** | Clock-domain checks: mtime vs EDF header consistency, intra-burst stamp diversity, cadence regularity (flush-interval distribution) | §9 E1 + A.2.3 + A.3.3 |
| **E2** | Header fetch cost / `Range:` support, live and post-session | §11.8 E2 |
| **E3** | Comb-fit validation offline: re-fit a night of recorded polls, compare `T_0`/`P` against ground truth | new (this appendix) |
| **E4** | UART ground truth: `TIC` bisection, `STK` rate, λ/λ₁/L calibration, locked-vs-independent decision | §9 E4 |
| **E5** | Latency-signature probe (optional, drop on no correlation) | §9 E5 |
| **E6** | Host rig: listing staleness, RTT distribution | §9 E6 |
| **E7** | Multi-week end-to-end vs UART truth | §9 E7 |

### A.8 Bottom line

Adopt from §11: the quiet-window default, the two-tier split, the 8.3 warning, and the
sharper view of the envelope's cold-start cost. Reject from §11: the sub-second offset
claim (a mid-cycle poll cannot localize an event), the 0.01 ppm rate figure
(arithmetically 9.8 ppm, and not derivable from its own design), and the "unquantized
noon" claim. Add what §11 missed: the record-count comb fit (quiet, sub-second `T_0`,
~1–5 ppm/night), cross-night envelope pooling, the locked-cadence case, and the
odd-second listing anomaly. The result — **quiet dithering polls + comb fit + header
pairing + cross-night pooling, with sparse edge ladders as an opt-in stretch** — keeps
the safety posture of §11, the architecture of §1–§10, and an accuracy of **~±0.5 s on
night one and ~±0.2–0.3 s within a week**, at ~130–150 requests/night.

---

## Appendix B — Second Reviewer's Assessment

> **Role:** independent review of the full document as it stands (§1–§10, §11,
> Appendix A). This appendix does not modify any prior section; it adjudicates
> the open disputes, calls out residual gaps, and contributes observations
> neither previous pass raised.

### B.1 Overall Verdict

**Appendix A is the governing text and is substantially correct.** The
comb-fit synthesis (§A.4) resolves the central tension between §6.2's
edge-hunting and §11's claim that quiet polls can yield sub-second precision.
The quiet-dithered-poll + comb-fit design is sound, safe, and achieves the
stated accuracy. What follows are the remaining points where I either
strengthen, qualify, or disagree with what is on the table.

### B.2 Adjudication of Disputed Points

#### B.2.1 The 0.01 ppm Arithmetic (§11.5 vs A.2.2) — **A wins**

§11.5 claims:

> Rate Error ≈ √2 × σ_poll / 28,800 s ≈ 0.01 ppm

Appendix A correctly identifies this as arithmetically wrong:
`0.283 / 28,800 ≈ 9.8 × 10⁻⁶ = 9.8 ppm`, not 0.01 ppm. The author likely
confused ppm with a fractional value or dropped a factor of 10⁶. Additionally,
σ_poll = 0.2 s presumes the flush time is measured to ~200 ms — true for
edge-localized polls, but a mid-cycle quiet poll at φ ≈ +30 s only bounds
the flush to the poll interval (60–120 s), making σ_poll ≈ 30–60 s if no
record-counting is done. Appendix A's comb fit recovers the precision without
edge proximity, yielding ~1–5 ppm/night — an honest figure.

#### B.2.2 "Unquantized Noon Observation" (§11.6 vs A.2.4) — **A wins**

§11 claims the new `DATALOG\<YYYYMMDD>\` folder provides an "unquantized
observation of when the AS10's RTC struck 12:00:00." The folder's *creation
timestamp* is a FAT mtime with 2 s resolution, plus visibility latency `L`.
The observation is bounded, not unquantized. The *useful* part — one anchor
per day near noon — was already in §4.5. What §11 adds is the emphasis on this
as a structural boundary, which is fair; calling it unquantized is not.

#### B.2.3 Bus Contention Safety (§11.2 vs A.2.5) — **Both partially right**

§11's concern about MUX contention is valid as a *risk to investigate*, but
it overstates the danger:
- The AS10 buffers a full 60 s data record before writing. A single directory
  listing (~1 KB, ~50–100 ms SD bus time at the MUX) cannot plausibly overflow
  a 60 s buffer.
- The CPAP community runs hms-cpap at 65 s poll intervals during therapy
  against real EZShare cards without reported AS10 faults. This is weak
  evidence (no one has verified EDF `Crc16` integrity), but it sets a prior.

Appendix A is right that §11's specific "buffer overrun / dropped sensor
packets" scenario is implausible, and right that E0 must settle this
empirically rather than by assumption in either direction. The correct default
is §A.5: quiet-window polls with a configurable guard band, tightened only
with E0 data.

#### B.2.4 Upper-Envelope Phase Diversity (§11.3 vs A.1.4) — **Both partially right**

§11's core point is correct: the intra-night phase sweep Δq ≈ 0.58 s over
480 cycles is insufficient to cover the full 2 s bin in one night. The
single-night bias can reach ~1.4 s. Appendix A accepts this ("cold-start cost")
and repairs it via cross-night pooling and EDF header pairing (1 s quantum
instead of 2 s).

Where §11 goes too far is the "No Intra-Burst Diversity" claim (§11.3 point 5):
whether BRP, PLD, and SAD receive identical 2 s stamps in every cycle is
*unverified*. On the AS11, PCNT captures show that the write burst spans
~1–2 s; files are flushed sequentially, not atomically. If BRP finishes at
T and SAD finishes at T + 0.8 s, they may land in different 2 s FAT bins
~40% of the time (whenever they straddle an even-second boundary). This is a
real source of intra-burst diversity — just not a guaranteed one. The answer
comes from E1 (merged), not from first principles. Appendix A correctly marks
it as "plausible, unverified, and no longer load-bearing."

#### B.2.5 TSF Reading Feasibility (§11.6 vs A.2.6) — **A wins on practicality**

ESP-IDF's `esp_wifi_80211_tx()` and promiscuous-mode APIs allow beacon
capture, but they conflict with normal STA operation. The ESP32-S3 can
time-share between promiscuous receive and STA mode, but this adds complexity
and jitter. Appendix A is right that association/disassociation events provide
the same boundary information (card powered on/off) without any of the
complexity. TSF capture is an engineering luxury, not a necessity.

### B.3 Residual Gaps in the Combined Document

#### B.3.1 The EDF+ Annotation Channel Is Undervalued

Both §11 and Appendix A effectively dismiss the EDF+ annotation channel (the
original C5 in `as10-time-drift.md`). This is premature:

- EVE.edf and CSL.edf are EDF+ annotation files. Their annotation onsets are
  *relative* to the file's `starttime`, which is in AS10-clock time. The
  annotations describe clinically timestamped events (apneas, hypopneas, large
  leaks) that occur at specific points during therapy.
- **Post-session**, SomnoBridge bridges the entire file. SomnoTrace can then
  compare the annotation onset (AS10 time) against the O2 ring's desaturation
  event (NTP time). A 3% SpO2 desaturation following an obstructive apnea has
  a physiological delay of 15–45 s. This is coarse, but it is an *independent*
  cross-check of the offset — it uses a completely different observation
  pathway (physiological, not filesystem) and does not share any of the FAT
  quantization, `L`, or `q` noise.
- This does not improve accuracy, but it provides a **sanity check** on the
  primary estimate. If the comb-fit offset says the AS10 is +19 min 42 s
  ahead, and the apnea-desaturation correlation confirms the events align to
  within ±30 s after correction, the estimator's output gains credibility.
- **Recommendation:** add this as a validation channel (not a primary channel)
  in E7's pass criteria. No additional card load required — the files are
  being transferred anyway.

#### B.3.2 STR.edf's MaskOn/MaskOff Are a Direct Anchor

STR.edf records `MaskOn` and `MaskOff` as minute-of-day values (signals `ONT`
and `OFT` in `edf_signals.md`). These are AS10-clock timestamps. The
corresponding physical event — mask removal — is observable by SomnoBridge
through two independent channels:
1. WiFi disassociation if the AS10 powers down the SD slot at therapy end
   (immediate).
2. The cessation of file-growth in `/dir` listings (detected within one poll
   cycle).

More importantly, SomnoTrace records the O2 ring's accelerometer/HR spike at
mask-off (NTP-timestamped). The `MaskOff` value from STR.edf vs the NTP time
of the HR spike is a direct `(AS10 says X, truth says Y)` pair at ~1 minute
resolution. It requires no special card interaction and is available
post-session.

This is strictly weaker than the comb fit (~60 s resolution vs sub-second) but
provides an independent cross-validation datum per session. **Add to E7's
validation protocol.**

#### B.3.3 Record-Size Ambiguity Across AS10 Variants

The comb fit (§A.4) requires knowing the exact record size to derive `k` from
`S_j`. The EDF spec allows the record size to vary: it equals
`sum(samples_per_record × 2 bytes)` across all signals. From
`edf_signals.md`:

| File | Signals (AS variant) | Record size (bytes) |
|---|---|---|
| BRP.edf (AutoSet, 3 signals) | 1500 + 1500 + 1 = 3001 samples | **6002** |
| BRP.edf (VAuto, 4 signals) | 1500 + 1500 + 1500 + 1 = 4501 samples | **9002** |
| PLD.edf (AutoSet, 11 signals) | 30×11 + 1 = 331 samples | **662** |
| PLD.edf (VAuto, 15 signals) | 30×14 + 1 = 421 samples | **842** |
| SAD.edf | 60 + 60 + 1 = 121 samples | **242** |

The record size varies by AS10 variant (CPAP, AutoSet, VAuto, ASV).
**SomnoBridge cannot assume a fixed record size.** The correct approach: read the
EDF header once (post-session, during the file transfer it is already doing)
to extract `nr_signals` and `samples_per_record[]`, compute the record size,
and apply it retroactively to the in-session size observations. This is
compatible with the two-tier design — Tier 1 logs raw sizes; Tier 2 resolves
record counts from the header.

**Alternative for Tier 1 (without headers):** if SomnoBridge observes two
consecutive size values `S_j` and `S_{j+1}` with `S_{j+1} > S_j`, then
`ΔS = S_{j+1} − S_j` is the record size (or a multiple thereof if more than
one flush occurred between polls). A single observed `ΔS` at the start of
therapy gives the record size empirically, no header read needed. Log this
in the evidence tuple so Tier 2 can cross-check.

#### B.3.4 The "Header vs Creation" Latency Constant λ₁ Needs Care

§A.6 introduces three latency constants: `λ` (creation→flush 0),
`λ₁` (creation→header start time), `L` (stamp→visibility). The notation
suggests these are independently measurable. In practice:

- `λ₁ = 0` by definition: the EDF `starttime` *is* the creation time
  (the header is written at file creation, before any data records).
- `λ` = time from file creation to the first 60 s record being flushed to
  SD ≈ 60 s (the AS10 accumulates one full record, then writes it).
- `L` = MUX arbitration + directory-sector cache flush ≈ 0.1–2 s.

So the composite is really: the first flush appears at
`t_header + λ + L ≈ t_header + 60 + L`. What matters for the comb fit is `P`
(the period, ~60 s) and `T_0` (the time of the first flush). The header
`starttime` anchors `T_0` to `t_header + λ_composite`. If `λ ≈ 60 s` exactly
(one record period), then `T_0 ≈ t_header + P + L`, and the relationship
between the comb and the header is clean. If the first flush is at some
non-integer multiple of `P`, there is an extra constant.

**Recommendation:** E4 (UART rig) should specifically measure the interval
between the EDF header's `starttime` and the first observable size increment,
to confirm whether it is exactly `P` or carries an additional offset.

#### B.3.5 Persistent State Across Power Cycles

The GEEK's TF card is the evidence store. What happens if:
1. SomnoBridge loses power mid-session (USB unplugged, power outage)?
2. The TF card fills up?
3. The TF card is corrupt?

§7.1 mentions "write-then-rename segments" and "size-capped rotation" but
does not specify the crash-recovery contract. For a device that runs
unattended:
- **Monotonic log IDs** must survive reboot. Use a boot counter persisted to
  NVS (ESP32 flash), not derived from the TF card.
- **Evidence must be flushed frequently.** A crash during a 60 s poll cycle
  loses at most one evidence tuple. `fsync` after each append.
- **TF card absence is not fatal.** Fall back to ESP32 flash (limited, but
  sufficient for one night's worth of evidence tuples — each is ~100 bytes,
  480 per night ≈ 47 KB).

This is implementation detail, but the proposal should state the contract:
**no single failure mode (power, TF, BLE) loses more than one cycle's worth
of evidence, and the estimator degrades gracefully rather than failing
silently.**

### B.4 Implementation Sequencing Recommendation

The document contains seven experiments and a multi-component firmware design.
The risk of "analysis paralysis" is real — the experiments depend on hardware
that may not all be available immediately. A pragmatic build order:

1. **Week 1: SomnoBridge WiFi client + HTTP listing parser.**
   Join the EZShare AP, poll `/dir`, parse the HTML `<pre>` listing, log
   `{mono_us, entries[]}` to serial and/or TF. This is useful immediately for
   E0/E1 and is the foundation of all timing work.

2. **Week 2: BLE GATT server + NTP time exchange.**
   SomnoTrace connects, pushes NTP anchors. SomnoBridge logs
   `{mono_us, ntp_ms, rtt_us}`. Now the evidence log has both clocks.

3. **Week 3: Comb fit prototype (offline, on PC).**
   Take the raw logs from weeks 1–2, run the comb fit in Python, compare
   against EDF headers from a manual card pull. This validates §A.4 before
   any embedded estimator is written.

4. **Week 4+: File transfer bridge, embedded estimator, LCD status.**
   The timing subsystem is validated; the bridge is the primary SomnoBridge
   feature; the estimator runs alongside.

E4 (UART rig) can run in parallel whenever the hardware is available — it
does not block the firmware build.

### B.5 One Missing Risk: Therapy-Off Writes

The proposal assumes the AS10 writes only during therapy. This is likely true
for BRP/PLD/SAD, but STR.edf and Identification files may be updated outside
therapy (settings changes, compliance summary writes, alarm events). If the
AS10 writes to the card while SomnoBridge is not expecting it:
- A `/dir` poll during a surprise write hits the same MUX contention window.
- The evidence log may contain unexpected size/mtime changes that confuse the
  comb fit.

**Mitigation:** the comb fit is inherently robust to extra writes (they add
valid `(t_j, k_j)` tuples, or tuples for files outside the tracked set that
are simply ignored). The MUX contention risk is the same as during therapy —
bounded by the guard band. No special handling needed, but the firmware should
not assume "no writes outside therapy" when scheduling polls.

### B.6 Summary of Recommendations

| # | Recommendation | Reference |
|---|---|---|
| 1 | **Adopt Appendix A as governing.** Its comb fit, safety posture, and accuracy figures supersede both §1–§10 and §11. | B.1 |
| 2 | **Add EDF+ annotation cross-validation** to E7's pass criteria (apnea onset vs O2 ring desaturation). | B.3.1 |
| 3 | **Add STR.edf MaskOff vs O2 ring HR spike** as an independent per-session anchor in the validation protocol. | B.3.2 |
| 4 | **Handle record-size variability** by inferring ΔS empirically in Tier 1 and confirming from the EDF header in Tier 2. | B.3.3 |
| 5 | **Measure the header→first-flush interval** in E4 to confirm whether it equals exactly one period `P`. | B.3.4 |
| 6 | **State the crash-recovery contract** for evidence persistence (no silent loss, graceful degradation). | B.3.5 |
| 7 | **Follow the build sequencing** in B.4 to derisk the firmware incrementally. | B.4 |
| 8 | **Do not assume "no writes outside therapy"** in the polling scheduler. | B.5 |

---

## Appendix C — Full Revalidation (§1–§11, Appendix A, Appendix B)

> **Scope:** a line-by-line re-check of every quantitative claim and cited fact in the
> document as it stands, including Appendix A (written by the same author as §1–§10).
> No prior section is modified. Findings are graded **Critical** (changes the design
> or its headline accuracy), **Major** (changes a number, a schema or a step), or
> **Minor** (wording/fact fix, no design impact). Where this appendix conflicts with
> earlier text, **this appendix governs**.

### C.0 Verdict

The architecture holds: BLE time anchors, raw-evidence logging, a two-tier split, a
quiet-by-default poller, and record counting derived from file size. The accuracy
story does not. **Appendix A's central claim, that quiet mid-cycle polls plus a comb fit
give a sub-second offset and 1–5 ppm rate on night one, is wrong.** It relies on poll
phases that are uniform over the *whole* cycle, which contradicts the quiet-window
dither and the guard band in the same appendix. Appendix B endorsed that claim without
catching it. §C.1 proves the correct bound. §C.2 gives a safe design that actually
reaches the useful target. §C.3 shows that the useful target is **±0.5 s**, not
±0.1 s, because plain EDF headers cannot represent anything finer. Several smaller
facts across §1–§11 and Appendices A/B also need correction (§C.4–§C.6).

---

### C.1 Critical — quiet polls cannot localise the flush (this also invalidates A.4–A.6)

Appendix A.2.1 states the right principle: *a mid-cycle poll observes state, not an
event.* Appendix A.4 then breaks it.

**Derivation.** A poll at true time `t_j` that sees `k_j` records gives the constraint
`T_0 + k_j·P < t_j ≤ T_0 + (k_j+1)·P`. Write the poll phase as
`φ_j = t_j − (T_0 + k_j·P) ∈ (0, P]`. Intersecting all constraints gives

```
T_0 ∈ ( t − (k+1)P ,  t − kP )   for every poll
    ⇒ width(T_0) = P − (max φ_j − min φ_j)
```

- A.5 baseline dither `φ ∈ [+15 s, +45 s]` gives width ≥ 60 − 30 = **30 s**, i.e. `T_0`
  is known only to **±15 s**.
- A guard band `G` around the edge, with phases uniform over `[G, P − G]`, gives width
  ≥ **2G**. With the default `G = 3 s`, `T_0` is known to **±3 s**.
- The "`P/2n` ≈ 0.06 s" row in A.4 holds only when phases are uniform over the full
  `(0, P]`, including right at the edge. That is exactly the edge-hunting A.4 claims
  to avoid.

**Consequences for Appendix A:**

1. *"`G` bounds `T_0` precision but not offset accuracy"* (A.4, A.5) is **false**. Every
   offset path computes `o` from `T_k = T_0 + kP`, both the mtime path and the header
   path (§C.2.3), so the offset inherits the full `T_0` error.
2. A.6's first-night figures (±0.4–0.5 s with header pairing, 1–5 ppm timer rate) do
   **not** follow from A.5's baseline. Under the baseline they are ±3 s to ±15 s and
   ~100–500 ppm/night.
3. *"Nobody needs to poll the burst"* (A.4) is **false as a general statement**. Here is
   the corrected statement:

> **Localisation theorem (informal).** The flush comb cannot be located more precisely
> than the widest region around each edge that is never sampled. Information about an
> edge comes only from observations taken near it.

Safety and precision really do trade against each other. What *can* be engineered is
*where* the unsampled region sits and *how often* a poll risks hitting the burst. §C.2
does that.

**Field context that the document missed.** The 65 s unsynchronised polling used by
hms-cpap is **not** "no timing information", which is what §6.3 says. Against a 60 s cycle
its phase slides 5 s per cycle, so it sweeps the whole cycle every 12 polls. That is a
vernier, and it gives exactly the uniform-phase sampling the `P/2n` bound needs. By the
same token it already lands inside a ~1–2 s write burst on ~`D/P` ≈ 2–3 % of polls,
roughly **10–15 burst hits per night**. So the community prior is "some burst contact
is tolerated" (weak evidence), not "burst contact never happens".

---

### C.2 Critical — the design that does reach the useful target safely

#### C.2.1 Burst-bracketing gate (replaces both §6.2's PLL and A.5's dither)

Model the write burst as `[B, B + D]`. `D` ≈ 1–2 s on the AS11; the AS10 value is unmeasured.
Each file `f` becomes visible at `e_f ∈ [B, B + D]`. The **first** file to update shows
up at the start of the burst and the **last** at its end. Place two polls per tracked cycle:

- **Early gate** at `B̂ − g`: should see *no* file updated.
- **Late gate** at `B̂ + D̂ + g`: should see *all* files updated.

`g` is the safety margin (e.g. 0.3–0.5 s). Both polls sit **outside** the burst by
construction. When a gate flips (early sees new, or late sees old), the burst estimate
`B̂` has drifted and is shifted. This is an early/late gate with a dead zone of `D + 2g`,
tracking the burst *boundaries* rather than a file edge.

- **Localisation.** `B` is known to ±`g`, provided `D` is calibrated (E4/E0b measure
  it once per firmware family and it is stable while record sizes are fixed). The
  flush comb is then known to ±`g` ⊕ `σ_D`.
- **Contention.**
  - *Acquisition:* a binary search over the unknown interval `W` risks a hit with
    probability `D/W` per poll, so a few expected hits per session.
  - *Tracking:* no hits unless flush jitter exceeds `g`.
  - *Gate flips:* each flip means a poll touched or passed the burst edge. At a
    rate known to 5 ppm the burst moves ~0.3 ms/cycle, so flips are rare.
- **Load.** Two polls every `N` cycles. With `N = 5` that is ~200 requests/night,
  the same order as §6.3's "Balanced" profile.
- **Jitter caveat.** If the AS10's write moment jitters by more than `g` from cycle to
  cycle, the rigid comb `T_k = T_0 + kP` is the wrong model. E1 must measure the
  flush-interval distribution **before** any of this is built.

#### C.2.2 A cheaper fallback if E0b shows the card tolerates occasional contact

Run the hms-cpap-style vernier on purpose: polls at an incommensurate period (e.g.
61–65 s) plus the comb fit. This needs no gate logic, gives `T_0` ≈ `P/2n` and rate
≈ 1–5 ppm/night, and carries the same ~2–3 % burst-hit rate the field already
tolerates. The two designs differ only in risk posture. The **gate** is the safe default;
the **vernier** is a validated alternative.

#### C.2.3 Offset path: header plus comb, and mtime is optional

Assume a write is triggered when a record completes. Then:

```
T_first_flush  =  T_start_true + P + λ_w        (λ_w = record-complete → visible)
t_h            =  floor1( T_start_true + o )    (EDF header starttime, 1 s)
⇒  o_start     =  t_h + u − (T_0 − P − λ_w),    u ∈ [0, 1)
```

The offset therefore comes from the **1 s header** plus the comb. The 2 s FAT mtime is
not needed at all. That makes the header the primary offset source and the mtime a
Tier-1 convenience (a real-time estimate before the header is read), plus the only
view of the RTC *rate* (§C.3.2).

Error budget: `T_0` (±`g` ⊕ `σ_D`) ⊕ header quantisation `u` (uniform over 1 s,
σ 0.29 s) ⊕ residual `λ_w` calibration. Plausible single-night result: **±0.5–0.7 s**,
and about **±0.4 s** once `λ_w` and `D` are calibrated (E4).

A correction to A.4 item 1: pairing the header's `floor1` with the first flush's
`floor2` mtime does **not** simply shrink a 2 s bin to 1 s. The two stamps are taken
at *different instants*, ~`P + λ_w` apart, so combining them needs `λ_w` in RTC
seconds, which is the same calibration constant anyway.

#### C.2.4 Cross-night pooling is not proven

A.4/A.6 claim ±0.2–0.3 s after a week of pooled nights. Pooling needs the RTC offset
carried between nights through the RTC rate, and that rate is estimated from the same
data. A 1–2 ppm rate error over a 3–4 day lever arm is 0.3–0.7 s, which is already
larger than the claimed result. The joint offset+rate fit may still converge, but that
has not been shown. **Treat pooled figures as unproven until simulated (§C.7, E-sim).**

---

### C.3 Critical — which clocks matter, and what the output can represent

#### C.3.1 Output representability sets the useful accuracy floor

The goal is corrected **EDF files**. Plain EDF (`BRP`/`PLD`/`SAD`) stores the start time
as `hh.mm.ss`, so a corrected start can be written only to **1 s**, i.e. ±0.5 s.

- `SAD` (1 Hz) and `PLD` (0.5 Hz) cannot be shifted by less than a sample (1 s / 2 s)
  even by re-packing their data.
- `BRP` (25 Hz) *can* be aligned to ~40 ms. That means re-packing every record by `n`
  samples across record boundaries and recomputing each record's `Crc16`. It is feasible
  post-session in SomnoTrace, but it is new work.
- `EVE`/`CSL` onsets are integer seconds relative to the header start (the AS11 layout
  in airbreak-plus `edf_annotations.md`; AS10 to be confirmed).

**Consequence.** Accuracy much below ±0.5 s buys almost nothing for EDF output (BRP
re-packing aside). ±0.5 s is also one O2-ring sample, so the right target is
**"±0.5 s reliably"**. That makes the sparse edge ladders (A.5 stretch) and the
±0.1–0.2 s rows in §1, §8 and A.6 low-priority. §1's line *"comfortably inside the 1 Hz
sample spacing"* for ±1 s was also wrong: ±1 s *is* the sample spacing.

#### C.3.2 Two rates, not one

§3 defines `ε` as the *RTC* rate, and §7.4 applies a single `rate_ppm` to EDF data.
EDF records are 60 s of samples paced by the **sample timer**, not the RTC. Events very
likely use `STK`, the 10 ms tick that airbreak-plus `var_reference.tsv` calls "the event
timestamp timebase". So:

- **Timer rate** (`ε_timer`) places samples and events *within* a session. The flush
  comb period `P` measures it directly, provided a write is triggered by record
  completion.
- **RTC rate** (`ε_rtc`) matters only for (a) interpreting mtimes and (b) carrying the
  offset *between* sessions.

The §7.4 schema should carry both (`timer_rate_ppm`, `rtc_rate_ppm`) along with their
provenance. A.6 already separates the columns, but §7.4 was never updated.

#### C.3.3 The within-session rate is largely unrepresentable in EDF

§7.4 says the correction `t_as10 + drift + rate·Δt` is applied to "EDF header start times,
filenames and STR mask events". A header start has `Δt = 0`, so the rate term never
reaches the sample data. EDF can express a timer rate only by changing the record
duration field: `60.00` → e.g. `60.0012` fits the 8-character field, but whether OSCAR,
SleepHQ and ResScan tolerate it is unknown. Recommendation:

- The default is to write the offset evaluated at the **session midpoint**. That halves
  the worst-case in-session error (20–50 ppm over 8 h is 0.6–1.4 s, which becomes
  ±0.3–0.7 s).
- Use the rate term only for things carrying explicit times (EVE/CSL onsets, STR
  MaskOn/MaskOff) and in SomnoTrace's own timeline.
- Add a compatibility test (E8) for a non-integer record duration before using it.

#### C.3.4 Correction must be applied consistently, including noon-day labels and header CRCs

- OSCAR matches STR.edf sessions to DATALOG files within about a **1-minute** window
  (SomnoTrace `spec/0002-edf-export.md` §4.3.5). A 20-minute correction applied to EDF
  headers and filenames but not to STR `MaskOn`/`MaskOff` (or the reverse) breaks that
  matching. Correct all of them or none.
- The **noon-day label** must follow SomnoTrace's existing rule: labels come from *raw*
  device time. Correcting across 12:00 must not move a session to a different day folder
  (§4.3.3 of the same spec explains why).
- STR `MaskOn`/`MaskOff` are **minutes since the noon day start** (SomnoTrace
  reconstructs `local noon + MaskOn·60 s`), not "minute-of-day" as B.3.2 says. They
  have 1-minute resolution, so only offsets ≥30 s change them.
- On the AS11, the EDF patient-ID field carries **CRC16s over the header bytes**
  (airbreak-plus `edf_header.md`). If the AS10 does the same, rewriting `starttime`
  invalidates them. They must be recomputed, and E0 should confirm the AS10 layout.

---

### C.4 Major corrections to specific claims

| # | Location | Claim | Finding |
|---|---|---|---|
| 1 | B.3.3 | PLD record size 662 B (AutoSet) / 842 B (VAuto) | **Wrong.** Per airbreak-plus `edf_signals.md`, AutoSet PLD has **9** data signals + Crc16 → 9·30 + 1 = 271 samples → **542 B**. VAuto has **13** + Crc16 → 391 → **782 B**. BRP (6002 / 9002 B) and SAD (242 B) rows are correct. |
| 2 | B.3.3 | Size varies by *variant* | True, but incomplete. Firmware patched with airbreak-plus `edf_ccx_merge.py` writes **superset** files on every variant (BRP 9002 B, PLD 14 + 1 → 842 B). B's "VAuto" PLD figure is actually the superset. This reinforces B's conclusion: never hard-code sizes; infer `ΔS` and confirm from the header. Also take the **gcd** of observed `ΔS` values, since one flush may append more than one record (e.g. after a pause). |
| 3 | A.1.3 | 8.3 alias is "a truncation, not a hash"; `2071A5~1.EDF` "likely not an AS10 card" | **Wrong (my error).** FatFs (and Windows) switch to a **CRC16-hashed** alias after ~5 collisions (`gen_numname`, seq > 5; confirmed in ESP-IDF's FatFs port). A DATALOG day folder holds many `YYYYMMDD_*` files sharing a 6-character prefix, so hashed names like `20xxxx~1.EDF` are **expected**. §11.4 was closer to right. The conclusion stands either way: the filename timestamp is lost in `/dir`. |
| 4 | A.2.6 / B.2.5 | TSF needs promiscuous mode; unreadable while connected | **Overstated.** ESP-IDF provides `esp_wifi_get_tsf_time(WIFI_IF_STA)`, which returns the associated AP's TSF directly (valid after one beacon, and inaccurate under power save other than modem sleep). B.2.5's `esp_wifi_80211_tx()` is a *transmit* API and does not apply. Value is still low: TSF is the *card's* crystal, says nothing about the AS10, and its resets (card reboots) are rare behind the EZShare MUX. Keep it as a free log field, not a channel. |
| 5 | A.3.4 | Odd second `10:24:01` "impossible" in a FAT mtime | Correct for the 2 s *write-time* field. However, the FAT **creation** time has an extra 10 ms field (`DIR_CrtTimeTenth`, 0–1.99 s), so odd seconds are possible there. An odd second in real listings would suggest EZShare shows **creation** time, in which case per-flush mtime (Channel 1's `m_k`) is **not visible at all** and only size remains. This makes the header path (§C.2.3) the only offset source and must be settled in E0. |
| 6 | B.3.4 | `λ₁ = 0` "by definition" | **Not by definition.** Many writers create the file and write the header together with the *first record*, so the file may first appear ~`P` after `starttime`, and the FAT create time may be ~60 s later than the header. B's recommendation (measure header → first visible growth in E4) is right; the premise is an assumption. |
| 7 | B.3.2 | WiFi disassociation at therapy end is an immediate anchor | **Unlikely to exist.** Per the maintainer, the EZShare's hardware MUX means no slot power-cycling. Whether the AS10 cuts slot power at therapy end is unknown and probably not the case. Growth stopping (within one poll) is the real end marker. |
| 8 | B.3.5 | ~100 B per evidence tuple, 47 KB/night | Too low if full listings are logged. A day folder with 10–20 entries is ~300–600 B per poll, i.e. **~150–300 KB/night**. Log **changed entries only** (deltas), plus a full snapshot per session, to reach B's figure. The NVS boot counter and one-cycle loss contract are sound. |
| 9 | §6.3 | 65 s unsynchronised polling yields "*none*" | **Wrong.** It is a vernier (§C.1). It carries timing information *and* ~2–3 % burst contact. |
| 10 | §7.3 | Treat estimates near multiples of 3600 s as TZ/DST | **Fragile.** Real zones step by 15/30/45 min (SomnoTrace's `as11_time` snaps to 15 min), and that overlaps the ~20-min drift range. Instead, compare AS10 stamps as **ESP-local wall time**, so the zone cancels if the AS10 was set to local time. Flag ±1 h steps **coinciding with the ESP TZ's DST transition dates**, rather than classifying offsets by size. |

### C.5 Minor corrections

- **§11.2** gives flow sampling as "25–50 Hz" and pressure as "40 Hz". Per the EDF
  tables, both are **25 Hz** (40 ms). §11.2 also asserts "deterministic ~50–80 ms"
  quiet-window RTTs; that is unmeasured (E6).
- **§2** cites hms-cpap for "files grow every ~60 s". That README describes both the
  FYSETC and EZShare paths. Growth being visible *through an EZShare listing* is
  likely, but it should be confirmed in E0 rather than cited as established.
- **§4.5 / A.2.4 / B.2.2** agree that noon-folder creation is quantised. It is correct
  as a 1/day anchor.
- **A.2.5**: the "≥60 s buffer" argument covers overrun. It does not cover the real
  failure mode, which is an **SD command timeout** on the AS10 while the card is busy
  (SD write-busy and read timeouts are hundreds of ms). E0b must measure worst-case
  listing service time *during* a burst, not just average RTT.
- **B.3.1**: apnea → desaturation lag is ~15–45 s with large spread, so it is a coarse
  sanity check only, and only for users with an O2 ring. Agreed as a validation check,
  not a channel.
- **Arithmetic re-checked and correct:** §3 (0.6–2.9 s/night at 20–100 ppm), §4.3
  (0.82 ppm at n = 480; 2.6 ppm at n = 48), §4.4 (1.2 ms/cycle slip at 20 ppm; 2/(N+1)
  envelope for *uniform* phases), A.2.2 (9.8 ppm, not 0.01 ppm).

### C.6 Integration note not covered anywhere

SomnoTrace's BLE stack has a history of fragility (AS11 supervision-timeout disconnects,
coexistence tuning). An AS10 user has **no AS11 link**, so SomnoBridge takes that slot
(SomnoBridge + O2 ring = two links, as today) instead of adding a third. Still, the
time-exchange and file-transfer traffic should follow SomnoTrace's existing
connection-parameter and coexistence settings, and share its reconnect logic rather
than reinvent it.

---

### C.7 Revised experiment order

| Step | Experiment | Why first |
|---|---|---|
| **E-sim** | Monte-Carlo simulator of the estimator. Parameters: `P`, `D`, flush jitter, `L`/`λ_w`, `ε_timer`, `ε_rtc`, `g`, poll policy (gate / vernier / dither), mtime-vs-creation listing. Run the estimator on synthetic logs and report bias and error | Every accuracy figure in this document is a hand estimate, and two reviews missed §C.1. A ~200-line Python sim would have caught it immediately. Build it **before** firmware; it becomes the E3 offline fitter |
| **E0** | Card profile: real listing format (8.3/hashed/long names; **mtime or ctime**; second parity), whether sizes grow per flush, `Date:` header, firmware version, AS10 header CRC layout | Decides whether Channel 1 exists at all (§C.4.5) |
| **E1** | Flush-interval and burst-duration distribution (`P`, `D`, jitter) from a vernier run at 61–65 s | Decides whether the rigid comb model and the gate's `g` are valid (§C.2.1) |
| **E0b** | Contention: worst-case listing service time *inside* vs *outside* the burst; AS10 SD errors; EDF `Crc16`/record counts | Picks gate vs vernier (§C.2.1–C.2.2) |
| **E4** | UART ground truth (`TIC` bisection, `STK`): `λ_w`, `D`, locked-vs-independent, header → first-visible interval | Calibrates the header path to ~±0.4 s |
| **E8 (new)** | Consumer tolerance: OSCAR, SleepHQ and ResScan with (a) rewritten `starttime` plus recomputed header CRCs, (b) non-integer record duration, (c) re-packed BRP | Decides what corrections can be *written* (§C.3.1–C.3.4) |
| E2, E5–E7 | As in A.7 | Unchanged |

### C.8 Revised expectations (supersede §1, §8, A.6)

| Regime | Absolute offset at session start | In-session placement | Notes |
|---|---|---|---|
| Quiet dither only (A.5 baseline) | ±3 s (G = 3) to ±15 s | poor | **Do not ship as the timing design** |
| Burst-bracketing gate, uncalibrated `D`/`λ_w` | ~±0.7–1.0 s | timer rate from gate, ~5–20 ppm | Safe by default |
| Gate + E4 calibration + header path | **~±0.4–0.5 s** | midpoint offset: ±0.3–0.7 s worst case (±0.1 s if record-duration rate is accepted, E8) | **Target; matches EDF 1 s representability** |
| Vernier (if E0b allows) + E4 | ~±0.3–0.4 s | 1–5 ppm | Limited by the 1 s header, not by timing |
| Header unavailable, mtime only | ±1–1.5 s | as above | Tier-1 real-time estimate only |

All rows are still estimates. E-sim must reproduce them before any are quoted.

### C.9 Bottom line

- **Keep:** the architecture (§5, §7.1), the quiet-by-default safety posture (§11), the
  two-tier split (§11.5), record counting from size (§11.2/A.3.1), the per-card
  record-size inference (B.3.3, with corrected numbers), and the crash-recovery
  contract (B.3.5).
- **Withdraw:** A.4's claim of sub-second quiet-poll precision and A.6's first-night
  and pooled figures.
- **Replace them with:**
  - a burst-bracketing gate that tracks burst *boundaries* from outside (§C.2.1), or
    the vernier if contention proves benign;
  - the header+comb offset path (§C.2.3);
  - separate timer and RTC rates (§C.3.2);
  - a midpoint-offset output policy (§C.3.3);
  - consistent correction across STR, EDF, filenames and header CRCs, without moving
    noon-day labels (§C.3.4).
- **Target ±0.5 s reliably**, which is what plain EDF can express and one O2-ring
  sample. Do not target ±0.1 s.
- **Build the simulator first.**

// The above appendix C is written by Claude Opus 5.5 High //


---

## Appendix D — Revalidation of Appendix C, and Final Pass

> **Appended by GLM-5.3 Max.**
> **Scope:** independent re-derivation of Appendix C's claims (C was authored
> separately, per the note above), plus a final consistency pass over §1–§11 and
> Appendices A/B. No prior section is modified. Where this appendix conflicts with
> earlier text, this appendix governs.

### D.0 What I think

Appendix C's central criticism of Appendix A is **correct and important** — I
re-derived the interval-intersection bound independently and it holds. C's
architectural direction also stands: header-derived offset, two separate rates,
midpoint output policy, consistent correction across STR/EDF/filenames, simulator
first. However, **Appendix C contains one significant error of exactly the class it
criticises** (its vernier example assumes phase coverage it does not get), one
internally inconsistent accuracy row, and several under-specified points that matter
for implementation. All are fixable without moving C's conclusions: the ±0.5 s
target survives, and the experiment order survives with one correction.

### D.1 Findings against Appendix C

#### D.1.1 Critical — the vernier example in C.2.2 (and C.7's E1 row) is wrong

C.2.2 proposes "polls at an incommensurate period (e.g. 61–65 s)" and claims this
"needs no gate logic, gives `T_0 ≈ P/2n`". **It does not.** A vernier with poll period
`Q` advances its phase by `a = Q − P` per poll. The phases form a *grid* of spacing
`a`, not a uniform sample of the cycle:

```
phase grid   = a                ⇒  T_0 known to ±a/2
sweep time   = P/a polls        (one full pass over the cycle)
hits/night   ≈ (D/P)·n          for a ≤ D  — independent of a
               but for a > D: 0 or (a/P)·n  — lumpy, and can be WORSE
```

- `Q = 65 s` → `a = 5 s` → grid 5 s → **`T_0` to ±2.5 s**, not sub-second. The claim
  "`T_0 ≈ P/2n`" holds only when `a ≈ P/n` (~0.125 s), i.e. one sweep per night.
- `Q = 61 s` → `a = 1 s` → `T_0` ±0.5 s — marginal, no better than the header
  quantum it feeds.
- C.1 attacked A.4 for assuming "phases uniform over the whole cycle"; C.2.2 then
  assumes the same thing about a 5 s-step vernier. Same error class, smaller stakes.

**Correction.** Choose the vernier step deliberately, bounded below by the
observation blur (§D.2) and above by the burst width `D`:

```
recommended a ≈ 0.2–0.5 s   (Q ≈ 60.2–60.5 s)
→ T_0 ≈ ±0.1–0.25 s, full sweep every 2–5 h, expected contact (D/P)·n ≈ 16/night
```

The `a > D` lumpiness matters: a 5 s-step vernier whose grid happens to sit on the
burst hits it **every sweep** (~40/night at 480 polls), worse than the fine vernier —
another reason fine steps dominate. C.1's "10–15 burst hits per night" for hms-cpap
is correct as an *expectation* over random relative phase, but the realisation is
lumpy in the same way.

#### D.1.2 Critical — C.8's vernier row contradicts C.3.1's own floor

C.8 lists "Vernier + E4: ~±0.3–0.4 s". C.3.1 (correctly) shows the EDF header
quantum bounds any single-session offset to **±0.5 s worst case** (σ 0.29 s) no
matter how precisely `T_0` is known — `t_h = floor1(τ)` loses the sub-second, and no
other stamp recovers it (the 2 s mtime is coarser). Once `T_0` error drops below
~0.2 s, gate and vernier **converge on the same offset accuracy**; their differences
are safety, rate precision and robustness, not offset. C.8 also mixes worst-case and
1σ conventions across rows without saying so.

**One exception worth hunting:** the FAT *creation* time carries a 10 ms field
(`DIR_CrtTimeTenth`). If the EZShare listing ever renders creation time *including
tenths*, the 1 s header floor breaks and sub-second offset becomes possible. This is
checkable in E0 for free and is the only known single-session route below ±0.5 s.

#### D.1.3 Major — the gate formulas omit visibility latency, and staleness makes them asymmetric

C.2.1's late gate is `B̂ + D̂ + g`, but "all files updated" requires the last
directory-sector write to be **visible**, not merely written: the gate must sit at
`B̂ + D̂ + L + g`, with the dead zone `D + L + 2g` and `σ_D` including the variance of
`L`. More subtly, if the card serves stale listings (assumption A3):

- an *early-gate violation* (update already visible before `B̂ − g`) is trustworthy —
  staleness cannot fabricate an update;
- a *late-gate delay* (update not yet visible after `B̂ + D̂ + L + g`) is **not**
  trustworthy — staleness mimics "burst later than predicted".

The gate's control law should therefore be one-sided: react to early violations
immediately; treat late-side anomalies as *suspect*, not as evidence the burst moved.
C.2.1 does not say this, and a symmetric bang-bang law would mis-track under A3.

#### D.1.4 Major — gate flips are jitter-driven, not rate-driven

C.2.1: "At a rate known to 5 ppm the burst moves ~0.3 ms/cycle, so flips are rare."
True but irrelevant: at 5 ppm the burst *prediction* drift is negligible, and what
actually moves the burst between cycles is **flush jitter**. The gate's safety claim
("no hits unless flush jitter exceeds `g`") and its flip rate are the *same*
condition. So E1's jitter measurement is not just "before any of this is built" — it
is the **deciding** experiment between the gate (needs jitter < g) and the vernier
(tolerates jitter by design, averaging it in the fit). This strengthens C.7's
ordering but changes its rationale.

#### D.1.5 Major — E1 cannot measure the burst duration `D` from listings

C.7's E1 row says "Flush-interval and burst-duration distribution (`P`, `D`,
jitter) from a vernier run". A listing vernier localises each burst to the grid
(±a/2), which yields `P` and interval jitter, but `D` (the 1–2 s burst width) is
invisible unless a poll lands *inside* the burst and shows a partial update — rare
on a fine grid, unreliable under staleness. `D` must come from E0b (deliberate
in-burst dense polling), E5 (response-latency signature), or the UART rig's event
stream. Fix E1's row: it measures `P` and jitter only.

#### D.1.6 Major — the header-semantics ambiguity is `P`-sized, not a small constant

C.2.3 derives `o_start = t_h + u − (T_0 − P − λ_w)` assuming `starttime` = session
start. C.4.6 itself notes the file may be *created* at the first flush. If
`starttime` actually records file-creation time rather than therapy start, `o` is
wrong by ~`P` (60 s), which is not a calibration constant anyone would absorb
silently. Circumstantial evidence says `starttime` = session start (OSCAR users
would have noticed minute-late sessions), but this is exactly the kind of assumption
this document exists to kill. **E4 must compare a UART-known session start against
the header `starttime` explicitly** — it is a 60 s-class question, not a 0.1 s one.

#### D.1.7 Minor — acquisition safety is unspecified everywhere

A.5 claimed "no edge approach at all"; C.2.1's acquisition says "a few expected
hits". Neither states the safe procedure: before the phase is known, *any* poll can
land in the burst. Options: (a) accept the ~`D/P` ≈ 3 % per-poll contact the field
already tolerates during a coarse sweep, or (b) only poll after two consecutive
unchanged listings (a confirmed quiet gap). Pick one deliberately in E0b; do not
leave it to the implementation.

#### D.1.8 Minor — numeric inconsistencies to harmonise

- **ppm ranges:** §3 and C.5 use 20–100 ppm (0.6–2.9 s/night); C.3.3 and C.8 use
  20–50 ppm (0.6–1.4 s). The midpoint policy's worst case should be quoted on one
  range: ±0.3–1.4 s at 20–100 ppm.
- **A.4's guard row is 2× optimistic** (repeated by C.2's "100–500 ppm"): the phase
  interval excluded by guard `G` is `2G` wide, so `δP/P ≈ 2G/(k_max·P)` → ~208 ppm
  at `G = 3`, and the dither case (width 30 s) is ~1000 ppm, not 500.
- **C.4.5 is incomplete:** a plain FAT *creation* time is also 2 s-quantised;
  odd seconds require the renderer to include `DIR_CrtTimeTenth`. The `10:24:01`
  sample therefore has three live explanations — ctime-with-tenths, the card's own
  clock, or an idealised sample. If it is the card's clock, A1 dies in a *different*
  way (stamps may be meaningless), which is worse than the ctime hypothesis. E0
  must classify all three.

### D.2 The observation blur is the universal floor

Every edge-localisation scheme — gate, vernier, ladder — ultimately asks "was the
update visible when this poll's response was generated?" The response reflects card
state at an unknown instant inside `[t_request, t_response]`, and the card's
processing latency varies (§8 already carries this as "HTTP observation window,
50–150 ms"; C.5 notes §11.2's RTT claims are unmeasured). Consequences:

- the gate's `g` and the vernier's step `a` must both exceed the blur, or the
  sub-blur precision is fictional;
- once `T_0` error < blur, *all* designs are blur-limited for timing and
  header-quantum-limited (±0.5 s) for offset — the differences between them reduce
  to safety and rate, as D.1.2 says;
- E6 (RTT distribution on the host rig) is therefore not just calibration — it sets
  a design parameter.

### D.3 Independently confirmed (no change)

- **C.1's interval-intersection derivation** — re-derived from scratch; `width(T_0)
  = P − (max φ_j − min φ_j)` is correct, as are the dither (±15 s) and guard (±G)
  consequences.
- **PLD record sizes (C.4.1)** — re-counted from airbreak-plus `edf_signals.md`:
  AutoSet 9 data signals + Crc16 → 542 B; VAuto 13 + Crc16 → 782 B; superset 842 B.
  BRP 6002/9002 B and SAD 242 B confirmed.
- **FatFs hashed aliases (C.4.3)** — confirmed against the ESP-IDF FatFs port
  (`gen_numname`, CRC16-CCITT for seq > 5).
- **`esp_wifi_get_tsf_time()` (C.4.4)** — confirmed in the ESP-IDF reference
  (returns the associated AP's TSF; power-save caveats as stated).
- **EDF representability floors (C.3.1)** — 1 s header; SAD/PLD sample shifts
  1 s/2 s; BRP 40 ms with re-pack + CRC recompute.
- **STR/OSCAR facts (C.3.4)** — minutes-since-noon, ~1-minute matching window,
  noon-day labelling rule — all match SomnoTrace `spec/0002-edf-export.md`.
- **C.5's arithmetic re-checks** — reproduced.

### D.4 Corrected single source of truth (supersedes C.8)

Worst-case and 1σ stated explicitly; single-session, post-E4 calibration.

| Design | Offset (worst / 1σ) | In-session placement | Expected burst contact |
|---|---|---|---|
| Burst-bracketing gate (needs jitter < g) | **±0.5 s / 0.29 s** | rate 5–20 ppm; midpoint → ±0.3–1.4 s (20–100 ppm) | ~0 after lock; few during acquisition |
| Vernier, step a ≈ 0.2–0.5 s | **±0.5 s / 0.29 s** | timer rate 1–5 ppm; midpoint as above | ~(D/P)·n ≈ 16/night, any full-coverage schedule |
| Quiet dither only (A.5 baseline) | ±3–15 s | poor | ~0 | 
| mtime only, header unavailable | ±1–1.5 s | as above | as designed |

Sub-±0.5 s single-session offset exists **only** if the listing renders creation
time with tenths (E0) or via BRP sample re-packing (E8). Otherwise the header
quantum is the wall, and gate vs vernier is a safety/rate trade, not an accuracy
trade.

### D.5 Killers that would change the conclusions

1. **Listing timestamps are the card's own clock (E0)** — Channel 1 dies, Tier-1
   real-time estimates die, and the header path survives alone (±0.5 s) with weaker
   appearance-window anchors.
2. **Flush jitter > 1 s (E1)** — the rigid comb dies with it the gate; the vernier
   becomes an interval regression; expect ~±2 s.
3. **`starttime` ≠ session start (E4)** — a 60 s-class bias until measured once.
4. **Contact is harmful (E0b)** — the vernier dies too; the honest floor is quiet
   dither (±3–15 s) plus post-session header pairing (±2–5 s) — still ~100× better
   than the current 20 minutes, and the document should say so plainly rather than
   promise ±0.5 s unconditionally.

### D.6 Document health

The document now carries four generations of accuracy tables (§1/§8 → A.6 → C.8 →
D.4) and three "this governs" declarations (A, B endorsing A, C). That is fine as a
review record but will confuse a future implementer. Recommendation for the
maintainer's next edit (not done here, to honour the no-modification rule): collapse
§1/§8 into a short "current design" statement pointing at D.4, and mark §6.2/A.5's
polling phases as historical. The E-sim (C.7) remains the correct first build — this
appendix's D.1.1/D.1.2 findings are precisely the class of error a simulator catches
and hand-derivation misses.

— appended by GLM-5.3 Max

---

## Appendix E — Revalidation of Appendices C and D

> **Appended by Devin.**
> **Scope:** re-derivation of the claims in Appendix D against the text of Appendix C
> it reviews, plus a consistency pass over the whole document. No prior section is
> modified. Where this appendix conflicts with C or D, this appendix governs.

### E.0 Verdict

Appendix D's arithmetic is sound, and its two best contributions — the vernier
*grid* correction (D.1.1) and the observation-blur floor (D.2) — are real and should
stand. But D makes **one conceptual error that inflates the whole accuracy story**:
it treats the ±0.5 s EDF header quantum as a *measurement* floor on what we can
learn, when it is only an *output-format* floor on what a plain EDF file can carry.
Those are different things, and the difference is exactly what SomnoTrace can
exploit. D also rejects one measurement (burst width from listings) that a fine
vernier does in fact provide, and it over-grades two engineering notes as "Major".

Net: the design direction from C survives intact; D's corrections to C survive with
one reversal (D.1.5) and one reframing (D.1.2); and the acquisition problem that
neither C nor D solved (D.1.7) has a clean staged answer (§E.3).

### E.1 Confirmed from Appendix D (no change)

- **D.1.1 grid arithmetic.** A rigid 65 s poll against a rigid 60 s comb produces
  phases `φ_0 + 5j mod 60`, so after a full sweep `width(T_0) = P − (max φ − min φ)
  = 5 s` → ±2.5 s. C.2.2's "`T_0 ≈ P/2n`" indeed assumed uniform coverage it does
  not get. The `a > D` lumpiness argument (a coarse grid that lands on the burst hits
  it every sweep) is also correct: `(a/P)·n = 40` hits/night vs the `(D/P)·n ≈ 12–16`
  expectation. **Caveat in §E.2.4** — the rigid-grid model is pessimistic in practice.
- **D.1.3 one-sided control law.** Under listing staleness (A3), an *early* violation
  is trustworthy and a *late* one is not. Correct. But C.2.1 defined `e_f` as the
  *visible* update time, so `L` was already folded into C's `B` and `D`; D.1.3 is a
  useful clarification, not an omission. Downgrade to Minor.
- **D.1.4 flips are jitter-driven.** Correct, and C.2.1 already carried the jitter
  caveat. The reframing (E1 *decides* between gate and vernier) is right.
- **D.1.8 arithmetic.** `2G/(k_max·P) = 6/28 800 = 208 ppm` and `30/28 800 ≈ 1040 ppm`
  reproduce. Three-way classification of the odd-second sample is right.
- **D.2 observation blur** is the correct universal floor for edge timing. `g` and
  `a` must both exceed it.
- **D.3 spot-checks.** Re-counted PLD from the airbreak-plus **SX567** table (the
  AS10-family reference, not the AS11 one): AutoSet columns = 9 data signals × 30 ×
  2 B + Crc16 = **542 B**; VAuto = 13 → **782 B**. Confirmed, and confirmed as
  AS10-relevant.

### E.2 Corrections to Appendix D

#### E.2.1 Critical — D.1.2 conflates the offset quantum with correction accuracy

D.1.2 says the header quantum "bounds any single-session offset to ±0.5 s … no
matter how precisely `T_0` is known", and D.4 then reports ±0.5 s as *the* accuracy
of every design. That is true of the quantity `o` (device − true) and **irrelevant to
the correction we actually apply**. Follow C.2.3's own model:

```
T_first_flush = T_start_true + P + λ_w          (observed via the comb, true time)
⇒ T_start_true = T_0 − P − λ_w                  (no o, no u, no header involved)
record k covers [T_start_true + kP, T_start_true + (k+1)P]   in true time
```

The corrected EDF start time and every in-session sample position come **directly
from the comb**, with error `±a/2 ⊕ blur ⊕ σ(λ_w) ⊕ σ_jitter` — plausibly
**±0.2–0.3 s** after calibration, *not* ±0.5 s. The header's `u ∈ [0,1)` enters only
when we need `o` itself, and `o` is needed only for:

- STR fields (minute resolution — `u` is invisible there),
- carrying the offset across nights (C.2.4, unproven anyway),
- filenames / FAT stamps (2 s resolution).

So the three quantities the document keeps blurring must be separated:

| Quantity | What limits it | Plausible value |
|---|---|---|
| Session start in *true* time (what we write) | comb: `a/2`, blur, `λ_w`, jitter | ±0.2–0.3 s |
| Offset `o` (device − true) | above ⊕ header `u` (σ 0.29 s) | ±0.5 s worst, 0.29 s 1σ; `/√m` over `m` sessions per night |
| What a **plain EDF file** can carry | 1 s header start; SAD/PLD 1 s / 2 s samples | ±0.5 s rounding, *known* and deterministic |

Two consequences D missed:

1. **The hand-off (§7.4) must carry the millisecond-precise start**, and SomnoTrace's
   own metadata (it already stores `clock_drift_ms`) should keep it. The 1 s rounding
   happens *only at EDF export*, and consumers reading a 1 s field can do no better
   anyway. Rounding a well-known value to 1 s is not a measurement error.
2. **Multi-session nights average `u`.** Each mask-on session has an independent `u`
   with the same `o`, so `σ_o ≈ 0.29/√m`. Bathroom-break nights are common; this is
   free precision on `o` that nobody in §1–D counted.

D.1.2's "only route below ±0.5 s is `DIR_CrtTimeTenth`" is therefore wrong as
stated: it is the only route below ±0.5 s **for `o`**; for the corrected timeline the
comb already gets there.

#### E.2.2 Major — D.1.5 is wrong: a fine vernier does measure `D`

D.1.5 claims burst width `D` is "invisible unless a poll lands inside the burst".
With a fine vernier (`a ≈ 0.2–0.5 s`) the sweep necessarily passes *through* the
burst, and it localises **each file's edge `e_f` separately** to ±`a/2`. Then
`D = max_f e_f − min_f e_f` is measured to ±`a`, and the per-file write *order*
(which file the AS10 writes first) comes for free. Partial-update polls (some files
new, some old) are the *same* observations, not a rare bonus. C.7's E1 row stands as
written, under the same "contact tolerated" precondition the vernier itself needs.
What D.1.5 gets right is that a *gate* cannot measure `D` — it only brackets it.

#### E.2.3 Major — D.1.6 is real but has a cheaper test, and one observation may dissolve it

D.1.6 wants a UART rig to check `starttime` = session start rather than
file-creation time. Two shortcuts:

- **Existing data already discriminates a 60 s error.** STR `MaskOn` (minutes since
  noon, device clock) versus the BRP/PLD header `starttime` (same device clock) on
  any real AS10 card: if they agree to < 1 minute across many sessions, the header is
  session start, not first-flush time. No hardware needed; add to E0.
- **The file may appear header-only.** If the AS10 creates the EDF set and writes
  the 256 + 256·ns-byte header *at mask-on*, before the first record completes, the
  new files appear in the listing with `size == header_size` at session start. That
  event pins `T_start_true` **directly**, with no `P`, no `λ_w`, and no first-flush
  ambiguity — and it removes the largest calibration constant in C.2.3's error
  budget. E0/E4 must record the *first observed size* of each new file. This is
  the single most valuable cheap observable that none of §1–D lists.

#### E.2.4 Major — D.1.1's fine-vernier recommendation has two blind spots

- **Latency to first fix.** `a = 0.2–0.5 s` needs `P/a = 120–300` polls
  (2–5 h) for one sweep. A 90-minute session, or a night that ends early, gets **no
  localisation at all**; the coarse 65 s vernier gives ±2.5 s in 12 minutes. Fine
  steps are the right *steady state*, not the right *start*. See §E.3.
- **Contact profile changes shape.** Each fine sweep crossing the burst makes
  `D/a ≈ 4–10` **consecutive** in-burst polls, not isolated hits. If the MUX or the
  AS10's write path has any cumulative sensitivity (retry back-off, buffer pressure),
  consecutive contact is the case to test in E0b — the field prior (hms-cpap, isolated
  hits) says nothing about it.
- **Rigid grid is pessimistic** (in D's favour, but the simulator must model it):
  SomnoBridge's own poll scheduling jitter, AS10 flush jitter, and the rate mismatch
  `60·ε ≈ 3 ms/cycle` at 50 ppm (≈ 36 ms grid shift per 65 s sweep) all fill the
  grid over a night. A 65 s vernier over 8 h is therefore better than ±2.5 s in
  practice — how much better is a simulator question, not a hand-derivation one.

#### E.2.5 Minor

- D.5 item 2 reads "the rigid comb dies with it the gate" — garbled; intended "dies,
  and with it the gate".
- D.4 quotes "±0.5 s / 0.29 s" as the *offset* column for both active designs and
  then says the designs differ only in safety/rate. After §E.2.1 they also differ in
  **time-to-fix** and **`D`-measurability**, both of which favour a hybrid.
- D.4's "Quiet dither only ±3–15 s" is a worst-case interval width while the other
  rows are worst/1σ pairs; label it.
- D.1.7's option (b) "poll only after two consecutive unchanged listings" is
  incoherent as an acquisition rule: two unchanged listings 60 s apart bracket a
  flush between them by construction — they do not certify a quiet gap.

### E.3 Synthesis — staged acquisition, which C and D both left open

The two designs in C.2 and the two vernier steps in D.1.1 are not alternatives; they
are **phases** of one controller:

```
Phase 1  Coarse vernier    Q = 65 s, 12 polls (~13 min)   → T_0 to ±2.5 s
                           expected contact: ≤1 poll
Phase 2  One ladder pass   a ≈ 0.2 s across the ±2.5 s window, ~25 polls
                           → B and B+D each to ±0.1 s; D and file order measured
                           contact: D/a ≈ 5–10 consecutive polls, ONCE per session
Phase 3  Gate tracking     C.2.1 early/late gates at g ≈ 0.3–0.5 s, N = 5
                           one-sided law (D.1.3); contact ≈ 0 while jitter < g
Phase 3' Fine vernier      if E1 shows jitter > g: a ≈ 0.3 s continuously
                           (interval regression, D-tolerant), ~16 contacts/night
```

Time to a sub-second fix: ~40 polls ≈ 40 min. Total contact per session: ~10 polls in
Phase 2, then near zero. If **E0b bans contact entirely**, skip Phase 2 and let the
Phase 3 gates converge from ±2.5 s by halving the dead zone from outside (C.2.1's
"acquisition" — now well-defined, ~6 flips, no contact). If the **header-only
appearance** of §E.2.3 is real, Phase 1 also gets a free ±(blur) anchor at mask-on.

### E.4 Corrected expectations (supersede C.8 and D.4)

Single session, after E4 calibration of `λ_w`; worst-case / 1σ where meaningful.

| Quantity | Gate (jitter < g) | Vernier / hybrid (§E.3) | Quiet dither only | Header + comb unavailable (mtime only) |
|---|---|---|---|---|
| Session start, true time (ms in hand-off) | ±0.3–0.5 s / 0.2 s | **±0.2–0.3 s / 0.15 s** | ±3–15 s | ±1–1.5 s |
| Offset `o`, one session | ±0.5 s / 0.29 s | same, `/√m` over m sessions | ±3–15 s | ±1–1.5 s |
| Plain EDF representability | 1 s header, 1 s / 2 s samples — a **known rounding**, not an error | same | same | same |
| In-session timer rate | 5–20 ppm | 1–5 ppm | 100–1000 ppm | — |
| Burst width `D` | bracketed only | **measured** to ±a | no | no |
| Time to sub-second fix | from ±2.5 s in ~6 flips (~30 cycles) | ~40 polls | never | — |
| Contact per session | ≈ 0 (+ acquisition) | ~10 once, then ≈ 0 (gate) or ~16/night (fine vernier) | 0 | as designed |

The **useful target remains ±0.5 s in the EDF file** (C.3.1 was right about what
plain EDF can carry), but the *measured* timeline that SomnoTrace stores should be
**±0.2–0.3 s**, and that is what BRP re-packing (E8) or any future sub-second
consumer would draw on.

### E.5 Amendments to the experiment plan

- **E0 (card profile) — add:** (i) first observed size of every new file — is it
  `header_size` (created at mask-on) or `header + record` (created at first flush)?
  (ii) whether all files of a session appear in the same listing; (iii) the odd-second
  three-way classification (D.1.8); (iv) STR `MaskOn` vs header `starttime` agreement
  on an existing card (§E.2.3, no hardware).
- **E0b (contention) — add:** consecutive in-burst polls (5–10 in a row), not only
  isolated hits.
- **E1 — restore** `D` and per-file write order to the deliverables (§E.2.2), via
  the fine-vernier or the Phase-2 ladder.
- **E-sim — must include:** poll-schedule jitter, flush jitter, rate-mismatch grid
  filling, listing staleness, and the staged controller of §E.3, so that D.1.1's
  rigid-grid pessimism and C.2.2's uniform-coverage optimism are both replaced by a
  number.
- **§7.4 hand-off — add** `session_start_true_ms` alongside the 1 s fields; the EDF
  rounding is applied at export by SomnoTrace, never before.

### E.6 Bottom line

- Appendix C's diagnosis of A stands; Appendix D's grid and blur corrections stand.
- **D's ±0.5 s "wall" is a file-format wall, not a knowledge wall.** Measure to
  ±0.2–0.3 s, store it, round only at export.
- Use a **staged controller** — coarse vernier → one ladder pass → gate — instead of
  choosing between C's gate and D's fine vernier.
- Before any of it, **look at the first size a new EDF file appears with**. If it is
  header-only, the hardest calibration constant in this document disappears.
- Build the simulator first (unchanged since C.7); it would have caught C.2.2, D.1.2
  and D.1.5 alike.

— appended by Devin
