# SomnoPal — implementation update 5: independent scoring and qualification

Status: host-side collector/scorer implemented; one beacon-generation firmware
fix is implemented and build-verified but not flashed; firmware qualification
still pending. Written after the 2026-10-02 update-4 run and updated after the
host-tooling smoke test and first automated live session. This document
records the supplied run, read-only live checks, implemented analysis plumbing,
and remaining requirements. It does not authorize therapy control, flashing,
profile certification, or changes to SomnoTrace. The base implementation
contract and update-4 timing semantics remain in force.

Implemented host tooling:

- `ble_query.py` now derives BLE anchor source uncertainty from chrony by
  default, retains source provenance/epoch, and supports non-committing raw
  exchanges for offline mapping.
- `validate_timing.py observe` performs bounded passive collection of
  SomnoPal status/drift, raw brackets, SomnoTrace clock references, session
  metadata, BLE exchange anchors, and journal records without adding card
  requests.
- `validate_timing.py analyze` now deduplicates observations, maps them with
  raw BLE exchanges when available, scores interval coverage/containment, and
  derives residual timestamp→visibility delay intervals.
- Smoke result: three snapshots, four retained observations, nine clock
  references, three host-side anchor exchanges, and matching SomnoTrace
  session metadata were scored successfully. References were intentionally
  collected after the smoke window, so delay intervals were wide; this is an
  analyzer-path check, not a visibility characterization result.

## 1. What the latest run demonstrates

The supplied session reached the desired numerical precision:

| Measurement | Result |
|---|---|
| Initial live half-width | 3,319 ms |
| Final live half-width | 1,266 ms |
| Final correction, true time minus device time | −507,615 ms |
| Final correction interval | [−508,882, −506,349] ms |
| Independent SomnoTrace stop-time correction | −508,008 ms |
| Signed midpoint difference, SomnoPal minus reference | +393 ms |
| Latest raw bracket | 554.002 ms |
| Narrowest raw bracket shown | 554.002 ms |
| Final observation count / clock segments | 11 / 1 |
| Active timing model | revision 3, assumed zero cache/immediate visibility |

The reference point is inside the final interval, 874 ms above its lower edge
and 1,659 ms below its upper edge. This is encouraging agreement, not a measured
±393 ms accuracy bound. The midpoint difference is one comparison; uncertainty
in both measurements and their different reference times remains relevant.
The stop reference was measured 23.373 seconds after SomnoPal's final
`reference_at_ms`; at the installed 500 ppm device-rate bound, transporting an
interval across that gap adds about 11.7 ms per edge. Always score at a common
time rather than comparing unrelated timestamps silently.

Observed brackets shrank from about 5.1 s to 2.2 s, then approximately 1.0 s,
and finally about 0.55 s. This is evidence that adaptive acquisition/refinement
is narrowing actual observations, not merely improving a prediction display.
The 2-second FAT timestamp bin, UTC mapping uncertainty, rate expansion, and
intersection of compatible events explain why the final offset interval remains
about 2.53 seconds wide. Half-width is an enclosing interval under the model,
not a standard deviation or a confidence percentage.

The session-start interval remains about 5.15 seconds wide. A precise current
clock correction does not prove the native file's creation instant equals
therapy start or the start of the first recorded sample. Start semantics need
separate scoring; do not silently replace that interval with the drift midpoint.

## 2. Why qualified SOT was not promoted

No width-threshold change is needed. `accurate_half_us` is 2,000,000 us;
1,266 ms passes it. The selected source is mtime, but the compiled profile has
`stamp_channel_validated=false`. The emitted `timing_unvalidated` flag is the
specific blocker, and `approximate` is a validation tier, not a statement that
the numerical half-width exceeds two seconds.

Current qualification logic in `components/drift/drift.c`:

- Estimator: usable bound, accepted mapping, at least two observations,
  validated selected source, and half-width no greater than two seconds.
- Qualified SOT: usable bound/mapping, one clock segment, no unresolved source,
  contradiction, or source disagreement, at least two observations, validated
  source, and the same width threshold.
- Provisional SOT: the safety conditions above, at least three observations,
  and half-width no greater than five seconds, without source certification.
- The legacy `source_of_truth` view intentionally selects qualified SOT only.
  The provisional slot is separate; explicit best-available opt-in can expose a
  non-stale applicable provisional record.

A SOT **was** promoted, but into the provisional slot: model revision 3,
[−508,795, −506,228] ms, half-width 1,283 ms. The old revision-1 slot correctly
became stale and was replaced. Later 1,266 ms and 1,257 ms live results improved
by only 17 ms and 26 ms relative to that saved record. Replacement normally
requires at least 250 ms improvement, non-overlapping bounds, or a stale/empty
slot. This hysteresis avoids NVS churn; it is unrelated to qualification.

Recommendation: retain both thresholds and replacement hysteresis. Collect
independent coverage evidence before certifying the source. The tool should
say `qualified SOT unavailable: selected source not validated`, not imply that
nothing has ever been saved when a useful provisional slot exists.

A future certification change must require supported cache/visibility semantics,
not just flipping a validation boolean. Add explicit enforcement that an assumed
zero-cache/immediate-visibility model cannot produce qualified truth solely by
setting `stamp_channel_validated`. Increment model identity for certification
or bound changes; do not relabel old provisional measurements as qualified.

## 3. Read-only checks performed for this proposal

No therapy commands, waveform downloads, firmware writes, or anchor commits
were made during these checks.

- SomnoPal GET_STATUS / GET_DRIFT responded over authenticated BLE. The session
  is now observation-closed, retains the 1,266 ms estimate, and has a valid,
  persisted revision-3 provisional SOT. It is out of therapy.
- ESP-side listing statistics reported 216 successes, zero failures, minimum
  33.057 ms, maximum 365.967 ms, and rolling median 207.654 ms. The last listing
  took 262.751 ms. Min/max/count cover the association; the median covers a
  bounded recent window. These are empirical statistics, not future hard bounds.
- `POST http://192.168.86.102/api/ble/passthrough` with only `GetDateTime`
  responded. A three-query check returned midpoints −508,023.8, −507,961.3,
  and −508,063.5 ms, with enclosing HTTP RTTs 116.3, 247.6, and 171.5 ms.
- The current chrony-derived conservative host uncertainty was ±149 ms.
  The best-RTT query's existing-tool reference interval was approximately
  [−508,231.9, −507,815.6] ms. This is a later check, not a replacement for the
  run's contemporaneous reference, and is conditional on RPC timestamp semantics.
- SomnoTrace's documented `/api/status` responded, reported NTP synchronization
  and a paired AS11. Its returned status did not include PRD sample timestamps
  or an interval-valued AS11 clock reference.

The bridge provides a working near-real-time independent clock-query path.
It is not a verified HTTP subscription to the raw PRD stream. Do not invent a
stream endpoint or open a second AS11 BLE connection. Use the existing paired
SomnoTrace connection and documented protocols only. Millisecond-looking RPC
text alone does not certify millisecond clock resolution or sampling semantics.

## 4. Priority gap: honest anchor and reference uncertainty

`ble_query.py::time_exchange()` defaults to `unc_us=1000`, and the CLI's
`--time-sync` calls it without measuring the host time source. Its displayed
uncertainty adds half the measured RTT and a timestamp allowance, but it does
not discover chrony uncertainty. The supplied 39.54 ms RTT / ±25.77 ms result
is consistent with that assumed 1 ms source input; device acceptance certifies
protocol checks, not the correctness of the caller's source claim.

Chrony was ±149 ms during this investigation. Its value at the earlier sync
was not recorded here and cannot be inferred retroactively. Therefore the
25.77 ms anchor must not be treated as independently verified from this log
alone. A conservative host bound is not evidence that the actual error was
149 ms, either; it says the smaller guarantee has not been established.

Required changes:

1. Share the existing `clock_bound()` logic with BLE time-sync tooling. Query
   chrony immediately before each exchange group, retain provenance, reject
   unsynchronized/unknown sources, and never silently default to 1 ms.
2. Explicit source overrides require provenance and a warning; they are not a
   knob for forcing acceptance. Retain actual raw exchanges and source bounds.
3. Preserve the existing 100 ms source acceptance limit. If the measured source
   exceeds it, report why and keep exchanges for offline interval mapping;
   do not commit a knowingly understated bound or relax the firmware gate.
4. Detect wall-clock steps and source-epoch changes. Map only within one
   uninterrupted domain, with accepted anchors and oscillator expansion.
5. Refresh anchors at a bounded cadence where the source is usable. Preserve
   anchor uncertainty and age in diagnostics, including each event's mapping.
6. Audit earlier evidence for declared versus verified source uncertainty.
   Re-score when raw exchanges and contemporaneous source evidence exist;
   otherwise flag the coverage result conditional/uncertainty unknown.

SomnoTrace metadata provides a useful independent point but presently the
supplied fields do not include its NTP uncertainty, RPC request/response
endpoints, timestamp quantization model, or clock-source epoch. `ntp_synced`
and `clock_drift_usable` alone are insufficient to derive those bounds. The
host-bracketed GetDateTime path can be used without modifying SomnoTrace;
additional device-side reference metadata would improve it but requires a
separate, clean-room SomnoTrace task.

## 5. Automated passive collection

Extend `scripts/validate_timing.py`, rather than introducing another card poller.
Once launched, the collector should observe ordinary user-started sessions,
score them, and continue/recover without manual BLE queries. No mask operation,
therapy start/stop, or clinical action is required from the collector. An idle
machine produces no new flush evidence; automation cannot manufacture sessions.

Implemented command:

```bash
python3 scripts/validate_timing.py observe \
    --day $(date +%Y%m%d) --duration 7200 \
    --host 192.168.86.102 --no-anchor-commit
python3 scripts/validate_timing.py analyze \
    --input /tmp/somnopal-validation-XXXX/evidence.jsonl
```

Existing gaps now split into:

- `capture` remains the older cached-catalog recorder. The new `observe` mode
  captures GET_DRIFT raw observations, model provenance, HTTP timing,
  provisional/qualified SOT state, references, metadata, and journal records.
- `analyze` now replays update-4 brackets and reports coverage/containment;
  older cached-catalog transition summaries remain available for comparison.
- The serial parser has been updated for current scheduler logs, but BLE-only
  collection remains the primary path and serial is optional.
- GET_DRIFT exposes only the latest four observations; the internal diagnostic
  ring has twelve. Slow/disconnected collection can miss transitions.
- GET_STATUS's last request can miss fast bursts; its rolling median is not
  explicitly labelled with window size, unlike association-wide count/min/max.
- Cached API records lack a self-contained monotonic-domain identity suitable
  for safe de-duplication across boots and session sequence resets.
- Native replay currently returns retained journal data, not a cursor-based
  incremental observation API. Journal RAM tail may take up to 60 s to commit.

Default collection policy (implemented in `observe`, subject to load testing):

- Keep one authenticated SomnoPal BLE connection, serialize operations, and
  read cached GET_STATUS + GET_DRIFT every five seconds while active. Back off
  to 30–60 seconds when idle. Skip overlapping work rather than accumulating it.
- Collect raw observations keyed by boot/domain, session/segment, beacon, and
  observation sequence. Keep repeated mappings as revisions, not new events.
  Record missing sequences, reconnects, elapsed read time, and eviction.
- Read cached LIST_DAY less often, for session association and per-type context;
  catalog differences at host cadence are not substitutes for ESP brackets.
- Use a three-query GetDateTime group roughly once per minute while active,
  sparse queries while idle, and bounded edge queries on session transitions.
  Respect shared RPC busy responses/timeouts with backoff; do not interfere
  with post-therapy spool work. Record complete host request/response intervals.
- Retrieve whitelisted session timing metadata over existing read-only FTP at
  session closure. Tolerate partial/changed JSON and retry without claiming
  finality until completed. Do not download PRD/EDF waveforms or patient fields.
- Retrieve journal evidence at bounded session/reconnect boundaries, not by
  repeatedly streaming the entire journal in every five-second iteration.
  Retain boot boundaries, anchors, model changes, and gap flags.
- Make serial optional and fix its parser. BLE-only collection must work.
- Add a bounded observer mode for multiple ordinary sessions; duration, disk
  quota, rotation, retry counts, timeouts, and shutdown behavior are explicit.
  No always-on service/configuration is installed implicitly.
- Store evidence privately (directory 0700, files 0600), timing-only allowlist,
  bounded sizes and retention. Exclude device identifiers from shared reports.

BLE receipt time is only a collection timestamp. SomnoPal event endpoints remain
ESP monotonic values. Querying status/drift must not generate extra card HTTP
requests. Collection traffic can still perturb BLE/WiFi coexistence; include
collector load as an experimental condition and test lighter collection too.

## 6. Offline scoring and visibility characterization

Use `true_time - device_time` consistently. For each raw stamp transition,
retain the file-specific timestamp bin and actual visibility interval V.
With FAT bin `[M,M+2 s]` and independently bounded correction O at the event:

```
T_stamp = [M + O_lo, M + 2 s + O_hi]
D_total = [V_lo - T_stamp_hi, V_hi - T_stamp_lo]
```

D_total measures the residual timestamp-to-observed-visibility relation,
including any unseparated freshness delay. HTTP service duration is already
inside V. Never subtract RTT or add another HTTP allowance.

Example using the supplied −508,008 ms reference point and observation 11:

```
V       = [1790920684994, 1790920685651] ms
M       = 1790921192000 ms
T_stamp = [1790920683992, 1790920685992] ms
D_total = [−998, +1659] ms
```

This example temporarily treats the reference as exact to illustrate arithmetic.
Actual reference uncertainty and rate transport widen the edges. It is
compatible with immediate visibility, but does not establish delay below
0.5 seconds, no cache, or negative physical delay. Intersect with nonnegative
delay only after validating that timestamp interpretation; an empty interval
is a violation to investigate, not something to clamp or discard.

Requirements for analysis:

1. Match the same machine/session by bounded time association and file identity;
   flag ambiguous matches. User-confirmed same-machine topology is retained.
2. Prefer contemporaneous bracketed clock references. Transport start/stop
   references with documented rate uncertainty; split on clock changes and
   never interpolate through an NTP step or reboot.
3. Preserve the reference's uncertainty, quantization, sampling semantics, and
   provenance. Fractional digits without verified resolution remain conditional.
4. Report point residuals separately from interval coverage: fully contained,
   overlapping/inconclusive, disjoint, or unavailable. Account for shared host
   NTP error across anchor and RPC measurements; they are correlated, not two
   independent clocks. Either model that common-mode term or use conservative
   enclosing intervals without claiming statistical independence.
5. Compare mtime, creation, periodic candidates and selected fit separately.
   Replay fixed recorded events under alternative models without circularly
   using SomnoPal's own fitted offset to calibrate delay.
6. Delays may vary across events. Do not intersect them as one constant.
   Report event intervals and conservative session/condition hulls; observed
   maxima/percentiles are empirical and cannot certify all future operation.
7. Fit prospective bounds on characterization sessions, then score separate
   held-out sessions. Do not select bounds merely because they make this run
   agree, remove inconvenient events, or shrink the displayed half-width.
8. Keep per-file write ordering and initial/final flushes separate. The current
   published observations concern the selected beacon, not all three file types.
   Add bounded per-type diagnostic evidence if that coverage is required.

Required report: session/event counts; missing evidence; HTTP/bracket
min/median/p90/p95/p99/max with sample counts and scopes; offset residual
median/range; interval coverage/violations; reference uncertainty and age;
time to reach ≤2 s; SOT promotions/rejections; phase state/misses; request
ledger deltas; load/reconnect/listing-size conditions; and start-edge accuracy.
Deduplicate cached last-request timing before calculating service statistics;
otherwise repeated status reads weight one request many times.

## 7. Opportunities to improve precision without false certainty

Prioritize reference correctness and lossless evidence over narrower numbers.
The current run already meets the numerical target. Likely useful improvements:

- Verify/calibrate UTC anchor provenance and refresh healthy anchors; this
  prevents spurious tightness and reduces real mapping-age uncertainty.
- Use collected service distributions for existing scheduling guards and
  bounded phase shifts. Do not use their median to compensate event timing.
- Study whether misses come from grid placement, WiFi load, or slow listings
  before changing burst sizes. No continuous dense polling or cap increases.
- Keep sparse maintenance and rate-expanded bounds. Longer observation time
  does not justify shrinking a systematic-delay term by averaging.
- Characterize effective visibility semantics with independent references,
  then install an evidence-backed bound and increment model revision.
  A measured nonzero bound may honestly widen the result while improving its
  reliability. Cache age and SD publication delay may remain inseparable;
  avoid counting the same effect in both terms.
- Validate FAT encoding and filename semantics across phases. A two-second
  stamp bin remains a substantial floor on per-event precision; finer channels
  require validated semantics, not assumed rounding or record timing.
- Calibrate device-rate bounds only from independent long-baseline evidence;
  do not tighten the installed 500 ppm bound from this short run alone.
- Ensure timezone/DST transitions are handled; the current fixed `AEST-10`
  rule is suitable for this date but is not a year-round Melbourne DST rule.

No direct AS11 reference should silently replace the autonomous SomnoPal fit.
The reference is an experimental scorer, not a new production dependency.

## 8. Firmware/API follow-up and acceptance

Add backward-compatible bounded diagnostics where needed:

- Ephemeral monotonic-domain token, session/segment identity, model identity,
  anchor provenance/age/uncertainty, oldest/newest observation sequences, and
  dropped/evicted counts. This is evidence identity, not catalog versioning.
- Cursor-based read-only observation replay of the existing bounded ring,
  with a gap response if a requested sequence has expired; no card polling.
- Explicit service scopes: association count/min/max/failures and recent-window
  size/median. Export request sequence to de-duplicate service samples.
- Machine-readable qualification blockers and SOT replacement reasons, including
  `source_not_validated`, `width_exceeds_threshold`, mapping/safety failures,
  and `improvement_below_hysteresis` where applicable.
- Auditable firmware/card identity: current `card_fw` in the live SOT contains
  a truncated XML prolog, not a trustworthy firmware version. Treat it unknown
  until proper parsing is verified; do not silently certify all card firmware.
- Preserve bounded stacks, heap/serialization error behavior, authenticated
  access, persisted budgets, transfer exclusion, and the read-only card policy.

Acceptance tests:

1. Unknown/unsynchronized/high-uncertainty host clocks cannot silently commit
   1 ms anchors; verified overrides and discontinuities retain provenance.
2. A <2 s unvalidated source stays provisional; a validated supported model
   passes all qualification gates; assumed policies cannot bypass certification.
3. Four/latest-twelve observation windows, overflow, repeated snapshots, boots,
   session resets, gaps, delayed journal commits, and reconnects are replay-safe.
4. Reference quantization, delayed/asymmetric RPCs, shared clock error, clock
   steps, rate transport, timezone changes, missing metadata, and held-out
   coverage violations are reported rather than hidden.
5. The supplied final comparison reproduces +393 ms and reference containment;
   observation 11 reproduces the illustrative [−998,+1659] ms delay interval.
6. Synthetic hidden visibility delay must not become characterized merely from
   short HTTP requests or absence of stamp regressions.
7. Passive collection does not invoke therapy/settings commands or increase
   the card ledger beyond firmware's own scheduled requests. Busy RPCs degrade
   collection safely. Long-session storage, API load, and budget tests pass.
8. Run host tests, timing self-tests, Python validation, and target build for
   implemented changes. Qualify across the existing ≥10-session hardware gate,
   varied start phases, pause/resume, listing sizes, reconnects, and loads;
   retain scope and independent reference bounds, not just a passing count.

## 9. First automated live result

A masked session (`20261002_164953`-era SomnoTrace metadata; SomnoPal beacon
`20261002_164826_BRP.edf`) was collected passively after the user confirmed
mask placement. SomnoTrace `EnterTherapy` was acknowledged; the user stopped
therapy manually. No card writes or additional card polls were added.

| Item | Result |
|---|---|
| SomnoPal snapshots | 216 |
| Bracketed `GetDateTime` references | 69 |
| BLE exchange groups | 3, host-side only; source bound ≈ ±148–150 ms |
| Journal replays | 4; final replay reported no gaps/truncation |
| Deduplicated timing observations | 29 across retained sessions |
| BLE reconnects | 0 |
| Final live estimate | [−509,889, −506,943] ms, half-width 1,473 ms |
| Completed-session reference | −508,036 ms, measured 9.8 s after the estimate reference |
| Latest interval score | `reference_contained`; midpoint delta −391 ms |
| Session-reference score | `reference_contained`; midpoint delta −380 ms |
| Fresh delay hull | [−2,904, +5,867] ms over 15 contemporaneous observations |
| Delay outcomes | 29/29 compatible with zero; none prove a nonzero delay |

This is strong coverage evidence for the current model on this session, but it
is not yet enough to certify `assumed_immediate`/`assumed_zero`. The measured
delay intervals are still broad enough to admit substantial positive delay, and
the run is only one post-tooling session. H2-style multi-session coverage and
held-out bounds remain open.

The run exposed a separate correctness bug: when a same-day restart selected a
new beacon, old create/periodic events were re-evaluated against the new
filename origin. That yielded an invalid creation interval, set
`source_disagreement`, and prevented a fresh provisional SOT from replacing a
record that had been marked stale. A filename-origin generation was added to
each event so only same-generation events feed filename candidates. Host tests
and the ESP32-S3 build pass; the fix was not flashed in this task.

## 10. Proposed implementation sequence

1. Correct host anchor provenance and improve qualification/SOT explanations.
   Done for collection/offline mapping; on-device anchor acceptance remains
   intentionally limited by source quality.
2. Upgrade passive capture/analyze to retain GET_DRIFT evidence and independent
   bounded references, including session-close timing metadata. Done.
3. Add minimal replay/domain/scope diagnostics needed to prevent evidence loss.
   The filename-generation fix is implemented; cursor replay and explicit SOT
   blocker fields remain open.
4. Flash the corrected firmware only with explicit authorization, then repeat
   ordinary-session collection and held-out scoring.
5. Only after review install characterized semantics, bump model identity, and
   verify compatible qualified SOT behavior. No width-threshold relaxation.
