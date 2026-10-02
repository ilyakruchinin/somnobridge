/*
 * SomnoPal — time anchors, monotonic→UTC mapping, and drift records
 * (spec §8, §8.5; update spec U3–U7)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Primary estimator: timestamp-first interval intersection (U4). File
 * timestamps on the card listing are the AS11 clock rendered UTC-shaped
 * and quantized to FAT's 2 s units; each stamp transition brackets the
 * offset in true time. The comb fitter + EDF header survive only as an
 * optional diagnostic profile — no runtime path reads a file body.
 *
 * Pure-ish module: callers pass monotonic microseconds (esp_timer on
 * target, synthetic time in host tests). No wall-clock dependence, no
 * esp_timer calls inside — deadlines belong to the caller.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ezs_parse.h"   /* ezs_entry_t */
#include "catalog.h"     /* cat_diff_t  */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- anchors (§8.1, §10.4) ---------------- */

#define DRIFT_ANCHORS_MAX      24
#define DRIFT_RTT_MAX_US       250000      /* reject slower exchanges   */
#define DRIFT_SRC_UNC_MAX_US   100000      /* reject worse source unc.  */
#define DRIFT_STAMP_ALLOW_US   5000        /* provisional, §8.1         */
#define DRIFT_EXTRAP_PPM       40.0        /* outside-bracket bound     */
#define DRIFT_NO_ANCHOR_US     (2LL*3600*1000000LL) /* ±2 h evidence    */

typedef struct {
    int64_t  mono_us;
    int64_t  utc_us;
    uint32_t uncertainty_us;
    uint32_t source_epoch;
} drift_anchor_t;

typedef enum {
    ANCHOR_OK = 0,
    ANCHOR_RTT,           /* rtt > 250 ms or negative                 */
    ANCHOR_UNCERTAIN,     /* source uncertainty > 100 ms              */
    ANCHOR_STALE,         /* commit arrived >5 s after exchange       */
    ANCHOR_EPOCH,         /* accepted but starts new segment          */
    ANCHOR_DISCONTINUITY, /* inconsistent with prior mapping          */
    ANCHOR_BAD,           /* impossible times / corrupt values        */
} anchor_result_t;

/* One pending exchange, filled at TIME_REQUEST completion and TIME_REPLY
 * submission; committed on TIME_COMMIT.                               */
typedef struct {
    bool     used;
    uint32_t request_id;
    uint64_t t1_utc_us;
    uint32_t src_uncertainty_us;
    uint32_t source_epoch;
    int64_t  m2_us;
    int64_t  m3_us;
    int64_t  deadline_us;          /* commit must arrive by this      */
} drift_pending_t;

void          drift_reset(void);   /* new monotonic domain (boot)      */
esp_err_t     drift_time_begin(uint32_t request_id, uint64_t t1_utc_us,
                               uint32_t src_uncertainty_us,
                               uint32_t source_epoch, int64_t m2_us);
void          drift_time_reply_tsp(uint32_t request_id, int64_t m3_us);
anchor_result_t drift_time_commit(uint32_t request_id, uint64_t t4_utc_us,
                                  int64_t now_us, uint32_t *uncertainty_us);

/* Map monotonic → UTC interval [lo,hi]. Returns false with null output
 * when no bracketing/extrapolating anchor exists (§8.1).               */
bool drift_map_utc(int64_t mono_us, int64_t *utc_lo, int64_t *utc_hi);
int  drift_anchor_count(void);

/* ---------------- characterization profile (U5/update4) ----------
 * Build-time record of what this card combination is allowed to claim.
 * Residual cache/visibility terms are explicit model assumptions until
 * independently characterized; actual listing request intervals are
 * measured separately and are not represented by these bounds. Phase
 * tracking/refinement is scheduler policy, not a profile switch.      */
typedef enum {
    DRIFT_ENC_UNKNOWN = 0,       /* union of truncate/round bins      */
    DRIFT_ENC_TRUNCATED,         /* filename second is [F,F+1 s)      */
    DRIFT_ENC_ROUNDED,           /* filename second is [F-0.5,F+0.5]  */
} drift_filename_enc_t;

/* Explicit semantics for the two residual terms. The listing request
 * interval itself is measured from ESP request start through completed
 * response; these bounds cover only uncertainty not represented by
 * those endpoints.                                                    */
typedef enum {
    DRIFT_CACHE_BOUNDED = 0,      /* nonzero conservative bound       */
    DRIFT_CACHE_ASSUMED_ZERO,     /* scoped operating assumption      */
    DRIFT_CACHE_CHARACTERIZED,    /* independently supported bound    */
} drift_cache_policy_t;

typedef enum {
    DRIFT_VIS_BOUNDED = 0,        /* finite prior/characterized bound */
    DRIFT_VIS_ASSUMED_IMMEDIATE,  /* scoped operating assumption      */
    DRIFT_VIS_CHARACTERIZED,      /* independently supported bound    */
} drift_visibility_policy_t;

typedef struct {
    bool     stamp_channel_validated; /* U5 gate 4 — mtime source     */
    uint8_t  cache_policy;            /* drift_cache_policy_t         */
    uint8_t  visibility_policy;       /* drift_visibility_policy_t    */
    uint32_t cache_bound_us;          /* additional freshness bound C */
    uint32_t jitter_us;               /* separate per-edge allowance  */
    uint32_t latency_lo_us;           /* residual visibility delay    */
    uint32_t latency_hi_us;
    uint32_t dev_rate_ppm;            /* AS11 rate bound — re-refer-  */
                                      /* ences intervals across gaps */
    uint32_t accurate_half_us;        /* certified-tier gate (2 s)    */
    uint32_t provisional_half_us;     /* provisional SOT gate (5 s)   */
    uint32_t cadence_step_us;         /* Δstamp vs Δmono step thresh  */
    uint16_t model_rev;               /* timing model identity        */

    /* Filename candidates are provisional unless their own source
     * model is independently qualified. Zero-width or absent bounds
     * are never silently substituted.                               */
    uint8_t  filename_encoding;       /* drift_filename_enc_t         */
    bool     filename_origin_bounded; /* creation delay D bounded     */
    int64_t  fname_create_lo_us;
    int64_t  fname_create_hi_us;
    bool     filename_periodic_bounded;/* origin adjustment bounded  */
    int64_t  fname_period_lo_us;
    int64_t  fname_period_hi_us;
    bool     filename_create_validated;
    bool     filename_periodic_validated;
} drift_profile_t;

const drift_profile_t *drift_profile(void);
void drift_profile_set(const drift_profile_t *p); /* characterization */

/* ---------------- stamps / beacons ---------------- */

typedef enum {
    DRIFT_BC_NONE = 0,   /* not a timing file                        */
    DRIFT_BC_BRP,        /* sampled beacons, in preference order     */
    DRIFT_BC_PLD,
    DRIFT_BC_SA2,
    DRIFT_BC_ANNOT,      /* CSL/EVE — creation is session-start only */
} drift_beacon_t;

drift_beacon_t drift_beacon_class(const char *name);

/* Parse a card-listing stamp ("2026- 9-30   14: 9:52" — space-padded)
 * into device-domain seconds since 2000-01-01. The stamp is UTC-shaped
 * on the AS11's own clock — not UTC, not AS11-local.                 */
bool drift_stamp_parse(const char *raw, int64_t *dev_s_out);

/* Session filename "YYYYMMDD_HHMMSS_XXX.edf" → ISO claim string
 * "YYYY-MM-DDTHH:MM:SS" (AS11-local wall claim, for display only).  */
bool drift_file_start_iso(const char *name, char *out, size_t cap);
/* Strict filename local-time parser used by update3 candidates. */
bool drift_filename_local(const char *name, int64_t *local_s);

/* ---------------- observation session (U3/U4) ---------------- */

typedef enum {
    EST_PENDING = 0,
    EST_PROVISIONAL,
    EST_VALIDATED,
    EST_INVALID,
} est_state_t;

typedef enum {
    ACC_NONE = 0,
    ACC_APPROXIMATE,       /* bounded but the accurate gates unmet    */
    ACC_ACCURATE,          /* certified ±accurate_half_us tier        */
} acc_tier_t;

/* Session flags (bitfield — serialized as a JSON flag list)          */
#define DF_NO_ANCHOR      (1u<<0)   /* no anchor reached the evidence */
#define DF_OBSERVED_LATE  (1u<<1)   /* no pre-session scan bound      */
#define DF_BACKWARD       (1u<<2)   /* stamp moved backwards → split  */
#define DF_STEP           (1u<<3)   /* Δstamp/Δmono mismatch → split  */
#define DF_CONTRADICTION  (1u<<4)   /* empty intersection → split     */
#define DF_STAMP_BAD      (1u<<5)   /* unparseable/missing stamp      */
#define DF_BEACON_GONE    (1u<<6)   /* beacon vanished mid-session    */
#define DF_EV_OVERFLOW    (1u<<7)   /* diff event cap hit             */
#define DF_UNVALIDATED    (1u<<8)   /* stamp channel not certified    */
#define DF_TZ_UNRESOLVED  (1u<<9)   /* filename timezone/rule unknown */
#define DF_FNAME_ORIGIN   (1u<<10)  /* filename origin bound missing  */
#define DF_CYCLE_AMBIG    (1u<<11)  /* flush cycle index ambiguous    */
#define DF_SOURCE_DISAGREE (1u<<12) /* offset candidates do not meet  */

typedef enum {
    DRIFT_SRC_NONE = 0,
    DRIFT_SRC_MTIME,
    DRIFT_SRC_FNAME_CREATE,
    DRIFT_SRC_FNAME_PERIODIC,
} drift_source_t;

typedef enum {
    DRIFT_EV_TRANSITION = 0, /* beacon stamp/size update              */
    DRIFT_EV_CREATE,         /* beacon appeared (creation window)     */
} drift_ev_kind_t;

/* One stamp observation: the beacon's stamp read `stamp_s` was first
 * visible inside the actual monotonic listing interval [unchanged
 * request start, changed response end]. Residual cache/visibility model
 * terms are applied at evaluation; this record stays raw/recomputable. */
typedef struct {
    int64_t obs_lo_us;    /* actual unchanged request start           */
    int64_t obs_hi_us;    /* actual changed response end              */
    int64_t stamp_s;      /* new listing mtime in device-domain secs  */
    uint8_t kind;
    uint8_t fname_gen;    /* filename-origin generation at feed time  */
    uint8_t pad[6];
} drift_ev_t;

/* Bounded diagnostic ring: one record per estimator event, retaining
 * the actual ESP-side listing endpoints so offline replay can audit the
 * observation interval without mixing in BLE transport time.          */
#define DRIFT_OBS_DIAG_CAP 12
typedef struct {
    uint32_t seq;                /* transition diagnostic sequence    */
    uint32_t unch_seq;           /* unchanged-scan sequence (0 none)  */
    uint32_t chg_seq;            /* changed-scan sequence             */
    int32_t  ev_index;           /* drift_ev index; -1 when capped    */
    uint8_t  kind;               /* drift_ev_kind_t                   */
    int64_t  stamp_s;            /* stamped second (device domain)    */
    int64_t  unch_req_us;        /* unchanged listing request start   */
    int64_t  unch_resp_us;       /* unchanged response end (0 unknown)*/
    int64_t  chg_req_us;         /* changed listing request start     */
    int64_t  chg_resp_us;        /* changed response end              */
} drift_obs_diag_t;

/* Compact published copy with the event's contribution mapped under
 * the active profile.                                                */
#define DRIFT_OBS_PUB_MAX 4
typedef struct {
    uint32_t seq, unch_seq, chg_seq;
    int32_t  ev_index;
    uint8_t  kind;
    bool     mapped;
    int64_t  stamp_s;
    int64_t  unch_req_us, unch_resp_us;
    int64_t  chg_req_us, chg_resp_us;
    int64_t  utc_lo_ms, utc_hi_ms;
    int64_t  offset_lo_ms, offset_hi_ms;
} drift_obs_pub_t;

#define DRIFT_EV_CAP 96

typedef struct {
    uint32_t cache_bound_us;
    uint32_t jitter_us;
    bool     use_profile_bounds;  /* live sessions follow profile      */

    /* binding: one day folder, one sampled beacon                    */
    char     day[9];             /* CAT_DAY_STR                      */
    char     beacon[96];         /* EZS_NAME_MAX; "" = none chosen   */
    int      beacon_prio;        /* lower = better; -1 = none        */

    /* beacon tuple at the last fed scan                            */
    bool     have_tuple;
    uint64_t size_tok;
    bool     stamp_ok;
    int64_t  stamp_s;
    int64_t  prev_scan_us;        /* req_start of the previous feed   */
    int64_t  prev_resp_us;        /* resp_end of the previous feed    */
    uint32_t prev_scan_seq;       /* previous feed sequence           */
    uint32_t scan_seq;            /* feeds for this session/day       */
    uint32_t obs_seq;             /* transition diagnostic sequence   */
    bool     baseline_bounded;    /* a pre-open scan bound existed    */

    drift_obs_diag_t obs_diag[DRIFT_OBS_DIAG_CAP];
    uint8_t  obs_diag_next;
    uint8_t  n_obs_diag;

    drift_ev_t ev[DRIFT_EV_CAP];
    int      n_ev;
    bool     ev_overflow;

    /* filename-origin evidence and per-source candidates            */
    bool     fname_known;
    uint8_t  fname_gen;              /* incremented per beacon reset  */
    int64_t  fname_lo_us, fname_hi_us; /* UTC-shaped device interval  */
    int      n_create_ev;
    int      n_periodic_ev;
    bool     periodic_cycle_valid;
    int64_t  mtime_lo_us, mtime_hi_us;
    int64_t  create_lo_us, create_hi_us;
    int64_t  periodic_lo_us, periodic_hi_us;
    uint8_t  preferred_source;         /* drift_source_t             */
    char     source_reason[48];

    /* cadence diagnostic (stamp deltas) — the published flush period.
     * Phase/cadence prediction for polling lives in the scheduler's
     * phase tracker (update2), not here.                            */
    int      n_deltas;
    int64_t  per_lo_ms, per_hi_ms;
    int64_t  flush_period_ms;

    /* eval outputs (recomputed by drift_sess_eval)                 */
    uint32_t n_segments;
    uint32_t n_splits;
    int      n_windows;           /* contributing windows, cur seg. */
    bool     est_bound_valid;
    int64_t  off_lo_us, off_hi_us;/* offset interval (true−device)  */
    int64_t  ref_mono_us;         /* newest contributing event      */
    bool     anchor_used;
    acc_tier_t tier;
    uint32_t flag_bits;           /* DF_* — feed-time bits persist,  */
                                  /* eval bits recompute each pass   */

    /* optional diagnostics — populated by an opportunistic header
     * prefix parse during ordinary post-therapy downloads (U2).     */
    bool     header_known;
    uint32_t header_bytes;
    uint32_t record_bytes;
    uint32_t record_duration_s;
    int64_t  hdr_start_local_s;
    int64_t  complete_records;
} drift_session_t;

/* ---------------- the one live session ----------------
 * Owned by this component; the card worker writes, BLE ops read.   */
drift_session_t *drift_live(void);       /* NULL when no session      */
drift_session_t *drift_last(void);       /* live OR closed session    */
bool             drift_live_is(void);
bool             drift_last_closed(void);
void             drift_live_begin(const char *day);
void             drift_live_end(int64_t now_us); /* therapy gap / OUT */
/* Reactivate a closed session if the same day folder resumed — keeps
 * retained evidence rather than discarding it (update2 §3.5).       */
bool             drift_live_resume(const char *day);

void drift_sess_begin(drift_session_t *s, uint32_t cache_bound_us,
                      uint32_t jitter_us);

/* Feed one complete, successfully-parsed day-folder listing — every
 * scan of the session's day, changed or not. `diff` carries the
 * catalog's per-file change events (NULL = unknown). Returns the
 * number of estimator events appended. The caller (sched task) also
 * stamps obs times; the estimator tracks prev-scan bounds itself.   */
int drift_feed_scan(drift_session_t *s, const char *day,
                    const ezs_entry_t *entries, int n,
                    const cat_diff_t *diff,
                    int64_t req_start_us, int64_t resp_end_us);

/* Recompute the estimate: intersect all event intervals within the
 * current clock segment, classify state/tier, promote to the SOT when
 * accurate. `now_us` is monotonic now (computed_at reference).       */
void drift_sess_eval(drift_session_t *s, int64_t now_us);

/* Diagnostic header + comb-fit profile (demoted — not the primary
 * estimator; U2). Kept for opportunistic post-therapy use.          */
void drift_sess_header(drift_session_t *s, uint32_t header_bytes,
                       uint32_t record_bytes, uint32_t record_duration_s,
                       int64_t hdr_start_local_s);

/* ---------------- published estimate (U5) + SOT (U6) ---------------- */

typedef struct {
    bool     available;
    uint8_t  source;                /* drift_source_t               */
    int64_t  lo_ms, hi_ms;
    int64_t  half_ms;
    uint16_t observations;
    char     reason[48];
} drift_candidate_t;

typedef struct {
    est_state_t state;
    acc_tier_t  tier;
    uint8_t     quality;            /* none|provisional|qualified   */
    uint8_t     source;             /* selected drift_source_t      */
    char        source_reason[48];
    char        model_id[24];
    uint16_t    model_rev;
    uint8_t     cache_policy;         /* drift_cache_policy_t        */
    uint8_t     visibility_policy;    /* drift_visibility_policy_t   */
    uint32_t    cache_bound_us;
    uint32_t    visibility_lo_us;
    uint32_t    visibility_hi_us;
    uint32_t    jitter_us;
    uint32_t    dev_rate_ppm;
    uint64_t    assumptions;
    bool        observation_closed;
    bool        has_drift;
    int64_t     clock_drift_ms;
    int64_t     drift_lo_ms, drift_hi_ms;    /* offset interval        */
    int64_t     accuracy_half_ms;
    int64_t     computed_at_ms;              /* UTC fit instant        */
    int64_t     reference_at_ms;             /* UTC instant it applies */
    int         n_windows;
    int         n_segments;
    bool        has_period;
    int64_t     flush_period_ms;
    int64_t     period_lo_ms, period_hi_ms;
    bool        has_point;                   /* session start bound    */
    int64_t     session_start_true_ms;
    int64_t     start_lo_ms, start_hi_ms;
    int64_t     complete_records;            /* opportunistic diag     */
    uint32_t    record_bytes;
    uint32_t    record_duration_s;
    drift_candidate_t candidates[3];         /* mtime/create/periodic  */
    char        flags[224];                  /* comma list, mach. codes*/
    char        record_key[24];
    char        beacon[96];
    char        day[9];
    char        session_start_as10[24];
    uint8_t     n_observations;
    drift_obs_pub_t observations[DRIFT_OBS_PUB_MAX];
} drift_record_t;

/* The sched task publishes the evaluated record under a sequence
 * counter; readers (BLE/UI) take a consistent snapshot. False when no
 * session has ever produced a record this boot.                     */
bool drift_pub_get(drift_record_t *out);
int  drift_estimate_json(const drift_record_t *r, char *buf, size_t cap);

/* Source of truth — the newest accurate-tier offset (U6). Carried in
 * RAM, persisted to NVS on the §U6 triggers, loaded at boot.          */
typedef enum {
    DRIFT_SOT_SESSION = 1,
    DRIFT_SOT_NVS     = 2,
} drift_sot_origin_t;

typedef enum {
    DRIFT_Q_NONE = 0,
    DRIFT_Q_PROVISIONAL = 1,
    DRIFT_Q_QUALIFIED = 2,
} drift_quality_t;

typedef enum {
    DRIFT_SOT_PROVISIONAL = 0,
    DRIFT_SOT_QUALIFIED   = 1,
    DRIFT_SOT_N           = 2,
} drift_sot_slot_t;

#define DRIFT_A_ZERO_CACHE        (1ull << 0)
#define DRIFT_A_IMMEDIATE_VISIBLE (1ull << 1)
#define DRIFT_A_FILENAME_ORIGIN   (1ull << 2)
#define DRIFT_A_FILENAME_PERIODIC (1ull << 3)
#define DRIFT_A_TZ_FIXED          (1ull << 4)
#define DRIFT_A_LEGACY_MODEL      (1ull << 5)

typedef struct {
    bool     valid;
    bool     applicable;         /* binding+time checks complete      */
    bool     freshness_unknown;  /* restored before a live anchor     */
    bool     stale;              /* invalidated for current context   */
    uint8_t  quality;            /* drift_quality_t                   */
    uint8_t  source;             /* drift_source_t                    */
    uint8_t  filename_encoding;
    uint8_t  origin;
    uint32_t model_rev;
    uint64_t assumptions;
    uint8_t  cache_policy;         /* drift_cache_policy_t           */
    uint8_t  visibility_policy;    /* drift_visibility_policy_t      */
    uint32_t anchor_epoch;
    uint32_t dev_rate_ppm;
    uint32_t cache_bound_us;
    int32_t  latency_lo_us;
    int32_t  latency_hi_us;
    uint32_t jitter_us;
    int32_t  fname_lo_us;
    int32_t  fname_hi_us;
    int64_t  filename_period_us;
    uint16_t observations;
    int64_t  clock_drift_ms;
    int64_t  lo_ms, hi_ms;
    int64_t  accuracy_half_ms;
    int64_t  current_half_ms;
    int64_t  computed_at_ms;
    int64_t  reference_at_ms;
    int64_t  ref_mono_us;
    uint32_t persisted_seq;
    bool     dirty;
    esp_err_t last_error;
    char     source_reason[48];
    char     card_fw[24];
    char     model_id[24];
} drift_sot_t;

bool      drift_sot_view(drift_sot_t *out);       /* legacy qualified */
bool      drift_sot_slot_view(drift_sot_slot_t slot, drift_sot_t *out);
bool      drift_sot_best(drift_sot_t *out);
/* Re-reference a stored bound to now_us; expands it by elapsed-time
 * rate uncertainty. False when no trusted current map exists or the
 * record's post-reboot freshness is still unknown.                  */
bool      drift_sot_apply(const drift_sot_t *s, int64_t now_us,
                          int64_t *lo_ms, int64_t *hi_ms);
int       drift_sot_json(const drift_sot_t *s, char *buf, size_t cap);
esp_err_t drift_sot_load(void);          /* boot: NVS -> RAM           */
esp_err_t drift_sot_commit(bool force, int64_t now_us); /* throttled */
bool      drift_sot_dirty(void);
void      drift_sot_invalidate(void);    /* explicit evidence reset    */
void      drift_card_fw_note(const char *fw); /* card identity check  */

/* Confirmed fixed POSIX-style timezone for filename interpretation —
 * e.g. UTC0 or AEST-10. Complex DST rules remain unresolved.          */
void      drift_timezone_set(const char *rule, bool confirmed);
const char *drift_source_name(uint8_t source);
const char *drift_quality_name(uint8_t quality);

/* Metadata-partition record store (§9): 64 rotating JSON records.
 * ESP_PLATFORM only — host tests skip persistence.                  */
#ifdef ESP_PLATFORM
esp_err_t drift_meta_save(const char *json, size_t len);
size_t    drift_meta_latest(char *buf, size_t cap, uint32_t *seq_out);
#endif

#ifndef ESP_PLATFORM
/* host-test hook: inject an anchor without a BLE exchange            */
void drift_test_anchor(int64_t mono_us, int64_t utc_us,
                       uint32_t unc_us, uint32_t epoch);
#endif

#ifdef __cplusplus
}
#endif
