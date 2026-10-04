# SomnoBridge — implementation update 3: refined offsets and persisted provisional SOT

Status: proposed implementation plan, revision 1, 2026-10-02.
Documentation only. This document does not change firmware, enable a profile,
qualify a timing channel, or authorize a flash.

## 1. Purpose, authority, and decisions

Deliver a useful persisted clock correction now, improve event localization,
and add filename-origin offset estimation without confusing assumed precision
with validated accuracy. An overnight run is not a prerequisite for provisional
operation. Qualified operation still requires independent evidence.

`implementation-spec.md` remains the authoritative contract. Update3 proposes
changes to update2's implementation and to the persistence policy introduced
by `implementation-update-spec.md`. Where this proposal conflicts with the
base contract, the relevant contract sections and acceptance matrix MUST be
amended explicitly during implementation; a newer document alone does not
silently override them. In particular, review the base contract's scheduling,
request limits, qualification, and metadata sections before enabling the new
profile. Preserve security, read-only card access, transfer gating, clean-room
separation, and resource limits.

### 1.1 Required outcomes

1. Implement actual bounded phase/period interval refinement, not only shifted
   polling deadlines and midpoint fitting.
2. Keep the mtime estimator; add separate filename-creation and filename-periodic
   candidates. Prefer a filename candidate only when eligible and applicable.
3. Support an explicitly assumption-based zero-cache profile, with independent
   visibility-delay settings. Never infer immediate visibility from zero cache.
4. Persist a **provisional SOT** when its half-width is at most 5 seconds and
   all provisional eligibility checks pass.
5. Retain a distinct **qualified SOT** at the existing 2-second half-width gate,
   with source-specific validation. Provisional records MUST NOT overwrite it.
6. Report source, quality, full interval, reference time, assumptions, persistence
   state, phase precision, and reasons for unavailable/rejected candidates.
7. Verify with deterministic host simulations, firmware build, and a separately
   authorized short live check. No waiting overnight to demonstrate mechanics.

### 1.2 Terminology

- Offset is `true_time - device_time`; apply it by adding it to device time.
- Interval width is `hi - lo`; midpoint half-width is half that width.
- Visible bracket is the previous unchanged request start through the changed
  response end, using actual monotonic times, never invented deadline times.
- Phase interval locates recurring listing visibility; it is not clock offset.
- Provisional SOT means best eligible persisted offset under explicit assumptions.
  It is not a certificate of physical accuracy or a validated timing channel.
- Qualified SOT means an offset with a source-specific validated model meeting
  the qualified width gate.
- Session identity includes card binding, day, filename session origin, and clock
  segment. Same day alone is insufficient to identify the same therapy session.

## 2. Revalidated current implementation and known gaps

The current tree contains WATCH/HUNT/NARROW1/NARROW2/VERNIER/REFINE/MAINTAIN
states, actual request timestamps, a rolling service median, and mtime interval
intersection. It has host tests and builds, but those facts do not establish
that the advertised inter-cycle refinement or physical accuracy is achieved.

### 2.1 Gaps this implementation must address

- `phase.c` stores transition midpoints and fits their deltas; shifted grids do
  not currently translate/intersect full intervals. REFINE therefore does not
  yet implement the 0.2–0.4 second common-phase target.
- A median of midpoint deltas is still not a bounded period fit. Apparent periods
  near 59 or 61 seconds can be polling-grid bias, not actual oscillator behavior.
- The existing plausible-delta range permits skipped-cycle gaps to contaminate
  the period estimate; cycle ambiguity must be resolved explicitly.
- Scheduler jobs carry priority, but FIFO execution does not honor that priority.
  Coverage can delay a refinement job despite its nominal deadline.
- Phase transition detection is coupled to the count of appended estimator
  events. When `DRIFT_EV_CAP` fills, the phase tracker can stop seeing real
  updates. Detection must remain functional independently of evidence capacity.
- Fetching eight catalog days and sorting them is not a general newest-day fix:
  collection truncates before sorting. Select the newest over all eligible days.
- Quiet foreign-day listings must not close the active session. Foreign activity
  also requires session classification; deletion alone is not proof of rollover.
- Same-day resume retains evidence today but must distinguish a real new session
  or beacon change from a continuity gap. Creation evidence and cycle origin must
  not leak into the next session.
- Current NVS SOT is a version-1 single record promoted only by `ACC_ACCURATE`.
  It has no provisional slot or source/model provenance.

`windows` counts contributing observations in the current clock segment.
`segments=1` is normally healthy: it indicates no detected discontinuity or
contradiction. More segments are not a refinement objective. A successful
refinement need not shrink the bound on every cycle: rate expansion, mapping
uncertainty, and real timing changes can widen it legitimately.

## 3. Accuracy model and assumptions

### 3.1 Interval equations

First map a monotonic visible bracket to UTC using valid anchor intervals.
Let `[Vlo,Vhi]` include that mapping uncertainty. Let additional cache staleness
be `[0,C]`, visibility delay be `[Llo,Lhi]`, and genuinely separate timestamp
measurement uncertainty be `E` per edge:

```
write_true = [Vlo - C - Lhi - E, Vhi - Llo + E]
mtime_device = [M, M + 2 s]
mtime_offset = [write_lo - (M + 2 s), write_hi - M]
```

Use closed enclosing intervals internally even when the encoding bin is
half-open. Round outward when converting units. A known fixed delay moves
the midpoint but does not widen the interval; delay range width does.

Late scheduling and request service duration are already represented by the
actual visible bracket. Do not add them again as generic scheduling jitter.
A separate uncertainty allowance requires a documented effect not represented
by those timestamps. Model write-to-visibility delivery delay asymmetrically;
it is not automatically a symmetric plus/minus jitter term.

### 3.2 Profiles

- **Conservative:** retain the current provisional cache/visibility bounds as a
  comparison model; these are not independently qualified merely by existing.
- **Zero-cache provisional:** `C=0` as requested; retain a separately specified
  nonzero visibility-delay interval unless immediate visibility is selected.
- **Immediate-visibility provisional:** `C=0`, `Llo=Lhi=0`, and no duplicated
  scheduling allowance. This is an explicit optimistic operating assumption.
- **Qualified:** only independently supported component bounds and source
  semantics; qualification applies to a model revision and hardware scope.

Changes of profile MUST change model identity, trigger recomputation from raw
observations where possible, and invalidate applicability of records made under
incompatible assumptions. Do not change `stamp_channel_validated` just to
promote an optimistic result.

### 3.3 Conditional numbers and limits

For an event interval of width `b`, one-second filename-origin bin, negligible
origin/cycle error, immediate visibility, and anchor uncertainty ±0.03 seconds:

| Event width b (before anchor allowance) | Mtime half-width | Filename half-width |
|---|---|---|
| 1.0 s | ±1.53 s | ±1.03 s |
| 0.6 s | ±1.33 s | ±0.83 s |
| 0.4 s | ±1.23 s | ±0.73 s |
| 0.2 s | ±1.13 s | ±0.63 s |

Additional origin/delay interval width adds half that width to these values.
These are arithmetic examples, not promised outcomes. With cache zero but
visibility delay `[0,2 s]`, add 1 second to each half-width. With a genuinely
separate ±0.3 second allowance, add another 0.3 second.

A 0.2–0.4 second common-phase interval remains a conditional target, not an
acceptance demand that justifies dropping uncertainty. A practical first target
is roughly 0.6–0.8 second direct brackets when service times permit. The 2-second
mtime bin need not average away under an exact-60-second device timer.

Lower-edge correction has error `[-width,0]`, under the complete applicable
model. Thus ±2 seconds gives +0/-4 seconds. The requested +0/-1 second guarantee
requires total offset width at most 1 second; this proposal does not establish
it from a whole-second filename and uncertain event timing.

## 4. Bounded phase/period tracker

### 4.1 Observation ownership and data

Keep bounded raw observations containing session/beacon identity, actual request
start, response end, transition classification, cycle index or ambiguity, and
profile revision. A successful listing is unchanged only with respect to the
same tracked beacon tuple. Creation, disappearance, final stop-time writes,
clock changes, and beacon swaps are not ordinary periodic flushes.

Tuple transition detection MUST remain independent of mtime parse success,
anchor availability, and offset-event append capacity. BRP size updates can
supply phase evidence even if mtime cannot supply offset evidence. Mark missing
or overflowed evidence explicitly; do not classify a changed beacon as unchanged
because the estimator appended zero events.

Use a bounded recent ring, initially 24 phase observations. Evidence eviction
must retain a correctly re-referenced accumulated offset summary or explicitly
expire old constraints. The existing 96-event array must not silently disable
an overnight estimator. No unbounded growth or allocation per cycle.

### 4.2 Interval fitting, not midpoint delta averaging

Model visible event k in the monotonic domain as:

```
visible_k = phase_origin + k * period + cycle_deviation_k
cycle_deviation_k in [-Jk, +Jk]
period in [Plo, Phi]
```

`Jk` describes cycle variation beyond the period/rate model, not request latency
already in the observed bracket. For relative bounded interval `[Blo,Bhi]`,
each known k contributes constraints:

```
Blo - Jk <= phase_origin + k * period <= Bhi + Jk
```

Initialize P around nominal 60 seconds with an explicit model rate allowance;
500 ppm corresponds to ±30 ms over 60 seconds. This nominal assumption does not
prove timer stability. Large real deviations trigger reacquisition or a wider
model, not a fabricated tight bound. Keep device-offset rate and visible-cycle
variation distinct even if their initial numeric allowances happen to match.

Recommended implementation: a bounded two-parameter feasible-region solver
for phase and period using half-plane clipping, or an equivalent tested interval
solver. Retain phase/period correlation for next-cycle prediction. If using only
interval hulls, predict conservatively; never intersect independently narrowed
hulls as if they retained the original joint constraints. Use relative times,
checked arithmetic, and outward numerical rounding. Cap solver vertices and
constraints; on capacity/numeric failure degrade to HUNT with an explicit reason.

Project the feasible region at cycle k to obtain `[pred_lo,pred_hi]`.
Translate observations only with their supported period and cycle bounds.
Shifted polling grids reduce common-phase uncertainty only through these actual
constraints. Do not treat a grid shift itself as an accuracy gain.

### 4.3 Cycle assignment and discontinuities

- Determine the admissible integer cycle increments from elapsed intervals and
  period bounds. Accept a cycle count only when unique.
- A missed observation with unique two-cycle increment updates constraints as
  two cycles; it must not produce a measured 120-second fundamental period.
- Ambiguous cycle counts suspend filename-periodic eligibility. Reacquire;
  bounded hypothesis handling is optional, but arbitrary rounding is forbidden.
- Empty feasible intersections trigger a documented widened model or a fresh
  phase segment; contradictory constraints must not be silently discarded.
- A phase miss is not evidence of therapy OUT or an AirSense clock step.
- A true device-clock step invalidates affected offset applicability separately
  from phase reacquisition. An NTP source-epoch step invalidates mapping continuity,
  not the monotonic event times themselves.

### 4.4 State behavior

- WATCH: retain 5-second tracked-folder pulls outside therapy, plus discovery.
- HUNT: same cadence until a usable periodic event bracket is obtained.
- NARROW1: nominal 2-second grid, covering the whole predicted interval.
- NARROW2: nominal 0.8-second grid; do not advance only because a counter moved.
- VERNIER: translated-interval constraints from shifted grids; advance on valid
  evidence and supported model width, not a presumed 0.4-second result.
- REFINE: enable bounded further cycles with 0.2-second grid shifts and optionally
  faster service-aware starts. Maximum two cycles per acquisition attempt.
- MAINTAIN: normally three starts around the prediction at about 1-second spacing;
  widen the set when needed, and re-refine when the projected width degrades.

Target common-phase width 0.4 seconds, optionally 0.2 seconds. If allowances
prevent it, report the actual width and enter degraded maintenance rather than
looping indefinitely. Set an acquisition ceiling of six cycles and ten starts
per cycle; these are proposed bounded attempts subject to the global ledger
and explicit base-contract load amendments. After repeated misses fall to HUNT
with backoff. Record why a target was not attained.

## 5. Scheduling, coverage, and lifecycle

Use deadline-aware execution that actually honors priority and release time.
Jobs should carry `earliest_start`, optional deadline, minimum spacing, purpose,
and session/target token. Completion uses actual times. An HTTP request cannot
be preempted; defer optional work whose conservative service estimate would
cross the next phase deadline. Never execute two card requests concurrently.

Choose refinement start spacing as at least the nominal grid and twice the
recent service median, with slow-request feedback and a bounded recovery path.
A 400–500 ms refinement grid is allowed only when this guard permits it. Do not
promise 200 ms sequential polling on a card taking about 236 ms per listing.
Queue congestion and delay are observable, not hidden inside offset assumptions.

### 5.1 Coverage and accounting

Deduplicate queued day/parent jobs and reject expired session tokens. Count all
actual HTTP attempts, including failures, optional reads, streaming requests,
and retries. A dropped job must notify its owner so an armed window cannot
wait forever. Jobs dropped before HTTP starts are not card attempts.

Retain independent parent/hot-folder coverage and fail-closed OUT/transfer
checks. Defer coverage around refinement windows only within its allowed
freshness deadline. If both deadlines cannot be met, lose phase precision rather
than silently lose monitoring safety. Required coverage outranks optional
refinement at its hard deadline. Successful polling of one day cannot prove
neighbors fresh.

The current tree has an 8000/8-hour universal ceiling; do not raise it again in
this work. Review its mismatch with the base contract's 700 active/uncertain
limit explicitly. Calculate WATCH, coverage, acquisition, retries, and cold
crawl load together: 12 WATCH requests/minute plus optional work can exhaust
8000. Optional crawl must not consume the reserve required for monitoring.
Preserve checkpoint freshness and conservative reboot accounting; test bucket
expiry after idle gaps, repeated reboots, and failed commits. Qualification of
card read tolerance remains separate from mathematical estimator correctness.

### 5.2 Session classification

Use a full-scope newest-day query or sort before truncation; neither cap=1 nor
cap=8 is a general solution. Preserve active folders separately from newest-day
fallback, and identify actual updating beacons, not merely historical BRP files.

Quiet neighbor scans do not close a session. A foreign-day file creation or
periodic write may establish rollover; disappearance alone does not. Multiple
simultaneously active folders require explicit scheduling/ambiguity handling,
not repeated session switching on every listing.

Resume historical offset evidence through a gap only for the same card/session
and compatible clock segment. Discard precise cycle extrapolation after an
unbounded gap. A new same-day filename origin resets filename and phase evidence;
older offsets may remain historical records, not this new session's constraints.
Track finalization writes separately so they cannot pull the periodic fit.

## 6. Filename-origin offset candidates

Keep mtime available throughout. Filename offset is an additional estimator,
not a textual substitution into the existing mtime equation.

### 6.1 Parsing, encoding, and clock domain

Strictly validate `YYYYMMDD_HHMMSS`, calendar dates, leap years, time fields,
and expected file class. The current display parser is insufficient for numeric
estimation. Decode into a device-clock origin interval with provenance:

- Truncated seconds: `[F,F+1 s]`.
- Rounded seconds: `[F-0.5 s,F+0.5 s]`.
- Unknown between these conventions: enclose their union as
  `[F-0.5 s,F+1 s]`, width 1.5 seconds. Other unsupported encodings make the
  candidate unavailable rather than guessing either convention.

The filename is local device time in the observed AS11 combination; mtime is
UTC-shaped. Require an explicit device timezone rule or offset valid at the
session instant, not the host's current timezone. Handle historical DST,
nonexistent local times, and repeated local times. Reject unresolved ambiguity
or expose separate hypotheses; never choose a DST branch because it yields a
tighter correction. No new timezone dependency without reviewing existing
settings/libraries and resource costs.

Encoding, timezone, and origin event semantics are separate qualifications.
Evidence from one AS11/card combination does not qualify AS10 behavior.

### 6.2 Filename-creation candidate

Let the true visible appearance bracket be `[Vlo,Vhi]` and the filename origin
be `[Flo,Fhi]` in the device's UTC-shaped clock domain. Let physical creation
occur `D in [Dlo,Dhi]` after the event represented by the filename, and let
visibility delay be `[Llo,Lhi]`:

```
true_origin = [Vlo - C - Lhi - Dhi - E, Vhi - Llo - Dlo + E]
creation_offset = [true_origin_lo - Fhi, true_origin_hi - Flo]
```

Keep D distinct from visibility latency. The observed roughly six-second delay
from therapy entry to sampled-file appearance is not a universal D value or
proof that filename time means therapy-entry time. Creation brackets must belong
to the particular file/event used in the model. Do not borrow a CSL appearance
moment for BRP without a bounded cross-file relation.

On first new annotation/session activity, allow a bounded rapid creation-watch
window, e.g. ten seconds at service-aware 0.5-second spacing, at most twenty
starts and charged to the acquisition allowance. Stop on the matching sampled
file appearance or the deadline. First-ever resident baseline is not a bounded
creation event. This improves later file discovery, not the already-missed first
annotation appearance; default WATCH remains 5 seconds.

If D is unknown, publish `origin_unresolved`; do not emit a narrow offset.
An explicitly configured assumed D can make a provisional candidate eligible,
provided its full interval and assumption flag are exposed. Do not install a
zero-D default solely to make this candidate win.

### 6.3 Filename-periodic candidate

For uniquely identified cycle k, model the device flush instant as:

```
device_flush_k = filename_origin + k * device_period + origin_adjustment_k
```

Carry interval bounds for device period and origin adjustment, including any
accumulating rate variation. Estimate the true flush interval from the phase
model, including visibility delay and supported cycle expansion:

```
periodic_offset_k = [true_flush_lo - device_flush_hi,
                     true_flush_hi - device_flush_lo]
```

Device-period semantics and observed monotonic visibility period are not
interchangeable. The phase fit alone does not establish the filename-origin
adjustment or which observation was flush 1. Resolve the cycle via bounded
creation/first-flush evidence or a separately supported record-to-flush model.
Mid-session joins can remain unavailable for this source while mtime still works.
Do not add routine in-therapy header/file reads to resolve ambiguity.

Unknown origin adjustment means unavailable. A configured assumption permits
provisional use, not qualified use. Do not calibrate the adjustment from mtime,
remove mtime's uncertainty, then claim the derived filename estimate provides
independent tighter information. A shared calibration must retain its full
uncertainty and dependence. Pause/resume, missed cycles, session changes, and
clock steps revoke cycle eligibility until uniquely resolved again.

### 6.4 Candidate comparison

Re-reference all candidates to the same instant and clock segment before
comparison. Publish each eligible candidate and unavailable reason. Prefer
qualified over provisional, then the smaller applicable full interval with
source-selection hysteresis. Creation-only evidence must widen with age/rate.
Do not select a stale narrow interval over current evidence.

Disagreement creates an explicit flag and suspends filename promotion pending
model resolution. Retain a separately applicable mtime result if its own checks
pass; do not hide disagreement by taking a midpoint or selecting whichever
source is closer to an expected answer. Intersection is allowed only with
compatible event models, shared uncertainty preserved, and no double-counting
of the same observation as independent evidence.

## 7. Persisted SOT: provisional and qualified

### 7.1 Separate records and quality

Maintain separate provisional and qualified records. A selected best-available
view is derived from applicability and quality; it is not a third authoritative
blob. Never overwrite qualified history with provisional data. Keep estimator
state (`pending/provisional/validated/invalid`), legacy accuracy tier, and
persistence quality distinct.

| Quality | Half-width ceiling | Required qualification |
|---|---|---|
| Provisional | 5000 ms | Complete bounded model, assumptions explicitly recorded |
| Qualified | 2000 ms | Independently validated source-specific model and hardware scope |

These ceilings are policy thresholds, not proofs of actual error. An unresolved
origin is not a bounded model just because zero was substituted for it.
Phase-only localization is never eligible as an offset SOT.

### 7.2 Provisional eligibility

All conditions MUST hold before promotion:

1. Accepted anchors map the contributing evidence; source quality and mapping
   epoch are known. No fabricated UTC or pending anchor is used.
2. At least three distinct supporting offset observations in the current
   compatible clock segment. Re-evaluations, multiple files representing the
   same physical flush, and repeated use of one creation origin are not three
   independent observations. Shared constraints remain shared.
3. Offset bounds are finite, ordered, applicable, and half-width is at most
   5000 ms using outward-rounded bounds.
4. Source/event relation and cycle count are resolved or explicitly bounded by
   a declared provisional model. No unresolved timezone/origin/cycle branch.
5. No unresolved source disagreement, clock step, evidence overflow affecting
   completeness, or incompatible card/model identity.
6. Monitoring is current enough to associate the estimate with its session.
   An HTTP failure alone does not erase historical SOT, but may suspend new
   promotion. Final eligible evidence can commit on confirmed session close.
7. Full provenance and assumptions fit the bounded record; omission of critical
   provenance is a promotion failure, not permission to truncate it.

A creation-only filename estimate can be displayed immediately but cannot meet
three-observation provisional persistence by duplicating its single bracket.
It can qualify separately through an independently validated creation model;
for this revision qualified SOT still requires at least two distinct applicable
supporting observations, consistent with existing policy. Mtime offers the
immediate three-flush route if filename-periodic eligibility is unavailable.

### 7.3 Qualified eligibility

Retain the valid-anchor and at-least-two-observation checks and the 2000 ms
half-width ceiling. Add source-specific qualification rather than inheriting
`stamp_channel_validated` for all sources. Qualification covers event semantics,
encoding, timezone, visibility bounds, origin relation where applicable, rate,
cycle handling, and supported hardware/profile revision.

Self-consistent intervals and a sufficient observation count alone cannot
qualify a model. Independent reference comparison must establish coverage,
including systematic bias. AS11 measurements do not silently qualify AS10.
No default qualified flag is enabled by this implementation plan.

### 7.4 Replacement, ageing, and invalidation

- Within a quality slot, compare records at a common reference with rate
  expansion. Replace on materially better applicable bounds or a new supported
  clock segment; not merely on newer `computed_at`.
- Proposed improvement threshold: at least 250 ms reduction in half-width.
  Refresh an ageing record when new evidence improves its present-time bound,
  even if its reference-time width did not shrink by 250 ms.
- If a clock step invalidates an old qualified record, preserve it as history
  but exclude it from best-available selection. An applicable provisional record
  may then be selected; it still does not overwrite the qualified history.
- Apply rate expansion `half_at_t >= half_at_ref + rate_ppm * abs(t-ref)` in
  consistent units. For 500 ppm, eight hours adds 14.4 seconds. A stored narrow
  interval is not an equally narrow all-night correction without further evidence.
- After reboot, UTC freshness is unknown until trusted time is restored. Mark
  the record `freshness_unknown`, not freshly qualified or currently applicable.
  A missing live anchor must not erase a CRC-valid historical record.
- Settings/card/model changes and detected clock discontinuities invalidate
  affected applicability. Card firmware alone cannot distinguish identical cards;
  retain that limit and do not promise reliable identical-card swap detection.
- Qualification upgrades use a new explicitly supported model revision, never
  a metadata flag flip on an old assumption-based record.

### 7.5 NVS schema and write policy

Preserve the legacy `drift_sot` key during migration. Introduce independent
versioned bounded blobs for provisional and qualified records, for example
`drift_prov_v2` and `drift_qual_v2` (within NVS key limits). Each contains:

- Magic, schema version, byte length, per-slot committed sequence, and CRC.
- Offset midpoint, outward-rounded lo/hi, and derived half-width.
- Computed UTC, reference UTC, mapping epoch/provenance, and rate allowance.
- Source, quality, model revision/fingerprint, hardware/card binding available.
- Assumption bitset and parameters actually used, including cache/visibility,
  filename encoding, timezone reference, and origin-adjustment bounds.
- Session/clock-segment association and contributing-observation count.

Use explicit serialization or a defined fixed-width layout, not pointers or
ABI-dependent in-memory structs. Validate bounds, enums, lengths, arithmetic,
strings, version, and CRC before accepting a blob. CRC is corruption detection,
not authentication; retain encrypted metadata NVS.

First eligible provisional/qualified promotion requests an immediate commit.
Subsequent writes are at most once/minute per slot, normally on meaningful
improvement, refresh, quality/model change, invalidation, or session close.
Coalesce repeated forced triggers; closing cannot defeat wear limits indefinitely.
RAM promotion and successful NVS commit are distinct states. Advance sequence
only after successful commit; on failure keep dirty state, report the error,
and retry with bounded backoff. Never claim a value persisted if commit failed.
Use NVS's supported atomic key update semantics; no partition erase or bulk reset.

Migration: accept a valid legacy v1 record as `legacy_qualified` history with
unknown estimator-source/model provenance, preserving its original values and
sequence. Do not infer missing qualification scope or select it as currently
applicable until binding/time checks succeed. Prefer valid v2 slots; corrupt v2
must not silently resurrect an incompatible legacy value. Leave legacy data
intact until a deliberate migration/deprecation policy permits removal.

## 8. API, diagnostics, and consumer compatibility

### 8.1 GET_STATUS phase block

Add actual last-bracket width, common-phase width, period bounds, prediction
width, cycle-index state, achieved/target width, deadline slip, service guard,
miss count, reacquisition reason, and model/assumption identity. Mark fields
unavailable with null rather than zero precision. Snapshot under a consistent
publication mechanism; readers must not mix fields from two tracker updates.

### 8.2 GET_DRIFT envelope

Preserve existing estimate fields and their units. Add source/model/assumption
metadata and candidates, together with these explicit views:

- `provisional_source_of_truth`: independent provisional slot or null.
- `qualified_source_of_truth`: independent qualified slot or null.
- `best_available_offset`: selected applicable record or null, plus selection
  reason and whether provisional consumption requires client opt-in.
- Each record: `quality`, `source`, `model_id`, `assumptions`, full interval,
  reference/computed times, reference and current half-widths, applicability,
  stale/freshness state, persisted sequence, dirty state, and last commit error.
- Candidates: `mtime`, `filename_creation`, `filename_periodic`, each with an
  interval or unavailable reason. Distinguish pending from rejected evidence.

Keep legacy `source_of_truth` qualified-only. Do not put provisional data into
that field and hope old clients inspect an unfamiliar quality field. Introduce
an advertised capability/schema revision for consumers choosing best-available
provisional offsets. Old clients retain existing behavior; opting into the
expanded persistence policy requires an explicit consumer/API contract update.
This means persistence can work now without claiming old SomnoTrace automatically
uses the new provisional slot. No SomnoTrace source edits are part of this spec.

Do not rename existing `accuracy_tier=accurate` to mean provisional ≤5 seconds.
Expose `quality=provisional` even when numerical half-width is under 2 seconds.
A quality badge describes model qualification, not only numeric precision.

Bound response sizes, use checked serializers and valid escaping, and fail
explicitly on truncation. Do not ship partially valid JSON. Ensure examples,
GET_STATUS buffers, GET_DRIFT buffers, and BLE fragmentation handle the added
fields. Format 64-bit fields according to existing protocol conventions.

### 8.3 Journal and host tool

Add bounded records for phase refinement, cycle ambiguity, source selection,
assumption changes, disagreement, promotion rejection, RAM promotion, commit
success/failure, restore/migration, and invalidation. Store timing metadata only:
no waveforms, patient header fields, secrets, or real-data fixtures.

Correct existing bare-word journal fields such as `"round":quiet` to valid JSON
strings while touching those records. Test journal payload validity and bounded
lengths. Update `ble_query.py --status --drift --json` to show source, quality,
reference/current widths, assumptions, and persistence status clearly. A restored
record must visibly say it came from NVS and whether current applicability is
known. Never print provisional SOT as certified/high accuracy.

## 9. Implementation touchpoints and sequence

Prefer edits to existing modules; add a small pure-logic helper only if the
interval solver or record encoding warrants it. New sources require the project
license header. Do not add a dependency or change security policy to make builds
pass. Do not copy SomnoTrace code or modify its repository for this work.

### Step 1 — Contract and model types

- Reconcile proposed changes with authoritative `implementation-spec.md`,
  especially §§6, 8, 10 and the acceptance matrix; preserve unchanged controls.
- Define candidate source, quality, assumptions, model identity, full interval,
  cycle state, applicability, and persistence state in drift interfaces.
- Define separate source qualifications and 5000/2000 ms promotion thresholds.
- Add profile settings with explicit zero-cache and immediate-visibility semantics.
  Keep conservative comparison available; do not silently select optimistic
  defaults. Select the requested profile through an explicit reviewed setting.

### Step 2 — Raw observations and interval phase fit

- `components/scheduler/include/phase.h`, `phase.c`: replace midpoint-only
  inference with bounded phase/period constraints and cycle assignment.
- Decouple periodic tuple detection from `drift_feed_scan` append counts.
- Make bounded evidence retention work past 96 updates without losing detection.
- Add solver tests before integrating; remove obsolete midpoint fit only after
  matching behavior/failure tests exist. Do not retain unused APIs as fake models.

### Step 3 — Deadline scheduler and lifecycle

- `components/scheduler/include/sched.h`, `sched.c`: effective priority/deadline
  scheduling, duplicate suppression, stale-token cancellation, budget accounting,
  service guards, and coverage deadline protection.
- `components/catalog/catalog.c` and interfaces: full-scope newest/hot selection
  before truncation, with bounded host-testable cases.
- Classify quiet neighbor, rollover, genuine same-day new session, finalization,
  and monitoring gaps separately. Preserve offset evidence only where compatible.
- Check all live card HTTP paths, including BLE listings/downloads, obey one
  worker and universal accounting; direct calls outside it must be integrated.

### Step 4 — Offset candidates and selection

- `components/drift/drift.c`, `include/drift.h`: strict numeric filename parser,
  timezone treatment, creation and periodic origin models, rate re-referencing,
  and source disagreement/selection.
- Keep numerical mtime path as fallback and conservative comparison.
- Add actual common-phase evidence to offset estimation only with its cycle and
  model allowances; never substitute a scheduling prediction for observed proof.

### Step 5 — Persistence

- Refactor promotion away from the hardcoded live `ACC_ACCURATE` path into a
  tested eligibility/selection function with separate quality slots.
- Add versioned encoding, legacy migration, commit throttling, dirty/error status,
  restored applicability, and invalidation persistence.
- `settings` integration: use existing encrypted metadata handles and applicable
  identity/reset events; no partition-layout change unless separately justified.

### Step 6 — API and verification

- `components/ble_api/ble_ops.c`, drift serializers, and `scripts/ble_query.py`:
  additive candidates/quality/persistence views and compatibility handling.
- `scripts/host_test.c`, `run_host_tests.sh`, component CMake only as needed:
  integrate new pure logic and stub-NVS tests.
- Correct documentation of achieved behavior in update2 when implementation is
  verified; do not leave its "fully implemented" claims contradicting missing
  refinement. Keep update3 status proposed until its criteria are actually met.

## 10. Verification matrix

All fixtures must be synthetic. Tests must assert interval coverage and failure
behavior, not merely a state name or an attractive midpoint.

### 10.1 Phase and scheduling

- Exact 60-second events over varied fractional phases and 20–400 ms service
  durations; both request-start and response-end sampling possibilities.
- A ladder reaches maintenance while retaining actual interval constraints;
  translated shifted grids tighten a stable model when their allowances permit.
- Variable service, FIFO contention, delayed execution, and slow outliers widen
  evidence or degrade state rather than falsely improve precision.
- Constant added visibility delay: phase follows the delayed events while the
  zero-delay offset model is demonstrably biased; it must remain provisional.
- Cycle variation exceeds a target: report attainable width; no fabricated
  0.2-second result. Empirical residual maxima are not a validated hard bound.
- Missed one/two cycles with unique indexing; ambiguous cycle branches; clock
  steps; time-source epoch changes; beacon swaps; stop/resume; day rollover.
- Quiet neighbor scans cannot reset active phase or offset evidence.
- Final stop writes cannot fit a new fundamental period.
- More than 96 flushes and a full simulated eight-hour run retain detection and
  bounded memory. Test newest-day selection with more than eight table entries.
- Budget exhaustion, failed checkpoints, deduplication, dropped jobs, and paused
  planners do not lead to false OUT, dangling windows, or uncounted HTTP attempts.
- Required coverage meets its deadlines or fails closed despite refinement load.

### 10.2 Filename and offset arithmetic

- Valid/invalid dates, leap years, suffix classes, truncated/rounded/unknown
  one-second encodings, timezone offsets and DST ambiguity.
- Creation offset with nonzero D; mismatched annotation/sample appearance times;
  first-listing baseline; mid-session join with no origin evidence.
- Periodic filename candidate with known/unknown origin adjustment, correctly
  identified first cycle, missing cycles, and pause/resume ambiguity.
- Shared mtime calibration cannot manufacture tighter independent filename data.
- Constant origin bias and visibility bias remain possible in optimistic mode:
  assumptions persist in API/NVS even with mutually consistent observations.
- Width/half-width and outward rounding, negative offsets, one-sided correction,
  re-referencing and 500 ppm ageing. No multiplication/serialization overflow.
- Disagreement suspends affected candidate promotion without discarding an
  independently applicable fallback. Same-day new session does not inherit
  another session's origin or contributing-observation count.

### 10.3 SOT and NVS

- Three distinct eligible observations with half-width ≤5000 ms promote a
  provisional record, commit it, and restore the same interval/provenance.
- Two observations do not satisfy provisional eligibility; three re-evaluations
  of one observation do not count. Exact threshold boundaries are tested.
- Provisional results never replace qualified history or acquire `accurate`
  status simply because half-width is ≤2000 ms.
- Qualified promotion requires its separate source qualification and threshold.
- Narrow but unresolved filename origin cannot win eligibility/selection.
- Better-than-aged-record refresh works; merely newer/wider results do not
  displace better applicable records. Clock-step invalidation revokes selection.
- Missing post-reboot anchors yield freshness unknown, not fabricated current
  applicability. Stored records remain historical without being erased.
- NVS set/commit errors leave dirty state and unchanged committed sequence;
  successful bounded retry commits. Repeated close/force calls respect throttling.
- Absent/corrupt/truncated/unknown-version blobs, invalid enums/bounds/CRC,
  legacy v1 migration, mixed valid/invalid slots, and interrupted updates fail
  safely without partition erase or accidental incompatible legacy resurrection.
- Old API clients receive qualified-only legacy SOT; new clients can explicitly
  opt into the provisional best-available view. All payloads parse as valid JSON.

### 10.4 Commands and short live acceptance

Run from `/opt/shared/somnobridge`:

```
./scripts/run_host_tests.sh
./scripts/idf.sh build
python3 scripts/validate_timing.py self-test
```

Check Python changes with the repository's available syntax/test workflow and
review the full diff. Documentation-only work does not require a firmware build.

A later live verification requires separate flash authorization. Stop all serial
readers before flashing. Use the GEEK's documented physical reset/power-cycle
procedure if the app remains in download mode after RTS reset. Do not manipulate
therapy or device settings without separate authorization.

During a naturally occurring or user-started session:

1. Establish an accepted BLE time anchor.
2. Check WATCH/creation discovery, then actual phase constraints through
   narrowing and maintenance; log achieved widths, cycle state, and misses.
3. Confirm neighboring coverage does not reset evidence and windows accumulate.
4. After three eligible offset observations, confirm provisional RAM promotion,
   successful NVS commit, sequence, assumptions, and quality in GET_DRIFT.
5. If filename eligibility is unresolved, expect an explicit unavailable reason
   and mtime fallback, not a fabricated narrower filename correction.
6. With permission to reboot, confirm NVS restoration and freshness-unknown state
   before synchronization, then applicable selection after binding/time checks.
7. Compare against an independent reference if available. A short mechanical
   demonstration is not source qualification or proof of SD write noninterference.

## 11. Completion criteria and deliberate exclusions

The implementation is complete when bounded phase refinement genuinely operates,
filename candidates are implemented with explicit ambiguity handling, mtime
fallback remains usable, eligible provisional offsets persist/restore with honest
quality, qualified history is protected, consumer compatibility is documented,
and the verification matrix passes without resource/security regressions.

Not promised by completion:

- A universal 0.2–0.4 second phase result or monotonic improvement every cycle.
- Qualified/high-accuracy status from assumptions or interval self-consistency.
- A +0/-1 second absolute correction guarantee.
- A filename estimate when origin/cycle/timezone information is unresolved.
- Automatic use of provisional SOT by an unchanged SomnoTrace client.
- Independently measured visibility latency from dense polling alone.
- Transfer safety exemption, unlimited polling, routine in-therapy file reads,
  copied SomnoTrace implementation, or unrequested flashing/commits.

Recommended delivery: implement the missing interval model and scheduling first,
then explicit provisional profiles, filename candidates, and two-quality SOT.
Use provisional mtime persistence as the immediate dependable software path;
allow filename to improve the selected estimate only when its model is complete.
Retain independent qualification as a separate upgrade, not a blocker to seeing
provisional persistence work now and not a label to bypass for convenience.
