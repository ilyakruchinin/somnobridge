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
  selection, wider-final replacement, below-gate keep-old, 180 s stale
  reference rejection, model-incompatible record non-application, and
  corrupt-blob invalidation.
- `python3 scripts/validate_timing.py self-test`: passes.
- `./scripts/idf.sh build`: clean.
- **Overnight hardware evidence collected** (2026-10-04 70-min +
  2026-10-05 6.5-h captures): sustained REFINE2 with graceful
  reacquisition, rolling evidence to 360 events, fallback commit +
  next-session selection, and 1163/1164 interval containment vs AS11
  `GetDateTime`. Sustained-REFINE2 load measured: 16,947 requests /
  0 failures / 0 backoff over ~8 h (~9 req/min, median service 204 ms).
- **Still open:** ten-minute host-anchor cadence depends on the client
  supplying anchors (firmware side is ready, `validate_timing.py observe`
  defaults to 600 s); qualified-tier accuracy needs a ≤100 ms host
  clock source.

# NEXT

Findings from the 2026-10-04 70-minute live capture
(`/tmp/somnopal-observe-20261004-0139.jsonl`, update6 firmware):

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

## VERIFY: session-end finalize + next-day fallback path — VERIFIED (2026-10-05 capture)

Originally flagged because the 70-minute capture ended while therapy
was still IN. Confirmed live in the 6.5-hour overnight capture
(`/tmp/somnopal-observe-20261004-2348.jsonl`, 8742 records):

- Session 1 (toilet-break pause, ~51 min): finalize ran at OUT and
  correctly **declined** — journal `fallback/ref_stale age_us=195571274`,
  newest evidence was 196 s old > the 180 s freshness gate (§12.6).
- Session 2 (main night, ~5.3 h): `drift_fb` committed on OUT —
  `persisted_seq:1`, `model_rev:5`, `clock_drift_ms:−509096`,
  `half:1504`, `stale:false`, `fallback_eligible:true`.
- Post-session: `selected_view:"previous_session_fallback"`,
  `reason:"no_current_candidate"`, `adjusted_half_ms:2504` — the
  completed-session correction is being served as designed.

Remaining for full closure: observe the *next session's* early
fallback use (record is committed in NVS — tonight's session is the
first real next-day exercise), and confirm restored-record behavior
across a reboot.

## VERIFY: evidence accumulation past the old 96-event cap — VERIFIED

`evidence_total:360`, `evicted:328`, `n_windows` pinned at 32 across
a 6.5 h session — 3.75× past the old 96-event stop. Estimate stayed
valid through every eviction; zero `diff_overflow`/`clock_step` flags.

## MEASURE: qualified accuracy still unproven (host bound)

Anchor exchanges all returned `usable:true, qualified_source:false`
because the host chrony bound (~±150 ms WAN NTP) exceeds the 100 ms
qualification gate. The estimate correctly stayed
`provisional`/`approximate` in both captures. Independent containment
evidence is now very strong — see "Accuracy outcomes" below — but
qualified-tier reporting still needs a ≤100 ms host clock source
(LAN NTP / PTP / GPS) or explicit acceptance of best-effort for this
deployment.

## WATCH: estimate midpoint skew — recurred, worth one more night

- Night 1 (70 min): median residual −148 ms vs AS11 `GetDateTime`.
- Night 2 (6.5 h): median residual **+5 ms**, σ=186 ms, but a
  systematic −200…−330 ms bias developed after the ~04:22 file
  rotation (beacon swap to a different data file). Hypothesis: per-file
  write→visible latency differences shift the bracket mid.
  Recommendation: keep watching; if the bias tracks specific beacon
  files, consider per-beacon bracket calibration or documenting the
  bias bound. No code change yet.

## NIT: `phase.reacq_reason` label

`"day_change"` persisted as the reason string for the entire session
after the initial acquisition — the last reason is never cleared on
successful transitions. Cosmetic; clear or annotate it when the phase
state is next touched.

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
- **Hardware-verified (2026-10-05 capture):** zero `sot/stale` journal
  events across the whole night (old firmware showed ~1/h); the
  committed `drift_fb` record survived the session and is eligible.
  The version-invalidation source is eliminated.

## Accuracy outcomes (both captures)

| Metric | Night 1 (70 min) | Night 2 (6.5 h) |
|---|---|---|
| Interval containment vs AS11 `GetDateTime` | 171/171 (100%) | 1163/1164 (99.9%) |
| Midpoint residual | median −148 ms | median +5 ms, σ=186 ms |
| SomnoTrace `measured_stop` agreement | ~21 ms | ~0.2–0.3 s across 3 sessions |
| Stated half-width (conservative) | ±1.36 s | ±1.27…1.74 s |
| AS11 RTC drift measured | +3 ms (flat) | **−76 ms / 6.5 h ≈ 3 ppm** |
| Tier | provisional / best-effort | provisional / best-effort |

Note: the single containment miss was an 84 ms lower-bound shortfall —
inside the reference's own ±150 ms uncertainty, not a demonstrated
estimator failure. A side-effect of the +508 s offset: tonight's
filenames rolled into "tomorrow" (`20261005_000019_BRP.edf` created at
23:51 UTC Oct 4) — exactly why the correction matters.

## FOLLOW-UP: session-end save freshness margin is thin

The 180 s reference-freshness gate (`DRIFT_CUR_REF_MAX_US`) interacts
with the 125 s OUT timeout: a session whose final observed flush lands
>180 s before finalize declines the save (`ref_stale`). Session 1
tonight declined for exactly this reason. Recommendation: keep the
gate, but consider measuring freshness against the newest *observed*
evidence rather than finalize-call time, or relaxing slightly
(e.g. 240 s) — pauses that end sessions commonly land in the
125–200 s band. Watch save/decline ratio across more nights before
changing.

## NIT: journal replay is a lossy window

Each replay returns only a subset of the bounded ring; post-hoc the
`fallback/save` event line wasn't captured (the `persisted_seq:1`
record proves the commit regardless). Acceptable — but don't rely on
journal replay for complete long-session event history; increase
replay cadence if finer granularity is needed.
