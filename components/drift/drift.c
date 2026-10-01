/*
 * SomnoPal — anchors, mapping, stamp-transition estimator, SOT
 * (spec §8.1/§8.3/§8.5; update spec U3–U7)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "drift.h"

#include "esp_crc.h"
#include "esp_log.h"
#include "journal.h"
#include "nvs_flash.h"
#include "settings.h"

static const char *TAG = "drift";

static void drift_jrn(const char *kind, const char *fmt, ...)
{
    char body[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    (void)jrn_put(kind, body);
}

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
static void sot_consider(const drift_session_t *s, int64_t now_us);
static void sot_anchor_ready(void);
static void sot_mark_stale(bool filename_only);
static uint64_t assumption_mask(const drift_session_t *s,
                                const drift_profile_t *p);

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
    sot_anchor_ready();
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
    sot_anchor_ready();
}
#endif

/* ---------------- characterization profile ---------------- */

static bool s_prof_initialized;
static drift_profile_t s_prof = {
    /* Conservative comparison model: provisional intervals keep the
     * broad cache/visibility priors until source-specific evidence
     * qualifies a tighter revision. Filename origin/cycle bounds stay
     * unresolved by default; no zero-D assumption is installed.       */
    .stamp_channel_validated = false,
    .phase_track_enabled     = true,
    .phase_refine_enabled    = true,
    .cache_bound_us          = 2000000,
    .jitter_us               = 0,
    .latency_lo_us           = 0,
    .latency_hi_us           = 2000000,
    .dev_rate_ppm            = 500,
    .accurate_half_us        = 2000000,
    .provisional_half_us     = 5000000,
    .cadence_step_us         = 90000000,
    .model_rev               = 1,
    .filename_encoding       = DRIFT_ENC_UNKNOWN,
    .filename_origin_bounded = false,
    .fname_create_lo_us      = 0,
    .fname_create_hi_us      = 0,
    .filename_periodic_bounded = false,
    .fname_period_lo_us      = 0,
    .fname_period_hi_us      = 0,
    .filename_create_validated = false,
    .filename_periodic_validated = false,
};

const drift_profile_t *drift_profile(void) { return &s_prof; }

void drift_profile_set(const drift_profile_t *p)
{
    bool model_change = s_prof_initialized && p &&
                        p->model_rev != s_prof.model_rev;
    if (p) s_prof = *p;
    s_prof_initialized = true;
    if (model_change) {
        sot_mark_stale(false);
    }
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
    int64_t local_s;
    if (!drift_filename_local(name, &local_s)) return false;
    /* "YYYYMMDD_HHMMSS_*" → "YYYY-MM-DDTHH:MM:SS"                    */
    snprintf(out, cap, "%.4s-%.2s-%.2sT%.2s:%.2s:%.2s",
             name, name + 4, name + 6, name + 9, name + 11, name + 13);
    return true;
}

/* ---------------- filename interpretation (update3 §6) ------------ */

static bool s_tz_confirmed;
static bool s_tz_valid;
static int32_t s_tz_offset_s;
static char s_tz_rule[24];
static bool s_tz_initialized;

static bool leap_year(int y)
{
    return !(y % 4) && ((y % 100) || !(y % 400));
}

static int mdays(int y, int m)
{
    static const int d[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return m == 2 ? d[1] + leap_year(y) : d[m - 1];
}

static int parse_uint(const char **p, int maxd)
{
    int v = 0, n = 0;
    while (isdigit((unsigned char)**p) && n < maxd) {
        v = v * 10 + (**p - '0');
        (*p)++;
        n++;
    }
    return n ? v : -1;
}

/* Fixed-offset POSIX TZ subset: UTC0, and NAME±H[:MM[:SS]] where the
 * POSIX sign means seconds west of Greenwich. DST rules are deliberately
 * not guessed; they make filename-origin interpretation unavailable.  */
static bool tz_fixed_offset(const char *rule, int32_t *offset_s)
{
    if (!rule || !rule[0] || strchr(rule, ',')) return false;
    if (!strcmp(rule, "UTC0") || !strcmp(rule, "GMT0")) {
        *offset_s = 0;
        return true;
    }
    const char *p = rule;
    while (*p && *p != '+' && *p != '-') p++;
    if (!*p) return false;
    int sign = *p++ == '-' ? -1 : 1;
    int h = parse_uint(&p, 2);
    if (h < 0 || h > 14) return false;
    int m = 0, s = 0;
    if (*p == ':') {
        p++;
        m = parse_uint(&p, 2);
        if (m < 0 || m > 59) return false;
        if (*p == ':') {
            p++;
            s = parse_uint(&p, 2);
            if (s < 0 || s > 59) return false;
        }
    }
    if (*p) return false;
    *offset_s = -sign * (h * 3600 + m * 60 + s);
    return true;
}

void drift_timezone_set(const char *rule, bool confirmed)
{
    int32_t off;
    bool valid = tz_fixed_offset(rule, &off);
    bool changed = s_tz_initialized &&
                   (confirmed != s_tz_confirmed || valid != s_tz_valid ||
                   (valid && off != s_tz_offset_s) ||
                   strncmp(rule ? rule : "", s_tz_rule,
                           sizeof(s_tz_rule)) != 0);
    s_tz_confirmed = confirmed;
    s_tz_valid = valid;
    s_tz_offset_s = s_tz_valid ? off : 0;
    strncpy(s_tz_rule, rule ? rule : "", sizeof(s_tz_rule) - 1);
    s_tz_rule[sizeof(s_tz_rule) - 1] = '\0';
    s_tz_initialized = true;
    if (changed) {
        /* filename-derived SOT depends on the configured clock
         * interpretation; a rule change makes stored bounds stale.   */
        sot_mark_stale(true);
    }
}

/* Strict numeric filename parser: exactly YYYYMMDD_HHMMSS followed by
 * '_' and a supported file-class suffix.                                  */
bool drift_filename_local(const char *name, int64_t *local_s)
{
    if (!name || !local_s || strlen(name) < 16 || name[8] != '_' ||
        name[15] != '_') {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        if (!isdigit((unsigned char)name[i])) return false;
    }
    for (int i = 9; i < 15; i++) {
        if (!isdigit((unsigned char)name[i])) return false;
    }
    int y = (name[0]-'0')*1000 + (name[1]-'0')*100 +
            (name[2]-'0')*10 + name[3]-'0';
    int mo = (name[4]-'0')*10 + name[5]-'0';
    int d = (name[6]-'0')*10 + name[7]-'0';
    int h = (name[9]-'0')*10 + name[10]-'0';
    int mi = (name[11]-'0')*10 + name[12]-'0';
    int s = (name[13]-'0')*10 + name[14]-'0';
    if (y < 1999 || y > 2100 || mo < 1 || mo > 12 ||
        d < 1 || d > mdays(y, mo) || h > 23 || mi > 59 || s > 59) {
        return false;
    }
    if (drift_beacon_class(name) == DRIFT_BC_NONE) {
        return false;
    }
    *local_s = civil_seconds(y, mo, d, h, mi, s);
    return true;
}

static bool filename_device_interval(const char *name,
                                     int64_t *lo_us, int64_t *hi_us)
{
    int64_t local_s;
    if (!drift_filename_local(name, &local_s)) return false;
    if (!s_tz_confirmed || !s_tz_valid) return false;
    int64_t f_us = (local_s - (int64_t)s_tz_offset_s) * 1000000LL;
    switch ((drift_filename_enc_t)drift_profile()->filename_encoding) {
    case DRIFT_ENC_TRUNCATED:
        *lo_us = f_us;
        *hi_us = f_us + 1000000;
        break;
    case DRIFT_ENC_ROUNDED:
        *lo_us = f_us - 500000;
        *hi_us = f_us + 500000;
        break;
    default:
        *lo_us = f_us - 500000;
        *hi_us = f_us + 1000000;
        break;
    }
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

bool drift_live_resume(const char *day)
{
    /* Reactivate a closed session when the SAME day folder resumes
     * writing — an AS11 pause/resume or a transient monitoring gap is
     * not a new observation domain: the card clock and the monotonic
     * map are unchanged, so retained events remain valid bounds (a
     * genuine clock step contradicts them honestly via DF_STEP /
     * DF_CONTRADICTION segmentation). The only cost is that this
     * feed's prev-scan bound dates from before the gap — a wider,
     * still-correct interval (update2 §3.5). A different day is a
     * different session entirely.                                   */
    if (!s_closed || s_live || !day ||
        strncmp(s_live_sess.day, day, sizeof(s_live_sess.day))) {
        return false;
    }
    s_closed = false;
    s_live = true;
    return true;
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
static void beacon_reset(drift_session_t *s, const char *name, int prio)
{
    /* A new same-day filename is a new session origin: filename and
     * periodic evidence must not leak across it. Older raw intervals
     * remain in the bounded event array as prior segment history.     */
    strncpy(s->beacon, name, sizeof(s->beacon) - 1);
    s->beacon[sizeof(s->beacon) - 1] = '\0';
    s->beacon_prio = prio;
    s->have_tuple = false;
    s->fname_known = filename_device_interval(name, &s->fname_lo_us,
                                                &s->fname_hi_us);
    if (!s->fname_known) {
        s->flag_bits |= DF_TZ_UNRESOLVED;
    }
    s->n_create_ev = 0;
    s->n_periodic_ev = 0;
    s->periodic_cycle_valid = false;
    s->create_lo_us = s->create_hi_us = 0;
    s->periodic_lo_us = s->periodic_hi_us = 0;
}

static void pick_beacon(drift_session_t *s, const ezs_entry_t *e, int n)
{
    const ezs_entry_t *sel = NULL;
    for (int i = 0; i < n; i++) {
        if (e[i].is_dir) continue;
        drift_beacon_t bc = drift_beacon_class(e[i].name);
        if (bc == DRIFT_BC_NONE || bc == DRIFT_BC_ANNOT) continue;
        if (!sel) {
            sel = &e[i];
            continue;
        }
        int pa = bc_prio(bc);
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
        if (s->beacon[0] && strcmp(s->beacon, sel->name)) {
            /* session-relative filename evidence belongs to the old
             * beacon; keep mtime events only as old-segment history   */
            s->n_splits++;
        }
        beacon_reset(s, sel->name, sp);
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

/* Cadence diagnostic: the stamp delta feeds the published flush
 * period. Only deltas in a plausible one-flush range count — a missed
 * scan merges two flushes into one larger delta. Phase prediction for
 * polling is the scheduler's job (update2), not this estimator's.    */
static void note_cadence(drift_session_t *s, int64_t ddev_s)
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
    }
}

static void push_ev(drift_session_t *s, drift_ev_kind_t kind,
                    int64_t stamp_s, int64_t obs_lo, int64_t obs_hi)
{
    if (s->n_ev >= DRIFT_EV_CAP) {
        s->ev_overflow = true;
        s->flag_bits |= DF_EV_OVERFLOW;
        return;
    }
    s->ev[s->n_ev++] = (drift_ev_t){
        .obs_lo_us = obs_lo, .obs_hi_us = obs_hi,
        .stamp_s = stamp_s, .kind = (uint8_t)kind,
    };
}

int drift_feed_scan(drift_session_t *s, const char *day,
                    const ezs_entry_t *entries, int n,
                    const cat_diff_t *diff,
                    int64_t req_start_us, int64_t resp_end_us)
{
    if (!s || !day || strcmp(day, s->day)) {
        return 0;
    }
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
                                prev, resp_end_us);
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
                note_cadence(s, m_new - m_old);
                if (prev) {
                    push_ev(s, DRIFT_EV_TRANSITION, m_new,
                            prev, resp_end_us);
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
            note_cadence(s, d);
            if (prev) {
                push_ev(s, DRIFT_EV_TRANSITION, ms,
                        prev, resp_end_us);
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

    r.source = s->preferred_source;
    r.quality = s->tier == ACC_ACCURATE ? DRIFT_Q_QUALIFIED :
                s->est_bound_valid ? DRIFT_Q_PROVISIONAL : DRIFT_Q_NONE;
    snprintf(r.model_id, sizeof(r.model_id), "u3_profile_%u",
             (unsigned)drift_profile()->model_rev);
    r.assumptions = assumption_mask(s, drift_profile());
    strncpy(r.source_reason, s->source_reason, sizeof(r.source_reason) - 1);
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
    r.candidates[0].source = DRIFT_SRC_MTIME;
    r.candidates[1].source = DRIFT_SRC_FNAME_CREATE;
    r.candidates[2].source = DRIFT_SRC_FNAME_PERIODIC;
    if (s->n_windows) {
        r.candidates[0].available = true;
        r.candidates[0].lo_ms = us_floor_ms(s->mtime_lo_us);
        r.candidates[0].hi_ms = us_ceil_ms(s->mtime_hi_us);
        r.candidates[0].half_ms =
            (r.candidates[0].hi_ms - r.candidates[0].lo_ms) / 2;
        r.candidates[0].observations = s->n_windows;
        snprintf(r.candidates[0].reason,
                 sizeof(r.candidates[0].reason), "available");
    } else {
        snprintf(r.candidates[0].reason,
                 sizeof(r.candidates[0].reason), "no_bounded_event");
    }
    if (s->n_create_ev) {
        r.candidates[1].available = true;
        r.candidates[1].lo_ms = us_floor_ms(s->create_lo_us);
        r.candidates[1].hi_ms = us_ceil_ms(s->create_hi_us);
        r.candidates[1].half_ms =
            (r.candidates[1].hi_ms - r.candidates[1].lo_ms) / 2;
        r.candidates[1].observations = s->n_create_ev;
        snprintf(r.candidates[1].reason,
                 sizeof(r.candidates[1].reason), "available");
    } else {
        snprintf(r.candidates[1].reason,
                 sizeof(r.candidates[1].reason), "%s",
                 !s->fname_known ? "filename_unresolved" :
                 !drift_profile()->filename_origin_bounded ?
                 "creation_delay_unbounded" : "no_creation_event");
    }
    if (s->periodic_cycle_valid) {
        r.candidates[2].available = true;
        r.candidates[2].lo_ms = us_floor_ms(s->periodic_lo_us);
        r.candidates[2].hi_ms = us_ceil_ms(s->periodic_hi_us);
        r.candidates[2].half_ms =
            (r.candidates[2].hi_ms - r.candidates[2].lo_ms) / 2;
        r.candidates[2].observations = s->n_periodic_ev;
        snprintf(r.candidates[2].reason,
                 sizeof(r.candidates[2].reason), "available");
    } else {
        snprintf(r.candidates[2].reason,
                 sizeof(r.candidates[2].reason), "%s",
                 !s->fname_known ? "filename_unresolved" :
                 !drift_profile()->filename_periodic_bounded ?
                 "period_relation_unbounded" :
                 (s->flag_bits & DF_CYCLE_AMBIG) ? "cycle_ambiguous" :
                 "no_periodic_event");
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
        if (drift_map_utc(s->ev[0].obs_lo_us, &lo, &hi) &&
            drift_map_utc(s->ev[0].obs_hi_us, &lo2, &hi2)) {
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
    if (fb & DF_TZ_UNRESOLVED) flag(r.flags, sizeof(r.flags),
                                    "timezone_unresolved");
    if (fb & DF_FNAME_ORIGIN)  flag(r.flags, sizeof(r.flags),
                                    "filename_origin_unresolved");
    if (fb & DF_CYCLE_AMBIG)   flag(r.flags, sizeof(r.flags),
                                    "cycle_ambiguous");
    if (fb & DF_SOURCE_DISAGREE) flag(r.flags, sizeof(r.flags),
                                    "source_disagreement");

    s_pub_seq++;
    s_pub = r;
    s_pub_seq++;
    s_pub_valid = true;
}

/* ---------------- SOT (U6/update3) ---------------- */

static drift_sot_t s_sot[DRIFT_SOT_N];
static bool        s_sot_dirty[DRIFT_SOT_N];
static int64_t     s_sot_last_store_us[DRIFT_SOT_N];
static int64_t     s_sot_committed_half[DRIFT_SOT_N];
static uint32_t    s_sot_persist_seq[DRIFT_SOT_N];
static char        s_card_fw[24];

const char *drift_source_name(uint8_t source)
{
    switch (source) {
    case DRIFT_SRC_MTIME: return "mtime";
    case DRIFT_SRC_FNAME_CREATE: return "filename_creation";
    case DRIFT_SRC_FNAME_PERIODIC: return "filename_periodic";
    default: return "none";
    }
}

const char *drift_quality_name(uint8_t quality)
{
    switch (quality) {
    case DRIFT_Q_PROVISIONAL: return "provisional";
    case DRIFT_Q_QUALIFIED: return "qualified";
    default: return "none";
    }
}

bool drift_sot_slot_view(drift_sot_slot_t slot, drift_sot_t *out)
{
    if (!out || slot >= DRIFT_SOT_N || !s_sot[slot].valid) return false;
    *out = s_sot[slot];
    return true;
}

bool drift_sot_view(drift_sot_t *out)
{
    return drift_sot_slot_view(DRIFT_SOT_QUALIFIED, out) &&
           !out->stale;
}

bool drift_sot_best(drift_sot_t *out)
{
    if (drift_sot_view(out)) return true;
    return drift_sot_slot_view(DRIFT_SOT_PROVISIONAL, out) &&
           !out->stale;
}

bool drift_sot_dirty(void)
{
    return s_sot_dirty[DRIFT_SOT_PROVISIONAL] ||
           s_sot_dirty[DRIFT_SOT_QUALIFIED];
}

static void sot_anchor_ready(void)
{
    /* Restored records stop being freshness-unknown once a trusted
     * current-time anchor establishes this boot's monotonic domain. */
    for (int i = 0; i < DRIFT_SOT_N; i++) {
        if (s_sot[i].valid && !s_sot[i].stale) {
            s_sot[i].freshness_unknown = false;
            s_sot[i].applicable = true;
        }
    }
}

static void sot_mark_stale(bool filename_only)
{
    for (int i = 0; i < DRIFT_SOT_N; i++) {
        if (!s_sot[i].valid ||
            (filename_only && s_sot[i].source == DRIFT_SRC_MTIME)) {
            continue;
        }
        s_sot[i].stale = true;
        s_sot[i].applicable = false;
        s_sot[i].dirty = true;
        s_sot_dirty[i] = true;
    }
    drift_jrn("sot", "\"e\":\"stale\",\"filename_only\":%s",
              filename_only ? "true" : "false");
}

static uint64_t assumption_mask(const drift_session_t *s,
                                const drift_profile_t *p)
{
    uint64_t m = 0;
    if (!p->cache_bound_us) m |= DRIFT_A_ZERO_CACHE;
    if (!p->latency_lo_us && !p->latency_hi_us) {
        m |= DRIFT_A_IMMEDIATE_VISIBLE;
    }
    if (s->preferred_source != DRIFT_SRC_MTIME) {
        m |= DRIFT_A_TZ_FIXED;
    }
    if (s->preferred_source == DRIFT_SRC_FNAME_CREATE &&
        p->filename_origin_bounded) {
        m |= DRIFT_A_FILENAME_ORIGIN;
    }
    if (s->preferred_source == DRIFT_SRC_FNAME_PERIODIC &&
        p->filename_periodic_bounded) {
        m |= DRIFT_A_FILENAME_PERIODIC;
    }
    return m;
}

static bool source_validated(const drift_profile_t *p, uint8_t source)
{
    switch (source) {
    case DRIFT_SRC_MTIME: return p->stamp_channel_validated;
    case DRIFT_SRC_FNAME_CREATE: return p->filename_create_validated;
    case DRIFT_SRC_FNAME_PERIODIC: return p->filename_periodic_validated;
    default: return false;
    }
}

static void sot_fill(drift_sot_t *r, const drift_session_t *s,
                     const drift_profile_t *p, uint8_t quality,
                     int64_t now_us)
{
    bool ok;
    int64_t lo = us_floor_ms(s->off_lo_us);
    int64_t hi = us_ceil_ms(s->off_hi_us);
    memset(r, 0, sizeof(*r));
    r->valid = true;
    r->applicable = true;
    r->origin = DRIFT_SOT_SESSION;
    r->dirty = true;
    r->clock_drift_ms = (lo + hi) / 2;
    r->lo_ms = lo;
    r->hi_ms = hi;
    r->accuracy_half_ms = (hi - lo) / 2;
    r->current_half_ms = r->accuracy_half_ms;
    r->computed_at_ms = map_mid_ms(now_us, &ok);
    if (!ok) r->computed_at_ms = 0;
    r->reference_at_ms = map_mid_ms(s->ref_mono_us, &ok);
    if (!ok) r->reference_at_ms = 0;
    r->ref_mono_us = s->ref_mono_us;
    r->observations = s->n_windows;
    r->model_rev = p->model_rev;
    snprintf(r->model_id, sizeof(r->model_id), "u3_profile_%u",
             (unsigned)p->model_rev);
    r->quality = quality;
    r->source = s->preferred_source;
    r->filename_encoding = p->filename_encoding;
    r->filename_period_us = 60000000;
    r->assumptions = assumption_mask(s, p);
    r->cache_bound_us = p->cache_bound_us;
    r->latency_lo_us = p->latency_lo_us;
    r->latency_hi_us = p->latency_hi_us;
    r->jitter_us = p->jitter_us;
    r->dev_rate_ppm = p->dev_rate_ppm;
    r->fname_lo_us = s->preferred_source == DRIFT_SRC_FNAME_PERIODIC
                   ? p->fname_period_lo_us : p->fname_create_lo_us;
    r->fname_hi_us = s->preferred_source == DRIFT_SRC_FNAME_PERIODIC
                   ? p->fname_period_hi_us : p->fname_create_hi_us;
    strncpy(r->source_reason, s->source_reason,
            sizeof(r->source_reason) - 1);
    strncpy(r->card_fw, s_card_fw, sizeof(r->card_fw) - 1);
    drift_sot_slot_t slot = quality == DRIFT_Q_QUALIFIED
                          ? DRIFT_SOT_QUALIFIED : DRIFT_SOT_PROVISIONAL;
    r->persisted_seq = s_sot_persist_seq[slot];
}

/* Replacement is evidence-driven: materially tighter (≥250 ms half-width
 * reduction), a non-overlapping materially different bound, or a first
 * trusted bound after reboot can replace the same quality slot. A
 * provisional record can never touch the qualified slot.              */
static void sot_offer(drift_sot_slot_t slot, const drift_sot_t *cand)
{
    drift_sot_t *cur = &s_sot[slot];
    bool replace = !cur->valid || cur->stale;
    if (!replace && cand->accuracy_half_ms + 250 <=
        cur->accuracy_half_ms) {
        replace = true;
    }
    if (!replace && (cand->hi_ms < cur->lo_ms ||
                     cand->lo_ms > cur->hi_ms)) {
        replace = true;
    }
    if (replace) {
        uint32_t seq = cur->persisted_seq;
        *cur = *cand;
        cur->persisted_seq = seq;
        cur->dirty = true;
        s_sot_dirty[slot] = true;
        drift_jrn("sot", "\"e\":\"promote\",\"q\":\"%s\",\"src\":\"%s\","
                         "\"half\":%lld,\"n\":%u",
                  drift_quality_name(cand->quality),
                  drift_source_name(cand->source),
                  (long long)cand->accuracy_half_ms,
                  (unsigned)cand->observations);
    } else {
        drift_jrn("sot", "\"e\":\"reject\",\"q\":\"%s\",\"src\":\"%s\","
                         "\"half\":%lld",
                  drift_quality_name(cand->quality),
                  drift_source_name(cand->source),
                  (long long)cand->accuracy_half_ms);
    }
}

static void sot_consider(const drift_session_t *s, int64_t now_us)
{
    const drift_profile_t *p = drift_profile();
    if (s->flag_bits & (DF_BACKWARD | DF_STEP)) {
        /* A detected device-clock discontinuity invalidates bounds
         * re-referenced from an older clock segment.                  */
        sot_mark_stale(false);
        return;
    }
    bool source_unresolved =
        (s->preferred_source == DRIFT_SRC_FNAME_CREATE &&
         (s->flag_bits & DF_FNAME_ORIGIN)) ||
        (s->preferred_source == DRIFT_SRC_FNAME_PERIODIC &&
         (s->flag_bits & (DF_FNAME_ORIGIN | DF_CYCLE_AMBIG)));
    if (!s->est_bound_valid || !s->anchor_used || s->n_segments != 1 ||
        source_unresolved ||
        (s->flag_bits & (DF_CONTRADICTION | DF_SOURCE_DISAGREE))) {
        return;
    }
    int64_t half_us = (s->off_hi_us - s->off_lo_us) / 2;
    bool validated = source_validated(p, s->preferred_source);
    drift_sot_t cand;
    if (validated && s->n_windows >= 2 &&
        half_us <= (int64_t)p->accurate_half_us) {
        sot_fill(&cand, s, p, DRIFT_Q_QUALIFIED, now_us);
        if (cand.computed_at_ms && cand.reference_at_ms) {
            sot_offer(DRIFT_SOT_QUALIFIED, &cand);
        }
    } else if (s->n_windows >= 3 &&
               half_us <= (int64_t)p->provisional_half_us) {
        sot_fill(&cand, s, p, DRIFT_Q_PROVISIONAL, now_us);
        if (cand.computed_at_ms && cand.reference_at_ms) {
            sot_offer(DRIFT_SOT_PROVISIONAL, &cand);
        }
    }
}

bool drift_sot_apply(const drift_sot_t *s, int64_t now_us,
                     int64_t *lo_ms, int64_t *hi_ms)
{
    if (!s || !s->valid || s->freshness_unknown || s->stale ||
        !s->applicable || !lo_ms || !hi_ms) {
        return false;
    }
    int64_t ulo, uhi;
    if (!drift_map_utc(now_us, &ulo, &uhi)) return false;
    int64_t elapsed_ms = uhi - s->reference_at_ms;
    if (elapsed_ms < 0) elapsed_ms = 0;
    int64_t widen_ms = elapsed_ms * (int64_t)s->dev_rate_ppm / 1000000;
    *lo_ms = s->lo_ms - widen_ms;
    *hi_ms = s->hi_ms + widen_ms;
    return true;
}

void drift_card_fw_note(const char *fw)
{
    if (!fw || !fw[0]) return;
    bool changed = s_card_fw[0] && strcmp(s_card_fw, fw);
    for (int i = 0; i < DRIFT_SOT_N; i++) {
        if (s_sot[i].valid && s_sot[i].card_fw[0] &&
            strcmp(s_sot[i].card_fw, fw)) {
            s_sot[i].stale = true;
            s_sot[i].applicable = false;
            s_sot[i].dirty = true;
            s_sot_dirty[i] = true;
        }
    }
    if (changed) {
        /* card firmware/identity changed underneath us — persisted
         * characterization is historical for another binding.        */
        sot_mark_stale(false);
    }
    strncpy(s_card_fw, fw, sizeof(s_card_fw) - 1);
    s_card_fw[sizeof(s_card_fw) - 1] = '\0';
}

/* True instant when a write became list-visible, derived from the raw
 * observation bounds and the active timing profile. The card cache is
 * a one-sided staleness extension of the unchanged edge; visibility
 * delay is a separate model term.                                 */
static bool event_window(const drift_session_t *s,
                         const drift_profile_t *p,
                         const drift_ev_t *e,
                         int64_t *w_lo, int64_t *w_hi)
{
    int64_t alo, ahi, blo, bhi;
    if (!drift_map_utc(e->obs_lo_us, &alo, &ahi) ||
        !drift_map_utc(e->obs_hi_us, &blo, &bhi)) {
        return false;
    }
    (void)ahi; (void)blo;
    *w_lo = alo - (int64_t)s->cache_bound_us
          - (int64_t)p->latency_hi_us - (int64_t)s->jitter_us;
    *w_hi = bhi - (int64_t)p->latency_lo_us
          + (int64_t)s->jitter_us;
    return true;
}

static bool iv_intersect(int64_t *lo, int64_t *hi,
                         int64_t nlo, int64_t nhi)
{
    if (nlo > *lo) *lo = nlo;
    if (nhi < *hi) *hi = nhi;
    return *lo <= *hi;
}

/* Resolve the periodic filename cycle by intersecting the bounded
 * filename-origin model with the event's own 2-second mtime bin. A
 * unique compatible integer is required; a nearest guess is not used. */
static bool filename_cycle(const drift_session_t *s,
                           const drift_profile_t *p,
                           int64_t stamp_s, uint32_t *cycle)
{
    if (!s->fname_known || !p->filename_periodic_bounded ||
        p->fname_period_lo_us > p->fname_period_hi_us) {
        return false;
    }
    int64_t rate = 60000000LL * (int64_t)p->dev_rate_ppm / 1000000;
    int64_t p_lo = 60000000LL - rate;
    int64_t p_hi = 60000000LL + rate;
    int64_t m_lo = stamp_s * 1000000LL;
    int64_t m_hi = m_lo + 2000000LL;
    int found = 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i <= 1024; i++) {
        int64_t dlo = s->fname_lo_us + i * p_lo + p->fname_period_lo_us;
        int64_t dhi = s->fname_hi_us + i * p_hi + p->fname_period_hi_us;
        if (dlo <= m_hi && dhi >= m_lo) {
            found++;
            k = i;
            if (found > 1) break;
        }
    }
    if (found == 1) {
        *cycle = k;
        return true;
    }
    return false;
}

void drift_sess_eval(drift_session_t *s, int64_t now_us)
{
    const drift_profile_t *p = drift_profile();
    /* eval-recomputed flags reset; feed-time flags persist           */
    s->flag_bits &= ~(DF_NO_ANCHOR | DF_BACKWARD | DF_STEP |
                      DF_CONTRADICTION | DF_UNVALIDATED |
                      DF_TZ_UNRESOLVED | DF_FNAME_ORIGIN |
                      DF_CYCLE_AMBIG | DF_SOURCE_DISAGREE);

    bool have_acc = false;
    int64_t alo = 0, ahi = 0, ref = 0;
    int nw = 0;
    uint32_t seg = 0, splits = s->n_splits;
    bool anch = false;
    bool have_prev = false;
    int64_t prev_st = 0, prev_resp = 0;
    int64_t cr_lo = 0, cr_hi = 0;
    int n_create = 0;
    int64_t pe_lo = 0, pe_hi = 0;
    int n_periodic = 0;
    bool periodic_bad = false;

    for (int i = 0; i < s->n_ev; i++) {
        const drift_ev_t *e = &s->ev[i];
        if (have_prev) {
            if (e->stamp_s < prev_st) {
                seg++;
                splits++;
                have_acc = false;
                s->flag_bits |= DF_BACKWARD;
            } else {
                int64_t ddev  = (e->stamp_s - prev_st) * 1000000LL;
                int64_t dmono = e->obs_hi_us - prev_resp;
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
        int64_t wlo, whi;
        if (!event_window(s, p, e, &wlo, &whi)) {
            s->flag_bits |= DF_NO_ANCHOR;
        } else {
            anch = true;
            int64_t lo = wlo - (e->stamp_s + 2) * 1000000LL;
            int64_t hi = whi - e->stamp_s * 1000000LL;
            if (!have_acc) {
                alo = lo;
                ahi = hi;
                nw = 1;
                have_acc = true;
            } else {
                int64_t dt = e->obs_hi_us - ref;
                if (dt > 0) {
                    int64_t widen = dt * (int64_t)p->dev_rate_ppm / 1000000;
                    alo -= widen;
                    ahi += widen;
                }
                if (!iv_intersect(&alo, &ahi, lo, hi)) {
                    seg++;
                    splits++;
                    s->flag_bits |= DF_CONTRADICTION;
                    alo = lo;
                    ahi = hi;
                    nw = 1;
                } else {
                    nw++;
                }
            }
            ref = e->obs_hi_us;

            if (e->kind == DRIFT_EV_CREATE) {
                if (s->fname_known && p->filename_origin_bounded &&
                    p->fname_create_lo_us <= p->fname_create_hi_us) {
                    int64_t tlo = wlo - p->fname_create_hi_us;
                    int64_t thi = whi - p->fname_create_lo_us;
                    int64_t olo = tlo - s->fname_hi_us;
                    int64_t ohi = thi - s->fname_lo_us;
                    if (!n_create) {
                        cr_lo = olo;
                        cr_hi = ohi;
                    } else if (!iv_intersect(&cr_lo, &cr_hi, olo, ohi)) {
                        periodic_bad = true;
                    }
                    n_create++;
                }
            } else if (e->kind == DRIFT_EV_TRANSITION &&
                       s->fname_known && p->filename_periodic_bounded &&
                       p->fname_period_lo_us <= p->fname_period_hi_us) {
                uint32_t k;
                if (!filename_cycle(s, p, e->stamp_s, &k)) {
                    periodic_bad = true;
                    s->flag_bits |= DF_CYCLE_AMBIG;
                } else {
                    int64_t rate = 60000000LL *
                                   (int64_t)p->dev_rate_ppm / 1000000;
                    int64_t pd_lo = 60000000LL - rate;
                    int64_t pd_hi = 60000000LL + rate;
                    int64_t dlo = s->fname_lo_us + k * pd_lo +
                                  p->fname_period_lo_us;
                    int64_t dhi = s->fname_hi_us + k * pd_hi +
                                  p->fname_period_hi_us;
                    int64_t olo = wlo - dhi;
                    int64_t ohi = whi - dlo;
                    if (!n_periodic) {
                        pe_lo = olo;
                        pe_hi = ohi;
                    } else if (!iv_intersect(&pe_lo, &pe_hi, olo, ohi)) {
                        periodic_bad = true;
                        s->flag_bits |= DF_CONTRADICTION;
                    }
                    n_periodic++;
                }
            }
        }
        have_prev = true;
        prev_st = e->stamp_s;
        prev_resp = e->obs_hi_us;
    }

    s->mtime_lo_us = alo;
    s->mtime_hi_us = ahi;
    s->create_lo_us = cr_lo;
    s->create_hi_us = cr_hi;
    s->periodic_lo_us = pe_lo;
    s->periodic_hi_us = pe_hi;
    s->n_create_ev = n_create;
    s->n_periodic_ev = n_periodic;
    s->periodic_cycle_valid = n_periodic > 0 && !periodic_bad;

    if (!s->fname_known) s->flag_bits |= DF_TZ_UNRESOLVED;
    if (!p->filename_origin_bounded) s->flag_bits |= DF_FNAME_ORIGIN;
    if (!p->filename_periodic_bounded) s->flag_bits |= DF_CYCLE_AMBIG;

    /* Source selection keeps mtime as fallback. A filename candidate
     * wins only when resolved and materially narrower (250 ms).       */
    int64_t mw = have_acc ? ahi - alo : INT64_MAX;
    int64_t cw = n_create ? cr_hi - cr_lo : INT64_MAX;
    int64_t pw = s->periodic_cycle_valid ? pe_hi - pe_lo : INT64_MAX;
    uint8_t src = DRIFT_SRC_MTIME;
    strcpy(s->source_reason, "mtime_fallback");
    if (pw != INT64_MAX && pw + 250000 < mw) {
        src = DRIFT_SRC_FNAME_PERIODIC;
        strcpy(s->source_reason, "filename_periodic_narrower");
    } else if (cw != INT64_MAX && cw + 250000 < mw) {
        src = DRIFT_SRC_FNAME_CREATE;
        strcpy(s->source_reason, "filename_creation_narrower");
    }
    bool disagree = false;
    if (have_acc && n_create &&
        (cr_lo > ahi || cr_hi < alo)) {
        disagree = true;
    }
    if (have_acc && s->periodic_cycle_valid &&
        (pe_lo > ahi || pe_hi < alo)) {
        disagree = true;
    }
    if (disagree) {
        s->flag_bits |= DF_SOURCE_DISAGREE;
        src = DRIFT_SRC_MTIME;
        strcpy(s->source_reason, "source_disagreement_mtime");
    }
    s->preferred_source = src;
    switch (src) {
    case DRIFT_SRC_FNAME_CREATE:
        s->est_bound_valid = n_create > 0;
        s->off_lo_us = cr_lo;
        s->off_hi_us = cr_hi;
        s->n_windows = n_create;
        break;
    case DRIFT_SRC_FNAME_PERIODIC:
        s->est_bound_valid = s->periodic_cycle_valid;
        s->off_lo_us = pe_lo;
        s->off_hi_us = pe_hi;
        s->n_windows = n_periodic;
        break;
    default:
        s->est_bound_valid = have_acc;
        s->off_lo_us = alo;
        s->off_hi_us = ahi;
        s->n_windows = nw;
        break;
    }
    s->ref_mono_us = ref;
    s->n_segments = have_acc ? seg + 1 : seg;
    s->n_splits = splits;
    s->anchor_used = anch;

    int64_t half = s->est_bound_valid
                 ? (s->off_hi_us - s->off_lo_us) / 2 : 0;
    bool src_validated =
        (src == DRIFT_SRC_MTIME && p->stamp_channel_validated) ||
        (src == DRIFT_SRC_FNAME_CREATE && p->filename_create_validated) ||
        (src == DRIFT_SRC_FNAME_PERIODIC && p->filename_periodic_validated);
    int need_obs = src == DRIFT_SRC_MTIME ? 2 : 2;
    if (s->est_bound_valid && anch && s->n_windows >= need_obs &&
        src_validated && half <= (int64_t)p->accurate_half_us) {
        s->tier = ACC_ACCURATE;
    } else if (s->est_bound_valid) {
        s->tier = ACC_APPROXIMATE;
    } else {
        s->tier = ACC_NONE;
    }
    if (!src_validated) {
        s->flag_bits |= DF_UNVALIDATED;
    }

    publish(s, now_us);
    if (s == drift_live()) {
        sot_consider(s, now_us);
    }
}

/* ---------------- SOT NVS blobs (metadata partition) ---------------
 * Version 1 was a single qualified blob under "drift_sot". Version 2
 * keeps provisional and qualified records under separate keys; neither
 * slot can overwrite the other. Corrupt, truncated, or unsupported
 * records are ignored without fabricating an offset.                 */

#define SOT_KEY_LEGACY "drift_sot"
#define SOT_KEY_PROV   "drift_sot_p"
#define SOT_KEY_QUAL   "drift_sot_q"
#define SOT_MAGIC      0x534F5431u   /* "SOT1" */
#define SOT_VERSION    2

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
} sot_v1_blob_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  quality;
    uint8_t  source;
    uint8_t  encoding;
    uint8_t  stale;
    uint8_t  rsv1[3];
    uint32_t seq;
    uint32_t model_rev;
    uint64_t assumptions;
    uint16_t observations;
    uint16_t rsv2;
    int64_t  drift_ms;
    int64_t  lo_ms, hi_ms;
    int64_t  half_ms;
    int64_t  comp_ms;
    int64_t  ref_ms;
    uint32_t cache_us;
    int32_t  latency_lo_us;
    int32_t  latency_hi_us;
    uint32_t jitter_us;
    uint32_t rate_ppm;
    int32_t  fname_lo_us;
    int32_t  fname_hi_us;
    int64_t  period_us;
    char     reason[24];
    char     fw[24];
    uint32_t crc32;
} sot_v2_blob_t;

static const char *sot_key(drift_sot_slot_t slot)
{
    return slot == DRIFT_SOT_QUALIFIED ? SOT_KEY_QUAL : SOT_KEY_PROV;
}

static bool sot_record_sane(const sot_v2_blob_t *b,
                            drift_sot_slot_t slot)
{
    uint8_t q = slot == DRIFT_SOT_QUALIFIED ? DRIFT_Q_QUALIFIED
                                          : DRIFT_Q_PROVISIONAL;
    return b->magic == SOT_MAGIC && b->version == SOT_VERSION &&
           b->quality == q &&
           b->source <= DRIFT_SRC_FNAME_PERIODIC &&
           b->encoding <= DRIFT_ENC_ROUNDED &&
           b->lo_ms <= b->hi_ms && b->half_ms >= 0 &&
           b->ref_ms != 0 && b->comp_ms != 0 &&
           b->cache_us <= 10000000 && b->latency_lo_us >= 0 &&
           b->latency_hi_us >= b->latency_lo_us &&
           b->jitter_us <= 10000000 && b->rate_ppm <= 10000 &&
           b->fname_lo_us <= b->fname_hi_us;
}

static void sot_from_blob(drift_sot_slot_t slot, const sot_v2_blob_t *b)
{
    drift_sot_t *r = &s_sot[slot];
    memset(r, 0, sizeof(*r));
    r->valid = true;
    r->stale = b->stale != 0;
    r->freshness_unknown = true;   /* no trusted current map yet       */
    r->clock_drift_ms = b->drift_ms;
    r->lo_ms = b->lo_ms;
    r->hi_ms = b->hi_ms;
    r->accuracy_half_ms = b->half_ms;
    r->computed_at_ms = b->comp_ms;
    r->reference_at_ms = b->ref_ms;
    r->quality = b->quality;
    r->source = b->source;
    r->observations = b->observations;
    r->filename_encoding = b->encoding;
    r->filename_period_us = b->period_us;
    r->assumptions = b->assumptions;
    r->cache_bound_us = b->cache_us;
    r->latency_lo_us = b->latency_lo_us;
    r->latency_hi_us = b->latency_hi_us;
    r->jitter_us = b->jitter_us;
    r->dev_rate_ppm = b->rate_ppm;
    r->fname_lo_us = b->fname_lo_us;
    r->fname_hi_us = b->fname_hi_us;
    r->model_rev = b->model_rev;
    snprintf(r->model_id, sizeof(r->model_id), "u3_profile_%u",
             (unsigned)b->model_rev);
    r->persisted_seq = b->seq;
    r->origin = DRIFT_SOT_NVS;
    memcpy(r->source_reason, b->reason, sizeof(r->source_reason));
    r->source_reason[sizeof(r->source_reason) - 1] = '\0';
    memcpy(r->card_fw, b->fw, sizeof(r->card_fw));
    r->card_fw[sizeof(r->card_fw) - 1] = '\0';
    s_sot_persist_seq[slot] = b->seq;
    s_sot_committed_half[slot] = b->half_ms;
}

static esp_err_t sot_load_slot(nvs_handle_t h, drift_sot_slot_t slot)
{
    sot_v2_blob_t b;
    size_t want = sizeof(b);
    esp_err_t err = nvs_get_blob(h, sot_key(slot), &b, &want);
    if (err != ESP_OK) return err;
    if (want != sizeof(b) || !sot_record_sane(&b, slot) ||
        esp_crc32_le(0, (const uint8_t *)&b,
                     offsetof(sot_v2_blob_t, crc32)) != b.crc32) {
        ESP_LOGW(TAG, "%s invalid — ignored", sot_key(slot));
        return ESP_ERR_INVALID_RESPONSE;
    }
    sot_from_blob(slot, &b);
    s_sot_dirty[slot] = false;
    drift_jrn("sot", "\"e\":\"restore\",\"q\":\"%s\",\"seq\":%lu",
              drift_quality_name(b.quality), (unsigned long)b.seq);
    return ESP_OK;
}

static esp_err_t sot_load_legacy(nvs_handle_t h)
{
    sot_v1_blob_t b;
    size_t want = sizeof(b);
    if (nvs_get_blob(h, SOT_KEY_LEGACY, &b, &want) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (want != sizeof(b) || b.magic != SOT_MAGIC || b.version != 1 ||
        b.lo_ms > b.hi_ms || b.half_ms < 0 ||
        esp_crc32_le(0, (const uint8_t *)&b,
                     offsetof(sot_v1_blob_t, crc32)) != b.crc32) {
        ESP_LOGW(TAG, "legacy drift_sot blob invalid — ignored");
        return ESP_ERR_INVALID_RESPONSE;
    }
    drift_sot_t *r = &s_sot[DRIFT_SOT_QUALIFIED];
    memset(r, 0, sizeof(*r));
    r->valid = true;
    r->freshness_unknown = true;
    r->clock_drift_ms = b.drift_ms;
    r->lo_ms = b.lo_ms;
    r->hi_ms = b.hi_ms;
    r->accuracy_half_ms = b.half_ms;
    r->computed_at_ms = b.comp_ms;
    r->reference_at_ms = b.ref_ms;
    r->quality = DRIFT_Q_QUALIFIED;
    r->source = DRIFT_SRC_MTIME;
    r->dev_rate_ppm = s_prof.dev_rate_ppm;
    r->observations = 2;
    r->assumptions = DRIFT_A_LEGACY_MODEL;
    strncpy(r->model_id, "legacy_v1", sizeof(r->model_id) - 1);
    strncpy(r->source_reason, "legacy_v1_migration",
            sizeof(r->source_reason) - 1);
    r->origin = DRIFT_SOT_NVS;
    memcpy(r->card_fw, b.fw, sizeof(r->card_fw));
    r->card_fw[sizeof(r->card_fw) - 1] = '\0';
    r->persisted_seq = b.seq;
    s_sot_persist_seq[DRIFT_SOT_QUALIFIED] = b.seq;
    s_sot_committed_half[DRIFT_SOT_QUALIFIED] = b.half_ms;
    s_sot_dirty[DRIFT_SOT_QUALIFIED] = true;  /* migrate to v2          */
    drift_jrn("sot", "\"e\":\"migrate_v1\",\"seq\":%lu",
              (unsigned long)b.seq);
    return ESP_OK;
}

esp_err_t drift_sot_load(void)
{
    nvs_handle_t h = settings_nvs_metadata();
    if (!h) return ESP_ERR_INVALID_STATE;
    esp_err_t ep = sot_load_slot(h, DRIFT_SOT_PROVISIONAL);
    esp_err_t eq = sot_load_slot(h, DRIFT_SOT_QUALIFIED);
    if (eq == ESP_ERR_NVS_NOT_FOUND || eq == ESP_ERR_NOT_FOUND) {
        eq = sot_load_legacy(h);
    }
    return ep == ESP_OK || eq == ESP_OK ? ESP_OK : eq;
}

static esp_err_t sot_commit_slot(nvs_handle_t h, drift_sot_slot_t slot,
                                 bool force, int64_t now_us)
{
    drift_sot_t *r = &s_sot[slot];
    if (!r->valid || !s_sot_dirty[slot]) return ESP_OK;
    if (!force) {
        if (now_us - s_sot_last_store_us[slot] < 60LL * 1000 * 1000) {
            return ESP_OK;
        }
        if (s_sot_committed_half[slot] &&
            s_sot_committed_half[slot] - r->accuracy_half_ms < 250) {
            return ESP_OK;
        }
    }
    sot_v2_blob_t b;
    memset(&b, 0, sizeof(b));
    b.magic = SOT_MAGIC;
    b.version = SOT_VERSION;
    b.quality = r->quality;
    b.source = r->source;
    b.encoding = r->filename_encoding;
    b.stale = r->stale;
    b.seq = s_sot_persist_seq[slot] + 1;
    b.model_rev = r->model_rev;
    b.assumptions = r->assumptions;
    b.observations = r->observations;
    b.drift_ms = r->clock_drift_ms;
    b.lo_ms = r->lo_ms;
    b.hi_ms = r->hi_ms;
    b.half_ms = r->accuracy_half_ms;
    b.comp_ms = r->computed_at_ms;
    b.ref_ms = r->reference_at_ms;
    b.cache_us = r->cache_bound_us;
    b.latency_lo_us = r->latency_lo_us;
    b.latency_hi_us = r->latency_hi_us;
    b.jitter_us = r->jitter_us;
    b.rate_ppm = r->dev_rate_ppm;
    b.fname_lo_us = r->fname_lo_us;
    b.fname_hi_us = r->fname_hi_us;
    b.period_us = r->filename_period_us;
    strncpy(b.reason, r->source_reason, sizeof(b.reason) - 1);
    strncpy(b.fw, s_card_fw, sizeof(b.fw) - 1);
    b.crc32 = esp_crc32_le(0, (const uint8_t *)&b,
                           offsetof(sot_v2_blob_t, crc32));
    esp_err_t err = nvs_set_blob(h, sot_key(slot), &b, sizeof(b));
    if (err == ESP_OK) err = nvs_commit(h);
    r->last_error = err;
    if (err == ESP_OK) {
        s_sot_persist_seq[slot] = b.seq;
        r->persisted_seq = b.seq;
        s_sot_last_store_us[slot] = now_us;
        s_sot_committed_half[slot] = r->accuracy_half_ms;
        s_sot_dirty[slot] = false;
        r->dirty = false;
        r->last_error = ESP_OK;
        ESP_LOGI(TAG, "SOT %s persisted: seq=%lu drift=%lldms ±%lldms",
                 drift_quality_name(r->quality), (unsigned long)b.seq,
                 (long long)b.drift_ms, (long long)b.half_ms);
        drift_jrn("sot", "\"e\":\"commit\",\"q\":\"%s\",\"seq\":%lu",
                  drift_quality_name(r->quality), (unsigned long)b.seq);
    } else {
        drift_jrn("sot", "\"e\":\"commit_fail\",\"q\":\"%s\",\"err\":%d",
                  drift_quality_name(r->quality), (int)err);
    }
    return err;
}

esp_err_t drift_sot_commit(bool force, int64_t now_us)
{
    nvs_handle_t h = settings_nvs_metadata();
    if (!h) return ESP_ERR_INVALID_STATE;
    esp_err_t ep = sot_commit_slot(h, DRIFT_SOT_PROVISIONAL, force, now_us);
    esp_err_t eq = sot_commit_slot(h, DRIFT_SOT_QUALIFIED, force, now_us);
    return ep != ESP_OK ? ep : eq;
}

void drift_sot_invalidate(void)
{
    nvs_handle_t h = settings_nvs_metadata();
    if (h) {
        nvs_erase_key(h, SOT_KEY_LEGACY);
        nvs_erase_key(h, SOT_KEY_PROV);
        nvs_erase_key(h, SOT_KEY_QUAL);
        nvs_commit(h);
    }
    memset(s_sot, 0, sizeof(s_sot));
    memset(s_sot_dirty, 0, sizeof(s_sot_dirty));
    memset(s_sot_committed_half, 0, sizeof(s_sot_committed_half));
    drift_jrn("sot", "\"e\":\"invalidate\"");
}

/* ---------------- JSON ---------------- */

/* Bounded append: never runs the write offset past cap. On
 * truncation (or a snprintf error) o pins at cap so later appends
 * are no-ops and callers can detect the condition via o >= cap —
 * an underflowed cap - o would otherwise size the next write
 * to gigabytes.                                               */
#define JAPP(buf, cap, o, ...)                                        \
    do {                                                              \
        if ((o) < (cap)) {                                            \
            int n_ = snprintf((buf) + (o), (cap) - (o), __VA_ARGS__); \
            if (n_ >= 0 && (size_t)n_ < (cap) - (o)) (o) += (size_t)n_; \
            else (o) = (cap);                                         \
        }                                                             \
    } while (0)

static void emit_ll_or_null(char *b, size_t cap, size_t *o,
                            const char *key, bool has, int64_t v)
{
    if (has) JAPP(b, cap, *o, "\"%s\":%lld,", key, (long long)v);
    else     JAPP(b, cap, *o, "\"%s\":null,", key);
}

static void emit_iv_or_null(char *b, size_t cap, size_t *o,
                            const char *key, bool has, int64_t lo, int64_t hi)
{
    if (has) JAPP(b, cap, *o, "\"%s\":[%lld,%lld],",
                  key, (long long)lo, (long long)hi);
    else     JAPP(b, cap, *o, "\"%s\":null,", key);
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

static void emit_assumptions(char *buf, size_t cap, size_t *o,
                             uint64_t mask)
{
    static const struct { uint64_t bit; const char *name; } names[] = {
        {DRIFT_A_ZERO_CACHE, "zero_cache"},
        {DRIFT_A_IMMEDIATE_VISIBLE, "immediate_visibility"},
        {DRIFT_A_FILENAME_ORIGIN, "filename_origin_bounded"},
        {DRIFT_A_FILENAME_PERIODIC, "filename_periodic_bounded"},
        {DRIFT_A_TZ_FIXED, "fixed_timezone"},
        {DRIFT_A_LEGACY_MODEL, "legacy_model"},
    };
    JAPP(buf, cap, *o, "\"assumption_names\":[");
    bool first = true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(mask & names[i].bit)) continue;
        JAPP(buf, cap, *o, "%s\"%s\"",
             first ? "" : ",", names[i].name);
        first = false;
    }
    JAPP(buf, cap, *o, "],");
}

int drift_estimate_json(const drift_record_t *r, char *buf, size_t cap)
{
    static const char *st[] = {"pending", "provisional", "validated",
                               "invalid"};
    static const char *tier[] = {"none", "approximate", "accurate"};
    size_t o = 0;
    JAPP(buf, cap, o,
        "{\"estimate_state\":\"%s\",\"observation_closed\":%s,"
        "\"accuracy_tier\":\"%s\",\"preferred_source\":\"%s\","
        "\"source_reason\":\"%s\",",
        st[r->state], r->observation_closed ? "true" : "false",
        tier[r->tier], drift_source_name(r->source),
        r->source_reason);
    if (r->beacon[0]) {
        JAPP(buf, cap, o, "\"files\":[{\"dir\":\"A:\\\\DATALOG\\\\");
        json_escape(buf, cap, &o, r->day);
        JAPP(buf, cap, o, "\",\"name\":\"");
        json_escape(buf, cap, &o, r->beacon);
        JAPP(buf, cap, o, "\"}],");
    } else {
        JAPP(buf, cap, o, "\"files\":[],");
    }
    if (r->session_start_as10[0]) {
        JAPP(buf, cap, o, "\"session_start_as10\":\"%s\",",
             r->session_start_as10);
    } else {
        JAPP(buf, cap, o, "\"session_start_as10\":null,");
    }
    if (s_tz_rule[0]) {
        JAPP(buf, cap, o, "\"timezone_rule\":\"");
        json_escape(buf, cap, &o, s_tz_rule);
        JAPP(buf, cap, o, "\",\"timezone_confirmed\":%s,",
             s_tz_confirmed ? "true" : "false");
    } else {
        JAPP(buf, cap, o,
             "\"timezone_rule\":null,\"timezone_confirmed\":false,");
    }
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
    emit_iv_or_null(buf, cap, &o, "mtime_interval_ms",
                    r->candidates[0].available,
                    r->candidates[0].lo_ms, r->candidates[0].hi_ms);
    emit_iv_or_null(buf, cap, &o, "filename_creation_interval_ms",
                    r->candidates[1].available,
                    r->candidates[1].lo_ms, r->candidates[1].hi_ms);
    emit_iv_or_null(buf, cap, &o, "filename_periodic_interval_ms",
                    r->candidates[2].available,
                    r->candidates[2].lo_ms, r->candidates[2].hi_ms);
    JAPP(buf, cap, o,
        "\"filename_creation_reason\":\"%s\","
        "\"filename_periodic_reason\":\"%s\",",
        r->candidates[1].reason, r->candidates[2].reason);
    JAPP(buf, cap, o,
        "\"sigma_start_ms\":null,\"sigma_drift_ms\":null,"
        "\"systematic_components\":[\"stamp_quantization\","
        "\"poll_visibility\",\"latency_prior\",\"cache_bound\","
        "\"anchor_uncertainty\"],");
    emit_ll_or_null(buf, cap, &o, "flush_period_ms",
                    r->has_period, r->flush_period_ms);
    emit_iv_or_null(buf, cap, &o, "flush_period_interval_ms",
                    r->has_period, r->period_lo_ms, r->period_hi_ms);
    JAPP(buf, cap, o,
        "\"timer_rate_ppm\":null,\"timer_rate_interval_ppm\":null,"
        "\"rtc_rate_ppm\":null,\"record_duration_s\":%lu,"
        "\"record_bytes\":%lu,\"complete_records\":\"%llu\","
        "\"n_windows\":%d,\"segments\":%d,"
        "\"model_id\":\"%s\",\"quality\":\"%s\",\"assumptions\":%llu,",
        (unsigned long)r->record_duration_s,
        (unsigned long)r->record_bytes,
        (unsigned long long)r->complete_records,
        r->n_windows, r->n_segments, r->model_id,
        drift_quality_name(r->quality),
        (unsigned long long)r->assumptions);
    emit_assumptions(buf, cap, &o, r->assumptions);
    JAPP(buf, cap, o, "\"flags\":[");
    const char *p = r->flags;
    while (*p && o + 8 < cap) {
        const char *c = strchr(p, ',');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        JAPP(buf, cap, o, "\"%.*s\",", (int)len, p);
        if (o >= cap) break;
        p += c ? len + 1 : len;
    }
    if (o >= cap) return -1;          /* truncated: fail explicitly   */
    if (o && buf[o - 1] == ',') o--;
    snprintf(buf + o, cap - o, "]}");
    return (int)strlen(buf);
}

int drift_sot_json(const drift_sot_t *s, char *buf, size_t cap)
{
    size_t o = 0;
    JAPP(buf, cap, o,
        "{\"clock_drift_ms\":%lld,\"interval_ms\":[%lld,%lld],"
        "\"accuracy_half_ms\":%lld,\"computed_at_ms\":%lld,"
        "\"reference_at_ms\":%lld,\"origin\":\"%s\","
        "\"persisted_seq\":%lu,\"quality\":\"%s\",\"source\":\"%s\","
        "\"applicable\":%s,\"freshness_unknown\":%s,\"stale\":%s,"
        "\"current_half_ms\":%lld,\"observations\":%u,"
        "\"assumptions\":%llu,\"model_rev\":%lu,\"model_id\":\"%s\","
        "\"dirty\":%s,\"last_error\":%d,\"source_reason\":\"%s\","
        "\"filename_encoding\":%u,",
        (long long)s->clock_drift_ms, (long long)s->lo_ms,
        (long long)s->hi_ms, (long long)s->accuracy_half_ms,
        (long long)s->computed_at_ms, (long long)s->reference_at_ms,
        s->origin == DRIFT_SOT_NVS ? "nvs" : "session",
        (unsigned long)s->persisted_seq,
        drift_quality_name(s->quality),
        drift_source_name(s->source),
        s->applicable ? "true" : "false",
        s->freshness_unknown ? "true" : "false",
        s->stale ? "true" : "false",
        (long long)s->current_half_ms, (unsigned)s->observations,
        (unsigned long long)s->assumptions,
        (unsigned long)s->model_rev, s->model_id,
        s->dirty ? "true" : "false", (int)s->last_error,
        s->source_reason,
        (unsigned)s->filename_encoding);
    /* card_fw is card-supplied text (e.g. an XML prolog) — it can
     * carry quotes/backslashes, so it must be escaped, not %s'd  */
    JAPP(buf, cap, o, "\"card_fw\":\"");
    json_escape(buf, cap, &o, s->card_fw);
    JAPP(buf, cap, o,
        "\",\"anchor_epoch\":%lu,\"fname_us\":[%ld,%ld],"
        "\"filename_period_us\":%lld,",
        (unsigned long)s->anchor_epoch,
        (long)s->fname_lo_us, (long)s->fname_hi_us,
        (long long)s->filename_period_us);
    emit_assumptions(buf, cap, &o, s->assumptions);
    if (o >= cap) return -1;          /* truncated: fail explicitly   */
    if (o && buf[o - 1] == ',') o--;
    snprintf(buf + o, cap - o, "}");
    return (int)strlen(buf);
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
