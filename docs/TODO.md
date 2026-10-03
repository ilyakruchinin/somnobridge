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
