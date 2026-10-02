# TODO

## SOT replacement logic reports stale drift values indefinitely

The persisted source-of-truth (SOT) record selection logic is wrong for a
time-varying quantity like AS11 clock drift. A record persisted on one day can
keep being reported as current on much later days, even though the true drift
has long since moved.

### Defects

1. **Reported interval is frozen at promotion time.** `drift_sot_apply()`
   (which re-references a stored bound to "now" by widening it with
   `elapsed × dev_rate_ppm`) is never called in the live path — only in host
   tests. `op_get_drift` serializes the stored record verbatim, so
   `interval_ms` and `current_half_ms` reflect the promotion instant, not the
   read instant. See `components/drift/drift.c` (`drift_sot_apply`,
   `drift_sot_json`, `sot_fill`) and `components/ble_api/ble_ops.c`
   (`op_get_drift`).

2. **No age-based expiry.** Records are marked stale only on clock
   discontinuity, model-revision mismatch, card firmware change, or explicit
   invalidation. Nothing ages a record out. A record from day 14 is still
   `applicable=true, stale=false` on day 56.

3. **Newer evidence cannot refresh the record.** `sot_offer()` replaces the
   stored record only when the slot is empty/stale, the new interval is
   disjoint (non-overlapping), or the new half-width is ≥250 ms tighter.
   An overlapping, slightly-wider or equal-width estimate from a *newer*
   session is rejected — so `reference_at_ms`/`computed_at_ms` stay old even
   when fresh measurements exist. The disjoint rule only rescues monotonic
   drift once the new interval fully stops overlapping; slowly-drifting
   estimates that keep partially overlapping may never displace the stale
   record. Host tests currently assert this behavior ("same-width re-eval does
   not supersede", "older eval never displaces SOT",
   `scripts/host_test.c` ~line 983).

### Consequence

`best_available_offset` / `provisional_source_of_truth` can return a drift
bound measured days or weeks ago as if it were current, while a newer valid
session estimate exists. Freshness of evidence is not considered — only
interval width and overlap.

### Direction

Decision deferred — proposed remedies were rejected. To be decided how
freshness should factor into replacement, whether read-time widening should be
exposed, and whether/how records should age out. Any change to replacement
semantics requires a `model_rev` bump and host-test updates.
