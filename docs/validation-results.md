# SomnoPal — timing-channel validation results

Status: measured results from the 2026-10-01 validation session. Evidence for
`implementation-update.spec` revision 2. All observations are from the
confirmed topology below; they qualify that hardware/firmware combination and
do not by themselves transfer to an AS10.

## 1. Scope and method

- **Topology (user-confirmed):** the EZShare card monitored by SomnoPal is
  physically inside the same AirSense 11 that is paired with SomnoTrace
  (`192.168.86.102`).
- **Independent clock reference:** SomnoTrace's read-only `GetDateTime` RPC
  via its documented HTTP bridge, plus its per-session metadata
  (`clock_drift_ms`, `clock_drift_valid`, `clock_drift_source`), retrieved
  read-only over anonymous FTP. Host NTP discipline: chrony, leap normal;
  conservative source uncertainty from root distance ≈ **±150 ms**.
- **SomnoPal observation:** cached BLE catalog reads (5 s cadence), the
  device's own serial poll log, and — after a diagnostic firmware flash —
  native EDF header inspections with exact HTTP content lengths. No estimator
  or therapy-threshold changes; no card writes; therapy started/stopped via
  documented RPCs with the user's standing approval; settings untouched.
- **Sessions:** two unmasked RPC-started runs (111 s, 183 s), one user-run
  session (481 s, 11:04 local), one masked RPC-started run (302 s, 11:21–11:26
  local). Historical comparison across the five completed sessions of
  2026-09-30 (25 native catalog files matched to SomnoTrace references).
- Evidence was captured to private mode-0600 JSONL files under `/tmp`
  (`somnopal-validation-*`); only timing/layout metadata was recorded — no
  patient fields, waveforms, or recording content.

## 2. Reference quality

| Item | Result |
|---|---|
| Live `GetDateTime` drift (NTP − AS11) | median ≈ **−507,600 ms**; best single-query interval width ≈ 0.4–0.9 s (RTT 84–216 ms) |
| SomnoTrace `clock_drift_ms` (measured_stop) | −507,637 / −507,599 / −507,610 / −507,587 ms across sessions |
| Live RPC vs session metadata agreement | within ≈ **10–50 ms** — the two references are interchangeable at our resolution |
| Host source uncertainty | ±150 ms (chrony root distance). Above the firmware's 100 ms anchor-acceptance limit, so BLE anchors were logged for offline mapping but could not qualify this run |

## 3. Findings

**F1 — Clock-domain map (confirmed).** One AS11 clock, three renderings:

| Artifact | Rendering | Example (masked session) |
|---|---|---|
| Native filename | AS11 local (Melbourne) | `20261001_112949` |
| EDF header `starttime` | AS11 local | `01.10.26 11.29.49` |
| Listing stamp | AS11 UTC-shaped | `2026-10- 1 1:29:50` |
| SomnoTrace session start/end | ESP NTP domain | 01:21:15Z / 01:26:17Z |

Stamp + measured drift lands inside the independently observed update window.
No second (EZShare) clock is needed to explain any observed timestamp.

**F2 — Size encoding (confirmed, 5/5 files).** `listed = ceil(exact/1024)` KiB:

| Type | H | R | D (s) | Exact bytes | Listed | Token |
|---|---|---|---|---|---|---|
| CSL | 768 | — | 0.00 | 832 | 1024 | 1KB ✓ |
| EVE | 768 | — | 0.00 | 832 | 1024 | 1KB ✓ |
| SA2 | 1024 | 242 | 60 | 1992 | 2048 | 2KB ✓ |
| PLD | 2816 | 542 | 60 | 4984 | 5120 | 5KB ✓ |
| BRP | 1024 | 6002 | 60 | 25032 | 25600 | 25KB ✓ |

Exact lengths came from HTTP `Content-Length` on the card's listing-derived
`?file=<8.3>` download path. Sizes are **rounded up**, never exact below 1 KiB
quantum. No ≥1 MiB file was observed, so an MB-unit transition remains
unverified.

**F3 — Record/flush mapping (confirmed on this session).** 302 s session,
4 flushes, stamp deltas 56/58/60/60 s, first flush ~63–65 s after therapy
start. Final files contain **exactly H + 4·R bytes — no trailing partial
record**; the AS11 flushes complete records only and discards the final
partial. Header `declared_records` (4) matches observed complete records.

**F4 — Timestamp-only updates (confirmed; the core finding).** PLD changed
mtime on 2 flushes with no size change (4096→4096, 5120→5120); SA2 on 3
(2048 for 4 flushes). BRP (6 KiB/flush) always crossed the KiB boundary.
Size-only detection misses real flushes; the (size, stamp) tuple catches
every observed one.

**F5 — Annotation files are creation-stamped.** CSL/EVE are created at
session start (~6 s before the sampled files) and never updated: their mtime
equals the creation time (historical stamp-vs-end distances grow with session
length; native inspection shows 832 bytes, one 0.00 s record). They are
**not** valid growth beacons; their stamps are however a stable session-start
claim.

**F6 — Mask required for sampled EDFs.** Both unmasked runs produced only
CSL/EVE files, while SomnoTrace's BLE streams captured normal BRP/PLD/SA2
data with zero reported gaps. With the mask on, all five file types appeared.
The AS11 does not write sampled EDFs to the card for flow without therapy.

**F7 — Session start semantics (corrected).** SomnoTrace's session start
equals the `EnterTherapy` ack (11:21:15 local); native files were created
~6 s later (drift-corrected filename time 11:21:21). An earlier SmartStart
hypothesis is **withdrawn** — the apparent 50 s lead was the 60 s watch-scan
discovery delay, not an earlier session start.

**F8 — Write-to-visibility latency (bounded, not tight).** Flush 4
(true ≈ 11:25:18.4–20.4 from stamp + reference) was absent from the ~11:25:11
scan and visible to the ~11:26:08 scan → **latency ∈ [≈0, ≈50 s]**. Fresh
post-session listings matched exact content lengths (no stale-cache
discrepancy in these samples). Listing service times 20–243 ms. A tight
latency bound requires a short dense-poll window during a live session.

**F9 — Download path (validated).** The card's `?file=<8.3 short name>`
path, resolved from the listing itself, served all five files' bytes — the
inspect operation used it end-to-end. This validates the resolver approach
for `GET_FILE`/probe URLs.

**F10 — Per-file layouts differ (confirmed).** BRP R=6002 B vs PLD 542 B vs
SA2 242 B; CSL/EVE are 0.00 s-duration single-record files. Any design that
transfers one file's parsed layout to another file's size fit is invalid.
The timestamp-first design removes that coupling entirely.

**F11 — Load ledger persistence (observation).** Across the reflash the
ledger restored 221 pre-boot requests (not zero). The drop from the pre-flash
658 is consistent with the 8-hour minute-ring wrap (later test requests
overwrite night positions), not a reset. Worth a dedicated review later; no
action taken.

**F12 — Request latency (measured over BLE bridge + card-side).**

| Request | Path | Median | Notes |
|---|---|---|---|
| `GET_STATUS` | local | 184 ms | BLE round-trip floor (n=5) |
| `LIST_DAY` (cached, 51 files) | local | 1229 ms | BLE delivery of ~3.3 KB (n=5) |
| Day-folder listing | card | 218–243 ms (median ≈ 236 ms, n=5) | the burst-poll unit; fits 0.5 s spacing |
| `GET_FILE` single SA2 | card GET | 319 ms cycle (n=3) | **fails before body**: ceil-KiB `expected_size` (2048) vs exact Content-Length (1992) |
| `LIST_DIRECTORY 'A:\'` | card | 236 ms | **parse failure**: root-page chrome anchors rejected by the strict parser (pre-existing) |

No per-file stat endpoint exists in the card API; the day-folder listing is
the minimum unit carrying stamps. BLE is never in the polling path (burst
polls are scheduler-internal card requests).

## 4. Implications for the estimator

1. **Offset measurement without content reads is feasible:** pair each stamp
   change with its bounded visibility window; offset interval =
   true-window − quantized stamp interval. With 60 s liveness polls alone the
   interval is ~±35 s (approximate tier only). A bounded dense window
   (~5 s spacing for ~2–3 min, ≈24 requests — inside the existing 40/set
   acquisition allowance) plus a characterized latency prior brings the
   half-width to the 2 s quantization floor.
2. **±1 s half-width is a theoretical floor, not a target:** the 2 s FAT
   stamp quantization alone makes the device-side interval 2 s wide. The
   achievable "accurate" gate is **±2 s**; report the true half-width either
   way. Optional refinements exist (ctime 10 ms at creation; header starttime
   1 s post-therapy) but are not required for the primary result.
3. **Beacon selection:** only sampled files (BRP/PLD/SA2) update per flush;
   CSL/EVE stamps are creation-fixed. BRP is the best size-change beacon;
   PLD/SA2 prove stamp-only detection.
4. **The EDF header is no longer on the critical path.** It remains a
   post-therapy validation artifact (domain cross-check: header local time vs
   stamp UTC shape) obtainable during ordinary downloads.
5. **SomnoTrace metadata is a usable reference** (`clock_drift_ms` agreed
   with the live RPC to tens of ms), enabling offline scoring of estimator
   candidates on every future session.

## 5. Limitations and remaining work

- Results qualify **this AS11 + this EZShare card firmware**; AS10 behavior
  is unverified and needs its own confirmation.
- One masked session with full observation; the base spec's H2 gate
  (≥10 sessions, varied start phases, pause/resume) remains open.
- MB-unit transition above 1 MiB unverified.
- Tight latency bound requires one dense-poll characterization window.
- Host reference uncertainty (±150 ms) exceeded the firmware anchor limit
  this run; anchors were logged but not qualified. A lower-uncertainty
  reference (or relaxed source bound) is needed for on-device anchor
  qualification during characterization.
- The load-ledger wrap observation (F11) needs a separate review.

## 6. Evidence pointers

Private JSONL evidence (mode 0600, timing metadata only):
`/tmp/somnopal-validation-{ns4guvr7,wgc3ac4f,y664tk12,7d4kxv17,cbn4e3u4}/evidence.jsonl`
(snapshots, clock references, transitions), and
`/tmp/somnopal-validation-{9loqe74g,npp84tik,37_d9v7x,_q5kstbj}/evidence.jsonl`
(therapy-control cycles). Recorder: `scripts/validate_timing.py`
(self-test, history, capture, cycle, analyze modes). Diagnostic firmware
additions: `INSPECT_EDF_HEADER` BLE op, listing-derived download resolver,
per-scan request timestamps — all uncommitted.

## 7. Follow-up — 2026-10-02 masked session (capture lt_hjhvx)

Passive capture, 210 catalog snapshots @5 s, ~18 flush cycles. Patient data
untouched (timing/size metadata only).

- **Flush period:** exactly 60.0 s across BRP (18/18), PLD (18/18), SA2
  (18/18); single 56 s (PLD) and 58 s (SA2) outliers at first flush.
- **Create delay** (filename instant → listing appearance, drift-corrected):
  CSL/EVE −0.5..+5.1 s; PLD/SA2/BRP +0.1..+5.8 s — window includes the 5 s
  poll quantum. Profile bound set to [−3 s, +10 s].
- **Periodic residual** (`stamp − (filename + k·60 s)`): **−1 s** for
  BRP/SA2, **−3 s** for PLD, stable across all 18 cycles. Profile bound set
  to [−5 s, +1 s]. PLD differs from BRP/SA2 — any per-type bound must
  account for this.
- **Filename encoding:** not separable from this data (needs EDF header
  `starttime` comparison); `DRIFT_ENC_UNKNOWN` retained.
- **Filename channels still lose to mtime at 5 s polling:** candidate width
  = observation window + filename quantum + bound ≈ 9–20 s vs mtime's ~6.5 s.
  They provide the source-agreement cross-check, not a tighter estimate.
  Reaching the ≤2 s qualified gate needs a dense-poll window (~1–2 s
  spacing) to shrink the per-event observation window, plus a measured
  write→visibility bound to tighten `latency_hi_us`/`cache_bound_us`
  (F8 remains loose at [≈0, ≈50 s]).
- Profile installed: `filename_origin_bounded`, `filename_periodic_bounded`.

## 8. Update-4 timing model implementation (2026-10-02)

The update-4 model removes the unexplained `cache_bound_us=2 s` and residual
`latency_hi_us=2 s` defaults for the tested AS11+EZShare scope:

- `cache_policy=assumed_zero`, `cache_bound_us=0`
- `visibility_policy=assumed_immediate`, `latency_lo/hi_us=0`
- `model_rev=3`; persisted SOT from a different model, policy, or stored
  timing/source bound now loads as stale/inapplicable rather than silently
  applying.
- Actual request endpoints and service durations are exported via GET_STATUS;
  recent raw transition brackets are exported via GET_DRIFT and journaled as
  bounded `timing_obs` evidence.

Important scope note: zero cache and immediate visibility remain explicit
assumptions, not independently proven hardware behavior. Consequently narrower
intervals are conditional/provisional; `stamp_channel_validated` remains false
and no qualified SOT may promote under this semantic model alone.
