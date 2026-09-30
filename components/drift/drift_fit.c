/*
 * SomnoPal — (T_1, P) feasible-region fit and §8.5 record (spec §8.3–8.5)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Constraints per observation i with q complete records at listed size:
 *     T_q   = T1 + (q-1)*P  <= r_i + J        ->  T1 + (q-1)P <= c
 *     T_q+1 = T1 +    q *P  >  s_i - C - J    -> -T1 - q*P    <= c'
 * plus prior bounds on T1 and P. The feasible set is a convex polygon;
 * interval projections come from vertex enumeration.
 */
#include <stdio.h>
#include <string.h>

#include "drift.h"

typedef struct {
    double a, b, c;          /* a*T1 + b*P <= c */
} halfplane_t;

#define MAX_HP (2 * DRIFT_OBS_MAX + 4)

static bool feasible(const halfplane_t *hp, int n, double t1, double p,
                     double tol)
{
    for (int i = 0; i < n; i++) {
        if (hp[i].a * t1 + hp[i].b * p > hp[i].c + tol) return false;
    }
    return true;
}

bool drift_sess_fit(drift_session_t *s, int64_t p_prior_lo_us,
                    int64_t p_prior_hi_us, int64_t t1_prior_lo_us,
                    int64_t t1_prior_hi_us)
{
    s->fit_valid = false;
    if (!s->header_known || !s->seen_nonheader || s->n_obs == 0) {
        return false;
    }
    halfplane_t hp[MAX_HP];
    int n = 0;
    const double J = s->jitter_us;
    const double C = s->cache_bound_us;

    for (int i = 0; i < s->n_obs; i++) {
        const drift_obs_t *o = &s->obs[i];
        uint64_t payload = o->size - s->header_bytes;
        if (payload % s->record_bytes) continue;   /* torn: no q point  */
        uint64_t q = payload / s->record_bytes;
        if (q == 0) continue;
        /* T1 + (q-1)P <= r_i + J                                      */
        hp[n++] = (halfplane_t){1.0, (double)(q - 1),
                                (double)o->resp_us + J};
        /* -(T1 + q*P) <= -(s_i - C - J)                               */
        hp[n++] = (halfplane_t){-1.0, -(double)q,
                                -((double)o->issue_us - C - J)};
    }
    hp[n++] = (halfplane_t){0,  1, (double)p_prior_hi_us};
    hp[n++] = (halfplane_t){0, -1, -(double)p_prior_lo_us};
    hp[n++] = (halfplane_t){1,  0, (double)t1_prior_hi_us};
    hp[n++] = (halfplane_t){-1, 0, -(double)t1_prior_lo_us};

    double t1_lo = 0, t1_hi = 0, p_lo = 0, p_hi = 0;
    bool first = true;
    const double tol = 1.0;   /* 1 µs slack for fp round-off           */

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            /* solve hp[i] and hp[j] as equalities                     */
            double det = hp[i].a * hp[j].b - hp[j].a * hp[i].b;
            if (det == 0.0 || (det < 1e-9 && det > -1e-9)) continue;
            double t1 = (hp[i].c * hp[j].b - hp[j].c * hp[i].b) / det;
            double p  = (hp[i].a * hp[j].c - hp[j].a * hp[i].c) / det;
            if (!feasible(hp, n, t1, p, tol)) continue;
            if (first) {
                t1_lo = t1_hi = t1;
                p_lo = p_hi = p;
                first = false;
            } else {
                if (t1 < t1_lo) t1_lo = t1;
                if (t1 > t1_hi) t1_hi = t1;
                if (p < p_lo) p_lo = p;
                if (p > p_hi) p_hi = p;
            }
        }
    }
    if (first) return false;            /* empty feasible region       */

    s->fit_valid = true;
    s->t1_lo_us = (int64_t)(t1_lo - 0.5);
    s->t1_hi_us = (int64_t)(t1_hi + 0.5);
    s->p_lo_us  = (int64_t)(p_lo - 0.5);
    s->p_hi_us  = (int64_t)(p_hi + 0.5);
    return true;
}

/* ---------------- §8.5 record ---------------- */

static void flag(char *f, size_t cap, const char *name)
{
    size_t l = strlen(f);
    if (l) snprintf(f + l, cap - l, ",%s", name);
    else   snprintf(f, cap, "%s", name);
}

void drift_build_record(const drift_session_t *s, drift_record_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = EST_PENDING;
    out->complete_records = s->complete_records;
    out->record_bytes = s->record_bytes;
    out->record_duration_s = s->record_duration_s;

    if (!s->header_known) {
        flag(out->flags, sizeof(out->flags), "clock_drift_pending");
        flag(out->flags, sizeof(out->flags), "timing_unvalidated");
        flag(out->flags, sizeof(out->flags), "metadata_missing_after_reboot");
        return;
    }
    if (!s->fit_valid) {
        flag(out->flags, sizeof(out->flags), "clock_drift_pending");
        if (s->seen_nonheader) {
            flag(out->flags, sizeof(out->flags), "a2_violation");
        }
        flag(out->flags, sizeof(out->flags), "timer_model_unverified");
        flag(out->flags, sizeof(out->flags), "timing_unvalidated");
        return;
    }

    /* T_start interval = T_1 - P (lambda_w folded into systematic
     * components until a characterized latency prior exists)          */
    int64_t s_lo = s->t1_lo_us - s->p_hi_us;
    int64_t s_hi = s->t1_hi_us - s->p_lo_us;

    int64_t u_lo, u_hi, p_lo_utc, p_hi_utc;
    bool m_s = drift_map_utc(s_lo, &u_lo, &u_hi) &&
               drift_map_utc(s_hi, &p_lo_utc, &p_hi_utc);
    if (!m_s) {
        flag(out->flags, sizeof(out->flags), "no_anchor_2h");
        flag(out->flags, sizeof(out->flags), "clock_drift_pending");
        flag(out->flags, sizeof(out->flags), "timing_unvalidated");
        /* flush period is still publishable (monotonic, §8.4)         */
        out->has_period = true;
        out->period_lo_ms = s->p_lo_us / 1000;
        out->period_hi_ms = s->p_hi_us / 1000;
        out->flush_period_ms = (s->p_lo_us + s->p_hi_us) / 2000;
        out->state = EST_PENDING;
        return;
    }
    int64_t start_lo = (u_lo < p_lo_utc ? u_lo : p_lo_utc);
    int64_t start_hi = (u_hi > p_hi_utc ? u_hi : p_hi_utc);
    out->has_point = true;
    out->start_lo_ms = start_lo / 1000;
    out->start_hi_ms = start_hi / 1000;
    out->session_start_true_ms = (start_lo + start_hi) / 2000;

    /* device start = h + [0,1s); drift = true - device                */
    int64_t h_us = s->hdr_start_local_s * 1000000LL;
    int64_t d_lo = start_lo - (h_us + 1000000LL);
    int64_t d_hi = start_hi - h_us;
    out->has_drift = true;
    out->drift_lo_ms = d_lo / 1000;
    out->drift_hi_ms = d_hi / 1000;
    out->clock_drift_ms = (d_lo + d_hi) / 2000;

    out->has_period = true;
    out->period_lo_ms = s->p_lo_us / 1000;
    out->period_hi_ms = s->p_hi_us / 1000;
    out->flush_period_ms = (s->p_lo_us + s->p_hi_us) / 2000;

    /* timer rate only when record duration is validated — which needs
     * the model profile; mark unverified until characterization says   */
    flag(out->flags, sizeof(out->flags), "timer_model_unverified");
    flag(out->flags, sizeof(out->flags), "timing_unvalidated");
    out->state = EST_PROVISIONAL;
}

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

int drift_record_json(const drift_record_t *r, const char *files_json,
                      const char *session_start_as10,
                      char *buf, size_t cap)
{
    static const char *st[] = {"pending","provisional","validated","invalid"};
    size_t o = 0;
    int n = snprintf(buf, cap,
        "{\"schema\":1,\"record_key\":\"%s\",\"files\":%s,"
        "\"estimate_state\":\"%s\",\"observation_closed\":%s,"
        "\"session_start_as10\":%s%s%s,\"timezone_rule\":null,",
        r->record_key,
        files_json ? files_json : "[]",
        st[r->state], r->observation_closed ? "true" : "false",
        session_start_as10 ? "\"" : "",
        session_start_as10 ? session_start_as10 : "null",
        session_start_as10 ? "\"" : "");
    o = (n > 0) ? (size_t)n : 0;
    emit_ll_or_null(buf, cap, &o, "session_start_true_ms",
                    r->has_point, r->session_start_true_ms);
    emit_iv_or_null(buf, cap, &o, "start_interval_ms",
                    r->has_point, r->start_lo_ms, r->start_hi_ms);
    emit_ll_or_null(buf, cap, &o, "clock_drift_ms",
                    r->has_drift, r->clock_drift_ms);
    emit_iv_or_null(buf, cap, &o, "drift_interval_ms",
                    r->has_drift, r->drift_lo_ms, r->drift_hi_ms);
    n = snprintf(buf + o, cap - o,
        "\"sigma_start_ms\":null,\"sigma_drift_ms\":null,"
        "\"systematic_components\":[\"listing_jitter\",\"cache_bound\","
        "\"anchor_uncertainty\"],");
    o += (n > 0) ? (size_t)n : 0;
    emit_ll_or_null(buf, cap, &o, "flush_period_ms",
                    r->has_period, r->flush_period_ms);
    emit_iv_or_null(buf, cap, &o, "flush_period_interval_ms",
                    r->has_period, r->period_lo_ms, r->period_hi_ms);
    /* flags: comma list in r->flags -> JSON string array             */
    n = snprintf(buf + o, cap - o,
        "\"timer_rate_ppm\":null,\"timer_rate_interval_ppm\":null,"
        "\"rtc_rate_ppm\":null,\"record_duration_s\":%lu,"
        "\"record_bytes\":%lu,\"complete_records\":\"%llu\","
        "\"model_id\":\"uncharacterized\",\"flags\":[",
        (unsigned long)r->record_duration_s, (unsigned long)r->record_bytes,
        (unsigned long long)r->complete_records);
    o += (n > 0) ? (size_t)n : 0;
    const char *p = r->flags;
    while (*p && o < cap - 8) {
        const char *c = strchr(p, ',');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        n = snprintf(buf + o, cap - o, "\"%.*s\",", (int)len, p);
        o += (n > 0) ? (size_t)n : 0;
        p += c ? len + 1 : len;
    }
    if (o && buf[o - 1] == ',') o--;      /* trim trailing comma       */
    snprintf(buf + o, cap - o, "],\"evidence_available\":false}");
    return (int)strlen(buf);
}
