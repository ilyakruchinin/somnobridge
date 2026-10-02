# Tuning — drift estimator characterization

How to raise clock-drift accuracy from the older ~±3 s mtime floor to the ≤±2 s
"accurate" tier. This documents the measurement workflow, where the measured
values land, and who the results apply to.

## TL;DR

1. Update 4 removes the unexplained 2 s cache and 2 s residual-delay defaults.
   The active AS11+EZShare model uses `C=0` and `D=0` as explicit, unvalidated
   operating assumptions (`cache_policy=assumed_zero`,
   `visibility_policy=assumed_immediate`). HTTP request duration is already in
   the measured observation bracket and is not subtracted again.
2. A conditional estimate can now get below 2 s with dense phase brackets, but
   it remains provisional until timestamp→visibility semantics are independently
   characterized. Do not set the `*_validated` gates on these assumptions alone.
3. The filename channels (`create`, `periodic`) use measured bounds installed in
   `s_prof`; they provide cross-checks and may narrow the estimate when
   unambiguous.
4. Installing profile changes = editing `s_prof` in `components/drift/drift.c`
   and bumping `model_rev`. There is currently **no runtime install path**
   (no BLE op, no NVS persistence) — bounds are compiled in.
5. The bounds characterize the **machine+card model**, not your unit — once
   measured for AS11+EZShare they can ship in code for everyone with that
   combination. The per-unit clock offset is measured live at runtime.

## The gate chain

Each `unavailable`/`flags` reason maps to one specific blocker:

| Flag / reason | Blocker | How it clears |
|---|---|---|
| `timezone_unresolved` | AS10/AS11 configured tz not in settings | Set POSIX tz (e.g. `AEST-10` for UTC+10) in the portal — done |
| `creation_delay_unbounded` | `filename_origin_bounded=false` (fname→create delay unmeasured) | Measure, set `fname_create_lo/hi_us`, set flag |
| `period_relation_unbounded` | `filename_periodic_bounded=false` (filename↔flush-cycle relation unmeasured) | Measure, set `fname_period_lo/hi_us`, set flag |
| `cycle_ambiguous` | same gate as periodic | clears with `filename_periodic_bounded` |
| `filename_origin_unresolved` | `DF_FNAME_ORIGIN` — origin bounds absent | clears with `filename_origin_bounded` |
| `timing_unvalidated` | `stamp_channel_validated=false` — mtime model not certified on real HW | Set true once the measured bounds are reviewed and consistent |

## What needs measuring (profile fields)

All live in the `s_prof` literal, `components/drift/drift.c`:

| Field | Meaning | Current | Evidence needed |
|---|---|---|---|
| `fname_create_lo/hi_us` | Bound on delay between the filename's timestamp instant and the file's actual creation/visibility | `[-3,+10] s` | Measured 2026-10-02; wide because it includes the 5 s observation window |
| `fname_period_lo/hi_us` | Bound on the filename↔flush-cycle phase relation | `[-5,+1] s` | Measured 2026-10-02 across 18 flush cycles |
| `filename_encoding` | Whether filename seconds truncate (`DRIFT_ENC_TRUNCATED`: second = [F, F+1s)) or round (`DRIFT_ENC_ROUNDED`) | `DRIFT_ENC_UNKNOWN` (conservative union) | Compare filename seconds vs EDF header `starttime` (1 s resolution, post-therapy) |
| `latency_lo/hi_us` | Residual timestamp→listing-visibility delay | `0,0` assumed | Independent truth needed; HTTP service duration does not prove this value |
| `cache_bound_us` | Additional listing freshness bound beyond measured endpoints | `0` assumed | Scoped zero-cache operating model; absence of regressions alone cannot qualify it |
| `cache_policy` / `visibility_policy` | Provenance labels serialized with estimates/SOT | `assumed_zero` / `assumed_immediate` | Switch to `characterized` only with supported evidence |
| `filename_origin_bounded` / `filename_periodic_bounded` | "these bounds are now evidence-backed" switches | `true` | Based on the 2026-10-02 capture; keep wide bounds honest |
| `stamp_channel_validated` / `filename_create_validated` / `filename_periodic_validated` | Certification flags — gate QUALIFIED promotion | `false` | Set only after coverage evidence supports the semantic model, not merely tight intervals |
| `dev_rate_ppm` | Max device clock rate error | 500 ppm (conservative) | Optional tightening from long-baseline anchors |

## Step-by-step measurement

Prerequisites:

- Host clock disciplined: `chronyc tracking` → leap normal, root distance
  ≲ 100 ms for firmware anchor qualification (at ±150 ms anchors are still
  recorded for offline mapping — usable for characterization).
- Device bonded to this host; card link healthy (`GET_STATUS` shows
  `sta_connected`, no `budget.backoff_ms`).
- A real therapy session (masked — unmasked sessions produce no sampled
  EDFs; see F6 in `validation-results.md`).

### Passive capture (primary tool)

```bash
python3 scripts/validate_timing.py capture \
    --day $(date +%Y%m%d) --duration 1200 \
    --serial /dev/ttyACM0 \
    --host 192.168.86.102          # SomnoTrace host serving GetDateTime RPC
```

- **Capture never starts therapy** — it is a passive observer. Therapy must
  be running for useful evidence: start it on the machine manually (mask
  on — unmasked sessions produce no sampled EDFs), or run `cycle` in a
  second terminal as below.
- Duration: default 1200 s is the right order — you want ≥5 flush cycles
  (~60 s each) *after* session start plus margin before/after. Do not stop
  early; the first ~70 s are session creation, flushes start after.
- It is read-only: cached BLE catalog reads + clock references + serial
  poll log. Adds no card polls, never writes to the card.
- Stop it before flashing (it owns the serial reader). Evidence lands in a
  private mode-0600 JSONL under `/tmp/somnopal-validation-*`.

### Passive observer (automated multi-session collection)

```bash
python3 scripts/validate_timing.py observe \
    --day $(date +%Y%m%d) --duration 7200 \
    --host 192.168.86.102 --no-anchor-commit
```

`observe` is the update-5 collection path. It keeps one authenticated BLE
connection, reads cached GET_STATUS/GET_DRIFT more frequently while therapy or
an open observation is active, samples SomnoTrace `GetDateTime`, replays
bounded journal evidence, and collects whitelisted session metadata. It adds
no card requests and never controls therapy. `--no-anchor-commit` retains raw
BLE time exchanges for offline monotonic→UTC mapping without committing a new
device anchor; omit it only when the host source is disciplined within the
firmware's 100 ms acceptance limit.

### Controlled therapy cycle (optional, reproducible edges)

Requires a *running* capture file plus `--allow-therapy-control`; drives
`EnterTherapy`/`EnterStandby` through SomnoTrace's documented RPC bridge
(240–600 s):

```bash
python3 scripts/validate_timing.py cycle \
    --input /tmp/somnopal-validation-XXXX/evidence.jsonl \
    --allow-therapy-control --therapy-seconds 480 \
    --host 192.168.86.102 --day $(date +%Y%m%d)
```

Gives clean start/stop edges for `fname_create_*` measurement without
waiting on the user.

### Analysis

```bash
python3 scripts/validate_timing.py analyze \
    --input /tmp/somnopal-validation-XXXX/evidence.jsonl
```

Emits: per-file-type transition counts (size-only vs stamp-only vs both),
stamp delta distribution (→ flush period), reference drift envelope, and
update-5 scoring when `observe` records are present: unique raw brackets,
reference containment/overlap, midpoint residuals, and timestamp→visibility
delay intervals. Cross-check against completed sessions with
`history --day YYYYMMDD`.

## Installing the bounds

`drift_profile_set()` exists but is **only called from host tests** —
production firmware has no runtime profile path. The install is:

1. Edit `s_prof` in `components/drift/drift.c`: set measured
   `fname_create_lo/hi_us`, `fname_period_lo/hi_us`,
   `filename_encoding`, `latency_lo/hi_us`, `cache_bound_us`, and the
   `cache_policy`/`visibility_policy` provenance; set the `*_bounded` flags;
   set `*_validated` flags only once the semantic evidence is reviewed.
2. **Bump `model_rev`** — runtime `drift_profile_set` marks persisted SOT
   stale, and boot-time load marks a record with a different `model_rev` or
   cache/visibility policy inapplicable. Old bounds cannot leak forward.
3. `./scripts/run_host_tests.sh`, rebuild, flash.
4. Verify: `GET_DRIFT` should show `timing_model`, `recent_observations`,
   and `create`/`periodic` intervals. With update 4, `timing_unvalidated`
   remains until visibility semantics are independently supported; provisional
   half-width may be below 2 s, while qualified promotion still requires a
   validated source at or below `accurate_half_us`.

If a runtime install path is ever wanted (per-unit field calibration,
avoiding reflashes), it needs a new BLE op + NVS persistence — currently
none exists. For now compile-time is intentional: a wrong-but-confident
bound silently corrupts EDF timestamps, so bounds should only change with a
code review, not a user action.

## Device-specific or universal?

The profile bounds describe **firmware behavior of the machine+card
model** (AS11 flush cadence, EZShare listing latency, filename encoding) —
not your unit. Per-unit values (clock offset, drift rate) are always
measured live at runtime. So:

- AS11 + this EZShare firmware: bounds are shareable — ship in code.
- AS10 or a different card: **unverified** — needs its own capture. If the
  values differ, key them by model (or bump `model_rev` and note which
  combo it qualifies).
- `dev_rate_ppm` (500) is a deliberately wide crystal bound; safe for all
  units, tightenable with long-baseline anchors.

## Do not fake the bounds

Setting `*_bounded=true` with guessed values does not just inflate a number
— it produces *confident, wrong* drift intervals that SomnoTrace applies to
real EDF timestamps. A bound that is honestly wide beats a tight lie.
