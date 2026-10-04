# TODO

## SOT replacement logic reports stale drift values indefinitely — RESOLVED (update6)

The persisted source-of-truth (SOT) record selection logic was wrong for a
time-varying quantity like AS11 clock drift. A record persisted on one day
could keep being reported as current on much later days, even though the
true drift had long since moved.

### Resolution (implemented)

The user-approved policy from `docs/implmenetation-update6-spec.md` §§8–12
is implemented:

1. **Frozen interval → read-time effective view.** The old verbatim-record
   serialization is replaced by `drift_select()`/`drift_sel_json()`: the API
   reports the measured interval, the effective (allowance-adjusted)
   interval, measurement reference age, and the selection reason. The
   previously dead `drift_sot_apply()` elapsed-time widening had a µs/ms
   unit bug that is fixed; the selector performs equivalent transport.

2. **No age expiry → explicit 36 h expiry + fixed allowance.** The new
   completed-session fallback record (`drift_fb` NVS blob) expires
   36 hours after its measurement reference, evaluated on read — never by
   rewriting flash. While eligible it is used only at
   `measured_half + 1000 ms` (DRIFT_FB_ALLOW_MS), never at its stored
   width. Unknown age after reboot (freshness_unknown) blocks use until an
   authenticated current map exists.

3. **Width-only replacement → latest-session replacement + read-time
   selection.** `drift_finalize_fallback()` runs once on the confirmed
   IN→OUT transition: when the session's final estimate is usable within
   ±2000 ms (after transport widening), it replaces the stored record
   unconditionally — even when wider than the previous one. Below the gate
   the old record is kept with its age unrefreshed. The 250 ms improvement
   hysteresis no longer decides what is reported: `drift_select()` prefers
   a fresh current-session candidate, then a materially-better eligible
   fallback (≥500 ms advantage after allowance), then a merely-usable
   current. Legacy `drift_sot_p`/`drift_sot_q` slots remain as historical
   diagnostics only (`historical:true, applicable:false` in schema 5).

Also resolved in the same change: the 96-event estimator saturation
(rolling 32-event evidence buffer with pinned session-start `ev0`),
sustained REFINE2 phase tracking instead of coarsening to MAINTAIN, and
removal of the cumulative 8 h request ledger/NVS checkpoint that could
suspend observation.

### Verification status

- `scripts/host_test.c` `test_update6` covers: rolling eviction past the
  old cap, fallback restore/expiry/allowance, current-vs-fallback margin
  selection, wider-final replacement, below-gate keep-old, inclusive
  180 s live and 240 s finalization boundaries, future-reference rejection,
  unchanged fallback on rejection, elapsed-time width widening,
  model-incompatible record non-application, and corrupt-blob invalidation.
- Host tests also cover recovery-reason clearing and interrupted/overwritten
  journal replays. `./scripts/run_host_tests.sh`: passes.
- `python3 scripts/validate_timing.py self-test`: passes, including replay
  completion, byte-count/SHA-256, and malformed-record rejection.
- `python3 scripts/ble_query.py --self-test`: passes; no device connection.
- `./scripts/idf.sh build`: clean, app size `0x12fe90`, 70% partition free.
- **Overnight hardware evidence collected** (2026-10-04 70-min +
  2026-10-05 6.5-h captures): sustained REFINE2 with graceful
  reacquisition, rolling evidence to 360 events, fallback commit +
  post-OUT selection, and 1163/1164 interval containment vs AS11
  `GetDateTime`. Final HTTP counters reported 16,947 samples and zero
  failures; median service was 204 ms and observed backoff was zero.
  These are since-boot counters, not a capture-only request rate.
- **Decision:** best-effort/provisional accuracy is accepted for this
  deployment. Qualified-tier accuracy is not a release requirement; quality
  labels and qualification thresholds remain truthful and unchanged.
- **Still open:** hardware verification of the strengthened fixes below,
  next-session fallback use, and restore across a reboot. Ten-minute
  host-anchor cadence remains client-supplied (`--anchor-interval 600`).

# NEXT

Findings from the 2026-10-04 70-minute live capture
(`/tmp/somnobridge-observe-20261004-0139.jsonl`, update6 firmware):

## FIX: card-identity probe burns persisted records (drift.c)

The hourly `JOB_VERSION` probe calls `drift_card_fw_note()`
(`components/scheduler/sched.c:614-618`), which runs
`sot_mark_stale(false)` whenever the `/client?command=version` response
string differs from the stored `s_card_fw`
(`components/drift/drift.c:1758-1784`). Journal evidence shows the
response string is **unstable across probes**:

- Pre-update6 boot journal: `sot/stale` events at t≈22 h, 23 h, 24 h,
  25 h uptime — exactly 3600 s apart, i.e. one invalidation per hourly
  probe for at least 4 consecutive hours.
- Update6 capture: one `sot/stale` at device t=3607.76 s (the first
  hourly re-probe after the boot-time fetch), then `promote` 300 ms
  later on the next eval — self-healed, and `s_fb` was empty anyway.

Impact under update6: `sot_mark_stale` also invalidates `drift_fb`
(`drift.c:1759-1763`), so a transient version-response anomaly mid
session can destroy a valid committed previous-session fallback — the
exact record the user relies on next day. Two plausible triggers:
empty body on an HTTP-200 response (`ezs_client_version` returns
`buf[0]='\0'` with `ESP_OK` when `r<=0`, and `"" != stored`), or a
genuinely varying response field.

Recommended fix (simplest first):

1. Treat an empty version body as "unknown" — do not call
   `drift_card_fw_note("")`; alternatively have `drift_card_fw_note`
   ignore `fw[0]=='\0'`.
2. Require N (e.g. 2–3) consecutive differing probes before marking
   stale; a single differing probe is diagnostic-only.
3. If the response contains a genuinely varying field, normalize to a
   stable fingerprint (e.g. first token / fixed prefix) before compare
   — inspect a few raw responses first to pick the rule.
4. Observability: journal the note event with the new string (or its
   hash) so future captures can see what actually changed; consider
   exposing `card_fw` in `GET_STATUS`/`GET_DRIFT`.

Regression test in `scripts/host_test.c`: empty-probe does not
invalidate; single differing probe does not invalidate; N consecutive
differing probes do.

## VERIFY: session-end finalize + next-day fallback path — PARTLY VERIFIED

Originally flagged because the 70-minute capture ended while therapy
was still IN. Confirmed live in the 6.5-hour overnight capture
(`/tmp/somnobridge-observe-20261004-2348.jsonl`, 8742 records):

- Session 1 (toilet-break pause, ~51 min): finalize ran at OUT and
  correctly **declined** — journal `fallback/ref_stale age_us=195571274`,
  newest evidence was 196 s old > the 180 s freshness gate (§12.6).
- Session 2 (main night, ~5.3 h): `drift_fb` committed on OUT —
  `persisted_seq:1`, `model_rev:5`, `clock_drift_ms:−509096`,
  `half:1504`, `stale:false`, `fallback_eligible:true`.
- Post-session: `selected_view:"previous_session_fallback"`,
  `reason:"no_current_candidate"`, `adjusted_half_ms:2504` — the
  completed-session correction is being served as designed.

The confirmed result is save plus post-OUT fallback selection, not yet use
in the next therapy session. Remaining for full closure: observe early
fallback use in a resumed/new session and restore across a reboot. Neither
has been established by the overnight capture alone.

## VERIFY: evidence accumulation past the old 96-event cap — VERIFIED

`evidence_total:360`, `evicted:328`, `n_windows` pinned at 32 across
a 6.5 h session — 3.75× past the old 96-event stop. Estimate stayed
valid through every eviction; zero `diff_overflow`/`clock_step` flags.

## DECIDED: best-effort accuracy is sufficient

The maintainer accepts best-effort/provisional accuracy for this deployment.
No better host clock or relaxed qualification threshold is required.
Anchors remain `usable:true, qualified_source:false` when the chrony bound
(~±150 ms in these captures) exceeds 100 ms; the estimate must continue to
report `provisional`/`approximate`, not claim qualified accuracy. The fixed
fallback allowance and expiry remain operating assumptions, not certified
RTC bounds. Qualified-tier characterization is optional future work.

## WATCH: estimate midpoint skew — recurred, worth one more night

- Night 1 (70 min): median residual −148 ms vs AS11 `GetDateTime`.
- Night 2 (6.5 h): median residual **+5 ms**, σ=186 ms, but a
  systematic −200…−330 ms bias developed after the ~04:22 file
  rotation (beacon swap to a different data file). Hypothesis: per-file
  write→visible latency differences shift the bracket mid.
  Recommendation: keep watching; if the bias tracks specific beacon
  files, consider per-beacon bracket calibration or documenting the
  bias bound. No code change yet.

## NIT: `phase.reacq_reason` label — FIXED

`"day_change"` persisted as the reason string for the entire session
after the initial acquisition — the last reason was never cleared on
successful transitions. Fixed in `phase_obs` (clears on an accepted
bracket); see "NIT: `phase.reacq_reason` label — IMPLEMENTED / HOST-VERIFIED"
below for semantics and regression coverage.

## Appendix — revised recommendation: ignore EZShare version for timing validity

This appendix supersedes the version-probe fix recommendations above;
existing content is retained as requested. No firmware changes have been
made for this recommendation.

### Why ignoring it is the simplest correct fix

Given the maintainer's hardware constraint that released EZShare cards have
no manufacturer-supported firmware-update mechanism, periodic version checks
provide no useful update detection. More importantly, the reported firmware
version is neither a unique card identity nor an identity of the AirSense RTC
whose offset we are estimating. Two cards can report the same version, and a
card replacement need not change the AirSense clock. A differing response
therefore does not justify discarding the previous-session clock correction.

**Recommendation: make EZShare version text diagnostic-only. Remove all
version-string comparisons that invalidate timing records, and stop automatic
hourly version polling.** Do not replace this with mismatch counters, prefix
normalization, or a new fingerprinting subsystem. Those would make an
irrelevant signal more complicated without establishing clock identity.

### Corrections to the earlier diagnosis

- `drift_card_fw_note()` already returns immediately for NULL or empty input.
  The proposed empty-response guard is already present; an empty version body
  does not explain invalidation through this function in the inspected code.
- The journal's hourly stale events make the version probe a strong suspect,
  but raw version responses and caller-specific invalidation reasons were not
  captured. The capture alone does not prove that the card changes its version
  string or establish the exact invalidation trigger.
- There is also a concrete comparison hazard: `JOB_VERSION` passes a buffer of
  up to 63 response characters, whereas `s_card_fw` stores only 23 characters.
  `drift_card_fw_note()` compares the stored truncated string against the full
  incoming response. A stable response longer than 23 characters can therefore
  look different on every probe. Persisted `card_fw` comparisons have the same
  risk when their stored value is truncated. This is a plausible software
  explanation, not a confirmed explanation of this capture.

These corrections strengthen the case for removing the timing dependency,
not for trying to infer a reliable identity from the response body.

### Implementation handoff

1. In `components/drift/drift.c`, make `drift_card_fw_note()` a bounded
   diagnostic-text update only. Remove both the direct SOT/fallback mismatch
   invalidation loops and the `changed` → `sot_mark_stale(false)` path. Version
   input must not change `stale`, `applicable`, freshness, selection, or timing
   persistence dirty flags. Retaining the existing function and `card_fw`
   fields avoids unnecessary API and NVS-layout churn.
2. In `components/scheduler/sched.c`, remove recurring hourly `JOB_VERSION`
   submission. If a version read is useful for support, retain a one-time
   diagnostic read on association or an explicitly requested diagnostic read;
   neither may gate timing validity or polling. Do not add a new command just
   to support this fix.
3. Keep existing blob layouts and CRC/version validation. Stored version text
   may remain historical diagnostic metadata, but must not participate in
   timing compatibility or fallback eligibility. Do not automatically clear
   an already persisted stale flag: its original cause may be unknown. A new
   good completed session can replace an invalidated fallback normally.
4. Preserve genuine timing invalidation: detected clock discontinuities,
   incompatible timing model/profile, relevant timezone interpretation
   changes, and explicit reset/reconfiguration behavior. Preserve transport,
   catalog, and Range-capability recovery rules; ignoring version for timing
   does not grant trust to a different network peer or prove card continuity.
5. Before implementation, reconcile the authoritative specification's
   version/firmware-binding wording with this approved policy. Review
   `docs/implementation-spec.md` §§5, 6, and 8 and any related identity checks;
   do not silently broaden this change to unrelated transfer safeguards.

### Regression and hardware verification

- Add host tests with valid SOT and completed-session fallback records:
  identical, empty, NULL, differing, alternating, and long (>23-character)
  version inputs must leave timing validity and fallback selection unchanged.
  Repeated long identical responses must not cause stale transitions or
  timing-record writes. Include restored-record behavior after a fresh anchor.
- Verify that real clock-step/model/timezone/reset invalidation still works;
  no test should expect N repeated version mismatches to invalidate timing.
- Verify recurring housekeeping no longer enqueues version probes, while
  REFINE2, normal listing cadence, and service/backoff guards remain intact.
- Run host tests, timing self-test, and the ESP-IDF build. Then collect hardware
  evidence spanning an hourly boundary with an eligible completed-session
  fallback: confirm no version-induced stale event or fallback loss. Include
  therapy OUT and the following session to verify save and fallback selection.

**Trade-off:** firmware version will no longer pretend to detect card swaps.
It never reliably did so. Unexpected replacement of the AirSense itself or an
unobserved RTC adjustment can still invalidate yesterday's offset without an
observable version change. Keep the existing age/allowance limits and prefer
fresh session evidence; use explicit reset/reconfiguration for known device
replacement rather than inventing a new automatic identity heuristic.

### Status — implemented

- `drift_card_fw_note()` (`components/drift/drift.c`) is now a bounded
  diagnostic-text store: the SOT/fallback mismatch invalidation loops and
  the `changed` → `sot_mark_stale(false)` path are removed. Version input
  can no longer alter `stale`, `applicable`, freshness, selection, or dirty
  flags.
- The recurring hourly `JOB_VERSION` submission is removed with
  `plan_neigh` (`components/scheduler/sched.c`); `JOB_VERSION`,
  `ezs_client_version`, and the note path remain for explicit diagnostic
  use and never gate timing. Version-fetch failures also stop feeding
  service/backoff stats.
- Stored `card_fw` fields and blob layouts are unchanged — now provenance
  only. Genuine invalidation (clock steps, model/profile mismatch,
  timezone interpretation change, explicit reset) is untouched.
- Host-test regression in `test_update6`: differing, alternating, empty,
  NULL, and >23-character version inputs leave the committed fallback and
  selection untouched; the latest string is kept as provenance.
- Verification: `scripts/run_host_tests.sh` all pass;
  `validate_timing.py self-test` passes; `./scripts/idf.sh build` clean.
- **Hardware evidence (2026-10-05 capture):** no `sot/stale` events were
  found in collected replays (old firmware showed ~1/h), and a committed
  fallback was eligible after OUT. Replay history was incomplete, so absence
  of an event is not proof of complete event coverage. The code and host
  regression establish removal of version-based invalidation; an eligible
  fallback surviving into the following session remains a hardware check.

## Accuracy outcomes (both captures)

| Metric | Night 1 (70 min) | Night 2 (6.5 h) |
|---|---|---|
| Interval containment vs AS11 `GetDateTime` | 171/171 (100%) | 1163/1164 (99.9%) |
| Midpoint residual | median −148 ms | median +5 ms, σ=186 ms |
| SomnoTrace `measured_stop` agreement | ~21 ms | ~0.2–0.3 s across 3 sessions |
| Stated half-width (conservative) | ±1.36 s | ±1.27…1.74 s |
| AS11 RTC drift measured | +3 ms (flat) | **−76 ms / 6.5 h ≈ 3 ppm** |
| Tier | provisional / best-effort | provisional / best-effort |

Note: the single non-containment was an 84 ms lower-bound shortfall with
partial interval overlap. It was not full containment and must not be
counted as such; the capture does not establish absolute clock accuracy.
A side-effect of the +508 s offset: filenames rolled into "tomorrow" (`20261005_000019_BRP.edf` created at
23:51 UTC Oct 4) — exactly why the correction matters.

## FOLLOW-UP: session-end save freshness margin — IMPLEMENTED / HOST-VERIFIED

Live selection remains limited to 180 s. Confirmed-OUT finalization uses
its own inclusive 240 s limit, allowing more decision latency without
retimestamping the measurement. Future references and references beyond
240 s are rejected; the effective ±2000 ms width gate, original timestamp,
36 h expiry, and fixed fallback allowance remain unchanged.

The earlier ~215 s normal-end estimate is not a proven maximum. The 240 s
limit is an accepted best-effort trade-off: it admits an additional minute
of evidence age and cannot detect an unobserved RTC step during that gap.
The historical 196 s decline followed the old policy; it is not proof that
all similarly aged evidence is safe under every circumstance.

Host regressions cover exactly 180/240 s and 1 µs beyond, future references,
original reference preservation, unchanged record/dirty state on age
rejection, and exactly 2000 ms effective width versus 2001 ms after elapsed
widening. A new real session-end capture is still needed.

## FIX: journal replay integrity — IMPLEMENTED / HOST-VERIFIED

The shorter replay interval was not a root-cause fix. `GET_EVIDENCE` streams
the entire retained committed journal oldest-first, not just new events.
The overnight replays all reported completion, but coverage varied and
some contained malformed records; ordinary ring eviction alone was not
established as the cause.

Implemented without a new protocol, flash layout, or larger ring:

- `jrn_walk()` returns failure when its consumer stops early. The existing
  BLE completion path therefore reports failed transport rather than
  falsely declaring a partial replay complete. A regression reproduced
  the old success-on-interruption bug before the fix.
- Replay rechecks the indexed chunk sequence before emission. A slot
  overwritten during replay is reported as a gap, not emitted as another
  generation's chunk. CRC and payload-length checks remain enforced.
- The collector verifies stream completion, byte count, SHA-256, UTF-8,
  and JSON-object records. Invalid replays retain diagnostic metadata but
  no observations, and are reported through the existing observer-error
  path. Valid transferred subsets with reported gaps/truncation have
  `integrity_valid:true, complete:false`; they cannot prove event absence.
- The BLE client sends ABORT when an opened replay times out or raises,
  and ignores foreign-request or late byte frames. This prevents a timeout
  from leaving the credit-waiting stream active indefinitely when abort
  delivery succeeds. Failed abort delivery remains visible in verbose logs.
- Default replay cadence is restored to 600 s. Each replay records elapsed
  time; choose any tighter cadence only after measuring download cost and
  retention. No immediate retry loop or incremental replay subsystem added.

Host tests cover interrupted consumers, recovery on a later replay,
slot replacement during replay, completion/byte-count/hash failures,
malformed JSON/UTF-8, empty journals, gap/truncation flags, BLE timeout
abort, credit failure, and stale/late frames.

`complete` describes the transferred retained committed journal, not all
session history: evicted records, the uncommitted RAM tail, and events
written after replay indexing may be absent. `persisted_seq` plus dirty
state remains the evidence for a durable fallback commit. The fixes do
not establish the exact cause of every missing event in the old capture.

## NIT: `phase.reacq_reason` label — IMPLEMENTED / HOST-VERIFIED

The reason now denotes recovery pending until an enabled model accepts a
bracket, not the historical last reason and not completion of the entire
refinement ladder. Accepted recovery clears it to `none`; unchanged/wide
observations do not. A disabled model keeps its `disabled` reason even if
it accepts a bracket; recovery after re-enabling clears it normally.

Direct regressions cover miss, beacon/day changes, unchanged observations,
wide brackets, disabled-model visibility, and re-enabled recovery. No
polling schedule or estimate-validity gate is changed by this field.

## NEXT: hardware verification of strengthened follow-ups

1. Flash the new build when requested; these changes are built but have
   not been flashed or tested live in this follow-up.
2. Capture therapy OUT and a resumed/new session: verify final save,
   unchanged true reference age, early fallback use, then preference for
   fresh current evidence. Also verify restored fallback after reboot and
   an authenticated anchor.
3. Exercise journal replay success and timeout/link loss: verify truthful
   completion, observer rejection of invalid payloads, and successful
   later requests after abort. Measure replay elapsed time and snapshot
   gaps before deciding whether 600 s should change.
4. Continue observing midpoint skew; no per-file calibration from one
   night's bias. Keep best-effort labels and do not require qualified-tier
   accuracy for this deployment.

Verification: host tests, timing self-test, BLE stream self-test, and
ESP-IDF build pass; `git diff --check` is clean. App size is `0x12fe90`
with 70% partition free. No commit, push, or flash performed.

## FOLLOW-UP: catalog reinitialization leak — FIXED / HOST-VERIFIED

The earlier sanitizer finding (2584 bytes in 14 leaked allocations) came
from `cat_init()` clearing folder pointers without releasing their arrays.
The same path is used by the portal's rebuild-catalog action.

Implemented in `components/catalog/catalog.c`:

- Reuse the existing mutex and `free_files()` helper to release every
  folder's resident allocation before clearing catalog state. Cleanup and
  reset are one locked operation; empty/repeated resets are safe.
- Account new file arrays only when they are published under the mutex.
  Scratch preparation outside the lock no longer updates resident PSRAM
  usage, so a rebuild cannot reset the counter underneath an in-flight feed.
  Failed publication frees its scratch array without changing accounting.
- Protect the scalar state/count/PSRAM queries with the same mutex. Existing
  file/list queries already copy under that mutex, so caller-owned snapshots
  remain valid after reset. Separate query calls are not a single combined
  atomic status snapshot.

The portal caller, catalog API, PSRAM budget, timing policy, and polling
schedule are unchanged. A feed completing after reset may repopulate the
catalog, as expected for a rebuild; no new generation/identity subsystem is
introduced.

Regression and verification:

- A new 64-cycle populate/reset test reproduced the leak before the fix
  under LeakSanitizer; it now passes. It covers multiple resident folders,
  an empty folder, filtered directory entries, stale/hot state, accounting,
  idempotent empty resets, and preservation of copied file snapshots.
- Host catalog locking can use a real pthread mutex via `CAT_HOST_THREADS`;
  the standard runner enables it with `-pthread`. A stress test runs 10,000
  resets concurrently with 10,000 feeds and 10,000 reader iterations, then
  confirms that a final reset leaves no resident allocation.
- Full host suite passes AddressSanitizer/UndefinedBehaviorSanitizer with
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1`. No leak suppression or disabled detection.
- Standard host tests, timing self-test, BLE stream self-test, ESP-IDF build,
  and `git diff --check` pass. Latest app size is `0x12ff80`, 70% free.

Remaining hardware check: after flashing when requested, repeat portal
catalog rebuilds while listing/monitoring and confirm stable PSRAM and
continued catalog reconstruction. Host stress tests do not replace this
on-device check. No flash, commit, or push performed.
