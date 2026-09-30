/*
 * SomnoPal — time anchors, monotonic→UTC mapping, and drift records
 * (spec §8, §8.5)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
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

/* ---------------- observation session (§8.3) ---------------- */

#define DRIFT_OBS_MAX        192     /* constrained size observations   */

typedef struct {
    int64_t  issue_us;             /* request issue (s_i)             */
    int64_t  resp_us;              /* complete response (r_i)         */
    uint64_t size;                 /* listed beacon size              */
} drift_obs_t;

typedef enum {
    EST_PENDING = 0,
    EST_PROVISIONAL,
    EST_VALIDATED,
    EST_INVALID,
} est_state_t;

typedef struct {
    /* EDF layout learned from the bounded header probe (§8.2)         */
    bool     header_known;
    uint32_t header_bytes;         /* H = 256*(1+n_signals)           */
    uint32_t record_bytes;         /* R = 2*sum(samples_per_record)   */
    uint32_t record_duration_s;    /* D_record                        */
    int64_t  hdr_start_local_s;    /* h: raw header start, AS10 local */
    uint32_t complete_records;     /* latest q                        */

    /* fit state                                                      */
    drift_obs_t obs[DRIFT_OBS_MAX];
    int      n_obs;
    bool     seen_nonheader;       /* first record-bearing size seen  */

    /* fitted (T_1, P) in monotonic microseconds                       */
    bool     fit_valid;
    int64_t  t1_lo_us, t1_hi_us;
    int64_t  p_lo_us,  p_hi_us;
    uint32_t cache_bound_us;       /* C: listing cache upper bound    */
    uint32_t jitter_us;            /* per-edge jitter bound           */
} drift_session_t;

/* ---------------- the one live session ----------------
 * Owned by this component; the card worker writes, BLE ops read.   */
drift_session_t *drift_live(void);       /* NULL when no session      */
drift_session_t *drift_last(void);       /* live OR closed session    */
bool             drift_live_is(void);
bool             drift_last_closed(void);
void             drift_live_begin(uint32_t cache_us, uint32_t jitter_us,
                                  const char *day, const char *beacon);
void             drift_live_end(void);   /* therapy gap / OUT         */
/* Beacon = the tracked growing file; day_out is CAT_DAY_STR bytes. */
const char      *drift_live_beacon(char *day_out, char *name_out);
void             drift_live_set_beacon(const char *day, const char *name);
bool             drift_live_probe_used(void);   /* §8.2: 1 per file set */
void             drift_live_probe_mark(void);

/* Metadata-partition record store (§9): 64 rotating JSON records.
 * ESP_PLATFORM only — host tests skip persistence.                  */
esp_err_t drift_meta_save(const char *json, size_t len);
size_t    drift_meta_latest(char *buf, size_t cap, uint32_t *seq_out);

void drift_sess_begin(drift_session_t *s, uint32_t cache_bound_us,
                      uint32_t jitter_us);
void drift_sess_header(drift_session_t *s, uint32_t header_bytes,
                       uint32_t record_bytes, uint32_t record_duration_s,
                       int64_t hdr_start_local_s);
bool drift_sess_observe(drift_session_t *s, int64_t issue_us,
                        int64_t resp_us, uint64_t size);
/* Intersect all constraints; sets fit_valid/t1/p bounds or clears.     */
bool drift_sess_fit(drift_session_t *s, int64_t p_prior_lo_us,
                    int64_t p_prior_hi_us, int64_t t1_prior_lo_us,
                    int64_t t1_prior_hi_us);

/* ---------------- §8.5 record ---------------- */

typedef struct {
    est_state_t state;
    bool        observation_closed;
    int64_t     session_start_true_ms;   /* valid when !null point     */
    bool        has_point;
    int64_t     start_lo_ms, start_hi_ms;
    int64_t     clock_drift_ms;
    bool        has_drift;
    int64_t     drift_lo_ms, drift_hi_ms;
    int64_t     flush_period_ms;
    bool        has_period;
    int64_t     period_lo_ms, period_hi_ms;
    double      timer_rate_ppm;
    bool        has_timer_rate;
    uint64_t    complete_records;
    uint32_t    record_bytes;
    uint32_t    record_duration_s;
    char        flags[160];              /* comma list, machine codes  */
    char        record_key[24];          /* opaque journal lookup key  */
} drift_record_t;

/* Build the publishable record from a session + anchor state. Mapping
 * happens through drift_map_utc; failures produce null fields + flags. */
void drift_build_record(const drift_session_t *s, drift_record_t *out);
/* Serialize as compact JSON (≤8 KiB) for GET_DRIFT / metadata store.   */
int  drift_record_json(const drift_record_t *r, const char *files_json,
                       const char *session_start_as10,
                       char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
