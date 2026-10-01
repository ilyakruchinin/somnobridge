# SomnoPal — implementation update 2: phase-tracking poll schedule

Status: proposed requirements, **revision 2**. Documentation only; this
revision does not enable a profile, change firmware, or authorize flashing.
Normative delta over `implementation-update-spec.md` (revision 2): this
file supersedes poll scheduling and the accuracy interpretations addressed
below. Anchor discipline, authenticated API, SOT persistence, transfer
safety, and evidence-journal requirements remain in force.

Basis: the maintainer's progressive-refinement proposal, the observations
in `docs/validation-results.md`, and the reported live runs on 2026-10-01.
Proposed timing guarantees are distinguished from measured facts.

Goal: replace redundant day scans with one phase-tracking schedule. Target
an NTP-referenced **visible-update phase interval 0.2–0.4 s wide**, when
service time and measured cycle stability permit. The separate clock-offset
goal remains the `accurate` gate of **±2 s**, with a conditional finer
filename-based path described in §6. Neither target is guaranteed after a
fixed number of minutes.

## 1. Three observables: what each establishes

| Observable | Information | Resolution / limitation |
|---|---|---|
| Filename `YYYYMMDD_HHMMSS` | AirSense local-clock session/start claim | Whole seconds; rounding versus truncation must be characterized |
| File mtime | AirSense-clock reading associated with a file update | Modelled as truncated FAT 2 s interval `[M, M+2)` |
| Tuple transition bracket on SomnoPal's anchored monotonic clock | When an update became visible through the listing interface | Limited by poll timing, service time, visibility/cache delay, anchor uncertainty |

The third observable is the best way to locate the **update moment in true
time**. Its resolution is not restricted to 2 s by FAT. BRP size changes
can establish transitions even without using the numerical mtime; PLD/SA2
may require stamp changes because listed size can remain unchanged (F4).

However, a precisely measured true-time event is not, by itself, an
AirSense clock-offset measurement. Even an exactly 60 s periodic sequence
can have any phase relative to the AirSense clock. A session-relative
60 s timer is not necessarily a device-clock minute-boundary timer.

Two distinct outputs are therefore required:

1. **Visible-update phase:** a true-time interval for the recurring event,
   usable for polling and tracking cadence without numerical timestamps.
2. **Clock offset:** true time minus AirSense time at a corresponding
   event, requiring mtime or a validated filename-to-flush relationship.

Do not label phase precision as clock-offset accuracy.

## 2. Terminology and timing arithmetic

- **Pull:** one complete day-folder listing (`A:\DATALOG\<day>`).
  This is the known stamp-bearing endpoint; no per-file stat endpoint has
  been established. Filter locally after complete parsing, not by accepting
  a partial listing.
- **Visible bracket `[B_lo, B_hi]`:** previous unchanged request start to
  changed response end, conservatively covering when the update became
  visible. Preserve actual request/response times, not nominal deadlines.
- **Write-to-visible delay `L`:** independently characterized delay from
  the relevant device write/stamp event to visibility. It may include cache
  delay. Count cache separately only if it represents an additional effect.
- **Mark:** predicted center of the next visible update.
- **Period `P`:** nominal 60 s recurrence, fitted/checked in monotonic time.
  AirSense seconds and SomnoPal seconds may differ slightly in rate.
- **Prediction uncertainty `iunc`:** bounded cycle-to-cycle phase error,
  period uncertainty, oscillator-rate uncertainty, and visibility variation.
  No automatic 2 s floor applies when predicting from observed moments
  rather than quantized stamp deltas.
- **Vernier refinement:** shift the polling grid between cycles, translate
  brackets to a common phase origin, then intersect after expanding for
  cycle variation and mapping uncertainty.

For a mtime-based offset, first map the bracket to UTC with anchor bounds:

```
write interval  = [B_lo - L_hi, B_hi - L_lo]
device interval = [M, M + 2 s)
offset interval = [B_lo - L_hi - (M + 2 s), B_hi - L_lo - M]
width           = (B_hi - B_lo) + (L_hi - L_lo) + 2 s
```

If extra cache uncertainty `C` and per-edge allowance `J` are genuinely
separate, add `C + 2J`. Include anchor uncertainty at both interval edges.
Latency contributes its **range width**, not `L_hi + L_lo`. A fixed known
latency changes the midpoint but not the width.

## 3. Unified schedule

The schedule replaces day polling, rather than adding another scan stream.
Each listing serves catalog, therapy monitoring, and timing observation.

```
WATCH -> HUNT -> NARROW -> VERNIER -> [optional REFINE] -> MAINTAIN
            ^             |                |                |
            +-------------+----------------+----------------+
                          miss / phase inconsistency
```

### 3.1 WATCH: outside therapy

- Discover the current day through `A:\DATALOG` initially; watch its full
  listing every 5 s. Do not re-list every historical day at that cadence.
- Check the parent periodically (proposed 5 min), and promptly near a
  possible day rollover or if the tracked folder becomes unavailable.
  A 5 min discovery interval alone cannot guarantee 5 s detection of a
  session starting in a previously undiscovered folder.
- Keep necessary neighbor discovery and bounded cold crawling, but avoid
  duplicate jobs. UNKNOWN is not equivalent to confirmed OUT for budgets
  or transfer permission.
- First membership/tuple changes establish activity, select the active
  sampled beacon, and start phase acquisition. A first-ever listing is a
  baseline, not proof of a write.
- Twelve requests/minute at the measured 236 ms service median occupy
  about 4.7% of wall time. This is an HTTP service-duty estimate, not a
  measurement of SD/MUX occupancy or proof that concurrent reads are safe.

### 3.2 HUNT: acquire the phase

- Continue 5 s pulls until a beacon transition is bracketed.
- Use its bracket as a phase interval. Use nominal 60 s recurrence for the
  next prediction, conservatively widened for unvalidated cycle behavior.
- Fit period from multiple translated moment brackets. Do not use raw
  response-end deltas as exact write intervals: discovery delay changes
  between cycles, especially when the polling spacing changes.

### 3.3 NARROW and VERNIER

The maintainer's grids are reasonable starting points under sufficiently
stable recurrence:

| Stage | Nominal starts | Span between first and last start | Ideal phase result |
|---|---|---|---|
| HUNT | Every 5 s | Continuous acquisition | 5 s bracket |
| NARROW-1 | 4 pulls, 2 s apart | 6 s | 2 s bracket |
| NARROW-2 | 4 pulls, 0.8 s apart | 2.4 s | 0.8 s bracket |
| VERNIER | 3 pulls, 0.8 s apart, grid shifted 0.4 s | 1.6 s | 0.4 s common-phase interval |
| Optional REFINE | Additional cycles with 0.2 s grid shifts | Adaptive | 0.2 s common-phase interval |

These are nominal values, not guaranteed physical-write bounds. In
particular, 0.8 s start spacing with 236 ms service can produce a
conservative request-start/response-end bracket around 1.04 s wide.

Requirements:

1. Cover the entire predicted phase interval, including `iunc`, service
   allowance, and the requested margins. Increase the pull count or widen
   the grid if four/three pulls cannot cover it; do not assume the table's
   spans always suffice. A 2 s bracket expanded by 0.3 s at both ends is
   2.6 s, not 2.4 s.
2. Keep all deadlines monotonic; UTC anchors map evidence to true time.
3. Stop after the first observed transition. If the first pull already
   shows it, use the previous real unchanged scan to form the wider
   bracket; do not invent an unchanged boundary at the window opening.
4. Translate consecutive brackets by the period before intersecting.
   Expand each for cycle jitter, visibility variation, and rate bounds.
   Shifted grids do not create a 0.4/0.2 s guarantee if these effects are
   larger. Contradictions widen/reacquire rather than being discarded.
5. Serialize requests and enforce the service-time guard. The measured
   218–243 ms response times do not support sustained sequential pulls
   every 0.2 s. A 0.2 s inter-cycle grid shift is different and may be
   practical without 0.2 s request spacing.
6. A miss returns to 5 s HUNT, refits the phase, and retries. A missed
   predicted window alone does not prove therapy ended.

Optional refinement is **restored as a conditional option**, not rejected
categorically. It is useful for phase localization and possibly for the
filename-based offset path (§6.3), provided its gain exceeds measured
service/visibility/cycle uncertainty.

### 3.4 MAINTAIN

- Proposed three pulls at `mark - 1 s`, `mark`, `mark + 1 s`; center the
  window symmetrically. The transition need not always occur on pull 2.
  Accept a valid old/new bracket on either side of the mark.
- Three 1 s-spaced pulls give roughly a 1 s single-cycle bracket plus
  service time. They do not independently re-establish 0.2 s precision.
- Retain a finer phase model only while period/rate/jitter bounds support
  it. Refit from maintenance evidence and periodically refine if necessary.
- Any unbracketed transition or inconsistency returns to HUNT. Failures
  back off; transfer gating remains fail-closed.

### 3.5 Session and monitoring lifecycle

- End/reset phase tracking on confirmed therapy OUT, beacon/session change,
  or an actual change of tracked day. A quiet neighbor listing must not
  terminate the active day's estimator session.
- Choose the active session from observed membership/activity, using
  filenames for ordering only when their clock domain is consistent.
  An old BRP must not outrank the current session's updating PLD/SA2.
- Do not combine session-relative phase evidence across a beacon swap.
- Preserve historical offset evidence through a transient monitoring gap,
  but stop precise extrapolation until continuity is re-established.
- Successful monitoring of one folder cannot prove all required folders
  fresh. Preserve complete-round/coverage checks for OUT and transfers.

## 4. Retained and changed behavior

- Replace the blind day-scan plus burst planners with one phase tracker.
- Retain mtime interval intersection as the default absolute-offset path.
- Add a separately qualified filename-origin path only after §6.3 is
  characterized; phase tracking alone must not promote an offset SOT.
- Keep authenticated BLE, time-anchor discipline, source-tuple checks,
  settings/card invalidation, SOT freeze/supersede rules, and journal.
- Keep backoff, checkpoint freshness, global request accounting, and
  coverage-based therapy/transfer safety. Explicitly allow sub-second
  refinement spacing only under the measured service-time guard.

## 5. Request budget

- WATCH: 12 day pulls/minute; other discovery/crawl requests also exist.
- Acquisition: approximately 12 hunt pulls plus 4 + 4 + 3 narrowing pulls
  in the ideal sequence, plus optional refinement and reacquisition.
  Budget actual attempts, including failures and early changes.
- MAINTAIN: 3 day pulls/minute plus discovery/coverage requests.
- At 3.2/min over 8 h, maintenance alone is 1536 requests; acquisition,
  retries, and neighbors require additional allowance. The existing 700
  cap is incompatible with continuous three-pull maintenance.
- A revised cap and bounded acquisition/refinement allowances require
  explicit review before implementation. A 1600 cap leaves little margin;
  neither raise it silently nor describe OUT reads as physically cost-free
  merely because the current active ledger excludes confirmed OUT.

## 6. Expected accuracy and qualification

### 6.1 True-time event / phase accuracy

Separate interval **width** from midpoint **half-width**:

| Result | Interval width | Midpoint uncertainty before additional effects |
|---|---|---|
| Ideal 0.8 s single-cycle bracket | 0.8 s | ±0.4 s |
| Ideal shifted-grid phase interval | 0.4 s | ±0.2 s |
| Ideal optional refined phase interval | 0.2 s | ±0.1 s |

These apply to visible-update phase, conditional on stable recurrence.
With 236 ms service, a single 0.8 s-spaced pair can instead give about
1.04 s width (±0.52 s), before anchor and other allowances. For example,
a qualified 0.4 s phase interval plus ±0.03 s anchor uncertainty gives
±0.23 s; a 0.2 s interval gives ±0.13 s. Neither is a measured promise.

To describe the physical write moment, additionally account for the
independently bounded write-to-visible delay. A constant but unknown delay
is systematic: repeating polls or shifting their grid cannot reveal it.

**Recommendation:** target 0.2–0.4 s visible-phase width; report achieved
bounds, and report physical-write uncertainty separately. Small updates
and 60 s periodicity make this plausible, not yet certified.

### 6.2 Default mtime-based absolute clock offset

Let `b` be the UTC-visible-bracket width excluding separately listed anchor
allowance, `C` extra cache width, `Lw` latency-range width, `J` per-edge
allowance, and `A` per-edge anchor uncertainty:

```
offset half-width = (b + 2 s + C + Lw + 2J + 2A) / 2
```

Illustrative arithmetic, **not validated priors**:

| Assumptions | b=0.8 s | b=0.4 s | b=0.2 s |
|---|---|---|---|
| Current provisional C=2 s, Lw=2 s, J=0.3 s, A=0.03 s | ±3.73 s | ±3.53 s | ±3.43 s |
| Target C=0.5 s, Lw=0.5 s, J=0.1 s, A=0.03 s | ±2.03 s | ±1.83 s | ±1.73 s |
| Ideal C=Lw=J=A=0 | ±1.40 s | ±1.20 s | ±1.10 s |

The ±2 s gate remains realistic with sufficiently tight validated bounds,
not automatic. With exactly 60 device-second recurrence and fixed stamp
phase, repeated mtime observations retain the same unknown fractional
2 s position. Improved moment detection does not remove that ambiguity.

If fractional stamp phase varies, compatible intervals may intersect more
tightly; that is interval constraint tightening, not averaging independent
noise. Integer stamp deltas such as 56/58/60 s do not by themselves prove
fractional-phase diversity. Do not claim a finer tier from delta spread.

### 6.3 Conditional filename + periodic-moment offset path

The filename has a potentially finer device-clock origin. Let its decoded,
timezone-qualified claim be `F`, with actual origin in `[F+f_lo,F+f_hi]`.
A whole-second encoding might be truncated `[0,1)` or rounded
`[-0.5,0.5)`; either has width 1 s. Encoding precision alone does not prove
which physical event the filename represents.

If independently validated, model flush k's device time as:

```
device_write_k = filename_origin + k * 60 s + a_k
```

Here k is the correctly identified cycle index and `a_k` includes the
origin-to-first-flush relationship and bounded phase/rate deviations.
No record/header reads are required in the runtime path, but the
relationship must be characterized rather than assumed.

Then:

```
write_true_k   = visible_bracket_k - visibility_delay
clock_offset  = write_true_k - device_write_k
half-width    = (b + filename_width + a_width + C + Lw + 2J + 2A) / 2
```

For a 1 s filename interval, negligible extra terms, and phase width
0.4/0.2 s, the ideal offset half-width is **±0.7/±0.6 s**. Adding ±0.03 s
anchor uncertainty gives **±0.73/±0.63 s** before visibility and origin
uncertainty. This is a worthwhile conditional improvement over mtime, but
not a defensible ±0.2 s offset claim.

A finer result would require additional independently validated structure,
such as writes aligned to known integer-second device-clock boundaries,
allowing phase to be determined modulo 60 s while the timestamp resolves
the integer cycle. An arbitrary exact-60 s timer does not establish this.
Do not implement that inference without supporting evidence.

Current evidence (F7/F3): filename-derived file creation was about 6 s
after therapy start; first flush was about 63–65 s after therapy start.
Thus `F + k*60 s` is not yet a validated flush-time identity. Filename
and mtime also use different renderings (local versus UTC-shaped, F1);
correct timezone/DST conversion and unambiguous cycle indexing are required.

**Recommendation:** characterize this origin link alongside phase tracking.
Keep mtime as the qualified default until the filename path's complete
bound is demonstrably tighter on held-out sessions.

### 6.4 The requested +0 s / -1 s range

For an offset bound `[lo,hi]`, midpoint correction has error bounded by
±`(hi-lo)/2`. Lower-edge correction gives error `[-(hi-lo),0]`, at the
reference instant and under the validated model.

Therefore **+0/-1 s requires total offset width ≤1 s**, equivalently
midpoint half-width ≤0.5 s. A ±2 s result yields +0/-4 s, not +0/-2 s;
a ±1 s result yields +0/-2 s, not +0/-1 s. Earlier statements claiming
otherwise are withdrawn.

A 1 s-wide filename origin already consumes the whole requested width
before detection, visibility, and anchor errors. The requested range is
not guaranteed by these three observables alone without a finer validated
origin/phase relationship. A stored bound applies at `reference_at_ms`;
extrapolation to later data must also carry clock-rate/step uncertainty.

### 6.5 What characterization can and cannot establish

- Dense listings measure visible brackets, cycle stability, and HTTP
  service times. They do **not** independently measure physical
  write-to-visibility latency or cache freshness.
- A narrow bracket can coexist with a long constant visibility delay.
  The minimum bracket across a night is not a latency upper bound.
- Compare against independent device-clock observations and a qualified
  true-time reference to test complete offset coverage. To separate origin
  delay from visibility delay requires independent event timing; otherwise
  retain a conservative combined bound.
- Qualify filename encoding/event semantics, first-flush alignment, cycle
  indexing, rate, pause/resume, manual clock changes, and timezone behavior
  across varied sessions (base H2). Do not enable validation merely because
  intersections are self-consistent.

## 7. Implementation requirements (future work only)

- Scheduler: explicit WATCH/HUNT/NARROW/VERNIER/REFINE/MAINTAIN states,
  actual scan-time evidence, service-aware deadlines, one worker, bounded
  attempts, and discovery/monitoring coverage independent of phase success.
- Phase tracker: translated moment intervals, period/rate bounds,
  cycle-jitter expansion, session reset rules, and deterministic miss
  recovery. Do not infer cadence from response-end differences alone.
- Offset estimator: retain mtime path; add optional characterized filename
  origin only with explicit provenance and separate qualification.
- API/journal: report phase width separately from offset half-width,
  estimator source, reference instant, model qualification, and misses.
  Phase-only precision must never populate the offset SOT.
- Host tests: fixed 60 s phase, biased constant visibility delay, variable
  latency, real request durations, shifted-grid intersection, optional
  refinement, misses, beacon swaps, neighbor scans, clock steps, filename
  rounding/truncation and origin delay, cycle ambiguity, width/half-width
  arithmetic, and one-sided correction.
- No firmware/profile/ledger edits or flashing are part of this document
  change. Budget and API changes need review before implementation.

## 8. Validation and rollout

1. Implement/test the unified schedule without narrowing unvalidated
   visibility/origin bounds or promoting phase-only results to SOT.
2. Characterize phase precision and combined offset coverage against an
   independent reference. Restore optional refinement only when useful.
3. Verify H3/E0b write safety (EDF record counts/integrity and SD errors).
   Read-only operation does not automatically establish noninterference.
4. Qualify bounds across ≥10 varied sessions before enabling the offset
   `accurate` tier by default. Enable the filename path separately if its
   origin model and coverage are validated.

## Summary recommendation

- **Visible update phase:** aim for 0.2–0.4 s total width, plus validated
  timing allowances. Optional shifted-grid refinement can be useful.
- **Mtime-based offset:** retain ±2 s as the qualification target; actual
  results depend on visibility/cache bounds, not poll precision alone.
- **Filename + moment offset:** conditionally around ±0.6–0.7 s before
  additional uncertainties, if the origin-to-flush link is validated.
- **+0/-1 s absolute correction:** not currently established; needs total
  offset width ≤1 s and stronger device-origin/phase information.
