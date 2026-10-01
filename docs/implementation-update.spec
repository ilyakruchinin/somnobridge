# SomnoPal — implementation update: timestamp-first drift estimation, accuracy tiers, and persisted source of truth

Status: proposed requirements, **revision 2**. Normative delta over
`implementation-spec.md`; where they conflict, this document wins for the
affected behavior. Revision 2 replaces the record/flush-comb estimator as the
primary drift path with a **timestamp-first offset estimator**, based on the
measured evidence in `docs/validation-results.md` (masked-session validation
against an independent AS11 clock reference). All other guarantees of the
base spec stand.

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
7. **No runtime header dependency.** The filename carries the same start
   claim as the EDF header (validated: header `starttime` == filename time),
   and stamps carry the offset observable — no estimator path reads a
   header, and no BLE operation exists for header reads. Re-characterization
   of a replacement card/machine is a host-side action (SomnoTrace FTP or a
   laptop on the card AP), recorded as a characterization update. The header
   parser is retained only for an opportunistic bounded-prefix parse during
   ordinary post-therapy downloads — zero extra requests or bytes; header
   reads never touch sample data (H ≤ 16 KiB).
8. `computed_at_ms` and `reference_at_ms` are both published (U6).

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
- **Gate rationale (measured):** the 2 s stamp quantization makes a 1 s
  half-width unreachable on this channel; ±2 s is the honest achievable
  target and matches the original requirement's primary figure. ±1 s is a
  theoretical floor, not a target. If the characterized latency prior is
  wider than ~2 s, the interval simply stays wider and the tier remains
  `approximate` — the gate is a pure function of the published bound.
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
- Listing service times 20–243 ms; fresh listings matched exact lengths
  (no stale-cache discrepancy observed).
- Write-to-visibility latency: bounded [≈0, ≈50 s] — **not yet tightly
  characterized**.

Pending gates before `accurate` may be enabled:

1. **Dense-poll latency characterization:** one bounded window (~5 s spacing,
   ≤3 min, inside the existing 40/set acquisition allowance) during a live
   session, using the per-scan request timestamps now logged. Produces the
   latency prior and its provenance.
2. **≥10 varied sessions** (base-spec H2): start phases, pause/resume,
   stop/restart, one >1 MiB file (MB-unit question).
3. Estimator-candidate comparison on held-out sessions against the
   SomnoTrace reference (offset error vs declared interval coverage).

## U10. Touchpoints and tests

Firmware:

- `catalog` — tuple change detection already present; expose stamp-transition
  events to the estimator feed.
- `drift` — replace session core: stamp-event observations, interval
  intersection, segment handling, tiers, SOT struct; `edf_hdr` retained for
  opportunistic download-prefix validation and diagnostics only.
- `scheduler` — feed stamp events; hook IN→OUT → persist; optional bounded
  dense window gated on the characterization record.
- `ble_ops` — `GET_DRIFT {}` envelope (U7). No header-inspection op: the BLE
  surface is exactly the base §10.2 set plus the envelope change.
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
- **Achievable precision:** with a characterized small latency prior and a
  ~5 s dense window, the intersection floor is ≈ (q + latency range)/2;
  ±2 s is the realistic certified tier, ±1 s only via optional refinements
  (ctime at creation, post-therapy header) that are out of scope for v1.
