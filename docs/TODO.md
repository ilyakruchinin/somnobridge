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
- **Still open:** overnight hardware evidence (§6 of the update6 spec) and
  measured-load characterization for sustained REFINE2 (§13.5) have not
  been collected; ten-minute host-anchor cadence depends on the client
  supplying anchors (firmware side is ready, `validate_timing.py observe`
  defaults to 600 s).

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

## VERIFY: session-end finalize + next-day fallback path (untested live)

The capture ended while therapy was still IN (`state:1`), so
`drift_finalize_fallback()`, the `drift_fb` NVS commit, and the
next-session `drift_select()` fallback branch have never run on
hardware — only in host tests. A full-night `observe` run that spans
the IN→OUT transition (and ideally the following session) is needed to
confirm: fb commit on OUT, next-session eligibility, allowance
application, and `selected_view:"previous_session_fallback"`.

## VERIFY: evidence accumulation past the old 96-event cap

The run reached `evidence_total:59` (`evicted:27`, `n_windows` pinned
32 — rolling eviction proven at the 32-boundary). >96-event sustained
collection needs the same full-night capture as above.

## MEASURE: qualified accuracy still unproven (host bound)

Anchor exchanges all returned `usable:true, qualified_source:false`
because the host chrony bound (~±150 ms WAN NTP) exceeds the 100 ms
qualification gate. The estimate correctly stayed
`provisional`/`approximate` all run. Independent containment evidence
is strong (171/171 AS11 `GetDateTime` references inside the firmware
interval, median midpoint residual −148 ms), but qualified-tier
reporting needs a ≤100 ms host clock source (LAN NTP / PTP / GPS) or
acceptance of best-effort for this deployment.

## WATCH: estimate midpoint skew

Median residual vs AS11 truth was −148 ms (interval skews slightly
early). Well inside the stated ±1.3–1.4 s width and 100% containment,
but if the same direction/magnitude recurs across nights it is a
systematic bracket asymmetry worth understanding — no change now.

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
- **Still open:** hardware capture spanning an hourly boundary with an
  eligible committed fallback (no version-induced stale), plus the
  session-end finalize/next-day fallback verification listed above.
