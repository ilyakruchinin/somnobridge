# SomnoPal — implementation update 6: sustained refinement and recent-session fallback

Status: implementation handoff. The user approved the recent-session fallback
policy in sections 8–12 to fix `TODO.md`. Sections 1–7 describe sustained
refinement, with concrete initial implementation choices in section 13.
No firmware changes, tests, live measurements, flashing, or therapy control
have been performed by editing this document. The filename follows the
requested spelling.

The base implementation spec remains authoritative for safety, privacy,
authorization, and resource limits. The maintainer explicitly approved removing
the cumulative 700-listing/eight-hour cap; the base contract now permits guarded
sustained refinement without a cumulative allowance. Section 13.5 specifies
this change, telemetry, and retained instantaneous/per-window protections.
No unresolved cumulative polling-budget approval blocks this implementation.
When implementing approved behavior, synchronize the base contract and older
timing docs so historical replacement rules do not remain contradictory.

## 1. Recommendation in simple words

Once SomnoPal learns when the next card update is likely, keep taking a small
cluster of spaced readings around that predicted update throughout therapy.
Move that cluster earlier or later as new readings show the update moving.
Do not deliberately drop to a coarser schedule simply because acquisition
finished. Slow down or widen the search when actual card response times or
missed updates require it.

This sustained mode can be called `REFINE2`. Its purpose is to keep producing
fresh, tight observations, not to preserve a best-ever number indefinitely.
Pair it with bounded rolling evidence and fresh host time anchors. More
precise polling alone cannot fix the existing frozen SOT API behavior.

The user's proposal makes sense. A few well-spaced requests near each expected
flush are very different from polling rapidly throughout the entire minute.
Short REFINE runs demonstrate feasibility under those conditions, not proven
10-hour reliability. There is no measured basis here for asserting that
sustained REFINE would overload the card, or that MAINTAIN is necessary.

## 2. What the code actually does today

### 2.1 Acquisition and stage progression

`components/scheduler/phase.c` defines these nominal request-start spacings:

| Stage | Nominal spacing | Grid shift |
|---|---:|---:|
| WATCH / HUNT | 5 s | None |
| NARROW1 | 2 s | None |
| NARROW2 | 0.8 s | None |
| VERNIER | 0.8 s | +0.4 s |
| REFINE | 0.5 s | +0.2 s, then +0.4 s |
| MAINTAIN | 1 s | None |

These are defaults, not guaranteed actual request times. Both the phase
planner and executor increase spacing to at least twice the measured service
median. Backoff and worker delays can increase it further.

The stage normally advances after one successfully bracketed transition.
The minimum-pull counts of three or four are requests planned inside a
window, not three or four flush cycles required per stage. REFINE normally
lasts two successful cycles. There is no mandatory 20-minute acquisition
period; elapsed time depends on discovery, flush phase, misses, and responses.

MAINTAIN already re-enters REFINE for one cycle when the stored phase width
exceeds 800 ms (`phase_obs`). This is not feedback from the clock-offset
half-width, nor a test of the next-cycle prediction width.

### 2.2 What is adaptive and what is not

The next-cycle window is rebuilt from recent observations. The predictor fits
phase and period jointly using up to 24 transition brackets, assumes nominal
period allowance and per-cycle variation, and predicts an interval for the
next update. Consequently, even today's positive grid shift is relative to a
moving prediction, not an ever-increasing delay from therapy start.

However, the +0.2/+0.4 s shifts themselves are predetermined, not feedback
that chooses an earlier or later shift from the observed slot. They translate
the planned window; there is no explicit rule preserving coverage of the
unshifted earliest boundary when this happens.

A transition cancels remaining requests in the current window. The code uses
the most recent unchanged request start and the first changed response end
as the observation bracket. There are at least three planned requests in
REFINE, but not necessarily three executed requests, and broad predictions
can require more (up to ten).

### 2.3 Critical overnight limitation: the estimator fills up

The phase predictor rolls its 24-bracket history, but the drift estimator
has a different buffer: `DRIFT_EV_CAP = 96`. `push_ev()` sets an overflow flag
and discards every new estimator event once that buffer fills.

At approximately one event per minute, saturation happens around 96 minutes,
not ten hours; creation events and actual cadence can change the time.
Catalog changes still feed phase tracking after saturation, but no fresh
clock-offset evidence enters the estimator. Its latest reference can stop
advancing while polling continues. Sustained REFINE alone would not fix this.

### 2.4 The live estimate can widen

Before intersecting the next mtime observation, the estimator expands its
previous accumulated interval by elapsed time times `dev_rate_ppm` (currently
500 ppm). It then intersects with the new observation interval. This supports
changing offset, but is not an explicit rolling estimator buffer today.

The width can increase, decrease, or remain similar. Earlier statements that
it can only shrink, or always retains its REFINE peak, were incorrect. No
fixed 30–60 minute effective window or MAINTAIN accuracy floor has been
established from the code or the short runs.

### 2.5 Host mapping also ages

ESP monotonic time must be mapped to UTC. Outside host-anchor pairs,
`drift_map_utc()` expands mapping uncertainty at 40 ppm. Ten hours from a
single anchor adds approximately 1.44 s per edge, before other uncertainty.
A single start-of-night sync cannot support a promise of unchanged precision
all night even with perfect flush polling.

## 3. Three separate moving quantities

1. **Visible flush phase:** when an EZShare listing first exposes a file
   update. This is what the request scheduler predicts.
2. **AirSense clock offset:** true UTC minus the machine's file timestamp.
   This is what SomnoTrace needs for correction.
3. **SomnoPal monotonic-to-UTC mapping:** maintained by authenticated host
   time exchanges, with its own uncertainty and epochs.

Following the visible flush does not directly measure the machine's RTC rate.
Flush timing can also move because of buffering, load, and visibility delay.
The machine's clock and the card's visible-update schedule must not be treated
as one proven oscillator. The listed timestamp is a machine-domain claim,
not independently established evidence of an EZShare RTC.

A 0.4–0.5 s offset change over ten hours corresponds to about 11–14 ppm. That
is a useful test scenario, not a validated replacement for the 500 ppm bound.
At 500 ppm, propagating an unsupported old interval for ten hours adds 18 s
per edge; fresh evidence is essential.

## 4. Why centered, bidirectional adaptation is preferable

The user's idea is correct, with one important refinement: there is no unique
"middle request" at which the flush should happen. A request takes time, and
an update occurring during it cannot safely be assigned an exact instant.
The goal is a recent unchanged reading immediately before the update and a
changed reading immediately after it, with a small measured bracket.

Example only, ignoring response duration for illustration:

- Predicted update: 60.00 s.
- Request starts: 59.25, 59.75, 60.25 s.
- The first two are unchanged and the third is changed.
- The update is bracketed between the second request start and third response
  end. A change at the second request can also be useful if the first was
  unchanged. Neither result alone proves the prediction was wrong.

Choose this orientation so the expected update sits between the latter two
starts, with an extra early baseline. Cancel unneeded requests on detection.
If every request is unchanged, the update may be late or absent. If the first
request is already changed, the update may be early; an old unchanged reading
must not be mistaken for a tight bracket.

For the next cycle, fit a prediction from actual interval evidence. Observed
updates consistently earlier than expected move the cluster earlier;
consistently later updates move it later. Use bounded interval constraints,
not a claimed exact flush time or a slot-to-200-ms lookup table. Ambiguous
intervals do not justify a definite signed correction.

Positive and negative motion is therefore supplied by the predictor. A small
optional symmetric dither is a separate experiment for avoiding repeated
sampling alignment; it is not the mechanism that follows clock drift.

## 5. Proposed REFINE2 requirements

### 5.1 State and coverage

- Retain WATCH/HUNT and the acquisition ladder for initial discovery and
  reacquisition. After successful REFINE, enter sustained REFINE2 instead of
  automatically coarsening to MAINTAIN.
- Recompute next-cycle phase/period bounds on every usable transition from
  recent evidence. Use a bounded rolling predictor with explicit interval
  allowances for changing period and cycle-to-cycle variation.
- Reusing today's 24-bracket predictor is a starting point, not proof its
  constant-period fit handles all overnight temperature/rate changes. Test
  curved phase trajectories and define when to expire/reset incompatible
  older constraints rather than merely inflating all variation indefinitely.
- Center the cluster relative to the next-cycle prediction, not the historic
  base-cycle phase width. Allow it to move in either direction.
- For a narrow prediction, plan three starts at nominal 0.5 s spacing. A
  starting template is `center - 1.5*g`, `center - 0.5*g`, `center + 0.5*g`,
  where `g` is the guarded spacing. This is not sufficient coverage for every
  possible prediction width or service time; expand as needed.
- Ensure the first reading can supply a fresh unchanged baseline before the
  earliest plausible update and the final response covers the latest one.
  Account for service uncertainty, lateness, and configured edge allowances.
- Broader prediction requires more spaced requests or reacquisition. Never
  claim three requests suffice regardless of uncertainty. Retain the bounded
  ten-request limit and explicit miss fallback; never silently truncate the
  required coverage to meet the cap.
- Cancel remaining requests once a usable transition is detected. After a
  failure or miss, widen/reacquire and use existing backoff. Never issue a
  burst of overdue requests to catch up.

### 5.2 Actual spacing and load

- Initially retain the 0.5 s REFINE spacing floor, service-time guard, serial
  card worker, transfer gating, and backoff. Do not introduce 0.2 s request
  spacing merely because the old dither used a 0.2 s shift.
- Approximately three requests per 60 s cycle is about 180 requests/hour and
  1,800 over ten hours, excluding coverage traffic, acquisition, and retries.
  This is an arithmetic workload estimate, not measured card utilization.
- Sustained load must be judged from actual response distributions, failures,
  misses, WiFi behavior, and request counts. A short healthy run does not
  establish long-duration reliability, but also does not justify assuming
  this sparse cluster is excessive.
- Begin with existing service guards; evaluate whether a recent high-percentile
  response statistic improves resilience beyond the current eight-sample
  median. Delayed responses must affect actual evidence bounds regardless.

### 5.3 Dither is optional, bounded, and separate

- First implement/test centered adaptive polling without obligatory dither.
- Compare a bounded symmetric pattern, for example alternating −0.1/+0.1 s,
  only if it improves measured observation or offset widths at similar load.
  These are trial values, not characterized requirements.
- Do not accumulate shifts indefinitely. A dither may move inner sample
  positions only if the full prediction interval retains coverage; otherwise
  add a guard request or skip the dither.
- Shifting polls improves where an update is bracketed. It does not move the
  actual flush or change the FAT timestamp bin. It cannot by itself undo
  2-second timestamp quantization. Extra intersections help only when the
  actual mapped offset constraints provide complementary information.

### 5.4 Rolling offset evidence is mandatory

- Replace the stop-at-96 behavior with bounded rolling recent evidence; keep
  accepting new transitions for the full therapy duration.
- A candidate starting design is 32 recent transition observations, evaluated
  at the newest observation reference. The chosen count/time horizon must be
  tested, not presented as a proven optimal 32-minute window.
- Retain raw intervals and propagate rate uncertainty before intersection.
  Do not intersect offsets from different times as one immutable offset.
- Keep session-start/creation metadata separately from rolling current-offset
  evidence so eviction does not destroy start information or mix filename
  origins. Preserve clock-segment, boot, anchor-epoch, and beacon identity.
- Treat planned eviction differently from lost evidence/overflow. Expose
  retained span, newest reference, total observations, eviction count, and
  true losses separately.
- Clock steps, contradictory observations, and missing origin information
  must remain explicit; never smooth across them to keep a narrow number.

### 5.5 Maintain the host time reference

- The connected authenticated host should supply periodic bounded anchors
  throughout therapy; do not invent UTC from card polling.
- Choose cadence from an explicit uncertainty budget. At the installed 40 ppm
  extrapolation allowance, ten minutes adds about 24 ms per edge, whereas ten
  hours adds 1.44 s. Ten-minute sync is a candidate starting cadence, not a
  verified optimal setting.
- Preserve source uncertainty, best-effort versus qualified distinctions,
  reconnect behavior, and source epochs. Bounded anchor retention must also
  be compatible with the rolling observation horizon.
- If the host is absent, report growing mapping uncertainty instead of
  promising constant precision.

### 5.6 API and NVS boundary

Sections 8–12 now specify the user-approved fix for the persisted-record
selection flaw. REFINE2 supplies fresh evidence; the approved fallback policy
selects between that evidence and a recent completed-session record.

Do not retain the old quality-first/width-only persisted selector or adopt the
previously rejected union/regression-cap/forecasting remedies. Changing the
polling-stage name alone is not a SOT fix. The implementation must integrate
rolling evidence, read-time selection, and session-end persistence. Revision 4
is the existing firmware; section 11 specifies migration for update 6.

## 6. Verification plan before claiming overnight precision

### Deterministic host tests

- Ten-hour/600-cycle run: latest estimator reference keeps advancing beyond
  observation 96; memory remains bounded and eviction preserves identities.
- Gradual ±11–14 ppm machine-offset drift, plus changing drift rate and flush
  phase in both directions; distinguish machine offset from visibility jitter.
- Assert the current mapped interval encloses the synthetic true offset at
  the same reference time. Allow honest width growth; never force a target.
- Three-pull early/middle/late outcomes, first-pull changes, all-unchanged
  misses, skipped cycles, delayed worker starts, failed/slow responses.
- Every scheduled cluster covers its declared feasible prediction or takes
  an explicit fallback. Guard spacing and request caps always hold.
- Compare sustained centered REFINE2, symmetric dither, and existing
  MAINTAIN/REFINE behavior using identical synthetic traces.
- Beacon changes, midnight, session restarts, reboots, host disconnects,
  source-epoch changes, and rolling filename-generation preservation.
- Single old host anchor versus periodic anchors: reported uncertainty must
  reflect mapping age and must not silently stay at its start-of-night width.

Use `./scripts/run_host_tests.sh`,
`python3 scripts/validate_timing.py self-test`, and `./scripts/idf.sh build`
for implementation verification. Targeted regression tests must fail against
current behavior before the relevant fixes are accepted.

### Hardware evidence

Perform a consented long-duration passive observation, ideally a full night,
with independent bounded references near the beginning, middle, and end.
The observer must not add card polls beyond the firmware's tested schedule.
Collect only private timing/layout metadata, not patient fields or waveforms.

Compare actual bracket widths, current offset widths, reference containment,
response-time distributions, failures, misses, actual pulls per cycle, and
reference age over the full run. Separate host mapping uncertainty from the
flush measurement uncertainty and evaluate both drift directions in tests.

Success is fresh evidence throughout the run, bounded resource use and load,
and honest current-time uncertainty without a systematic avoidable precision
loss after acquisition. Neither ±1 s nor permanent ±1.3 s accuracy is promised.
`stamp_channel_validated=false` remains unchanged without separate validation.

## 7. Code locations supporting these findings

- `components/scheduler/phase.c`: grid constants, `grid_shift`, `win_arm`,
  `phase_due`, `phase_obs`, rolling predictor fitting and miss paths.
- `components/scheduler/include/phase.h`: stage definitions and 24-bracket cap.
- `components/scheduler/sched.c`: `earliest_start`, `plan_phase`, measured
  service median, card worker, and continued phase tracking after event cap.
- `components/drift/drift.c`: `push_ev`, `drift_sess_eval`, `drift_map_utc`,
  `sot_offer`, `drift_sot_apply`, and `drift_sot_json`.
- `components/drift/include/drift.h`: 96-event cap, 500 ppm profile semantics,
  and 40 ppm host-mapping extrapolation allowance.
- `docs/TODO.md`: existing persisted-record freshness and selection defects;
  sections 8–12 below are the approved implementation remedy.

## 8. Approved policy: latest good session, not best-ever accuracy

### 8.1 Goals and non-goals

NVS holds a recent completed-session measurement for temporary use when the
current session has no useful estimate or is substantially less precise.
It is not a best-ever accuracy competition or a learned RTC forecast.

Separate three objects:

1. **Current evidence:** a live session estimate in RAM, continuously updated.
2. **Completed-session fallback:** the latest qualifying completed session,
   loaded from NVS or awaiting a retry of its session-end commit.
3. **Selected correction:** an immutable read-time view chosen by section 9.
   Reading or selecting must never modify or persist either source object.

Raw uncertainty means half-width, e.g. ±1500 ms, not total interval width.
Clock correction sign remains true UTC minus device time. Quality certification
is separate from numerical half-width and selection provenance.

### 8.2 Build constants and exact boundaries

| Constant | Initial value | Meaning |
|---|---:|---|
| CURRENT_GOOD_HALF_MS | 2000 | Current estimate is good enough at or below this value |
| SAVE_GOOD_HALF_MS | 2000 | Latest final estimate may be saved at or below this value |
| FALLBACK_MAX_AGE_MS | 129600000 | Eligible through 36 hours inclusive |
| FALLBACK_ALLOWANCE_MS | 1000 | Fixed additional half-width for using an older session |
| FALLBACK_ADVANTAGE_MS | 500 | Required advantage over current, inclusive |
| USABLE_HALF_MAX_MS | 5000 | Maximum selected half-width, inclusive |
| CURRENT_REF_MAX_AGE_US | 180000000 | Live-selection freshness: reference no more than 180 s old |
| FINAL_REF_MAX_AGE_US | 240000000 | Session-end save envelope: newest reference no more than 240 s old at confirmed OUT |

The first five values implement the approved recommendation. The 5 s limit
preserves the current provisional numerical ceiling. The 180 s freshness
limit is a specified initial implementation choice, allowing normal minute
updates and stop-detection latency without accepting hours-old live evidence.
Keep constants named and host-testable; do not introduce a portal tuning UI.

The fixed 1 s allowance and 36 h expiry are best-effort operating assumptions,
not measured RTC bounds or a confidence guarantee. Do not substitute the
existing 500 ppm rate for the fixed fallback allowance: that would implement
a different policy. Retain the 500 ppm model for transporting live evidence
across short gaps and the 40 ppm host mapping allowance. Do not reduce either
bound based solely on the user's expected daily drift.

### 8.3 Validity before numerical selection

Current evidence must have a mapped, finite interval, a valid current clock
segment and compatible source context, and enough distinct observations.
Preserve existing observation gates: at least three for provisional evidence,
or at least two for evidence meeting all existing qualified gates. A repeated
one-second refit is not another observation.

Reject contradictory/source-disagreeing intervals, unresolved selected
filename origin/cycle, missing anchors, obsolete boot-domain evidence, and
unusable contexts. Unvalidated mtime or a best-effort anchor are not rejection
conditions for explicitly opted-in provisional use. A discontinuity invalidates
old-context fallback immediately; new-segment evidence can become usable again
once it independently passes the observation/consistency gates.

Eligibility also requires current reference age between zero and 180 s,
measured using monotonic time in this boot. A frozen closed record, an earlier
session's published snapshot, or an early-session best half-width is not
current evidence. No new therapy estimate is implied by THERAPY_UNKNOWN;
existing live evidence may remain eligible only within the same freshness
limit. Transfer gating stays conservative and independent of timing selection.

Fallback requires a valid committed/completed record with raw half-width
≤2000 ms, matching model and source context, known age, no invalidation, and
measurement age ≤36 h. If age is unknown or provably negative, reject it.
After reboot, obtain an authenticated current-time map before using fallback;
never infer age from uptime or reset its measurement time at load.

## 9. Read-time correction selection

### 9.1 Evaluate both candidates at the requested current time

**Current:** evaluate retained observations using the accepted host anchors;
this includes the host-map uncertainty at each observation endpoint. Snapshot
the resulting interval at the newest observation reference, then transport it
to query time using monotonic elapsed time in the same boot:

```
dt_us = query_mono_us - newest_reference_mono_us
widen_ms = ceil(dt_us * dev_rate_ppm / 1000000000)
current_lo = measurement_lo_ms - widen_ms
current_hi = measurement_hi_ms + widen_ms
current_half = ceil((current_hi - current_lo) / 2)
```

Require `0 <= dt_us <= CURRENT_REF_MAX_AGE_US`. This is a pure read-time
materialization of the published evidence, not an NVS write or an extra
observation. New accepted anchors may cause the owner to refit evidence before
publishing another snapshot. Do not add query-map uncertainty again to an
interval already mapped at the observation; query UTC is used for age/display.
Use overflow-safe arithmetic and outward rounding. `current_half_ms` may
exceed the historical measurement width even when the reference is fresh.

**Fallback:** determine age from its stored measurement reference, not when it
was written, refitted, loaded, or selected. Use the upper current UTC bound
for conservative age/expiry checking. The stored reference remains the
measurement-time claim with its original uncertainty metadata; the allowance
is explicitly a best-effort assumption, not a formally bounded transport.
If reference is later than the entire current UTC interval, age is invalid;
if the UTC interval overlaps reference, conservatively use nonnegative age.

For eligible fallback with stored interval `[lo, hi]`, the selected interval is:

```
fallback_lo = lo - 1000 ms
fallback_hi = hi + 1000 ms
fallback_half = ceil((fallback_hi - fallback_lo) / 2)
```

Apply the fixed allowance once, even immediately after completion or for a
same-day restart. Do not repeatedly widen the saved blob on API reads. The
fallback midpoint stays unchanged; do not predict its direction of movement.
Retain original interval/half-width separately for historical diagnostics.

### 9.2 Exact decision order for opted-in best_available

```
C = valid fresh current-session candidate evaluated at query time, else none
F = eligible completed-session fallback with fixed allowance, else none

if C exists and C.half <= 2000:
    select C, reason = current_good
else if F exists and F.half <= 5000 and
        (C is none or F.half + 500 <= C.half):
    select F, reason = fallback_no_current or fallback_materially_better
else if C exists and C.half <= 5000:
    select C, reason = current_preferred
else:
    select none
```

Evaluate validity before this comparison. If C is too broad but structurally
valid, its width can still be compared; it cannot be selected above 5 s.
A tie or advantage below 500 ms favors current. Exactly 500 ms favors fallback
unless current is already ≤2 s. Selection is recalculated on every read; no
session-best latch or selection hysteresis beyond this explicit margin.

Quality does not override this freshness policy: an older qualified record
must not automatically beat a newer provisional estimate. Provisional use
still requires opt-in. A saved qualified historical measurement used with the
assumed age allowance is a provisional fallback correction, not a currently
qualified correction.

### 9.3 Examples (already evaluated at query time)

| Current half | Stored half | Stored age | Selected result |
|---:|---:|---:|---|
| None | 1500 ms | 24 h | Fallback ±2500 ms |
| 4500 ms | 1500 ms | 24 h | Fallback ±2500 ms |
| 2500 ms | 1500 ms | 24 h | Current ±2500 ms |
| 3000 ms | 1500 ms | 24 h | Fallback ±2500 ms (exact 500 ms advantage) |
| 2999 ms | 1500 ms | 24 h | Current ±2999 ms |
| 1900 ms | 1000 ms | 24 h | Current ±1900 ms (current-good rule) |
| 2500 ms | 1500 ms | >36 h | Current ±2500 ms |
| None | 1500 ms | >36 h | No correction |
| 5500 ms | None | — | No correction; raw estimate remains diagnostic |

Stored age expiry applies even if repeated sessions never reach ±2 s.
Unusable new evidence must not extend fallback lifetime.

## 10. Session-end replacement and persistence

### 10.1 Save the final fresh result once

At a confirmed therapy transition to OUT:

1. Evaluate latest session evidence, including eligible final writes already
   observed, at the end decision time.
2. Build a final candidate using the same context/observation checks
   as section 8.3. Reference freshness uses the session-end envelope
   (reference age from zero through FINAL_REF_MAX_AGE_US = 240 s),
   not the 180 s live-selection gate. OUT declaration waits for ~125 s
   of quiet plus clean rounds; a bracketed reference may also predate the
   last growth. The 240 s limit is a bounded best-effort allowance for
   that delay, not a proven maximum or continuity guarantee. Its
   effective half-width at that decision time must be ≤2000 ms. Do not
   save an earlier peak if final evidence is worse or stale.
3. If qualifying, replace the completed-session fallback in RAM regardless
   of old width, interval overlap, or 250 ms improvement. Store the original
   latest-observation interval and its original reference together; the
   transported end-time interval is used for the eligibility test, not stored
   with an earlier reference. End/refit/write time must not masquerade as the
   measurement reference.
4. Commit the new fallback to NVS once. Report persistence success only after
   `nvs_commit` succeeds. A failed commit keeps a dirty pending candidate with
   its original reference, error, and sequence status for bounded retries.
5. If not qualifying, keep the old fallback unchanged, including its original
   reference, age, quality, and persisted sequence. Existing invalidation or
   expiry may still make that old fallback unusable.

For example, yesterday's ±1400 ms must be replaced by today's valid final
±1800 ms. Today's final ±2001 ms must not overwrite it. A 1-minute session
without enough observations must not save or refresh old evidence.

Both live promotion (`sot_offer`) and flash commit (`sot_commit_slot`) currently
contain 250 ms improvement rules. Remove these rules from this completed-session
fallback path. Bypassing only one is insufficient. Do not persist new fallback
on every refit, first-good transition, or read. Timing evidence/history journal
writes remain separate and bounded.

### 10.2 End identity, rollover, and retries

Use a RAM session generation or equivalent explicit completed-session identity
to prevent duplicate finalization/writes on repeated OUT polling. Do not put a
new boot counter in settings NVS. A same-day resumed session becomes a new
completion generation and may save a newer final estimate at its next OUT.
Its current evidence must still be fresh, with no false pre-gap tight bracket.

A folder/day switch during ongoing therapy is a source-context change, not
confirmed therapy stop. Existing `drift_live_end` is also called on that path;
do not attach unconditional NVS save to every `drift_live_end` invocation.
Finalize storage only for confirmed OUT. Preserve/reset evidence on rollover
as context requires and retain the latest applicable live result for OUT.
UNKNOWN, connection loss, reset, and API requests are not completion events.

The scheduler owns finalization, dirty state, retry bookkeeping, and commits;
BLE readers take consistent immutable snapshots, never write NVS. Preserve
existing sequence-counter/locking conventions for selected views.

On write failure, the pending completed record may be used as RAM fallback
under the same age rules with `persisted=false` and `dirty=true`; it must not
claim to survive reboot. Previous committed data remains the last durable
record. Retry no faster than once per 60 s with existing bounded recovery;
failed retries never reset age or advance persisted sequence. A later qualifying
completion supersedes the pending candidate. A later invalidation must prevent
retrying an obsolete candidate as valid. Do not erase unrelated NVS data.

After power loss before successful commit, loading the previous durable record
is acceptable only with its real age and eligibility checks. This is not a
reason to report it as the newly completed session.

### 10.3 Relationship to provisional and qualified slots

Use one authoritative **latest qualifying completed-session fallback** in a
single dedicated versioned blob under metadata NVS key `drift_fallback`,
regardless of whether its measurement is provisional or qualified. Do not
arbitrate old quality slots by minimum width or add an atomic multi-blob index.
Existing qualified/provisional records may remain as historical diagnostics,
but cannot bypass this fallback selection policy.

Do not overwrite a qualified-history slot with provisional data. Store
measurement quality in the fallback blob; selected assumed-age fallback quality
is provisional even when measurement quality was qualified. Keep optional
history maintenance separate from authoritative fallback replacement; history
is not required to provide the selected live-or-fallback correction.

## 11. API contract, compatibility, and migration

### 11.1 GET_DRIFT selected view

Bump the envelope schema from 4 to 5 because best_available now means selected
live-or-fallback correction, not only persisted SOT. Keep the request aliases
`best_available:true` and `include_provisional:true`; without opt-in, do not
expose provisional correction as usable qualified truth.

- `estimate`: raw latest live or closed-session diagnostic estimate, retaining
  its original measurement reference and `observation_closed` semantics.
- `best_available_offset`: selected view from section 9 when opted in, else
  null. `interval_ms`, `clock_drift_ms`, `accuracy_half_ms`, and
  `current_half_ms` describe the interval for use at query time, not a frozen
  historical width. Round conservatively and derive midpoint consistently.
- `source_of_truth`: only currently applicable qualified correction. Use a
  fresh live qualified candidate meeting current-time accurate gates; otherwise
  null. Never send assumed-age fallback through this legacy qualified field.
- `qualified_source_of_truth` and `provisional_source_of_truth`: retain as
  explicitly historical measurement diagnostics, with original intervals,
  real references, storage status, and no implied current applicability.
  Do not populate these archives from every live refit. Their absence must not
  prevent use of a valid live best_available candidate or latest fallback.
- Add `completed_session_fallback` as the diagnostic authoritative fallback
  measurement, including eligibility and rejection reason; null if absent.

Selected view must include:

```
selection_origin: current_session | previous_session_fallback
quality: provisional | qualified
measurement_quality: provisional | qualified
source: mtime | existing resolved filename source
reference_at_ms: original observation reference
computed_at_ms: original fit time
applies_at_ms: query-time UTC claim
reference_age_ms: conservative age, or null when unknown
measurement_interval_ms: original interval
measurement_half_ms: original half-width
interval_ms: interval selected for use now
accuracy_half_ms / current_half_ms: selected effective half-width
age_allowance_ms: 1000 for fallback, 0 for live
uncertainty_policy: current_rate_bound | fixed_recent_session_allowance
policy_assumed: true for fallback
applicable / freshness_unknown / stale
model_rev / model_id / assumptions / flags
persisted / dirty / last_error / persisted_seq
```

Preserve existing `origin` (session versus loaded NVS) independently of
`selection_origin`: an unsaved live estimate must not claim to be an NVS value.
A current selection normally has `persisted=false`; use zero/null sequence
consistently and document it. Pending completed fallback may have
`selection_origin=previous_session_fallback` but `persisted=false`.

Reasons must distinguish selected branches from failure: client opt-in
required, no usable current evidence, fallback expired, fallback age unknown,
context invalid, too few observations, current reference stale, excessive
width, and persistence failure. Add separate candidate rejection reasons if
needed so a single overall reason does not hide why a source was ineligible.
Remove the misleading `selected_by_quality_then_width` success reason.

Historical diagnostic fields must have `historical=true`,
`applicable=false` for direct current correction, and explicitly distinguish
historical validity from fallback eligibility. Expiry is evaluated on read,
not a flash rewrite. The authoritative fallback diagnostic exposes original
measurement width and `fallback_eligible`; consumers use the selected field
for adjusted current correction.

Use one selector shared by BLE/UI/tooling callers, not duplicated decision
logic in serializers. Unit-test selector output and complete GET_DRIFT JSON.
Update `scripts/ble_query.py` and `scripts/validate_timing.py` parsing/display
for schema 5, effective versus historical widths, selection provenance, and
null/unknown age. Maintain bounded serializers and explicit overflow errors.
Do not modify SomnoTrace source; document its opt-in consumption contract.

### 11.2 Revision and persisted format

Bump timing model revision from 4 to 5 for this implementation; include the
new selection policy and numerical constants in model identity/compatibility
checks. Any later semantic tuning requires another compatibility review.
`stamp_channel_validated` stays false, and best-effort anchor limits stay
unchanged. Selection policy must not confer physical certification.

Version and CRC-check the new fallback blob. Store original observation/fit
UTC references, interval, source, assumptions, measurement quality, model/context
identity, and persisted sequence; never store read-time widened intervals.
Runtime session generations are not persisted boot counters. Use the existing
metadata NVS partition and checked, power-loss-safe commit path.

Existing revision-4 blobs remain historical/incompatible and must not seed an
eligible revision-5 fallback by merely changing their revision number. No
silent erase/reset, promotion, or fabrication of current time. Until the first
qualifying revision-5 completion, a valid live estimate can be selected without
any eligible persisted fallback. Preserve corrupt/unsupported-record rejection
and independent qualified/provisional history semantics where retained.

### 11.3 Code integration checklist

- `drift.h` / `drift.c`: distinct live, completed fallback, selected-view state;
  shared pure selector and interval materialization; age/validity checks;
  rolling evidence and true newest-reference tracking; qualified separation.
- `sched.c`: remove first-accurate and improvement-triggered fallback commits;
  finalize only on confirmed OUT; serialize state transitions and bounded
  failed-commit retries. Keep journal writes and transfer gating intact.
- `ble_ops.c`: schema-5 snapshot/selection, effective intervals, opt-in and
  qualified-only contract, diagnostic fields, bounded serialization.
- Host NVS stubs/tests: new blob/key, commit failures, reload/migration,
  latest-completion replacement, selector and JSON integration regressions.
- Host scripts: display selected versus historical accuracy and age explicitly;
  scorer must use original evidence/reference for analysis, not mistake the
  fallback allowance for a newly measured observation.
- Docs: reconcile base contract and historical update-3/4/5 descriptions;
  mark TODO defects fixed only after tests prove the implemented behavior.

## 12. Required fallback regression tests and acceptance

Write failing tests for existing defects before implementation. In addition
to section 6, cover:

1. Raw saved ±1500 ms, age 24 h, current ±2500 ms: current wins; current
   ±4500 ms: fallback wins and API reports ±2500 ms, not ±1500 ms.
2. Margin boundaries: effective fallback 2500 versus current 3000 chooses
   fallback; versus 2999 chooses current. Current ≤2000 always wins when valid.
3. Age exactly 36 h is eligible; 36 h + 1 ms is not. Unknown/future age rejects.
   Day-14 record cannot be selected on day 56, even with no new good sessions.
4. Repeated reads and reboot/load never reset reference or compound allowance;
   an authenticated current map is required after reboot.
5. Old ±1400 ms replaced at OUT by final ±1800 ms; equal-width, overlapping,
   and non-overlapping valid final records all replace. Exactly 2000 saves;
   2001 does not. Commit success survives round-trip reload.
6. Earlier good peak followed by a worse final estimate does not save the
   peak. Reference older than 180 s cannot be selected as fresh live
   evidence; reference older than 240 s cannot be saved at finalize.
7. Few observations, contradictions, unresolved source, invalidation, and
   missed anchors cannot save/refresh an older record; a valid provisional
   unvalidated estimate can save without becoming qualified.
8. No per-refit/API/first-good fallback writes; repeated OUT is idempotent;
   same-day resume can produce another valid completion; midnight folder
   changes and UNKNOWN do not independently trigger completion saves.
9. Commit failure preserves age/sequence and dirty status, retries are bounded,
   success clears dirty, reboot restores only durable data. Test crash before
   commit and invalidation/new completion superseding a pending retry.
10. Older qualified history cannot override a newer provisional current or
    authoritative completed fallback. Assumed-age use of a qualified
    measurement is provisional and never appears in legacy source_of_truth.
11. Current evidence can deteriorate after improvement: selector changes as
    needed, does not latch peak. Effective current width includes short-gap
    rate widening and host-map uncertainty without double counting.
12. Schema-5 JSON has consistent selected midpoint/interval/half-width,
    provenance, historical diagnostics, opt-in gating, expiration reasons,
    persistence status, and no truncation. Scripts accept absent/null fields.

Completion requires passing host tests, timing synthetic self-tests, script
compile checks, target build, and reviewed documentation consistency. A live
smoke check is additional evidence only when device operations are authorized;
no automatic flashing or therapy control. Full-night behavior remains subject
to section 6 hardware evidence and section 13.5 measured-load checks.

## 13. Concrete initial REFINE2 handoff and measured-load requirements

### 13.1 Predictor and scheduling defaults

Implement REFINE2 as sustained 0.5 s nominal spacing with minimum three planned
starts, maximum ten, existing 0.5 s edge margin and measured service guard.
Keep the existing 24-bracket joint predictor and cycle assignment initially.
Retain per-cycle variation and miss/reacquisition paths. Do not introduce a
second phase-control loop or arbitrary signed slot nudges.

On inconsistent prediction fit, explicitly reset/reacquire from new evidence
as current code does. Planned rolling eviction is not a contradiction. A
future variable-rate predictor can be added only if tests show this baseline
cannot meet the tracking requirements; do not preemptively fit RTC slopes.
Enter REFINE2 after the existing two REFINE successes. After subsequent
successes stay in REFINE2; misses go to HUNT and then the acquisition ladder.
No compulsory dither in REFINE2 for initial implementation.

For predicted interval `[L,H]`, center `(L+H)/2`, spacing `g`, and margin `m`,
start with the three-start template from section 5.1. Extend its beginning
backward in multiples of `g` until it is no later than `L-m-service_guard`,
and extend its end forward until its last start is no earlier than
`H+m+service_guard`. This deliberately does not rely on a long HTTP response
to cover the last edge. Use the existing half-service allowance initially.
If coverage needs more than ten starts, take explicit broad-window fallback;
do not shrink the declared prediction or silently skip an edge. Honor actual
executor lateness/backoff; actual request/response bounds are authoritative.

### 13.2 Offset buffer defaults

Use a 32-transition rolling buffer initially, retaining separate creation/
session-start metadata. Evict the oldest transition on insertion, keep current
segment identity and discontinuity state independently of the ring, and
re-evaluate chronological retained evidence at the newest usable reference.
Reset filename-origin candidates when their binding changes; never combine
old creation candidates with a later segment or reference without the required
rate transport. Retain 500 ppm propagation, conservative quantization, and
current source-consistency rules. Expose eviction separately from true loss.

Avoid silently clearing clock-step invalidation when its triggering event
rolls out. Keep discontinuity generation and invalidated fallback context in
persistent/runtime metadata as needed; current new-segment evidence can recover
without reviving pre-step fallback. Diagnostic observation IDs remain monotonic
within a boot and do not reuse ring indices as identities.

### 13.3 Host anchoring scope

Use ten-minute anchors when an authenticated host collector/client supplies
them, with current exchange provenance and epoch rules. Firmware does not
actively invent a host RPC or become an NTP client. Update the existing host
tool's explicit observation workflow to permit this bounded cadence; absence
of a client remains supported with honest uncertainty growth. SomnoTrace
client implementation is outside this repository/task. Test the firmware with
synthetic anchors whether or not a production client currently supplies them.

### 13.4 Implementation order

1. Add selector/persistence regressions and rolling-buffer/long-run regressions.
2. Implement rolling evidence and true reference freshness, then the shared
   selector and confirmed-OUT storage. These fix TODO independently of load.
3. Integrate schema-5 API/tooling, model-5 migration, and checked serializers.
4. Implement/test REFINE2 with request-spacing, response-time, peak/window,
   and backoff guards retained. Remove cumulative budget enforcement/checkpoint
   gates if present; compare positive/negative and curved traces.
5. Run verification, reconcile contracts, review full diffs. Report actual
   service/load problems or remaining hardware qualification requirements
   explicitly, not as a completed characterization claim.

### 13.5 Approved removal of the cumulative listing cap

The maintainer approved removing the 700-listing-per-eight-hour cap. The base
contract's sections 6.1–6.4 now permit sustained guarded 500 ms-or-slower
refinement throughout therapy without cumulative request-count suspension.
Approximately three starts per minute plan 1440 requests over eight hours,
or 1800 over ten hours before housekeeping/recovery and cancellation. These
are diagnostic workload estimates, not limits or evidence of measured safety.

Implementation requirements:

- Do not stop, coarsen, or delay valid refinement because a lifetime, session,
  or rolling eight-hour request total is high. Do not replace 700 with another
  cumulative allowance, a longer window, or an approval switch.
- Remove cumulative budget enforcement, once-per-minute `load_budget` NVS
  checkpoint requirements, lost-tail reserves, and recovery wait periods if
  implemented. Existing legacy budget data may remain inert; do not silently
  erase unrelated metadata, settings, bonds, or partitions.
- Keep bounded RAM request counters by real purpose, service distributions,
  failures, and missed-window telemetry. Reboot may reset diagnostic counts;
  monitoring must not depend on reconstructing historic counts.
- Preserve one in-flight card request, no catch-up bursts, the 500 ms floor
  widened by measured service time, at most ten requests per prediction window,
  HTTP backoff, monitoring freshness, and therapy-aware transfer gating.
- Retain the legacy faster 200 ms profile qualification and short-term dense
  peak/acquisition protections. Do not charge sustained 500 ms-or-slower
  refinement to the legacy 40-request acquisition allowance or force MAINTAIN
  once that allowance is spent. Physical timing qualification remains separate
  from permission to execute the scoped best-effort schedule.
- Slow down or reacquire in response to actual latency, failures, uncertain
  predictions, or worker contention, rather than cumulative counts. Long-run
  measurements remain required to characterize load and interval accuracy.

Add regressions that execute more than 700 active listings and continue fresh
observation/REFINE2 scheduling across eight and ten hours; missing/corrupt old
budget blobs or failed legacy checkpoint writes must not gate monitoring.
Separately assert retained spacing, concurrency, per-window/peak guards, and
backoff; actual stale monitoring must still block transfers and never imply OUT.

This is a specification change, not a claim that firmware already removes the
cap or that ten-hour load has been hardware-characterized.
