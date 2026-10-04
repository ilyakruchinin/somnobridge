# AS10 Clock-Drift Detection — Consolidated Design

**Status:** design, pre-implementation. Supersedes `proposal.md` (§1–§11, Appendices
A–E), which is kept as the review record. Every figure here is an estimate to be
reproduced by the simulator (§10, E-sim) before it is quoted anywhere else.

## 0. Constraints that shaped this version

| Constraint | Consequence |
|---|---|
| **No wired access** to the AS10 (no UART, no service port). | Nothing can be calibrated against the AS10's own clock. All latencies between "AS10 writes" and "SomnoBridge sees it" become **bounded priors**, not measured constants (§7). |
| **Good accuracy early** — within 30 min, preferably 5 min. Higher polling is acceptable for a short time, at a reasonable rate. | A **staged acquisition** (§5): dense but short polling right after therapy start, then very sparse tracking. Sub-second by ~minute 4, ~30 requests. |
| **Drift is accumulated over the night and read as metadata** by SomnoTrace when it pulls the last file; SomnoTrace stores it and applies it to the EDFs *it* generates. | SomnoBridge never rewrites anything. It publishes a per-session drift record (§8) at ms precision; SomnoTrace rounds only at EDF export. |

Everything else from the review record still applies: no writes to the card, evidence
logged raw so estimators can be re-run, and load kept modest and bounded by the
hard guardrails in §5.7.

## 1. Result in one page

**Primary observable:** the AS10 appends one 60 s record to each open EDF file about
once a minute. Seen through EZShare directory listings, that is a file-size step per
file per minute — a *comb* in true time, `T_k = T_0 + k·P`. SomnoBridge locates the comb
edges by polling the listing at chosen moments and timestamping the answers on an
NTP-anchored clock. Two quantities fall out:

- `T_0` (true time of the first flush) → **true session start** `= T_0 − P − λ_w`;
- `P` (true period) → the AS10's **sample-timer rate**.

**Secondary observable:** the EDF header `starttime` (1 s, AS10 clock), read once by
a single bounded probe (§5.4) a few minutes into the session — a `Range` read of
just the header bytes if E0 verifies `Range`, else one `GET` of the smallest open
EDF ≤16 KiB, else deferred to post-therapy. Together with `T_0` it gives the
**clock offset** `o = AS10 − true`.

**Controller:** watch → detect → coarse (5 s polls, one cycle) → ladder (bisect the
edge, two cycles) → tracking (one quiet-window liveness poll every fitted cycle;
that poll is upgraded to an early+late gate pair every 3rd then every 5th cycle).
~30 requests in the first 5 minutes, ~610 for the night.

**Expected accuracy** (assumptions in §6; systematic term from §7 included):

| When | True session start | Offset `o` | Timer rate | Requests so far |
|---|---|---|---|---|
| +1 min | ±2.5 s | — | — | ~12 |
| **+4–5 min** | **±0.3 s** | **±0.6 s** — only if the one early header probe (§5.4) ran; otherwise `clock_drift_ms` stays **pending** until post-therapy | — | ~30 |
| +30 min | ±0.3 s | ±0.6 s | ±50 ppm (not useful yet) | ~65 |
| session end (6–8 h) | ±0.3 s | ±0.6 s (±0.4 s with 2–3 sessions/night) | **2–5 ppm** | ~610 |
| one week, no clock step | ±0.3 s | **±0.2 s** + RTC rate ±1 ppm | 2–5 ppm | — |
| **written into a plain EDF** | 1 s header rounding, applied at export — a known rounding, not an error | | | |

The dominant systematic term is the uncalibrated write-to-visible latency (±0.25 s,
§7). The dominant random term is the 1 s quantum of the header start time. Both are
inside the ±0.5 s that a plain EDF can represent and inside one O2-ring sample.
Anything finer is a *format* question (BRP re-packing), not a measurement question.

## 2. Facts, assumptions, verification status

### Established (with source)

| # | Fact | Source |
|---|---|---|
| F1 | AS10 writes a set of EDF files per session (`BRP`, `PLD`, `SAD`/`SA2`, `EVE`, `CSL`; `STR.edf` per day). Sampled files use 60 s records. | airbreak-plus `docs/edf_signals.md` (SX567 table) |
| F2 | Record sizes: BRP 6002 B (AutoSet) / 9002 B (VAuto); PLD 542 B / 782 B; SAD 242 B. Each record ends with a `Crc16` sample. | same table; recounted in Appendix E |
| F3 | EDF header `startdate`/`starttime` are `dd.mm.yy` / `hh.mm.ss` — **1 s** resolution, AS10 local clock, no zone. | EDF spec; airbreak-plus `edf_header.md` |
| F4 | EZShare listing: `GET /dir?dir=A:\DATALOG\<day>` returns HTML with name, size, timestamp per entry. `/download?fname=…&fdir=…` returns file bytes. | SomnoTrace `spec/archive/files-over-http.md` |
| F5 | EZShare has a hardware MUX; the AS10's SD-slot power cycling does **not** reach the card. | maintainer |
| F6 | FAT write-time is 2 s resolution; FAT creation time adds a 10 ms field. FatFs switches to CRC-hashed 8.3 aliases after ~5 collisions, so `/dir` may show `20xxxx~1.EDF`-style names. | FAT spec; ESP-IDF FatFs port |
| F7 | SomnoTrace has NTP, stores `clock_drift_ms = NTP − device` per session, labels noon-days from *raw* device time, and OSCAR matches STR sessions to DATALOG files within ~1 min. | SomnoTrace `spec/0002-edf-export.md`, `as11_time.h` |
| F8 | STR `MaskOn`/`MaskOff` are minutes since the noon-day start, AS10 clock. | same spec |
| F9 | CPAP-AutoSync PCNT captured SD-bus activity on an AirSense 11 AutoSet: active-therapy writes at ~55–60 s intervals, longest observed idle ~57.9 s. Preliminary: one device; AS10 through EZShare still unverified. | CPAP-AutoSync `docs/dev/archive/01-FINDINGS.md` §6.1 |

### Assumed — each is verified in §10 before firmware relies on it

| # | Assumption | If false |
|---|---|---|
| A1 | File growth is visible in the listing per flush (size steps every ~60 s). | No comb → offset from header only, ±1–2 s. **Kills the design**; test first. |
| A2 | A flush is triggered by *record completion* (sample timer), not by an RTC minute boundary. | Comb phase measures the RTC, not the session start; `T_start = T_0 − P − λ_w` invalid. Detectable in software (§10, E1c). |
| A3 | Flush-to-flush jitter is small (≲0.3 s) relative to the burst width `D ≈ 1–2 s`. | Gate tracking fails; fall back to continuous fine vernier (§5.6). |
| A4 | Listings are served live, or with a bounded, roughly constant cache age. | Cache age folds into `λ_w` (bias) if constant; if variable it widens `σ`. |
| A5 | Header `starttime` = therapy (mask-on) start, not first-flush time. | 60 s error in `o`. Cheap software check (§10, E2). |
| A6 | Reading the listing while the AS10 writes is harmless behind the MUX (F5). | Acquisition ladder is replaced by outside-in gates (§5.6); +2 cycles to fix. |
| A7 | AS10 clock is set to local wall time, same zone as the ESP. | `o` includes a zone/DST step; handled in §8.4. |
| A8 | Listing timestamps are the AS10's FAT stamps (mtime or ctime), not the card's own clock. | Only affects the optional mtime channel (§3.4); comb and header are unaffected. |

## 3. Clock model and observables

### 3.1 Three clocks, two rates

```
true time      NTP, delivered to SomnoBridge over BLE (§4)
AS10 RTC       stamps filenames, FAT entries, EDF starttime, STR fields   rate ε_rtc
AS10 timer     paces samples, records and therefore flushes              rate ε_timer
EZShare        irrelevant; it never stamps anything we use (verify A8)
```

`ε_timer` is measured every night from the comb period. `ε_rtc` is measured only across
nights, from the drift of `o`. They are carried separately in the hand-off (§8).

### 3.2 Measurement equations

```
T_k        = T_0 + k·P                       true time of flush k          (comb)
T_start    = T_0 − P − λ_w                   true therapy start            (A2)
t_h        = floor1( T_start + o )           header starttime, AS10 clock  (F3, A5)
⇒ o        = t_h + u − T_start,  u ∈ [0,1)   offset, AS10 − true
ε_timer    = P/60 − 1
```

`λ_w` is the delay from the record completing inside the AS10 to the size change being
visible in a listing (SD write + directory update + EZShare read). It is bounded but
**not measurable without wires** (§7).

**Preliminary prior on `P` (do not hard-code).** F9 suggests bus-visible write onsets
begin roughly 57 s after the previous write on one AS11, while the EDF logical record
duration remains 60 s. These are **different observables**: onset spacing is what the
listing comb sees; record duration is what the file format declares. On AS10 +
EZShare both are unmeasured — E0/E1 measure them. The estimator therefore **fits
`P`** from the night's gates rather than assuming 60 s; `ε_timer = P/60 − 1` stays
defined against the nominal 60 s record duration.

### 3.3 Observables, ranked

| Rank | Observable | Gives | Resolution | Cost | Status |
|---|---|---|---|---|---|
| 1 | **Size step per file per cycle** (listing) | `T_0`, `P` — the comb | poll placement ⊕ blur (~0.1 s) | 1 listing request | A1 |
| 2 | **EDF header `starttime`** (one bounded probe, §5.4) | `o` | 1 s (`u`) | ≤16 KiB once, minutes into the session | A5 |
| 3 | **First observed size of a new file** | If `== header size`: a direct mask-on anchor, no `P`, no `λ_w` | blur | free (same listing) | unknown — high value, test in E0 |
| 4 | Growth stops | session end | one tracking interval (~5 min) | free | — |
| 5 | STR `MaskOn` vs header | validates A5; nothing else | 1 min | post-session | F8 |
| 6 | Listing timestamp (FAT) | real-time `o` estimate before the header is read; RTC rate hints | 2 s (mtime) or 10 ms (ctime, if rendered) | free | A8 — **optional**, log it, do not depend on it |
| 7 | Noon-day folder creation | 1/day anchor | 2 s | free | weak |
| — | WiFi association/TSF, HTTP `Date:`, RTT spikes | log only | — | — | not channels |

## 4. True time inside SomnoBridge

SomnoBridge is on the EZShare AP with no internet route. Its time source is SomnoTrace,
over the BLE link the bridge already uses.

- **Two-way exchange, NTP style**, on every SomnoTrace connection and then every
  ~10 min: offset `((t2−t1)+(t3−t4))/2`, RTT `(t4−t1)−(t3−t2)`. Keep low-RTT samples.
  BLE connection-interval granularity bounds this at ~±10–30 ms — well below the
  listing blur, so it is not the limiter.
- **Every observation is stamped on `esp_timer` monotonic time.** NTP is assigned
  retrospectively by interpolating between the anchors that bracket it. This tolerates
  a SomnoTrace that is asleep or disconnected for hours: the night's evidence resolves
  when the next anchor arrives.
- **Crystal holdover:** an uncalibrated ESP32 XTAL (±10 ppm) drifts up to 0.3 s over
  8 h — *not* negligible against a ±0.3 s target. SomnoBridge must (a) self-calibrate its
  crystal ppm from the anchor history (residual ~1 ppm → 30 ms/8 h), and (b) flag any
  session whose evidence has no anchor within ~2 h on both sides.

## 5. Polling controller

Terminology: `P ≈ 60 s` nominal cycle (fitted, never assumed — §3.2); burst `[B, B+D]` is the ~1–2 s during which the AS10
updates all files; a poll is one listing request of the current DATALOG day folder
(~300–600 B of HTML); *contact* = a poll whose service overlaps the burst.

### 5.1 Watch (before therapy)

| Window | Interval | Requests/h | Detects start within |
|---|---|---|---|
| user's bedtime window (configurable, default 20:00–02:00) | 15 s | 240 | 15 s |
| otherwise | 60 s | 60 | 60 s |

A new file set (or new day folder) in the listing = **therapy start detected**, at
true time `t_a` ± one watch interval. Record the **first observed size** of every new
file (observable 3).

### 5.2 Coarse — one cycle, 5 s polls

From `t_a`, poll every 5 s until the first size step is seen at poll `j` → the first
flush lies in `(t_{j−1}, t_j]`, so `T_0` to **±2.5 s**. ≤12 requests, ≤60 s.
Expected contact: ≤1 poll (the one that catches the step).

If the files appeared header-only (observable 3), `t_a` already pins `T_start` to ±one
watch interval, and the coarse phase confirms `P`.

### 5.3 Ladder — two cycles, bisect the edge

Predict the next flush `B̂ = T_0 + P` (±2.5 s). In cycle 2, poll at `B̂ − 2.5, −1.5,
−0.5, +0.5, +1.5, +2.5 s` (6 polls, 1 s steps) → the *first-updated-file* edge to
±0.5 s and the *last-updated-file* edge likewise, so `D` is measured to ±1 s. In
cycle 3, poll 0.2 s steps across the ±0.5 s window around each edge (≈6 polls) →
edges to **±0.1 s** (blur-limited).

- Requests: ~12 over two cycles. Contact: ~4–8 polls, *once per session*. This is the
  only deliberate contact in the design and is gated on A6 (§10, E0b).
- Output at **~minute 4**: `T_0` ±0.1 s, `D` ±0.2 s, per-file write order.

### 5.4 Early header probe — at most once, minutes 2–5

Exactly **one** bounded early header acquisition is allowed, as a *timing metadata
probe*, not periodic synchronization. It runs only inside a predicted quiet window
(`B̂ + D̂ + 10 s`, so it never overlaps a burst) and only while the transfer policy
(§5.9) would otherwise forbid bulk reads:

- **Range capability is deployment state, probed once by E0.** Runtime sends
  `Range: bytes=0-N` only when the stored capability is `VERIFIED_206` (a real `206`
  with correct `Content-Range` seen in E0). `N` covers just the fixed + signal
  headers, derived from the EDF header-size field (two-step: a small first range,
  then the computed remainder if needed).
- **If capability is unknown or unsupported:** permit one full `GET` of the smallest
  open EDF **only if its currently listed size ≤ 16 KiB**; if larger, abort and
  defer the header read to post-therapy.
- **A `200` during a supposedly verified runtime Range request is a protocol
  regression:** abort the body if possible, mark the capability disabled, mark the
  probe failed/deferred — do **not** silently consume an uncapped body. (In E0 only
  a `206` + correct `Content-Range` counts as Range support.)
- **No repeats.** A second attempt is allowed only if the first failed *before* body
  transfer began (TCP/HTTP error, no bytes). Never retry mid-file.

Parse `starttime`, record count, record size, signal table. This gives `o` (±0.5 s
from `u`, plus §7) and confirms `ΔS` = one record. One request, ≤16 KiB.

**Why this exception exists:** without it, early output would be true session start
±0.3 s but `clock_drift_ms` would remain *pending* until the post-therapy header
read (§5.9). The probe lets `o` ship at minute ~5 at trivial cost. If it is skipped
or deferred, the accuracy table's `o` column applies only post-session — flag the
hand-off `clock_drift_pending`.

### 5.5 Tracking — rest of the night (liveness every cycle + gated pairs)

The therapy FSM (§5.8) needs per-cycle evidence, so the baseline is **one liveness
listing every fitted cycle**, placed in the known quiet window *after* the predicted
burst (`B̂ + D̂ + a few s`, clear of `B̂ + D̂ + g`). It detects coherent size growth
and updates `last_growth` using the **estimated growth edge** `B̂` (bounded by the
listing), not merely the HTTP completion time.

On a subset of cycles that single liveness poll is **replaced** by the
early+late gate pair — early gate at `B̂ − g` (must see nothing new), late gate at
`B̂ + D̂ + g` (must see everything new), `g ≈ 0.3–0.5 s` — which costs **one extra
request** on those cycles, not two beyond baseline:

- Gate cadence: every 3rd cycle for the first 30 min (faster rate convergence),
  then every 5th cycle.
- Arithmetic for an 8 h session at `P ≈ 60 s` (approximate — fitted `P` may
  differ): ~480 base per-cycle liveness polls + ~10 extra gate polls in the first
  30 min + ~90 extra gate polls thereafter ≈ **~580 tracking requests**. With
  coarse/ladder/header, call it **~610 requests after therapy start**.
- Control law is **one-sided** (a late-gate "nothing yet" may be a stale listing; an
  early-gate "already there" cannot be faked): re-centre `B̂` immediately on
  early-gate violations; treat late-gate violations as suspect until repeated.
- Contact ≈ 0 while jitter < `g`. Each gate pair contributes an interval
  constraint on the comb; the night's fit gives `P` to 2–5 ppm.
- Session end: per the therapy FSM (§5.8) — because liveness is per cycle,
  OUT_OF_THERAPY normally confirms ~125–180 s after the final append; final record
  count from the last listing. Never conclude OUT from a single quiet cycle (F9's
  ~57.9 s longest observed idle is close to a nominal cycle).

### 5.6 Fallbacks

| Condition (from §10) | Change |
|---|---|
| A6 false — contact must be avoided | Replace §5.3 with outside-in gates: start at `B̂ ± 2.5 s`, halve the dead zone each cycle while gates hold. ±0.15 s after ~5 cycles (≈ minute 7). |
| A3 false — jitter > `g` | Replace §5.5 with a **fine vernier**: one poll per cycle at `Q = P + 0.3 s`, sweeping the edge every ~200 cycles. Interval-regression fit. ~16 contacts/night; `T_0` still ±0.15 s, rate 1–5 ppm. |
| A1 false — no size growth visible | Timing design off. Offset from header + first-appearance time only: ±1–2 s. |

### 5.7 Load summary

| Phase | Requests | Duration | Contact |
|---|---|---|---|
| Watch (bedtime window) | 240/h | until start | 0 |
| Coarse | ≤12 | 1 min | ≤1 |
| Ladder | ~12 | 2 min | 4–8, once |
| Early header probe | 1 (≤16 KiB, or a small Range read) | once, optional | 0 |
| Tracking (per-cycle liveness + gated pairs) | ~580 | rest of night | ≈0 |
| **Night total after therapy start** | **~610** | | **~5–9** |

Hard guardrails regardless of what the estimator wants: **≤700 listing requests per
8 h active-therapy session** (scale proportionally for longer sessions; this
supersedes any per-night cap — watch-mode requests are accounted separately); peak
≤1 request/s during the ladder; steady-state average ~1.25 requests/min during
tracking. Exponential back-off on HTTP errors or service time > 3× baseline,
no listing polls while a bulk transfer is running, and a "no-timing" mode that
sacrifices timing rather than load the card.

### 5.8 Therapy-status FSM (required metadata)

Therapy status is **required** metadata — SomnoTrace's transfer policy (§5.9)
consumes it, so it must exist even when the estimator is unsure. Internal
three-state machine; the public field `therapy_active` maps as:

| Internal state | Public `therapy_active` | Meaning |
|---|---|---|
| `UNKNOWN` | `null` | No reliable evidence (start-up, stale/error listings) |
| `IN_THERAPY` | `true` | Session file set exists and is growing coherently |
| `OUT_OF_THERAPY` | `false` | Growth has verifiably stopped |

Transitions:

- **→ IN_THERAPY** on a new session file set, **or** two successive coherent
  growth events on the current set. While in this state the per-cycle liveness
  listing (§5.5) detects coherent appends and each updates `last_growth_ntp_ms`
  at the **estimated growth edge** `B̂`, not the HTTP completion time. A new
  coherent append within `expected_append_max_ms` of the last is *expected
  liveness*, **not** an exit condition.
- **IN_THERAPY → OUT_OF_THERAPY** only when **both** hold: ≥ `out_timeout_ms`
  since the estimated last coherent growth edge, **and** ≥2 fresh successful
  non-growing listings inside that interval. With per-cycle liveness this normally
  confirms OUT ~125–180 s after the final append. There is no 62 s exit: a single
  missed or delayed append must never produce OUT.
- **Effective thresholds are derived, not constants.** Defaults
  `expected_append_max_ms = 62 000` and `out_timeout_ms = 125 000` come from the
  preliminary CPAP-AutoSync observation (F9 — an AS11 bus capture, not proof for
  AS10/EZShare) and must be validated in E0/E1. If fitted `P` exceeds 62 s or
  observed append gaps repeatedly exceed 62 s, set
  `expected_append_max_ms = max(62 000, fitted P upper confidence bound + observed
  jitter allowance)` and `out_timeout_ms = max(125 000, 2·expected_append_max_ms +
  listing blur allowance)`. Both effective values are published so SomnoTrace can
  diagnose decisions.
- **Never OUT on bad evidence.** A failed or stale liveness poll does **not**
  count as a non-growing confirmation — fail closed. HTTP errors, stale listings,
  or EZShare unavailability yield `UNKNOWN`, or preserve the last state with
  `stale=true` — never `OUT_OF_THERAPY`. OUT requires positive evidence (fresh,
  successful, non-growing listings).
- OUT → IN on any new file set or coherent growth (resume).

Published fields (§8.3): `therapy_active`, `state_since`, `last_growth`
(NTP ms), `age_ms` (now − last_growth), `expected_append_max_ms`,
`out_timeout_ms`, `confidence`, `stale`, `reason` (human-readable cause of last
transition).

### 5.9 Transfer policy (SomnoBridge ↔ SomnoTrace)

Default rule: **fail closed for transfer.** The recommendation is *no in-therapy
file synchronization at all*: open EDFs are mutable, the known EZShare API has no
verified Range/resume, full re-pulls would waste EZShare/SD/BLE capacity, and the
timing/status polling (§5.5, §5.8) already supplies the live metadata SomnoTrace
needs. The single capped header probe (§5.4) is metadata acquisition, not
synchronization.

- While `IN_THERAPY`, or `UNKNOWN`/`stale`: **never** list-for-bulk or download
  session files. SomnoTrace may still query SomnoBridge's *local* status/metadata over
  BLE — that answers from SomnoBridge's state and costs **no** EZShare request.
- Only after `OUT_OF_THERAPY` is confirmed: freeze/finalize the session's evidence
  (§8.2 runs, hand-off record published), *then* permit SomnoTrace enumeration and
  download of the session's files.
- **Resume-during-transfer:** if therapy restarts mid-pull (new file set or
  coherent growth), stop *starting* new file downloads; the in-flight HTTP request
  may run to completion rather than being cancelled abruptly. Return to
  timing/status mode and mark the transfer `deferred`.
- **No periodic incremental pulls.** HTTP Range/resume is unverified (E0), so an
  open EDF cannot be topped up — it will have changed anyway, making in-therapy
  pulls redundant while adding avoidable EZShare/SD load. The single early header
  probe (§5.4) is the only exception.
- **Explicit user diagnostic override** may transfer during therapy, but the request
  and resulting files must be flagged `override_during_therapy` with a warning.

## 6. Accuracy budget

Assumptions for the figures: `D ≈ 1–2 s`; jitter ≲ 0.3 s; listing blur (request-to-
response ambiguity) ≈ 0.1 s; BLE anchor error ≈ 0.03 s; XTAL holdover calibrated to
≲ 0.05 s; `λ_w` prior per §7.

| Quantity | Terms | Result |
|---|---|---|
| Comb edge `T_0` (after ladder) | step 0.1 ⊕ blur 0.1 ⊕ anchor 0.03 | **±0.15 s** |
| **True session start** | `T_0` ⊕ `λ_w` prior ±0.25 | **±0.3 s** |
| **Offset `o`, one session** | start ⊕ header `u` (±0.5 worst, σ 0.29) | **±0.6 s worst, ~0.4 s 1σ** |
| `o`, `m` sessions in one night | `u` averages as 1/√m | ±0.4 s at m = 2–3 |
| `o`, one week regressed (no step) | 7 nights × σ 0.4 s | intercept **±0.2 s**, RTC rate **±1 ppm** |
| Timer rate, one night | two edges 8 h apart, ±0.15 s each | ±7 ppm worst; **2–5 ppm** from the full gate set |
| Timer rate, first 30 min | same over 1800 s | ±50 ppm — use the prior night's value |
| In-session placement if only an offset is written | 20–100 ppm × 8 h, evaluated at session midpoint | ±0.3–1.4 s worst; **±0.1 s** if the rate is also applied |
| Written into plain EDF | 1 s header rounding | deterministic ±0.5 s on top of the above |

Why no better than ±0.3 s without wires: see §7. Why no better than ±0.5 s in the file:
plain EDF stores start time to 1 s and `SAD`/`PLD` samples are 1 s/2 s apart. Only
`BRP` (25 Hz) can carry finer alignment, and only by re-packing records and recomputing
their `Crc16` — a separate, optional SomnoTrace feature.

## 7. What cannot be calibrated without wires, and how it is bounded

Every software observable sees the AS10 *through* the card: record completes → AS10
writes sectors → FAT directory updates → EZShare reads it → HTTP answers. The comb
locates the *visible* edge, not the internal event. The difference `λ_w` is the one
constant a UART rig would have measured; without it:

- **Bound it from physics.** A few-sector SD write plus a directory-sector read is
  tens of ms; EZShare's HTTP service adds tens to low hundreds. Prior: `λ_w ∈ [0.05,
  0.5] s`, used as **0.25 ± 0.25 s**. That is the ±0.25 s systematic in §6.
- **Tighten it from data, partially.** `λ_w` is confounded with `o` in a single
  session (`o = t_h + u − T_0 + P + λ_w`), but `u` is uniform and known to be in
  `[0, 1)`: across many sessions the *lower envelope* of `t_h − (T_0 − P)` estimates
  `o − λ_w` to ~0.1 s, and `o` cannot be separated from `λ_w` — but the **sum** is
  exactly what the correction needs. The residual ±0.25 s matters only for the split
  between "AS10 clock" and "card latency", which nobody consumes.
- **Header-only appearance** (observable 3), if real, replaces `λ_w` (write latency
  after a 60 s record) with `λ_c` (create latency), which is plausibly smaller and
  more stable, and removes `P` from the start-time equation.
- **Model check for A2 in software:** if flushes are triggered by record completion,
  the comb phase `(T_0 + o) mod 60` in AS10 seconds should track the header
  `starttime` seconds across sessions (correlation ~1). If it is constant (e.g. always
  near `:00`), flushes are RTC-scheduled and `T_start = T_0 − P − λ_w` is wrong. Ten
  sessions decide this. (E1c)

Net: the design is *self-consistent* without wires; what it cannot do is prove its own
absolute bias below ~±0.25 s. That is accepted under the ±0.5 s EDF target.

## 8. Evidence, estimation, hand-off

### 8.1 Evidence log (SomnoBridge, append-only, crash-safe)

```
poll    { mono_req_us, mono_resp_us, dir, http, changed:[{name, size, stamp_raw}],
          plan:{phase, cycle, target_phi_ms} }      # changed entries only + 1 full snapshot/session
head    { mono_us, name, starttime, startdate, n_records, rec_size, n_signals, patient_field_raw }
anchor  { mono_us, ntp_ms, rtt_us }
event   { mono_us, kind: session_start | growth_stop | assoc | disassoc | http_backoff }
config  { fw, ezshare_version, esp_tz, watch_window, profile }
```

Segments on the TF card (flash fallback), write-then-rename, size-capped rotation,
NVS boot counter. ~20–40 KB/night with changed-entries-only logging.

### 8.2 Estimator (runs on SomnoBridge at growth-stop; re-runnable offline)

1. Map every `mono` to `[t_lo, t_hi]` in NTP via anchor interpolation (with crystal ppm).
2. Turn "changed / unchanged" observations into interval constraints on `(T_0, P)`
   per file; intersect (rigid comb) or regress (jittery comb, flagged).
3. `T_start = T_0 − P − λ_w_prior`; `o = t_h + 0.5 − T_start` (midpoint of `u`),
   `σ_o` from §6; `ε_timer = P/60 − 1`.
4. Across sessions of the night: pool `o` (1/√m). Across nights (SomnoTrace side, §8.3):
   regress `o(day)` for `ε_rtc`, with change-point detection for clock steps.
5. Flags: `comb_locked`, `jitter_high`, `header_only_start`, `no_anchor_2h`,
   `contact_detected`, `tz_step_suspect`, `low_coverage`, `a2_violation`.

### 8.3 Hand-off to SomnoTrace

Finalised per session at growth-stop and published over the metadata API; SomnoTrace
fetches it when it pulls the session's last file, stores it in its per-session drift
index (alongside its AS11 `clock_drift_ms`), and applies it **only at its own EDF
export**. Sign convention: `NTP − device`, as SomnoTrace already uses.

```
{ "session_id":            "20260114_223107",          // AS10 file-set stem, raw
  "session_start_as10":    "2026-01-14T22:31:07",      // header starttime, raw, local, no zone
  "session_start_true_ms": 1768429867300,              // NTP epoch ms — the primary output
  "clock_drift_ms":        -1181300,                   // NTP − AS10 at session start
  "sigma_start_ms":        300,
  "sigma_drift_ms":        400,
  "timer_rate_ppm":        -37.2,   "sigma_timer_ppm": 4,
  "rtc_rate_ppm":          null,                       // filled by SomnoTrace across nights
  "n_records":             412,     "rec_size":        542,
  "flags":                 ["comb_locked"],
  "evidence_ref":          "seg-0042" }
```

While `clock_drift_pending` is flagged — i.e. the early header probe (§5.4) was
skipped, deferred, or failed — `clock_drift_ms` and `sigma_drift_ms` are `null`
until the post-therapy header read supplies `o`; the record then finalizes as in
the example above.

The same API exposes live therapy status (§5.8) and the transfer decision (§5.9),
answerable **locally over BLE with no EZShare request**:

```
{ "therapy_active":   true,                 // true=IN_THERAPY, false=OUT, null=UNKNOWN
  "state_since":      1768427461000,        // NTP ms when the FSM entered this state
  "last_growth":      1768430002030,        // NTP ms of last coherent append
  "age_ms":           41200,                // now − last_growth
  "expected_append_max_ms": 62000,          // effective liveness threshold (§5.8)
  "out_timeout_ms":   125000,               // effective OUT timeout (§5.8)
  "confidence":       "high",               // fresh listings agreeing
  "stale":            false,                // true if last listing(s) failed/stale
  "reason":           "coherent growth 41 s ago",
  "transfer_allowed": false }               // §5.9: true only when OUT is confirmed
```

Additional flags used by the status/transfer path: `clock_drift_pending` (early
header probe skipped or deferred — `o` arrives post-therapy), `transfer_deferred`
(resume interrupted a pull), `override_during_therapy` (user-forced transfer).

SomnoTrace applies, consistently and together (F7):

- EDF header `startdate`/`starttime` ← `round1(session_start_true_ms)`; recompute the
  header CRCs in the patient-ID field if the AS10 uses them (verify in E0).
- Filenames and any per-file times ← same correction.
- STR `MaskOn`/`MaskOff` ← `+clock_drift_ms` (+ rate term), re-quantised to minutes.
- EDF+ annotation onsets ← header-relative, so they follow the header; apply
  `timer_rate_ppm` only if the record duration is left at `60.00` (default).
- **Noon-day labels stay on raw AS10 time.** A correction across 12:00 must not move a
  session to another day.
- Store `session_start_true_ms` verbatim in SomnoTrace's own metadata for any future
  sub-second consumer (BRP re-packing, O2-ring alignment).

### 8.4 Timezone and DST

Compare AS10 stamps as **ESP-local wall time**, so a correctly set AS10 zone cancels.
A residual near ±1 h that first appears on the ESP zone's DST transition date is
flagged `tz_step_suspect`, not folded into drift. Never classify by "near a multiple of
3600 s" — real zones step by 15/30/45 min, which overlaps the drift range.

## 9. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| Listing contact disturbs the AS10 (A6 false) | SD command timeouts on the recorder — the one failure mode that touches therapy data | E0b before any ladder ships; §5.6 outside-in fallback; EDF `Crc16` and record-count checks after every experimental night |
| Growth not visible per flush (A1 false) | No comb | E0 first; header-only design ±1–2 s |
| RTC-scheduled flushes (A2 false) | Start-time equation wrong | E1c software check; then use header + first-appearance instead |
| Header CRC layout differs on AS10 | Rewritten headers rejected by consumers | E0 reads the patient field; E8 tests OSCAR/SleepHQ/ResScan |
| BLE anchors absent for hours | XTAL holdover error up to 0.3 s | crystal self-calibration; `no_anchor_2h` flag; SomnoTrace reconnect on wake |
| Manual clock change / DST mid-night | Step in `o` | change-point detection; never pool across a step |
| Multiple record sizes per flush (pauses, catch-up) | `ΔS` not one record | use `gcd` of observed steps; confirm with header `rec_size` |
| Card firmware caches listings (A4) | Bias or widened σ | E0 measures cache age; one-sided gate law |
| **False OUT_OF_THERAPY** — the dangerous case | SomnoTrace pulls files while the AS10 is still writing → redundant/conflicting load and a redundant pull of an EDF that will change anyway | OUT only on positive evidence: ≥`out_timeout_ms` (125 s provisional default) with no coherent growth + ≥2 fresh successful listings (§5.8); transfer fails closed — UNKNOWN/stale blocks pulls (§5.9); E-FSM tests it |

## 10. Validation plan — software only, cheapest first

| Step | What | Decides |
|---|---|---|
| **E-sim** | Monte-Carlo of §5 + §8.2: `P`, `D`, jitter, blur, `λ_w`, cache age, both rates, poll-schedule jitter, mtime-vs-ctime; report bias/σ per phase and per minute after start | Whether §1's table is real; catches the class of error found three times in the review record |
| **E0** | One real EZShare + AS10 card, one night, a laptop on the card's AP polling at 5 s: listing format (names, stamp type, second parity, cache age), size growth per file per cycle, **first observed size of new files**, HTTP `Date:`/service time, EZShare version; **`Range: bytes=0-511` probe — accept only a real `206` with correct `Content-Range`; a `200` means full body was sent and Range must be treated as unsupported**; download one file set and read header layout + patient-field CRCs | A1, A4, A8, observable 3, CRC layout, Range support |
| **E0b** | Same rig: dense polling deliberately *inside* bursts (5–10 consecutive), then verify EDF `Crc16`/record counts and AS10 SD-error indications next morning | A6 → ladder allowed or outside-in only |
| **E-FSM** | Scripted + on-rig exercise of §5.8/§5.9: delayed/missed listing, one missing append, HTTP errors, brief pause/resume, final stop | No false OUT before 125 s; bad listings → UNKNOWN/stale, never OUT; transfer gating holds |
| **E1** | Fine vernier run (`Q = 60.3 s`) over a night: `P`, jitter, `D`, per-file write order | A3 → gate vs vernier |
| **E1c** | Comb phase vs header seconds across ≥10 sessions (§7) | A2 |
| **E2** | STR `MaskOn` vs header `starttime` on any existing AS10 card | A5 |
| **E3** | Run §8.2 on E0/E1 logs; compare `o` across sessions and nights | σ figures; RTC rate |
| **E8** | OSCAR, SleepHQ, ResScan with rewritten `starttime` + recomputed CRCs; optional non-integer record duration; optional re-packed BRP | What SomnoTrace may write |
| E9 | Longitudinal: 2 weeks of nightly `o`; check linearity, steps, agreement with the user's manual clock reading | RTC rate, step detection |

No step needs wires. E0–E2 need one card, one AS10 and a laptop; E-sim needs neither.

## 11. Decisions taken (and why), for the record

- **Comb-first, header-second.** The comb gives the true session start directly; the
  header is needed only for the offset, so its 1 s quantum does not limit the
  corrected timeline. (Appendix E correction to D.)
- **Staged acquisition instead of choosing gate *or* vernier.** Coarse → ladder → gate
  reaches sub-second in ~4 minutes with ~30 requests and near-zero contact afterwards;
  a fine vernier alone would take 2–5 h to its first fix.
- **Deliberate contact, once, only if E0b allows.** Consecutive contact inside a
  burst is tested explicitly before the ladder ships.
- **`λ_w` as a bounded prior.** With no wired truth, ±0.25 s systematic is accepted;
  it is below the EDF representability floor and the correction needs `o + λ_w` anyway.
- **SomnoBridge publishes, SomnoTrace applies.** ms-precision metadata, rounding at
  export, consistency across header/filenames/STR, noon-day on raw time.
- **Two rates, carried separately.** Timer rate from the comb each night; RTC rate from
  `o(day)` across nights.
- **Not channels:** WiFi TSF, association events, HTTP `Date:`, RTT spikes, filename
  timestamps (hashed aliases), oximetry cross-correlation (15–45 s lag). Logged where
  free, never relied upon.
