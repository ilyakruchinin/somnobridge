/*
 * SomnoPal — anchors, mapping, stamp-transition estimator, SOT
 * (spec §8.1/§8.3/§8.5; update spec U3–U7)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "drift.h"

#include "esp_crc.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "settings.h"

static const char *TAG = "drift";

/* ---------------- anchors ---------------- */

static drift_anchor_t  s_anchors[DRIFT_ANCHORS_MAX];
static int             s_nanchors;
static uint32_t        s_cur_epoch;
static bool            s_epoch_set;
static drift_pending_t s_pend[5];   /* §10.4: cache last five          */

/* published snapshot + live-session storage — declared early so
 * drift_reset can drop state fitted against a dead monotonic domain  */
static drift_record_t  s_pub;
static bool            s_pub_valid;
static volatile uint32_t s_pub_seq;
static drift_session_t s_live_sess;
static bool            s_live;
static bool            s_closed;   /* session ended, data retained    */

void drift_reset(void)
{
    s_nanchors = 0;
    s_epoch_set = false;
    memset(s_pend, 0, sizeof(s_pend));
    s_live = false;
    s_closed = false;
    s_pub_valid = false;        /* prior estimate used dead anchors   */
}

static drift_pending_t *pend_find(uint32_t rid)
{
    for (int i = 0; i < 5; i++) {
        if (s_pend[i].used && s_pend[i].request_id == rid) return &s_pend[i];
    }
    return NULL;
}

esp_err_t drift_time_begin(uint32_t request_id, uint64_t t1_utc_us,
                           uint32_t src_uncertainty_us,
                           uint32_t source_epoch, int64_t m2_us)
{
    if (!request_id) return ESP_ERR_INVALID_ARG;
    /* §10.4 caches pending exchanges for ≤5 s. Expire them first, or a
     * peer that reuses a request_id after that window is refused
     * forever with INVALID_STATE (the entry only frees when all five
     * slots fill and the oldest is evicted).                          */
    for (int i = 0; i < 5; i++) {
        if (s_pend[i].used && m2_us > s_pend[i].deadline_us) {
            s_pend[i].used = false;
        }
    }
    if (pend_find(request_id)) return ESP_ERR_INVALID_STATE;
    drift_pending_t *p = NULL;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < 5; i++) {
        if (!s_pend[i].used) { p = &s_pend[i]; break; }
        if (s_pend[i].deadline_us < oldest) { oldest = s_pend[i].deadline_us; p = &s_pend[i]; }
    }
    *p = (drift_pending_t){
        .used = true, .request_id = request_id,
        .t1_utc_us = t1_utc_us,
        .src_uncertainty_us = src_uncertainty_us,
        .source_epoch = source_epoch, .m2_us = m2_us,
        .deadline_us = m2_us + 5LL * 1000 * 1000,   /* >5 s = stale   */
    };
    return ESP_OK;
}

void drift_time_reply_tsp(uint32_t request_id, int64_t m3_us)
{
    drift_pending_t *p = pend_find(request_id);
    if (p) p->m3_us = m3_us;
}

anchor_result_t drift_time_commit(uint32_t request_id, uint64_t t4_utc_us,
                                  int64_t now_us, uint32_t *uncertainty_us)
{
    drift_pending_t *p = pend_find(request_id);
    if (!p) return ANCHOR_STALE;
    drift_pending_t e = *p;
    p->used = false;

    if (now_us > e.deadline_us) return ANCHOR_STALE;
    if (e.m3_us < e.m2_us)      return ANCHOR_BAD;
    if (e.src_uncertainty_us > DRIFT_SRC_UNC_MAX_US) return ANCHOR_UNCERTAIN;

    int64_t rtt = (int64_t)t4_utc_us - (int64_t)e.t1_utc_us
                - (e.m3_us - e.m2_us);
    if (rtt < 0 || rtt > DRIFT_RTT_MAX_US) return ANCHOR_RTT;
    if (t4_utc_us < e.t1_utc_us)           return ANCHOR_BAD;

    drift_anchor_t a = {
        .mono_us = (e.m2_us + e.m3_us) / 2,
        .utc_us  = (int64_t)((e.t1_utc_us + t4_utc_us) / 2),
        .uncertainty_us = e.src_uncertainty_us + (uint32_t)rtt / 2
                        + DRIFT_STAMP_ALLOW_US,
        .source_epoch = e.source_epoch,
    };

    anchor_result_t res = ANCHOR_OK;
    if (!s_epoch_set || e.source_epoch != s_cur_epoch) {
        /* new source epoch -> new interpolation segment; flush anchors
         * from a different epoch (never interpolate across, §8.1)     */
        s_nanchors = 0;
        s_cur_epoch = e.source_epoch;
        s_epoch_set = true;
        res = ANCHOR_EPOCH;
    } else if (s_nanchors > 0) {
        /* consistency: |new_map(mono_new) - utc_new| beyond combined
         * uncertainty + oscillator bound -> discontinuity segment     */
        for (int i = 0; i < s_nanchors; i++) {
            const drift_anchor_t *o = &s_anchors[i];
            double dt = (double)(a.mono_us - o->mono_us);
            double bound = (double)(o->uncertainty_us + a.uncertainty_us)
                         + (dt < 0 ? -dt : dt) * DRIFT_EXTRAP_PPM / 1e6;
            double gap = (double)(a.utc_us - o->utc_us) - dt;
            if (gap < 0) gap = -gap;
            if (gap > bound) {
                s_nanchors = 0;      /* do not interpolate through     */
                res = ANCHOR_DISCONTINUITY;
                break;
            }
        }
    }

    if (s_nanchors == DRIFT_ANCHORS_MAX) {
        memmove(s_anchors, s_anchors + 1,
                (DRIFT_ANCHORS_MAX - 1) * sizeof(drift_anchor_t));
        s_nanchors--;
    }
    s_anchors[s_nanchors++] = a;
    if (uncertainty_us) *uncertainty_us = a.uncertainty_us;
    return res;
}

bool drift_map_utc(int64_t mono_us, int64_t *utc_lo, int64_t *utc_hi)
{
    if (!utc_lo || !utc_hi || s_nanchors == 0) return false;

    /* bracketing pair = consecutive anchors straddling mono_us, same
     * domain (single-domain array by construction)                    */
    const drift_anchor_t *a = NULL, *b = NULL;
    for (int i = 0; i + 1 < s_nanchors; i++) {
        if (s_anchors[i].mono_us <= mono_us &&
            mono_us <= s_anchors[i + 1].mono_us) {
            a = &s_anchors[i];
            b = &s_anchors[i + 1];
            break;
        }
    }
    if (a && b) {
        /* affine interpolation, uncertainty = linear blend of endpoint
         * uncertainties (conservative envelope, §8.1)                  */
        double f = (double)(mono_us - a->mono_us) /
                   (double)(b->mono_us - a->mono_us);
        double mid = (double)a->utc_us +
                     f * (double)(b->utc_us - a->utc_us);
        double unc = a->uncertainty_us +
                     f * (double)(b->uncertainty_us - a->uncertainty_us);
        *utc_lo = (int64_t)(mid - unc);
        *utc_hi = (int64_t)(mid + unc);
        return true;
    }
    /* extrapolate from nearest endpoint with ±DRIFT_EXTRAP_PPM bound   */
    const drift_anchor_t *e =
        (mono_us < s_anchors[0].mono_us) ? &s_anchors[0]
                                         : &s_anchors[s_nanchors - 1];
    double dm = (double)(mono_us - e->mono_us);
    double unc = e->uncertainty_us +
                 (dm < 0 ? -dm : dm) * DRIFT_EXTRAP_PPM / 1e6;
    *utc_lo = (int64_t)((double)e->utc_us + dm - unc);
    *utc_hi = (int64_t)((double)e->utc_us + dm + unc);
    return true;
}

int drift_anchor_count(void) { return s_nanchors; }

#ifndef ESP_PLATFORM
void drift_test_anchor(int64_t mono_us, int64_t utc_us,
                       uint32_t unc_us, uint32_t epoch)
{
    if (s_nanchors >= DRIFT_ANCHORS_MAX) return;
    if (!s_epoch_set) { s_cur_epoch = epoch; s_epoch_set = true; }
    s_anchors[s_nanchors++] = (drift_anchor_t){
        .mono_us = mono_us, .utc_us = utc_us,
        .uncertainty_us = unc_us, .source_epoch = epoch,
    };
}
#endif

/* ---------------- characterization profile ---------------- */

static drift_profile_t s_prof = {
    /* characterization-run profile (U9): dense polling armHeyHed, priors
     * tightened to the evidence-backed provisional values. The stamp
     * channel stays UNVALIDATED until this run's data characterizes
     * the write→visibility latency — a too-tight guessed prior can
     * bias self-consistently, so nothing promotes to the SOT yet.    */
    .stamp_channel_validated = false,
    .dense_enabled           = true,
    .cache_bound_us          = 2000000,   /* F12: fresh listings +guard */
    .jitter_us               = 300000,
    .latency_lo_us           = 0,
    .latency_hi_us           = 2000000,   /* provisional, measured now  */
    .dense_spacing_us        = 500000,
    .dense_margin_loose_us   = 4000000,
    .dense_margin_tight_us   = 1500000,
    .dev_rate_ppm            = 500,
    .accurate_half_us        = 2000000,
    .cadence_step_us         = 90000000,
};

const drift_profile_t *drift_profile(void) { return &s_prof; }

void drift_profile_set(const drift_profile_t *p)
{
    if (p) s_prof = *p;
}

/* ---------------- stamps / beacons ---------------- */

/* civil→seconds since 1970-01-01 (Hinnant) — the stamp is UTC-shaped
 * on the AS11's own clock, so it shares the anchors' Unix epoch and
 * subtracts cleanly against mapped UTC.                            */
static int64_t civil_seconds(int y, int mo, int d, int h, int mi, int s)
{
    y -= mo <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153U * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2) / 5
                 + (unsigned)(d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;   /* unix epoch   */
    return days * 86400LL + h * 3600 + mi * 60 + s;
}

bool drift_stamp_parse(const char *raw, int64_t *dev_s_out)
{
    if (!raw || !dev_s_out) return false;
    /* card rows space-pad each field: "2026- 9-30    5: 5: 0"        */
    const char *p = raw;
    int v[6];
    for (int vi = 0; vi < 6; vi++) {
        while (*p && !isdigit((unsigned char)*p)) {
            if (*p != '-' && *p != ':' &&
                !isspace((unsigned char)*p)) {
                return false;
            }
            p++;
        }
        if (!isdigit((unsigned char)*p)) return false;
        int x = 0, nd = 0;
        while (isdigit((unsigned char)*p)) {
            x = x * 10 + (*p - '0');
            p++;
            nd++;
        }
        if (nd < 1 || nd > 4 || (vi == 0 && nd != 4)) return false;
        v[vi] = x;
        if (vi < 2) {
            if (*p != '-') return false;
            p++;
        } else if (vi == 2) {
            if (!isspace((unsigned char)*p)) return false;
            while (isspace((unsigned char)*p)) p++;
        } else if (vi < 5) {
            if (*p != ':') return false;
            p++;
        }
    }
    while (isspace((unsigned char)*p)) p++;
    if (*p) return false;
    if (v[0] < 1999 || v[0] > 2100 || v[1] < 1 || v[1] > 12 ||
        v[2] < 1 || v[2] > 31 || v[3] > 23 || v[4] > 59 || v[5] > 59) {
        return false;
    }
    *dev_s_out = civil_seconds(v[0], v[1], v[2], v[3], v[4], v[5]);
    return true;
}

drift_beacon_t drift_beacon_class(const char *name)
{
    if (!name) return DRIFT_BC_NONE;
    size_t n = strlen(name);
    if (n < 8) return DRIFT_BC_NONE;
    /* suffix "_XXX.edf" — tolerate case                             */
    char suf[9];
    memcpy(suf, name + n - 8, 8);
    suf[8] = '\0';
    for (int i = 0; i < 8; i++) suf[i] = (char)toupper((unsigned char)suf[i]);
    if (!strcmp(suf, "_BRP.EDF")) return DRIFT_BC_BRP;
    if (!strcmp(suf, "_PLD.EDF")) return DRIFT_BC_PLD;
    if (!strcmp(suf, "_SA2.EDF")) return DRIFT_BC_SA2;
    if (!strcmp(suf, "_CSL.EDF") || !strcmp(suf, "_EVE.EDF")) {
        return DRIFT_BC_ANNOT;
    }
    return DRIFT_BC_NONE;
}

bool drift_file_start_iso(const char *name, char *out, size_t cap)
{
    if (!name || !out || cap < 20) return false;
    /* "YYYYMMDD_HHMMSS_*" → "YYYY-MM-DDTHH:MM:SS"                    */
    if (strlen(name) < 15 || name[8] != '_') return false;
    for (int i = 0; i < 8; i++) {
        if (!isdigit((unsigned char)name[i])) return false;
    }
    for (int i = 9; i < 15; i++) {
        if (!isdigit((unsigned char)name[i])) return false;
    }
    snprintf(out, cap, "%.4s-%.2s-%.2sT%.2s:%.2s:%.2s",
             name, name + 4, name + 6, name + 9, name + 11, name + 13);
    return true;
}

drift_session_t *drift_live(void)   { return s_live ? &s_live_sess : NULL; }
drift_session_t *drift_last(void)
{
    if (s_live || s_closed) return &s_live_sess;
    return NULL;
}
bool drift_live_is(void)        { return s_live; }
bool drift_last_closed(void)    { return s_closed && !s_live; }

void drift_sess_begin(drift_session_t *s, uint32_t cache_bound_us,
                      uint32_t jitter_us)
{
    memset(s, 0, sizeof(*s));
    s->cache_bound_us = cache_bound_us;
    s->jitter_us = jitter_us;
    s->beacon_prio = -1;
}

void drift_live_begin(const char *day)
{
    const drift_profile_t *p = drift_profile();
    drift_sess_begin(&s_live_sess, p->cache_bound_us, p->jitter_us);
    s_closed = false;
    memset(s_live_sess.day, 0, sizeof(s_live_sess.day));
    if (day) {
        strncpy(s_live_sess.day, day, sizeof(s_live_sess.day) - 1);
    }
    s_live = true;
}

void drift_live_end(int64_t now_us)
{
    /* retain the closed session for GET_DRIFT (§8.5)                 */
    if (s_live) {
        s_closed = true;
        drift_sess_eval(&s_live_sess, now_us);
    }
    s_live = false;
}

/* ---------------- stamp-event feed (U3/U4) ---------------- */

static int bc_prio(drift_beacon_t b)
{
    return b == DRIFT_BC_BRP ? 0 : b == DRIFT_BC_PLD ? 1 :
           b == DRIFT_BC_SA2 ? 2 : 99;
}

/* Choose / upgrade the sampled beacon from the current listing.
 * Order: class first (BRP preferred), then the chronologically latest
 * name within the class — a day folder holds one sampled file per
 * session, so after a stop/restart the latest same-class file is the
 * active session's. A swap re-baselines the tuple; the old beacon's
 * accumulated evidence stays as history.                           */
static void pick_beacon(drift_session_t *s, const ezs_entry_t *e, int n)
{
    const ezs_entry_t *sel = NULL;
    for (int i = 0; i < n; i++) {
        if (e[i].is_dir) continue;
        if (!sel) {
            sel = &e[i];
            continue;
        }
        int pa = bc_prio(drift_beacon_class(e[i].name));
        int ps = bc_prio(drift_beacon_class(sel->name));
        if (pa < ps ||
            (pa == ps && strcmp(e[i].name, sel->name) > 0)) {
            sel = &e[i];
        }
    }
    if (!sel) return;
    int sp = bc_prio(drift_beacon_class(sel->name));
    if (s->beacon_prio < 0 || sp < s->beacon_prio ||
        (sp == s->beacon_prio && strcmp(sel->name, s->beacon) > 0)) {
        strncpy(s->beacon, sel->name, sizeof(s->beacon) - 1);
        s->beacon[sizeof(s->beacon) - 1] = '\0';
        s->beacon_prio = sp;
        s->have_tuple = false;
    }
}

static void set_tuple(drift_session_t *s, uint64_t size, bool has_ts,
                      const char *raw)
{
    s->size_tok = size;
    s->stamp_ok = has_ts && drift_stamp_parse(raw, &s->stamp_s);
    if (has_ts && !s->stamp_ok) {
        s->flag_bits |= DF_STAMP_BAD;
    }
    s->have_tuple = true;
}

/* Cadence diagnostics: the stamp delta feeds the published flush
 * period; the monotonic resp-delta feeds dense-window prediction.
 * Only deltas in a plausible one-flush range count — a missed scan
 * merges two flushes into one larger delta.                          */
static void note_cadence(drift_session_t *s, int64_t ddev_s,
                         int64_t resp_us)
{
    if (ddev_s >= 30 && ddev_s <= 95) {
        int64_t ms = ddev_s * 1000;
        if (!s->n_deltas) {
            s->per_lo_ms = s->per_hi_ms = ms;
            s->flush_period_ms = ms;
        } else {
            if (ms < s->per_lo_ms) s->per_lo_ms = ms;
            if (ms > s->per_hi_ms) s->per_hi_ms = ms;
            s->flush_period_ms +=
                (ms - s->flush_period_ms) / (s->n_deltas + 1);
        }
        s->n_deltas++;
        if (s->last_ev_resp_us) {
            int64_t dr = resp_us - s->last_ev_resp_us;
            if (dr > 0) {
                if (s->n_resp < (int)(sizeof(s->resp_deltas) /
                                      sizeof(s->resp_deltas[0]))) {
                    s->resp_deltas[s->n_resp++] = dr;
                } else {
                    memmove(s->resp_deltas, s->resp_deltas + 1,
                            3 * sizeof(s->resp_deltas[0]));
                    s->resp_deltas[3] = dr;
                }
            }
        }
    }
}

static void push_ev(drift_session_t *s, drift_ev_kind_t kind,
                    int64_t stamp_s, int64_t req_lo, int64_t resp_hi)
{
    if (s->n_ev >= DRIFT_EV_CAP) {
        s->ev_overflow = true;
        s->flag_bits |= DF_EV_OVERFLOW;
        return;
    }
    s->ev[s->n_ev++] = (drift_ev_t){
        .req_lo_us = req_lo, .resp_hi_us = resp_hi,
        .stamp_s = stamp_s, .kind = (uint8_t)kind,
    };
    s->last_ev_resp_us = resp_hi;
}

/* Window bounds for an event: [prev_scan_req − C − λ_hi, resp − λ_lo]
 * — the write landed while the listing still showed the old tuple at
 * the previous scan, and was visible by this response (U4).         */
static int64_t win_lo(const drift_session_t *s, const drift_profile_t *p,
                      int64_t prev_req)
{
    return prev_req - (int64_t)s->cache_bound_us
         - (int64_t)p->latency_hi_us - (int64_t)s->jitter_us;
}
static int64_t win_hi(const drift_profile_t *p, int64_t resp)
{
    return resp - (int64_t)p->latency_lo_us + (int64_t)p->jitter_us;
}

int drift_feed_scan(drift_session_t *s, const char *day,
                    const ezs_entry_t *entries, int n,
                    const cat_diff_t *diff,
                    int64_t req_start_us, int64_t resp_end_us)
{
    if (!s || !day || strcmp(day, s->day)) {
        return 0;
    }
    const drift_profile_t *p = drift_profile();
    int64_t prev = s->prev_scan_us;
    if (!prev && diff && diff->prev_scan_us) {
        prev = diff->prev_scan_us;   /* resident listing pre-open bound */
    }
    int before = s->n_ev;
    if (diff && diff->overflowed) {
        s->flag_bits |= DF_EV_OVERFLOW;
    }

    pick_beacon(s, entries, n);        /* every scan: tracks the active
                                        * session's file (U3)          */

    const ezs_entry_t *cur = NULL;
    if (s->beacon[0]) {
        for (int i = 0; i < n; i++) {
            if (!entries[i].is_dir && !strcmp(entries[i].name, s->beacon)) {
                cur = &entries[i];
                break;
            }
        }
    }

    bool ev_seen = false;
    if (diff) {
        for (int i = 0; i < diff->n_events; i++) {
            const cat_event_t *e = &diff->ev[i];
            if (!s->beacon[0] || strcmp(e->name, s->beacon)) {
                continue;
            }
            ev_seen = true;
            switch (e->kind) {
            case CAT_EV_GONE:
                s->flag_bits |= DF_BEACON_GONE;
                s->have_tuple = false;
                break;
            case CAT_EV_NEW: {
                int64_t m_new;
                if (e->new_stamp_ok &&
                    drift_stamp_parse(e->new_stamp, &m_new)) {
                    if (prev) {
                        push_ev(s, DRIFT_EV_CREATE, m_new,
                                win_lo(s, p, prev), win_hi(p, resp_end_us));
                    } else {
                        s->flag_bits |= DF_OBSERVED_LATE;
                    }
                } else {
                    s->flag_bits |= DF_STAMP_BAD;
                }
                break;
            }
            case CAT_EV_GREW:
            case CAT_EV_SHRUNK:
            case CAT_EV_STAMP: {
                int64_t m_new, m_old;
                bool ok_n = e->new_stamp_ok &&
                            drift_stamp_parse(e->new_stamp, &m_new);
                bool ok_o = e->old_stamp_ok &&
                            drift_stamp_parse(e->old_stamp, &m_old);
                if (!ok_n || !ok_o) {
                    s->flag_bits |= DF_STAMP_BAD;
                    break;
                }
                note_cadence(s, m_new - m_old, resp_end_us);
                if (prev) {
                    push_ev(s, DRIFT_EV_TRANSITION, m_new,
                            win_lo(s, p, prev), win_hi(p, resp_end_us));
                } else {
                    s->flag_bits |= DF_OBSERVED_LATE;
                }
                break;
            }
            }
        }
    }
    /* Fallback path: the beacon's stored tuple diverged without a
     * matching diff event (event-cap overflow, or a feed with diff ==
     * NULL). Emit the transition with only the new stamp — the cadence
     * delta is unusable but the offset interval is not.              */
    if (cur && !ev_seen) {
        int64_t ms;
        bool okp = cur->has_timestamp &&
                   drift_stamp_parse(cur->timestamp_raw, &ms);
        if (s->have_tuple && okp &&
            (ms != s->stamp_s || cur->size != s->size_tok)) {
            int64_t d = s->stamp_ok ? ms - s->stamp_s : 0;
            note_cadence(s, d, resp_end_us);
            if (prev) {
                push_ev(s, DRIFT_EV_TRANSITION, ms,
                        win_lo(s, p, prev), win_hi(p, resp_end_us));
            } else {
                s->flag_bits |= DF_OBSERVED_LATE;
            }
        } else if (s->have_tuple && !okp && s->stamp_ok) {
            s->flag_bits |= DF_STAMP_BAD;   /* stamp vanished          */
        }
    }
    if (cur) {
        set_tuple(s, cur->size, cur->has_timestamp, cur->timestamp_raw);
    }
    s->prev_scan_us = req_start_us;
    if (!prev) {
        s->baseline_bounded = false;
    }
    return s->n_ev - before;
}

/* ---------------- estimate evaluation ---------------- */

static int64_t us_floor_ms(int64_t us)
{
    return us >= 0 ? us / 1000 : -((-us + 999) / 1000);
}
static int64_t us_ceil_ms(int64_t us)
{
    return us >= 0 ? (us + 999) / 1000 : -((-us) / 1000);
}
static int64_t map_mid_ms(int64_t mono_us, bool *ok)
{
    int64_t lo, hi;
    if (!drift_map_utc(mono_us, &lo, &hi)) {
        *ok = false;
        return 0;
    }
    *ok = true;
    return (lo + hi) / 2000;
}

/* ---------------- published snapshot ---------------- */

bool drift_pub_get(drift_record_t *out)
{
    if (!s_pub_valid) return false;
    if (!out) return true;             /* validity probe (UI)         */
    for (int t = 0; t < 4; t++) {
        uint32_t a = s_pub_seq;
        if (a & 1) continue;
        *out = s_pub;
        if (s_pub_seq == a) return true;
    }
    *out = s_pub;
    return true;
}

static void flag(char *f, size_t cap, const char *name)
{
    size_t l = strlen(f);
    if (l) snprintf(f + l, cap - l, ",%s", name);
    else   snprintf(f, cap, "%s", name);
}

static void publish(const drift_session_t *s, int64_t now_us)
{
    drift_record_t r;
    memset(&r, 0, sizeof(r));

    r.n_windows  = s->n_windows;
    r.n_segments = (int)s->n_segments;
    r.tier       = s->tier;
    r.observation_closed = s_closed || s != &s_live_sess;
    r.complete_records  = s->complete_records;
    r.record_bytes      = s->record_bytes;
    r.record_duration_s = s->record_duration_s;
    strncpy(r.day, s->day, sizeof(r.day) - 1);
    strncpy(r.beacon, s->beacon, sizeof(r.beacon) - 1);
    if (s->beacon[0]) {
        drift_file_start_iso(s->beacon, r.session_start_as10,
                             sizeof(r.session_start_as10));
    }

    if (s->est_bound_valid) {
        r.has_drift = true;
        r.drift_lo_ms = us_floor_ms(s->off_lo_us);
        r.drift_hi_ms = us_ceil_ms(s->off_hi_us);
        r.clock_drift_ms = (r.drift_lo_ms + r.drift_hi_ms) / 2;
        r.accuracy_half_ms = (r.drift_hi_ms - r.drift_lo_ms) / 2;
        bool ok;
        r.computed_at_ms = map_mid_ms(now_us, &ok);
        if (!ok) r.computed_at_ms = 0;
        r.reference_at_ms = map_mid_ms(s->ref_mono_us, &ok);
        if (!ok) r.reference_at_ms = 0;
    }
    if (s->n_deltas) {
        r.has_period = true;
        r.flush_period_ms = s->flush_period_ms;
        r.period_lo_ms = s->per_lo_ms;
        r.period_hi_ms = s->per_hi_ms;
    }
    /* session-start bound: the beacon's creation window, if seen     */
    if (s->n_ev && s->ev[0].kind == DRIFT_EV_CREATE) {
        int64_t lo, hi, lo2, hi2;
        if (drift_map_utc(s->ev[0].req_lo_us, &lo, &hi) &&
            drift_map_utc(s->ev[0].resp_hi_us, &lo2, &hi2)) {
            r.has_point = true;
            r.start_lo_ms = us_floor_ms(lo);
            r.start_hi_ms = us_ceil_ms(hi2);
            r.session_start_true_ms = (r.start_lo_ms + r.start_hi_ms) / 2;
        }
    }

    /* state */
    if (!s->n_ev) {
        r.state = EST_PENDING;
    } else if (s->est_bound_valid) {
        r.state = s->tier == ACC_ACCURATE ? EST_VALIDATED
                                          : EST_PROVISIONAL;
    } else {
        r.state = EST_PENDING;
    }

    uint32_t fb = s->flag_bits;
    if (fb & DF_NO_ANCHOR)     flag(r.flags, sizeof(r.flags), "no_anchor_2h");
    if (fb & DF_OBSERVED_LATE) flag(r.flags, sizeof(r.flags), "observed_late");
    if (fb & DF_BACKWARD)      flag(r.flags, sizeof(r.flags), "clock_step_back");
    if (fb & DF_STEP)          flag(r.flags, sizeof(r.flags), "clock_step");
    if (fb & DF_CONTRADICTION) flag(r.flags, sizeof(r.flags),
                                    "interval_contradiction");
    if (fb & DF_STAMP_BAD)     flag(r.flags, sizeof(r.flags), "stamp_missing");
    if (fb & DF_BEACON_GONE)   flag(r.flags, sizeof(r.flags), "beacon_lost");
    if (fb & DF_EV_OVERFLOW)   flag(r.flags, sizeof(r.flags), "diff_overflow");
    if (fb & DF_UNVALIDATED)   flag(r.flags, sizeof(r.flags),
                                    "timing_unvalidated");

    s_pub_seq++;
    s_pub = r;
    s_pub_seq++;
    s_pub_valid = true;
}

/* ---------------- SOT (U6) ---------------- */

static drift_sot_t s_sot;
static bool        s_sot_dirty;
static int64_t     s_sot_last_store_us;
static int64_t     s_sot_committed_half;
static uint32_t    s_sot_persist_seq;
static char        s_card_fw[24];

bool drift_sot_view(drift_sot_t *out)
{
    if (!out || !s_sot.valid) return false;
    *out = s_sot;
    return true;
}

bool drift_sot_dirty(void) { return s_sot_dirty; }

void drift_card_fw_note(const char *fw)
{
    if (!fw || !fw[0]) return;
    if (s_card_fw[0] && strcmp(s_card_fw, fw)) {
        /* card firmware/identity changed underneath us — any persisted
         * characterization is about a different card (§9 invalidation) */
        drift_sot_invalidate();
    }
    strncpy(s_card_fw, fw, sizeof(s_card_fw) - 1);
    s_card_fw[sizeof(s_card_fw) - 1] = '\0';
}

void drift_sess_eval(drift_session_t *s, int64_t now_us)
{
    const drift_profile_t *p = drift_profile();
    /* eval-recomputed flags reset; feed-time flags (OBSERVED_LATE,
     * STAMP_BAD, BEACON_GONE, EV_OVERFLOW) persist                  */
    s->flag_bits &= ~(DF_NO_ANCHOR | DF_BACKWARD | DF_STEP |
                      DF_CONTRADICTION | DF_UNVALIDATED);

    bool have_acc = false;
    int64_t alo = 0, ahi = 0, ref = 0;
    int nw = 0;
    uint32_t seg = 0, splits = 0;
    bool anch = false;
    bool have_prev = false;
    int64_t prev_st = 0, prev_resp = 0;

    for (int i = 0; i < s->n_ev; i++) {
        const drift_ev_t *e = &s->ev[i];
        if (have_prev) {
            if (e->stamp_s < prev_st) {
                /* backwards stamp — the clock stepped or the file was
                 * rewritten: never interpolate through it (U4.1)      */
                seg++;
                splits++;
                have_acc = false;
                s->flag_bits |= DF_BACKWARD;
            } else {
                int64_t ddev  = (e->stamp_s - prev_st) * 1000000LL;
                int64_t dmono = e->resp_hi_us - prev_resp;
                int64_t gap = ddev - dmono;
                if (gap < 0) gap = -gap;
                if (gap > (int64_t)p->cadence_step_us) {
                    seg++;
                    splits++;
                    have_acc = false;
                    s->flag_bits |= DF_STEP;
                }
            }
        }
        int64_t ulo, uhi, vlo, vhi;
        if (!drift_map_utc(e->req_lo_us, &ulo, &uhi) ||
            !drift_map_utc(e->resp_hi_us, &vlo, &vhi)) {
            s->flag_bits |= DF_NO_ANCHOR;
        } else {
            anch = true;
            /* offset = true − device; device ∈ [M, M+q) at write    */
            int64_t lo = ulo - (e->stamp_s + 2) * 1000000LL;
            int64_t hi = vhi - e->stamp_s * 1000000LL;
            if (!have_acc) {
                alo = lo;
                ahi = hi;
                nw = 1;
                have_acc = true;
            } else {
                /* the offset may drift between windows at the AS11's
                 * own rate — re-reference the running bound to the
                 * newest event's instant before intersecting        */
                int64_t dt = e->resp_hi_us - ref;
                if (dt > 0) {
                    int64_t w = dt * (int64_t)p->dev_rate_ppm / 1000000;
                    alo -= w;
                    ahi += w;
                }
                if (lo > ahi || hi < alo) {
                    /* empty intersection — the evidence contradicted
                     * itself: close the segment, restart honestly   */
                    seg++;
                    splits++;
                    s->flag_bits |= DF_CONTRADICTION;
                    alo = lo;
                    ahi = hi;
                    nw = 1;
                } else {
                    if (lo > alo) alo = lo;
                    if (hi < ahi) ahi = hi;
                    nw++;
                }
            }
            ref = e->resp_hi_us;
        }
        have_prev = true;
        prev_st = e->stamp_s;
        prev_resp = e->resp_hi_us;
    }

    s->est_bound_valid = have_acc;
    s->off_lo_us = alo;
    s->off_hi_us = ahi;
    s->ref_mono_us = ref;
    s->n_windows = nw;
    s->n_segments = have_acc ? seg + 1 : seg;
    s->n_splits = splits;
    s->anchor_used = anch;

    int64_t half = have_acc ? (ahi - alo) / 2 : 0;
    if (have_acc && anch && nw >= 2 && p->stamp_channel_validated &&
        half <= (int64_t)p->accurate_half_us) {
        s->tier = ACC_ACCURATE;
    } else if (have_acc) {
        s->tier = ACC_APPROXIMATE;
    } else {
        s->tier = ACC_NONE;
    }
    if (!p->stamp_channel_validated) {
        s->flag_bits |= DF_UNVALIDATED;
    }

    publish(s, now_us);

    /* U6: a live session's accurate value promotes immediately —
     * newest computed_at wins; nothing else may displace the SOT.    */
    if (s == drift_live() && s->tier == ACC_ACCURATE) {
        bool ok;
        int64_t comp = map_mid_ms(now_us, &ok);
        if (!ok) comp = 0;
        int64_t refm = map_mid_ms(ref, &ok);
        if (!ok) refm = 0;
        if (comp && refm &&
            (!s_sot.valid || comp > s_sot.computed_at_ms)) {
            s_sot.valid = true;
            s_sot.clock_drift_ms = (us_floor_ms(alo) + us_ceil_ms(ahi)) / 2;
            s_sot.lo_ms = us_floor_ms(alo);
            s_sot.hi_ms = us_ceil_ms(ahi);
            s_sot.accuracy_half_ms = (s_sot.hi_ms - s_sot.lo_ms) / 2;
            s_sot.computed_at_ms = comp;
            s_sot.reference_at_ms = refm;
            s_sot.origin = DRIFT_SOT_SESSION;
            strncpy(s_sot.card_fw, s_card_fw, sizeof(s_sot.card_fw) - 1);
            s_sot_dirty = true;
        }
    }
}

/* ---------------- SOT NVS blob (metadata partition) ----------------
 * One slot; newest accurate wins; corrupt/absent → no value, never a
 * fabricated one. Host-testable through the stub NVS store.         */

#define SOT_KEY    "drift_sot"
#define SOT_MAGIC  0x534F5431u   /* "SOT1" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  rsv[3];
    uint32_t seq;
    int64_t  drift_ms;
    int64_t  lo_ms, hi_ms;
    int64_t  half_ms;
    int64_t  comp_ms;
    int64_t  ref_ms;
    char     fw[24];
    uint32_t crc32;
} sot_blob_t;

esp_err_t drift_sot_load(void)
{
    nvs_handle_t h = settings_nvs_metadata();
    if (!h) return ESP_ERR_INVALID_STATE;
    sot_blob_t b;
    size_t want = sizeof(b);
    if (nvs_get_blob(h, SOT_KEY, &b, &want) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (want != sizeof(b) || b.magic != SOT_MAGIC || b.version != 1 ||
        esp_crc32_le(0, (const uint8_t *)&b,
                     offsetof(sot_blob_t, crc32)) != b.crc32) {
        ESP_LOGW(TAG, "drift_sot blob invalid — ignored");
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_sot.valid = true;
    s_sot.clock_drift_ms = b.drift_ms;
    s_sot.lo_ms = b.lo_ms;
    s_sot.hi_ms = b.hi_ms;
    s_sot.accuracy_half_ms = b.half_ms;
    s_sot.computed_at_ms = b.comp_ms;
    s_sot.reference_at_ms = b.ref_ms;
    s_sot.persisted_seq = b.seq;
    s_sot.origin = DRIFT_SOT_NVS;
    memcpy(s_sot.card_fw, b.fw, sizeof(s_sot.card_fw));
    s_sot.card_fw[sizeof(s_sot.card_fw) - 1] = '\0';
    s_sot_persist_seq = b.seq;
    s_sot_committed_half = b.half_ms;
    s_sot_dirty = false;
    return ESP_OK;
}

esp_err_t drift_sot_commit(bool force, int64_t now_us)
{
    if (!s_sot.valid || !s_sot_dirty) return ESP_OK;
    if (!force) {
        /* throttled improvement writes: ≥250 ms half-width shrink,
         * at most once per 60 s (U6)                                 */
        if (now_us - s_sot_last_store_us < 60LL * 1000 * 1000) {
            return ESP_OK;
        }
        if (s_sot_committed_half &&
            s_sot_committed_half - s_sot.accuracy_half_ms < 250) {
            return ESP_OK;
        }
    }
    nvs_handle_t h = settings_nvs_metadata();
    if (!h) return ESP_ERR_INVALID_STATE;
    sot_blob_t b;
    memset(&b, 0, sizeof(b));
    b.magic = SOT_MAGIC;
    b.version = 1;
    b.seq = s_sot_persist_seq + 1;
    b.drift_ms = s_sot.clock_drift_ms;
    b.lo_ms = s_sot.lo_ms;
    b.hi_ms = s_sot.hi_ms;
    b.half_ms = s_sot.accuracy_half_ms;
    b.comp_ms = s_sot.computed_at_ms;
    b.ref_ms = s_sot.reference_at_ms;
    strncpy(b.fw, s_card_fw, sizeof(b.fw) - 1);
    b.crc32 = esp_crc32_le(0, (const uint8_t *)&b,
                           offsetof(sot_blob_t, crc32));
    esp_err_t err = nvs_set_blob(h, SOT_KEY, &b, sizeof(b));
    if (err == ESP_OK) err = nvs_commit(h);
    if (err == ESP_OK) {
        s_sot_persist_seq = b.seq;
        s_sot.persisted_seq = b.seq;
        s_sot_last_store_us = now_us;
        s_sot_committed_half = s_sot.accuracy_half_ms;
        s_sot_dirty = false;
        ESP_LOGI(TAG, "SOT persisted: seq=%lu drift=%lldms ±%lldms",
                 (unsigned long)b.seq, (long long)b.drift_ms,
                 (long long)b.half_ms);
    }
    return err;
}

void drift_sot_invalidate(void)
{
    nvs_handle_t h = settings_nvs_metadata();
    if (h) {
        nvs_erase_key(h, SOT_KEY);
        nvs_commit(h);
    }
    memset(&s_sot, 0, sizeof(s_sot));
    s_sot_dirty = false;
    s_sot_committed_half = 0;
}

/* ---------------- JSON ---------------- */

static int emit_ll_or_null(char *b, size_t cap, size_t *o,
                           const char *key, bool has, int64_t v)
{
    int n = has ? snprintf(b + *o, cap - *o, "\"%s\":%lld,", key, (long long)v)
                : snprintf(b + *o, cap - *o, "\"%s\":null,", key);
    *o += (n > 0) ? (size_t)n : 0;
    return n;
}

static int emit_iv_or_null(char *b, size_t cap, size_t *o,
                           const char *key, bool has, int64_t lo, int64_t hi)
{
    int n = has ? snprintf(b + *o, cap - *o, "\"%s\":[%lld,%lld],",
                           key, (long long)lo, (long long)hi)
                : snprintf(b + *o, cap - *o, "\"%s\":null,", key);
    *o += (n > 0) ? (size_t)n : 0;
    return n;
}

static size_t json_escape(char *buf, size_t cap, size_t *o, const char *in)
{
    for (; in && *in && *o + 3 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '\\' || c == '"') buf[(*o)++] = '\\';
        buf[(*o)++] = (char)c;
    }
    buf[*o] = '\0';
    return *o;
}

int drift_estimate_json(const drift_record_t *r, char *buf, size_t cap)
{
    static const char *st[] = {"pending", "provisional", "validated",
                               "invalid"};
    static const char *tier[] = {"none", "approximate", "accurate"};
    size_t o = 0;
    int n = snprintf(buf, cap,
        "{\"estimate_state\":\"%s\",\"observation_closed\":%s,"
        "\"accuracy_tier\":\"%s\",",
        st[r->state], r->observation_closed ? "true" : "false",
        tier[r->tier]);
    o = (n > 0) ? (size_t)n : 0;
    if (r->beacon[0]) {
        n = snprintf(buf + o, cap - o, "\"files\":[{\"dir\":\"A:\\\\DATALOG\\\\");
        o += (n > 0) ? (size_t)n : 0;
        json_escape(buf, cap, &o, r->day);
        n = snprintf(buf + o, cap - o, "\",\"name\":\"");
        o += (n > 0) ? (size_t)n : 0;
        json_escape(buf, cap, &o, r->beacon);
        n = snprintf(buf + o, cap - o, "\"}],");
        o += (n > 0) ? (size_t)n : 0;
    } else {
        n = snprintf(buf + o, cap - o, "\"files\":[],");
        o += (n > 0) ? (size_t)n : 0;
    }
    if (r->session_start_as10[0]) {
        n = snprintf(buf + o, cap - o, "\"session_start_as10\":\"%s\",",
                     r->session_start_as10);
        o += (n > 0) ? (size_t)n : 0;
    } else {
        n = snprintf(buf + o, cap - o, "\"session_start_as10\":null,");
        o += (n > 0) ? (size_t)n : 0;
    }
    n = snprintf(buf + o, cap - o, "\"timezone_rule\":null,");
    o += (n > 0) ? (size_t)n : 0;
    emit_ll_or_null(buf, cap, &o, "session_start_true_ms",
                    r->has_point, r->session_start_true_ms);
    emit_iv_or_null(buf, cap, &o, "start_interval_ms",
                    r->has_point, r->start_lo_ms, r->start_hi_ms);
    emit_ll_or_null(buf, cap, &o, "clock_drift_ms",
                    r->has_drift, r->clock_drift_ms);
    emit_iv_or_null(buf, cap, &o, "drift_interval_ms",
                    r->has_drift, r->drift_lo_ms, r->drift_hi_ms);
    emit_ll_or_null(buf, cap, &o, "accuracy_half_ms",
                    r->has_drift, r->accuracy_half_ms);
    emit_ll_or_null(buf, cap, &o, "computed_at_ms",
                    r->computed_at_ms != 0, r->computed_at_ms);
    emit_ll_or_null(buf, cap, &o, "reference_at_ms",
                    r->reference_at_ms != 0, r->reference_at_ms);
    n = snprintf(buf + o, cap - o,
        "\"sigma_start_ms\":null,\"sigma_drift_ms\":null,"
        "\"systematic_components\":[\"stamp_quantization\","
        "\"poll_visibility\",\"latency_prior\",\"cache_bound\","
        "\"anchor_uncertainty\"],");
    o += (n > 0) ? (size_t)n : 0;
    emit_ll_or_null(buf, cap, &o, "flush_period_ms",
                    r->has_period, r->flush_period_ms);
    emit_iv_or_null(buf, cap, &o, "flush_period_interval_ms",
                    r->has_period, r->period_lo_ms, r->period_hi_ms);
    n = snprintf(buf + o, cap - o,
        "\"timer_rate_ppm\":null,\"timer_rate_interval_ppm\":null,"
        "\"rtc_rate_ppm\":null,\"record_duration_s\":%lu,"
        "\"record_bytes\":%lu,\"complete_records\":\"%llu\","
        "\"n_windows\":%d,\"segments\":%d,"
        "\"model_id\":\"uncharacterized\",\"flags\":[",
        (unsigned long)r->record_duration_s,
        (unsigned long)r->record_bytes,
        (unsigned long long)r->complete_records,
        r->n_windows, r->n_segments);
    o += (n > 0) ? (size_t)n : 0;
    const char *p = r->flags;
    while (*p && o < cap - 8) {
        const char *c = strchr(p, ',');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        n = snprintf(buf + o, cap - o, "\"%.*s\",", (int)len, p);
        o += (n > 0) ? (size_t)n : 0;
        p += c ? len + 1 : len;
    }
    if (o && buf[o - 1] == ',') o--;
    snprintf(buf + o, cap - o, "]}");
    return (int)strlen(buf);
}

int drift_sot_json(const drift_sot_t *s, char *buf, size_t cap)
{
    int n = snprintf(buf, cap,
        "{\"clock_drift_ms\":%lld,\"interval_ms\":[%lld,%lld],"
        "\"accuracy_half_ms\":%lld,\"computed_at_ms\":%lld,"
        "\"reference_at_ms\":%lld,\"origin\":\"%s\","
        "\"persisted_seq\":%lu}",
        (long long)s->clock_drift_ms, (long long)s->lo_ms,
        (long long)s->hi_ms, (long long)s->accuracy_half_ms,
        (long long)s->computed_at_ms, (long long)s->reference_at_ms,
        s->origin == DRIFT_SOT_NVS ? "nvs" : "session",
        (unsigned long)s->persisted_seq);
    return n > 0 ? n : 0;
}

/* ---------------- dense-window prediction (U9) ---------------- */

bool drift_dense_window(int64_t now_us, int64_t *open_us,
                        int64_t *close_us)
{
    if (!s_live || !s_prof.dense_enabled || !s_live_sess.n_resp) {
        return false;
    }
    /* refine only the first few observed flushes — the intersection
     * floor is met after ~2–3 independent windows (U9)              */
    if (s_live_sess.n_ev >= 6) {
        return false;
    }
    int64_t period = 0;
    for (int i = 0; i < s_live_sess.n_resp; i++) {
        period += s_live_sess.resp_deltas[i];
    }
    period /= s_live_sess.n_resp;
    int64_t margin = s_live_sess.n_resp >= 3
                       ? (int64_t)s_prof.dense_margin_tight_us
                       : (int64_t)s_prof.dense_margin_loose_us;
    int64_t pred = s_live_sess.last_ev_resp_us + period;
    if (now_us < pred - margin || now_us > pred + margin) {
        return false;
    }
    if (open_us)  *open_us = pred - margin;
    if (close_us) *close_us = pred + margin;
    return true;
}

/* ---------------- diagnostic session helpers ---------------- */

void drift_sess_header(drift_session_t *s, uint32_t header_bytes,
                       uint32_t record_bytes, uint32_t record_duration_s,
                       int64_t hdr_start_local_s)
{
    s->header_known = true;
    s->header_bytes = header_bytes;
    s->record_bytes = record_bytes;
    s->record_duration_s = record_duration_s;
    s->hdr_start_local_s = hdr_start_local_s;
}

/* complete_records is written directly by the verified download path
 * (bytes → records via record_bytes) — the field stays public.      */

/* ---------------- metadata-partition record store (§9) ----------------
 * ESP_PLATFORM only — the rotating estimate-history store.            */

#ifdef ESP_PLATFORM

#define DRIFT_META_SLOTS 64
#define DRM_MAGIC 0x44524D31u   /* "DRM1" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  rsv[3];
    uint32_t seq;
    uint16_t len;
    uint16_t rsv2;
    uint32_t crc32;
} drm_hdr_t;

static void meta_key(uint8_t slot, char out[5])
{
    out[0] = 'd';
    out[1] = '0' + (slot / 100) % 10;
    out[2] = '0' + (slot / 10) % 10;
    out[3] = '0' + slot % 10;
    out[4] = '\0';
}

esp_err_t drift_meta_save(const char *json, size_t len)
{
    nvs_handle_t h = settings_nvs_metadata();
    if (!h || !json || !len || len > 8192) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t cur = 0;
    nvs_get_u32(h, "dcur", &cur);
    uint8_t slot = cur % DRIFT_META_SLOTS;
    uint32_t seq = cur + 1;

    static uint8_t buf[sizeof(drm_hdr_t) + 8192];
    drm_hdr_t *hd = (drm_hdr_t *)buf;
    *hd = (drm_hdr_t){ .magic = DRM_MAGIC, .version = 1,
                       .seq = seq, .len = (uint16_t)len,
                       .crc32 = esp_crc32_le(0, (const uint8_t *)json,
                                             len) };
    memcpy(buf + sizeof(*hd), json, len);
    char k[5];
    meta_key(slot, k);
    esp_err_t err = nvs_set_blob(h, k, buf, sizeof(*hd) + len);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "dcur", seq);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "meta save failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* Load the newest retained record JSON; NULL when none/absent. */
size_t drift_meta_latest(char *buf, size_t cap, uint32_t *seq_out)
{
    nvs_handle_t h = settings_nvs_metadata();
    if (!h) return 0;
    uint32_t cur = 0;
    if (nvs_get_u32(h, "dcur", &cur) != ESP_OK || !cur) return 0;
    char k[5];
    meta_key((cur - 1) % DRIFT_META_SLOTS, k);
    drm_hdr_t hd;
    size_t need = 0;
    if (nvs_get_blob(h, k, NULL, &need) != ESP_OK ||
        need < sizeof(hd) || need > cap) {
        return 0;
    }
    if (nvs_get_blob(h, k, buf, &need) != ESP_OK) return 0;
    memcpy(&hd, buf, sizeof(hd));
    if (hd.magic != DRM_MAGIC || hd.len > need - sizeof(hd) ||
        esp_crc32_le(0, (const uint8_t *)buf + sizeof(hd),
                     hd.len) != hd.crc32) {
        return 0;
    }
    memmove(buf, buf + sizeof(hd), hd.len);
    buf[hd.len] = '\0';
    if (seq_out) *seq_out = hd.seq;
    return hd.len;
}
#endif /* ESP_PLATFORM */
