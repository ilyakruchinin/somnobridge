# SomnoBridge — implementation update 4: measured request timing and explicit visibility semantics

Status: implemented, revision 1.
This update changes the timing model, diagnostics, and persisted-model
compatibility checks. It does not qualify accuracy, start therapy, or relax
authorization for flashing/live characterization.

## 1. Purpose and decisions

Use timing measured by the ESP, not BLE delivery times or arbitrary HTTP-delay
constants. Make bounded phase-shifted acquisition/refinement the single normal
precision strategy. Report exactly which uncertainty is measured and which
remains conditional on hardware semantics.

Decisions:

1. Expose each relevant EZShare listing's ESP-monotonic request interval and
   measured service duration through authenticated BLE diagnostics.
2. Do not add a fixed 2-second HTTP allowance or subtract half/full round-trip
   time from an observation. Actual request intervals already include HTTP time.
3. Propose a scoped zero-additional-cache operating profile for the tested
   AS11/EZShare combination. Zero is a model choice, not a numerical quantity
   established by the absence of timestamp regressions.
4. Separate HTTP service duration from timestamp-to-listing visibility delay.
   Do not replace the latter with request duration; they measure different things.
5. Use adaptive phase-shifted dense polling by default, without a user precision
   toggle. WATCH/HUNT and safe degradation remain states of the same strategy.
6. Produce the tightest defensible interval. A smaller conditional interval is
   useful but MUST NOT be presented as independently validated physical accuracy.

`implementation-spec.md` remains authoritative except where this update
explicitly amends it:

- §6/update2 profile gates: phase tracking/refinement is the scheduler's single
  normal timing path; the drift profile no longer carries enable switches.
- §8/update3 model defaults: `cache_bound_us=0` and residual
  `latency_lo/hi_us=0` for the tested AS11/EZShare scope, labelled as
  `assumed_zero`/`assumed_immediate` rather than measured truth.
- §10 diagnostics: GET_STATUS and GET_DRIFT schema 4 add ESP-monotonic timing,
  model provenance, and recent raw transition brackets.
- §9/persistence: persisted SOT identity includes `model_rev`, the
  cache/visibility policies, and the numeric timing/source bounds carried in
  the record; incompatible records are stale/inapplicable.

No safety budget, security policy, read-only constraint, transfer gate, or
accuracy qualification rule is implicitly relaxed.

## 2. Corrections to earlier interpretations

### 2.1 No regressions is not proof of zero cache

A cache can refresh every second, serve only monotonically newer snapshots,
and never return an older stamp after a newer stamp. Therefore zero regressions
across the previously reported 54 transitions is consistent with no cache, but
also with a monotonically delayed cache. Matched post-session listings likewise
do not determine refresh delay during writes.

The existing evidence does not establish a universal zero-cache bound. The
user's zero-cache premise can be adopted for the tested topology as an explicit
operating assumption. Validation requires independent freshness evidence or a
supported mechanism establishing when directory state is read. Observed
regressions invalidate freshness, but their absence cannot certify it.

### 2.2 Request duration is not write-to-visibility delay

The ESP requests a listing from EZShare, not directly from the AS11. It can time
that request without BLE. It cannot discover, from that duration alone, how long
ago the AS11 assigned the stamp or when its SD directory update became readable.

For example, both cases can return a listing in 200 ms:

- the listing reads directory state immediately;
- the listing serves directory state delayed by 1 second.

The HTTP measurement is identical; the clock correction need not be. A common
visibility delay and a clock offset are confounded without independent truth.
Faster polling and additional sessions do not alone remove that systematic bias.

### 2.3 Phase prediction is not a direct event measurement

In `phase.c`, `iunc_us` is half the predicted next-cycle interval width. It is
neither the last transition bracket width nor a measured physical write delay.
The actual last bracket is `[last_br_lo_us, last_br_hi_us]`. Earlier descriptions
of `iunc≈0.9 s` as a sub-second measured bracket were incorrect.

Likewise, the reported filename residual `−0.5..+5.8 s` is an appearance window
relative to a drift-corrected filename reference, not a stopwatch creation delay.
Its endpoints require reference uncertainty and encoding semantics. They are
not evidence of a negative physical delay or a precisely measured 5.8-second
write delay. Re-score these observations with interval-valued references before
using them to tighten a profile; do not certify the installed bounds from these
point estimates alone.

## 3. Verified current implementation

- `components/ezshare/ezshare.c::ezs_list_dir()` records `req_start_us` before
  `open_get()` and `resp_end_us` after the listing body read, before parsing.
  The interval includes connection setup, HTTP exchange, and body acquisition;
  it is not an exact packet-on-wire timestamp.
- `components/scheduler/sched.c::svc_note()` already computes
  `resp_end_us - req_start_us`. A rolling median guards polling start spacing.
  Successful day-listing logs already include both timestamps.
- Scheduler observations feed both the drift estimator and phase tracker.
  Raw estimator transitions use the previous scan start and changed response end;
  the phase tracker uses its retained unchanged start and changed response end.
- `components/drift/drift.c::event_window()` applies only explicit residual
  cache/visibility terms outside the measured request interval. Update 4's
  scoped model sets both to zero and reports them as assumptions; HTTP duration
  remains represented by the actual endpoints and is not compensated again.
- Phase tracking and refinement are now unconditional scheduler policy. The
  state machine has bounded bursts, actual observation intervals, shifted grids,
  service-time guards, and reacquisition. Reuse it; do not add a poller.
- GET_STATUS exposes the latest actual listing interval and bounded service
  statistics; GET_DRIFT exposes model provenance and recent transition
  observations; `timing_obs` journal records preserve transition endpoints.

The completed work is diagnostic export plus explicit model semantics — not a
new ESP request timer, which already existed.

## 4. Measurement and estimator model

### 4.1 Request measurements

For successful listing i, record:

```
s_i = ESP monotonic time before issuing the request
r_i = ESP monotonic time when the complete response body has been acquired
http_service_us_i = r_i - s_i
```

Retain the existing enclosing interval. Optional header/first-body timestamps
may help diagnose transport phases but MUST NOT replace the response-end bound
unless it is established that the relevant directory tuple was sampled earlier.
A directory listing is not automatically an atomic snapshot of all files.

Do not infer the server's snapshot time as `s_i + service_i/2`. Path symmetry,
processing delay, and snapshot timing are unknown. Do not use an average, median,
minimum, or observed maximum as a guaranteed future service bound. Use actual
per-request endpoints for evidence and statistics only for planning.

### 4.2 Zero-cache visibility bracket

Under fresh directory-read semantics, a previous successful unchanged listing u
and the first changed listing c give an enclosing listing-visibility bracket:

```
B_mono = [s_u, r_c]
B_utc  = [Vlo, Vhi]  # mapped through accepted anchor intervals
```

This includes request time, poll spacing, and actual scheduler slippage. Neither
HTTP RTT nor BLE delivery latency is added again or subtracted as compensation.
BLE merely transports a previously recorded measurement. BLE/WiFi coexistence
can still affect the HTTP measurements physically; the actual intervals capture it.

Use the same beacon, session, clock segment, and uninterrupted monotonic domain.
Failed, incomplete, malformed, or stale/unbound observations cannot provide an
unchanged edge. Never substitute a scheduled deadline for an actual timestamp.

### 4.3 Timestamp-to-visibility delay

Define D as the elapsed time from the instant represented by the AS11 timestamp
(before its quantization) to that update becoming list-visible. This includes
any applicable timestamp assignment, write ordering, SD publication, or directory
visibility effects not already represented by the request interval.

With additional cache age bounded by C, delay bounded by `[Dlo,Dhi]`, and a
validated truncated FAT stamp bin `[M,M+2 s]`:

```
write_utc = [Vlo - C - Dhi, Vhi - Dlo]
offset    = [write_lo - (M + 2 s), write_hi - M]
```

Offset means `true_time - device_time`. Other timestamp encodings need their own
bins. Anchor uncertainty is already in B_utc; a separate jitter term requires a
separate documented physical effect. Do not count a delay both in C and D.

For the update-4 AS11/EZShare operating profile, C=0 and D=0 are selected as
explicit scoped assumptions. They are serialized as
`cache_policy=assumed_zero` and
`visibility_policy=assumed_immediate`, included in `assumption_names`, and kept
below the independently validated/qualified tier. This yields the tightest
auditable conditional interval without pretending that HTTP service time proves
directory freshness.

If independent characterization later supports a nonzero visibility delay,
replace D with that bound and increment `model_rev`; persisted records from an
incompatible model are restored only as stale history and cannot be applied.
If evidence invalidates the zero-delay premise, fail closed rather than clamping
to zero.

The previous `[0,2 s]` default is removed and must not be called measured or
replaced with `[0,http_service_us]`. A legacy comparison result may remain
clearly labeled, not silently mixed with the new model.

### 4.4 Independent characterization, without circular calibration

Record dense brackets on the ESP; retrieve them later over BLE. A separately
bounded AS11-to-UTC clock reference (documented read-only protocol or independent
instrumentation) can bound the true stamp-assignment interval `[Tlo,Thi]`.
Then each event supplies a feasible delay interval:

```
D_event = [Vlo - Thi, Vhi - Tlo]  # zero-additional-cache model
```

Intersect with nonnegative delay only if that timestamp interpretation is valid;
an empty result is a model/reference violation, not permission to clamp to zero.
The interval includes quantization and reference uncertainty, so even a 200 ms
HTTP response does not guarantee a sub-500 ms characterization result.

Do not calibrate D using an offset estimated from the same card observations:
that would be circular. Do not intersect variable delays across events as though
they were one constant. Report per-event intervals and their conservative hull
with sample count, conditions, reference bounds, and hardware scope. Sampled
maxima characterize the experiment, not all possible future operation.

## 5. Mandatory adaptive strategy, bounded execution

For the supported operating profile, phase acquisition, shifted refinement, and
maintenance are automatic parts of the single scheduler strategy. There is no
normal fixed-poll-only user option and no BLE-driven independent polling loop.

Mandatory strategy does not mean continuous sub-second requests. Discovery,
WATCH/HUNT, reacquisition, safe backoff, and budget suspension remain necessary.
Enable refinement when its evidence, service, and budget prerequisites hold;
otherwise report a specific degraded reason and preserve honest wider bounds.
A prediction guides request placement; it cannot substitute for an observation.

Preserve one in-flight HTTP request, bounded queue/rings, service guards, burst
caps, no catch-up bursts, transfer exclusion, session/beacon resets, cycle
ambiguity handling, and persisted request accounting. Unknown/uncharacterized
hardware remains subject to the base contract's safe monitoring restrictions.

Do not describe this strategy as automatically budget-neutral. At 3–5 listings
per minute for 8 hours it would use 1,440–2,400 requests; the current persisted
8-hour cap is 8,000 requests, so the nominal ledger does not force fallback, but
continuous dense polling still creates avoidable card/WiFi load and queue
pressure. Concentrate bursts in bounded acquisition/re-refinement windows; use
sparse maintenance after convergence, retaining age/rate-expanded uncertainty.
Any cap change needs an explicit load/safety decision, not an implicit increase
because precision is mandatory. Exhaustion must degrade honestly, never reset
or bypass the ledger.

## 6. Diagnostics and evidence requirements

Extend existing authenticated status/drift/evidence APIs compatibly. BLE reads
return cached ESP measurements and MUST NOT cause extra card requests.

Required metadata:

- Boot/monotonic-domain identity, observation sequence, purpose, and scoped
  card/firmware/model identity; no waveform or patient-header data.
- Last listing `req_start_us`, `resp_end_us`, `http_service_us`, validity and age.
- Bounded service summary: count/window, min, median, maximum, failures. Label
  maxima empirical, not hard bounds. Reset association-specific summaries.
- Per-transition unchanged and changed observation identifiers/endpoints,
  beacon/session identity, raw stamp/bin, bracket width, mapped UTC bounds,
  and mapping validity/anchor uncertainty.
- Phase state, actual spacing, prediction half-width, actual last-bracket width,
  cycle, misses/reacquisition reason, and refinement/degradation reason.
- Offset full interval and half-width, separate contributions from observation,
  stamp quantization, anchors, cache and visibility model, and model assumptions.
- Visibility-delay bounds with provenance (`assumed`, `characterized`, or
  `unknown`), cache policy/provenance, qualification state, and budget headroom.

Use a bounded in-memory observation ring and replay sequence/gap reporting.
Do not write every poll to NVS. Reuse the bounded journal for selected transitions
if needed; disclose eviction/truncation and schema version. Size API buffers,
fragments, and task stacks explicitly; avoid large automatic snapshots.

Preserve raw data so model changes can recompute results. Increment model identity
for changed semantics, and reject/mark inapplicable persisted SOT records with
incompatible model revisions at boot as well as runtime. Old SOT must not appear
to have been obtained with the new assumptions.

## 7. Accuracy expectations and validation

For one zero-cache event with B_utc width b, FAT-bin width 2 seconds, and delay
range width d, the resulting offset half-width is `(b + 2 s + d)/2`.
This is interval arithmetic under the declared model, not a physical guarantee.

Example: a 0.6-second raw bracket, UTC mapping ±0.05 seconds per edge, and
independently supported D=0 give ±1.35 seconds. With D in `[0,0.5 s]`, the same
event gives ±1.60 seconds. If D is unknown, neither is a guaranteed bound.
FAT quantization remains; phase shifts cannot reliably average away a bin when
a 60-second cadence repeatedly lands at the same device-clock phase.

Acceptance work:

1. Host tests: variable/asymmetric HTTP times, delayed and monotonic-cached
   listings, failed/incomplete responses, anchor uncertainty, quantization,
   missed cycles, per-file write ordering, session changes, and ring overflow.
2. Assert actual timestamps are used; RTT is neither subtracted nor counted
   twice. Inject a constant hidden visibility delay and ensure HTTP-only
   diagnostics cannot falsely validate it. Keep phase precision distinct from
   offset accuracy and show unsupported immediate-visibility results conditional.
3. Test rolling peak/acquisition/8-hour budgets and long sessions, including
   obligatory degradation rather than continuous bursts exceeding the cap.
4. Test model-revision invalidation on reboot and runtime profile change.
5. Run `./scripts/run_host_tests.sh`, recorder self-tests, and firmware build;
   inspect scheduler/API stack and bounded allocation changes on target.
6. After separately authorized hardware installation, start passive capture
   before a user-started therapy session. Capture device timing metadata through
   cached BLE reads; BLE receipt times are not card-event endpoints.
7. Compare offset intervals with independently bounded clock truth across varied
   session phases, listing sizes, WiFi/BLE loads, reconnects, and supported
   hardware versions. Record reference quality, sample scope, interval coverage,
   budget use, and any violations. Qualification retains the contract's gates.

Do not change host anchor acceptance thresholds just to achieve qualification.
Do not tune priors or drop inconvenient observations to force `half < 2 s`.
Success is an auditable improvement where narrower intervals contain independent
truth under their declared scope, with degraded states explained when they do not.

## 8. Implementation status and remaining work

Implemented in this update:

1. Existing device-side request endpoints are exported and actual transition
   brackets are retained in a bounded diagnostic ring.
2. Scoped zero-cache and assumed-immediate-visibility provenance is explicit;
   model revision, policies, and numeric timing/source bounds are persisted
   with SOT records.
3. Adaptive phase tracking/refinement is always the scheduler policy; there is
   no fixed-poll precision path.
4. Persisted records from an incompatible model are stale/inapplicable after
   reboot, and runtime model changes mark existing SOT stale.

Remaining before any qualified tighter result:

- Characterize residual timestamp→visibility delay with independent truth.
- Validate coverage across sessions, listing sizes, load, reconnects, and
  hardware versions.
- Qualify the selected source only after those bounds are evidence-backed.

The immediate gain is removing unexplained HTTP/cache constants and exposing
replayable evidence. The reported interval may be narrower, but until residual
visibility semantics are independently supported it is a conditional provisional
interval, not a certified physical accuracy bound.
