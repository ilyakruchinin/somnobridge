# SomnoPal — implementation update: timestamp-first drift estimation, accuracy tiers, and persisted source of truth

Status: proposed requirements, **revision 2**. Normative delta over
`implementation-spec.md`; where they conflict, this document wins for the
affected behavior. Revision 2 replaces the record/flush-comb estimator as the
primary drift path with a **timestamp-first offset estimator**, based on the
measured evidence in `docs/validation-results.md` (masked-session validation
against an independent AS11 clock reference). All other guarantees of the
base spec stand.

**Poll scheduling is superseded by `implementation-update2-spec.md`**
(phase-tracking schedule); the estimator, tiers, SOT, and API sections
here remain normative.

Goal: **the API MUST always be able to return a high-accuracy time-drift
value.** Serve the newest accurate-tier value when one exists; otherwise the
last accurate value persisted in NVS. Never substitute an approximate value
for an accurate one, and never fabricate drift.

## U1. Terminology

- **Stamp** — the per-file timestamp rendered by the card listing
  (`timestamp_raw`). Validated behavior: the AS11's own clock rendered
  UTC-shaped, quantized to FAT's 2 s units, updated on every sampled-file
  flush (validation F1, F4, F6).
- **Update tuple** — `(size_token, timestamp_raw)` per file. A change in
  either is an update event; the stamp is the primary signal.
- **Update window** `[E_lo, E_hi]` — the true-time interval in which the
  observed change occurred: from the previous unchanged observation's request
  start (minus the cache bound) to the changed response's end (§8.3 rule,
  unchanged).
- **`accuracy_half_ms`** — half-width of the published offset bound:
  `(drift_interval_ms.hi - drift_interval_ms.lo) / 2`. Worst-case error under
  the model's honest bound, not a sigma.
- **`computed_at_ms` / `reference_at_ms`** — UTC epoch ms at which the value
  was last fitted, and the UTC instant the value applies to (the fit's
  reference event). Never a step-able local wall clock.
- **Source of truth (SOT)** — the most recent *accurate-tier* offset, carried
  in RAM during a session and persisted in NVS. One SOT slot; newest wins.
- **Beacon classes** — `sampled` (BRP/PLD/SA2: 60 s records, mtime updates
  per flush) vs `annotation` (CSL/EVE: created at session start, mtime never
  updated). Only sampled files are timing beacons.

## U2. Design change summary

**Simplified (timestamp-first replaces the comb as the primary path):**

- The primary offset observable is the beacon's **stamp transition**, not
  file-size arithmetic. Exact byte sizes, record counts, one-record-per-flush
  indexing, `T_1`/`P` reconstruction, and the in-therapy header probe are no
  longer required for the primary result.
- The early header probe (base §8.2) is demoted from "necessary for early
  offset" to an **optional post-therapy validation artifact** parsed during
  ordinary downloads. No in-therapy body read is required at all.
- Per-file EDF layouts are never cross-applied (validation F10 makes the old
  coupling incorrect, not merely fragile).
- Flush cadence, when published, comes from **stamp deltas** (measured ~60 s,
  2 s quantization) as a diagnostic — not from a `(T_1, P)` feasible-region
  fit. The comb fitter is demoted to an optional H2-gated diagnostic profile.

**Retained unchanged (resilience skeleton):**

- Therapy FSM, thresholds, fail-closed transfer gating (§7) — validated
  as-is; stamp-only changes already count as growth.
- §8.1 anchor discipline, monotonic-domain rules, and uncertainty arithmetic.
- One-worker card ownership, request budgets/caps, backoff, evidence journal.
- SOT semantics: newest accurate wins, NVS persistence, freeze outside
  therapy, never fabricate, honest flags (U6–U7).
- Every failure degrades to a coarser honest answer (monotonic-only cadence,
  approximate tier, pending offset) — never to a fabricated number.

## U3. Update detection

Detection uses the per-file tuple **(size, timestamp_raw)**, compared as
parsed values with the raw text retained for diagnostics:

1. **Primary signal:** a sampled-file stamp change. Validated to fire on
   every flush, including flushes the rounded size cannot show.
2. Size change remains a change signal (belt-and-suspenders), and new/
   removed files are activity events.
3. **Beacon:** exactly one sampled file (BRP preferred; PLD/SA2 eligible).
   Annotation files are excluded from timing use — their mtime is the
   creation time (F5) — but their creation is a session-start event.
4. Shrinkage, backwards stamps, missing metadata, or size/stamp disagreement
   beyond the encoding rule (`listed = ceil(exact/1024)` KiB) are
   **uncertain-write/discontinuity** inputs to the FSM and estimator — never
   silently normalized.
5. The therapy FSM's existing `changed` semantics (size or stamp) are
   retained; no threshold changes.
6. **Poll unit:** the day-folder listing is the only source of per-file
   stamps — the card API has no per-file stat endpoint. One listing serves
   both the therapy FSM (any-file change) and the estimator (beacon stamp);
   per-file requests would double the count for less information. Never
   partially parse or early-exit a listing to save time (§11.2).

## U4. Timestamp-first offset estimator

Replaces the `(T_1, P)` record/flush fit as the primary path. For one
validated stamp transition of the beacon:

```
stamp interval   D  = [M, M + q)          q = 2 s (FAT, F2-validated)
visibility window [E_lo, E_hi] = previous unchanged scan start − cache bound
                                     … changed response end
offset interval   = [E_lo − (M_hi), E_hi − M_lo]     (true − device)
```

Requirements:

1. **Interval arithmetic only.** Each observation contributes an offset
   interval; the estimate is the intersection of compatible intervals within
   one uninterrupted clock segment. Empty intersection ⇒ invalidate or split
   the segment with a flag — never discard contradictory polls to force a
   fit.
2. **True-time windows** come from the §8.1 anchor mapping over the
   observation bracket (previous scan start − cache bound → changed response
   end), with anchor uncertainty carried through. No accepted anchor ⇒
   monotonic-only operation; offset stays null with `no_anchor_2h` semantics.
3. **Quantization:** the stamp is a truncated 2 s value: event ∈ [M, M+2).
   This 2 s width is irreducible on the mtime channel and dominates the
   achievable bound (see U5).
4. **Mid-session join is supported:** a fresh baseline scan plus subsequent
   updates plus one anchor suffice — no requirement to observe session
   start. `observed_late` applies only when no usable baseline exists.
5. **Cadence diagnostic:** stamp deltas publish `flush_period_ms` (validated
   ~56–60 s, first flush ~63–65 s after start). This is diagnostic output,
   not an offset input, and never feeds a rate claim.
6. **Latency prior:** the stamp-to-visibility delay is characterized once
   (dense-poll gate, U9). Until characterized, the conservative observed
   bound applies and keeps the tier `approximate`. The prior is a bound with
   provenance, never an allegedly measured constant.
7. **Latency comes from poll placement, not request size.** The visibility
   term is set by poll spacing; the measured ~236 ms listing service time
   adds only ~0.12 s to the half-width. Minimize latency by placing polls
   densely inside the predicted update window (U9 burst shape) — never by
   shrinking, filtering, or partially reading the request.
8. **No runtime header dependency.** The filename carries the same start
   claim as the EDF header (validated: header `starttime` == filename time),
   and stamps carry the offset observable — no estimator path reads a
   header, and no BLE operation exists for header reads. Re-characterization
   of a replacement card/machine is a host-side action (SomnoTrace FTP or a
   laptop on the card AP), recorded as a characterization update. The header
   parser is retained only for an opportunistic bounded-prefix parse during
   ordinary post-therapy downloads — zero extra requests or bytes; header
   reads never touch sample data (H ≤ 16 KiB).
9. `computed_at_ms` and `reference_at_ms` are both published (U6).

## U5. Accuracy tiers (revised by validation)

The record carries `accuracy_tier` (`approximate` | `accurate`) and
`accuracy_half_ms` — half-width of the published offset interval, always
reported numerically when a bound exists.

- `approximate` — a bounded offset exists but the accurate criteria are
  unmet. Expected with 60 s liveness polling alone (half-width ~30 s+).
- `accurate` — latched when **all** hold:
  1. `estimate_state` is `provisional` or `validated`;
  2. `accuracy_half_ms <= 2000`;
  3. at least one accepted anchor contributed;
  4. the stamp channel is validated for this card profile (U9 gate);
  5. the bound held across intervals from **at least two independent update
     windows** (not two refits of the same evidence).
- **Gate rationale (measured):** per-observation interval width =
  2 s stamp quantization + visibility window + latency range; half-width =
  width / 2. With 0.5 s dense bursts (visibility ≈ 0.75 s) and a latency
  prior of [0, 1] s, half-width ≈ 1.9 s — the gate is met. A 1 s half-width
  (2 s total) is unreachable: the 2 s quantization alone fills the budget
  and quantization error does not average away. **±2 s each way is the
  certified tier**; the record always reports the true half-width, and a
  wider characterized latency simply keeps the tier `approximate` — the
  gate is a pure function of the published bound.
- Regression on discontinuity drops the tier honestly (approximate/pending/
  invalid per §8.4 rules).

## U6. Source-of-truth value and NVS persistence

Unchanged from revision 1, with one addition:

- SOT = newest accurate-tier estimate; promoted immediately; later accurate
  refinements supersede within a session (newer `computed_at_ms` wins).
- Persist triggers: IN→OUT transition; first `accurate` in a session;
  optional throttled improvement writes (≥250 ms shrink, ≤1/60 s, bounded).
- Freeze outside therapy; late anchors refit history but never replace a
  persisted SOT.
- NVS blob adds **`reference_at_ms`** (the UTC instant the offset applies to)
  alongside `computed_at_ms` (when it was fitted). A stored value's accuracy
  speaks to its reference instant, not to the clock today.
- Invalidation on card-identity/settings change or factory reset, per §9.

## U7. Serving, API, and script

Unchanged from revision 1:

- Boot loads the NVS SOT immediately; a new session's accurate value
  supersedes by `computed_at_ms`; approximate/absent estimates never
  displace it; absence is data (`ok` with nulls), not an error.
- `GET_DRIFT {}` takes no arguments and returns `estimate` + 
  `source_of_truth` (with `origin: session|nvs`, `persisted_seq`).
- `ble_query.py --drift` drops positional args and `--drift-auto`; prints
  both blocks; no discovery requests on the drift path.

## U8. LCD drift status

Unchanged from revision 1: two independent rows (`D:` estimate with tier
marker and half-width; `S:` SOT with provenance and age), human-scaled
signed values, never null-as-zero, rendered from the same snapshot `GET_DRIFT`
serves, preempted by pairing/setup/fault overlays.

## U9. Validation-derived parameters and pending characterization

Measured on the validated combination (see `validation-results.md`):

- Stamp quantization `q = 2 s` (truncation, not rounding).
- Size encoding `listed = ceil(exact/1024)` KiB (5/5 files, 832–25,032 B).
- Flush cadence ~56–60 s; first flush ~63–65 s after therapy start; only
  complete records flushed (exact size = H + k·R, no trailing bytes).
- **Card-side listing service time (day folder, 51 files): 218–243 ms
  (median ≈ 236 ms, n = 5).** Fits 0.5 s dense spacing with ~260 ms margin.
  Fresh listings matched exact lengths (no stale-cache discrepancy
  observed).
- **BLE transport (never in the polling path):** small-response round-trip
  floor ≈ 184 ms; cached 51-file view ≈ 1229 ms. Burst polls are
  scheduler-internal card requests; BLE cost applies only when serving
  results to SomnoTrace.
- Write-to-visibility latency: bounded [≈0, ≈50 s] — **not yet tightly
  characterized**.

**Dense-burst characterization procedure (the `accurate` gate):**

- Spacing **0.5 s** (measured 236 ms service fits with margin).
- **Adaptive window:** open at predicted update ±4 s while the cadence fit
  is loose; tighten toward ±1.5 s after 2–3 fitted stamp deltas.
- **One-sided stop:** if the stamp already changed when the window opens,
  stop the burst — the observation still counts with its wider window and
  the next burst uses the improved prediction. If the window closes with no
  change, fall back to liveness and re-fit; a late miss alone never
  classifies therapy as stopped (§6.3).
- **Refine only the first 3–5 flushes per session** (~20–40 requests, inside
  the 40/set acquisition allowance), then 60 s liveness — the intersection
  floor is reached after ~2–3 independent windows.
- **Safety:** this is deliberate contact with the write burst — requires the
  base H3/E0b gate (one controlled run, then EDF record-count/CRC and
  SD-error verification) before it ships.
- **Output:** the latency prior with provenance; `accurate` unlocks only if
  the prior keeps the published half-width ≤ 2000 ms.

Other pending gates before `accurate` may be enabled:

1. **≥10 varied sessions** (base-spec H2): start phases, pause/resume,
   stop/restart, one >1 MiB file (MB-unit question).
2. Estimator-candidate comparison on held-out sessions against the
   SomnoTrace reference (offset error vs declared interval coverage).

**Required fixes surfaced by measurement:**

- **GET_FILE size gate (demonstrated blocker):** the catalog's ceil-KiB
  listed size is used as `expected_size`, but the card's true
  Content-Length is smaller (listed 2048 vs exact 1992) — every DATALOG
  download fails before the first body byte. Fix: accept the exact length
  within the ceil-KiB envelope (exact ≤ listed < exact + 1024) and use the
  listing-derived `?file=` path (resolver implemented and host-tested).
- **LIST_DIRECTORY outside DATALOG (pre-existing):** the card's root page
  carries non-listing anchors (page chrome) that the strict parser rejects
  wholesale — live non-DATALOG listings currently fail. Low priority; the
  therapy path is unaffected; fix only with §11.2 fail-closed parsing
  intact.
- **Folder-growth guard:** listing service time scales with entry count
  (~90 B/row). The scheduler tracks the service-time median; spacing MUST
  widen when service time approaches the poll interval (base §6.3).
- Optional future (not v1): one idle-time H4-gated probe of Range /
  `Last-Modified` behavior — if the card serves per-file headers, a 1-byte
  ranged GET becomes a ~20–90 ms per-file probe. The stamp channel does not
  require it.

## U10. Touchpoints and tests

Firmware:

- `catalog` — tuple change detection already present; expose stamp-transition
  events to the estimator feed.
- `drift` — replace session core: stamp-event observations, interval
  intersection, segment handling, tiers, SOT struct; `edf_hdr` retained for
  opportunistic download-prefix validation and diagnostics only.
- `scheduler` — feed stamp events; hook IN→OUT → persist; optional bounded
  dense window gated on the characterization record.
- `ble_ops`/`ezshare` — `GET_DRIFT {}` envelope (U7); fix the GET_FILE size
  gate (U9). No header-inspection op: the BLE surface is exactly the base
  §10.2 set plus the envelope change.
- `settings`/NVS — `drift_sot` blob (U6).

Host tests MUST cover: interval intersection and empty-intersection
invalidation, segment split on backwards stamps/steps, quantization
envelope (`[M, M+2)`), ceil-KiB consistency check, tier transitions with
two-independent-window hysteresis, SOT supersede ordering and freeze, NVS
round-trip/corruption, mid-session join, GET_DRIFT envelope combinations,
and the script's argument handling.

## U11. Rollout order

1. Detection: stamp-event plumbing + journal records (no estimator change).
2. Estimator: interval intersection replacing the comb fit as primary;
   comb fit demoted to optional profile.
3. Tiers + SOT + NVS + `GET_DRIFT {}` + script (U5–U7).
4. LCD block (U8).
5. Dense-poll characterization gate → enable `accurate`.

Each step keeps the previous tier's behavior available behind flags so a
regression can revert without losing detection or persistence.

## Open advisory summary

- **Accurate gate:** `accuracy_half_ms <= 2000` ms (±2 s), held across
  intervals from two independent update windows. The 2 s stamp quantization
  makes ±1 s unreachable on this channel; report the true half-width.
- **Out-of-therapy trigger:** unchanged —
  `out_timeout_ms = max(125000, 2 * expected_append_max_ms + allowance)`.
- **Offset-only first:** no RTC-rate or timer-rate claims in version 1 of
  this update; `rtc_rate_ppm` stays null on SomnoPal (base §8.4).
- **Achievable precision:** interval width = 2 s quantization + visibility
  + latency; with 0.5 s dense bursts and a latency prior ≲1 s, half-width
  ≈ ±1.9 s — the **±2 s each way** gate is realistic. ±1 s total is
  unreachable on this channel; optional refinements (ctime at creation,
  post-therapy header) are out of scope for v1.
